set -e
cd "$1"
# A chain shaped like the ones that broke: a P-384 ECDSA intermediate
# signing a P-256 leaf with SHA-384, under a P-384 root. This is the
# DigiCert "Hybrid ECC SHA384" shape.
openssl ecparam -name secp384r1 -genkey -noout -out root384.key 2>/dev/null
openssl req -x509 -key root384.key -sha384 -out root384.crt -days 3650 \
  -subj "/CN=Test Root P384" -addext "basicConstraints=critical,CA:TRUE" 2>/dev/null
openssl ecparam -name secp384r1 -genkey -noout -out int384.key 2>/dev/null
openssl req -new -key int384.key -out int384.csr -subj "/CN=Test Intermediate P384" 2>/dev/null
openssl x509 -req -in int384.csr -CA root384.crt -CAkey root384.key -sha384 \
  -set_serial 20 -days 3650 \
  -extfile <(printf "basicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign,cRLSign\n") \
  -out int384.crt 2>/dev/null
openssl ecparam -name prime256v1 -genkey -noout -out leaf384.key 2>/dev/null
openssl req -new -key leaf384.key -out leaf384.csr -subj "/CN=ecc.example.test" 2>/dev/null
openssl x509 -req -in leaf384.csr -CA int384.crt -CAkey int384.key -sha384 \
  -set_serial 21 -days 3650 \
  -extfile <(printf "basicConstraints=critical,CA:FALSE\nsubjectAltName=DNS:ecc.example.test\n") \
  -out leaf384.crt 2>/dev/null
# An RSA chain signed with SHA-384, the other half of the same gap.
openssl req -x509 -newkey rsa:2048 -nodes -sha384 -keyout root384r.key \
  -out root384r.crt -days 3650 -subj "/CN=Test Root RSA384" \
  -addext "basicConstraints=critical,CA:TRUE" 2>/dev/null
openssl req -newkey rsa:2048 -nodes -keyout leaf384r.key -out leaf384r.csr \
  -subj "/CN=rsa384.example.test" 2>/dev/null
openssl x509 -req -in leaf384r.csr -CA root384r.crt -CAkey root384r.key -sha384 \
  -set_serial 22 -days 3650 \
  -extfile <(printf "subjectAltName=DNS:rsa384.example.test\n") -out leaf384r.crt 2>/dev/null
for f in root384 int384 leaf384 root384r leaf384r; do
  openssl x509 -in $f.crt -outform DER -out $f.der
done
rm -f *.csr *.key *.crt
ls *.der
