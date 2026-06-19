#ifndef _TEXTDOC_H_
#define _TEXTDOC_H_

#include <os.h>   /* error_code */

/*
 * mvedit's document model: an ordered list of text lines. The model is
 * framework-agnostic (it knows nothing about MVKit or cgfx) -- the app
 * (mvedit.c) wraps it in the file lifecycle and the editable view
 * (text_view.c) does the rendering and cursor/selection editing on top of it.
 *
 * Lines are NUL-terminated, fixed-width slots; num_lines is always >= 1 (an
 * "empty" document is one empty line). On disk a line is terminated by an OS-9
 * carriage return (0x0D), the NitrOS-9 text-file convention.
 *
 * The whole struct is a plain fixed-size value, so a snapshot for single-level
 * undo is just a struct assignment (`*a = *b`).
 */

#define ED_MAX_LINES 50              /* document capacity, in lines            */
#define ED_MAX_COLS  127             /* longest line, in characters           */
#define ED_LINE_SZ   (ED_MAX_COLS + 1)   /* slot size incl. the NUL           */

/* Line separator used inside an in-memory text run (clipboard / insert_text).
   Distinct from the on-disk 0x0D so the two conventions never get confused. */
#define ED_NL '\n'

typedef struct {
    int num_lines;                       /* always >= 1                       */
    char lines[ED_MAX_LINES][ED_LINE_SZ];
} TextDoc;


/* ---- lifecycle (used by the app's File menu) ---------------------------- */

/* Reset to a single empty line. Returns 0. */
extern error_code textdoc_new(TextDoc *doc, const char *filename);

/* Load from filename, splitting on 0x0D (0x0A tolerated). Over-long lines and
   over-capacity files are truncated. Falls back to empty on a read error. */
extern error_code textdoc_open(TextDoc *doc, const char *filename);

/* Write every line to filename, each terminated by 0x0D. */
extern error_code textdoc_save(const TextDoc *doc, const char *filename);


/* ---- queries ------------------------------------------------------------ */

extern int textdoc_num_lines(const TextDoc *doc);
extern int textdoc_line_len(const TextDoc *doc, int line);   /* 0 if out of range */
extern const char *textdoc_line(const TextDoc *doc, int line);  /* "" if out of range */


/* ---- editing primitives (return true on success / change) --------------- */

/* Insert character c into `line` at `col` (chars at/after col shift right).
   Fails if the line is already ED_MAX_COLS long. */
extern int textdoc_insert_char(TextDoc *doc, int line, int col, char c);

/* Delete the character at (line, col), shifting later chars left. */
extern int textdoc_delete_char(TextDoc *doc, int line, int col);

/* Split `line` at `col` into two lines (the Enter key). Fails if the document
   is already at ED_MAX_LINES. */
extern int textdoc_split_line(TextDoc *doc, int line, int col);

/* Append line (line+1) onto `line` and remove line+1. Fails if there is no
   following line or the join would overflow ED_MAX_COLS. */
extern int textdoc_join_line(TextDoc *doc, int line);

/* Insert a (possibly multi-line, ED_NL-separated) text run at *line/*col,
   advancing *line/*col to the end of the inserted text. */
extern int textdoc_insert_text(TextDoc *doc, int *line, int *col, const char *text);

/* Delete everything in the half-open range [(l0,c0), (l1,c1)). The range must
   be ordered (l0,c0) <= (l1,c1). Lines between are removed and the surviving
   prefix/suffix are joined. */
extern int textdoc_delete_range(TextDoc *doc, int l0, int c0, int l1, int c1);

/* Copy the text in [(l0,c0), (l1,c1)) into out (ED_NL between lines),
   NUL-terminated, never writing more than outsz bytes (incl. the NUL). */
extern void textdoc_get_range(const TextDoc *doc, int l0, int c0, int l1, int c1,
                              char *out, int outsz);

#endif /* _TEXTDOC_H_ */
