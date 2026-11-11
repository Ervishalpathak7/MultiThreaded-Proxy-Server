#!/usr/bin/env sh
# Generate a self-signed certificate for local testing into certs/.
#
#   ./scripts/gen-certs.sh            -> certs/server.crt + certs/server.key (CN=localhost)
#   ./scripts/gen-certs.sh example.com -> certs/example.com.crt + .key
set -e
cd "$(dirname "$0")/.."
mkdir -p certs
HOST="${1:-localhost}"
if [ "$HOST" = "localhost" ]; then
    CRT=certs/server.crt; KEY=certs/server.key
else
    CRT="certs/$HOST.crt"; KEY="certs/$HOST.key"
fi
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
    -keyout "$KEY" -out "$CRT" -days 365 -subj "/CN=$HOST" \
    -addext "subjectAltName=DNS:$HOST,DNS:localhost,IP:127.0.0.1,IP:::1" 2>/dev/null
echo "wrote $CRT and $KEY"
