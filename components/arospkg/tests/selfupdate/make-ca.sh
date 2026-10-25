#!/bin/sh
# A throwaway CA and a server certificate for IP 10.0.2.2, for relserver.py.
#   tests/selfupdate/make-ca.sh CERTDIR
# CERTDIR gets ca.crt (what the guest appends to its RAM copy of the bundle),
# server.crt and server.key. The CA's key is deleted once the server
# certificate is signed: nothing else can ever be issued under it.
set -e
D=${1:?usage: make-ca.sh CERTDIR}
mkdir -p "$D"; cd "$D"
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.crt -days 30 \
    -subj "/CN=arospkg self-update test CA" \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign" 2>/dev/null
openssl req -newkey rsa:2048 -nodes -keyout server.key -out server.csr -subj "/CN=10.0.2.2" 2>/dev/null
printf 'subjectAltName=IP:10.0.2.2\nextendedKeyUsage=serverAuth\nbasicConstraints=CA:FALSE\n' > ext.cnf
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out server.crt \
    -days 30 -extfile ext.cnf 2>/dev/null
rm -f ca.key server.csr ext.cnf ca.srl
openssl verify -CAfile ca.crt server.crt
