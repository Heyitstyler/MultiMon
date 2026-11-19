/* Host MD5 (DGT "Hash1") and SHA-1 (DGT "Hash2") for the NitroSDK DGT API; the DS versions are
 * prebuilt ARM code. State lives in the caller's DGTHash*Context fields. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nitro.h>
#include "pc_platform.h"

/* Running hash state, pointing into the fields of the caller's SDK context struct. */
typedef struct {
    u32 *h;
    u64 *length; /* bytes hashed so far */
    u8 *buf;     /* 64-byte block buffer */
} HashState;

static HashState Md5State(DGTHash1Context *c) { return (HashState){ (u32 *)c->state, (u64 *)&c->length, c->buffer8 }; }
/* Nl/Nh are adjacent u32s, used together as one 64-bit byte count. */
static HashState Sha1State(DGTHash2Context *c) { return (HashState){ (u32 *)&c->h0, (u64 *)&c->Nl, (u8 *)c->data }; }

static u32 rol(u32 x, int n) { return (x << n) | (x >> (32 - n)); }

/* ---- MD5 (RFC 1321) ---- */
static u32 sMd5K[64];
static const u8 sMd5S[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };

static void Md5Block(u32 *h, const u8 *p) {
    if (sMd5K[0] == 0) {
        for (int i = 0; i < 64; i++) sMd5K[i] = (u32)(fabs(sin(i + 1.0)) * 4294967296.0);
    }
    u32 m[16];
    for (int i = 0; i < 16; i++) m[i] = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (u32)p[i * 4 + 3] << 24;
    u32 a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
        u32 f;
        int g;
        switch (i / 16) {
        case 0: f = (b & c) | (~b & d); g = i; break;
        case 1: f = (d & b) | (~d & c); g = (5 * i + 1) % 16; break;
        case 2: f = b ^ c ^ d; g = (3 * i + 5) % 16; break;
        default: f = c ^ (b | ~d); g = (7 * i) % 16; break;
        }
        u32 t = d;
        d = c;
        c = b;
        b = b + rol(a + f + sMd5K[i] + m[g], sMd5S[(i / 16) * 4 + i % 4]);
        a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

/* ---- SHA-1 (FIPS 180-4) ---- */
static void Sha1Block(u32 *h, const u8 *p) {
    u32 w[80];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        u32 t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

typedef void (*BlockFn)(u32 *, const u8 *);

static void Update(HashState s, BlockFn block, const u8 *data, u32 len) {
    while (len > 0) {
        u32 used = (u32)(*s.length % 64), take = 64 - used < len ? 64 - used : len;
        memcpy(s.buf + used, data, take);
        *s.length += take;
        data += take;
        len -= take;
        if (*s.length % 64 == 0) block(s.h, s.buf);
    }
}

/* Pads with 0x80, zeros, then the bit length (little-endian for MD5, big-endian for SHA-1). */
static void Finish(HashState s, BlockFn block, BOOL bigEndian) {
    u64 bits = *s.length * 8;
    u8 pad[72] = { 0x80 };
    u32 padLen = (u32)((*s.length % 64 < 56 ? 56 : 120) - *s.length % 64);
    for (int i = 0; i < 8; i++) pad[padLen + i] = (u8)(bits >> (bigEndian ? 56 - 8 * i : 8 * i));
    Update(s, block, pad, padLen + 8);
}

void DGT_Hash1Reset(DGTHash1Context *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->a = 0x67452301; ctx->b = 0xEFCDAB89; ctx->c = 0x98BADCFE; ctx->d = 0x10325476;
}
void DGT_Hash1SetSource(DGTHash1Context *ctx, const void *data, unsigned long len) {
    Update(Md5State(ctx), Md5Block, data, (u32)len);
}
void DGT_Hash1GetDigest_R(void *digest, DGTHash1Context *ctx) {
    Finish(Md5State(ctx), Md5Block, FALSE);
    for (int i = 0; i < 16; i++) ((u8 *)digest)[i] = (u8)(ctx->state[i / 4] >> (8 * (i % 4)));
}

void DGT_Hash2Reset(DGTHash2Context *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->h0 = 0x67452301; ctx->h1 = 0xEFCDAB89; ctx->h2 = 0x98BADCFE; ctx->h3 = 0x10325476; ctx->h4 = 0xC3D2E1F0;
}
void DGT_Hash2SetSource(DGTHash2Context *ctx, const unsigned char *data, unsigned long len) {
    Update(Sha1State(ctx), Sha1Block, data, (u32)len);
}
void DGT_Hash2GetDigest(DGTHash2Context *ctx, unsigned char *digest) {
    Finish(Sha1State(ctx), Sha1Block, TRUE);
    const u32 *h = &ctx->h0;
    for (int i = 0; i < 20; i++) digest[i] = (u8)(h[i / 4] >> (24 - 8 * (i % 4)));
}

/* HMAC-SHA-1 (RFC 2104). */
void DGT_Hash2CalcHmac(void *digest, void *bin_ptr, int bin_len, void *key_ptr, int key_len) {
    u8 key[64] = { 0 }, pad[64], inner[20];
    DGTHash2Context ctx;
    if (key_len > 64) {
        DGT_Hash2Reset(&ctx);
        DGT_Hash2SetSource(&ctx, key_ptr, key_len);
        DGT_Hash2GetDigest(&ctx, key);
    } else {
        memcpy(key, key_ptr, key_len);
    }
    for (int i = 0; i < 64; i++) pad[i] = key[i] ^ 0x36;
    DGT_Hash2Reset(&ctx);
    DGT_Hash2SetSource(&ctx, pad, 64);
    DGT_Hash2SetSource(&ctx, bin_ptr, bin_len);
    DGT_Hash2GetDigest(&ctx, inner);
    for (int i = 0; i < 64; i++) pad[i] = key[i] ^ 0x5C;
    DGT_Hash2Reset(&ctx);
    DGT_Hash2SetSource(&ctx, pad, 64);
    DGT_Hash2SetSource(&ctx, inner, 20);
    DGT_Hash2GetDigest(&ctx, digest);
}

int DGT_SetOverlayTableMode(int flag) { (void)flag; return 0; }

/* Known-answer check, run once at startup. */
void pc_dgt_selftest(void) {
    static const u8 md5abc[16] = { 0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0, 0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72 };
    static const u8 sha1abc[20] = { 0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e, 0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d };
    u8 out[20];
    DGTHash1Context c1;
    DGTHash2Context c2;
    DGT_Hash1Reset(&c1);
    DGT_Hash1SetSource(&c1, "abc", 3);
    DGT_Hash1GetDigest_R(out, &c1);
    BOOL ok = memcmp(out, md5abc, 16) == 0;
    DGT_Hash2Reset(&c2);
    DGT_Hash2SetSource(&c2, (const u8 *)"abc", 3);
    DGT_Hash2GetDigest(&c2, out);
    ok = ok && memcmp(out, sha1abc, 20) == 0;
    if (!ok) {
        fprintf(stderr, "hg_pc: MD5/SHA-1 self-test failed\n");
        exit(1);
    }
}
