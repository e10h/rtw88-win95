/*
 * wpa2_crypto.h - Self-contained WPA2-PSK (802.11i) & AES-128-CCMP crypto
 * No libc, zero dynamic allocation, Pentium MMX safe.
 *
 * Implements:
 *   - SHA-1 & HMAC-SHA1
 *   - PBKDF2-HMAC-SHA1 (4096 iter -> 32-byte PMK)
 *   - IEEE 802.11i PRF-384 (-> 48-byte PTK: KCK[16], KEK[16], TK[16])
 *   - AES-128 block cipher (FIPS-197)
 *   - AES Key Unwrap (RFC 3394, for GTK extraction from EAPOL M3)
 *   - IEEE 802.11 AES-128-CCMP Encrypt & Decrypt (RFC 3610 / 802.11-2012)
 */
#ifndef WPA2_CRYPTO_H
#define WPA2_CRYPTO_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

static void c_zero(void *p, u32 n) {
    volatile u8 *b = (volatile u8 *)p;
    while (n--) *b++ = 0;
}

static void c_copy(void *dst, const void *src, u32 n) {
    volatile u8 *d = (volatile u8 *)dst;
    const volatile u8 *s = (const volatile u8 *)src;
    while (n--) *d++ = *s++;
}

static int c_cmp(const void *a, const void *b, u32 n) {
    const u8 *p1 = (const u8 *)a;
    const u8 *p2 = (const u8 *)b;
    for (u32 i = 0; i < n; i++) {
        if (p1[i] != p2[i]) return (int)p1[i] - (int)p2[i];
    }
    return 0;
}

/* =========================================================================
 * SHA-1 & HMAC-SHA1
 * ========================================================================= */
typedef struct {
    u32 h[5];
    u32 count[2];
    u8  buf[64];
} SHA1_CTX;

static u32 rol32(u32 v, int bits) {
    return (v << bits) | (v >> (32 - bits));
}

static void sha1_transform(u32 state[5], const u8 block[64]) {
    u32 w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((u32)block[i * 4] << 24) |
               ((u32)block[i * 4 + 1] << 16) |
               ((u32)block[i * 4 + 2] << 8) |
               ((u32)block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    u32 a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999UL;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1UL;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCUL;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6UL;
        }
        u32 temp = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static void sha1_init(SHA1_CTX *ctx) {
    ctx->h[0] = 0x67452301UL;
    ctx->h[1] = 0xEFCDAB89UL;
    ctx->h[2] = 0x98BADCFEUL;
    ctx->h[3] = 0x10325476UL;
    ctx->h[4] = 0xC3D2E1F0UL;
    ctx->count[0] = 0;
    ctx->count[1] = 0;
}

static void sha1_update(SHA1_CTX *ctx, const u8 *data, u32 len) {
    u32 idx = (ctx->count[0] >> 3) & 63;
    u32 lo = ctx->count[0] + (len << 3);
    if (lo < ctx->count[0]) ctx->count[1]++;
    ctx->count[0] = lo;
    ctx->count[1] += (len >> 29);

    u32 i = 0;
    if (idx) {
        u32 fill = 64 - idx;
        if (len < fill) {
            c_copy(ctx->buf + idx, data, len);
            return;
        }
        c_copy(ctx->buf + idx, data, fill);
        sha1_transform(ctx->h, ctx->buf);
        i = fill;
    }
    while (i + 64 <= len) {
        sha1_transform(ctx->h, data + i);
        i += 64;
    }
    if (i < len) {
        c_copy(ctx->buf, data + i, len - i);
    }
}

static void sha1_final(SHA1_CTX *ctx, u8 out[20]) {
    u8 bits[8];
    for (int i = 0; i < 4; i++) {
        bits[i]     = (u8)(ctx->count[1] >> (24 - i * 8));
        bits[4 + i] = (u8)(ctx->count[0] >> (24 - i * 8));
    }
    u32 idx = (ctx->count[0] >> 3) & 63;
    u32 padLen = (idx < 56) ? (56 - idx) : (120 - idx);
    u8 pad[128];
    c_zero(pad, padLen);
    pad[0] = 0x80;
    sha1_update(ctx, pad, padLen);
    sha1_update(ctx, bits, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4 + 0] = (u8)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (u8)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (u8)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (u8)(ctx->h[i]);
    }
}

static void hmac_sha1(const u8 *key, u32 keyLen, const u8 *msg, u32 msgLen, u8 out[20]) {
    u8 kpad[64];
    u8 tk[20];
    c_zero(kpad, 64);
    if (keyLen > 64) {
        SHA1_CTX c;
        sha1_init(&c);
        sha1_update(&c, key, keyLen);
        sha1_final(&c, tk);
        c_copy(kpad, tk, 20);
    } else {
        c_copy(kpad, key, keyLen);
    }

    u8 ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = (u8)(kpad[i] ^ 0x36);
        opad[i] = (u8)(kpad[i] ^ 0x5C);
    }

    u8 inner[20];
    SHA1_CTX ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, ipad, 64);
    sha1_update(&ctx, msg, msgLen);
    sha1_final(&ctx, inner);

    sha1_init(&ctx);
    sha1_update(&ctx, opad, 64);
    sha1_update(&ctx, inner, 20);
    sha1_final(&ctx, out);
}

/* PBKDF2-HMAC-SHA1 -> 32-byte PMK (4096 iterations) */
static void pbkdf2_sha1_pmk(const u8 *pass, u32 passLen, const u8 *ssid, u32 ssidLen, u8 pmk[32]) {
    u8 salt[40];
    if (ssidLen > 32) ssidLen = 32;
    c_copy(salt, ssid, ssidLen);
    salt[ssidLen + 0] = 0;
    salt[ssidLen + 1] = 0;
    salt[ssidLen + 2] = 0;

    u8 full[40];
    for (u8 block = 1; block <= 2; block++) {
        u8 u[20], t[20];
        salt[ssidLen + 3] = block;
        hmac_sha1(pass, passLen, salt, ssidLen + 4, u);
        c_copy(t, u, 20);
        for (int iter = 1; iter < 4096; iter++) {
            hmac_sha1(pass, passLen, u, 20, u);
            for (int j = 0; j < 20; j++) t[j] ^= u[j];
        }
        c_copy(full + (block - 1) * 20, t, 20);
    }
    c_copy(pmk, full, 32);
}

/* IEEE 802.11i PRF-384 -> 48-byte PTK (KCK[16] | KEK[16] | TK[16]) */
static void wpa2_derive_ptk(const u8 pmk[32], const u8 aa[6], const u8 spa[6],
                            const u8 anonce[32], const u8 snonce[32], u8 ptk[48]) {
    static const char label[] = "Pairwise key expansion";
    u8 buf[128];
    u32 pos = 0;
    for (int i = 0; label[i]; i++) buf[pos++] = (u8)label[i];
    buf[pos++] = 0x00;

    if (c_cmp(aa, spa, 6) < 0) {
        c_copy(buf + pos, aa, 6);  pos += 6;
        c_copy(buf + pos, spa, 6); pos += 6;
    } else {
        c_copy(buf + pos, spa, 6); pos += 6;
        c_copy(buf + pos, aa, 6);  pos += 6;
    }

    if (c_cmp(anonce, snonce, 32) < 0) {
        c_copy(buf + pos, anonce, 32); pos += 32;
        c_copy(buf + pos, snonce, 32); pos += 32;
    } else {
        c_copy(buf + pos, snonce, 32); pos += 32;
        c_copy(buf + pos, anonce, 32); pos += 32;
    }

    u8 out[60];
    for (u8 i = 0; i < 3; i++) {
        buf[pos] = i;
        hmac_sha1(pmk, 32, buf, pos + 1, out + i * 20);
    }
    c_copy(ptk, out, 48);
}

/* =========================================================================
 * AES-128 (FIPS-197) + AES Key Unwrap (RFC 3394)
 * ========================================================================= */
static const u8 aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const u8 aes_rsbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static const u8 aes_rcon[10] = { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };

static u8 gf_xtime(u8 x) { return (u8)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

static u8 gf_mul(u8 a, u8 b) {
    u8 res = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) res ^= a;
        a = gf_xtime(a);
        b >>= 1;
    }
    return res;
}

static void aes128_expand(const u8 key[16], u8 rk[176]) {
    c_copy(rk, key, 16);
    for (int i = 4; i < 44; i++) {
        u8 t[4];
        c_copy(t, rk + (i - 1) * 4, 4);
        if ((i & 3) == 0) {
            u8 tmp = t[0];
            t[0] = aes_sbox[t[1]] ^ aes_rcon[(i >> 2) - 1];
            t[1] = aes_sbox[t[2]];
            t[2] = aes_sbox[t[3]];
            t[3] = aes_sbox[tmp];
        }
        for (int j = 0; j < 4; j++) {
            rk[i * 4 + j] = (u8)(rk[(i - 4) * 4 + j] ^ t[j]);
        }
    }
}

static void aes128_encrypt_block(const u8 rk[176], const u8 in[16], u8 out[16]) {
    u8 s[16];
    for (int i = 0; i < 16; i++) s[i] = (u8)(in[i] ^ rk[i]);

    for (int r = 1; r <= 10; r++) {
        u8 t[16];
        for (int i = 0; i < 16; i++) s[i] = aes_sbox[s[i]];
        /* ShiftRows */
        t[0]  = s[0];  t[4]  = s[4];  t[8]  = s[8];  t[12] = s[12];
        t[1]  = s[5];  t[5]  = s[9];  t[9]  = s[13]; t[13] = s[1];
        t[2]  = s[10]; t[6]  = s[14]; t[10] = s[2];  t[14] = s[6];
        t[3]  = s[15]; t[7]  = s[3];  t[11] = s[7];  t[15] = s[11];
        if (r < 10) {
            /* MixColumns */
            for (int c = 0; c < 4; c++) {
                int o = c * 4;
                u8 a0 = t[o], a1 = t[o + 1], a2 = t[o + 2], a3 = t[o + 3];
                u8 xr = (u8)(a0 ^ a1 ^ a2 ^ a3);
                s[o + 0] = (u8)(a0 ^ xr ^ gf_xtime((u8)(a0 ^ a1)));
                s[o + 1] = (u8)(a1 ^ xr ^ gf_xtime((u8)(a1 ^ a2)));
                s[o + 2] = (u8)(a2 ^ xr ^ gf_xtime((u8)(a2 ^ a3)));
                s[o + 3] = (u8)(a3 ^ xr ^ gf_xtime((u8)(a3 ^ a0)));
            }
        } else {
            c_copy(s, t, 16);
        }
        const u8 *k = rk + r * 16;
        for (int i = 0; i < 16; i++) s[i] ^= k[i];
    }
    c_copy(out, s, 16);
}

static void aes128_decrypt_block(const u8 rk[176], const u8 in[16], u8 out[16]) {
    u8 s[16];
    for (int i = 0; i < 16; i++) s[i] = (u8)(in[i] ^ rk[160 + i]);

    for (int r = 9; r >= 0; r--) {
        u8 t[16];
        /* InvShiftRows */
        t[0]  = s[0];  t[4]  = s[4];  t[8]  = s[8];  t[12] = s[12];
        t[1]  = s[13]; t[5]  = s[1];  t[9]  = s[5];  t[13] = s[9];
        t[2]  = s[10]; t[6]  = s[14]; t[10] = s[2];  t[14] = s[6];
        t[3]  = s[7];  t[7]  = s[11]; t[11] = s[15]; t[15] = s[3];
        /* InvSubBytes + AddRoundKey */
        const u8 *k = rk + r * 16;
        for (int i = 0; i < 16; i++) s[i] = (u8)(aes_rsbox[t[i]] ^ k[i]);
        if (r > 0) {
            /* InvMixColumns */
            for (int c = 0; c < 4; c++) {
                int o = c * 4;
                u8 a0 = s[o], a1 = s[o + 1], a2 = s[o + 2], a3 = s[o + 3];
                t[o + 0] = (u8)(gf_mul(a0, 14) ^ gf_mul(a1, 11) ^ gf_mul(a2, 13) ^ gf_mul(a3, 9));
                t[o + 1] = (u8)(gf_mul(a0, 9)  ^ gf_mul(a1, 14) ^ gf_mul(a2, 11) ^ gf_mul(a3, 13));
                t[o + 2] = (u8)(gf_mul(a0, 13) ^ gf_mul(a1, 9)  ^ gf_mul(a2, 14) ^ gf_mul(a3, 11));
                t[o + 3] = (u8)(gf_mul(a0, 11) ^ gf_mul(a1, 13) ^ gf_mul(a2, 9)  ^ gf_mul(a3, 14));
            }
            c_copy(s, t, 16);
        }
    }
    c_copy(out, s, 16);
}

/* RFC 3394 AES Key Unwrap: cipherLen = (n + 1) * 8, plainLen = n * 8 */
static int aes128_key_unwrap(const u8 kek[16], const u8 *cipher, u32 cipherLen, u8 *plain) {
    if (cipherLen < 16 || (cipherLen & 7) != 0) return 0;
    u32 n = (cipherLen / 8) - 1;
    u8 rk[176];
    aes128_expand(kek, rk);

    u8 a[8];
    c_copy(a, cipher, 8);
    c_copy(plain, cipher + 8, n * 8);

    for (int j = 5; j >= 0; j--) {
        for (int i = (int)n; i >= 1; i--) {
            u32 t = (u32)(n * (u32)j + (u32)i);
            u8 b[16];
            c_copy(b, a, 8);
            b[4] ^= (u8)(t >> 24);
            b[5] ^= (u8)(t >> 16);
            b[6] ^= (u8)(t >> 8);
            b[7] ^= (u8)(t);
            c_copy(b + 8, plain + (i - 1) * 8, 8);
            aes128_decrypt_block(rk, b, b);
            c_copy(a, b, 8);
            c_copy(plain + (i - 1) * 8, b + 8, 8);
        }
    }

    for (int i = 0; i < 8; i++) {
        if (a[i] != 0xA6) return 0;
    }
    return 1;
}

/* =========================================================================
 * IEEE 802.11 AES-128-CCMP Encrypt & Decrypt
 * ========================================================================= */
static u32 ccmp_build_aad_and_nonce(const u8 *hdr, u32 hdrLen, const u8 pn[6],
                                    u8 aad[32], u8 nonce[13]) {
    u8 fc0 = hdr[0];
    u8 fc1 = hdr[1];
    int isQos = ((fc0 & 0x8C) == 0x88);
    int is4Addr = ((fc1 & 0x03) == 0x03);
    u8 priority = 0;

    if (isQos && hdrLen >= (is4Addr ? 32U : 26U)) {
        priority = (u8)(hdr[is4Addr ? 30 : 24] & 0x0F);
    }

    /* Nonce (13 bytes): Priority (1) | A2 (6) | PN5..PN0 (6) */
    nonce[0] = priority;
    c_copy(nonce + 1, hdr + 10, 6);
    nonce[7]  = pn[5];
    nonce[8]  = pn[4];
    nonce[9]  = pn[3];
    nonce[10] = pn[2];
    nonce[11] = pn[1];
    nonce[12] = pn[0];

    /* AAD (IEEE 802.11-2012 11.4.3.3.3):
     *   FC: mask subtype bits 4..6 for data, mask retry/pwrmgt/moredata, force Protected=1
     *   A1(6), A2(6), A3(6), SC & 0x000F (2), [A4(6)], [QC & 0x000F (2)] */
    u32 pos = 0;
    aad[pos++] = (u8)(fc0 & 0x8F);
    aad[pos++] = (u8)((fc1 & (isQos ? 0x47 : 0xC7)) | 0x40);
    c_copy(aad + pos, hdr + 4, 18); pos += 18; /* A1, A2, A3 */
    aad[pos++] = (u8)(hdr[22] & 0x0F);
    aad[pos++] = 0x00;
    if (is4Addr && hdrLen >= 30) {
        c_copy(aad + pos, hdr + 24, 6); pos += 6;
    }
    if (isQos && hdrLen >= (is4Addr ? 32U : 26U)) {
        aad[pos++] = (u8)(hdr[is4Addr ? 30 : 24] & 0x0F);
        aad[pos++] = 0x00;
    }
    return pos;
}

static void ccmp_cbc_mac(const u8 rk[176], const u8 nonce[13],
                         const u8 *aad, u32 aadLen,
                         const u8 *plain, u32 plainLen, u8 mic[8]) {
    u8 b[16], x[16];
    /* B_0: Flags = 0x59 (Adata=1, M'=3 (8B MIC), L'=1 (2B len)) */
    b[0] = 0x59;
    c_copy(b + 1, nonce, 13);
    b[14] = (u8)(plainLen >> 8);
    b[15] = (u8)(plainLen & 0xFF);
    aes128_encrypt_block(rk, b, x);

    /* AAD blocks: 2-byte big-endian length prefix + AAD + zero padding */
    u8 aadBuf[48];
    c_zero(aadBuf, sizeof(aadBuf));
    aadBuf[0] = (u8)(aadLen >> 8);
    aadBuf[1] = (u8)(aadLen & 0xFF);
    c_copy(aadBuf + 2, aad, aadLen);
    u32 aadTotal = ((2 + aadLen + 15) & ~15UL);
    for (u32 off = 0; off < aadTotal; off += 16) {
        for (int i = 0; i < 16; i++) x[i] ^= aadBuf[off + i];
        aes128_encrypt_block(rk, x, x);
    }

    /* Plaintext blocks */
    for (u32 off = 0; off < plainLen; off += 16) {
        u32 chunk = plainLen - off;
        if (chunk > 16) chunk = 16;
        for (u32 i = 0; i < chunk; i++) x[i] ^= plain[off + i];
        aes128_encrypt_block(rk, x, x);
    }
    c_copy(mic, x, 8);
}

static void ccmp_ctr_crypt(const u8 rk[176], const u8 nonce[13],
                           const u8 *in, u8 *out, u32 len,
                           const u8 micIn[8], u8 micOut[8]) {
    u8 ctr[16], s[16];
    ctr[0] = 0x01; /* L' = 1 */
    c_copy(ctr + 1, nonce, 13);

    /* S_0 encrypts/decrypts MIC */
    ctr[14] = 0;
    ctr[15] = 0;
    aes128_encrypt_block(rk, ctr, s);
    for (int i = 0; i < 8; i++) micOut[i] = (u8)(micIn[i] ^ s[i]);

    /* S_1 .. S_m encrypts/decrypts payload */
    u16 blk = 1;
    for (u32 off = 0; off < len; off += 16) {
        ctr[14] = (u8)(blk >> 8);
        ctr[15] = (u8)(blk & 0xFF);
        aes128_encrypt_block(rk, ctr, s);
        u32 chunk = len - off;
        if (chunk > 16) chunk = 16;
        for (u32 i = 0; i < chunk; i++) out[off + i] = (u8)(in[off + i] ^ s[i]);
        blk++;
    }
}

/* Encrypt 802.11 payload in-place/into out:
 * Writes 8-byte CCMP header + encrypted payload + 8-byte MIC into outBuf.
 * Returns total bytes written (plainLen + 16). */
static u32 ccmp_encrypt(const u8 tk[16], const u8 *hdr, u32 hdrLen,
                        u8 keyId, const u8 pn[6],
                        const u8 *plain, u32 plainLen, u8 *outBuf) {
    u8 rk[176], aad[32], nonce[13], mic[8];
    aes128_expand(tk, rk);
    u32 aadLen = ccmp_build_aad_and_nonce(hdr, hdrLen, pn, aad, nonce);

    /* 8-byte CCMP header */
    outBuf[0] = pn[0];
    outBuf[1] = pn[1];
    outBuf[2] = 0x00;
    outBuf[3] = (u8)(0x20 | ((keyId & 3) << 6)); /* ExtIV = 1 */
    outBuf[4] = pn[2];
    outBuf[5] = pn[3];
    outBuf[6] = pn[4];
    outBuf[7] = pn[5];

    ccmp_cbc_mac(rk, nonce, aad, aadLen, plain, plainLen, mic);
    ccmp_ctr_crypt(rk, nonce, plain, outBuf + 8, plainLen, mic, outBuf + 8 + plainLen);
    return plainLen + 16;
}

/* Decrypt 802.11 CCMP body (inBuf starts at 8-byte CCMP header, inLen includes 8B hdr + 8B MIC).
 * Returns 1 if MIC valid, 0 if invalid. Writes plainLen = inLen - 16 bytes into outPlain. */
static int ccmp_decrypt(const u8 tk[16], const u8 *hdr, u32 hdrLen,
                        const u8 *inBuf, u32 inLen, u8 *outPlain, u32 *outPlainLen) {
    if (inLen < 16) return 0;
    if (!(inBuf[3] & 0x20)) return 0; /* Must have ExtIV=1 */

    u8 pn[6];
    pn[0] = inBuf[0];
    pn[1] = inBuf[1];
    pn[2] = inBuf[4];
    pn[3] = inBuf[5];
    pn[4] = inBuf[6];
    pn[5] = inBuf[7];

    u32 plainLen = inLen - 16;
    u8 rk[176], aad[32], nonce[13], rxMic[8], calcMic[8];
    aes128_expand(tk, rk);
    u32 aadLen = ccmp_build_aad_and_nonce(hdr, hdrLen, pn, aad, nonce);

    ccmp_ctr_crypt(rk, nonce, inBuf + 8, outPlain, plainLen, inBuf + 8 + plainLen, rxMic);
    ccmp_cbc_mac(rk, nonce, aad, aadLen, outPlain, plainLen, calcMic);

    if (c_cmp(rxMic, calcMic, 8) != 0) return 0;
    *outPlainLen = plainLen;
    return 1;
}

#endif /* WPA2_CRYPTO_H */
