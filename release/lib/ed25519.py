#
# Zeitlos release tooling -- Ed25519 signatures (RFC 8032), in plain
# Python, for signing update releases (docs/distribution.md).
#
# Why not a library: the release machine should need nothing installed
# beyond Python to sign a release, and signing happens once per publish
# over one manifest, so speed does not matter (a signature takes well
# under a second). The board checks with Monocypher's
# crypto_ed25519_check() (sw/ext/monocypher/monocypher-ed25519.c), the
# same algorithm: RFC 8032's Ed25519, SHA-512, no context, no prehash.
# release/lib/test_store.py checks this against RFC 8032's test vectors,
# and sw/apps/update/tests checks the board's side against signatures
# made here.
#
# Follows RFC 8032 section 5.1 and its reference code (section 6), with
# extended coordinates.
#
#   seed = os.urandom(32)              the private key (keep it secret)
#   pub  = public_key(seed)            32 bytes
#   sig  = sign(seed, msg)             64 bytes
#   verify(pub, msg, sig) -> bool
#

import hashlib

p = 2 ** 255 - 19
q = 2 ** 252 + 27742317777372353535851937790883648493   # the group order
d = (-121665 * pow(121666, p - 2, p)) % p


def _sha512(b):
    return hashlib.sha512(b).digest()


def _inv(x):
    return pow(x, p - 2, p)


# Points are (X, Y, Z, T) with x = X/Z, y = Y/Z, x*y = T/Z.

def _add(P, Q):
    A = (P[1] - P[0]) * (Q[1] - Q[0]) % p
    B = (P[1] + P[0]) * (Q[1] + Q[0]) % p
    C = 2 * P[3] * Q[3] * d % p
    D = 2 * P[2] * Q[2] % p
    E, F, G, H = B - A, D - C, D + C, B + A
    return (E * F % p, G * H % p, F * G % p, E * H % p)


def _mul(s, P):
    Q = (0, 1, 1, 0)        # the neutral element
    while s > 0:
        if s & 1:
            Q = _add(Q, P)
        P = _add(P, P)
        s >>= 1
    return Q


def _equal(P, Q):
    if (P[0] * Q[2] - Q[0] * P[2]) % p != 0:
        return False
    return (P[1] * Q[2] - Q[1] * P[2]) % p == 0


_modp_sqrt_m1 = pow(2, (p - 1) // 4, p)


def _recover_x(y, sign):
    if y >= p:
        return None
    x2 = (y * y - 1) * _inv(d * y * y + 1)
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (p + 3) // 8, p)
    if (x * x - x2) % p != 0:
        x = x * _modp_sqrt_m1 % p
    if (x * x - x2) % p != 0:
        return None
    if (x & 1) != sign:
        x = p - x
    return x


_gy = 4 * _inv(5) % p
_gx = _recover_x(_gy, 0)
G = (_gx, _gy, 1, _gx * _gy % p)


def _compress(P):
    zinv = _inv(P[2])
    x = P[0] * zinv % p
    y = P[1] * zinv % p
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _decompress(s):
    if len(s) != 32:
        return None
    y = int.from_bytes(s, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % p)


def _secret_expand(seed):
    if len(seed) != 32:
        raise ValueError("an Ed25519 private key is 32 bytes")
    h = _sha512(seed)
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= (1 << 254)
    return a, h[32:]


def public_key(seed):
    a, _ = _secret_expand(seed)
    return _compress(_mul(a, G))


def sign(seed, msg):
    a, prefix = _secret_expand(seed)
    A = _compress(_mul(a, G))
    r = int.from_bytes(_sha512(prefix + msg), "little") % q
    R = _compress(_mul(r, G))
    h = int.from_bytes(_sha512(R + A + msg), "little") % q
    s = (r + h * a) % q
    return R + int.to_bytes(s, 32, "little")


def verify(pub, msg, sig):
    if len(pub) != 32 or len(sig) != 64:
        return False
    A = _decompress(pub)
    if not A:
        return False
    R = _decompress(sig[:32])
    if not R:
        return False
    s = int.from_bytes(sig[32:], "little")
    if s >= q:
        return False
    h = int.from_bytes(_sha512(sig[:32] + pub + msg), "little") % q
    return _equal(_mul(s, G), _add(R, _mul(h, A)))
