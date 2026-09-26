// The `crypto` standard library module (import 'crypto'). It is linked only
// when a program imports 'crypto'. Signatures must match the crypto rows in
// src/builtins.nio and genCryptoCall in src/codegen.nio.
//
// Rules for all code here that handles secrets:
//   - No branches that depend on a secret, and no loads indexed by a secret.
//     Control flow can depend only on input lengths, which are public.
//   - No original cryptographic constructions. Each body follows a spec or a
//     vetted implementation, named at its section. tests/crypto_test.nio
//     checks each one against published vectors.
//   - Secret comparisons go through ct_equal.
//   - The base64 and hex decoders branch on input byte values. They must not
//     receive secret key material. A PEM private-key reader needs its own
//     constant-time decoder.
//
// The SHA constant tables are generated from FIPS 180-4. They are not copied.
// This file requires a 64-bit target and __int128, like types.naturalBits.

#include "runtime.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <sys/random.h>
#else
#include <errno.h>
#include <sys/random.h>
#endif

// Root descriptor for a byte[]. The elements are scalars, so tracing stops at
// the block.
static TypeDesc td_byte_array = {TD_ARRAY, &rt_td_uint8, 0, NULL, NULL, 0};

// ---- SHA-256 (FIPS 180-4) ----

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

typedef struct {
    uint32_t h[8];
    uint64_t nbytes;
    uint8_t buf[64];
    size_t buflen;
} Sha256;

static inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_init(Sha256 *s) {
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(s->h, iv, sizeof iv);
    s->nbytes = 0;
    s->buflen = 0;
}

static void sha256_block(Sha256 *s, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_update(Sha256 *s, const uint8_t *p, size_t n) {
    s->nbytes += n;
    if (s->buflen > 0) {
        size_t take = 64 - s->buflen;
        if (take > n) take = n;
        memcpy(s->buf + s->buflen, p, take);
        s->buflen += take;
        p += take;
        n -= take;
        if (s->buflen == 64) {
            sha256_block(s, s->buf);
            s->buflen = 0;
        }
    }
    while (n >= 64) {
        sha256_block(s, p);
        p += 64;
        n -= 64;
    }
    if (n > 0) {
        memcpy(s->buf, p, n);
        s->buflen = n;
    }
}

static void sha256_final(Sha256 *s, uint8_t out[32]) {
    uint64_t bitlen = s->nbytes * 8;
    uint8_t pad[72];
    size_t padlen = (s->buflen < 56) ? 56 - s->buflen : 120 - s->buflen;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (int i = 0; i < 8; i++) pad[padlen + i] = (uint8_t)(bitlen >> (56 - 8 * i));
    sha256_update(s, pad, padlen + 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)(s->h[i]);
    }
}

// ---- SHA-512 core, of which SHA-384 is the truncation (FIPS 180-4) ----

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

typedef struct {
    uint64_t h[8];
    uint64_t nbytes;
    uint8_t buf[128];
    size_t buflen;
} Sha512;

static inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static void sha384_init(Sha512 *s) {
    static const uint64_t iv[8] = {
        0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
        0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL,
    };
    memcpy(s->h, iv, sizeof iv);
    s->nbytes = 0;
    s->buflen = 0;
}

static void sha512_block(Sha512 *s, const uint8_t *p) {
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint64_t)p[8 * i] << 56 | (uint64_t)p[8 * i + 1] << 48 |
               (uint64_t)p[8 * i + 2] << 40 | (uint64_t)p[8 * i + 3] << 32 |
               (uint64_t)p[8 * i + 4] << 24 | (uint64_t)p[8 * i + 5] << 16 |
               (uint64_t)p[8 * i + 6] << 8 | (uint64_t)p[8 * i + 7];
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint64_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + K512[i] + w[i];
        uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha512_update(Sha512 *s, const uint8_t *p, size_t n) {
    s->nbytes += n;
    if (s->buflen > 0) {
        size_t take = 128 - s->buflen;
        if (take > n) take = n;
        memcpy(s->buf + s->buflen, p, take);
        s->buflen += take;
        p += take;
        n -= take;
        if (s->buflen == 128) {
            sha512_block(s, s->buf);
            s->buflen = 0;
        }
    }
    while (n >= 128) {
        sha512_block(s, p);
        p += 128;
        n -= 128;
    }
    if (n > 0) {
        memcpy(s->buf, p, n);
        s->buflen = n;
    }
}

// The length field is 128 bits (FIPS 180-4 §5.1.2). A uint64 byte count fills
// only the low half, so the high 8 bytes of the pad stay zero.
static void sha384_final(Sha512 *s, uint8_t out[48]) {
    uint64_t bitlen = s->nbytes * 8;
    uint8_t pad[144];
    size_t padlen = (s->buflen < 112) ? 112 - s->buflen : 240 - s->buflen;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (int i = 0; i < 8; i++) pad[padlen + 8 + i] = (uint8_t)(bitlen >> (56 - 8 * i));
    sha512_update(s, pad, padlen + 16);
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 8; j++) {
            out[8 * i + j] = (uint8_t)(s->h[i] >> (56 - 8 * j));
        }
    }
}

// ---- one-shot digests over the two ----

#define ALG_SHA256 0
#define ALG_SHA384 1

static const size_t alg_digest[2] = {32, 48};
static const size_t alg_block[2] = {64, 128};

static void digest_of(int alg, const uint8_t *p, size_t n, uint8_t *out) {
    if (alg == ALG_SHA256) {
        Sha256 s;
        sha256_init(&s);
        sha256_update(&s, p, n);
        sha256_final(&s, out);
    } else {
        Sha512 s;
        sha384_init(&s);
        sha512_update(&s, p, n);
        sha384_final(&s, out);
    }
}

// ---- HMAC (RFC 2104), as a streaming context over either core ----

typedef struct {
    int alg;
    uint8_t k0[128]; // block-sized key, used again for the outer pass
    Sha256 s256;
    Sha512 s512;
} Hmac;

static void hmac_init(Hmac *h, int alg, const uint8_t *key, size_t keylen) {
    size_t block = alg_block[alg];
    h->alg = alg;
    memset(h->k0, 0, block);
    if (keylen > block) {
        digest_of(alg, key, keylen, h->k0);
    } else {
        memcpy(h->k0, key, keylen);
    }
    uint8_t pad[128];
    for (size_t i = 0; i < block; i++) pad[i] = h->k0[i] ^ 0x36;
    if (alg == ALG_SHA256) {
        sha256_init(&h->s256);
        sha256_update(&h->s256, pad, block);
    } else {
        sha384_init(&h->s512);
        sha512_update(&h->s512, pad, block);
    }
}

static void hmac_update(Hmac *h, const uint8_t *p, size_t n) {
    if (h->alg == ALG_SHA256) {
        sha256_update(&h->s256, p, n);
    } else {
        sha512_update(&h->s512, p, n);
    }
}

static void hmac_final(Hmac *h, uint8_t *out) {
    size_t block = alg_block[h->alg];
    size_t dlen = alg_digest[h->alg];
    uint8_t inner[48];
    uint8_t pad[128];
    for (size_t i = 0; i < block; i++) pad[i] = h->k0[i] ^ 0x5c;
    if (h->alg == ALG_SHA256) {
        sha256_final(&h->s256, inner);
        sha256_init(&h->s256);
        sha256_update(&h->s256, pad, block);
        sha256_update(&h->s256, inner, dlen);
        sha256_final(&h->s256, out);
    } else {
        sha384_final(&h->s512, inner);
        sha384_init(&h->s512);
        sha512_update(&h->s512, pad, block);
        sha512_update(&h->s512, inner, dlen);
        sha384_final(&h->s512, out);
    }
}

static void hmac_of(int alg, const uint8_t *key, size_t keylen,
                    const uint8_t *msg, size_t msglen, uint8_t *out) {
    Hmac h;
    hmac_init(&h, alg, key, keylen);
    hmac_update(&h, msg, msglen);
    hmac_final(&h, out);
}

// ---- HKDF (RFC 5869) ----

// T(i) = HMAC(prk, T(i-1) | info | i), RFC 5869 §2.3. The caller computes the
// length, and it never comes from outside input, so the 255-block limit
// panics.
static void hkdf_expand_of(int alg, const uint8_t *prk, size_t prklen,
                           const uint8_t *info, size_t infolen,
                           uint8_t *out, size_t outlen) {
    size_t dlen = alg_digest[alg];
    uint8_t t[48];
    size_t tlen = 0;
    size_t done = 0;
    uint8_t counter = 1;
    while (done < outlen) {
        Hmac h;
        hmac_init(&h, alg, prk, prklen);
        hmac_update(&h, t, tlen);
        hmac_update(&h, info, infolen);
        hmac_update(&h, &counter, 1);
        hmac_final(&h, t);
        tlen = dlen;
        size_t take = outlen - done < dlen ? outlen - done : dlen;
        memcpy(out + done, t, take);
        done += take;
        counter++;
    }
}

// ---- ChaCha20 (RFC 8439 §2.3) ----
// It uses no tables, so no load is indexed by a secret.

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define QUARTERROUND(a, b, c, d)              \
    a += b; d ^= a; d = ROTL32(d, 16);        \
    c += d; b ^= c; b = ROTL32(b, 12);        \
    a += b; d ^= a; d = ROTL32(d, 8);         \
    c += d; b ^= c; b = ROTL32(b, 7);

static uint32_t load32_le(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void store32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// One 64-byte keystream block. s[0..3] are the ASCII of "expand 32-byte k".
static void chacha20_block(const uint8_t key[32], uint32_t counter,
                           const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = load32_le(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = load32_le(nonce + 4 * i);

    uint32_t w[16];
    memcpy(w, s, sizeof w);
    for (int i = 0; i < 10; i++) {
        QUARTERROUND(w[0], w[4], w[8], w[12])
        QUARTERROUND(w[1], w[5], w[9], w[13])
        QUARTERROUND(w[2], w[6], w[10], w[14])
        QUARTERROUND(w[3], w[7], w[11], w[15])
        QUARTERROUND(w[0], w[5], w[10], w[15])
        QUARTERROUND(w[1], w[6], w[11], w[12])
        QUARTERROUND(w[2], w[7], w[8], w[13])
        QUARTERROUND(w[3], w[4], w[9], w[14])
    }
    for (int i = 0; i < 16; i++) store32_le(out + 4 * i, w[i] + s[i]);
}

// XORs the keystream from block `counter` into out. out can alias in (§2.4).
static void chacha20_xor(const uint8_t key[32], uint32_t counter,
                         const uint8_t nonce[12], const uint8_t *in,
                         uint8_t *out, size_t len) {
    uint8_t block[64];
    size_t done = 0;
    while (done < len) {
        chacha20_block(key, counter, nonce, block);
        size_t take = len - done < 64 ? len - done : 64;
        for (size_t i = 0; i < take; i++) out[done + i] = (uint8_t)(in[done + i] ^ block[i]);
        done += take;
        counter++;
    }
}

// ---- Poly1305 (RFC 8439 §2.5) ----
//
// Ported from the 64-bit implementation of poly1305-donna (Andrew Moon, public
// domain, https://github.com/floodyberry/poly1305-donna): 44/44/42-bit limbs
// with 128-bit products. It is a one-time MAC: a key authenticates one
// message. The per-nonce key derivation of the AEAD guarantees this.

typedef unsigned __int128 poly1305_uint128;

typedef struct {
    uint64_t r[3];
    uint64_t h[3];
    uint64_t pad[2];
    size_t leftover;
    uint8_t buffer[16];
    uint8_t final;
} Poly1305;

static uint64_t load64_le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void store64_le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void poly1305_init(Poly1305 *st, const uint8_t key[32]) {
    uint64_t t0 = load64_le(key);
    uint64_t t1 = load64_le(key + 8);
    // r &= 0xffffffc0ffffffc0ffffffc0fffffff: the clamp of RFC 8439 §2.5.
    st->r[0] = t0 & 0xffc0fffffff;
    st->r[1] = ((t0 >> 44) | (t1 << 20)) & 0xfffffc0ffff;
    st->r[2] = (t1 >> 24) & 0x00ffffffc0f;
    st->h[0] = 0;
    st->h[1] = 0;
    st->h[2] = 0;
    st->pad[0] = load64_le(key + 16);
    st->pad[1] = load64_le(key + 24);
    st->leftover = 0;
    st->final = 0;
}

static void poly1305_blocks(Poly1305 *st, const uint8_t *m, size_t bytes) {
    const uint64_t hibit = st->final ? 0 : ((uint64_t)1 << 40); // 1 << 128
    uint64_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2];
    uint64_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2];
    uint64_t s1 = r1 * (5 << 2);
    uint64_t s2 = r2 * (5 << 2);

    while (bytes >= 16) {
        uint64_t t0 = load64_le(m);
        uint64_t t1 = load64_le(m + 8);

        // h += m[i]
        h0 += t0 & 0xfffffffffff;
        h1 += ((t0 >> 44) | (t1 << 20)) & 0xfffffffffff;
        h2 += (((t1 >> 24)) & 0x3ffffffffff) | hibit;

        // h *= r
        poly1305_uint128 d0 = (poly1305_uint128)h0 * r0 +
                              (poly1305_uint128)h1 * s2 + (poly1305_uint128)h2 * s1;
        poly1305_uint128 d1 = (poly1305_uint128)h0 * r1 +
                              (poly1305_uint128)h1 * r0 + (poly1305_uint128)h2 * s2;
        poly1305_uint128 d2 = (poly1305_uint128)h0 * r2 +
                              (poly1305_uint128)h1 * r1 + (poly1305_uint128)h2 * r0;

        // (partial) h %= p
        uint64_t c = (uint64_t)(d0 >> 44);
        h0 = (uint64_t)d0 & 0xfffffffffff;
        d1 += c;
        c = (uint64_t)(d1 >> 44);
        h1 = (uint64_t)d1 & 0xfffffffffff;
        d2 += c;
        c = (uint64_t)(d2 >> 42);
        h2 = (uint64_t)d2 & 0x3ffffffffff;
        h0 += c * 5;
        c = h0 >> 44;
        h0 &= 0xfffffffffff;
        h1 += c;

        m += 16;
        bytes -= 16;
    }
    st->h[0] = h0;
    st->h[1] = h1;
    st->h[2] = h2;
}

static void poly1305_update(Poly1305 *st, const uint8_t *m, size_t bytes) {
    if (st->leftover > 0) {
        size_t want = 16 - st->leftover;
        if (want > bytes) want = bytes;
        memcpy(st->buffer + st->leftover, m, want);
        bytes -= want;
        m += want;
        st->leftover += want;
        if (st->leftover < 16) return;
        poly1305_blocks(st, st->buffer, 16);
        st->leftover = 0;
    }
    if (bytes >= 16) {
        size_t want = bytes & ~((size_t)15);
        poly1305_blocks(st, m, want);
        m += want;
        bytes -= want;
    }
    if (bytes > 0) {
        memcpy(st->buffer + st->leftover, m, bytes);
        st->leftover += bytes;
    }
}

static void poly1305_finish(Poly1305 *st, uint8_t mac[16]) {
    if (st->leftover > 0) {
        size_t i = st->leftover;
        st->buffer[i++] = 1;
        for (; i < 16; i++) st->buffer[i] = 0;
        st->final = 1;
        poly1305_blocks(st, st->buffer, 16);
    }

    uint64_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], c;

    // fully carry h
    c = h1 >> 44; h1 &= 0xfffffffffff;
    h2 += c;     c = h2 >> 42; h2 &= 0x3ffffffffff;
    h0 += c * 5; c = h0 >> 44; h0 &= 0xfffffffffff;
    h1 += c;     c = h1 >> 44; h1 &= 0xfffffffffff;
    h2 += c;     c = h2 >> 42; h2 &= 0x3ffffffffff;
    h0 += c * 5; c = h0 >> 44; h0 &= 0xfffffffffff;
    h1 += c;

    // compute h + -p
    uint64_t g0 = h0 + 5; c = g0 >> 44; g0 &= 0xfffffffffff;
    uint64_t g1 = h1 + c; c = g1 >> 44; g1 &= 0xfffffffffff;
    uint64_t g2 = h2 + c - ((uint64_t)1 << 42);

    // select h if h < p, or h + -p if h >= p, with a mask and no branch
    c = (g2 >> 63) - 1;
    g0 &= c;
    g1 &= c;
    g2 &= c;
    c = ~c;
    h0 = (h0 & c) | g0;
    h1 = (h1 & c) | g1;
    h2 = (h2 & c) | g2;

    // h = h + pad
    uint64_t t0 = st->pad[0];
    uint64_t t1 = st->pad[1];
    h0 += t0 & 0xfffffffffff;             c = h0 >> 44; h0 &= 0xfffffffffff;
    h1 += (((t0 >> 44) | (t1 << 20)) & 0xfffffffffff) + c;
                                          c = h1 >> 44; h1 &= 0xfffffffffff;
    h2 += (((t1 >> 24)) & 0x3ffffffffff) + c;           h2 &= 0x3ffffffffff;

    // mac = h % 2^128
    store64_le(mac, (h0) | (h1 << 44));
    store64_le(mac + 8, (h1 >> 20) | (h2 << 24));

    // Clear the one-time key. The copies on the caller's heap remain.
    memset(st, 0, sizeof *st);
}

// ---- AEAD_CHACHA20_POLY1305 (RFC 8439 §2.8) ----

#define AEAD_KEY_LEN 32
#define AEAD_NONCE_LEN 12
#define AEAD_TAG_LEN 16

// Constant-time comparison. An early exit tells an attacker how much of a
// forged tag was correct. The construction is the same as in lib/string.c.
static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return (int)((((unsigned int)diff - 1u) >> 8) & 1u);
}

// The MAC input of §2.8: aad || pad16(aad) || ct || pad16(ct) || le64(|aad|) ||
// le64(|ct|). The padding prevents an undetected move of a byte between the
// two fields.
static void aead_tag(const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *ct, size_t ct_len, uint8_t tag[16]) {
    // The one-time Poly1305 key is the first 32 bytes of block 0 (§2.6).
    uint8_t block0[64];
    chacha20_block(key, 0, nonce, block0);

    Poly1305 st;
    poly1305_init(&st, block0);

    static const uint8_t zeros[16] = {0};
    poly1305_update(&st, aad, aad_len);
    if (aad_len % 16 != 0) poly1305_update(&st, zeros, 16 - (aad_len % 16));
    poly1305_update(&st, ct, ct_len);
    if (ct_len % 16 != 0) poly1305_update(&st, zeros, 16 - (ct_len % 16));

    uint8_t lengths[16];
    store64_le(lengths, (uint64_t)aad_len);
    store64_le(lengths + 8, (uint64_t)ct_len);
    poly1305_update(&st, lengths, 16);

    poly1305_finish(&st, tag);
    memset(block0, 0, sizeof block0);
}

// ---- X25519 (RFC 7748) ----
//
// The field arithmetic is the generated, formally verified code of fiat-crypto.
// Curve math is never written by hand. fiat_curve25519_64.h is byte-for-byte
// the file fiat-crypto publishes as fiat-c/src/curve25519_64.c (SHA-256
// 645233c37707ba0580338aa84d8380357078a2c9bb2db80f0c5ff4e979650e3e), with
// only a rename. Never edit it. MIT/Apache-2.0/BSD-1-Clause. It is included
// and not compiled alone, because fiat generates it with `--static`.
//
// The Montgomery ladder is BoringSSL's x25519_scalar_mult_generic, the
// arrangement that its Coq proof covers. That includes the cases that the
// naive formula gets wrong (x1 = 0, and points on the quadratic twist). The
// inversion chain is BoringSSL's fe_loose_invert.
#include "fiat_curve25519_64.h"

typedef fiat_25519_tight_field_element fe;
typedef fiat_25519_loose_field_element fe_loose;

// Replaces (f, g) with (g, f) when b is 1. It uses a mask and no branch,
// because the swap bit comes from the secret scalar.
static void fe_cswap(uint64_t f[5], uint64_t g[5], uint64_t b) {
    uint64_t mask = 0 - b;
    for (int i = 0; i < 5; i++) {
        uint64_t x = (f[i] ^ g[i]) & mask;
        f[i] ^= x;
        g[i] ^= x;
    }
}

// out = z^-1 = z^(p-2), or 0 for z = 0. The chain is BoringSSL's, unchanged.
static void fe_invert(fe out, const fe z) {
    fe t0, t1, t2, t3;
    int i;

    fiat_25519_carry_square(t0, z);
    fiat_25519_carry_square(t1, t0);
    for (i = 1; i < 2; ++i) fiat_25519_carry_square(t1, t1);
    fiat_25519_carry_mul(t1, z, t1);
    fiat_25519_carry_mul(t0, t0, t1);
    fiat_25519_carry_square(t2, t0);
    fiat_25519_carry_mul(t1, t1, t2);
    fiat_25519_carry_square(t2, t1);
    for (i = 1; i < 5; ++i) fiat_25519_carry_square(t2, t2);
    fiat_25519_carry_mul(t1, t2, t1);
    fiat_25519_carry_square(t2, t1);
    for (i = 1; i < 10; ++i) fiat_25519_carry_square(t2, t2);
    fiat_25519_carry_mul(t2, t2, t1);
    fiat_25519_carry_square(t3, t2);
    for (i = 1; i < 20; ++i) fiat_25519_carry_square(t3, t3);
    fiat_25519_carry_mul(t2, t3, t2);
    fiat_25519_carry_square(t2, t2);
    for (i = 1; i < 10; ++i) fiat_25519_carry_square(t2, t2);
    fiat_25519_carry_mul(t1, t2, t1);
    fiat_25519_carry_square(t2, t1);
    for (i = 1; i < 50; ++i) fiat_25519_carry_square(t2, t2);
    fiat_25519_carry_mul(t2, t2, t1);
    fiat_25519_carry_square(t3, t2);
    for (i = 1; i < 100; ++i) fiat_25519_carry_square(t3, t3);
    fiat_25519_carry_mul(t2, t3, t2);
    fiat_25519_carry_square(t2, t2);
    for (i = 1; i < 50; ++i) fiat_25519_carry_square(t2, t2);
    fiat_25519_carry_mul(t1, t2, t1);
    fiat_25519_carry_square(t1, t1);
    for (i = 1; i < 5; ++i) fiat_25519_carry_square(t1, t1);
    fiat_25519_carry_mul(out, t1, t0);
}

// `scalar` is clamped as RFC 7748 §5 requires: the low three bits cleared,
// bit 254 set, bit 255 cleared. The RFC puts the clamp here, and not in the
// caller.
static void x25519_scalar_mult(uint8_t out[32], const uint8_t scalar[32],
                               const uint8_t point[32]) {
    fe x1, x2, z2, x3, z3, tmp0, tmp1;
    fe_loose x2l, z2l, x3l, tmp0l, tmp1l;

    uint8_t e[32];
    memcpy(e, scalar, 32);
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;

    // fiat's from_bytes requires a value below 2^255, so the high bit of the
    // u-coordinate is masked off (RFC 7748 §5). The second test vector of the
    // RFC sets that bit, to catch an implementation that skips this.
    uint8_t u[32];
    memcpy(u, point, 32);
    u[31] &= 0x7f;
    fiat_25519_from_bytes(x1, u);
    // x2 = 1, z2 = 0, x3 = x1, z3 = 1
    memset(x2, 0, sizeof x2);
    x2[0] = 1;
    memset(z2, 0, sizeof z2);
    memcpy(x3, x1, sizeof x3);
    memset(z3, 0, sizeof z3);
    z3[0] = 1;

    uint64_t swap = 0;
    for (int pos = 254; pos >= 0; --pos) {
        uint64_t b = 1 & ((uint64_t)e[pos / 8] >> (pos & 7));
        swap ^= b;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = b;
        // The ladder step, in the order of BoringSSL's Coq transcription.
        fiat_25519_sub(tmp0l, x3, z3);
        fiat_25519_sub(tmp1l, x2, z2);
        fiat_25519_add(x2l, x2, z2);
        fiat_25519_add(z2l, x3, z3);
        fiat_25519_carry_mul(z3, tmp0l, x2l);
        fiat_25519_carry_mul(z2, z2l, tmp1l);
        fiat_25519_carry_square(tmp0, tmp1l);
        fiat_25519_carry_square(tmp1, x2l);
        fiat_25519_add(x3l, z3, z2);
        fiat_25519_sub(z2l, z3, z2);
        fiat_25519_carry_mul(x2, tmp1, tmp0);
        fiat_25519_sub(tmp1l, tmp1, tmp0);
        fiat_25519_carry_square(z2, z2l);
        fiat_25519_carry_scmul_121666(z3, tmp1l);
        fiat_25519_carry_square(x3, x3l);
        fiat_25519_add(tmp0l, tmp0, z3);
        fiat_25519_carry_mul(z3, x1, z2);
        fiat_25519_carry_mul(z2, tmp1l, tmp0l);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert(z2, z2);
    fiat_25519_carry_mul(x2, x2, z2);
    fiat_25519_to_bytes(out, x2);

    memset(e, 0, sizeof e);
}

// The base point, u = 9 (RFC 7748 §4.1).
static const uint8_t x25519_basepoint[32] = {9};

// ---- the module entry points ----
//
// Each digest goes into a stack buffer before the output array is allocated,
// so no heap pointer is held across an allocation and no GCFrame is needed.
// The base64 and hex codecs are different: their outputs are too large for
// the stack, so they root the input across the allocation.

static Arr *bytes_result(const uint8_t *d, size_t n) {
    Arr *out = rt_arr_new((int64_t)n, 1);
    memcpy(rt_arr_bytes(out), d, n);
    return out;
}

Arr *rt_crypto_sha256(Arr *data) {
    uint8_t d[32];
    digest_of(ALG_SHA256, rt_arr_bytes(data), (size_t)data->len, d);
    return bytes_result(d, 32);
}

Arr *rt_crypto_sha384(Arr *data) {
    uint8_t d[48];
    digest_of(ALG_SHA384, rt_arr_bytes(data), (size_t)data->len, d);
    return bytes_result(d, 48);
}

Arr *rt_crypto_hmac_sha256(Arr *key, Arr *data) {
    uint8_t d[32];
    hmac_of(ALG_SHA256, rt_arr_bytes(key), (size_t)key->len,
            rt_arr_bytes(data), (size_t)data->len, d);
    return bytes_result(d, 32);
}

Arr *rt_crypto_hmac_sha384(Arr *key, Arr *data) {
    uint8_t d[48];
    hmac_of(ALG_SHA384, rt_arr_bytes(key), (size_t)key->len,
            rt_arr_bytes(data), (size_t)data->len, d);
    return bytes_result(d, 48);
}

// Extract is HMAC with the salt as the key (RFC 5869 §2.2). HMAC zero-pads its
// key, so an empty salt and the RFC default of "HashLen zeros" are the same.
Arr *rt_crypto_hkdf_extract_sha256(Arr *salt, Arr *ikm) {
    uint8_t d[32];
    hmac_of(ALG_SHA256, rt_arr_bytes(salt), (size_t)salt->len,
            rt_arr_bytes(ikm), (size_t)ikm->len, d);
    return bytes_result(d, 32);
}

Arr *rt_crypto_hkdf_extract_sha384(Arr *salt, Arr *ikm) {
    uint8_t d[48];
    hmac_of(ALG_SHA384, rt_arr_bytes(salt), (size_t)salt->len,
            rt_arr_bytes(ikm), (size_t)ikm->len, d);
    return bytes_result(d, 48);
}

static Arr *hkdf_expand_entry(int alg, Arr *prk, Arr *info, int64_t length) {
    size_t dlen = alg_digest[alg];
    if (length < 0 || (uint64_t)length > 255 * (uint64_t)dlen) {
        rt_panic("crypto.hkdfExpand: length out of range (0 to 255 * hash length)");
    }
    // 255 * 48 at most, so the full expansion fits on the stack and no input
    // is held across the output allocation.
    uint8_t okm[255 * 48];
    hkdf_expand_of(alg, rt_arr_bytes(prk), (size_t)prk->len,
                   rt_arr_bytes(info), (size_t)info->len, okm, (size_t)length);
    return bytes_result(okm, (size_t)length);
}

Arr *rt_crypto_hkdf_expand_sha256(Arr *prk, Arr *info, int64_t length) {
    return hkdf_expand_entry(ALG_SHA256, prk, info, length);
}

Arr *rt_crypto_hkdf_expand_sha384(Arr *prk, Arr *info, int64_t length) {
    return hkdf_expand_entry(ALG_SHA384, prk, info, length);
}

// ---- base64 (RFC 4648 §4: the standard alphabet, padding required) ----

static const char b64_alphabet[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

Str *rt_crypto_base64_encode(Arr *data) {
    // The input is read after the allocation, so it is rooted across it.
    TypeDesc *tds[1] = {&td_byte_array};
    int64_t slots[1] = {(int64_t)(intptr_t)data};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    int64_t n = data->len;
    int64_t groups = (n + 2) / 3;
    Str *out = rt_str_alloc(groups * 4);
    const uint8_t *p = rt_arr_bytes(data);
    char *q = out->data;
    int64_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
        *q++ = b64_alphabet[v >> 18];
        *q++ = b64_alphabet[(v >> 12) & 63];
        *q++ = b64_alphabet[(v >> 6) & 63];
        *q++ = b64_alphabet[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)p[i] << 16;
        int two = i + 1 < n;
        if (two) v |= (uint32_t)p[i + 1] << 8;
        *q++ = b64_alphabet[v >> 18];
        *q++ = b64_alphabet[(v >> 12) & 63];
        *q++ = two ? b64_alphabet[(v >> 6) & 63] : '=';
        *q++ = '=';
    }

    rt_gc_top = f.prev;
    return out;
}

static void *codec_error(const char *fn, const char *reason, int64_t at) {
    char msg[160];
    snprintf(msg, sizeof msg, "crypto.%s: %s at byte %lld", fn, reason, (long long)at);
    return rt_error_new(msg, NIO_ERR_INVALID);
}

// The decoder is strict: the standard alphabet only, correct padding, no
// whitespace and no newlines. A PEM reader removes its line breaks first.
Arr *rt_crypto_base64_decode(Str *s, void **err) {
    int64_t n = s->len;
    if (n % 4 != 0) {
        *err = codec_error("base64Decode", "length is not a multiple of 4", n);
        return NULL;
    }
    // Reverse table, built once: 0..63 is a value, 64 is padding, 255 is
    // invalid.
    static uint8_t rev[256];
    static int rev_ready = 0;
    if (!rev_ready) {
        memset(rev, 255, sizeof rev);
        for (int i = 0; i < 64; i++) rev[(uint8_t)b64_alphabet[i]] = (uint8_t)i;
        rev['='] = 64;
        rev_ready = 1;
    }
    int64_t pad = 0;
    if (n >= 1 && s->data[n - 1] == '=') pad++;
    if (n >= 2 && s->data[n - 2] == '=') pad++;
    int64_t outlen = (n / 4) * 3 - pad;

    TypeDesc *tds[1] = {&rt_td_string};
    int64_t slots[1] = {(int64_t)(intptr_t)s};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;
    Arr *out = rt_arr_new(outlen, 1);
    rt_gc_top = f.prev;

    const uint8_t *p = (const uint8_t *)s->data;
    uint8_t *q = rt_arr_bytes(out);
    for (int64_t i = 0; i < n; i += 4) {
        uint8_t a = rev[p[i]], b = rev[p[i + 1]], c = rev[p[i + 2]], d = rev[p[i + 3]];
        // Padding may appear only as the last one or two characters.
        int last = i + 4 == n;
        if (a >= 64 || b >= 64 || (c >= 64 && !(last && c == 64 && d == 64)) ||
            (d >= 64 && !(last && d == 64))) {
            int64_t at = i;
            if (a < 64) at = b >= 64 ? i + 1 : (c >= 64 ? i + 2 : i + 3);
            *err = codec_error("base64Decode", "invalid character", at);
            return NULL;
        }
        uint32_t v = (uint32_t)a << 18 | (uint32_t)b << 12;
        *q++ = (uint8_t)(v >> 16);
        if (c < 64) {
            v |= (uint32_t)c << 6;
            *q++ = (uint8_t)(v >> 8);
            if (d < 64) {
                v |= d;
                *q++ = (uint8_t)v;
            } else if ((v & 0xff) != 0) {
                // Bits below the padding must be zero. Otherwise one value has
                // two encodings (RFC 4648 §3.5).
                *err = codec_error("base64Decode", "non-zero padding bits", i + 2);
                return NULL;
            }
        } else if (((v >> 12) & 0x0f) != 0) {
            *err = codec_error("base64Decode", "non-zero padding bits", i + 1);
            return NULL;
        }
    }
    return out;
}

// ---- hex (RFC 4648 §8; lower-case out, either case in) ----

Str *rt_crypto_hex_encode(Arr *data) {
    TypeDesc *tds[1] = {&td_byte_array};
    int64_t slots[1] = {(int64_t)(intptr_t)data};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    static const char hexdig[16] = "0123456789abcdef";
    Str *out = rt_str_alloc(data->len * 2);
    const uint8_t *p = rt_arr_bytes(data);
    for (int64_t i = 0; i < data->len; i++) {
        out->data[2 * i] = hexdig[p[i] >> 4];
        out->data[2 * i + 1] = hexdig[p[i] & 15];
    }

    rt_gc_top = f.prev;
    return out;
}

static int hex_val(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Arr *rt_crypto_hex_decode(Str *s, void **err) {
    if (s->len % 2 != 0) {
        *err = codec_error("hexDecode", "odd length", s->len);
        return NULL;
    }
    TypeDesc *tds[1] = {&rt_td_string};
    int64_t slots[1] = {(int64_t)(intptr_t)s};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;
    Arr *out = rt_arr_new(s->len / 2, 1);
    rt_gc_top = f.prev;

    const uint8_t *p = (const uint8_t *)s->data;
    uint8_t *q = rt_arr_bytes(out);
    for (int64_t i = 0; i < s->len; i += 2) {
        int hi = hex_val(p[i]), lo = hex_val(p[i + 1]);
        if (hi < 0 || lo < 0) {
            *err = codec_error("hexDecode", "invalid character", hi < 0 ? i : i + 1);
            return NULL;
        }
        q[i / 2] = (uint8_t)(hi << 4 | lo);
    }
    return out;
}

// ---- randomBytes: the operating system's generator, and nothing else ----

// Fills buf from the OS CSPRNG, or panics. A program cannot recover when the
// entropy source does not answer. getentropy limits a request to 256 bytes.
static void os_random(uint8_t *buf, size_t n) {
#if defined(_WIN32)
    if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        rt_panic("crypto.randomBytes: the system random generator failed");
    }
#elif defined(__APPLE__)
    while (n > 0) {
        size_t take = n > 256 ? 256 : n;
        if (getentropy(buf, take) != 0) {
            rt_panic("crypto.randomBytes: the system random generator failed");
        }
        buf += take;
        n -= take;
    }
#else
    while (n > 0) {
        ssize_t got = getrandom(buf, n, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            rt_panic("crypto.randomBytes: the system random generator failed");
        }
        buf += (size_t)got;
        n -= (size_t)got;
    }
#endif
}

Arr *rt_crypto_random_bytes(int64_t n) {
    if (n < 0) rt_panic("crypto.randomBytes: negative length");
    Arr *out = rt_arr_new(n, 1);
    os_random(rt_arr_bytes(out), (size_t)n);
    return out;
}

// ---- AEAD and X25519 entry points ----
//
// A key, nonce or curve point of the wrong length is a bug in the calling
// program, so it panics. The ciphertext comes from outside, and its failure
// is an Error that the caller must handle.
static void need_len(const char *fn, const char *what, Arr *a, int64_t want) {
    if (a->len != want) {
        char msg[160];
        snprintf(msg, sizeof msg, "crypto.%s: %s must be %lld bytes, got %lld",
                 fn, what, (long long)want, (long long)a->len);
        rt_panic(msg);
    }
}

Arr *rt_crypto_chacha20poly1305_seal(Arr *key, Arr *nonce, Arr *plaintext, Arr *aad) {
    need_len("chacha20Poly1305Seal", "the key", key, AEAD_KEY_LEN);
    need_len("chacha20Poly1305Seal", "the nonce", nonce, AEAD_NONCE_LEN);

    // Every input is read after the allocation, so all four are rooted across
    // it.
    TypeDesc *tds[4] = {&td_byte_array, &td_byte_array, &td_byte_array, &td_byte_array};
    int64_t slots[4] = {(int64_t)(intptr_t)key, (int64_t)(intptr_t)nonce,
                        (int64_t)(intptr_t)plaintext, (int64_t)(intptr_t)aad};
    GCFrame f = {rt_gc_top, 4, tds, slots, NULL};
    rt_gc_top = &f;
    Arr *out = rt_arr_new(plaintext->len + AEAD_TAG_LEN, 1);
    rt_gc_top = f.prev;

    const uint8_t *k = rt_arr_bytes(key);
    const uint8_t *n = rt_arr_bytes(nonce);
    uint8_t *ct = rt_arr_bytes(out);
    // The payload is encrypted from block 1. Block 0 made the MAC key (§2.8).
    chacha20_xor(k, 1, n, rt_arr_bytes(plaintext), ct, (size_t)plaintext->len);
    aead_tag(k, n, rt_arr_bytes(aad), (size_t)aad->len, ct, (size_t)plaintext->len,
             ct + plaintext->len);
    return out;
}

Arr *rt_crypto_chacha20poly1305_open(Arr *key, Arr *nonce, Arr *ciphertext, Arr *aad,
                                     void **err) {
    need_len("chacha20Poly1305Open", "the key", key, AEAD_KEY_LEN);
    need_len("chacha20Poly1305Open", "the nonce", nonce, AEAD_NONCE_LEN);

    // A ciphertext too short for a tag reports as a failed tag. A decryption
    // oracle must not learn how the input was malformed.
    if (ciphertext->len < AEAD_TAG_LEN) {
        *err = rt_error_new("crypto.chacha20Poly1305Open: authentication failed",
                            NIO_ERR_AUTHENTICATION);
        return NULL;
    }
    int64_t ptlen = ciphertext->len - AEAD_TAG_LEN;
    const uint8_t *k = rt_arr_bytes(key);
    const uint8_t *n = rt_arr_bytes(nonce);
    const uint8_t *ct = rt_arr_bytes(ciphertext);

    // Verify before decrypting, and allocate nothing until verification passes.
    // A caller must never see plaintext that did not authenticate.
    uint8_t tag[AEAD_TAG_LEN];
    aead_tag(k, n, rt_arr_bytes(aad), (size_t)aad->len, ct, (size_t)ptlen, tag);
    if (!ct_equal(tag, ct + ptlen, AEAD_TAG_LEN)) {
        *err = rt_error_new("crypto.chacha20Poly1305Open: authentication failed",
                            NIO_ERR_AUTHENTICATION);
        return NULL;
    }

    TypeDesc *tds[4] = {&td_byte_array, &td_byte_array, &td_byte_array, &td_byte_array};
    int64_t slots[4] = {(int64_t)(intptr_t)key, (int64_t)(intptr_t)nonce,
                        (int64_t)(intptr_t)ciphertext, (int64_t)(intptr_t)aad};
    GCFrame f = {rt_gc_top, 4, tds, slots, NULL};
    rt_gc_top = &f;
    Arr *out = rt_arr_new(ptlen, 1);
    rt_gc_top = f.prev;

    chacha20_xor(rt_arr_bytes(key), 1, rt_arr_bytes(nonce), rt_arr_bytes(ciphertext),
                 rt_arr_bytes(out), (size_t)ptlen);
    return out;
}

Arr *rt_crypto_x25519_public(Arr *privateKey) {
    need_len("x25519PublicKey", "the private key", privateKey, 32);
    TypeDesc *tds[1] = {&td_byte_array};
    int64_t slots[1] = {(int64_t)(intptr_t)privateKey};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;
    Arr *out = rt_arr_new(32, 1);
    rt_gc_top = f.prev;

    // Clamping makes every scalar a nonzero multiple of the cofactor, and the
    // base point has large order. Thus this cannot produce the all-zero output
    // that the shared-secret path must reject.
    x25519_scalar_mult(rt_arr_bytes(out), rt_arr_bytes(privateKey), x25519_basepoint);
    return out;
}

Arr *rt_crypto_x25519_shared(Arr *privateKey, Arr *peerPublicKey, void **err) {
    need_len("x25519SharedSecret", "the private key", privateKey, 32);
    need_len("x25519SharedSecret", "the peer public key", peerPublicKey, 32);

    uint8_t shared[32];
    x25519_scalar_mult(shared, rt_arr_bytes(privateKey), rt_arr_bytes(peerPublicKey));

    // An all-zero result means that the peer sent a point of small order, so
    // the "shared" secret is a constant that the peer chose. RFC 8446 §7.4.2
    // makes the check mandatory for TLS 1.3, so this function is fallible.
    static const uint8_t zero[32] = {0};
    if (ct_equal(shared, zero, 32)) {
        memset(shared, 0, sizeof shared);
        *err = rt_error_new(
            "crypto.x25519SharedSecret: the peer's public key has small order",
            NIO_ERR_INVALID);
        return NULL;
    }

    TypeDesc *tds[2] = {&td_byte_array, &td_byte_array};
    int64_t slots[2] = {(int64_t)(intptr_t)privateKey, (int64_t)(intptr_t)peerPublicKey};
    GCFrame f = {rt_gc_top, 2, tds, slots, NULL};
    rt_gc_top = &f;
    Arr *out = rt_arr_new(32, 1);
    rt_gc_top = f.prev;

    memcpy(rt_arr_bytes(out), shared, 32);
    memset(shared, 0, sizeof shared);
    return out;
}

// ---- RSA signature verification (RFC 8017), on a Montgomery core ----
//
// Verification only. The modulus, exponent, digest and signature are all
// public, so this code runs in variable time over public values. There is no
// private-key operation here. The limb arithmetic is word-by-word Montgomery
// multiplication over 64-bit limbs.
//
// Both verifiers recover EM = sig^e mod n. PKCS#1 v1.5 (§8.2.2) encodes the
// expected block again and compares all of it, so it parses nothing out of
// bytes that an attacker controls. This prevents the Bleichenbacher '06
// forgery class. PSS (§9.1.2) is the recipe of the RFC with MGF1 and a salt as
// long as the hash, which the rsa_pss_rsae_* schemes of TLS 1.3 use.
//
// Malformed input gives INVALID. A signature that does not verify gives
// AUTHENTICATION. Callers and X.509 code act on the two differently.

#define RSA_MIN_BITS 1024
#define RSA_MAX_BITS 8192
#define RSA_MAX_LIMBS (RSA_MAX_BITS / 64)

// a < b ? -1 : a == b ? 0 : 1, over k little-endian limbs.
static int bm_cmp(const uint64_t *a, const uint64_t *b, int k) {
    for (int i = k - 1; i >= 0; i--) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

// a -= b over k limbs. Returns the final borrow (0 or 1).
static uint64_t bm_sub(uint64_t *a, const uint64_t *b, int k) {
    uint64_t borrow = 0;
    for (int i = 0; i < k; i++) {
        unsigned __int128 d = (unsigned __int128)a[i] - b[i] - borrow;
        a[i] = (uint64_t)d;
        borrow = (uint64_t)(d >> 64) & 1;
    }
    return borrow;
}

// x = 2x mod n. x < n before and after.
static void bm_double_mod(uint64_t *x, const uint64_t *n, int k) {
    uint64_t carry = 0;
    for (int i = 0; i < k; i++) {
        uint64_t v = x[i];
        x[i] = (v << 1) | carry;
        carry = v >> 63;
    }
    if (carry || bm_cmp(x, n, k) >= 0) bm_sub(x, n, k);
}

// -n[0]^-1 mod 2^64, by Newton iteration. An odd n0 is its own inverse mod 8,
// and each round doubles the number of correct bits.
static uint64_t bm_m0inv(uint64_t n0) {
    uint64_t inv = n0;
    for (int i = 0; i < 5; i++) inv *= 2 - n0 * inv;
    return (uint64_t)0 - inv;
}

// out = a * b * R^-1 mod n, R = 2^(64k): Montgomery multiplication by the
// standard CIOS loop. out can alias a or b. The work happens in t.
static void bm_mont_mul(uint64_t *out, const uint64_t *a, const uint64_t *b,
                        const uint64_t *n, uint64_t m0inv, int k) {
    uint64_t t[RSA_MAX_LIMBS + 2];
    memset(t, 0, (size_t)(k + 2) * sizeof(uint64_t));
    for (int i = 0; i < k; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < k; j++) {
            unsigned __int128 acc = (unsigned __int128)a[i] * b[j] + t[j] + carry;
            t[j] = (uint64_t)acc;
            carry = (uint64_t)(acc >> 64);
        }
        unsigned __int128 acc = (unsigned __int128)t[k] + carry;
        t[k] = (uint64_t)acc;
        t[k + 1] = (uint64_t)(acc >> 64);

        uint64_t m = t[0] * m0inv;
        acc = (unsigned __int128)m * n[0] + t[0];
        carry = (uint64_t)(acc >> 64);
        for (int j = 1; j < k; j++) {
            acc = (unsigned __int128)m * n[j] + t[j] + carry;
            t[j - 1] = (uint64_t)acc;
            carry = (uint64_t)(acc >> 64);
        }
        acc = (unsigned __int128)t[k] + carry;
        t[k - 1] = (uint64_t)acc;
        t[k] = t[k + 1] + (uint64_t)(acc >> 64);
        t[k + 1] = 0;
    }
    // t < 2n here, in k+1 limbs, so one conditional subtraction puts it below
    // n. The branch is on public data (see the section comment).
    if (t[k] || bm_cmp(t, n, k) >= 0) {
        uint64_t borrow = bm_sub(t, n, k);
        t[k] -= borrow;
    }
    memcpy(out, t, (size_t)k * sizeof(uint64_t));
}

// xR in, (x^e)R out. Left-to-right square-and-multiply from the top set bit
// of the exponent. e >= 1.
static void bm_mont_exp(uint64_t *out, const uint64_t *xR, uint64_t e,
                        const uint64_t *n, uint64_t m0inv, int k) {
    uint64_t acc[RSA_MAX_LIMBS];
    memcpy(acc, xR, (size_t)k * sizeof(uint64_t));
    int top = 63;
    while (top > 0 && ((e >> top) & 1) == 0) top--;
    for (int i = top - 1; i >= 0; i--) {
        bm_mont_mul(acc, acc, acc, n, m0inv, k);
        if ((e >> i) & 1) bm_mont_mul(acc, acc, xR, n, m0inv, k);
    }
    memcpy(out, acc, (size_t)k * sizeof(uint64_t));
}

// bm_mont_exp with the exponent as ek little-endian limbs, for the s^(n-2) of
// ECDSA. The exponent is also public here. e >= 1.
static void bm_mont_exp_limbs(uint64_t *out, const uint64_t *xR,
                              const uint64_t *e, int ek,
                              const uint64_t *n, uint64_t m0inv, int k) {
    uint64_t acc[RSA_MAX_LIMBS];
    memcpy(acc, xR, (size_t)k * sizeof(uint64_t));
    int top = 64 * ek - 1;
    while (top > 0 && ((e[top / 64] >> (top % 64)) & 1) == 0) top--;
    for (int i = top - 1; i >= 0; i--) {
        bm_mont_mul(acc, acc, acc, n, m0inv, k);
        if ((e[i / 64] >> (i % 64)) & 1) bm_mont_mul(acc, acc, xR, n, m0inv, k);
    }
    memcpy(out, acc, (size_t)k * sizeof(uint64_t));
}

static void *rsa_invalid(const char *fn, const char *reason) {
    char msg[192];
    snprintf(msg, sizeof msg, "crypto.%s: %s", fn, reason);
    return rt_error_new(msg, NIO_ERR_INVALID);
}

// Recovers EM = sig^e mod n into em, big-endian, one byte per modulus byte.
// Stores the byte length of the modulus in *out_klen and its bit length in
// *out_bits. Returns 0 and sets *err when the key or signature is malformed.
// em must hold RSA_MAX_BITS/8 bytes.
static int rsa_recover(const char *fn, Arr *modulus, int64_t exponent,
                       Arr *sig, uint8_t *em, int *out_klen, int *out_bits,
                       void **err) {
    const uint8_t *np = rt_arr_bytes(modulus);
    int64_t nlen = modulus->len;
    while (nlen > 0 && np[0] == 0) { np++; nlen--; }
    if (nlen == 0) {
        *err = rsa_invalid(fn, "the modulus is zero");
        return 0;
    }
    int bits = (int)(nlen * 8);
    for (uint8_t top = np[0]; (top & 0x80) == 0; top <<= 1) bits--;
    if (bits < RSA_MIN_BITS) {
        *err = rsa_invalid(fn, "the modulus is under 1024 bits");
        return 0;
    }
    if (bits > RSA_MAX_BITS) {
        *err = rsa_invalid(fn, "the modulus is over 8192 bits");
        return 0;
    }
    if ((np[nlen - 1] & 1) == 0) {
        *err = rsa_invalid(fn, "the modulus is even");
        return 0;
    }
    if (exponent < 3 || exponent > 0x7fffffff || (exponent & 1) == 0) {
        *err = rsa_invalid(fn, "the exponent must be odd and in 3..2^31-1");
        return 0;
    }
    if (sig->len != nlen) {
        *err = rsa_invalid(fn, "the signature length does not match the modulus");
        return 0;
    }

    int k = (int)((nlen + 7) / 8);
    uint64_t n[RSA_MAX_LIMBS], s[RSA_MAX_LIMBS];
    memset(n, 0, sizeof n);
    memset(s, 0, sizeof s);
    const uint8_t *sp = rt_arr_bytes(sig);
    for (int64_t i = 0; i < nlen; i++) {
        int64_t bit = (nlen - 1 - i) * 8;
        n[bit / 64] |= (uint64_t)np[i] << (bit % 64);
        s[bit / 64] |= (uint64_t)sp[i] << (bit % 64);
    }
    if (bm_cmp(s, n, k) >= 0) {
        *err = rsa_invalid(fn, "the signature is not below the modulus");
        return 0;
    }

    // R^2 mod n by doubling: 1 doubled 64k times is R mod n, and 64k more
    // doublings give R^2.
    uint64_t rr[RSA_MAX_LIMBS];
    memset(rr, 0, sizeof rr);
    rr[0] = 1;
    for (int i = 0; i < 2 * 64 * k; i++) bm_double_mod(rr, n, k);

    uint64_t m0inv = bm_m0inv(n[0]);
    uint64_t sR[RSA_MAX_LIMBS], m[RSA_MAX_LIMBS], one[RSA_MAX_LIMBS];
    bm_mont_mul(sR, s, rr, n, m0inv, k);            // s -> Montgomery domain
    bm_mont_exp(m, sR, (uint64_t)exponent, n, m0inv, k);
    memset(one, 0, sizeof one);
    one[0] = 1;
    bm_mont_mul(m, m, one, n, m0inv, k);            // and back out

    for (int64_t i = 0; i < nlen; i++) {
        int64_t bit = (nlen - 1 - i) * 8;
        em[i] = (uint8_t)(m[bit / 64] >> (bit % 64));
    }
    *out_klen = (int)nlen;
    *out_bits = bits;
    return 1;
}

// The DER DigestInfo prefixes of RFC 8017 §9.2, note 1.
static const uint8_t rsa_di_sha256[] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
    0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20,
};
static const uint8_t rsa_di_sha384[] = {
    0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
    0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30,
};

// Resolves the hash name. Any name other than "sha256" and "sha384" is
// INVALID, which includes sha1 and md5.
static int rsa_pick_hash(const char *fn, Str *hash, Arr *digest, int *hlen,
                         const uint8_t **di, int *dilen, void **err) {
    if (hash->len == 6 && memcmp(hash->data, "sha256", 6) == 0) {
        *hlen = 32;
        *di = rsa_di_sha256;
        *dilen = (int)sizeof rsa_di_sha256;
    } else if (hash->len == 6 && memcmp(hash->data, "sha384", 6) == 0) {
        *hlen = 48;
        *di = rsa_di_sha384;
        *dilen = (int)sizeof rsa_di_sha384;
    } else {
        *err = rsa_invalid(fn, "the hash must be \"sha256\" or \"sha384\"");
        return 0;
    }
    if (digest->len != *hlen) {
        *err = rsa_invalid(fn, "the digest length does not match the hash");
        return 0;
    }
    return 1;
}

void rt_crypto_rsa_verify_pkcs1v15(Arr *modulus, int64_t exponent, Str *hash,
                                   Arr *digest, Arr *sig, void **err) {
    int hlen, dilen;
    const uint8_t *di;
    if (!rsa_pick_hash("rsaVerifyPkcs1v15", hash, digest, &hlen, &di, &dilen, err)) return;

    uint8_t em[RSA_MAX_BITS / 8];
    int klen, bits;
    if (!rsa_recover("rsaVerifyPkcs1v15", modulus, exponent, sig, em, &klen, &bits, err)) return;

    // Encode the expected block, 00 01 FF..FF 00 DigestInfo digest, and compare
    // all of it. §8.2.2 requires at least eight FF bytes.
    int tlen = dilen + hlen;
    if (klen < tlen + 11) {
        *err = rsa_invalid("rsaVerifyPkcs1v15", "the modulus is too short for this hash");
        return;
    }
    uint8_t want[RSA_MAX_BITS / 8];
    want[0] = 0x00;
    want[1] = 0x01;
    memset(want + 2, 0xff, (size_t)(klen - tlen - 3));
    want[klen - tlen - 1] = 0x00;
    memcpy(want + klen - tlen, di, (size_t)dilen);
    memcpy(want + klen - hlen, rt_arr_bytes(digest), (size_t)hlen);

    if (!ct_equal(em, want, (size_t)klen)) {
        *err = rt_error_new("crypto.rsaVerifyPkcs1v15: the signature does not verify",
                            NIO_ERR_AUTHENTICATION);
    }
}

// MGF1 (RFC 8017 §B.2.1): XORs the mask into db in place. Thus there is no
// second buffer that can get out of step with the first.
static void rsa_mgf1_xor(uint8_t *db, int dblen, const uint8_t *seed, int seedlen,
                         int hlen) {
    uint8_t block[48];
    for (int counter = 0, done = 0; done < dblen; counter++) {
        uint8_t cnt[4] = {
            (uint8_t)(counter >> 24), (uint8_t)(counter >> 16),
            (uint8_t)(counter >> 8), (uint8_t)counter,
        };
        if (hlen == 32) {
            Sha256 s;
            sha256_init(&s);
            sha256_update(&s, seed, (size_t)seedlen);
            sha256_update(&s, cnt, 4);
            sha256_final(&s, block);
        } else {
            Sha512 s;
            sha384_init(&s);
            sha512_update(&s, seed, (size_t)seedlen);
            sha512_update(&s, cnt, 4);
            sha384_final(&s, block);
        }
        for (int i = 0; i < hlen && done < dblen; i++, done++) {
            db[done] ^= block[i];
        }
    }
}

void rt_crypto_rsa_verify_pss(Arr *modulus, int64_t exponent, Str *hash,
                              Arr *digest, Arr *sig, void **err) {
    int hlen, dilen;
    const uint8_t *di;
    if (!rsa_pick_hash("rsaVerifyPss", hash, digest, &hlen, &di, &dilen, err)) return;

    uint8_t raw[RSA_MAX_BITS / 8];
    int klen, bits;
    if (!rsa_recover("rsaVerifyPss", modulus, exponent, sig, raw, &klen, &bits, err)) return;

    // EMSA-PSS-VERIFY (§9.1.2), with a salt length equal to the hash length.
    // The encoded message is emLen = ceil((modBits-1)/8) bytes, and every
    // recovered byte above it must be zero. Every failure below gives
    // AUTHENTICATION, so no failure tells which check failed.
    int emBits = bits - 1;
    int emLen = (emBits + 7) / 8;
    uint8_t *em = raw + (klen - emLen);
    int bad = 0;
    for (int i = 0; i < klen - emLen; i++) bad |= raw[i];
    int slen = hlen;
    if (emLen < hlen + slen + 2) {
        *err = rsa_invalid("rsaVerifyPss", "the modulus is too short for this hash");
        return;
    }
    bad |= em[emLen - 1] != 0xbc;

    int dblen = emLen - hlen - 1;
    uint8_t *maskedDB = em;
    uint8_t *H = em + dblen;
    int topbits = 8 * emLen - emBits;   // 0..7 bits that must be zero
    if (topbits != 0) bad |= maskedDB[0] >> (8 - topbits);

    rsa_mgf1_xor(maskedDB, dblen, H, hlen, hlen);   // maskedDB is DB now
    if (topbits != 0) maskedDB[0] &= (uint8_t)(0xff >> topbits);

    for (int i = 0; i < dblen - slen - 1; i++) bad |= maskedDB[i];
    bad |= maskedDB[dblen - slen - 1] != 0x01;

    // H' = Hash(00 x8 || mHash || salt)
    static const uint8_t eight_zeros[8] = {0};
    uint8_t hprime[48];
    if (hlen == 32) {
        Sha256 s;
        sha256_init(&s);
        sha256_update(&s, eight_zeros, 8);
        sha256_update(&s, rt_arr_bytes(digest), (size_t)hlen);
        sha256_update(&s, maskedDB + dblen - slen, (size_t)slen);
        sha256_final(&s, hprime);
    } else {
        Sha512 s;
        sha384_init(&s);
        sha512_update(&s, eight_zeros, 8);
        sha512_update(&s, rt_arr_bytes(digest), (size_t)hlen);
        sha512_update(&s, maskedDB + dblen - slen, (size_t)slen);
        sha384_final(&s, hprime);
    }
    bad |= !ct_equal(H, hprime, (size_t)hlen);

    if (bad) {
        *err = rt_error_new("crypto.rsaVerifyPss: the signature does not verify",
                            NIO_ERR_AUTHENTICATION);
    }
}

// ---- ECDSA verification (FIPS 186-5 §6.4.2), P-256 and P-384 ----
//
// Verification only, as for RSA above. The public key, digest and signature
// are all public, so this code runs in variable time over public values. It
// has three layers, each copied from a source and not invented:
//
//   - The field arithmetic is the generated, formally verified Montgomery code
//     of fiat-crypto, vendored unchanged: fiat_p256_64.h is byte-for-byte
//     fiat-c/src/p256_64.c (SHA-256
//     68cc5c4fa08de660a869a412618a30848f11d18051245013fe53fa4e313ec701) and
//     fiat_p384_64.h is fiat-c/src/p384_64.c (SHA-256
//     d3cf74220c7b4c2e33e8e225ac91abcf54616140379b3f528ae5f380c4aa5698), with
//     only a rename. Never edit them. MIT/Apache-2.0/BSD-1-Clause.
//   - The group arithmetic uses the complete addition and doubling formulas
//     for a = -3 of Renes-Costello-Batina (https://eprint.iacr.org/2015/1060,
//     §A.2), and a 4-bit-window scalar multiplication over a 15-entry table. One formula applies to
//     every input pair, so there are no special cases. The table is indexed
//     directly, which is safe because the scalars are public.
//   - The scalar arithmetic mod the group order uses the Montgomery core above,
//     with s^-1 computed as s^(n-2) by Fermat.
//
// Both group orders are a whole number of 64-bit limbs wide, so the digest
// truncation below is byte truncation. A key or signature that cannot be used
// gives INVALID. A well-formed signature that fails the math gives
// AUTHENTICATION.

#include "fiat_p256_64.h"
#include "fiat_p384_64.h"

#define EC_MAX_LIMBS 6                 // P-384
#define EC_MAX_BYTES (EC_MAX_LIMBS * 8)

// One curve, as data. The function pointers are named wrappers, because the
// array-typed parameters of fiat decay to uint64_t*. to_bytes and from_bytes
// use the little-endian, non-Montgomery form of fiat. Everything above this
// struct uses SEC 1 big-endian and converts at the boundary.
typedef struct {
    const char *name;
    int limbs;                     // field element limbs: 4 or 6
    int nbytes;                    // field element / scalar bytes: 32 or 48
    void (*mul)(uint64_t *, const uint64_t *, const uint64_t *);
    void (*sqr)(uint64_t *, const uint64_t *);
    void (*add)(uint64_t *, const uint64_t *, const uint64_t *);
    void (*sub)(uint64_t *, const uint64_t *, const uint64_t *);
    void (*to_mont)(uint64_t *, const uint64_t *);
    void (*from_mont)(uint64_t *, const uint64_t *);
    void (*to_bytes_le)(uint8_t *, const uint64_t *);
    void (*from_bytes_le)(uint64_t *, const uint8_t *);
    void (*one)(uint64_t *);
    void (*nonzero)(uint64_t *, const uint64_t *);
    const uint8_t *p;              // field prime, big-endian nbytes
    const uint8_t *b;              // curve b, big-endian nbytes
    const uint8_t *gx, *gy;        // generator, big-endian nbytes
    const uint8_t *n;              // group order, big-endian nbytes
} EcCurve;

static void ec_p256_mul(uint64_t *o, const uint64_t *a, const uint64_t *b) { fiat_p256_mul(o, a, b); }
static void ec_p256_sqr(uint64_t *o, const uint64_t *a) { fiat_p256_square(o, a); }
static void ec_p256_add(uint64_t *o, const uint64_t *a, const uint64_t *b) { fiat_p256_add(o, a, b); }
static void ec_p256_sub(uint64_t *o, const uint64_t *a, const uint64_t *b) { fiat_p256_sub(o, a, b); }
static void ec_p256_to_mont(uint64_t *o, const uint64_t *a) { fiat_p256_to_montgomery(o, a); }
static void ec_p256_from_mont(uint64_t *o, const uint64_t *a) { fiat_p256_from_montgomery(o, a); }
static void ec_p256_to_bytes(uint8_t *o, const uint64_t *a) { fiat_p256_to_bytes(o, a); }
static void ec_p256_from_bytes(uint64_t *o, const uint8_t *a) { fiat_p256_from_bytes(o, a); }
static void ec_p256_one(uint64_t *o) { fiat_p256_set_one(o); }
static void ec_p256_nonzero(uint64_t *o, const uint64_t *a) { fiat_p256_nonzero(o, a); }

static void ec_p384_mul(uint64_t *o, const uint64_t *a, const uint64_t *b) { fiat_p384_mul(o, a, b); }
static void ec_p384_sqr(uint64_t *o, const uint64_t *a) { fiat_p384_square(o, a); }
static void ec_p384_add(uint64_t *o, const uint64_t *a, const uint64_t *b) { fiat_p384_add(o, a, b); }
static void ec_p384_sub(uint64_t *o, const uint64_t *a, const uint64_t *b) { fiat_p384_sub(o, a, b); }
static void ec_p384_to_mont(uint64_t *o, const uint64_t *a) { fiat_p384_to_montgomery(o, a); }
static void ec_p384_from_mont(uint64_t *o, const uint64_t *a) { fiat_p384_from_montgomery(o, a); }
static void ec_p384_to_bytes(uint8_t *o, const uint64_t *a) { fiat_p384_to_bytes(o, a); }
static void ec_p384_from_bytes(uint64_t *o, const uint8_t *a) { fiat_p384_from_bytes(o, a); }
static void ec_p384_one(uint64_t *o) { fiat_p384_set_one(o); }
static void ec_p384_nonzero(uint64_t *o, const uint64_t *a) { fiat_p384_nonzero(o, a); }

// SEC 2's curve constants, big-endian.
static const uint8_t ec_p256_p[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
static const uint8_t ec_p256_b[32] = {
    0x5a,0xc6,0x35,0xd8,0xaa,0x3a,0x93,0xe7,0xb3,0xeb,0xbd,0x55,0x76,0x98,0x86,0xbc,
    0x65,0x1d,0x06,0xb0,0xcc,0x53,0xb0,0xf6,0x3b,0xce,0x3c,0x3e,0x27,0xd2,0x60,0x4b};
static const uint8_t ec_p256_gx[32] = {
    0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,
    0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96};
static const uint8_t ec_p256_gy[32] = {
    0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,
    0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5};
static const uint8_t ec_p256_n[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51};

static const uint8_t ec_p384_p[48] = {
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff};
static const uint8_t ec_p384_b[48] = {
    0xb3,0x31,0x2f,0xa7,0xe2,0x3e,0xe7,0xe4,0x98,0x8e,0x05,0x6b,0xe3,0xf8,0x2d,0x19,
    0x18,0x1d,0x9c,0x6e,0xfe,0x81,0x41,0x12,0x03,0x14,0x08,0x8f,0x50,0x13,0x87,0x5a,
    0xc6,0x56,0x39,0x8d,0x8a,0x2e,0xd1,0x9d,0x2a,0x85,0xc8,0xed,0xd3,0xec,0x2a,0xef};
static const uint8_t ec_p384_gx[48] = {
    0xaa,0x87,0xca,0x22,0xbe,0x8b,0x05,0x37,0x8e,0xb1,0xc7,0x1e,0xf3,0x20,0xad,0x74,
    0x6e,0x1d,0x3b,0x62,0x8b,0xa7,0x9b,0x98,0x59,0xf7,0x41,0xe0,0x82,0x54,0x2a,0x38,
    0x55,0x02,0xf2,0x5d,0xbf,0x55,0x29,0x6c,0x3a,0x54,0x5e,0x38,0x72,0x76,0x0a,0xb7};
static const uint8_t ec_p384_gy[48] = {
    0x36,0x17,0xde,0x4a,0x96,0x26,0x2c,0x6f,0x5d,0x9e,0x98,0xbf,0x92,0x92,0xdc,0x29,
    0xf8,0xf4,0x1d,0xbd,0x28,0x9a,0x14,0x7c,0xe9,0xda,0x31,0x13,0xb5,0xf0,0xb8,0xc0,
    0x0a,0x60,0xb1,0xce,0x1d,0x7e,0x81,0x9d,0x7a,0x43,0x1d,0x7c,0x90,0xea,0x0e,0x5f};
static const uint8_t ec_p384_n[48] = {
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xc7,0x63,0x4d,0x81,0xf4,0x37,0x2d,0xdf,
    0x58,0x1a,0x0d,0xb2,0x48,0xb0,0xa7,0x7a,0xec,0xec,0x19,0x6a,0xcc,0xc5,0x29,0x73};

static const EcCurve ec_p256 = {
    "p256", 4, 32,
    ec_p256_mul, ec_p256_sqr, ec_p256_add, ec_p256_sub,
    ec_p256_to_mont, ec_p256_from_mont, ec_p256_to_bytes, ec_p256_from_bytes,
    ec_p256_one, ec_p256_nonzero,
    ec_p256_p, ec_p256_b, ec_p256_gx, ec_p256_gy, ec_p256_n};

static const EcCurve ec_p384 = {
    "p384", 6, 48,
    ec_p384_mul, ec_p384_sqr, ec_p384_add, ec_p384_sub,
    ec_p384_to_mont, ec_p384_from_mont, ec_p384_to_bytes, ec_p384_from_bytes,
    ec_p384_one, ec_p384_nonzero,
    ec_p384_p, ec_p384_b, ec_p384_gx, ec_p384_gy, ec_p384_n};

// Loads a big-endian field element, and returns 0 for a value >= p. The
// from_bytes of fiat makes that check the job of the caller.
static int ec_fe_from_be(const EcCurve *c, uint64_t *out, const uint8_t *be) {
    if (memcmp(be, c->p, (size_t)c->nbytes) >= 0) return 0;
    uint8_t le[EC_MAX_BYTES];
    for (int i = 0; i < c->nbytes; i++) le[i] = be[c->nbytes - 1 - i];
    uint64_t plain[EC_MAX_LIMBS];
    c->from_bytes_le(plain, le);
    c->to_mont(out, plain);
    return 1;
}

// Returns the canonical big-endian bytes of a Montgomery-form element.
static void ec_fe_to_be(const EcCurve *c, uint8_t *be, const uint64_t *a) {
    uint64_t plain[EC_MAX_LIMBS];
    uint8_t le[EC_MAX_BYTES];
    c->from_mont(plain, a);
    c->to_bytes_le(le, plain);
    for (int i = 0; i < c->nbytes; i++) be[i] = le[c->nbytes - 1 - i];
}

static int ec_fe_is_zero(const EcCurve *c, const uint64_t *a) {
    uint64_t plain[EC_MAX_LIMBS], nz;
    c->from_mont(plain, a);
    c->nonzero(&nz, plain);
    return nz == 0;
}

static int ec_fe_equal(const EcCurve *c, const uint64_t *a, const uint64_t *b) {
    uint64_t d[EC_MAX_LIMBS], nz;
    c->sub(d, a, b);
    c->nonzero(&nz, d);
    return nz == 0;
}

// out = base^e, e as big-endian bytes, square-and-multiply from the top set
// bit. The z^(p-2) of the affine conversion uses it. The exponent is a
// constant.
static void ec_fe_exp(const EcCurve *c, uint64_t *out, const uint64_t *base,
                      const uint8_t *e) {
    int bits = c->nbytes * 8;
    int top = bits - 1;
    while (top > 0 && ((e[c->nbytes - 1 - (top / 8)] >> (top % 8)) & 1) == 0) top--;
    uint64_t acc[EC_MAX_LIMBS];
    memcpy(acc, base, (size_t)c->limbs * sizeof(uint64_t));
    for (int i = top - 1; i >= 0; i--) {
        c->sqr(acc, acc);
        if ((e[c->nbytes - 1 - (i / 8)] >> (i % 8)) & 1) c->mul(acc, acc, base);
    }
    memcpy(out, acc, (size_t)c->limbs * sizeof(uint64_t));
}

// A point in projective coordinates (X:Y:Z), x = X/Z, y = Y/Z, all in the
// Montgomery domain. Z = 0 is the point at infinity, canonically (0 : 1 : 0).
typedef struct {
    uint64_t x[EC_MAX_LIMBS], y[EC_MAX_LIMBS], z[EC_MAX_LIMBS];
} EcPoint;

static void ec_point_infinity(const EcCurve *c, EcPoint *p) {
    memset(p, 0, sizeof *p);
    c->one(p->y);
}

// q = p1 + p2, any inputs, aliasing allowed. The complete addition formula for
// a = -3 of Renes-Costello-Batina §A.2. Each line carries the comment of the
// reference, so a reader can check the transcription against it.
static void ec_point_add(const EcCurve *c, EcPoint *q,
                         const EcPoint *p1, const EcPoint *p2,
                         const uint64_t *b) {
    uint64_t t0[EC_MAX_LIMBS], t1[EC_MAX_LIMBS], t2[EC_MAX_LIMBS];
    uint64_t t3[EC_MAX_LIMBS], t4[EC_MAX_LIMBS];
    uint64_t x3[EC_MAX_LIMBS], y3[EC_MAX_LIMBS], z3[EC_MAX_LIMBS];

    c->mul(t0, p1->x, p2->x);    // t0 := X1 * X2
    c->mul(t1, p1->y, p2->y);    // t1 := Y1 * Y2
    c->mul(t2, p1->z, p2->z);    // t2 := Z1 * Z2
    c->add(t3, p1->x, p1->y);    // t3 := X1 + Y1
    c->add(t4, p2->x, p2->y);    // t4 := X2 + Y2
    c->mul(t3, t3, t4);          // t3 := t3 * t4
    c->add(t4, t0, t1);          // t4 := t0 + t1
    c->sub(t3, t3, t4);          // t3 := t3 - t4
    c->add(t4, p1->y, p1->z);    // t4 := Y1 + Z1
    c->add(x3, p2->y, p2->z);    // X3 := Y2 + Z2
    c->mul(t4, t4, x3);          // t4 := t4 * X3
    c->add(x3, t1, t2);          // X3 := t1 + t2
    c->sub(t4, t4, x3);          // t4 := t4 - X3
    c->add(x3, p1->x, p1->z);    // X3 := X1 + Z1
    c->add(y3, p2->x, p2->z);    // Y3 := X2 + Z2
    c->mul(x3, x3, y3);          // X3 := X3 * Y3
    c->add(y3, t0, t2);          // Y3 := t0 + t2
    c->sub(y3, x3, y3);          // Y3 := X3 - Y3
    c->mul(z3, b, t2);           // Z3 := b * t2
    c->sub(x3, y3, z3);          // X3 := Y3 - Z3
    c->add(z3, x3, x3);          // Z3 := X3 + X3
    c->add(x3, x3, z3);          // X3 := X3 + Z3
    c->sub(z3, t1, x3);          // Z3 := t1 - X3
    c->add(x3, t1, x3);          // X3 := t1 + X3
    c->mul(y3, b, y3);           // Y3 := b * Y3
    c->add(t1, t2, t2);          // t1 := t2 + t2
    c->add(t2, t1, t2);          // t2 := t1 + t2
    c->sub(y3, y3, t2);          // Y3 := Y3 - t2
    c->sub(y3, y3, t0);          // Y3 := Y3 - t0
    c->add(t1, y3, y3);          // t1 := Y3 + Y3
    c->add(y3, t1, y3);          // Y3 := t1 + Y3
    c->add(t1, t0, t0);          // t1 := t0 + t0
    c->add(t0, t1, t0);          // t0 := t1 + t0
    c->sub(t0, t0, t2);          // t0 := t0 - t2
    c->mul(t1, t4, y3);          // t1 := t4 * Y3
    c->mul(t2, t0, y3);          // t2 := t0 * Y3
    c->mul(y3, x3, z3);          // Y3 := X3 * Z3
    c->add(y3, y3, t2);          // Y3 := Y3 + t2
    c->mul(x3, t3, x3);          // X3 := t3 * X3
    c->sub(x3, x3, t1);          // X3 := X3 - t1
    c->mul(z3, t4, z3);          // Z3 := t4 * Z3
    c->mul(t1, t3, t0);          // t1 := t3 * t0
    c->add(z3, z3, t1);          // Z3 := Z3 + t1

    memcpy(q->x, x3, (size_t)c->limbs * sizeof(uint64_t));
    memcpy(q->y, y3, (size_t)c->limbs * sizeof(uint64_t));
    memcpy(q->z, z3, (size_t)c->limbs * sizeof(uint64_t));
}

// q = p + p, aliasing allowed. The exception-free doubling from the same
// source.
static void ec_point_double(const EcCurve *c, EcPoint *q, const EcPoint *p,
                            const uint64_t *b) {
    uint64_t t0[EC_MAX_LIMBS], t1[EC_MAX_LIMBS], t2[EC_MAX_LIMBS];
    uint64_t t3[EC_MAX_LIMBS];
    uint64_t x3[EC_MAX_LIMBS], y3[EC_MAX_LIMBS], z3[EC_MAX_LIMBS];

    c->sqr(t0, p->x);            // t0 := X ^ 2
    c->sqr(t1, p->y);            // t1 := Y ^ 2
    c->sqr(t2, p->z);            // t2 := Z ^ 2
    c->mul(t3, p->x, p->y);      // t3 := X * Y
    c->add(t3, t3, t3);          // t3 := t3 + t3
    c->mul(z3, p->x, p->z);      // Z3 := X * Z
    c->add(z3, z3, z3);          // Z3 := Z3 + Z3
    c->mul(y3, b, t2);           // Y3 := b * t2
    c->sub(y3, y3, z3);          // Y3 := Y3 - Z3
    c->add(x3, y3, y3);          // X3 := Y3 + Y3
    c->add(y3, x3, y3);          // Y3 := X3 + Y3
    c->sub(x3, t1, y3);          // X3 := t1 - Y3
    c->add(y3, t1, y3);          // Y3 := t1 + Y3
    c->mul(y3, x3, y3);          // Y3 := X3 * Y3
    c->mul(x3, x3, t3);          // X3 := X3 * t3
    c->add(t3, t2, t2);          // t3 := t2 + t2
    c->add(t2, t2, t3);          // t2 := t2 + t3
    c->mul(z3, b, z3);           // Z3 := b * Z3
    c->sub(z3, z3, t2);          // Z3 := Z3 - t2
    c->sub(z3, z3, t0);          // Z3 := Z3 - t0
    c->add(t3, z3, z3);          // t3 := Z3 + Z3
    c->add(z3, z3, t3);          // Z3 := Z3 + t3
    c->add(t3, t0, t0);          // t3 := t0 + t0
    c->add(t0, t3, t0);          // t0 := t3 + t0
    c->sub(t0, t0, t2);          // t0 := t0 - t2
    c->mul(t0, t0, z3);          // t0 := t0 * Z3
    c->add(y3, y3, t0);          // Y3 := Y3 + t0
    c->mul(t0, p->y, p->z);      // t0 := Y * Z
    c->add(t0, t0, t0);          // t0 := t0 + t0
    c->mul(z3, t0, z3);          // Z3 := t0 * Z3
    c->sub(x3, x3, z3);          // X3 := X3 - Z3
    c->mul(z3, t0, t1);          // Z3 := t0 * t1
    c->add(z3, z3, z3);          // Z3 := Z3 + Z3
    c->add(z3, z3, z3);          // Z3 := Z3 + Z3

    memcpy(q->x, x3, (size_t)c->limbs * sizeof(uint64_t));
    memcpy(q->y, y3, (size_t)c->limbs * sizeof(uint64_t));
    memcpy(q->z, z3, (size_t)c->limbs * sizeof(uint64_t));
}

// p = scalar * q, with the scalar as nbytes big-endian bytes: a table of the
// first 15 multiples, then four doublings and one table addition per nibble.
// The table is indexed directly, as the section comment states.
static void ec_scalar_mult(const EcCurve *c, EcPoint *p, const EcPoint *q,
                           const uint8_t *scalar, const uint64_t *b) {
    EcPoint table[15];
    table[0] = *q;
    for (int i = 1; i < 15; i += 2) {
        ec_point_double(c, &table[i], &table[i / 2], b);
        ec_point_add(c, &table[i + 1], &table[i], q, b);
    }

    EcPoint acc;
    ec_point_infinity(c, &acc);
    for (int i = 0; i < c->nbytes; i++) {
        if (i != 0) {
            for (int d = 0; d < 4; d++) ec_point_double(c, &acc, &acc, b);
        }
        int window = scalar[i] >> 4;
        if (window != 0) ec_point_add(c, &acc, &acc, &table[window - 1], b);
        for (int d = 0; d < 4; d++) ec_point_double(c, &acc, &acc, b);
        window = scalar[i] & 0x0f;
        if (window != 0) ec_point_add(c, &acc, &acc, &table[window - 1], b);
    }
    *p = acc;
}

// y^2 == x^3 - 3x + b.
static int ec_on_curve(const EcCurve *c, const uint64_t *x, const uint64_t *y,
                       const uint64_t *b) {
    uint64_t rhs[EC_MAX_LIMBS], threex[EC_MAX_LIMBS], lhs[EC_MAX_LIMBS];
    c->sqr(rhs, x);
    c->mul(rhs, rhs, x);
    c->add(threex, x, x);
    c->add(threex, threex, x);
    c->sub(rhs, rhs, threex);
    c->add(rhs, rhs, b);
    c->sqr(lhs, y);
    return ec_fe_equal(c, lhs, rhs);
}

static void *ec_invalid(const char *reason) {
    char msg[192];
    snprintf(msg, sizeof msg, "crypto.ecdsaVerify: %s", reason);
    return rt_error_new(msg, NIO_ERR_INVALID);
}

// Loads blen big-endian bytes into k little-endian limbs. blen <= 8k.
static void bm_from_be(uint64_t *out, const uint8_t *be, int64_t blen, int k) {
    memset(out, 0, (size_t)k * sizeof(uint64_t));
    for (int64_t i = 0; i < blen; i++) {
        int64_t bit = (blen - 1 - i) * 8;
        out[bit / 64] |= (uint64_t)be[i] << (bit % 64);
    }
}

// Writes k limbs as 8k big-endian bytes.
static void bm_to_be(uint8_t *be, const uint64_t *a, int k) {
    for (int i = 0; i < 8 * k; i++) {
        int bit = (8 * k - 1 - i) * 8;
        be[i] = (uint8_t)(a[bit / 64] >> (bit % 64));
    }
}

static int bm_is_zero(const uint64_t *a, int k) {
    for (int i = 0; i < k; i++) {
        if (a[i]) return 0;
    }
    return 1;
}

// FIPS 186-5 §6.4.2. publicKey is the SEC 1 uncompressed point (0x04 || X ||
// Y), the only form that certificates carry. digest is the message hash, and
// this function truncates it to the width of the order. r and s are the two
// integers of the signature as minimal big-endian bytes. The caller has
// already removed the DER, because parsing is done in Nio.
void rt_crypto_ecdsa_verify(Str *curve, Arr *publicKey, Arr *digest,
                            Arr *r, Arr *s, void **err) {
    const EcCurve *c = NULL;
    if (curve->len == 4 && memcmp(curve->data, "p256", 4) == 0) c = &ec_p256;
    if (curve->len == 4 && memcmp(curve->data, "p384", 4) == 0) c = &ec_p384;
    if (c == NULL) {
        *err = ec_invalid("the curve must be \"p256\" or \"p384\"");
        return;
    }
    int k = c->nbytes / 8;      // scalar limbs

    const uint8_t *pk = rt_arr_bytes(publicKey);
    if (publicKey->len != 1 + 2 * c->nbytes || pk[0] != 0x04) {
        *err = ec_invalid("the public key is not an uncompressed SEC 1 point");
        return;
    }
    uint64_t b[EC_MAX_LIMBS];
    if (!ec_fe_from_be(c, b, c->b)) {
        *err = ec_invalid("internal: curve constant out of range");
        return;
    }
    EcPoint Q;
    memset(&Q, 0, sizeof Q);
    if (!ec_fe_from_be(c, Q.x, pk + 1) ||
        !ec_fe_from_be(c, Q.y, pk + 1 + c->nbytes)) {
        *err = ec_invalid("the public key's coordinates are not below the field prime");
        return;
    }
    if (!ec_on_curve(c, Q.x, Q.y, b)) {
        *err = ec_invalid("the public key is not on the curve");
        return;
    }
    c->one(Q.z);

    uint64_t n[EC_MAX_LIMBS], rv[EC_MAX_LIMBS], sv[EC_MAX_LIMBS];
    bm_from_be(n, c->n, c->nbytes, k);
    if (r->len < 1 || r->len > c->nbytes || s->len < 1 || s->len > c->nbytes) {
        *err = ec_invalid("the signature's integers do not fit the curve");
        return;
    }
    bm_from_be(rv, rt_arr_bytes(r), r->len, k);
    bm_from_be(sv, rt_arr_bytes(s), s->len, k);
    if (bm_is_zero(rv, k) || bm_cmp(rv, n, k) >= 0 ||
        bm_is_zero(sv, k) || bm_cmp(sv, n, k) >= 0) {
        *err = ec_invalid("the signature's integers are not in [1, n-1]");
        return;
    }

    // e: the left-most order-width bytes of the digest. The value is below 2n,
    // so one conditional subtraction reduces it.
    uint64_t e[EC_MAX_LIMBS];
    int64_t dlen = digest->len;
    if (dlen > c->nbytes) dlen = c->nbytes;
    bm_from_be(e, rt_arr_bytes(digest), dlen, k);
    if (bm_cmp(e, n, k) >= 0) bm_sub(e, n, k);

    // Scalar arithmetic mod n: w = s^-1 as s^(n-2), u1 = e*w, u2 = r*w.
    uint64_t m0inv = bm_m0inv(n[0]);
    uint64_t rr[EC_MAX_LIMBS];
    memset(rr, 0, sizeof rr);
    rr[0] = 1;
    for (int i = 0; i < 2 * 64 * k; i++) bm_double_mod(rr, n, k);

    uint64_t nm2[EC_MAX_LIMBS];
    memcpy(nm2, n, (size_t)k * sizeof(uint64_t));
    nm2[0] -= 2;                                    // n is odd, so no borrow

    uint64_t sR[EC_MAX_LIMBS], wR[EC_MAX_LIMBS], one[EC_MAX_LIMBS];
    uint64_t u1[EC_MAX_LIMBS], u2[EC_MAX_LIMBS];
    memset(one, 0, sizeof one);
    one[0] = 1;
    bm_mont_mul(sR, sv, rr, n, m0inv, k);           // s -> Montgomery domain
    bm_mont_exp_limbs(wR, sR, nm2, k, n, m0inv, k); // wR = (s^-1)R
    bm_mont_mul(u1, e, wR, n, m0inv, k);            // e * (s^-1)R * R^-1 = u1
    bm_mont_mul(u2, rv, wR, n, m0inv, k);           // r * (s^-1)R * R^-1 = u2

    // R = u1*G + u2*Q. Reject the point at infinity. v = R.x mod n == r.
    uint8_t u1b[EC_MAX_BYTES], u2b[EC_MAX_BYTES];
    bm_to_be(u1b, u1, k);
    bm_to_be(u2b, u2, k);

    EcPoint G;
    memset(&G, 0, sizeof G);
    if (!ec_fe_from_be(c, G.x, c->gx) || !ec_fe_from_be(c, G.y, c->gy)) {
        *err = ec_invalid("internal: generator out of range");
        return;
    }
    c->one(G.z);

    EcPoint p1, p2, R;
    ec_scalar_mult(c, &p1, &G, u1b, b);
    ec_scalar_mult(c, &p2, &Q, u2b, b);
    ec_point_add(c, &R, &p1, &p2, b);

    if (ec_fe_is_zero(c, R.z)) {
        *err = rt_error_new("crypto.ecdsaVerify: the signature does not verify",
                            NIO_ERR_AUTHENTICATION);
        return;
    }

    // Affine x: X / Z, with the inversion as z^(p-2).
    uint8_t pm2[EC_MAX_BYTES];
    memcpy(pm2, c->p, (size_t)c->nbytes);
    pm2[c->nbytes - 1] -= 2;                        // p is odd, so no borrow
    uint64_t zinv[EC_MAX_LIMBS], xaff[EC_MAX_LIMBS];
    ec_fe_exp(c, zinv, R.z, pm2);
    c->mul(xaff, R.x, zinv);

    uint8_t xb[EC_MAX_BYTES];
    ec_fe_to_be(c, xb, xaff);
    uint64_t v[EC_MAX_LIMBS];
    bm_from_be(v, xb, c->nbytes, k);
    if (bm_cmp(v, n, k) >= 0) bm_sub(v, n, k);      // x < p < 2n, one subtract

    if (bm_cmp(v, rv, k) != 0) {
        *err = rt_error_new("crypto.ecdsaVerify: the signature does not verify",
                            NIO_ERR_AUTHENTICATION);
    }
}

// ---- AES-128-GCM (NIST SP 800-38D), hardware only ----
//
// This suite exists only where the CPU can run it in constant time. A software
// AES is either table-driven, which is a cache-timing oracle, or bitsliced,
// which is a large separate task. There is no software fallback. On a CPU
// without the instructions, crypto.aesGcmAvailable() returns false, the two
// functions fail, and tls does not offer the suite.
//
// There are two hardware paths behind arch-neutral helpers: the AES and PMULL
// extensions of arm64, and AES-NI and PCLMULQDQ of x86-64. The SubWord of the
// key schedule takes key material and goes through the AES instructions, so
// no S-box table exists in memory.

#if defined(__aarch64__) && (defined(__ARM_FEATURE_AES) || defined(__ARM_FEATURE_CRYPTO))
#define NIO_AESGCM_ARM 1
#define NIO_AESGCM_HW 1
#include <arm_neon.h>
#define AESGCM_TARGET
typedef uint8x16_t blk;
#elif defined(__x86_64__) || defined(_M_X64)
#define NIO_AESGCM_X86 1
#define NIO_AESGCM_HW 1
#include <wmmintrin.h>
#include <tmmintrin.h>
#include <emmintrin.h>
#if defined(_WIN32)
#include <intrin.h>
#endif
// Baseline x86-64 has none of these, so each function requests them, and
// aes_gcm_usable() checks at run time before any of them runs.
#define AESGCM_TARGET __attribute__((target("aes,pclmul,ssse3")))
typedef __m128i blk;
#endif

#ifdef NIO_AESGCM_HW

#ifdef NIO_AESGCM_ARM
static inline blk blk_load(const uint8_t *p) { return vld1q_u8(p); }
static inline void blk_store(uint8_t *p, blk b) { vst1q_u8(p, b); }
static inline blk blk_xor(blk a, blk b) { return veorq_u8(a, b); }
static inline blk blk_zero(void) { return vdupq_n_u8(0); }
static inline blk blk_rbit(blk a) { return vrbitq_u8(a); }
static inline blk blk_shl64(blk a) { return vextq_u8(blk_zero(), a, 8); }
static inline blk blk_shr64(blk a) { return vextq_u8(a, blk_zero(), 8); }
static inline uint64_t blk_lo(blk a) { return vgetq_lane_u64(vreinterpretq_u64_u8(a), 0); }
static inline uint64_t blk_hi(blk a) { return vgetq_lane_u64(vreinterpretq_u64_u8(a), 1); }
static inline blk clmul64(uint64_t a, uint64_t b) {
    return vreinterpretq_u8_p128(vmull_p64((poly64_t)a, (poly64_t)b));
}
#else
static inline AESGCM_TARGET blk blk_load(const uint8_t *p) {
    return _mm_loadu_si128((const __m128i *)p);
}
static inline AESGCM_TARGET void blk_store(uint8_t *p, blk b) {
    _mm_storeu_si128((__m128i *)p, b);
}
static inline AESGCM_TARGET blk blk_xor(blk a, blk b) { return _mm_xor_si128(a, b); }
static inline AESGCM_TARGET blk blk_zero(void) { return _mm_setzero_si128(); }
// x86 has no per-byte bit reversal, so this uses two PSHUFB nibble permutes.
// PSHUFB is a register shuffle and not a memory lookup, so it stays constant
// time.
static inline AESGCM_TARGET blk blk_rbit(blk a) {
    const __m128i mask = _mm_set1_epi8(0x0f);
    const __m128i lut = _mm_setr_epi8(0x00, 0x08, 0x04, 0x0c, 0x02, 0x0a, 0x06, 0x0e,
                                      0x01, 0x09, 0x05, 0x0d, 0x03, 0x0b, 0x07, 0x0f);
    __m128i lo = _mm_and_si128(a, mask);
    __m128i hi = _mm_and_si128(_mm_srli_epi16(a, 4), mask);
    lo = _mm_shuffle_epi8(lut, lo);
    hi = _mm_shuffle_epi8(lut, hi);
    return _mm_or_si128(_mm_slli_epi16(lo, 4), hi);
}
static inline AESGCM_TARGET blk blk_shl64(blk a) { return _mm_slli_si128(a, 8); }
static inline AESGCM_TARGET blk blk_shr64(blk a) { return _mm_srli_si128(a, 8); }
static inline AESGCM_TARGET uint64_t blk_lo(blk a) {
    return (uint64_t)_mm_cvtsi128_si64(a);
}
// A byte shift and not _mm_extract_epi64, which needs SSE4.1. This path
// requests three CPU features and does not need a fourth.
static inline AESGCM_TARGET uint64_t blk_hi(blk a) {
    return (uint64_t)_mm_cvtsi128_si64(_mm_srli_si128(a, 8));
}
static inline AESGCM_TARGET blk clmul64(uint64_t a, uint64_t b) {
    return _mm_clmulepi64_si128(_mm_cvtsi64_si128((long long)a),
                                _mm_cvtsi64_si128((long long)b), 0x00);
}
#endif

typedef struct { blk rk[11]; } Aes128;

#ifdef NIO_AESGCM_ARM
// AESE(d, k) computes ShiftRows(SubBytes(d ^ k)). With a zero key and the word
// in all four columns, every row holds four equal bytes, so ShiftRows is the
// identity and each lane comes back as SubWord.
static inline uint32_t aes_sub_word(uint32_t w) {
    uint8x16_t v = vreinterpretq_u8_u32(vdupq_n_u32(w));
    v = vaeseq_u8(v, vdupq_n_u8(0));
    return vgetq_lane_u32(vreinterpretq_u32_u8(v), 0);
}

static void aes128_expand(Aes128 *a, const uint8_t key[16]) {
    static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10,
                                     0x20, 0x40, 0x80, 0x1b, 0x36};
    uint32_t w[44];
    for (int i = 0; i < 4; i++) {
        w[i] = (uint32_t)key[4 * i] | ((uint32_t)key[4 * i + 1] << 8)
             | ((uint32_t)key[4 * i + 2] << 16) | ((uint32_t)key[4 * i + 3] << 24);
    }
    for (int i = 4; i < 44; i++) {
        uint32_t t = w[i - 1];
        if (i % 4 == 0) {
            t = (t >> 8) | (t << 24);                   // RotWord
            t = aes_sub_word(t);
            t ^= (uint32_t)rcon[i / 4 - 1];
        }
        w[i] = w[i - 4] ^ t;
    }
    for (int r = 0; r <= 10; r++) {
        uint8_t buf[16];
        for (int i = 0; i < 4; i++) {
            uint32_t v = w[4 * r + i];
            buf[4 * i] = (uint8_t)v;
            buf[4 * i + 1] = (uint8_t)(v >> 8);
            buf[4 * i + 2] = (uint8_t)(v >> 16);
            buf[4 * i + 3] = (uint8_t)(v >> 24);
        }
        a->rk[r] = vld1q_u8(buf);
    }
    memset(w, 0, sizeof w);
}

static inline blk aes128_block(const Aes128 *a, blk b) {
    for (int i = 0; i < 9; i++) b = vaesmcq_u8(vaeseq_u8(b, a->rk[i]));
    b = vaeseq_u8(b, a->rk[9]);
    return veorq_u8(b, a->rk[10]);
}
#else
// AESKEYGENASSIST does the same in one instruction. It returns
// RotWord(SubWord(x)) ^ rcon in its top lane, and keeps the S-box out of
// memory.
#define AES_EXPAND_STEP(k, rc)                                                  \
    do {                                                                        \
        __m128i g = _mm_aeskeygenassist_si128((k), (rc));                       \
        g = _mm_shuffle_epi32(g, 0xff);                                         \
        __m128i t = (k);                                                        \
        t = _mm_xor_si128(t, _mm_slli_si128(t, 4));                             \
        t = _mm_xor_si128(t, _mm_slli_si128(t, 4));                             \
        t = _mm_xor_si128(t, _mm_slli_si128(t, 4));                             \
        (k) = _mm_xor_si128(t, g);                                              \
    } while (0)

static AESGCM_TARGET void aes128_expand(Aes128 *a, const uint8_t key[16]) {
    __m128i k = _mm_loadu_si128((const __m128i *)key);
    a->rk[0] = k;
    AES_EXPAND_STEP(k, 0x01); a->rk[1] = k;
    AES_EXPAND_STEP(k, 0x02); a->rk[2] = k;
    AES_EXPAND_STEP(k, 0x04); a->rk[3] = k;
    AES_EXPAND_STEP(k, 0x08); a->rk[4] = k;
    AES_EXPAND_STEP(k, 0x10); a->rk[5] = k;
    AES_EXPAND_STEP(k, 0x20); a->rk[6] = k;
    AES_EXPAND_STEP(k, 0x40); a->rk[7] = k;
    AES_EXPAND_STEP(k, 0x80); a->rk[8] = k;
    AES_EXPAND_STEP(k, 0x1b); a->rk[9] = k;
    AES_EXPAND_STEP(k, 0x36); a->rk[10] = k;
}

static inline AESGCM_TARGET blk aes128_block(const Aes128 *a, blk b) {
    b = _mm_xor_si128(b, a->rk[0]);
    for (int i = 1; i < 10; i++) b = _mm_aesenc_si128(b, a->rk[i]);
    return _mm_aesenclast_si128(b, a->rk[10]);
}
#endif

// GHASH (§6.4). GCM numbers the bits of a block in the opposite order from an
// integer: the most significant bit of byte 0 is the coefficient of x^0. The
// bits of each byte are reversed on input, so the multiply below is a plain
// carry-less schoolbook product with the textbook reduction. The reduction
// uses x^128 = x^7 + x^2 + x + 1 twice: once to fold the high half of the
// product down, and once for the carry that the fold moves past x^127.
#define GHASH_POLY 0x87ull

static inline AESGCM_TARGET blk gf_mul(blk a, blk b) {
    uint64_t a0 = blk_lo(a), a1 = blk_hi(a);
    uint64_t b0 = blk_lo(b), b1 = blk_hi(b);

    blk lo = clmul64(a0, b0);
    blk hi = clmul64(a1, b1);
    blk mid = blk_xor(clmul64(a0, b1), clmul64(a1, b0));

    blk L = blk_xor(lo, blk_shl64(mid));
    blk H = blk_xor(hi, blk_shr64(mid));

    blk t = clmul64(blk_lo(H), GHASH_POLY);
    blk u = clmul64(blk_hi(H), GHASH_POLY);
    L = blk_xor(L, t);
    L = blk_xor(L, blk_shl64(u));
    L = blk_xor(L, clmul64(blk_hi(u), GHASH_POLY));
    return L;
}

typedef struct { blk h; blk acc; } Ghash;

static inline AESGCM_TARGET void ghash_init(Ghash *g, blk h) {
    g->h = blk_rbit(h);
    g->acc = blk_zero();
}

static inline AESGCM_TARGET void ghash_block(Ghash *g, const uint8_t *p) {
    g->acc = gf_mul(blk_xor(g->acc, blk_rbit(blk_load(p))), g->h);
}

// Whole bytes, zero-padded to a block. The padding prevents an undetected move
// of a byte between the AAD and the ciphertext.
static AESGCM_TARGET void ghash_bytes(Ghash *g, const uint8_t *p, size_t n) {
    while (n >= 16) {
        ghash_block(g, p);
        p += 16;
        n -= 16;
    }
    if (n) {
        uint8_t pad[16] = {0};
        memcpy(pad, p, n);
        ghash_block(g, pad);
    }
}

static inline AESGCM_TARGET void ghash_final(Ghash *g, uint8_t out[16]) {
    blk_store(out, blk_rbit(g->acc));
}

// A 12-byte nonce makes J0 = IV || 0x00000001 with no GHASH of the IV. TLS uses
// only that case, and only that case is implemented.
static inline AESGCM_TARGET blk gcm_counter(const uint8_t nonce[12], uint32_t ctr) {
    uint8_t b[16];
    memcpy(b, nonce, 12);
    b[12] = (uint8_t)(ctr >> 24);
    b[13] = (uint8_t)(ctr >> 16);
    b[14] = (uint8_t)(ctr >> 8);
    b[15] = (uint8_t)ctr;
    return blk_load(b);
}

static AESGCM_TARGET void gcm_ctr(const Aes128 *a, const uint8_t nonce[12],
                                  uint32_t start, const uint8_t *in, uint8_t *out,
                                  size_t n) {
    uint32_t ctr = start;
    while (n >= 16) {
        blk ks = aes128_block(a, gcm_counter(nonce, ctr++));
        blk_store(out, blk_xor(blk_load(in), ks));
        in += 16;
        out += 16;
        n -= 16;
    }
    if (n) {
        uint8_t ks[16], tail[16] = {0};
        blk_store(ks, aes128_block(a, gcm_counter(nonce, ctr)));
        memcpy(tail, in, n);
        for (size_t i = 0; i < n; i++) tail[i] ^= ks[i];
        memcpy(out, tail, n);
        memset(ks, 0, sizeof ks);
    }
}

static AESGCM_TARGET void gcm_tag(const Aes128 *a, const uint8_t nonce[12],
                                  const uint8_t *aad, size_t aad_len,
                                  const uint8_t *ct, size_t ct_len,
                                  uint8_t tag[16]) {
    Ghash g;
    ghash_init(&g, aes128_block(a, blk_zero()));       // H = AES_K(0^128)
    ghash_bytes(&g, aad, aad_len);
    ghash_bytes(&g, ct, ct_len);

    uint8_t lengths[16];
    uint64_t abits = (uint64_t)aad_len * 8, cbits = (uint64_t)ct_len * 8;
    for (int i = 0; i < 8; i++) {
        lengths[i] = (uint8_t)(abits >> (56 - 8 * i));
        lengths[8 + i] = (uint8_t)(cbits >> (56 - 8 * i));
    }
    ghash_block(&g, lengths);

    uint8_t s[16];
    ghash_final(&g, s);
    // T = S ^ AES_K(J0). J0 is the counter block before the one of the payload.
    blk_store(tag, blk_xor(blk_load(s), aes128_block(a, gcm_counter(nonce, 1))));
    memset(s, 0, sizeof s);
}

static AESGCM_TARGET void aes128gcm_seal_raw(const uint8_t key[16], const uint8_t nonce[12],
                                             const uint8_t *pt, size_t pt_len,
                                             const uint8_t *aad, size_t aad_len,
                                             uint8_t *out) {
    Aes128 a;
    aes128_expand(&a, key);
    gcm_ctr(&a, nonce, 2, pt, out, pt_len);
    gcm_tag(&a, nonce, aad, aad_len, out, pt_len, out + pt_len);
    memset(&a, 0, sizeof a);
}

// Returns 1 when the tag verifies, and writes plaintext only then.
static AESGCM_TARGET int aes128gcm_open_raw(const uint8_t key[16], const uint8_t nonce[12],
                                            const uint8_t *ct, size_t ct_len,
                                            const uint8_t *aad, size_t aad_len,
                                            const uint8_t tag[16], uint8_t *out) {
    Aes128 a;
    aes128_expand(&a, key);
    uint8_t want[16];
    gcm_tag(&a, nonce, aad, aad_len, ct, ct_len, want);
    int ok = ct_equal(want, tag, 16);
    if (ok) gcm_ctr(&a, nonce, 2, ct, out, ct_len);
    memset(&a, 0, sizeof a);
    memset(want, 0, sizeof want);
    return ok;
}
#endif /* NIO_AESGCM_HW */

// Whether this build, on this CPU, can do AES-GCM in constant time. On arm64
// the AES extension of the target decides at compile time. On x86-64 the
// instructions are optional in the baseline, so the path is always compiled
// and this function gates it.
static int aes_gcm_usable(void) {
#if defined(NIO_AESGCM_X86) && defined(_WIN32)
    // The GCC builtin below reads a CPU model that compiler-rt fills in, and
    // the MSVC target does not link compiler-rt. Leaf 1 of cpuid gives the
    // same three bits directly.
    int r[4];
    __cpuid(r, 1);
    return (r[2] & (1 << 25)) != 0 && (r[2] & (1 << 1)) != 0 && (r[2] & (1 << 9)) != 0;
#elif defined(NIO_AESGCM_X86)
    return __builtin_cpu_supports("aes") && __builtin_cpu_supports("pclmul")
        && __builtin_cpu_supports("ssse3");
#elif defined(NIO_AESGCM_HW)
    return 1;
#else
    return 0;
#endif
}

int64_t rt_crypto_aes_gcm_available(void) {
    return aes_gcm_usable() ? 1 : 0;
}

static void *aes_gcm_unavailable(const char *fn) {
    char msg[240];
    snprintf(msg, sizeof msg,
             "crypto.%s: this CPU has no AES instructions, and a software AES "
             "would be a cache-timing oracle, so none is provided", fn);
    return rt_error_new(msg, NIO_ERR_INVALID);
}

#define AESGCM_KEY_LEN 16
#define AESGCM_NONCE_LEN 12

Arr *rt_crypto_aes128gcm_seal(Arr *key, Arr *nonce, Arr *plaintext, Arr *aad,
                              void **err) {
    need_len("aes128GcmSeal", "the key", key, AESGCM_KEY_LEN);
    need_len("aes128GcmSeal", "the nonce", nonce, AESGCM_NONCE_LEN);
    if (!aes_gcm_usable()) {
        *err = aes_gcm_unavailable("aes128GcmSeal");
        return NULL;
    }
#ifdef NIO_AESGCM_HW
    TypeDesc *tds[4] = {&td_byte_array, &td_byte_array, &td_byte_array, &td_byte_array};
    int64_t slots[4] = {(int64_t)(intptr_t)key, (int64_t)(intptr_t)nonce,
                        (int64_t)(intptr_t)plaintext, (int64_t)(intptr_t)aad};
    GCFrame f = {rt_gc_top, 4, tds, slots, NULL};
    rt_gc_top = &f;
    Arr *out = rt_arr_new(plaintext->len + 16, 1);
    rt_gc_top = f.prev;

    aes128gcm_seal_raw(rt_arr_bytes(key), rt_arr_bytes(nonce),
                       rt_arr_bytes(plaintext), (size_t)plaintext->len,
                       rt_arr_bytes(aad), (size_t)aad->len, rt_arr_bytes(out));
    return out;
#else
    return NULL;
#endif
}

Arr *rt_crypto_aes128gcm_open(Arr *key, Arr *nonce, Arr *ciphertext, Arr *aad,
                              void **err) {
    need_len("aes128GcmOpen", "the key", key, AESGCM_KEY_LEN);
    need_len("aes128GcmOpen", "the nonce", nonce, AESGCM_NONCE_LEN);
    if (!aes_gcm_usable()) {
        *err = aes_gcm_unavailable("aes128GcmOpen");
        return NULL;
    }
#ifdef NIO_AESGCM_HW
    // A ciphertext too short for a tag reports as a failed tag, for the
    // reason given in the ChaCha open.
    if (ciphertext->len < 16) {
        *err = rt_error_new("crypto.aes128GcmOpen: authentication failed",
                            NIO_ERR_AUTHENTICATION);
        return NULL;
    }
    int64_t ptlen = ciphertext->len - 16;

    // Allocate first, then verify and decrypt into it. The plaintext is written
    // only when the tag is valid. On failure nothing references the block.
    TypeDesc *tds[4] = {&td_byte_array, &td_byte_array, &td_byte_array, &td_byte_array};
    int64_t slots[4] = {(int64_t)(intptr_t)key, (int64_t)(intptr_t)nonce,
                        (int64_t)(intptr_t)ciphertext, (int64_t)(intptr_t)aad};
    GCFrame f = {rt_gc_top, 4, tds, slots, NULL};
    rt_gc_top = &f;
    Arr *out = rt_arr_new(ptlen, 1);
    rt_gc_top = f.prev;

    const uint8_t *ct = rt_arr_bytes(ciphertext);
    if (!aes128gcm_open_raw(rt_arr_bytes(key), rt_arr_bytes(nonce), ct, (size_t)ptlen,
                            rt_arr_bytes(aad), (size_t)aad->len, ct + ptlen,
                            rt_arr_bytes(out))) {
        *err = rt_error_new("crypto.aes128GcmOpen: authentication failed",
                            NIO_ERR_AUTHENTICATION);
        return NULL;
    }
    return out;
#else
    return NULL;
#endif
}
