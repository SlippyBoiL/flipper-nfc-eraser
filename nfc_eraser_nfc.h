/**
 * MIFARE Ultralight / Ultralight C protocol layer for the NFC Eraser app.
 *
 * Everything here is called from inside the Nfc poller callback (see
 * nfc_eraser_app.c), which is the only context where furi/nfc allows
 * transactions. The transport (field handling, timers, HAL state machine) is
 * the firmware's own lib/nfc, so only the Ultralight specific parts are here.
 */
#pragma once

#include <furi.h>
#include <lib/nfc/nfc.h>

#define ERASER_PAGE_SIZE      (4U)
#define ERASER_MAX_PAGES      (256U)
#define ERASER_UID_MAX_LEN    (10U)
#define ERASER_ULC_KEY_SIZE   (16U)
#define ERASER_ULC_KEY_FIRST_PAGE (44U)

/** Tag families this app knows how to identify. */
typedef enum {
    EraserTagTypeUnknown,
    EraserTagTypeUltralight, /**< MF0ICU1, 16 pages */
    EraserTagTypeUltralightC, /**< MF0ICU2, 48 pages, 3DES */
    EraserTagTypeUltralight11, /**< MF0ICU1 v1.1 / UL EV1 48B */
    EraserTagTypeUltralight21, /**< UL EV1 128B */
    EraserTagTypeNTAG203,
    EraserTagTypeNTAG213,
    EraserTagTypeNTAG215,
    EraserTagTypeNTAG216,
} EraserTagType;

typedef struct {
    EraserTagType type;
    uint8_t uid[ERASER_UID_MAX_LEN];
    size_t uid_len;
    uint8_t atqa[2];
    uint8_t sak;
    uint16_t pages_total; /**< Addressable pages, i.e. UID + data + config + key. */
    uint16_t user_end; /**< One past the last user data page (first page is 4). */
} EraserTag;

/** 2K3DES-CBC mutual authentication against an Ultralight C. */
bool eraser_link_ulc_auth(
    Nfc* nfc,
    BitBuffer* tx,
    BitBuffer* rx,
    const uint8_t key[ERASER_ULC_KEY_SIZE]);

/** Read one 4-byte page (uses the 0x30 command, 16-byte response, first page kept). */
bool eraser_link_read_page(Nfc* nfc, BitBuffer* tx, BitBuffer* rx, uint8_t page, uint8_t out[ERASER_PAGE_SIZE]);

/** Write one 4-byte page (0xA2). Returns true only on a valid 0x0A ACK. */
bool eraser_link_write_page(
    Nfc* nfc,
    BitBuffer* tx,
    BitBuffer* rx,
    uint8_t page,
    const uint8_t data[ERASER_PAGE_SIZE]);

/** REQA + anticollision + SELECT, filling in UID/ATQA/SAK. */
bool eraser_link_activate(Nfc* nfc, BitBuffer* tx, BitBuffer* rx, EraserTag* tag);

/**
 * @brief Activate a card and work out what it is.
 *
 * Probes GET_VERSION first (NTAG/UL EV1), then the Ultralight C authenticate
 * command, then page 41 (NTAG203), and finally assumes a plain Ultralight.
 * The card is left selected on success.
 */
bool eraser_link_scan(Nfc* nfc, BitBuffer* tx, BitBuffer* rx, EraserTag* tag);

/** Send HLTA so the card returns to IDLE and a new REQA is answered. */
void eraser_link_halt(Nfc* nfc, BitBuffer* tx, BitBuffer* rx);

const char* eraser_tag_type_name(EraserTagType type);
uint16_t eraser_tag_pages_total(EraserTagType type);
uint16_t eraser_tag_user_end(EraserTagType type);
