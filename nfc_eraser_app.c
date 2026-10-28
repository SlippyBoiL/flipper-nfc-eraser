#include "nfc_eraser_app.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <furi_hal_nfc.h>

#define TAG "NfcEraser"
#define NFC_ERASER_BUILD "1.2"

/** First page of user memory on every Ultralight/NTAG derivative. */
#define NFC_ERASER_FIRST_USER_PAGE (4U)

/* Seed keys used when keys.txt is missing. The first one is the factory
 * Ultralight C key, the other two are common "blank" keys. */
static const uint8_t nfc_eraser_seed_keys[][ERASER_ULC_KEY_SIZE] = {
    {'B', 'R', 'E', 'A', 'K', 'M', 'E', 'I', 'F', 'Y', 'O', 'U', 'C', 'A', 'N', '!'},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
};

/* ------------------------------------------------------------------------- */
/* Crash diagnostics                                                         */
/* ------------------------------------------------------------------------- */

/**
 * Appends one line to last_run.log and flushes it immediately. A firmware
 * assertion halts the system (no exception handler runs in the app), so the
 * last line in this file is the exact stage that was executing when it died.
 */
static void nfc_eraser_diag(NfcEraserApp* app, const char* fmt, ...) {
    if(app->diag == NULL) return;

    char line[128];
    int used = snprintf(line, sizeof(line), "%lu ", (unsigned long)furi_get_tick());
    if(used <= 0 || (size_t)used >= sizeof(line)) return;

    va_list args;
    va_start(args, fmt);
    int added = vsnprintf(line + used, sizeof(line) - (size_t)used, fmt, args);
    va_end(args);
    if(added <= 0) return;

    size_t len = strlen(line);
    if(len + 2 > sizeof(line)) len = sizeof(line) - 2;
    line[len++] = '\n';
    line[len] = '\0';

    storage_file_write(app->diag, line, len);
    storage_file_sync(app->diag);
}

static void nfc_eraser_diag_open(NfcEraserApp* app) {
    storage_common_mkdir(app->storage, NFC_ERASER_APP_DIR);

    app->diag = storage_file_alloc(app->storage);
    if(!storage_file_open(app->diag, NFC_ERASER_LOG_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(app->diag);
        app->diag = NULL;
        return;
    }

    nfc_eraser_diag(app, "=== NFC Eraser %s start ===", NFC_ERASER_BUILD);
}

static void nfc_eraser_diag_close(NfcEraserApp* app) {
    if(app->diag == NULL) return;
    nfc_eraser_diag(app, "exit: clean shutdown");
    storage_file_close(app->diag);
    storage_file_free(app->diag);
    app->diag = NULL;
}

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static void nfc_eraser_hex(const uint8_t* data, size_t len, FuriString* out, bool spaced) {
    for(size_t i = 0; i < len; i++) {
        furi_string_cat_printf(out, spaced ? "%02X " : "%02X", data[i]);
    }
}

static uint8_t nfc_eraser_hex_nibble(char c) {
    if(c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if(c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    if(c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    return 0xFF;
}

static bool nfc_eraser_parse_key(const char* line, uint8_t out[ERASER_ULC_KEY_SIZE]) {
    size_t digits = 0;
    for(const char* p = line; *p && *p != ' ' && *p != '\t' && *p != '#'; p++) {
        uint8_t nibble = nfc_eraser_hex_nibble(*p);
        if(nibble == 0xFF) return false;
        if(digits >= ERASER_ULC_KEY_SIZE * 2) return false;
        if((digits % 2) == 0) {
            out[digits / 2] = (uint8_t)(nibble << 4);
        } else {
            out[digits / 2] |= nibble;
        }
        digits++;
    }
    return digits == ERASER_ULC_KEY_SIZE * 2;
}

static bool nfc_eraser_load_keys(NfcEraserApp* app) {
    app->key_count = 0;

    File* file = storage_file_alloc(app->storage);
    bool loaded = false;
    size_t file_size = 0;

    if(storage_file_open(file, NFC_ERASER_KEYS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        file_size = storage_file_size(file);
    }

    if(file_size > 0 && file_size < 4096) {
        char* buffer = malloc(file_size + 1);
        size_t read = storage_file_read(file, buffer, file_size);
        buffer[read] = '\0';
        storage_file_close(file);

        char* line = buffer;
        while(line && *line && app->key_count < NFC_ERASER_MAX_KEYS) {
            char* end = strchr(line, '\n');
            if(end) *end = '\0';
            size_t len = strlen(line);
            if(len && line[len - 1] == '\r') line[len - 1] = '\0';

            if(*line && *line != '#' &&
               nfc_eraser_parse_key(line, app->keys[app->key_count])) {
                app->key_count++;
            }
            line = end ? end + 1 : NULL;
        }

        free(buffer);
        loaded = (app->key_count > 0);
    } else {
        storage_file_close(file);
    }
    storage_file_free(file);

    if(!loaded) {
        /* No usable key file: seed it so the user has something to edit. */
        app->key_count = COUNT_OF(nfc_eraser_seed_keys);
        memcpy(app->keys, nfc_eraser_seed_keys, sizeof(nfc_eraser_seed_keys));

        storage_common_mkdir(app->storage, NFC_ERASER_APP_DIR);
        File* out = storage_file_alloc(app->storage);
        if(storage_file_open(out, NFC_ERASER_KEYS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            const char* header =
                "# NFC Eraser 3DES keys, one 32-hex-digit key per line\n"
                "# First key is the default for 'set key'. Re-enter the app to reload.\n";
            storage_file_write(out, header, strlen(header));
            for(size_t i = 0; i < app->key_count; i++) {
                FuriString* line = furi_string_alloc();
                nfc_eraser_hex(app->keys[i], ERASER_ULC_KEY_SIZE, line, false);
                furi_string_cat_str(line, "\n");
                storage_file_write(out, furi_string_get_cstr(line), furi_string_size(line));
                furi_string_free(line);
            }
            storage_file_close(out);
        }
        storage_file_free(out);
    }

    FURI_LOG_I(TAG, "Loaded %u key(s)", (unsigned)app->key_count);
    return app->key_count > 0;
}

/* ------------------------------------------------------------------------- */
/* NFC worker                                                                */
/* ------------------------------------------------------------------------- */

/** Read every addressable page into app->pages (best effort). */
static uint16_t nfc_eraser_read_tag(NfcEraserApp* app, Nfc* nfc, BitBuffer* tx, BitBuffer* rx) {
    uint16_t read = 0;
    uint16_t total = app->tag.pages_total;
    if(total > ERASER_MAX_PAGES) total = ERASER_MAX_PAGES;

    memset(app->page_valid, 0, sizeof(app->page_valid));
    memset(app->pages, 0xFF, sizeof(app->pages));

    for(uint16_t page = 0; page < total; page++) {
        if(eraser_link_read_page(nfc, tx, rx, (uint8_t)page, app->pages[page])) {
            app->page_valid[page] = true;
            read++;
        }
        app->progress.page_current = page;
    }

    FURI_LOG_I(TAG, "Read %u/%u pages", read, total);
    return read;
}

/** Try every known key; a failed attempt deselects the card, so re-select each time. */
static bool nfc_eraser_try_auth(NfcEraserApp* app, Nfc* nfc, BitBuffer* tx, BitBuffer* rx) {
    if(app->tag.type != EraserTagTypeUltralightC) {
        return false;
    }

    for(size_t i = 0; i < app->key_count; i++) {
        if(app->abort_requested) break;
        if(!eraser_link_activate(nfc, tx, rx, &app->tag)) return false;
        if(eraser_link_ulc_auth(nfc, tx, rx, app->keys[i])) {
            app->active_key = (int)i;
            app->authenticated = true;
            FURI_LOG_I(TAG, "3DES auth OK with key %u", (unsigned)i);
            return true;
        }
    }

    /* Leave the card selected so unauthenticated reads/writes can still be tried. */
    eraser_link_activate(nfc, tx, rx, &app->tag);
    app->authenticated = false;
    return false;
}

/** Write a 16-byte 3DES key into pages 44..47 and verify it by re-authenticating. */
static bool nfc_eraser_write_ulc_key(
    NfcEraserApp* app,
    Nfc* nfc,
    BitBuffer* tx,
    BitBuffer* rx,
    const uint8_t key[ERASER_ULC_KEY_SIZE]) {
    bool written = true;

    for(uint16_t i = 0; i < ERASER_ULC_KEY_SIZE / ERASER_PAGE_SIZE; i++) {
        if(!eraser_link_write_page(
               nfc, tx, rx, (uint8_t)(ERASER_ULC_KEY_FIRST_PAGE + i), &key[i * ERASER_PAGE_SIZE])) {
            written = false;
            break;
        }
    }

    /* The key pages can never be read back, so prove the key by using it. */
    eraser_link_halt(nfc, tx, rx);
    if(!eraser_link_activate(nfc, tx, rx, &app->tag)) {
        return false;
    }
    bool verified = eraser_link_ulc_auth(nfc, tx, rx, key);

    return written && verified;
}

static void nfc_eraser_op_scan(NfcEraserApp* app, Nfc* nfc, BitBuffer* tx, BitBuffer* rx) {
    app->tag_present = false;
    app->authenticated = false;

    while(!app->abort_requested) {
        if(eraser_link_scan(nfc, tx, rx, &app->tag)) {
            app->tag_present = true;
            nfc_eraser_read_tag(app, nfc, tx, rx);
            nfc_eraser_try_auth(app, nfc, tx, rx);
            notification_message(app->notifications, &sequence_success);
            return;
        }
        furi_delay_ms(80);
    }
}

static void nfc_eraser_op_wipe(NfcEraserApp* app, Nfc* nfc, BitBuffer* tx, BitBuffer* rx) {
    const uint8_t zero[ERASER_PAGE_SIZE] = {0, 0, 0, 0};
    uint16_t from = NFC_ERASER_FIRST_USER_PAGE;
    uint16_t to = app->tag.user_end;

    app->progress.page_from = from;
    app->progress.page_to = to;

    if(to <= from) {
        snprintf(app->worker_message, sizeof(app->worker_message), "Tag has no writable user pages");
        return;
    }

    for(uint16_t page = from; page < to; page++) {
        if(app->abort_requested) {
            app->progress.aborted = true;
            break;
        }

        uint8_t current[ERASER_PAGE_SIZE];
        bool need_write = true;

        if(eraser_link_read_page(nfc, tx, rx, (uint8_t)page, current)) {
            if((current[0] | current[1] | current[2] | current[3]) == 0) {
                need_write = false;
            }
        }

        if(!need_write) {
            app->progress.skipped++;
        } else if(eraser_link_write_page(nfc, tx, rx, (uint8_t)page, zero)) {
            app->progress.written++;
        } else if(!app->authenticated && nfc_eraser_try_auth(app, nfc, tx, rx)) {
            app->progress.authenticated = true;
            if(eraser_link_write_page(nfc, tx, rx, (uint8_t)page, zero)) {
                app->progress.written++;
            } else {
                app->progress.failed++;
            }
        } else {
            app->progress.failed++;
        }

        app->progress.page_current = page;
        view_dispatcher_send_custom_event(app->view_dispatcher, NfcEraserEventProgress);
    }

    /* Verify with an independent pass over the same range. */
    for(uint16_t page = from; page < to; page++) {
        if(app->abort_requested) break;
        uint8_t current[ERASER_PAGE_SIZE];
        if(eraser_link_read_page(nfc, tx, rx, (uint8_t)page, current)) {
            if((current[0] | current[1] | current[2] | current[3]) == 0) {
                app->progress.verified++;
            }
        }
        app->progress.page_current = page;
        view_dispatcher_send_custom_event(app->view_dispatcher, NfcEraserEventProgress);
    }

    /* Success only if nothing failed, nothing was aborted and every page in the
     * range now reads back as zero. */
    uint16_t expected = (uint16_t)(to - from);
    app->worker_ok = !app->progress.aborted && (app->progress.failed == 0) &&
                     (app->progress.verified == expected);
}

static void nfc_eraser_op_rekey(NfcEraserApp* app, Nfc* nfc, BitBuffer* tx, BitBuffer* rx) {
    if(app->tag.type != EraserTagTypeUltralightC) {
        snprintf(app->worker_message, sizeof(app->worker_message), "3DES key is Ultralight C only");
        return;
    }

    const uint8_t* key = app->keys[app->selected_key];
    bool key_ok = nfc_eraser_write_ulc_key(app, nfc, tx, rx, key);
    if(key_ok) {
        app->active_key = (int)app->selected_key;
        app->authenticated = true;
        snprintf(app->worker_message, sizeof(app->worker_message), "3DES key updated and verified");
    } else {
        snprintf(
            app->worker_message,
            sizeof(app->worker_message),
            "Could not set key (need current key)");
    }

    /* Keep the wipe verdict when this runs as part of "erase + set key". */
    app->worker_ok = app->worker_ok && key_ok;
}

static void nfc_eraser_run_operation(NfcEraserApp* app, Nfc* nfc, BitBuffer* tx, BitBuffer* rx) {
    switch(app->pending_op) {
    case NfcEraserOpScan:
        nfc_eraser_op_scan(app, nfc, tx, rx);
        break;
    case NfcEraserOpWipe:
    case NfcEraserOpWipeRekey:
        if(!eraser_link_scan(nfc, tx, rx, &app->tag)) {
            snprintf(app->worker_message, sizeof(app->worker_message), "Tag removed");
            app->tag_present = false;
            return;
        }
        nfc_eraser_read_tag(app, nfc, tx, rx);
        nfc_eraser_try_auth(app, nfc, tx, rx);
        nfc_eraser_op_wipe(app, nfc, tx, rx);
        if(app->pending_op == NfcEraserOpWipeRekey) {
            eraser_link_halt(nfc, tx, rx);
            if(eraser_link_activate(nfc, tx, rx, &app->tag)) {
                nfc_eraser_op_rekey(app, nfc, tx, rx);
            }
        }
        break;
    case NfcEraserOpRekey:
        if(!eraser_link_scan(nfc, tx, rx, &app->tag)) {
            snprintf(app->worker_message, sizeof(app->worker_message), "Tag removed");
            app->tag_present = false;
            return;
        }
        nfc_eraser_read_tag(app, nfc, tx, rx);
        nfc_eraser_try_auth(app, nfc, tx, rx);
        eraser_link_halt(nfc, tx, rx);
        app->worker_ok = true;
        if(eraser_link_activate(nfc, tx, rx, &app->tag)) {
            nfc_eraser_op_rekey(app, nfc, tx, rx);
        } else {
            app->worker_ok = false;
            snprintf(app->worker_message, sizeof(app->worker_message), "Tag removed");
        }
        break;
    default:
        break;
    }
}

static NfcCommand nfc_eraser_nfc_callback(NfcEvent event, void* context) {
    NfcEraserApp* app = context;

    nfc_eraser_diag(app, "cb: event=%d", (int)event.type);

    if(event.type == NfcEventTypePollerReady) {
        BitBuffer* tx = bit_buffer_alloc(64);
        BitBuffer* rx = bit_buffer_alloc(64);

        nfc_eraser_run_operation(app, app->nfc, tx, rx);
        nfc_eraser_diag(app, "cb: op done tag=%d", (int)app->tag_present);

        bit_buffer_free(rx);
        bit_buffer_free(tx);
    }

    /* Signals the worker that the poller is done with the radio, so it may
     * call nfc_stop() without racing the worker thread's start-up. */
    furi_event_flag_set(app->nfc_done, 1);
    return NfcCommandStop;
}

static bool nfc_eraser_save_dump(NfcEraserApp* app) {
    storage_common_mkdir(app->storage, NFC_ERASER_APP_DIR);
    storage_common_mkdir(app->storage, NFC_ERASER_DUMP_DIR);

    FuriString* path = furi_string_alloc();
    furi_string_printf(path, "%s/", NFC_ERASER_DUMP_DIR);
    for(size_t i = 0; i < app->tag.uid_len; i++) {
        furi_string_cat_printf(path, "%02X", app->tag.uid[i]);
    }
    furi_string_cat_str(path, ".txt");

    FuriString* text = furi_string_alloc();
    furi_string_printf(text, "NFC Eraser dump\n");
    furi_string_cat_printf(text, "Type: %s\n", eraser_tag_type_name(app->tag.type));

    furi_string_cat_str(text, "UID: ");
    nfc_eraser_hex(app->tag.uid, app->tag.uid_len, text, true);
    furi_string_cat_str(text, "\nATQA: ");
    nfc_eraser_hex(app->tag.atqa, sizeof(app->tag.atqa), text, true);
    furi_string_cat_printf(text, "\nSAK: %02X\n", app->tag.sak);
    furi_string_cat_printf(
        text,
        "Pages: %u total, user 4-%u\n",
        app->tag.pages_total,
        (unsigned)(app->tag.user_end - 1));

    if(app->tag.type == EraserTagTypeUltralightC) {
        if(app->authenticated) {
            furi_string_cat_str(text, "3DES: authenticated, key ");
            FuriString* key = furi_string_alloc();
            nfc_eraser_hex(app->keys[app->active_key], ERASER_ULC_KEY_SIZE, key, false);
            furi_string_cat_printf(text, "%s\n", furi_string_get_cstr(key));
            furi_string_free(key);
        } else {
            furi_string_cat_str(text, "3DES: not authenticated\n");
        }
    }

    furi_string_cat_str(text, "\n");
    for(uint16_t page = 0; page < app->tag.pages_total && page < ERASER_MAX_PAGES; page++) {
        furi_string_cat_printf(text, "Page %02X: ", page);
        if(app->page_valid[page]) {
            nfc_eraser_hex(app->pages[page], ERASER_PAGE_SIZE, text, true);
        } else {
            furi_string_cat_str(text, "?? ?? ?? ?? (unreadable)");
        }
        furi_string_cat_str(text, "\n");
    }

    bool saved = false;
    File* file = storage_file_alloc(app->storage);
    if(storage_file_open(file, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        saved = storage_file_write(file, furi_string_get_cstr(text), furi_string_size(text)) ==
                furi_string_size(text);
        storage_file_close(file);
    }
    storage_file_free(file);

    snprintf(
        app->worker_message,
        sizeof(app->worker_message),
        saved ? "Saved %s" : "Could not save dump",
        furi_string_get_cstr(path));

    furi_string_free(text);
    furi_string_free(path);

    return saved;
}

static int32_t nfc_eraser_worker(void* context) {
    NfcEraserApp* app = context;

    nfc_eraser_diag(app, "worker: start op=%d", (int)app->pending_op);

    if(app->pending_op == NfcEraserOpSaveDump) {
        app->worker_ok = nfc_eraser_save_dump(app);
        view_dispatcher_send_custom_event(app->view_dispatcher, NfcEraserEventWorkerDone);
        return 0;
    }

    nfc_eraser_diag(app, "worker: hal probe");
    FuriHalNfcError hal = furi_hal_nfc_is_hal_ready();
    nfc_eraser_diag(app, "worker: hal probe=%d", (int)hal);
    if(hal != FuriHalNfcErrorNone) {
        snprintf(
            app->worker_message,
            sizeof(app->worker_message),
            "NFC hardware not ready (%d)",
            (int)hal);
        app->worker_ok = false;
        view_dispatcher_send_custom_event(app->view_dispatcher, NfcEraserEventWorkerDone);
        return 0;
    }

    nfc_eraser_diag(app, "worker: nfc_alloc");
    Nfc* nfc = nfc_alloc();
    nfc_eraser_diag(app, "worker: nfc_alloc ok");
    app->nfc = nfc;

    nfc_eraser_diag(app, "worker: config");
    nfc_config(nfc, NfcModePoller, NfcTechIso14443a);
    nfc_eraser_diag(app, "worker: config ok");
    nfc_set_fdt_listen_fc(nfc, 1172);
    nfc_set_fdt_poll_fc(nfc, 1172);
    nfc_set_fdt_poll_poll_us(nfc, 100);

    /* nfc_start() only launches the poller thread; it does not wait for the
     * callback. nfc_stop() asserts that the poller is running, so wait for our
     * callback to finish (it signals nfc_done) before stopping. */
    furi_event_flag_clear(app->nfc_done, 1);
    nfc_eraser_diag(app, "worker: nfc_start");
    nfc_start(nfc, nfc_eraser_nfc_callback, app);
    nfc_eraser_diag(app, "worker: waiting for poller");
    furi_event_flag_wait(app->nfc_done, 1, FuriFlagWaitAny, 30000);
    nfc_eraser_diag(app, "worker: nfc_stop");
    nfc_stop(nfc);
    nfc_eraser_diag(app, "worker: nfc_stop ok");

    app->nfc = NULL;
    nfc_free(nfc);
    nfc_eraser_diag(app, "worker: nfc_free ok");

    view_dispatcher_send_custom_event(app->view_dispatcher, NfcEraserEventWorkerDone);
    return 0;
}

static void nfc_eraser_start_worker(NfcEraserApp* app, NfcEraserOp op) {
    if(app->worker_running) {
        FURI_LOG_W(TAG, "Worker already running");
        return;
    }

    app->pending_op = op;
    app->abort_requested = false;
    app->worker_ok = false;
    app->worker_message[0] = '\0';
    memset(&app->progress, 0, sizeof(app->progress));
    app->progress.op = op;
    app->worker_running = true;

    furi_thread_start(app->worker);
}

/* ------------------------------------------------------------------------- */
/* Scenes                                                                    */
/* ------------------------------------------------------------------------- */

static void nfc_eraser_scene_scan_on_enter(void* context) {
    NfcEraserApp* app = context;

    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 14, AlignCenter, AlignTop, FontPrimary, "NFC Eraser");
    widget_add_string_multiline_element(
        app->widget,
        64,
        32,
        AlignCenter,
        AlignTop,
        FontSecondary,
        "Hold an Ultralight tag\nto the back of the Flipper\n\nBack: exit");

    view_dispatcher_switch_to_view(app->view_dispatcher, NfcEraserViewWidget);
    nfc_eraser_start_worker(app, NfcEraserOpScan);
}

static bool nfc_eraser_scene_scan_on_event(void* context, SceneManagerEvent event) {
    NfcEraserApp* app = context;

    if(event.type != SceneManagerEventTypeCustom || event.event != NfcEraserEventWorkerDone) {
        return false;
    }

    if(app->tag_present) {
        scene_manager_next_scene(app->scene_manager, NfcEraserSceneInfo);
        return true;
    }

    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 14, AlignCenter, AlignTop, FontPrimary, "NFC Eraser");
    widget_add_string_multiline_element(
        app->widget,
        64,
        34,
        AlignCenter,
        AlignTop,
        FontSecondary,
        "No tag found.\nPress OK to scan again");

    notification_message(app->notifications, &sequence_error);

    return true;
}

static void nfc_eraser_scene_info_on_enter(void* context) {
    NfcEraserApp* app = context;
    EraserTag* tag = &app->tag;

    FuriString* text = app->text;
    furi_string_reset(text);

    furi_string_printf(text, "%s\n", eraser_tag_type_name(tag->type));

    furi_string_cat_str(text, "UID: ");
    nfc_eraser_hex(tag->uid, tag->uid_len, text, true);
    furi_string_cat_printf(text, "\nPages: %u  user: 4-%u\n", tag->pages_total, tag->user_end - 1);

    if(tag->type == EraserTagTypeUltralightC) {
        if(app->authenticated) {
            furi_string_cat_printf(text, "3DES: key %u OK\n", app->active_key);
        } else {
            furi_string_cat_str(text, "3DES: no key\n");
        }
    } else {
        furi_string_cat_str(text, "3DES: not used\n");
    }

    if(app->page_valid[2] && app->page_valid[3]) {
        furi_string_cat_printf(
            text,
            "Lock: %02X%02X OTP %02X%02X\n",
            app->pages[2][2],
            app->pages[2][3],
            app->pages[3][0],
            app->pages[3][1]);
    }

    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 0, 1, AlignLeft, AlignTop, FontPrimary, "Tag detected");
    widget_add_string_multiline_element(
        app->widget, 0, 14, AlignLeft, AlignTop, FontSecondary, furi_string_get_cstr(text));
    widget_add_string_element(
        app->widget, 64, 54, AlignCenter, AlignTop, FontSecondary, "OK: actions");

    view_dispatcher_switch_to_view(app->view_dispatcher, NfcEraserViewWidget);
}

static bool nfc_eraser_scene_info_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

/* --- actions ------------------------------------------------------------- */

enum {
    NfcEraserActionWipe,
    NfcEraserActionRekey,
    NfcEraserActionWipeRekey,
    NfcEraserActionChooseKey,
    NfcEraserActionSaveDump,
    NfcEraserActionRescan,
    NfcEraserActionAbout,
};

static void nfc_eraser_submenu_callback(void* context, uint32_t index) {
    NfcEraserApp* app = context;

    switch(index) {
    case NfcEraserActionWipe:
        app->confirm_op = NfcEraserOpWipe;
        scene_manager_next_scene(app->scene_manager, NfcEraserSceneConfirm);
        break;
    case NfcEraserActionRekey:
        app->key_select_for_rekey = true;
        scene_manager_next_scene(app->scene_manager, NfcEraserSceneKeySelect);
        break;
    case NfcEraserActionWipeRekey:
        app->confirm_op = NfcEraserOpWipeRekey;
        scene_manager_next_scene(app->scene_manager, NfcEraserSceneConfirm);
        break;
    case NfcEraserActionChooseKey:
        app->key_select_for_rekey = false;
        scene_manager_next_scene(app->scene_manager, NfcEraserSceneKeySelect);
        break;
    case NfcEraserActionSaveDump:
        app->pending_op = NfcEraserOpSaveDump;
        nfc_eraser_start_worker(app, NfcEraserOpSaveDump);
        scene_manager_next_scene(app->scene_manager, NfcEraserSceneResult);
        break;
    case NfcEraserActionRescan:
        scene_manager_search_and_switch_to_previous_scene(app->scene_manager, NfcEraserSceneScan);
        break;
    case NfcEraserActionAbout:
        scene_manager_next_scene(app->scene_manager, NfcEraserSceneAbout);
        break;
    default:
        break;
    }
}

static void nfc_eraser_scene_actions_on_enter(void* context) {
    NfcEraserApp* app = context;

    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "NFC Eraser");

    submenu_add_item(app->submenu, "Erase memory", NfcEraserActionWipe, nfc_eraser_submenu_callback, app);

    FuriString* label = furi_string_alloc();
    furi_string_printf(label, "Set 3DES key (key %u)", (unsigned)app->selected_key);
    submenu_add_item(
        app->submenu, furi_string_get_cstr(label), NfcEraserActionRekey, nfc_eraser_submenu_callback, app);
    furi_string_free(label);

    submenu_add_item(
        app->submenu, "Erase + set key", NfcEraserActionWipeRekey, nfc_eraser_submenu_callback, app);
    submenu_add_item(
        app->submenu, "Choose 3DES key", NfcEraserActionChooseKey, nfc_eraser_submenu_callback, app);
    submenu_add_item(
        app->submenu, "Save dump to SD", NfcEraserActionSaveDump, nfc_eraser_submenu_callback, app);
    submenu_add_item(app->submenu, "Re-scan tag", NfcEraserActionRescan, nfc_eraser_submenu_callback, app);
    submenu_add_item(app->submenu, "About", NfcEraserActionAbout, nfc_eraser_submenu_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, NfcEraserViewSubmenu);
}

/* --- key selection ------------------------------------------------------- */

static void nfc_eraser_scene_key_select_on_enter(void* context) {
    NfcEraserApp* app = context;

    variable_item_list_reset(app->var_list);

    for(size_t i = 0; i < app->key_count; i++) {
        FuriString* label = furi_string_alloc_printf("Key %u", (unsigned)i);
        VariableItem* item = variable_item_list_add(app->var_list, furi_string_get_cstr(label), 0, NULL, NULL);
        furi_string_free(label);

        FuriString* value = furi_string_alloc();
        nfc_eraser_hex(app->keys[i], 8, value, false);
        furi_string_cat_str(value, "..");
        variable_item_set_current_value_text(item, furi_string_get_cstr(value));
        furi_string_free(value);
    }

    variable_item_list_set_selected_item(app->var_list, (uint32_t)app->selected_key);
    view_dispatcher_switch_to_view(app->view_dispatcher, NfcEraserViewVarList);
}

static void nfc_eraser_key_select_callback(void* context, uint32_t index) {
    NfcEraserApp* app = context;

    if(index < app->key_count) {
        app->selected_key = index;
    }

    if(app->key_select_for_rekey) {
        app->confirm_op = NfcEraserOpRekey;
        scene_manager_next_scene(app->scene_manager, NfcEraserSceneConfirm);
    } else {
        scene_manager_previous_scene(app->scene_manager);
    }
}

/* --- confirmation -------------------------------------------------------- */

static void nfc_eraser_scene_confirm_on_enter(void* context) {
    NfcEraserApp* app = context;

    FuriString* text = furi_string_alloc();

    switch(app->confirm_op) {
    case NfcEraserOpWipe:
        furi_string_printf(
            text,
            "Write zeros to pages 4-%u\nof this %s?\n\nThis cannot be undone.",
            app->tag.user_end - 1,
            eraser_tag_type_name(app->tag.type));
        break;
    case NfcEraserOpWipeRekey:
        furi_string_printf(
            text,
            "Erase pages 4-%u and set\n3DES key to key %u?\n\nThis cannot be undone.",
            app->tag.user_end - 1,
            (unsigned)app->selected_key);
        break;
    case NfcEraserOpRekey:
        furi_string_printf(
            text,
            "Write 3DES key %u to pages 44-47?\n\nTag must be Ultralight C and\nthe current key must be known.",
            (unsigned)app->selected_key);
        break;
    default:
        furi_string_printf(text, "Run operation?");
        break;
    }

    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 0, 1, AlignLeft, AlignTop, FontPrimary, "Confirm");
    widget_add_string_multiline_element(
        app->widget, 0, 14, AlignLeft, AlignTop, FontSecondary, furi_string_get_cstr(text));
    widget_add_string_element(
        app->widget, 64, 54, AlignCenter, AlignTop, FontSecondary, "OK: run   Back: cancel");

    furi_string_free(text);

    view_dispatcher_switch_to_view(app->view_dispatcher, NfcEraserViewWidget);
}

/* --- progress ------------------------------------------------------------ */

static void nfc_eraser_progress_update(NfcEraserApp* app) {
    NfcEraserProgress* progress = &app->progress;
    FuriString* text = furi_string_alloc();

    uint16_t total = progress->page_to > progress->page_from ?
                         (uint16_t)(progress->page_to - progress->page_from) :
                         0;
    uint16_t done = progress->page_current >= progress->page_from ?
                        (uint16_t)(progress->page_current - progress->page_from + 1) :
                        0;

    furi_string_printf(text, "Page %u/%u\n", done, total);
    furi_string_cat_printf(text, "Written: %u\n", progress->written);
    furi_string_cat_printf(text, "Skipped: %u\n", progress->skipped);
    furi_string_cat_printf(text, "Failed: %u\n", progress->failed);
    furi_string_cat_printf(text, "Verified: %u\n", progress->verified);
    furi_string_cat_str(text, app->abort_requested ? "\nAborting..." : "\nBack: abort");

    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 0, 1, AlignLeft, AlignTop, FontPrimary, "Working...");
    widget_add_string_multiline_element(
        app->widget, 0, 14, AlignLeft, AlignTop, FontSecondary, furi_string_get_cstr(text));

    furi_string_free(text);
}

static void nfc_eraser_scene_progress_on_enter(void* context) {
    NfcEraserApp* app = context;

    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 0, 1, AlignLeft, AlignTop, FontPrimary, "Working...");
    widget_add_string_element(
        app->widget, 0, 26, AlignLeft, AlignTop, FontSecondary, "Starting...");

    view_dispatcher_switch_to_view(app->view_dispatcher, NfcEraserViewWidget);
}

static bool nfc_eraser_scene_progress_on_event(void* context, SceneManagerEvent event) {
    NfcEraserApp* app = context;

    if(event.type != SceneManagerEventTypeCustom) {
        return false;
    }

    if(event.event == NfcEraserEventProgress) {
        nfc_eraser_progress_update(app);
    } else if(event.event == NfcEraserEventWorkerDone) {
        scene_manager_next_scene(app->scene_manager, NfcEraserSceneResult);
    }

    return true;
}

/* --- result -------------------------------------------------------------- */

static void nfc_eraser_scene_result_on_enter(void* context) {
    NfcEraserApp* app = context;
    NfcEraserProgress* progress = &app->progress;

    FuriString* text = furi_string_alloc();

    switch(app->pending_op) {
    case NfcEraserOpSaveDump:
        furi_string_printf(text, "%s", app->worker_message[0] ? app->worker_message : "Done");
        break;
    case NfcEraserOpWipe:
    case NfcEraserOpWipeRekey:
        furi_string_printf(
            text,
            "Written: %u\nSkipped: %u\nFailed: %u\nVerified: %u\n",
            progress->written,
            progress->skipped,
            progress->failed,
            progress->verified);
        if(progress->aborted) {
            furi_string_cat_str(text, "\nAborted - tag partly erased.");
        } else if(progress->failed) {
            furi_string_cat_str(text, "\nSome pages are locked or\nneed the correct key.");
        } else {
            furi_string_cat_str(text, "\nMemory erased.");
        }
        if(app->pending_op == NfcEraserOpWipeRekey && app->worker_message[0]) {
            furi_string_cat_printf(text, "\n%s", app->worker_message);
        }
        break;
    case NfcEraserOpRekey:
        furi_string_printf(text, "%s", app->worker_message[0] ? app->worker_message : "Done");
        break;
    default:
        furi_string_printf(text, "%s", app->worker_message);
        break;
    }

    widget_reset(app->widget);
    widget_add_string_element(
        app->widget,
        0,
        1,
        AlignLeft,
        AlignTop,
        FontPrimary,
        app->worker_ok ? "Success" : "Finished");
    widget_add_string_multiline_element(
        app->widget, 0, 14, AlignLeft, AlignTop, FontSecondary, furi_string_get_cstr(text));

    furi_string_free(text);

    notification_message(
        app->notifications, app->worker_ok ? &sequence_success : &sequence_error);

    view_dispatcher_switch_to_view(app->view_dispatcher, NfcEraserViewWidget);
}

/* --- about --------------------------------------------------------------- */

static void nfc_eraser_scene_about_on_enter(void* context) {
    NfcEraserApp* app = context;

    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 0, 1, AlignLeft, AlignTop, FontPrimary, "NFC Eraser");
    widget_add_text_scroll_element(
        app->widget,
        0,
        13,
        128,
        50,
        "Erases and re-keys MIFARE\n"
        "Ultralight / Ultralight C tags.\n\n"
        "Only user pages 4..(config-1)\n"
        "are written. Lock bits, OTP\n"
        "and config pages are never\n"
        "touched: they are one-way and\n"
        "cannot be undone.\n\n"
        "Ultralight C pages 44-47 hold\n"
        "the 3DES key. They are write\n"
        "only and can never be read\n"
        "back; the app proves a new key\n"
        "by authenticating with it.\n\n"
        "Keys live in\n"
        "apps_data/nfc_eraser/keys.txt\n"
        "(32 hex digits per line).\n\n"
        "Use only on tags you own.");

    view_dispatcher_switch_to_view(app->view_dispatcher, NfcEraserViewWidget);
}

/* ------------------------------------------------------------------------- */
/* Scene manager plumbing                                                    */
/* ------------------------------------------------------------------------- */

static void nfc_eraser_scene_empty(void* context) {
    UNUSED(context);
}

static bool nfc_eraser_scene_empty_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

static bool nfc_eraser_scene_result_on_event(void* context, SceneManagerEvent event) {
    NfcEraserApp* app = context;

    if(event.type == SceneManagerEventTypeCustom && event.event == NfcEraserEventWorkerDone) {
        /* Refresh once the worker has fully released the NFC hardware. */
        nfc_eraser_scene_result_on_enter(context);
        return true;
    }

    UNUSED(app);
    return false;
}

static void (*const nfc_eraser_on_enter_handlers[NfcEraserSceneNum])(void*) = {
    [NfcEraserSceneScan] = nfc_eraser_scene_scan_on_enter,
    [NfcEraserSceneInfo] = nfc_eraser_scene_info_on_enter,
    [NfcEraserSceneActions] = nfc_eraser_scene_actions_on_enter,
    [NfcEraserSceneKeySelect] = nfc_eraser_scene_key_select_on_enter,
    [NfcEraserSceneConfirm] = nfc_eraser_scene_confirm_on_enter,
    [NfcEraserSceneProgress] = nfc_eraser_scene_progress_on_enter,
    [NfcEraserSceneResult] = nfc_eraser_scene_result_on_enter,
    [NfcEraserSceneAbout] = nfc_eraser_scene_about_on_enter,
};

static bool (*const nfc_eraser_on_event_handlers[NfcEraserSceneNum])(void*, SceneManagerEvent) = {
    [NfcEraserSceneScan] = nfc_eraser_scene_scan_on_event,
    [NfcEraserSceneInfo] = nfc_eraser_scene_info_on_event,
    [NfcEraserSceneActions] = nfc_eraser_scene_empty_event,
    [NfcEraserSceneKeySelect] = nfc_eraser_scene_empty_event,
    [NfcEraserSceneConfirm] = nfc_eraser_scene_empty_event,
    [NfcEraserSceneProgress] = nfc_eraser_scene_progress_on_event,
    [NfcEraserSceneResult] = nfc_eraser_scene_result_on_event,
    [NfcEraserSceneAbout] = nfc_eraser_scene_empty_event,
};

static void (*const nfc_eraser_on_exit_handlers[NfcEraserSceneNum])(void*) = {
    [NfcEraserSceneScan] = nfc_eraser_scene_empty,
    [NfcEraserSceneInfo] = nfc_eraser_scene_empty,
    [NfcEraserSceneActions] = nfc_eraser_scene_empty,
    [NfcEraserSceneKeySelect] = nfc_eraser_scene_empty,
    [NfcEraserSceneConfirm] = nfc_eraser_scene_empty,
    [NfcEraserSceneProgress] = nfc_eraser_scene_empty,
    [NfcEraserSceneResult] = nfc_eraser_scene_empty,
    [NfcEraserSceneAbout] = nfc_eraser_scene_empty,
};

static const SceneManagerHandlers nfc_eraser_scene_handlers = {
    .on_enter_handlers = nfc_eraser_on_enter_handlers,
    .on_event_handlers = nfc_eraser_on_event_handlers,
    .on_exit_handlers = nfc_eraser_on_exit_handlers,
    .scene_num = NfcEraserSceneNum,
};

/* ------------------------------------------------------------------------- */
/* Input handling                                                            */
/* ------------------------------------------------------------------------- */

static bool nfc_eraser_widget_input(InputEvent* event, void* context) {
    NfcEraserApp* app = context;

    if(event->type != InputTypeShort && event->type != InputTypeRepeat) {
        return false;
    }

    uint32_t scene = scene_manager_get_current_scene(app->scene_manager);

    if(event->key == InputKeyOk) {
        switch(scene) {
        case NfcEraserSceneScan:
            if(!app->tag_present) {
                nfc_eraser_start_worker(app, NfcEraserOpScan);
            }
            return true;
        case NfcEraserSceneInfo:
            scene_manager_next_scene(app->scene_manager, NfcEraserSceneActions);
            return true;
        case NfcEraserSceneConfirm:
            nfc_eraser_start_worker(app, app->confirm_op);
            scene_manager_next_scene(app->scene_manager, NfcEraserSceneProgress);
            return true;
        case NfcEraserSceneResult:
            /* Never go back to the progress scene: it would re-arm the erase. */
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, NfcEraserSceneActions);
            return true;
        default:
            return false;
        }
    }

    if(event->key == InputKeyBack) {
        if(scene == NfcEraserSceneProgress) {
            app->abort_requested = true;
            return true; /* consume: abort instead of leaving the scene */
        }
        if(scene == NfcEraserSceneResult) {
            /* Wait for the worker, then skip the progress scene entirely. */
            if(app->worker_running) {
                return true;
            }
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, NfcEraserSceneActions);
            return true;
        }
    }

    return false;
}

static bool nfc_eraser_custom_event_callback(void* context, uint32_t event) {
    NfcEraserApp* app = context;

    if(event == NfcEraserEventWorkerDone && app->worker_running) {
        /* The worker has issued its last event: reap it so a new one can start. */
        furi_thread_join(app->worker);
        app->worker_running = false;
    }

    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool nfc_eraser_navigation_event_callback(void* context) {
    NfcEraserApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

/* ------------------------------------------------------------------------- */
/* App lifecycle                                                             */
/* ------------------------------------------------------------------------- */

int32_t nfc_eraser_app(void* p) {
    UNUSED(p);

    NfcEraserApp* app = malloc(sizeof(NfcEraserApp));
    memset(app, 0, sizeof(NfcEraserApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->storage = furi_record_open(RECORD_STORAGE);
    nfc_eraser_diag_open(app);
    nfc_eraser_diag(app, "boot: services open, gui=%p notif=%p storage=%p",
                    (void*)app->gui, (void*)app->notifications, (void*)app->storage);

    app->text = furi_string_alloc();
    app->nfc_done = furi_event_flag_alloc();

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&nfc_eraser_scene_handlers, app);

    app->widget = widget_alloc();
    app->submenu = submenu_alloc();
    app->var_list = variable_item_list_alloc();

    view_dispatcher_add_view(
        app->view_dispatcher, NfcEraserViewWidget, widget_get_view(app->widget));
    view_dispatcher_add_view(
        app->view_dispatcher, NfcEraserViewSubmenu, submenu_get_view(app->submenu));
    view_dispatcher_add_view(
        app->view_dispatcher, NfcEraserViewVarList, variable_item_list_get_view(app->var_list));
    nfc_eraser_diag(app, "boot: views added");

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, nfc_eraser_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, nfc_eraser_navigation_event_callback);

    view_set_input_callback(widget_get_view(app->widget), nfc_eraser_widget_input);
    variable_item_list_set_enter_callback(app->var_list, nfc_eraser_key_select_callback, app);

    nfc_eraser_load_keys(app);
    nfc_eraser_diag(app, "boot: keys=%u", (unsigned)app->key_count);

    app->worker =
        furi_thread_alloc_ex("NfcEraserWorker", 6 * 1024, nfc_eraser_worker, app);
    nfc_eraser_diag(app, "boot: worker thread allocated");

    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    scene_manager_next_scene(app->scene_manager, NfcEraserSceneScan);
    nfc_eraser_diag(app, "boot: scan scene entered, running dispatcher");
    view_dispatcher_run(app->view_dispatcher);
    nfc_eraser_diag(app, "boot: dispatcher returned");

    /* Make sure the NFC hardware is released before tearing anything down. */
    if(app->worker_running) {
        app->abort_requested = true;
        furi_thread_join(app->worker);
        app->worker_running = false;
    }
    furi_thread_free(app->worker);

    view_dispatcher_remove_view(app->view_dispatcher, NfcEraserViewWidget);
    view_dispatcher_remove_view(app->view_dispatcher, NfcEraserViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, NfcEraserViewVarList);

    variable_item_list_free(app->var_list);
    submenu_free(app->submenu);
    widget_free(app->widget);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    furi_string_free(app->text);
    furi_event_flag_free(app->nfc_done);
    nfc_eraser_diag_close(app);

    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);

    free(app);

    return 0;
}
