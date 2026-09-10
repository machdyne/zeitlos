#
# Zeitlos hwmap -- presence conditions.
#
# Every construct hwmap reads out of the RTL is tagged with the condition
# under which it exists, taken straight from the `ifdef stack around it.
# Nothing is ever evaluated: hwmap does not know or care which defines a
# particular board sets. That is the whole point -- the map shows every
# optional block, labelled with what makes it optional.
#
# A condition from a single `ifdef/`ifndef/`elsif/`else chain is always
# a CUBE: an AND of literals, each literal being a define name that must
# be defined (True) or must not be (False). `elsif B after `ifdef A is
# !A & B; `else after both is !A & !B. Nesting just adds literals. So
# a cube is all the representation a construct ever needs.
#
# Where several constructs are merged -- a wire declared in both halves
# of an `ifdef/`else, a net driven from three alternative branches -- the
# result is a COVER: a list of cubes, ORed. Covers are only ever compared
# (does this cover that?), never displayed raw, and they are small: a
# handful of cubes over a handful of names. That makes plain enumeration
# over the names involved exact and fast, with no solver.
#

TRUE = frozenset()


def cube(*lits):
    """cube(("A", True), ("B", False)) -> frozenset of literals."""
    return frozenset(lits)


def names(c):
    return {n for n, _ in c}


def is_sat(c):
    """A cube is unsatisfiable only if it holds both X and !X."""
    seen = {}
    for n, v in c:
        if n in seen and seen[n] != v:
            return False
        seen[n] = v
    return True


def conj(*cubes):
    out = set()
    for c in cubes:
        out |= c
    return frozenset(out)


def implies(a, b):
    """Cube a implies cube b (both satisfiable)."""
    return b <= a


def fmt(c, empty="always"):
    """Human form: 'MEM_SDRAM & !MEM_SRAM'. Positive literals first."""
    if not c:
        return empty
    pos = sorted(n for n, v in c if v)
    neg = sorted(n for n, v in c if not v)
    return " & ".join(pos + ["!" + n for n in neg])


def tag(c, parent=TRUE):
    """Short label for a block tag: only the literals the parent does
    not already state, positive before negative."""
    return fmt(frozenset(c) - frozenset(parent), empty="")


def to_json(c):
    return fmt(c)


# ---------------------------------------------------------------------
# Covers
# ---------------------------------------------------------------------

def absorb(cubes):
    """Drop unsatisfiable cubes and cubes implied by a smaller one, and
    merge pairs that differ only in one complementary literal (X&A,
    X&!A -> X). Repeats to a fixpoint."""
    cs = {c for c in cubes if is_sat(c)}
    changed = True
    while changed:
        changed = False
        # absorption
        for a in list(cs):
            for b in list(cs):
                if a is not b and a != b and a <= b and b in cs:
                    cs.discard(b)
                    changed = True
        # reduction: X | (Y & !X)  ->  X | Y
        for a in list(cs):
            for b in list(cs):
                if a == b or a not in cs or b not in cs:
                    continue
                for (n, v) in a:
                    if (n, not v) in b and (a - {(n, v)}) <= (b - {(n, not v)}):
                        nb = b - {(n, not v)}
                        cs.discard(b)
                        cs.add(frozenset(nb))
                        changed = True
                        break
                if changed:
                    break
            if changed:
                break
        if changed:
            continue
        # resolution
        lst = sorted(cs, key=lambda c: (len(c), fmt(c)))
        for i, a in enumerate(lst):
            for b in lst[i + 1:]:
                if len(a) != len(b):
                    continue
                d = a ^ b
                if len(d) == 2:
                    (n1, v1), (n2, v2) = sorted(d)
                    if n1 == n2 and v1 != v2:
                        m = a & b
                        if m not in cs:
                            cs.discard(a)
                            cs.discard(b)
                            cs.add(m)
                            changed = True
                            break
            if changed:
                break
    return sorted(cs, key=lambda c: (len(c), fmt(c)))


def sharp(u, d):
    """u AND NOT d, as a list of disjoint cubes (the 'sharp' operation)."""
    if not is_sat(conj(u, d)):
        return [u]
    out = []
    prefix = set(u)
    for (n, v) in sorted(d):
        if (n, v) in prefix:
            continue
        c = frozenset(prefix | {(n, not v)})
        if is_sat(c):
            out.append(c)
        prefix.add((n, v))
    return out


def uncovered(u, cover):
    """The parts of cube u NOT covered by the cover (list of cubes).
    Returns a minimised list of cubes; empty means fully covered."""
    rest = [u]
    for d in cover:
        nxt = []
        for r in rest:
            nxt.extend(sharp(r, d))
        rest = nxt
        if not rest:
            break
    return absorb(rest)


def overlap(a, b):
    """Satisfiable conjunction of two cubes, or None."""
    c = conj(a, b)
    return c if is_sat(c) else None


def exclusive(a, b):
    return overlap(a, b) is None
