# ML-KEM (FIPS 203): the pq-crystals reference implementation

Vendored from https://github.com/pq-crystals/kyber, `ref/`, commit
`3edd5af5991927164edd4aacebfcbee00b8064e7` (2 Aug 2026), unmodified.
Licence: **CC0 (public domain)** or Apache 2.0 (LICENSE); its Keccak
code is public domain too.

This branch (`main`) is **ML-KEM as standardized in FIPS 203**, not
round-3 Kyber (the two are not compatible): the shared secret is
K = G(m || H(ek)) with no further KDF, and key generation appends k to
the seed. `sw/common/tests` holds it to NIST's official FIPS 203
vectors and to an independent implementation (docs/mlkem.md).

The KyberSlash timing fixes are in: no division by q of a secret value
(the old divisions are left as comments in poly.c and polyvec.c; the
Makefile check in sw/common/tests confirms the compiled RISC-V code has
no divide instruction at all).

The authors no longer maintain it and point to **mlkem-native**
(https://github.com/pq-code-package/mlkem-native), which implements the
same standard, passes the same vectors, and could replace this behind
sw/common/zmlkem.h without any caller changing.

Built with KYBER_K=3 (ML-KEM-768) through sw/common/zmlkem.c, which also
does the input checks FIPS 203 requires and this code predates.
