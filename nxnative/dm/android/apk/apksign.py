# apksign.py: the stored ZIP and the APK Signature Scheme v2 signer (RSA-2048, SHA-256, self-signed certificate) of mkapk.py, with the Python standard library only.
# mkapk.py builds an APK with them; apkdriver.py rebuilds an installed one with another Vulkan driver library
import hashlib, secrets, struct, zlib

# ---------------- RSA, DER, self-signed certificate ----------------
def is_prime(n):
    if n < 2: return False
    for p in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if n % p == 0: return n == p
    d, r = n - 1, 0
    while d % 2 == 0: d //= 2; r += 1
    for _ in range(40):
        x = pow(secrets.randbelow(n - 3) + 2, d, n)
        if x in (1, n - 1): continue
        for _ in range(r - 1):
            x = pow(x, 2, n)
            if x == n - 1: break
        else: return False
    return True
def gen_prime(bits):
    while True:
        p = secrets.randbits(bits) | (3 << (bits - 2)) | 1
        if p % 65537 != 1 and is_prime(p): return p
def gen_key():
    p, q = gen_prime(1024), gen_prime(1024)
    n, e = p * q, 65537
    return {'n': n, 'e': e, 'd': pow(e, -1, (p - 1) * (q - 1))}

def der(tag, body):
    l = len(body)
    ln = bytes([l]) if l < 0x80 else (lambda b: bytes([0x80 | len(b)]) + b)(l.to_bytes((l.bit_length() + 7) // 8, 'big'))
    return bytes([tag]) + ln + body
def der_int(v): return der(0x02, v.to_bytes(v.bit_length() // 8 + 1, 'big'))
def der_seq(*xs): return der(0x30, b''.join(xs))
def der_oid(s):
    a = [int(x) for x in s.split('.')]; out = bytes([a[0] * 40 + a[1]])
    for v in a[2:]:
        b = [v & 0x7f]; v >>= 7
        while v: b.append(0x80 | (v & 0x7f)); v >>= 7
        out += bytes(reversed(b))
    return der(0x06, out)
NULL = b'\x05\x00'
ALG_SHA256_RSA = der_seq(der_oid('1.2.840.113549.1.1.11'), NULL)
def spki(k): return der_seq(der_seq(der_oid('1.2.840.113549.1.1.1'), NULL), der(0x03, b'\0' + der_seq(der_int(k['n']), der_int(k['e']))))
def rsa_sign(k, msg):
    di = bytes.fromhex('3031300d060960864801650304020105000420') + hashlib.sha256(msg).digest()
    kl = (k['n'].bit_length() + 7) // 8
    em = b'\x00\x01' + b'\xff' * (kl - len(di) - 3) + b'\x00' + di
    return pow(int.from_bytes(em, 'big'), k['d'], k['n']).to_bytes(kl, 'big')
def cert(k):
    name = der_seq(der(0x31, der_seq(der_oid('2.5.4.3'), der(0x0c, b'nx2a probe'))))
    validity = der_seq(der(0x17, b'260101000000Z'), der(0x18, b'20560101000000Z'))
    tbs = der_seq(der(0xa0, der_int(2)), der_int(secrets.randbits(62) | 1), ALG_SHA256_RSA, name, validity, name, spki(k))
    return der_seq(tbs, ALG_SHA256_RSA, der(0x03, b'\0' + rsa_sign(k, tbs)))

# ---------------- ZIP (stored) ----------------
def build_zip(entries):   # entries: [(name, data, align)]
    out, cd = bytearray(), bytearray()
    dos_time, dos_date = 0, (2026 - 1980) << 9 | 1 << 5 | 1
    for name, data, align in entries:
        nb = name.encode(); crc = zlib.crc32(data)
        pad = -(len(out) + 30 + len(nb)) % align
        if 0 < pad < 4: pad += align   # too small for an extra field header: go to the next alignment step
        extra = struct.pack('<HH', 0xd935, pad - 4) + b'\0' * (pad - 4) if pad else b''   # 0xd935: zipalign's alignment extra field
        off = len(out)
        out += struct.pack('<IHHHHHIIIHH', 0x04034b50, 10, 0, 0, dos_time, dos_date, crc, len(data), len(data), len(nb), len(extra)) + nb + extra + data
        cd += struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, 10, 10, 0, 0, dos_time, dos_date, crc, len(data), len(data), len(nb), 0, 0, 0, 0, 0, off) + nb
    eocd = struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, len(entries), len(entries), len(cd), len(out), 0)
    return bytes(out), bytes(cd), eocd

# ---------------- APK Signature Scheme v2 ----------------
def lp(b): return struct.pack('<I', len(b)) + b
def chunked_sha256(sections):
    digests = []
    for sec in sections:
        for i in range(0, len(sec), 1 << 20):
            c = sec[i:i + (1 << 20)]
            digests.append(hashlib.sha256(b'\xa5' + struct.pack('<I', len(c)) + c).digest())
    return hashlib.sha256(b'\x5a' + struct.pack('<I', len(digests)) + b''.join(digests)).digest()
def sign_v2(zip_entries, cd, eocd, k, crt):
    ALG = 0x0103   # RSASSA-PKCS1-v1_5 with SHA2-256
    digest = chunked_sha256([zip_entries, cd, eocd])   # EOCD's CD offset still points at the CD start = where the signing block will begin
    signed = lp(lp(struct.pack('<I', ALG) + lp(digest))) + lp(lp(crt)) + lp(b'')
    signer = lp(signed) + lp(lp(struct.pack('<I', ALG) + lp(rsa_sign(k, signed)))) + lp(spki(k))
    value = lp(lp(signer))
    pair = struct.pack('<QI', 4 + len(value), 0x7109871a) + value
    size = len(pair) + 8 + 16
    block = struct.pack('<Q', size) + pair + struct.pack('<Q', size) + b'APK Sig Block 42'
    new_eocd = eocd[:16] + struct.pack('<I', len(zip_entries) + len(block)) + eocd[20:]
    return zip_entries + block + cd + new_eocd
