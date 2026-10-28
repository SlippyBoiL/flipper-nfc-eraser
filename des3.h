/**
 * Minimal, dependency-free 2-key Triple DES (DES-EDE with K1,K2,K1) in CBC mode.
 *
 * MIFARE Ultralight C mutual authentication uses 2K3DES-CBC, so the crypto has
 * to be bit-exact. The firmware's mbedtls copy is linked into the firmware but
 * is not part of the public FAP API surface, so it is implemented here instead.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DES3_KEY_SIZE   (16U)
#define DES3_BLOCK_SIZE (8U)

/** Encrypt one 8-byte block with single DES (exposed for self-tests). */
void des_encrypt_block(const uint8_t key[DES3_BLOCK_SIZE], const uint8_t in[DES3_BLOCK_SIZE], uint8_t out[DES3_BLOCK_SIZE]);

/** Decrypt one 8-byte block with single DES (exposed for self-tests). */
void des_decrypt_block(const uint8_t key[DES3_BLOCK_SIZE], const uint8_t in[DES3_BLOCK_SIZE], uint8_t out[DES3_BLOCK_SIZE]);

/**
 * @brief 2K3DES-CBC encrypt.
 *
 * @param[in]  key  16-byte key, used as K1|K2 (K3 == K1).
 * @param[in]  iv   8-byte init vector (not modified).
 * @param[in]  in   plaintext, len bytes.
 * @param[in]  len  length, must be a multiple of 8.
 * @param[out] out  ciphertext, len bytes. May alias `in`.
 * @return true on success, false on invalid length.
 */
bool des3_2key_cbc_encrypt(const uint8_t* key, const uint8_t* iv, const uint8_t* in, size_t len, uint8_t* out);

/**
 * @brief 2K3DES-CBC decrypt.
 *
 * @param[in]  key  16-byte key, used as K1|K2 (K3 == K1).
 * @param[in]  iv   8-byte init vector (not modified).
 * @param[in]  in   ciphertext, len bytes.
 * @param[in]  len  length, must be a multiple of 8.
 * @param[out] out  plaintext, len bytes. May alias `in`.
 * @return true on success, false on invalid length.
 */
bool des3_2key_cbc_decrypt(const uint8_t* key, const uint8_t* iv, const uint8_t* in, size_t len, uint8_t* out);

#ifdef __cplusplus
}
#endif
