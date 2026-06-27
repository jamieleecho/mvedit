#ifndef _TEXT_VIEW_H_
#define _TEXT_VIEW_H_

#include <mvkit/mv_view.h>

#include "textdoc.h"

/*
 * mvedit's content view: an editable text area. Embeds MVView first so the app
 * dispatches to it via mv_view_dispatch_click()/mv_view_draw(). It owns the
 * cursor, the mouse selection (click to place, drag to extend), horizontal and
 * vertical scrolling, an internal cut/copy/paste clipboard, and rendering with
 * reverse-video selection plus a status line.
 *
 * The view stays framework-light: it knows MVView + cgfx + the TextDoc model.
 * Edits are reported to the app through will_change / did_change so the app can
 * snapshot for undo and mark the document dirty (the view never touches files).
 */

/* Visible text area, in 8x8 character cells. The window is 80x25; its chrome
   (frame + scrollbars) leaves a 78x23 working area. We use the bottom row for a
   status line, so the text occupies the top EDITOR_ROWS rows. Never writing the
   bottom-right corner cell avoids the window auto-scrolling on a full write. */
#define EDITOR_COLS   78    /* visible columns                                 */
#define EDITOR_ROWS   22    /* visible text rows (row EDITOR_ROWS is status)   */
#define EDITOR_STATUS_ROW EDITOR_ROWS

/* Clipboard capacity: large enough for a Select-All of the whole document. */
#define ED_CLIP_SZ    (ED_MAX_LINES * ED_LINE_SZ + 1)

typedef struct TextView {
    MVView view;
    TextDoc *doc;

    int cur_line, cur_col;     /* cursor, document coordinates                 */
    int sel_line, sel_col;     /* selection anchor; == cursor means no range   */

    int top_line;              /* document line shown on screen row 0          */
    int left_col;              /* document column shown at screen col 0        */
    int scroll_drow, scroll_dcol;  /* deltas of the last ensure_visible() scroll */

    int sbar_hor, sbar_ver;    /* last scroll-thumb values pushed (-1 = none)  */

    int fg, bg;                /* palette registers for text / paper           */

    /* status-line content, set by the app before a refresh */
    const char *doc_name;      /* file name, or 0 for "untitled"               */
    int doc_modified;          /* show the dirty marker                        */
    int status_dirty_shown;    /* dirty-state currently drawn (-1 = redraw)    */
    int status_line_shown;     /* Ln value currently drawn (-1 = redraw)       */
    int status_col_shown;      /* Col value currently drawn (-1 = redraw)      */

    int suppress_draw;         /* batch mode: ops mutate but don't repaint     */
    int batch_snapshotted;     /* batch took its single undo snapshot already  */

    /* edit notifications (set by the app; may be 0) */
    void (*will_change)(struct TextView *self);   /* before a model mutation   */
    void (*did_change)(struct TextView *self);    /* after a model mutation    */

    char clip[ED_CLIP_SZ];     /* cut/copy buffer (ED_NL between lines)         */
    int  clip_len;             /* 0 when empty                                 */
} TextView;


extern void text_view_init(TextView *v, int x, int y, int width, int height,
                           TextDoc *doc);

/* Full repaint (every visible row + status line + caret). */
extern void text_view_refresh(TextView *v);

/* Reset cursor/selection/scroll to the top-left (after New / Open). */
extern void text_view_reset(TextView *v);

/* Re-clamp the cursor/selection/scroll into the current document, then repaint
   (after an Undo swapped the document out from under the view). */
extern void text_view_reclamp(TextView *v);

extern void text_view_set_status(TextView *v, const char *name, int modified);

/* Handle one key (printable, Enter, Backspace, or an arrow). Returns true if it
   was consumed. Movement/editing and scrolling-to-keep-the-cursor-visible are
   all handled here. */
extern int text_view_key(TextView *v, char c);

/* When the keyboard is ahead of the display, bracket a run of text_view_key()
   calls with these: drawing is suppressed during the batch and done once at the
   end, and the batch takes a single undo snapshot (Undo reverts the whole run). */
extern void text_view_begin_batch(TextView *v);
extern void text_view_end_batch(TextView *v);

/* Scroll by whole cells without moving the cursor (the scrollbar handlers).
   dcol/drow are signed step counts. */
extern void text_view_scroll(TextView *v, int dcol, int drow);

/* Clipboard / selection commands (the Edit menu). */
extern void text_view_select_all(TextView *v);
extern void text_view_copy(TextView *v);
extern void text_view_cut(TextView *v);
extern void text_view_paste(TextView *v);
extern void text_view_delete(TextView *v);   /* delete selection, or char at cursor */

extern int  text_view_has_selection(const TextView *v);
extern int  text_view_has_clip(const TextView *v);

#endif /* _TEXT_VIEW_H_ */
