#
# Zeitlos ask -- the dense half.
#
# -- the asymmetry that makes this possible at all --
#
# Document vectors are computed ON THE HOST, once, for the whole
# corpus. Only the QUERY encoder ever runs on the Zeitlos machine. So
# the document side can be arbitrarily expensive and the query side has
# a hard budget of about a megabyte of weights and a few tens of
# millions of MACs.
#
# That budget is the only reason any of this fits. A query is sixteen
# tokens; a passage is four hundred. Understanding "how do I purify
# water" is a far smaller function than understanding two kilobytes of
# a field manual, and we only have to ship the small one.
#
# -- backends --
#
#   lsa    Truncated SVD over the corpus's own tf-idf. Needs no
#          training and no GPU. NOT DEPLOYABLE -- see below. Exists so
#          that ranking quality can be measured before anyone commits
#          to a training run.
#
#   bow    Term vectors taken from the `lsa` projection, restricted to
#          a fixed vocabulary, with idf-weighted mean pooling.
#          Deployable, needs no training, and is the floor everything
#          else has to beat. This is what a first distribution ships.
#
#   torch  A trained bi-encoder. Deployable. This is the one that is
#          supposed to win, and the eval harness is how you find out
#          whether it did.
#
# WHY `lsa` CANNOT SHIP, since it is otherwise the obvious choice: its
# query encoder is a tf-idf transform against the full corpus
# vocabulary followed by a projection. That is a 39,000-term dictionary
# and a 39,000 x 128 matrix -- about 20MB in float, 5MB quantised --
# to encode one short question. `bow` is the same idea with the
# vocabulary truncated to the terms a question is likely to contain,
# which turns 5MB into 512KB and costs surprisingly little ranking
# quality. Measure it rather than believing that sentence.
#
# -- Matryoshka --
#
# Vectors are L2-normalised and the COARSE index is the first
# `coarse_dim` components of the same vector, renormalised. For `lsa`
# and `bow` that is free and principled: SVD components are already
# ordered by explained variance, so a truncation is the best rank-k
# approximation. For `torch` it has to be trained for -- see
# MATRYOSHKA_DIMS below -- and if it is not, truncation degrades badly
# rather than gracefully.
#

import numpy as np


def _fit_svd(X, dim, what):
    """TruncatedSVD, with the component count clamped to what the data
    can actually support, and the clamp reported.

    n_components cannot exceed min(n_samples, n_features) - 1. A tiny
    corpus -- one document, or a dataset somebody is testing an adapter
    against -- silently produced FEWER components than asked for, and
    the failure surfaced hundreds of lines later as a broadcast error
    between a (128,) accumulator and a (29,) term vector. Clamp here,
    say so, and let every downstream dimension be read back from the
    fitted object rather than assumed.
    """
    from sklearn.decomposition import TruncatedSVD
    import numpy as _np
    cap = max(1, min(X.shape) - 1)
    use = min(dim, cap)
    if use < dim:
        print("  note: corpus supports only %d dimensions, not %d "
              "(%s has %d chunks, %d terms)"
              % (use, dim, what, X.shape[0], X.shape[1]))
    svd = TruncatedSVD(n_components=use, random_state=0)
    svd.fit(X)
    return svd, int(svd.components_.shape[0])


# Below this many chunks, a tf-idf fit with min_df=2 has nothing to
# work with and sklearn fails with "max_df corresponds to < documents
# than min_df", which does not say what is wrong. A corpus this small
# is a recipe pointed at the wrong place or an adapter that loaded
# almost nothing, and the message should say so.
MIN_CHUNKS = 8


def _check_corpus(texts):
    if len(texts) < MIN_CHUNKS:
        raise SystemExit(
            "only %d chunk%s to index -- too few to build an encoder.\n"
            "  Check the recipe's `source` lines actually point at "
            "content;\n  `ask ingest <recipe>` reports what each one "
            "loaded."
            % (len(texts), "" if len(texts) == 1 else "s"))

BACKENDS = {}

# Dimensions the torch backend trains to be independently usable.
# Must include both the coarse and fine dimension the packer uses.
MATRYOSHKA_DIMS = (32, 64, 128, 256)


def backend(name):
    def wrap(cls):
        BACKENDS[name] = cls
        return cls
    return wrap


def _l2(x):
    n = np.linalg.norm(x, axis=-1, keepdims=True)
    n[n == 0] = 1.0
    return x / n


class Encoder:
    """Interface every backend implements.

    encode_docs  is host-only and may be as slow as it likes.
    encode_query is the function that has to have a counterpart in C
                 on the device, so whatever it does must be expressible
                 in integer arithmetic over the exported weights.
    """

    name = "?"
    dim = 0
    deployable = False

    def encode_docs(self, texts):
        raise NotImplementedError

    def encode_query(self, text):
        raise NotImplementedError

    def export(self):
        """Return the weight blob the device loads, or None."""
        return None


# -- lsa ---------------------------------------------------------------

@backend("lsa")
class LSAEncoder(Encoder):

    name = "lsa"
    deployable = False

    def __init__(self, dim=128, **kw):
        self.dim = dim
        self.vec = None
        self.svd = None

    def fit(self, texts):
        from sklearn.feature_extraction.text import TfidfVectorizer
        _check_corpus(texts)
        self.vec = TfidfVectorizer(lowercase=True, stop_words="english",
                                   sublinear_tf=True, min_df=2,
                                   max_features=120000)
        X = self.vec.fit_transform(texts)
        self.svd, self.dim = _fit_svd(X, self.dim, "lsa")
        return self

    def encode_docs(self, texts):
        X = self.vec.transform(texts)
        return _l2(self.svd.transform(X).astype(np.float32))

    def encode_query(self, text):
        return self.encode_docs([text])[0]


# -- bow ---------------------------------------------------------------

@backend("bow")
class BowEncoder(Encoder):
    """Deployable, untrained. Term vectors + idf-weighted mean pooling.

    The device side is: tokenise, look each term up in a sorted table,
    accumulate `idf * vector` into an int32 accumulator, normalise.
    That is a few thousand MACs -- negligible next to the index scan --
    which is exactly why this is worth having even once a transformer
    exists: it is the fallback when the encoder file is missing, and
    the sanity check when the transformer returns nonsense.
    """

    name = "bow"
    deployable = True

    def __init__(self, dim=128, vocab=4096, **kw):
        self.dim = dim
        self.vocab_size = int(vocab)
        self.terms = []
        self.vectors = None     # (vocab, dim) float32, L2-normalised rows
        self.idf = None         # (vocab,) float32

    def fit(self, texts, lexicon=None):
        """Derive term vectors from an LSA projection of the corpus.

        The vocabulary is the `vocab_size` terms with the highest
        collection frequency among those the lexicon kept -- i.e. after
        stopwords and after the DF_MAX_RATIO cut. Keeping the MOST
        common of the informative terms is deliberate: a query is short
        and ordinary, and a vocabulary of hapax legomena would miss
        every word of it. The rare terms are not lost, they are handled
        by the lexical half, which is better at them anyway.
        """
        from sklearn.feature_extraction.text import TfidfVectorizer
        _check_corpus(texts)

        vec = TfidfVectorizer(lowercase=True, stop_words="english",
                              sublinear_tf=True, min_df=2,
                              max_features=120000)
        X = vec.fit_transform(texts)
        svd, self.dim = _fit_svd(X, self.dim, "bow")

        names = np.array(vec.get_feature_names_out())
        # Collection frequency, as a proxy for "a question might use
        # this word".
        cf = np.asarray(X.sum(axis=0)).ravel()
        keep = np.argsort(-cf)[:self.vocab_size]
        keep = keep[np.argsort(names[keep])]      # sorted, for bsearch

        self.terms = [str(t) for t in names[keep]]
        # components_ is (dim, n_features); a term's vector is its
        # column, which is its coordinates in the reduced space.
        self.vectors = _l2(svd.components_[:, keep].T.astype(np.float32))
        self.idf = vec.idf_[keep].astype(np.float32)
        self._index = {t: i for i, t in enumerate(self.terms)}
        return self

    def _pool(self, text):
        from .lexicon import tokenize
        acc = np.zeros(self.dim, dtype=np.float32)
        hits = 0
        for t in tokenize(text):
            i = self._index.get(t)
            if i is None:
                continue
            acc += self.vectors[i] * self.idf[i]
            hits += 1
        if hits == 0:
            return acc
        n = np.linalg.norm(acc)
        return acc / n if n else acc

    def encode_docs(self, texts):
        return np.stack([self._pool(t) for t in texts])

    def encode_query(self, text):
        return self._pool(text)

    def export(self):
        return {
            "kind": "bow",
            "dim": self.dim,
            "terms": self.terms,
            "vectors": self.vectors,
            "idf": self.idf,
        }


# -- torch -------------------------------------------------------------

@backend("trained")
class TrainedEncoder(BowEncoder):
    """The `bow` encoder with W and the term weights actually learned.

    Identical arithmetic, identical export, identical device code --
    only the numbers differ. `bow` initialises them from an SVD and
    stops; this one initialises the same way and then trains
    contrastively on pairs from lib/synth.py.

    Starting from the SVD rather than from noise matters: it is
    already a reasonable term space, so training refines rather than
    discovers, which is the difference between minutes and hours on a
    corpus this size.
    """

    name = "trained"
    deployable = True

    def __init__(self, dim=128, vocab=4096, **kw):
        BowEncoder.__init__(self, dim=dim, vocab=vocab, **kw)
        self.opts = kw
        self._trained = None

    def fit_trained(self, docs, chunks, texts, lx, opts, log=print):
        from . import synth, train as tr
        from .lexicon import tokenize

        # SVD initialisation, exactly as `bow` does.
        BowEncoder.fit(self, texts)

        pairs, counts = synth.build(
            docs, chunks, texts, lx,
            sources=opts.get("pairs", "ict,title"),
            seed=int(opts.get("seed", 0)),
            ict_per_chunk=int(opts.get("ict_per_chunk", 1)),
            import_path=opts.get("pairs_file"))
        log("  training pairs: %s  (total %d)"
            % (", ".join("%s %d" % (k, v) for k, v in sorted(counts.items())),
               len(pairs)))
        if len(pairs) < 64:
            raise SystemExit(
                "only %d training pairs -- too few to train on.\n"
                "  Most passages produced none, which usually means the "
                "corpus is tiny\n  or the chunks have no sentence "
                "structure." % len(pairs))

        enc = tr.BagEncoder(len(self.terms), self.dim,
                            seed=int(opts.get("seed", 0)))
        # Start from the UNTRAINED ENCODER, not from noise and not
        # from uniform weights: the SVD term directions AND their idf.
        # With both, epoch 0 reproduces `encoder = bow` exactly, so any
        # change in the measurement is training and nothing else.
        enc.W = self.vectors.astype("float32").copy()
        enc.s = tr.inv_softplus(self.idf)

        index = self._index
        qbags, dbags, groups = [], [], []
        seen_q = set()
        dropped_dup = 0
        for q, ci, drop in pairs:
            text = texts[ci]
            if drop:
                # Inverse Cloze: the sampled sentence is REMOVED from
                # the positive, or the model just learns to match a
                # string to itself.
                text = text.replace(drop, " ", 1)
            # Exact duplicates carry no extra signal and make the
            # false-negative problem worse: the same question attached
            # to two chunks guarantees a contradictory pair the moment
            # both land in a batch.
            key = (q.lower(), ci)
            if key in seen_q:
                dropped_dup += 1
                continue
            seen_q.add(key)

            qb = tr.bag_of(q, index, tokenize)
            db = tr.bag_of(text, index, tokenize)
            if len(qb[0]) and len(db[0]):
                qbags.append(qb)
                dbags.append(db)
                # Batches are built one-per-document, so a query is
                # never trained against another chunk of the document
                # that answers it. See lib/train.py's _batches().
                groups.append(chunks[ci].doc)
        log("  usable pairs:   %d%s"
            % (len(qbags),
               ("  (%d duplicates dropped)" % dropped_dup)
               if dropped_dup else ""))

        tr.train(enc, qbags, dbags,
                 # FOUR, not thirty.
                 #
                 # Measured, and the direction is consistent: on
                 # arklite with ict+title, 4 epochs beat the untrained
                 # baseline and 12 was already worse; at 30 epochs with
                 # generated questions added, dense recall at rank 1
                 # fell from 30/53 to 25/53 -- BELOW the encoder that
                 # was never trained at all.
                 #
                 # A run is under a minute, so `ask train --sweep
                 # 1,2,4,8,16` measures it for a given corpus rather
                 # than trusting this number.
                 epochs=int(opts.get("epochs", 4)),
                 batch=int(opts.get("batch", 128)),
                 lr=float(opts.get("lr", 0.05)),
                 tau=float(opts.get("tau", 0.05)),
                 seed=int(opts.get("seed", 0)), groups=groups, log=log)

        exp = tr.export(enc, self.terms)
        self.vectors = exp["vectors"]
        self.idf = exp["idf"]
        self._trained = exp
        return self

    def export(self):
        return self._trained if self._trained else BowEncoder.export(self)

    def load_cache(self, path):
        import numpy as np
        z = np.load(path, allow_pickle=True)
        self.dim = int(z["dim"])
        self.vectors = z["vectors"]
        self.idf = z["idf"]
        self.terms = [str(t) for t in z["terms"]]
        self._index = {t: i for i, t in enumerate(self.terms)}
        self.vocab_size = len(self.terms)
        self._trained = {"kind": "bow", "dim": self.dim,
                         "terms": self.terms, "vectors": self.vectors,
                         "idf": self.idf}
        return self


@backend("torch")
class TorchEncoder(Encoder):
    """A trained bi-encoder.

    *** NOT EXERCISED IN THE ENVIRONMENT THIS FILE WAS WRITTEN IN. ***
    There was no GPU and no torch. The `lsa` and `bow` backends above
    were run against real Ark data and their numbers are in
    docs/ask_app.md; this one has not been, and the first thing anyone
    should do with it is run `ask eval` and compare.
    """

    name = "torch"
    deployable = True

    def __init__(self, dim=128, vocab=4096, layers=4, heads=4, ff=384,
                 max_len=64, **kw):
        self.dim = int(dim)
        self.vocab_size = int(vocab)
        self.layers = int(layers)
        self.heads = int(heads)
        self.ff = int(ff)
        self.max_len = int(max_len)
        self.model = None
        self.tok = None

    def _require(self):
        try:
            import torch  # noqa: F401
        except ImportError:
            raise RuntimeError(
                "the `torch` backend needs PyTorch installed. Use "
                "`encoder = bow` for a build that needs no training, or "
                "install torch and re-run.")

    def fit(self, texts, pairs=None, teacher=None, **kw):
        """Contrastive training over (query, positive) pairs.

        `pairs` comes from lib/synth.py -- questions generated on the
        host by a larger model, with hard negatives mined from the
        corpus. `teacher`, if given, is a matrix of document vectors
        from a full-size encoder, in which case the loss becomes
        projection into the teacher's space rather than contrastive
        from scratch. The second is much the better option when a
        teacher is available: the small model then only has to learn
        what a QUESTION means, and inherits a good space for passages
        instead of having to discover one from synthetic data.
        """
        self._require()
        raise NotImplementedError(
            "training is the next phase -- see docs/ask_app.md, "
            "\"Phase 3\". The framework, formats, packing and eval are "
            "in place and `bow` produces a shippable distribution today.")
