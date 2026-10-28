#include "nfc_eraser_nfc.h"

#include <furi_hal.h>
#include <lib/nfc/helpers/iso14443_crc.h>
#include <lib/nfc/protocols/iso14443_3a/iso14443_3a.h>
#include <string.h>

#include "des3.h"

#define TAG "NfcEraserLink"

#define ERASER_FDT_POLL_FC     (1172U)
#define ERASER_FDT_POLL_US     (100U)
#define ERASER_FWT_FC          (1172U)
#define ERASER_FWT_AUTH_FC     (9400U)
#define ERASER_FWT_WRITE_FC    (135600U)

#define ERASER_CMD_READ_PAGE   (0x30U)
#define ERASER_CMD_WRITE_PAGE  (0xA2U)
#define ERASER_CMD_GET_VERSION (0x60U)
#define ERASER_CMD_AUTH        (0x1AU)
#define ERASER_CMD_HLTA        (0x50U)

#define ERASER_ULC_AUTH_RESPONSE_SIZE (9U)
#define ERASER_ULC_AUTH_RND_SIZE      (8U)
#define ERASER_ULC_AUTH_BLOCK_SIZE    (16U)

static bool eraser_trx_crc(Nfc* nfc, BitBuffer* tx, BitBuffer* rx, uint32_t fwt) {
    iso14443_crc_append(Iso14443CrcTypeA, tx);

    if(nfc_poller_trx(nfc, tx, rx, fwt) != NfcErrorNone) {
        return false;
    }
    if(!iso14443_crc_check(Iso14443CrcTypeA, rx)) {
        return false;
    }
    iso14443_crc_trim(rx);

    return true;
}

static void eraser_rotate_left(uint8_t* block, size_t len) {
    uint8_t first = block[0];
    for(size_t i = 1; i < len; i++) {
        block[i - 1] = block[i];
    }
    block[len - 1] = first;
}

bool eraser_link_ulc_auth(
    Nfc* nfc,
    BitBuffer* tx,
    BitBuffer* rx,
    const uint8_t key[ERASER_ULC_KEY_SIZE]) {
    bool authenticated = false;
    uint8_t rnd_a[ERASER_ULC_AUTH_RND_SIZE];
    uint8_t enc_rnd_b[ERASER_ULC_AUTH_RND_SIZE];
    uint8_t rnd_b[ERASER_ULC_AUTH_RND_SIZE];

    do {
        bit_buffer_reset(tx);
        bit_buffer_append_byte(tx, ERASER_CMD_AUTH);
        bit_buffer_append_byte(tx, 0x00);
        if(!eraser_trx_crc(nfc, tx, rx, ERASER_FWT_AUTH_FC)) {
            break;
        }
        if(bit_buffer_get_size_bytes(rx) != ERASER_ULC_AUTH_RESPONSE_SIZE) {
            break;
        }
        const uint8_t* response = bit_buffer_get_data(rx);
        if(response[0] != 0xAF) {
            break;
        }
        memcpy(enc_rnd_b, &response[1], sizeof(enc_rnd_b));

        if(!des3_2key_cbc_decrypt(key, (const uint8_t[8]){0}, enc_rnd_b, sizeof(enc_rnd_b), rnd_b)) {
            break;
        }
        eraser_rotate_left(rnd_b, sizeof(rnd_b));

        furi_hal_random_fill_buf(rnd_a, sizeof(rnd_a));

        uint8_t plain[ERASER_ULC_AUTH_BLOCK_SIZE];
        memcpy(&plain[0], rnd_a, sizeof(rnd_a));
        memcpy(&plain[ERASER_ULC_AUTH_RND_SIZE], rnd_b, sizeof(rnd_b));

        uint8_t cipher[ERASER_ULC_AUTH_BLOCK_SIZE];
        if(!des3_2key_cbc_encrypt(key, enc_rnd_b, plain, sizeof(plain), cipher)) {
            break;
        }

        bit_buffer_reset(tx);
        bit_buffer_append_byte(tx, 0xAF);
        bit_buffer_append_bytes(tx, cipher, sizeof(cipher));
        if(!eraser_trx_crc(nfc, tx, rx, ERASER_FWT_AUTH_FC)) {
            break;
        }
        if(bit_buffer_get_size_bytes(rx) != ERASER_ULC_AUTH_RESPONSE_SIZE) {
            break;
        }
        response = bit_buffer_get_data(rx);
        if(response[0] != 0x00) {
            break;
        }

        uint8_t decrypted[ERASER_ULC_AUTH_RND_SIZE];
        if(!des3_2key_cbc_decrypt(key, rnd_b, &response[1], sizeof(decrypted), decrypted)) {
            break;
        }

        eraser_rotate_left(rnd_a, sizeof(rnd_a));
        authenticated = (memcmp(rnd_a, decrypted, sizeof(rnd_a)) == 0);
    } while(false);

    if(!authenticated) {
        FURI_LOG_W(TAG, "Ultralight C authentication failed");
    }

    return authenticated;
}

void eraser_link_halt(Nfc* nfc, BitBuffer* tx, BitBuffer* rx) {
    bit_buffer_reset(tx);
    bit_buffer_append_byte(tx, ERASER_CMD_HLTA);
    bit_buffer_append_byte(tx, 0x00);
    eraser_trx_crc(nfc, tx, rx, ERASER_FWT_FC);
}

bool eraser_link_activate(Nfc* nfc, BitBuffer* tx, BitBuffer* rx, EraserTag* tag) {
    bool activated = false;

    bit_buffer_reset(tx);
    bit_buffer_reset(rx);

    do {
        if(nfc_iso14443a_poller_trx_short_frame(nfc, NfcIso14443aShortFrameSensReq, rx, ERASER_FWT_FC) !=
           NfcErrorNone) {
            break;
        }
        if(bit_buffer_get_size_bytes(rx) != sizeof(tag->atqa)) {
            break;
        }
        bit_buffer_write_bytes(rx, tag->atqa, sizeof(tag->atqa));

        uint8_t cascade_level = 0;
        tag->uid_len = 0;
        activated = true;

        while(true) {
            uint8_t sel = (uint8_t)(0x93 + 2 * cascade_level);

            bit_buffer_reset(tx);
            bit_buffer_set_size_bytes(tx, 2);
            bit_buffer_set_byte(tx, 0, sel);
            bit_buffer_set_byte(tx, 1, 0x20);
            if(nfc_iso14443a_poller_trx_sdd_frame(nfc, tx, rx, ERASER_FWT_FC) != NfcErrorNone) {
                activated = false;
                break;
            }
            if(bit_buffer_get_size_bytes(rx) != 5) {
                activated = false;
                break;
            }

            uint8_t nfcid[4];
            uint8_t bcc;
            bit_buffer_write_bytes(rx, nfcid, sizeof(nfcid));
            bcc = bit_buffer_get_byte(rx, 4);

            bit_buffer_reset(tx);
            bit_buffer_append_byte(tx, sel);
            bit_buffer_append_byte(tx, 0x70);
            bit_buffer_append_bytes(tx, nfcid, sizeof(nfcid));
            bit_buffer_append_byte(tx, bcc);
            if(!eraser_trx_crc(nfc, tx, rx, ERASER_FWT_FC)) {
                activated = false;
                break;
            }
            if(bit_buffer_get_size_bytes(rx) != 1) {
                activated = false;
                break;
            }
            tag->sak = bit_buffer_get_byte(rx, 0);

            if(tag->sak & 0x04) {
                if((cascade_level >= 2) || (tag->uid_len + 3 > sizeof(tag->uid))) {
                    activated = false;
                    break;
                }
                memcpy(&tag->uid[tag->uid_len], &nfcid[1], 3);
                tag->uid_len += 3;
                cascade_level++;
            } else {
                if(tag->uid_len + 4 > sizeof(tag->uid)) {
                    activated = false;
                    break;
                }
                memcpy(&tag->uid[tag->uid_len], nfcid, 4);
                tag->uid_len += 4;
                break;
            }
        }
    } while(false);

    if(!activated) {
        bit_buffer_reset(tx);
        bit_buffer_reset(rx);
    }

    return activated;
}

static bool eraser_link_read_version(Nfc* nfc, BitBuffer* tx, BitBuffer* rx, uint8_t out[8]) {
    bit_buffer_reset(tx);
    bit_buffer_append_byte(tx, ERASER_CMD_GET_VERSION);

    if(!eraser_trx_crc(nfc, tx, rx, ERASER_FWT_FC)) {
        return false;
    }
    if(bit_buffer_get_size_bytes(rx) != 8) {
        return false;
    }

    bit_buffer_write_bytes(rx, out, 8);
    return true;
}

bool eraser_link_read_page(Nfc* nfc, BitBuffer* tx, BitBuffer* rx, uint8_t page, uint8_t out[ERASER_PAGE_SIZE]) {
    bool read = false;

    do {
        bit_buffer_reset(tx);
        bit_buffer_append_byte(tx, ERASER_CMD_READ_PAGE);
        bit_buffer_append_byte(tx, page);
        if(!eraser_trx_crc(nfc, tx, rx, ERASER_FWT_FC)) {
            break;
        }
        if(bit_buffer_get_size_bytes(rx) < 4 * ERASER_PAGE_SIZE) {
            break;
        }
        bit_buffer_write_bytes(rx, out, ERASER_PAGE_SIZE);
        read = true;
    } while(false);

    return read;
}

bool eraser_link_write_page(
    Nfc* nfc,
    BitBuffer* tx,
    BitBuffer* rx,
    uint8_t page,
    const uint8_t data[ERASER_PAGE_SIZE]) {
    bool written = false;

    do {
        bit_buffer_reset(tx);
        bit_buffer_append_byte(tx, ERASER_CMD_WRITE_PAGE);
        bit_buffer_append_byte(tx, page);
        bit_buffer_append_bytes(tx, data, ERASER_PAGE_SIZE);
        iso14443_crc_append(Iso14443CrcTypeA, tx);

        if(nfc_poller_trx(nfc, tx, rx, ERASER_FWT_WRITE_FC) != NfcErrorNone) {
            break;
        }

        size_t bits = bit_buffer_get_size(rx);
        const uint8_t* raw = bit_buffer_get_data(rx);
        if(bits == 4) {
            written = ((raw[0] & 0xF0) == 0xA0);
        } else if(bits == 8) {
            written = (raw[0] == 0x0A);
        }
    } while(false);

    return written;
}

const char* eraser_tag_type_name(EraserTagType type) {
    switch(type) {
    case EraserTagTypeUltralight: return "MIFARE Ultralight";
    case EraserTagTypeUltralightC: return "MIFARE Ultralight C";
    case EraserTagTypeUltralight11: return "MIFARE Ultralight EV1 48B";
    case EraserTagTypeUltralight21: return "MIFARE Ultralight EV1 128B";
    case EraserTagTypeNTAG203: return "NTAG203";
    case EraserTagTypeNTAG213: return "NTAG213";
    case EraserTagTypeNTAG215: return "NTAG215";
    case EraserTagTypeNTAG216: return "NTAG216";
    default: return "Unknown";
    }
}

uint16_t eraser_tag_pages_total(EraserTagType type) {
    switch(type) {
    case EraserTagTypeUltralight: return 16;
    case EraserTagTypeUltralightC: return 48;
    case EraserTagTypeUltralight11: return 20;
    case EraserTagTypeUltralight21: return 41;
    case EraserTagTypeNTAG203: return 42;
    case EraserTagTypeNTAG213: return 45;
    case EraserTagTypeNTAG215: return 135;
    case EraserTagTypeNTAG216: return 231;
    default: return 0;
    }
}

uint16_t eraser_tag_user_end(EraserTagType type) {
    switch(type) {
    case EraserTagTypeUltralight:
    case EraserTagTypeUltralight11: return 16;
    case EraserTagTypeUltralightC:
    case EraserTagTypeNTAG203:
    case EraserTagTypeNTAG213: return 40;
    case EraserTagTypeUltralight21: return 36;
    case EraserTagTypeNTAG215: return 130;
    case EraserTagTypeNTAG216: return 226;
    default: return 0;
    }
}

static EraserTagType eraser_tag_type_from_version(const uint8_t version[8]) {
    switch(version[6]) {
    case 0x0B: case 0x00: return EraserTagTypeUltralight11;
    case 0x0E: return EraserTagTypeUltralight21;
    case 0x0F: return EraserTagTypeNTAG213;
    case 0x11: return EraserTagTypeNTAG215;
    case 0x13: return EraserTagTypeNTAG216;
    default: return EraserTagTypeUnknown;
    }
}

bool eraser_link_scan(Nfc* nfc, BitBuffer* tx, BitBuffer* rx, EraserTag* tag) {
    memset(tag, 0, sizeof(*tag));

    if(!eraser_link_activate(nfc, tx, rx, tag)) {
        return false;
    }

    uint8_t version[8];
    EraserTagType type = EraserTagTypeUnknown;

    if(eraser_link_read_version(nfc, tx, rx, version)) {
        type = eraser_tag_type_from_version(version);
    }

    if(type == EraserTagTypeUnknown) {
        eraser_link_halt(nfc, tx, rx);
        if(!eraser_link_activate(nfc, tx, rx, tag)) {
            return false;
        }

        bit_buffer_reset(tx);
        bit_buffer_append_byte(tx, ERASER_CMD_AUTH);
        bit_buffer_append_byte(tx, 0x00);
        if(eraser_trx_crc(nfc, tx, rx, ERASER_FWT_AUTH_FC) &&
           (bit_buffer_get_size_bytes(rx) == ERASER_ULC_AUTH_RESPONSE_SIZE) &&
           (bit_buffer_get_data(rx)[0] == 0xAF)) {
            type = EraserTagTypeUltralightC;
        }
    }

    if(type == EraserTagTypeUnknown) {
        eraser_link_halt(nfc, tx, rx);
        if(!eraser_link_activate(nfc, tx, rx, tag)) {
            return false;
        }

        uint8_t scratch[ERASER_PAGE_SIZE];
        if(eraser_link_read_page(nfc, tx, rx, 41, scratch)) {
            type = EraserTagTypeNTAG203;
        } else {
            type = EraserTagTypeUltralight;
        }
    }

    tag->type = type;
    tag->pages_total = eraser_tag_pages_total(type);
    tag->user_end = eraser_tag_user_end(type);

    eraser_link_halt(nfc, tx, rx);
    eraser_link_activate(nfc, tx, rx, tag);

    FURI_LOG_I(TAG, "Tag: %s, %u pages, UID len %u", eraser_tag_type_name(type), tag->pages_total, (unsigned)tag->uid_len);

    return (type != EraserTagTypeUnknown);
}
