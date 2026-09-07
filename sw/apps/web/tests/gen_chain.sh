set -e
cd "$1"
mk() { openssl req -x509 -newkey $2 -nodes -keyout $1.key -out $1.crt -days 3650 -subj "/CN=$3" -addext "basicConstraints=critical,CA:TRUE" >/dev/null 2>&1; }
# RSA root -> RSA intermediate -> RSA leaf
mk rootr rsa:2048 "Test Root RSA"
openssl req -newkey rsa:2048 -nodes -keyout interr.key -out interr.csr -subj "/CN=Test Intermediate RSA" >/dev/null 2>&1
openssl x509 -req -in interr.csr -CA rootr.crt -CAkey rootr.key -set_serial 2 -days 3650 \
  -extfile <(printf "basicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign,cRLSign\n") -out interr.crt >/dev/null 2>&1
openssl req -newkey rsa:2048 -nodes -keyout leafr.key -out leafr.csr -subj "/CN=example.test" >/dev/null 2>&1
openssl x509 -req -in leafr.csr -CA interr.crt -CAkey interr.key -set_serial 3 -days 3650 \
  -extfile <(printf "basicConstraints=critical,CA:FALSE\nsubjectAltName=DNS:example.test,DNS:*.example.test\n") -out leafr.crt >/dev/null 2>&1
# ECDSA root -> ECDSA leaf
openssl ecparam -name prime256v1 -genkey -noout -out roote.key >/dev/null 2>&1
openssl req -x509 -key roote.key -out roote.crt -days 3650 -subj "/CN=Test Root EC" -addext "basicConstraints=critical,CA:TRUE" >/dev/null 2>&1
openssl ecparam -name prime256v1 -genkey -noout -out leafe.key >/dev/null 2>&1
openssl req -new -key leafe.key -out leafe.csr -subj "/CN=ec.example.test" >/dev/null 2>&1
openssl x509 -req -in leafe.csr -CA roote.crt -CAkey roote.key -set_serial 4 -days 3650 \
  -extfile <(printf "basicConstraints=critical,CA:FALSE\nsubjectAltName=DNS:ec.example.test\n") -out leafe.crt >/dev/null 2>&1
# a leaf signed by the RSA root directly but marked CA:FALSE, used as an "intermediate"
openssl req -newkey rsa:2048 -nodes -keyout notca.key -out notca.csr -subj "/CN=Not A CA" >/dev/null 2>&1
openssl x509 -req -in notca.csr -CA rootr.crt -CAkey rootr.key -set_serial 5 -days 3650 \
  -extfile <(printf "basicConstraints=critical,CA:FALSE\n") -out notca.crt >/dev/null 2>&1
openssl req -newkey rsa:2048 -nodes -keyout under.key -out under.csr -subj "/CN=under.test" >/dev/null 2>&1
openssl x509 -req -in under.csr -CA notca.crt -CAkey notca.key -set_serial 6 -days 3650 \
  -extfile <(printf "subjectAltName=DNS:under.test\n") -out under.crt >/dev/null 2>&1
# expired leaf
faketime_ok=0
openssl req -newkey rsa:2048 -nodes -keyout exp.key -out exp.csr -subj "/CN=old.test" >/dev/null 2>&1
openssl x509 -req -in exp.csr -CA interr.crt -CAkey interr.key -set_serial 7 -days 1 \
  -extfile <(printf "subjectAltName=DNS:old.test\n") -out exp.crt >/dev/null 2>&1
for f in *.crt; do openssl x509 -in $f -outform DER -out ${f%.crt}.der; done
rm -f *.csr *.key *.crt
ls
