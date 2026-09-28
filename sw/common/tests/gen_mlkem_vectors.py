#!/usr/bin/env python3
# Regenerates mlkem_vectors.txt for test_zmlkem.c.
#
#   python3 gen_mlkem_vectors.py ML-KEM-keyGen-FIPS203.json ML-KEM-encapDecap-FIPS203.json > mlkem_vectors.txt
#
# The two JSON files are NIST's ACVP vectors for FIPS 203
# (github.com/usnistgov/ACVP-Server, gen-val/json-files/ML-KEM-*/
# internalProjection.json); only ML-KEM-768 is taken. Then cases from
# kyber-py (pip install kyber-py), an independent implementation of
# FIPS 203, on random inputs -- including tampered ciphertexts, whose
# implicit-rejection secret must match exactly, and keys that FIPS 203's
# checks must refuse.
#
# Lines:  <source> kg  <d> <z> <ek> <dk>
#         <source> enc <ek> <m> <c> <k>
#         <source> dec <dk> <c> <k>
#         <source> ekc <ek> <0|1>          (1: passes the modulus check)
#         <source> dkc <dk> <0|1>          (1: passes the hash check)
import json, os, random, sys
from kyber_py.ml_kem import ML_KEM_768 as K
random.seed(20260927)
def rb(n): return bytes(random.randrange(256) for _ in range(n))
out = []
kg, ed = json.load(open(sys.argv[1])), json.load(open(sys.argv[2]))
for g in kg["testGroups"]:
    if g["parameterSet"] != "ML-KEM-768": continue
    for t in g["tests"]: out.append("nist kg %s %s %s %s" % (t["d"], t["z"], t["ek"], t["dk"]))
for g in ed["testGroups"]:
    if g["parameterSet"] != "ML-KEM-768": continue
    f = g["function"]
    for t in g["tests"]:
        if f == "encapsulation": out.append("nist enc %s %s %s %s" % (t["ek"], t["m"], t["c"], t["k"]))
        elif f == "decapsulation": out.append("nist dec %s %s %s" % (t["dk"], t["c"], t["k"]))
        elif f == "encapsulationKeyCheck": out.append("nist ekc %s %d" % (t["ek"], 1 if t["testPassed"] else 0))
        elif f == "decapsulationKeyCheck": out.append("nist dkc %s %d" % (t["dk"], 1 if t["testPassed"] else 0))
for i in range(40):
    d, z, m = rb(32), rb(32), rb(32)
    ek, dk = K._keygen_internal(d, z)
    out.append("kyberpy kg %s %s %s %s" % (d.hex(), z.hex(), ek.hex(), dk.hex()))
    k, c = K._encaps_internal(ek, m)
    assert len(c) == 1088 and len(k) == 32
    out.append("kyberpy enc %s %s %s %s" % (ek.hex(), m.hex(), c.hex(), k.hex()))
    out.append("kyberpy dec %s %s %s" % (dk.hex(), c.hex(), k.hex()))
    bad = bytearray(c); bad[random.randrange(len(bad))] ^= 1 << random.randrange(8)
    out.append("kyberpy dec %s %s %s" % (dk.hex(), bytes(bad).hex(), K._decaps_internal(dk, bytes(bad)).hex()))
    # a coefficient of ek set to q (3329) or 4095: the modulus check refuses it
    e = bytearray(ek); j = random.randrange(384) * 3; v = random.choice([3329, 4095])
    e[j] = v & 0xFF; e[j + 1] = (e[j + 1] & 0xF0) | (v >> 8)
    out.append("kyberpy ekc %s 0" % bytes(e).hex())
    out.append("kyberpy ekc %s 1" % ek.hex())
    # dk with its copy of H(ek) changed: the hash check refuses it
    dd = bytearray(dk); dd[2400 - 64 + random.randrange(32)] ^= 1
    out.append("kyberpy dkc %s 0" % bytes(dd).hex())
print(len(out))
print("\n".join(out))
print("# %d lines" % len(out), file=sys.stderr)
