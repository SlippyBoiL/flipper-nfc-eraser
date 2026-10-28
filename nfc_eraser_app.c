#include "nfc_eraser_app.h"

#include <stdio.h>
#include <string.h>

#define TAG "NfcEraser"

#define NFC_ERASER_FIRST_USER_PAGE (4U)

static const uint8_t nfc_eraser_seed_keys[][ERASER_ULC_KEY_SIZE] = {
    {'B', 'R', 'E', 'A', 'K', 'M', 'E', 'I', 'F', 'Y', 'O', 'U', 'C', 'A', 'N', '!'},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
};

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
    furi_string_printf(text, "NFC Eraser dump\nType: %s\n", eraser_tag_type_name(app->tag.type));
    furi_string_cat_str(text, "UID: ");
    nfc_eraser_hex(app->tag.uid, app->tag.uid_len, text, true);
    furi_string_cat_str(text, "\n");

    for(uint16_t page = 0; page < app->tag.pages_total && page < ERASER_MAX_PAGES; page++) {
        furi_string_cat_printf(text, "Page %02X: ", page);
        if(app->page_valid[page]) {
            nfc_eraser_hex(app->pages[page], ERASER_PAGE_SIZE, text, true);
        } else {
            furi_string_cat_str(text, "?? ?? ?? ??");
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

    snprintf(app->worker_message, sizeof(app->worker_message), saved ? "Saved" : "Failed");

    furi_string_free(text);
    furi_string_free(path);

    return saved;
}

static int32_t nfc_eraser_worker(void* context) {
    NfcEraserApp* app = context;

    if(app->pending_op == NfcEraserOpSaveDump) {
        app->worker_ok = nfc_eraser_save_dump(app);
        view_dispatcher_send_custom_event(app->view_dispatcher, NfcEraserEventWorkerDone);
        return 0;
    }

    Nfc* nfc = nfc_alloc();
    app->nfc = nfc;

    nfc_config(nfc, NfcModePoller, NfcTechIso14443a);
    nfc_set_fdt_listen_fc(nfc, 1172);
    nfc_set_fdt_poll_fc(nfc, 1172);
    nfc_set_fdt_poll_poll_us(nfc, 100);

    nfc_start(nfc, NULL, app);
    nfc_stop(nfc);

    app->nfc = NULL;
    nfc_free(nfc);

    view_dispatcher_send_custom_event(app->view_dispatcher, NfcEraserEventWorkerDone);
    return 0;
}

int32_t nfc_eraser_app(void* p) {
    UNUSED(p);
    NfcEraserApp* app = malloc(sizeof(NfcEraserApp));
    memset(app, 0, sizeof(NfcEraserApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->storage = furi_record_open(RECORD_STORAGE);

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&nfc_eraser_scene_handlers, app);
    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->widget = widget_alloc();
    app->submenu = submenu_alloc();
    app->var_list = variable_item_list_alloc();
    app->text = furi_string_alloc();
    app->worker = furi_thread_alloc_ex("NfcEraserWorker", 4096, nfc_eraser_worker, app);

    view_dispatcher_add_view(app->view_dispatcher, NfcEraserViewWidget, widget_get_view(app->widget));
    view_dispatcher_add_view(app->view_dispatcher, NfcEraserViewSubmenu, submenu_get_view(app->submenu));
    view_dispatcher_add_view(app->view_dispatcher, NfcEraserViewVarList, variable_item_list_get_view(app->var_list));

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, nfc_eraser_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, nfc_eraser_navigation_event_callback);

    nfc_eraser_load_keys(app);
    app->selected_key = 0;

    scene_manager_next_scene(app->scene_manager, NfcEraserSceneScan);
    view_dispatcher_run(app->view_dispatcher);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);
    widget_free(app->widget);
    submenu_free(app->submenu);
    variable_item_list_free(app->var_list);
    furi_string_free(app->text);
    furi_thread_free(app->worker);

    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);

    free(app);
    return 0;
}
