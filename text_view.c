#include <cgfx.h>
#include <string.h>
#include <mvkit/mv_defs.h>   /* MV_OUTPATH, Flush */

#include "text_view.h"


/* Key codes delivered by the NitrOS-9 CoCo keyboard. Up/Down are confirmed by
   MVKit's file dialog (it scrolls on 0x0C / 0x0A); Right is 0x09 and the
   left/erase key is 0x08. The CoCo has no distinct Backspace, so 0x08 erases
   (cursor-left is done with the mouse, per this app's mouse-driven design).
   Verify on real hardware and adjust if your keymap differs. */
#define KEY_BACKSPACE 0x08
#define KEY_RIGHT     0x09
#define KEY_DOWN      0x0A
#define KEY_ENTER     0x0D
#define KEY_UP        0x0C


/* ---- small helpers ------------------------------------------------------ */

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int line_len(const TextView *v, int line) {
    return textdoc_line_len(v->doc, line);
}

static int has_selection(const TextView *v) {
    return v->sel_line != v->cur_line || v->sel_col != v->cur_col;
}

/* Order the anchor and the cursor into [(*l0,*c0), (*l1,*c1)). */
static void selection_range(const TextView *v, int *l0, int *c0, int *l1, int *c1) {
    if (v->sel_line < v->cur_line ||
        (v->sel_line == v->cur_line && v->sel_col <= v->cur_col)) {
        *l0 = v->sel_line; *c0 = v->sel_col;
        *l1 = v->cur_line; *c1 = v->cur_col;
    } else {
        *l0 = v->cur_line; *c0 = v->cur_col;
        *l1 = v->sel_line; *c1 = v->sel_col;
    }
}



/* ---- scrolling ---------------------------------------------------------- */

/* The largest column we ever scroll right to (last column of the longest
   possible line minus a screenful). */
#define MAX_LEFT (ED_MAX_COLS - EDITOR_COLS + 1)

static void update_scrollbars(TextView *v) {
    int nlines = textdoc_num_lines(v->doc);
    int vrange = nlines - EDITOR_ROWS;       /* scrollable lines */
    int hrange = MAX_LEFT;
    int hor, ver;

    /* _cgfx_ss_sbar takes a thumb position; we send a 0..255 proportional
       value. (Confirm the expected range on real hardware.) */
    ver = (vrange > 0) ? (int)((long)v->top_line * 255 / vrange) : 0;
    hor = (hrange > 0) ? (int)((long)v->left_col * 255 / hrange) : 0;
    hor = clampi(hor, 0, 255);
    ver = clampi(ver, 0, 255);

    /* _cgfx_ss_sbar is slow; only push when the thumb actually moves. A small
       scroll in a large document often leaves the 0..255 value unchanged. */
    if (hor != v->sbar_hor || ver != v->sbar_ver) {
        v->sbar_hor = hor;
        v->sbar_ver = ver;
        _cgfx_ss_sbar(MV_OUTPATH, hor, ver);
    }
}

/* Scroll the view so the cursor is visible. Returns true (and refreshes the
   scroll-bar thumbs) if the view actually scrolled -- the caller then needs a
   full repaint; otherwise a partial repaint suffices. */
static int ensure_visible(TextView *v) {
    int nlines = textdoc_num_lines(v->doc);
    int oldtop = v->top_line, oldleft = v->left_col;

    if (v->cur_line < v->top_line) {
        v->top_line = v->cur_line;
    } else if (v->cur_line >= v->top_line + EDITOR_ROWS) {
        v->top_line = v->cur_line - EDITOR_ROWS + 1;
    }
    v->top_line = clampi(v->top_line, 0, (nlines > 0) ? nlines - 1 : 0);

    if (v->cur_col < v->left_col) {
        v->left_col = v->cur_col;
    } else if (v->cur_col >= v->left_col + EDITOR_COLS) {
        v->left_col = v->cur_col - EDITOR_COLS + 1;
    }
    v->left_col = clampi(v->left_col, 0, MAX_LEFT);

    if (v->top_line != oldtop || v->left_col != oldleft) {
        update_scrollbars(v);
        return 1;
    }
    return 0;
}


/* ---- rendering ---------------------------------------------------------- */

/* Build the visible slice of document line `docline` into buf[EDITOR_COLS],
   space-padded; report the selected screen-column span [*ss0, *ss1) (or a
   zero-width span when this row carries no selection). */
static void build_row(const TextView *v, int docline, char *buf,
                      int *ss0, int *ss1) {
    int len = (docline < textdoc_num_lines(v->doc)) ? line_len(v, docline) : -1;
    const char *s = (len >= 0) ? textdoc_line(v->doc, docline) : "";
    int i;

    *ss0 = *ss1 = 0;

    for (i = 0; i < EDITOR_COLS; ++i) {
        int dc = v->left_col + i;
        buf[i] = (len >= 0 && dc < len) ? s[dc] : ' ';
    }

    if (has_selection(v) && len >= 0) {
        int l0, c0, l1, c1, a, b;
        selection_range(v, &l0, &c0, &l1, &c1);
        if (docline >= l0 && docline <= l1) {
            int dc0 = (docline == l0) ? c0 : 0;
            int dc1 = (docline == l1) ? c1 : len;   /* through end-of-line */
            a = clampi(dc0 - v->left_col, 0, EDITOR_COLS);
            b = clampi(dc1 - v->left_col, 0, EDITOR_COLS);
            if (b > a) { *ss0 = a; *ss1 = b; }
        }
    }
}

static void draw_text_row(const TextView *v, int srow) {
    char buf[EDITOR_COLS];
    int ss0, ss1;

    build_row(v, v->top_line + srow, buf, &ss0, &ss1);
    _cgfx_curxy(MV_OUTPATH, 0, srow);

    if (ss1 <= ss0) {                       /* no selection on this row */
        cwrite(MV_OUTPATH, buf, EDITOR_COLS);
        return;
    }
    if (ss0 > 0) {
        cwrite(MV_OUTPATH, buf, ss0);
    }
    _cgfx_revon(MV_OUTPATH);
    cwrite(MV_OUTPATH, buf + ss0, ss1 - ss0);
    _cgfx_revoff(MV_OUTPATH);
    if (ss1 < EDITOR_COLS) {
        cwrite(MV_OUTPATH, buf + ss1, EDITOR_COLS - ss1);
    }
}

/* The status line is split so a keystroke only repaints the part that changed:
   the LEFT part (name + dirty marker) changes rarely; the Ln/Col field lives at
   a fixed column and is rewritten on every cursor move. The bottom-right corner
   cell (EDITOR_COLS-1) is never written -- that would scroll the window. */
#define STATUS_POS_COL 60   /* fixed start column of the "Ln .. Col .." field */

/* Show the Ln/Col field? When off, the status line is just the name + dirty
   marker (redrawn only when that changes) -- no per-keystroke status work. */
#define SHOW_STATUS_POS 0

#if SHOW_STATUS_POS
#define STATUS_LEFT_END STATUS_POS_COL      /* leave room for the Ln/Col field */
#else
#define STATUS_LEFT_END (EDITOR_COLS - 1)   /* name fills the whole status line */
#endif

#if SHOW_STATUS_POS
/* Write n (>=0) left-justified in a `width`-char field, space-padded, in a
   SINGLE cwrite -- one OS-9 call instead of one per digit/space. */
static void put_int_w(int n, int width) {
    char buf[8], rev[8];
    int len = 0, w = 0, j;

    if (width > (int)sizeof(buf)) {
        width = sizeof(buf);
    }
    if (n == 0) {
        rev[len++] = '0';
    } else {
        while (n > 0 && len < (int)sizeof(rev)) {
            rev[len++] = (char)('0' + n % 10);
            n /= 10;
        }
    }
    for (j = len - 1; j >= 0; --j) {
        buf[w++] = rev[j];
    }
    while (w < width) {
        buf[w++] = ' ';
    }
    cwrite(MV_OUTPATH, buf, w);
}
#endif /* SHOW_STATUS_POS */

/* Left part: " name [*]", padded out to STATUS_LEFT_END (the Ln/Col column when
   the position field is shown, else the whole line). */
static void draw_status_left(const TextView *v) {
    const char *name = v->doc_name ? v->doc_name : "untitled";
    int nlen = strlen(name);
    int written = 1, pad;
    char sp = ' ';

    if (nlen > STATUS_LEFT_END - 4) {
        nlen = STATUS_LEFT_END - 4;         /* clip an over-long name */
    }
    _cgfx_curxy(MV_OUTPATH, 0, EDITOR_STATUS_ROW);
    _cgfx_revon(MV_OUTPATH);
    cwrite(MV_OUTPATH, &sp, 1);
    cwrite(MV_OUTPATH, name, nlen);             written += nlen;
    if (v->doc_modified) {
        cwrite(MV_OUTPATH, " *", 2);            written += 2;
    }
    for (pad = written; pad < STATUS_LEFT_END; ++pad) {
        cwrite(MV_OUTPATH, &sp, 1);
    }
    _cgfx_revoff(MV_OUTPATH);
}

#if SHOW_STATUS_POS
/* Fixed columns of the position field: "Ln " 60-62, line 63-66, " Col " 67-71,
   col 72-75, pad 76. The labels never change, so they are drawn once per full
   status redraw; the numbers are rewritten in place (4 cells each) only when
   they change -- a keystroke usually rewrites just the 4-cell Col number. */
#define STATUS_LINE_COL (STATUS_POS_COL + 3)    /* 63 */
#define STATUS_COL_COL  (STATUS_POS_COL + 12)   /* 72 */

/* The static labels + trailing pad of the position field. */
static void draw_status_labels(void) {
    char sp = ' ';
    _cgfx_revon(MV_OUTPATH);
    _cgfx_curxy(MV_OUTPATH, STATUS_POS_COL, EDITOR_STATUS_ROW);
    cwrite(MV_OUTPATH, "Ln ", 3);
    _cgfx_curxy(MV_OUTPATH, STATUS_POS_COL + 7, EDITOR_STATUS_ROW);
    cwrite(MV_OUTPATH, " Col ", 5);
    _cgfx_curxy(MV_OUTPATH, STATUS_POS_COL + 16, EDITOR_STATUS_ROW);
    cwrite(MV_OUTPATH, &sp, 1);                  /* pad col 76 */
    _cgfx_revoff(MV_OUTPATH);
}

/* Rewrite whichever of the Ln/Col numbers changed, inside ONE revon/revoff. */
static void draw_status_nums(TextView *v) {
    int line = v->cur_line + 1, col = v->cur_col + 1;
    int dl = (line != v->status_line_shown);
    int dc = (col != v->status_col_shown);

    if (!dl && !dc) {
        return;
    }
    _cgfx_revon(MV_OUTPATH);
    if (dl) {
        _cgfx_curxy(MV_OUTPATH, STATUS_LINE_COL, EDITOR_STATUS_ROW);
        put_int_w(line, 4);
        v->status_line_shown = line;
    }
    if (dc) {
        _cgfx_curxy(MV_OUTPATH, STATUS_COL_COL, EDITOR_STATUS_ROW);
        put_int_w(col, 4);
        v->status_col_shown = col;
    }
    _cgfx_revoff(MV_OUTPATH);
}
#endif /* SHOW_STATUS_POS */

/* Full status redraw: name, dirty marker, and (if enabled) the Ln/Col field. */
static void draw_status_full(TextView *v) {
    draw_status_left(v);
    v->status_dirty_shown = v->doc_modified ? 1 : 0;
#if SHOW_STATUS_POS
    draw_status_labels();
    v->status_line_shown = v->status_col_shown = -1;   /* force both numbers */
    draw_status_nums(v);
#endif
}

/* Incremental status: rewrite only the parts that changed. With the position
   field off this is a no-op unless the dirty marker flipped, so a keystroke
   does no status work at all. */
static void draw_status_inc(TextView *v) {
    int dirty = v->doc_modified ? 1 : 0;
    if (dirty != v->status_dirty_shown) {
        draw_status_left(v);
        v->status_dirty_shown = dirty;
    }
#if SHOW_STATUS_POS
    draw_status_nums(v);
#endif
}

static void draw_caret(const TextView *v) {
    int sc = v->cur_col - v->left_col;
    int sr = v->cur_line - v->top_line;
    if (sc >= 0 && sc < EDITOR_COLS && sr >= 0 && sr < EDITOR_ROWS) {
        _cgfx_curxy(MV_OUTPATH, sc, sr);
        _cgfx_curon(MV_OUTPATH);
    } else {
        _cgfx_curoff(MV_OUTPATH);
    }
}

/* Bracket every repaint with these. The graphics mouse pointer is composited
   over the text, so while it is on, every cwrite under it forces the driver to
   save/restore the area -- expensive. Turn it off for the duration of the write
   and back on (as the I-beam text pointer) afterward. The text caret is hidden
   the same way. setgc takes effect immediately, so the pointer goes back on
   *after* the Flush that paints the text. */
static void render_begin(void) {
    _cgfx_setgc(MV_OUTPATH, 0, 0);        /* mouse pointer off */
    _cgfx_curoff(MV_OUTPATH);             /* text caret off */
}

static void render_end(const TextView *v) {
    draw_caret(v);                        /* text caret back at the cursor */
    _cgfx_setgc(MV_OUTPATH, GRP_PTR, PTR_TXT);   /* I-beam pointer back on */
    Flush();   /* one flush sends: pointer-off, text, pointer-on (in order) */
}

static void text_view_draw(MVView *mv) {
    TextView *v = (TextView *)mv;
    int r;

    render_begin();
    for (r = 0; r < EDITOR_ROWS; ++r) {
        draw_text_row(v, r);
    }
    draw_status_full(v);
    render_end(v);
}


/* ---- partial repaint ----------------------------------------------------
   The CoCo is slow, so most edits touch only a row or two. These repaint just
   the changed screen rows (plus the always-cheap status line and the hardware
   caret), instead of all EDITOR_ROWS. The full text_view_draw is reserved for
   scrolls, selection changes spanning the view, and document swaps. */

/* How much of the text area an edit dirtied. */
typedef enum {
    RS_CARET,   /* text unchanged: only the cursor moved (status + caret)     */
    RS_ROW,     /* only the cursor's own line changed                         */
    RS_FROM,    /* the cursor's line and everything below it shifted/changed  */
    RS_ALL      /* the whole text area                                        */
} RefreshScope;

/* Repaint screen rows [sr0, sr1] (clamped) plus status + caret. */
static void repaint_rows(TextView *v, int sr0, int sr1) {
    int sr;
    if (sr0 < 0) sr0 = 0;
    if (sr1 > EDITOR_ROWS - 1) sr1 = EDITOR_ROWS - 1;
    render_begin();
    for (sr = sr0; sr <= sr1; ++sr) {
        draw_text_row(v, sr);
    }
    draw_status_inc(v);
    render_end(v);
}

/* Apply a refresh scope. `from_line` is the document line for RS_FROM. */
static void apply_refresh(TextView *v, RefreshScope scope, int from_line) {
    if (v->suppress_draw) {
        return;   /* batch in progress: end-of-batch does one repaint */
    }
    switch (scope) {
        case RS_CARET:
            render_begin();
            draw_status_inc(v);
            render_end(v);
            break;
        case RS_ROW:
            repaint_rows(v, v->cur_line - v->top_line, v->cur_line - v->top_line);
            break;
        case RS_FROM: {
            /* Only rows from the edit down to the first blank row after the
               document's content changed; everything lower is already blank.
               (num_lines - top_line is that first blank row, which also covers
               clearing the row vacated by a line-join.) */
            int last = textdoc_num_lines(v->doc) - v->top_line;
            if (last > EDITOR_ROWS - 1) {
                last = EDITOR_ROWS - 1;
            }
            repaint_rows(v, from_line - v->top_line, last);
            break;
        }
        case RS_ALL:
            text_view_draw(&v->view);
            break;
    }
}

/* Fastest path -- a same-line edit with no selection: repaint only screen
   columns [sc0, sc1) of the cursor's row (the chars that actually shifted),
   plus the position field and caret. Typing at the end of a line repaints a
   single cell instead of all EDITOR_COLS. */
static void repaint_row_span(TextView *v, int sc0, int sc1) {
    int srow = v->cur_line - v->top_line;
    if (v->suppress_draw) {
        return;
    }
    sc0 = clampi(sc0, 0, EDITOR_COLS);
    sc1 = clampi(sc1, 0, EDITOR_COLS);
    render_begin();
    if (srow >= 0 && srow < EDITOR_ROWS && sc1 > sc0) {
        char buf[EDITOR_COLS];
        int d0, d1;
        build_row(v, v->cur_line, buf, &d0, &d1);
        _cgfx_curxy(MV_OUTPATH, sc0, srow);
        cwrite(MV_OUTPATH, buf + sc0, sc1 - sc0);
    }
    draw_status_inc(v);
    render_end(v);
}


/* ---- change bracketing -------------------------------------------------- */

static void vc_begin(TextView *v) {
    if (v->suppress_draw) {
        /* In a batch, take the undo snapshot only once (before the first edit),
           so Undo reverts the whole burst and we skip N-1 whole-doc copies. */
        if (v->batch_snapshotted) {
            return;
        }
        v->batch_snapshotted = 1;
    }
    if (v->will_change) {
        v->will_change(v);
    }
}

static void vc_end(TextView *v) {
    if (v->did_change) {
        v->did_change(v);
    }
}


/* ---- selection / clipboard --------------------------------------------- */

/* Delete the current selection (caller has begun a change). Cursor collapses to
   the range start; returns true if anything was removed. */
static int delete_selection(TextView *v) {
    int l0, c0, l1, c1;
    if (!has_selection(v)) {
        return 0;
    }
    selection_range(v, &l0, &c0, &l1, &c1);
    textdoc_delete_range(v->doc, l0, c0, l1, c1);
    v->cur_line = v->sel_line = l0;
    v->cur_col = v->sel_col = c0;
    return 1;
}

int text_view_has_selection(const TextView *v) {
    return has_selection(v);
}

int text_view_has_clip(const TextView *v) {
    return v->clip_len > 0;
}

void text_view_select_all(TextView *v) {
    int last = textdoc_num_lines(v->doc) - 1;
    v->sel_line = 0;
    v->sel_col = 0;
    v->cur_line = last;
    v->cur_col = line_len(v, last);
    ensure_visible(v);
    text_view_refresh(v);
}

void text_view_copy(TextView *v) {
    int l0, c0, l1, c1;
    if (!has_selection(v)) {
        v->clip_len = 0;
        return;
    }
    selection_range(v, &l0, &c0, &l1, &c1);
    textdoc_get_range(v->doc, l0, c0, l1, c1, v->clip, ED_CLIP_SZ);
    v->clip_len = strlen(v->clip);
}

void text_view_cut(TextView *v) {
    if (!has_selection(v)) {
        return;
    }
    text_view_copy(v);
    vc_begin(v);
    delete_selection(v);
    ensure_visible(v);
    vc_end(v);
    text_view_refresh(v);
}

void text_view_paste(TextView *v) {
    if (v->clip_len <= 0) {
        return;
    }
    vc_begin(v);
    delete_selection(v);
    textdoc_insert_text(v->doc, &v->cur_line, &v->cur_col, v->clip);
    v->sel_line = v->cur_line;
    v->sel_col = v->cur_col;
    ensure_visible(v);
    vc_end(v);
    text_view_refresh(v);
}

void text_view_delete(TextView *v) {
    vc_begin(v);
    if (has_selection(v)) {
        delete_selection(v);
    } else {
        /* forward delete: char at cursor, or pull up the next line */
        if (v->cur_col < line_len(v, v->cur_line)) {
            textdoc_delete_char(v->doc, v->cur_line, v->cur_col);
        } else {
            textdoc_join_line(v->doc, v->cur_line);
        }
    }
    ensure_visible(v);
    vc_end(v);
    text_view_refresh(v);
}


/* ---- keyboard ----------------------------------------------------------- */

/* Collapse the selection to the cursor (movement keys drop the range). */
static void drop_selection(TextView *v) {
    v->sel_line = v->cur_line;
    v->sel_col = v->cur_col;
}

/* A cursor move: scroll if needed (full repaint), else clear any old selection
   (full repaint) or just move the caret. */
static void after_move(TextView *v, int had_selection) {
    drop_selection(v);
    if (ensure_visible(v)) {
        apply_refresh(v, RS_ALL, 0);
    } else {
        apply_refresh(v, had_selection ? RS_ALL : RS_CARET, 0);
    }
}

static void move_up(TextView *v) {
    int had = has_selection(v);
    if (v->cur_line > 0) {
        --v->cur_line;
        if (v->cur_col > line_len(v, v->cur_line)) {
            v->cur_col = line_len(v, v->cur_line);
        }
    }
    after_move(v, had);
}

static void move_down(TextView *v) {
    int had = has_selection(v);
    if (v->cur_line < textdoc_num_lines(v->doc) - 1) {
        ++v->cur_line;
        if (v->cur_col > line_len(v, v->cur_line)) {
            v->cur_col = line_len(v, v->cur_line);
        }
    }
    after_move(v, had);
}

static void move_right(TextView *v) {
    int had = has_selection(v);
    if (v->cur_col < line_len(v, v->cur_line)) {
        ++v->cur_col;
    } else if (v->cur_line < textdoc_num_lines(v->doc) - 1) {
        ++v->cur_line;
        v->cur_col = 0;
    }
    after_move(v, had);
}

/* The document line at the top of the region an edit may have dirtied: a
   selection's first line if one was active, else the cursor's line. */
static int edit_from_line(TextView *v, int had_selection) {
    if (had_selection) {
        int l0, c0, l1, c1;
        selection_range(v, &l0, &c0, &l1, &c1);
        return l0;
    }
    return v->cur_line;
}

static void type_char(TextView *v, char c) {
    int had = has_selection(v);
    int from = edit_from_line(v, had);
    int insert_col = v->cur_col;   /* where the char goes (no selection case) */

    vc_begin(v);
    delete_selection(v);
    if (textdoc_insert_char(v->doc, v->cur_line, v->cur_col, c)) {
        ++v->cur_col;
    }
    drop_selection(v);
    vc_end(v);

    if (ensure_visible(v)) {
        apply_refresh(v, RS_ALL, 0);
    } else if (had) {
        apply_refresh(v, RS_FROM, from);   /* replaced selection shifted lines */
    } else {
        /* Plain insert: only [insert_col, new_len) on this row changed (one cell
           when typing at the end of the line). */
        int new_len = line_len(v, v->cur_line);
        repaint_row_span(v, insert_col - v->left_col, new_len - v->left_col);
    }
}

static void do_enter(TextView *v) {
    int had = has_selection(v);
    int from = edit_from_line(v, had);

    vc_begin(v);
    delete_selection(v);
    if (textdoc_split_line(v->doc, v->cur_line, v->cur_col)) {
        ++v->cur_line;
        v->cur_col = 0;
    }
    drop_selection(v);
    vc_end(v);

    if (ensure_visible(v)) {
        apply_refresh(v, RS_ALL, 0);
    } else {
        apply_refresh(v, RS_FROM, from);   /* split shifts everything below */
    }
}

static void do_backspace(TextView *v) {
    int had = has_selection(v);
    int from = edit_from_line(v, had);
    int joined = 0;
    int old_len = line_len(v, v->cur_line);   /* before the edit (no-sel case) */
    int del_col = -1;

    vc_begin(v);
    if (had) {
        delete_selection(v);
    } else if (v->cur_col > 0) {
        textdoc_delete_char(v->doc, v->cur_line, v->cur_col - 1);
        --v->cur_col;
        del_col = v->cur_col;
    } else if (v->cur_line > 0) {
        int prev = v->cur_line - 1;
        int joincol = line_len(v, prev);
        if (textdoc_join_line(v->doc, prev)) {
            v->cur_line = prev;
            v->cur_col = joincol;
            from = prev;
            joined = 1;
        }
    }
    drop_selection(v);
    vc_end(v);

    if (ensure_visible(v)) {
        apply_refresh(v, RS_ALL, 0);
    } else if (had || joined) {
        apply_refresh(v, RS_FROM, from);   /* lines below shifted up */
    } else if (del_col >= 0) {
        /* erased one char: [del_col, old_len) shifted left, last cell cleared */
        repaint_row_span(v, del_col - v->left_col, old_len - v->left_col);
    } else {
        apply_refresh(v, RS_CARET, 0);     /* nothing changed (col 0, line 0) */
    }
}

int text_view_key(TextView *v, char c) {
    switch (c) {
        case KEY_UP:        move_up(v);      return 1;
        case KEY_DOWN:      move_down(v);    return 1;
        case KEY_RIGHT:     move_right(v);   return 1;
        case KEY_ENTER:     do_enter(v);     return 1;
        case KEY_BACKSPACE: do_backspace(v); return 1;
        default:
            if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F) {
                type_char(v, c);
                return 1;
            }
            return 0;       /* not for us */
    }
}


/* ---- mouse: click to place, drag to extend ------------------------------ */

/* Map a window-pixel mouse position to a clamped document (line, col). Mouse
   and text share the content-area origin, so cells are pixels/8. */
static void mouse_to_doc(const TextView *v, int wx, int wy, int *line, int *col) {
    int sr = clampi(wy / 8, 0, EDITOR_ROWS - 1);
    int sc = wx / 8;
    int ln = clampi(v->top_line + sr, 0, textdoc_num_lines(v->doc) - 1);
    int cl = clampi(v->left_col + sc, 0, line_len(v, ln));
    *line = ln;
    *col = cl;
}

static int text_view_handle_click(MVView *mv, MVUiEvent *event) {
    TextView *v = (TextView *)mv;
    MSRET mp;
    int line, col;

    if (event->event_type != MVUiEventType_MouseClick) {
        return 0;
    }
    mp = event->info.mouse;
    if (!mv_view_contains_point(mv, mp.pt_wrx, mp.pt_wry)) {
        return 0;       /* outside the text area (e.g. the status row) */
    }

    /* Press: place the cursor and start the selection anchor here. A click is
       always within the visible rows, so no scroll -- just move the caret, or
       repaint fully if there was an old selection to clear. */
    {
        int had = has_selection(v);
        mouse_to_doc(v, mp.pt_wrx, mp.pt_wry, &line, &col);
        v->cur_line = v->sel_line = line;
        v->cur_col = v->sel_col = col;
        apply_refresh(v, had ? RS_ALL : RS_CARET, 0);
    }

    /* Drag: extend the selection until button A releases, auto-scrolling when
       the pointer rides an edge. Only the rows between the cursor's old and new
       line need repainting (an edge scroll needs the whole area). */
    do {
        int nl, nc, edge = 0;
        int ocl = v->cur_line;
        _cgfx_gs_mouse(MV_OUTPATH, &mp);

        if (mp.pt_wry / 8 <= 0 && v->top_line > 0) {
            --v->top_line; edge = 1;
        } else if (mp.pt_wry / 8 >= EDITOR_ROWS - 1 &&
                   v->top_line + EDITOR_ROWS < textdoc_num_lines(v->doc)) {
            ++v->top_line; edge = 1;
        }
        if (mp.pt_wrx / 8 <= 0 && v->left_col > 0) {
            --v->left_col; edge = 1;
        } else if (mp.pt_wrx / 8 >= EDITOR_COLS - 1 && v->left_col < MAX_LEFT) {
            ++v->left_col; edge = 1;
        }

        mouse_to_doc(v, mp.pt_wrx, mp.pt_wry, &nl, &nc);
        if (nl != v->cur_line || nc != v->cur_col || edge) {
            v->cur_line = nl;
            v->cur_col = nc;
            if (edge) {
                update_scrollbars(v);
                text_view_draw(&v->view);
            } else {
                int a = (ocl < nl) ? ocl : nl;
                int b = (ocl > nl) ? ocl : nl;
                repaint_rows(v, a - v->top_line, b - v->top_line);
            }
        }
    } while (mp.pt_cbsa);

    return 1;
}


/* ---- scrollbar handlers ------------------------------------------------- */

void text_view_scroll(TextView *v, int dcol, int drow) {
    int nlines = textdoc_num_lines(v->doc);
    int vmax = (nlines > EDITOR_ROWS) ? nlines - EDITOR_ROWS : 0;

    v->top_line = clampi(v->top_line + drow, 0, vmax);
    v->left_col = clampi(v->left_col + dcol, 0, MAX_LEFT);
    update_scrollbars(v);
    text_view_refresh(v);
}


/* ---- public API --------------------------------------------------------- */

void text_view_init(TextView *v, int x, int y, int width, int height,
                    TextDoc *doc) {
    v->view.x = x;
    v->view.y = y;
    v->view.width = width;
    v->view.height = height;
    v->view.is_visible = true;
    v->view.draw = text_view_draw;
    v->view.handle_click = text_view_handle_click;

    v->doc = doc;
    v->cur_line = v->cur_col = 0;
    v->sel_line = v->sel_col = 0;
    v->top_line = v->left_col = 0;
    v->sbar_hor = v->sbar_ver = -1;   /* force the first thumb push */
    v->fg = 1;
    v->bg = 0;
    v->doc_name = 0;
    v->doc_modified = 0;
    v->status_dirty_shown = -1;        /* force the first status draws */
    v->status_line_shown = -1;
    v->status_col_shown = -1;
    v->suppress_draw = 0;
    v->batch_snapshotted = 0;
    v->will_change = 0;
    v->did_change = 0;
    v->clip_len = 0;
    v->clip[0] = 0;
}

void text_view_refresh(TextView *v) {
    text_view_draw(&v->view);
}

void text_view_begin_batch(TextView *v) {
    v->suppress_draw = 1;
    v->batch_snapshotted = 0;
}

void text_view_end_batch(TextView *v) {
    v->suppress_draw = 0;
    text_view_refresh(v);   /* one repaint for the whole drained run of keys */
}

void text_view_reset(TextView *v) {
    v->cur_line = v->cur_col = 0;
    v->sel_line = v->sel_col = 0;
    v->top_line = v->left_col = 0;
    v->status_dirty_shown = -1;        /* name may have changed; redraw it */
    update_scrollbars(v);
    text_view_refresh(v);
}

void text_view_reclamp(TextView *v) {
    int nlines = textdoc_num_lines(v->doc);
    v->cur_line = clampi(v->cur_line, 0, nlines - 1);
    v->cur_col = clampi(v->cur_col, 0, line_len(v, v->cur_line));
    v->sel_line = v->cur_line;
    v->sel_col = v->cur_col;
    ensure_visible(v);
    update_scrollbars(v);
    text_view_refresh(v);
}

void text_view_set_status(TextView *v, const char *name, int modified) {
    v->doc_name = name;
    v->doc_modified = modified;
}
