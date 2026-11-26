#!/usr/bin/env bash
# Regenerate src/x509_crt_bundle.c from curl's extract of the Mozilla CA store
# plus scripts/ca/extra_roots.pem. Needs curl, shasum and uv.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d /tmp/trmnl-cacert.XXXXXX)"
trap 'rm -rf "$work"' EXIT

curl -fsSL --max-time 60 -o "$work/cacert.pem" https://curl.se/ca/cacert.pem
curl -fsSL --max-time 60 -o "$work/cacert.pem.sha256" https://curl.se/ca/cacert.pem.sha256
(cd "$work" && shasum -a 256 -c cacert.pem.sha256)

uv run -q --with cryptography python -I "$repo/scripts/gen_ca_bundle.py" \
  --input "$work/cacert.pem" "$repo/scripts/ca/extra_roots.pem" \
  --out "$repo/src/x509_crt_bundle.c"
