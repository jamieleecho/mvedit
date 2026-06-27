#include <assert.h>
#include <fcntl.h>      /* struct sgbuf, _gs_opt, _ss_opt */
#include <stdio.h>      /* assert() expands to a printf */
#include <stdlib.h>     /* exit */
#include <string.h>
#include <unistd.h>

#include <cgfx.h>
#include <mvkit/mvkit.h>

#include "textdoc.h"
#include "text_view.h"
#include "version.h"


/*
 * mvedit -- a mouse-driven text editor for Multi-Vue, built with MVKit.
 *
 * Structure mirrors mvdraw: a theme, a menu bar, an MVDocument-like model
 * (textdoc.c), and a content view (text_view.c). The window is a WT_FSWIN
 * (framed with scrollbars); the scroll arrows arrive as the window manager's
 * MN_*SCRL menu ids and are routed to the view's scroller. Click places the
 * cursor, drag selects, and the usual File/Edit commands operate on the model.
 *
 * Undo is single-level: the app snapshots the whole document before each change
 * (cheap -- TextDoc is a plain value) and Edit > Undo restores it. The document
 * is "dirty" from the first change until the next save.
 */

#define MN_HELP 30   /* app-chosen menu id (cgfx reserves the low numbers) */

/* Monochrome (screen type 5) theme, as in mvdraw: only registers 0 and 1 show
   at 1bpp, so we keep 0=black, 1=white and mirror that into the chrome ramp so
   menus and dialogs resolve correctly. */
static const MVTheme theme = { {
    0x00, 0x3f, 0x00, 0x3f, 0x00, 0x3f, 0x00, 0x3f,
    0x00, 0x3f, 0x00, 0x3f, 0x00, 0x3f, 0x00, 0x3f
} };

#define DOC_EXT ".txt"

/* Undo costs a whole-document snapshot on every change; set to 0 to disable. */
#define ENABLE_UNDO 0


/* ---- model / view / document state -------------------------------------- */

static TextDoc textdoc;
#if ENABLE_UNDO
static TextDoc undo_buf;        /* single-level undo snapshot */
#endif
static int     has_undo;        /* always 0 when undo is disabled (Undo greyed) */

static char    path[MV_PATH_MAX];
static int     file_backed;
static char    status_name[MV_PATH_MAX];

static TextView editor;


/* ---- menus -------------------------------------------------------------- */

typedef enum { FileIndex_Save = 2 } FileIndex;

static MIDSCR file_items[] = {
    MV_MENU_ITEM("New"),
    MV_MENU_ITEM("Open..."),
    MV_MENU_ITEM("Save"),
    MV_MENU_ITEM("Save As..."),
    MV_MENU_SEPARATOR,
    MV_MENU_ITEM("Exit"),
};

typedef enum {
    EditIndex_Undo = 0,
    EditIndex_Cut = 2,
    EditIndex_Copy = 3,
    EditIndex_Paste = 4,
    EditIndex_Delete = 5,
    EditIndex_SelectAll = 7
} EditIndex;

static MIDSCR edit_items[] = {
    MV_MENU_ITEM("Undo"),
    MV_MENU_SEPARATOR,
    MV_MENU_ITEM("Cut"),
    MV_MENU_ITEM("Copy"),
    MV_MENU_ITEM("Paste"),
    MV_MENU_ITEM("Delete"),
    MV_MENU_SEPARATOR,
    MV_MENU_ITEM("Select All"),
};

static MIDSCR help_items[] = {
    MV_MENU_ITEM("About..."),
};

static MNDSCR menus[] = {
    MV_MENU("File", MN_FILE, file_items),
    MV_MENU("Edit", MN_EDIT, edit_items),
    MV_MENU("Help", MN_HELP, help_items),
};

/* Fix the window at 80x25 so the editor's 78x23 working area never changes. */
mv_set_menus_sized(mywindow, "mvedit", menus, 80, 25);


/* ---- helpers ------------------------------------------------------------ */

static void refresh_menus_if_changed(void);

static void set_status_name(void) {
    if (!file_backed || path[0] == 0) {
        strcpy(status_name, "untitled");
        return;
    }
    {
        const char *base = path, *p;
        for (p = path; *p; ++p) {
            if (*p == '/') {
                base = p + 1;
            }
        }
        strcpy(status_name, base);
    }
}

/* Try to save to the current file (or prompt for one). Returns 1 if the
   document was written, 0 if cancelled or the write failed. */
static int try_save_as(void);

static int try_save(void) {
    if (!file_backed) {
        return try_save_as();
    }
    if (textdoc_save(&textdoc, path) == 0) {
        editor.doc_modified = 0;
        refresh_menus_if_changed();
        text_view_refresh(&editor);
        return 1;
    }
    mv_app_show_message_box("Could not save file.", MVMessageBoxType_Error);
    return 0;
}

static int try_save_as(void) {
    char chosen[MV_PATH_MAX];
    if (!mv_app_show_save_dialog(chosen, DOC_EXT)) {
        return 0;       /* cancelled */
    }
    if (textdoc_save(&textdoc, chosen) == 0) {
        strcpy(path, chosen);
        file_backed = 1;
        editor.doc_modified = 0;
        set_status_name();
        refresh_menus_if_changed();
        text_view_refresh(&editor);
        return 1;
    }
    mv_app_show_message_box("Could not save file.", MVMessageBoxType_Error);
    return 0;
}

/* Prompt to save if dirty. Returns 1 if it is OK to discard the document. */
static int confirm_discard(void) {
    if (!editor.doc_modified) {
        return 1;
    }
    if (mv_app_show_message_box("Save changes?", MVMessageBoxType_YesNo)
            == MVMessageBoxResult_Yes) {
        return try_save();
    }
    return 1;   /* No: discard */
}

/* The only thing the menu bar's enabled-state depends on: Save (dirty), Undo,
   Cut/Copy (selection), Paste (clipboard). _cgfx_ss_umbar (the redraw the
   refresh triggers) is slow, and most keys/clicks change none of these, so only
   ask for a rebuild when this signature actually changes. */
static void refresh_menus_if_changed(void) {
    static int last = -1;
    int state =
        (editor.doc_modified ? 1 : 0) |
        (has_undo ? 2 : 0) |
        (text_view_has_selection(&editor) ? 4 : 0) |
        (text_view_has_clip(&editor) ? 8 : 0);
    if (state != last) {
        last = state;
        mv_app_refresh_menubar();
    }
}


/* ---- menu actions ------------------------------------------------------- */

static void new_action(MSRET *msinfo, int menuid, int itemno) {
    if (!confirm_discard()) {
        return;
    }
    textdoc_new(&textdoc, 0);
    path[0] = 0;
    file_backed = 0;
    has_undo = 0;
    editor.doc_modified = 0;
    set_status_name();
    text_view_reset(&editor);
    refresh_menus_if_changed();
}

static void open_action(MSRET *msinfo, int menuid, int itemno) {
    char chosen[MV_PATH_MAX];
    if (!confirm_discard()) {
        return;
    }
    if (!mv_app_show_open_dialog(chosen, DOC_EXT)) {
        return;
    }
    if (textdoc_open(&textdoc, chosen) == 0) {
        strcpy(path, chosen);
        file_backed = 1;
    } else {
        textdoc_new(&textdoc, 0);
        path[0] = 0;
        file_backed = 0;
        mv_app_show_message_box("Could not open file.", MVMessageBoxType_Error);
    }
    has_undo = 0;
    editor.doc_modified = 0;
    set_status_name();
    text_view_reset(&editor);
    refresh_menus_if_changed();
}

static void save_action(MSRET *msinfo, int menuid, int itemno) {
    try_save();
}

static void save_as_action(MSRET *msinfo, int menuid, int itemno) {
    try_save_as();
}

static void exit_action(MSRET *msinfo, int menuid, int itemno) {
    if (confirm_discard()) {
        exit(0);
    }
}

static void undo_action(MSRET *msinfo, int menuid, int itemno) {
#if ENABLE_UNDO
    if (!has_undo) {
        return;
    }
    textdoc = undo_buf;          /* restore the pre-change snapshot */
    has_undo = 0;
    editor.doc_modified = 1;
    text_view_reclamp(&editor);
    refresh_menus_if_changed();
#endif
}

static void cut_action(MSRET *msinfo, int menuid, int itemno) {
    text_view_cut(&editor);
    refresh_menus_if_changed();
}

static void copy_action(MSRET *msinfo, int menuid, int itemno) {
    text_view_copy(&editor);
    refresh_menus_if_changed();
}

static void paste_action(MSRET *msinfo, int menuid, int itemno) {
    text_view_paste(&editor);
    refresh_menus_if_changed();
}

static void delete_action(MSRET *msinfo, int menuid, int itemno) {
    text_view_delete(&editor);
    refresh_menus_if_changed();
}

static void select_all_action(MSRET *msinfo, int menuid, int itemno) {
    text_view_select_all(&editor);
    refresh_menus_if_changed();
}

static void about_action(MSRET *msinfo, int menuid, int itemno) {
    mv_app_show_message_box("mvedit v" APP_VERSION "\r(C) 2026 Jamie Cho",
                            MVMessageBoxType_Info);
}

/* Catch-all: the window manager's scroll arrows arrive here as MN_*SCRL ids
   (no explicit row matches them), as do MN_MOVE/MN_GROW, which we ignore. */
static void unhandled_menu(MSRET *msinfo, int menuid, int itemno) {
    switch (menuid) {
        case MN_USCRL: text_view_scroll(&editor, 0, -1); break;
        case MN_DSCRL: text_view_scroll(&editor, 0,  1); break;
        case MN_LSCRL: text_view_scroll(&editor, -1, 0); break;
        case MN_RSCRL: text_view_scroll(&editor,  1, 0); break;
        default: break;
    }
}

static MVMenuItemAction menu_actions[] = {
    {MN_CLOS, 1, exit_action},
    {MN_FILE, 1, new_action},
    {MN_FILE, 2, open_action},
    {MN_FILE, 3, save_action},
    {MN_FILE, 4, save_as_action},
    {MN_FILE, 6, exit_action},
    {MN_EDIT, 1, undo_action},
    {MN_EDIT, 3, cut_action},
    {MN_EDIT, 4, copy_action},
    {MN_EDIT, 5, paste_action},
    {MN_EDIT, 6, delete_action},
    {MN_EDIT, 8, select_all_action},
    {MN_HELP, 1, about_action},
    {-1, -1, unhandled_menu}   /* catch-all (handles the scroll arrows) */
};


/* ---- edit notifications: snapshot for undo, mark dirty ------------------- */

#if ENABLE_UNDO
static void on_will_change(TextView *v) {
    undo_buf = textdoc;
    has_undo = 1;
}
#endif

static void on_did_change(TextView *v) {
    editor.doc_modified = 1;
    refresh_menus_if_changed();
}


/* ---- event handling ----------------------------------------------------- */

/* Run a Ctrl shortcut if c is one; return 1 if handled. */
static int handle_shortcut(char c) {
    switch (c) {
        case 0x1A: undo_action(0, -1, -1);       return 1;   /* Ctrl-Z */
        case 0x13: save_action(0, -1, -1);       return 1;   /* Ctrl-S */
        case 0x0E: new_action(0, -1, -1);        return 1;   /* Ctrl-N */
        case 0x0F: open_action(0, -1, -1);       return 1;   /* Ctrl-O */
        case 0x01: select_all_action(0, -1, -1); return 1;   /* Ctrl-A */
        case 0x18: cut_action(0, -1, -1);        return 1;   /* Ctrl-X */
        case 0x03: copy_action(0, -1, -1);       return 1;   /* Ctrl-C */
        case 0x16: paste_action(0, -1, -1);      return 1;   /* Ctrl-V */
    }
    return 0;
}

/* The run loop hands us one key at a time, but when typing outpaces the (slow)
   repaint, more keys queue in the SCF input buffer. Drain them and apply the
   whole run with drawing suppressed, then repaint once -- one Flush instead of
   one per key. A single key (nothing queued) takes the normal fine-grained
   path; Ctrl shortcuts flush the batch and run on their own. */
static void handle_key_event(MVUiEvent *event) {
    char c = event->info.key.character;
    int batching = 0;

    for (;;) {
        if (handle_shortcut(c)) {
            if (batching) {
                text_view_end_batch(&editor);
                batching = 0;
            }
        } else {
            if (!batching && _gs_rdy(MV_INPATH) != -1) {
                text_view_begin_batch(&editor);   /* more queued: batch them */
                batching = 1;
            }
            text_view_key(&editor, c);
        }
        /* _gs_rdy (SS_Ready) returns -1 only when the input buffer is empty;
           a ready byte may read as 0, so test against -1 (as MVKit's file
           dialog does), not > 0 -- otherwise a queued key is left unread and
           waits for the run loop's next signal. */
        if (_gs_rdy(MV_INPATH) == -1) {
            break;
        }
        if (read(MV_INPATH, &c, 1) != 1) {
            break;
        }
    }
    if (batching) {
        text_view_end_batch(&editor);
    }
    refresh_menus_if_changed();
}

static void handle_click_event(MVUiEvent *event) {
    /* A click can move the cursor / change the selection, so refresh the menu
       bar to keep Cut/Copy/Paste enable-state in sync. */
    mv_view_dispatch_click(&editor.view, event);
    refresh_menus_if_changed();
}

static void mvedit_action(MVUiEvent *event) {
    switch (event->event_type) {
        case MVUiEventType_KeyPress:
            handle_key_event(event);
            break;
        case MVUiEventType_MouseClick:
            handle_click_event(event);
            break;
    }
}


/* ---- lifecycle ---------------------------------------------------------- */

static void mvedit_pre_init(int argc, char **argv) {
    if (argc > 2) {
        exit(1);
    }
    mv_app_set_theme(&theme);

    textdoc_new(&textdoc, 0);
    strcpy(status_name, "untitled");

    if (argc == 2) {
        if (textdoc_open(&textdoc, argv[1]) == 0) {
            strcpy(path, argv[1]);
            file_backed = 1;
            set_status_name();
        }
    }
}

static void mvedit_init(void) {
    struct sgbuf opts;

    assert(strncmp(file_items[FileIndex_Save]._mittl, "Save", 5) == 0);
    assert(strncmp(edit_items[EditIndex_Undo]._mittl, "Undo", 5) == 0);

    _cgfx_scalesw(MV_OUTPATH, false);
    _cgfx_font(MV_OUTPATH, GRP_FONT, FNT_S8X8);

    /* Clear the keyboard interrupt/abort chars so ESC/BREAK (0x05, used here as
       Backspace) and the Ctrl- shortcuts arrive as data instead of raising a
       signal -- the same thing MVKit's file dialog does. */
    if (_gs_opt(MV_INPATH, &opts) == 0) {
        opts.sg_kbich = 0;
        opts.sg_kbach = 0;
        _ss_opt(MV_INPATH, &opts);
    }

    /* Use the I-beam text pointer (hotspot 3,3) rather than the default arrow;
       the view also toggles this off around every repaint for speed. */
    _cgfx_setgc(MV_OUTPATH, GRP_PTR, PTR_TXT);

    text_view_init(&editor, 0, 0, EDITOR_COLS * 8, EDITOR_ROWS * 8, &textdoc);
#if ENABLE_UNDO
    editor.will_change = on_will_change;
#endif
    editor.did_change = on_did_change;
    text_view_set_status(&editor, status_name, 0);
    text_view_refresh(&editor);
}

static void mvedit_refresh_menus_action(void) {
    int sel = text_view_has_selection(&editor);

    mv_menu_item_set_enabled(file_items, FileIndex_Save, editor.doc_modified);
    mv_menu_item_set_enabled(edit_items, EditIndex_Undo, has_undo);
    mv_menu_item_set_enabled(edit_items, EditIndex_Cut, sel);
    mv_menu_item_set_enabled(edit_items, EditIndex_Copy, sel);
    mv_menu_item_set_enabled(edit_items, EditIndex_Paste, text_view_has_clip(&editor));
}

int main(int argc, char **argv) {
    return mv_app_run_with_scrollbars(argc, argv, &mywindow,
        mvedit_pre_init, mvedit_init, menu_actions,
        mvedit_refresh_menus_action, mvedit_action);
}
