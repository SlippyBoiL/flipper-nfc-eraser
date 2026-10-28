#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <lib/nfc/nfc.h>

#include "nfc_eraser_nfc.h"

#define NFC_ERASER_MAX_KEYS  (16U)
#define NFC_ERASER_APP_DIR   "/ext/apps_data/nfc_eraser"
#define NFC_ERASER_DUMP_DIR  "/ext/apps_data/nfc_eraser/dumps"
#define NFC_ERASER_KEYS_PATH "/ext/apps_data/nfc_eraser/keys.txt"
#define NFC_ERASER_LOG_PATH  "/ext/apps_data/nfc_eraser/last_run.log"

typedef enum {
    NfcEraserViewWidget,
    NfcEraserViewSubmenu,
    NfcEraserViewVarList,
} NfcEraserViewId;

typedef enum {
    NfcEraserSceneScan,
    NfcEraserSceneInfo,
    NfcEraserSceneActions,
    NfcEraserSceneKeySelect,
    NfcEraserSceneConfirm,
    NfcEraserSceneProgress,
    NfcEraserSceneResult,
    NfcEraserSceneAbout,
    NfcEraserSceneNum,
} NfcEraserSceneId;

typedef enum {
    NfcEraserOpScan,
    NfcEraserOpWipe,
    NfcEraserOpRekey,
    NfcEraserOpWipeRekey,
    NfcEraserOpSaveDump,
} NfcEraserOp;

typedef enum {
    NfcEraserEventWorkerDone = 100,
    NfcEraserEventProgress,
} NfcEraserEvent;

typedef struct {
    NfcEraserOp op;
    uint16_t page_from;
    uint16_t page_to;
    uint16_t page_current;
    uint16_t written;
    uint16_t skipped;
    uint16_t failed;
    uint16_t verified;
    bool authenticated;
    bool aborted;
} NfcEraserProgress;

typedef struct {
    Gui* gui;
    NotificationApp* notifications;
    Storage* storage;
    File* diag;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;

    Widget* widget;
    Submenu* submenu;
    VariableItemList* var_list;

    FuriThread* worker;
    FuriEventFlag* nfc_done;
    bool worker_running;
    volatile bool abort_requested;
    NfcEraserOp pending_op;
    NfcEraserOp confirm_op;
    bool worker_ok;
    char worker_message[96];

    Nfc* nfc;
    EraserTag tag;
    bool tag_present;
    bool authenticated;
    int active_key;

    uint8_t pages[ERASER_MAX_PAGES][ERASER_PAGE_SIZE];
    bool page_valid[ERASER_MAX_PAGES];

    uint8_t keys[NFC_ERASER_MAX_KEYS][ERASER_ULC_KEY_SIZE];
    size_t key_count;
    size_t selected_key;
    bool key_select_for_rekey;

    NfcEraserProgress progress;
    FuriString* text;
} NfcEraserApp;

int32_t nfc_eraser_app(void* p);
