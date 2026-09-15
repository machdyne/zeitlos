#
# Zeitlos ask -- training the encoder.
#
# ============ WHAT IS TRAINED, AND WHY IT IS THIS ============
#
# The device computes exactly one thing (sw/apps/ask/aidx.c,
# encode_query):
#
#     v = normalise( sum over query terms t of  a[t] * W[t] )
#
# `W` is a vocab x dim table of int8 vectors and `a` is a per-term
# weight in Q8.8. Today both come from an SVD of the corpus and are
# not trained at all.
#
# So the thing to train is W and a. That is a linear bag-of-embeddings
# dual encoder, and training it:
#
#   * needs NO new C on the device -- the arithmetic is unchanged,
#     only the numbers in encoder.zmd differ;
#   * needs NO format change -- `ask build` writes the same file;
#   * is directly comparable, because `ask eval` measures the same
#     pipeline before and after.
#
# Introducing a transformer would mean writing and verifying a
# transformer in C on a 48MHz core with no FPU, for a query of about
# sixteen tokens. Training the model that is already deployed is the
# cheaper half of that by a wide margin, and it has to be done first
# anyway to know whether the extra capacity is what was missing.
#
# ============ NO GPU, NO TORCH ============
#
# 4096 x 128 is 524,288 parameters. The forward pass for a batch is one
# gather and one matmul. This trains to convergence in minutes of numpy
# on a laptop, so torch is not a dependency and a GPU is not required.
#
# Where a GPU IS worth having: generating better training QUESTIONS.
# Run a 7B locally over the corpus, write JSONL, and point
# `pairs_file` at it (lib/synth.py's `import`). The questions are the
# scarce resource here, not the gradient steps.
#
# ============ THE LOSS ============
#
# InfoNCE with in-batch negatives. For a batch of B (query, passage)
# pairs, each query's positive is its own passage and its negatives
# are the other B-1 passages. Cheap, and the gradient signal scales
# with B, which is why the batch is large rather than the epochs many.
#
# Both sides use the SAME encoder. The document vectors `ask build`
# writes come from it too, so query and passage necessarily share a
# space -- unlike an asymmetric setup, which is stronger and needs a
# teacher model to project into.
#

import numpy as np


def _l2(x, eps=1e-8):
    n = np.linalg.norm(x, axis=-1, keepdims=True)
    return x / np.maximum(n, eps)


def _softplus(x):
    # a[t] must stay positive: it is stored unsigned Q8.8, and a
    # negative term weight would mean "this word makes the passage
    # less relevant", which the device cannot represent.
    return np.log1p(np.exp(-np.abs(x))) + np.maximum(x, 0)


def _softplus_grad(x):
    return 1.0 / (1.0 + np.exp(-x))


def inv_softplus(a):
    """s such that softplus(s) == a, for a > 0.

    Needed so training can START from the untrained encoder rather
    than from uniform term weights. `s = 0` gives softplus(0) = 0.693
    for EVERY term -- which silently threw away the idf, so epoch 0 was
    already much worse than the SVD baseline it was supposed to refine.
    That looked like overfitting (recall fell and kept falling) and was
    not: the run never started from where it claimed to.

    log(expm1(a)) overflows for large a; above ~20 the two are equal to
    float precision anyway.
    """
    a = np.asarray(a, dtype=np.float64)
    out = np.where(a > 20.0, a, np.log(np.expm1(np.clip(a, 1e-6, 20.0))))
    return out.astype(np.float32)


class BagEncoder:
    """The device's encoder, as trainable parameters.

    W  (vocab, dim)  term directions; exported L2-normalised and int8
    s  (vocab,)      pre-softplus term weights; exported as Q8.8 idf
    """

    def __init__(self, vocab, dim, seed=0):
        rng = np.random.default_rng(seed)
        self.W = rng.normal(0, 1.0 / np.sqrt(dim),
                            (vocab, dim)).astype(np.float32)
        self.s = np.zeros(vocab, dtype=np.float32)

    def weights(self):
        return _softplus(self.s)

    def encode_rows(self, bags):
        """bags: list of (idx array, count array). Returns (B, dim)."""
        a = self.weights()
        out = np.zeros((len(bags), self.W.shape[1]), dtype=np.float32)
        for i, (idx, cnt) in enumerate(bags):
            if len(idx):
                out[i] = (self.W[idx] * (a[idx] * cnt)[:, None]).sum(0)
        return out


def bag_of(text, index, tokenize, cap=256):
    """Token ids and counts for one string, in the encoder's vocab."""
    ids = {}
    for t in tokenize(text)[:cap]:
        k = index.get(t)
        if k is not None:
            ids[k] = ids.get(k, 0) + 1
    if not ids:
        return (np.zeros(0, np.int32), np.zeros(0, np.float32))
    return (np.fromiter(ids.keys(), np.int32, len(ids)),
            np.fromiter(ids.values(), np.float32, len(ids)))


def _batches(order, batch, groups, rng):
    """Yields batches with at most one pair per group where possible.

    `groups[i]` is the document a pair's passage came from.

    InfoNCE treats every other passage in the batch as a negative for
    each query. Two chunks of the Magna Carta in one batch means "What
    is the Magna Carta?" is trained to push the OTHER Magna Carta chunk
    away -- a false negative the loss cannot distinguish from a real
    one.

    Generated questions make this constant rather than occasional: one
    document yields many chunks and their questions overlap heavily by
    construction. Inverse Cloze pairs mostly escape it, because a
    sentence lifted from a passage is specific to that passage.

    ROUND ROBIN OVER DOCUMENT BUCKETS. The obvious implementation --
    scan the pending list, defer collisions, rebuild -- is quadratic,
    and at 18,809 pairs that is minutes per epoch rather than seconds.
    Bucketing once and cycling is linear and gives the same guarantee.
    """
    buckets = {}
    for i in order:
        buckets.setdefault(groups[i], []).append(int(i))
    keys = list(buckets.keys())
    rng.shuffle(keys)

    sel = []
    while keys:
        nxt = []
        for g in keys:
            b = buckets[g]
            sel.append(b.pop())
            if b:
                nxt.append(g)
            if len(sel) == batch:
                yield sel
                sel = []
        keys = nxt
    # The tail is dropped: a partial batch has fewer negatives, so its
    # gradient is on a different scale from every other batch.


def train(enc, qbags, dbags, epochs=30, batch=128, lr=0.05, tau=0.05,
          seed=0, groups=None, log=print):
    """InfoNCE with in-batch negatives, Adam, on (qbags[i], dbags[i]).

    `groups` (one id per pair, usually the document) keeps two pairs
    from the same source out of a batch -- see _batches().

    Returns the loss history.
    """
    rng = np.random.default_rng(seed)
    n = len(qbags)
    vocab, dim = enc.W.shape
    if groups is None:
        groups = list(range(n))

    ngroups = len(set(groups))
    if ngroups < batch:
        # Say so rather than quietly accepting false negatives. The
        # fix is a smaller batch, which costs gradient quality, or a
        # bigger corpus, which is usually the real answer.
        batch = max(8, min(batch, ngroups))
        if log:
            log("    note: only %d distinct documents; batch reduced to %d"
                % (ngroups, batch))

    mW = np.zeros_like(enc.W); vW = np.zeros_like(enc.W)
    ms = np.zeros_like(enc.s); vs = np.zeros_like(enc.s)
    b1, b2, eps = 0.9, 0.999, 1e-8
    step = 0
    hist = []

    for ep in range(epochs):
        order = rng.permutation(n)
        total, nb = 0.0, 0

        for sel in _batches(order, batch, groups, rng):
            qb = [qbags[i] for i in sel]
            db = [dbags[i] for i in sel]

            Q = enc.encode_rows(qb)
            D = enc.encode_rows(db)
            qn = np.maximum(np.linalg.norm(Q, axis=1, keepdims=True), 1e-8)
            dn = np.maximum(np.linalg.norm(D, axis=1, keepdims=True), 1e-8)
            Qh, Dh = Q / qn, D / dn

            logits = (Qh @ Dh.T) / tau
            logits -= logits.max(axis=1, keepdims=True)
            P = np.exp(logits)
            P /= P.sum(axis=1, keepdims=True)
            B = len(sel)
            loss = -np.log(np.maximum(P[np.arange(B), np.arange(B)], 1e-12))
            total += float(loss.mean()); nb += 1

            # dL/dlogits
            G = P.copy()
            G[np.arange(B), np.arange(B)] -= 1.0
            G /= B * tau

            dQh = G @ Dh
            dDh = G.T @ Qh
            # through the normalisation
            dQ = (dQh - (dQh * Qh).sum(1, keepdims=True) * Qh) / qn
            dD = (dDh - (dDh * Dh).sum(1, keepdims=True) * Dh) / dn

            gW = np.zeros_like(enc.W)
            ga = np.zeros(vocab, dtype=np.float32)
            a = enc.weights()
            for side, bags, dX in ((0, qb, dQ), (1, db, dD)):
                for i, (idx, cnt) in enumerate(bags):
                    if not len(idx):
                        continue
                    w = (a[idx] * cnt)[:, None]
                    np.add.at(gW, idx, w * dX[i][None, :])
                    np.add.at(ga, idx, cnt * (enc.W[idx] @ dX[i]))

            gs = ga * _softplus_grad(enc.s)

            step += 1
            for p, g, m, v in ((enc.W, gW, mW, vW), (enc.s, gs, ms, vs)):
                m *= b1; m += (1 - b1) * g
                v *= b2; v += (1 - b2) * (g * g)
                mh = m / (1 - b1 ** step)
                vh = v / (1 - b2 ** step)
                p -= lr * mh / (np.sqrt(vh) + eps)

        if nb == 0:
            raise SystemExit(
                "no training batches were produced -- %d pairs, batch %d.\n"
                "  Lower `train_batch` in the recipe, or train on a larger "
                "corpus." % (n, batch))
        avg = total / nb
        hist.append(avg)
        if log and (ep % 5 == 0 or ep == epochs - 1):
            log("    epoch %3d/%d  loss %.4f" % (ep + 1, epochs, avg))

    return hist


def export(enc, terms):
    """Into exactly the shape lib/pack.py's pack_encoder() wants.

    The magnitude lives in `a`, not in W: the device stores W as int8
    with a single global scale, so a row's length would be lost. Rows
    are L2-normalised here and their length folded into the weight,
    which is the same vector and survives quantisation.
    """
    a = _softplus(enc.s)
    norms = np.maximum(np.linalg.norm(enc.W, axis=1), 1e-8)
    W = enc.W / norms[:, None]
    a = a * norms
    # idf is u16 Q8.8 on the card, so the usable range is [0, 255.996].
    # Scale only if something would clip: the device normalises the
    # accumulator anyway, so a uniform factor changes nothing, but
    # rescaling unconditionally makes a trained export incomparable
    # with an untrained one for no reason.
    if float(a.max()) > 255.0:
        a = a * (200.0 / float(a.max()))
    a = np.clip(a, 0.0, 255.0)
    return {"kind": "bow", "dim": W.shape[1], "terms": list(terms),
            "vectors": W.astype(np.float32), "idf": a.astype(np.float32)}
