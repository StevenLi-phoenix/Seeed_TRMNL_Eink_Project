#!/usr/bin/env python3
"""Check that src/x509_crt_bundle.c lets the device verify a server, by
replaying the arduino-esp32 2.0.x esp_crt_bundle verify callback on the chain
the server actually presents.

The device (mbedTLS 2.28 + esp_crt_bundle.c) walks the presented chain to its
topmost certificate, looks up THAT certificate's issuer in the bundle (binary
search by DER name, memcmp over the bundle entry's length) and checks the
signature with the bundle key. A root that is in the bundle but presented
cross-signed by a root that is not still fails, which this script reproduces.

Usage: uv run --with cryptography python -I scripts/check_ca_bundle.py display.lishuyu.app [host ...]
"""
import os
import re
import socket
import ssl
import sys

from cryptography import x509
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa
from cryptography.hazmat.primitives.serialization import load_der_public_key

BUNDLE_C = os.environ.get('CA_BUNDLE_C') or os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'x509_crt_bundle.c')


def load_bundle(path):
    body = open(path).read().split('{', 1)[1].split('}', 1)[0]
    data = bytes(int(h, 16) for h in re.findall(r'0x([0-9a-f]{2})', body))
    count = data[0] << 8 | data[1]
    entries, off = [], 2
    for _ in range(count):
        name_len = data[off] << 8 | data[off + 1]
        key_len = data[off + 2] << 8 | data[off + 3]
        name = data[off + 4:off + 4 + name_len]
        key = data[off + 4 + name_len:off + 4 + name_len + key_len]
        entries.append((name, key))
        off += 4 + name_len + key_len
    assert off == len(data), 'trailing bytes in bundle'
    return entries


def find_issuer(entries, crt):
    """Same binary search as esp_crt_verify_callback()."""
    tbs = crt.tbs_certificate_bytes
    issuer = crt.issuer.public_bytes()
    tail = tbs[tbs.index(issuer):]  # memcmp may read past issuer_raw into the TBS
    start, end = 0, len(entries) - 1
    middle = (end - start) // 2
    while start <= end:
        name, key = entries[middle]
        probe = tail[:len(name)]
        if probe == name:
            return key
        if probe < name:
            end = middle - 1
        else:
            start = middle + 1
        middle = int((start + end) / 2)  # C integer division truncates toward zero
    return None


def check_sig(pub, crt):
    h = crt.signature_hash_algorithm
    if isinstance(pub, rsa.RSAPublicKey):
        pub.verify(crt.signature, crt.tbs_certificate_bytes, padding.PKCS1v15(), h)
    elif isinstance(pub, ec.EllipticCurvePublicKey):
        pub.verify(crt.signature, crt.tbs_certificate_bytes, ec.ECDSA(h))
    else:
        raise ValueError('unsupported key type %r' % type(pub))


def presented_chain(host, port=443):
    ctx = ssl.create_default_context()
    ctx.check_hostname = False  # only the presented chain is needed; the anchor check below is the test
    ctx.verify_mode = ssl.CERT_NONE
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2  # mbedTLS 2.28 speaks TLS <= 1.2
    with socket.create_connection((host, port), timeout=15) as sock:
        with ctx.wrap_socket(sock, server_hostname=host) as tls:
            return [x509.load_der_x509_certificate(c) for c in tls.get_unverified_chain()]


def check(host, entries):
    chain = presented_chain(host)
    for c in chain:
        print('   presented: %s  <-  %s' % (c.subject.rfc4514_string(), c.issuer.rfc4514_string()))
    # walk parent links inside the presented chain, like x509_crt_verify_chain()
    top = chain[0]
    rest = chain[1:]
    while True:
        parent = next((p for p in rest if p.subject == top.issuer and p is not top), None)
        if parent is None:
            break
        check_sig(parent.public_key(), top)
        rest = [p for p in rest if p is not parent]
        top = parent
    key = find_issuer(entries, top)
    if key is None:
        return 'FAIL: issuer of topmost cert (%s) not in bundle' % top.issuer.rfc4514_string()
    check_sig(load_der_public_key(key), top)
    return 'OK: %s anchored by bundle CA %s' % (top.subject.rfc4514_string(), top.issuer.rfc4514_string())


def main():
    entries = load_bundle(BUNDLE_C)
    print('bundle: %d CAs' % len(entries))
    failed = False
    for host in sys.argv[1:] or ['display.lishuyu.app']:
        print(host)
        try:
            verdict = check(host, entries)
        except Exception as e:  # noqa: BLE001 - report any verification error per host
            verdict = 'FAIL: %s' % e
        failed |= verdict.startswith('FAIL')
        print('   ' + verdict)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
