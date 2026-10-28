#include "des3.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* DES tables (FIPS 46-3). Bit numbering is 1-based, MSB first.              */
/* ------------------------------------------------------------------------- */

static const uint8_t des_ip[64] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7,
};

static const uint8_t des_fp[64] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25,
};

static const uint8_t des_e[48] = {
    32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11,
    12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
    22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1,
};

static const uint8_t des_p[32] = {
    16, 7,  20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
    2,  8,  24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25,
};

static const uint8_t des_pc1[56] = {
    57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18,
    10, 2,  59, 51, 43, 35, 27, 19, 11, 3,  60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15, 7,  62, 54, 46, 38, 30, 22,
    14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4,
};

static const uint8_t des_pc2[48] = {
    14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,
    26, 8,  16, 7,  27, 20, 13, 2,  41, 52, 31, 37, 47, 55, 30, 40,
    51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32,
};

static const uint8_t des_shifts[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};

static const uint8_t des_sbox[8][64] = {
    {/* S1 */
     14, 4,  13, 1, 2,  15, 11, 8,  3,  10, 6,  12, 5,  9,  0, 7,
     0,  15, 7,  4, 14, 2,  13, 1,  10, 6,  12, 11, 9,  5,  3, 8,
     4,  1,  14, 8, 13, 6,  2,  11, 15, 12, 9,  7,  3,  10, 5, 0,
     15, 12, 8,  2, 4,  9,  1,  7,  5,  11, 3,  14, 10, 0,  6, 13},
    {/* S2 */
     15, 1,  8,  14, 6,  11, 3,  4,  9,  7,  2,  13, 12, 0, 5,  10,
     3,  13, 4,  7,  15, 2,  8,  14, 12, 0,  1,  10, 6,  9, 11, 5,
     0,  14, 7,  11, 10, 4,  13, 1,  5,  8,  12, 6,  9,  3, 2,  15,
     13, 8,  10, 1,  3,  15, 4,  2,  11, 6,  7,  12, 0,  5, 14, 9},
    {/* S3 */
     10, 0,  9,  14, 6, 3,  15, 5,  1,  13, 12, 7,  11, 4,  2,  8,
     13, 7,  0,  9,  3, 4,  6,  10, 2,  8,  5,  14, 12, 11, 15, 1,
     13, 6,  4,  9,  8, 15, 3,  0,  11, 1,  2,  12, 5,  10, 14, 7,
     1,  10, 13, 0,  6, 9,  8,  7,  4,  15, 14, 3,  11, 5,  2,  12},
    {/* S4 */
     7,  13, 14, 3,  0,  6,  9,  10, 1,  2, 8,  5,  11, 12, 4,  15,
     13, 8,  11, 5,  6,  15, 0,  3,  4,  7, 2,  12, 1,  10, 14, 9,
     10, 6,  9,  0,  12, 11, 7,  13, 15, 1, 3,  14, 5,  2,  8,  4,
     3,  15, 0,  6,  10, 1,  13, 8,  9,  4, 5,  11, 12, 7,  2,  14},
    {/* S5 */
     2,  12, 4, 1,  7,  10, 11, 6,  8,  5,  3,  15, 13, 0,  14, 9,
     14, 11, 2, 12, 4,  7,  13, 1,  5,  0,  15, 10, 3,  9,  8,  6,
     4,  2,  1, 11, 10, 13, 7,  8,  15, 9,  12, 5,  6,  3,  0,  14,
     11, 8,  12, 7, 1,  14, 2,  13, 6,  15, 0,  9,  10, 4,  5,  3},
    {/* S6 */
     12, 1,  10, 15, 9,  2,  6,  8,  0,  13, 3, 4,  14, 7, 5,  11,
     10, 15, 4,  2,  7,  12, 9,  5,  6,  1,  13, 14, 0,  11, 3, 8,
     9,  14, 15, 5,  2,  8,  12, 3,  7,  0,  4, 10, 1,  13, 11, 6,
     4,  3,  2,  12, 9,  5,  15, 10, 11, 14, 1, 7,  6,  0, 8,  13},
    {/* S7 */
     4,  11, 2,  14, 15, 0,  8, 13, 3,  12, 9,  7,  5,  10, 6, 1,
     13, 0,  11, 7,  4,  9,  1, 10, 14, 3,  5,  12, 2,  15, 8, 6,
     1,  4,  11, 13, 12, 3,  7, 14, 10, 15, 6,  8,  0,  5,  9, 2,
     6,  11, 13, 8,  1,  4,  10, 7, 9,  5,  0,  15, 14, 2,  3, 12},
    {/* S8 */
     13, 2,  8, 4, 6,  15, 11, 1,  10, 9,  3,  14, 5,  0,  12, 7,
     1,  15, 13, 8, 10, 3,  7,  4,  12, 5,  6,  11, 0,  14, 9,  2,
     7,  11, 4, 1, 9,  12, 14, 2,  0,  6,  10, 13, 15, 3,  5,  8,
     2,  1,  14, 7, 4,  10, 8,  13, 15, 12, 9,  0,  3,  5,  6,  11},
};

static uint64_t des_permute(uint64_t in, const uint8_t* table, size_t nbits, size_t src_bits) {
    uint64_t out = 0;
    for(size_t i = 0; i < nbits; i++) {
        uint64_t bit = (in >> (src_bits - table[i])) & 1ULL;
        out = (out << 1) | bit;
    }
    return out;
}

static uint64_t des_bytes_to_u64(const uint8_t in[8]) {
    uint64_t v = 0;
    for(size_t i = 0; i < 8; i++) {
        v = (v << 8) | in[i];
    }
    return v;
}

static void des_u64_to_bytes(uint64_t v, uint8_t out[8]) {
    for(size_t i = 0; i < 8; i++) {
        out[7 - i] = (uint8_t)(v >> (8 * i));
    }
}

static uint32_t des_rotl28(uint32_t v, uint8_t n) {
    return ((v << n) | (v >> (28 - n))) & 0x0FFFFFFFUL;
}

static void des_make_subkeys(const uint8_t key[8], uint64_t subkeys[16]) {
    uint64_t k = des_bytes_to_u64(key);
    uint64_t pc1 = des_permute(k, des_pc1, 56, 64);
    uint32_t c = (uint32_t)((pc1 >> 28) & 0x0FFFFFFFUL);
    uint32_t d = (uint32_t)(pc1 & 0x0FFFFFFFUL);

    for(size_t i = 0; i < 16; i++) {
        c = des_rotl28(c, des_shifts[i]);
        d = des_rotl28(d, des_shifts[i]);
        uint64_t cd = ((uint64_t)c << 28) | d;
        subkeys[i] = des_permute(cd, des_pc2, 48, 56);
    }
}

static uint64_t des_crypt_block(uint64_t block, const uint64_t subkeys[16]) {
    uint64_t ip = des_permute(block, des_ip, 64, 64);
    uint32_t l = (uint32_t)((ip >> 32) & 0xFFFFFFFFUL);
    uint32_t r = (uint32_t)(ip & 0xFFFFFFFFUL);

    for(size_t round = 0; round < 16; round++) {
        uint64_t expanded = des_permute((uint64_t)r, des_e, 48, 32);
        uint64_t x = expanded ^ subkeys[round];

        uint32_t s_out = 0;
        for(size_t box = 0; box < 8; box++) {
            uint8_t six = (uint8_t)((x >> (42 - 6 * box)) & 0x3F);
            uint8_t row = (uint8_t)(((six & 0x20) >> 4) | (six & 0x01));
            uint8_t col = (uint8_t)((six >> 1) & 0x0F);
            s_out = (s_out << 4) | des_sbox[box][row * 16 + col];
        }

        uint32_t f = (uint32_t)des_permute((uint64_t)s_out, des_p, 32, 32);
        uint32_t next_l = r;
        r = l ^ f;
        l = next_l;
    }

    uint64_t preoutput = ((uint64_t)r << 32) | l;
    return des_permute(preoutput, des_fp, 64, 64);
}

void des_encrypt_block(
    const uint8_t key[DES3_BLOCK_SIZE],
    const uint8_t in[DES3_BLOCK_SIZE],
    uint8_t out[DES3_BLOCK_SIZE]) {
    uint64_t subkeys[16];
    des_make_subkeys(key, subkeys);
    des_u64_to_bytes(des_crypt_block(des_bytes_to_u64(in), subkeys), out);
}

void des_decrypt_block(
    const uint8_t key[DES3_BLOCK_SIZE],
    const uint8_t in[DES3_BLOCK_SIZE],
    uint8_t out[DES3_BLOCK_SIZE]) {
    uint64_t subkeys[16];
    des_make_subkeys(key, subkeys);
    uint64_t reversed[16];
    for(size_t i = 0; i < 16; i++) {
        reversed[i] = subkeys[15 - i];
    }
    des_u64_to_bytes(des_crypt_block(des_bytes_to_u64(in), reversed), out);
}

static void des3_2key_encrypt_block(const uint8_t* key, const uint8_t in[8], uint8_t out[8]) {
    uint8_t tmp[8];
    des_encrypt_block(&key[0], in, tmp);
    des_decrypt_block(&key[8], tmp, tmp);
    des_encrypt_block(&key[0], tmp, out);
}

static void des3_2key_decrypt_block(const uint8_t* key, const uint8_t in[8], uint8_t out[8]) {
    uint8_t tmp[8];
    des_decrypt_block(&key[0], in, tmp);
    des_encrypt_block(&key[8], tmp, tmp);
    des_decrypt_block(&key[0], tmp, out);
}

bool des3_2key_cbc_encrypt(
    const uint8_t* key,
    const uint8_t* iv,
    const uint8_t* in,
    size_t len,
    uint8_t* out) {
    if(!key || !iv || !in || !out || (len == 0) || (len % DES3_BLOCK_SIZE != 0)) {
        return false;
    }

    uint8_t chain[DES3_BLOCK_SIZE];
    memcpy(chain, iv, sizeof(chain));

    for(size_t offset = 0; offset < len; offset += DES3_BLOCK_SIZE) {
        uint8_t block[DES3_BLOCK_SIZE];
        for(size_t i = 0; i < DES3_BLOCK_SIZE; i++) {
            block[i] = in[offset + i] ^ chain[i];
        }
        des3_2key_encrypt_block(key, block, chain);
        memcpy(&out[offset], chain, DES3_BLOCK_SIZE);
    }

    return true;
}

bool des3_2key_cbc_decrypt(
    const uint8_t* key,
    const uint8_t* iv,
    const uint8_t* in,
    size_t len,
    uint8_t* out) {
    if(!key || !iv || !in || !out || (len == 0) || (len % DES3_BLOCK_SIZE != 0)) {
        return false;
    }

    uint8_t chain[DES3_BLOCK_SIZE];
    memcpy(chain, iv, sizeof(chain));

    for(size_t offset = 0; offset < len; offset += DES3_BLOCK_SIZE) {
        uint8_t cipher[DES3_BLOCK_SIZE];
        uint8_t plain[DES3_BLOCK_SIZE];
        memcpy(cipher, &in[offset], sizeof(cipher));
        des3_2key_decrypt_block(key, cipher, plain);
        for(size_t i = 0; i < DES3_BLOCK_SIZE; i++) {
            out[offset + i] = plain[i] ^ chain[i];
        }
        memcpy(chain, cipher, sizeof(chain));
    }

    return true;
}

#ifdef DES3_SELFTEST
#include <stdio.h>

static int hexcmp(const uint8_t* got, const uint8_t* want, size_t len) {
    for(size_t i = 0; i < len; i++) {
        if(got[i] != want[i]) return 0;
    }
    return 1;
}

int main(void) {
    int failures = 0;

    const uint8_t key1[8] = {0x13, 0x34, 0x57, 0x79, 0x9B, 0xBC, 0xDF, 0xF1};
    const uint8_t pt1[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    const uint8_t ct1[8] = {0x85, 0xE8, 0x13, 0x54, 0x0F, 0x0A, 0xB4, 0x05};
    uint8_t out[8] = {0};
    des_encrypt_block(key1, pt1, out);
    if(!hexcmp(out, ct1, 8)) { failures++; }
    uint8_t back[8] = {0};
    des_decrypt_block(key1, ct1, back);
    if(!hexcmp(back, pt1, 8)) { failures++; }

    const uint8_t kat_key[16] = {
        0x42, 0x52, 0x45, 0x41, 0x4B, 0x4D, 0x45, 0x49,
        0x46, 0x59, 0x4F, 0x55, 0x43, 0x41, 0x4E, 0x21};
    const uint8_t kat_iv[8] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    const uint8_t kat_pt[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    const uint8_t kat_ct[16] = {
        0x65, 0x92, 0x22, 0x61, 0x4A, 0xFA, 0x4A, 0x4F,
        0x72, 0xB9, 0x50, 0x33, 0xE1, 0xB1, 0xFE, 0xE3};
    uint8_t kat_out[16];
    if(!des3_2key_cbc_encrypt(kat_key, kat_iv, kat_pt, sizeof(kat_pt), kat_out) ||
       !hexcmp(kat_out, kat_ct, sizeof(kat_ct))) { failures++; }

    printf(failures ? "des3: %d FAILURE(S)\n" : "des3: all self tests passed\n", failures);
    return failures ? 1 : 0;
}
#endif
