#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "textdoc.h"


/* ---- small helpers ------------------------------------------------------ */

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Overlap-safe copy: cmoc's libc has memcpy but not memmove, and our in-line
   character shifts overlap. Copies forward or backward as the direction needs. */
static void ed_memmove(char *dst, const char *src, int n) {
    int i;
    if (dst < src) {
        for (i = 0; i < n; ++i) {
            dst[i] = src[i];
        }
    } else if (dst > src) {
        for (i = n - 1; i >= 0; --i) {
            dst[i] = src[i];
        }
    }
}

/* Clamp a (line, col) reference so line is a real line and col is within it. */
static void clamp_pos(const TextDoc *doc, int *line, int *col) {
    *line = clampi(*line, 0, doc->num_lines - 1);
    *col = clampi(*col, 0, (int)strlen(doc->lines[*line]));
}


/* ---- lifecycle ---------------------------------------------------------- */

void textdoc_init(TextDoc *doc) {
    doc->num_lines = 1;
    doc->lines[0][0] = 0;
}

error_code textdoc_new(TextDoc *doc, const char *filename) {
    (void)filename;
    textdoc_init(doc);
    return 0;
}


/* Append char c to the line being built, dropping the overflow past
   ED_MAX_COLS. *len tracks the current length. */
static void build_putc(char *line, int *len, char c) {
    if (*len < ED_MAX_COLS) {
        line[*len] = c;
        ++(*len);
    }
}

error_code textdoc_open(TextDoc *doc, const char *filename) {
    char buf[256];
    int fd, n, i;
    int len;        /* length of the line currently being built */

    fd = open(filename, FAP_READ);
    if (fd < 0) {
        textdoc_init(doc);
        return errno;
    }

    doc->num_lines = 1;
    doc->lines[0][0] = 0;
    len = 0;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < n; ++i) {
            char c = buf[i];
            if (c == 0x0D || c == 0x0A) {     /* OS-9 CR (LF tolerated) */
                doc->lines[doc->num_lines - 1][len] = 0;
                if (doc->num_lines >= ED_MAX_LINES) {
                    close(fd);
                    return 0;                 /* file longer than capacity */
                }
                doc->lines[doc->num_lines][0] = 0;
                ++doc->num_lines;
                len = 0;
            } else {
                build_putc(doc->lines[doc->num_lines - 1], &len, c);
            }
        }
    }
    doc->lines[doc->num_lines - 1][len] = 0;

    /* A trailing newline produced an extra empty line; that is fine and matches
       most editors (a file ending in CR has a blank final line). */
    close(fd);
    return 0;
}

error_code textdoc_save(const TextDoc *doc, const char *filename) {
    int fd, i;
    error_code retval = 0;
    char cr = 0x0D;

    fd = creat(filename, FAP_WRITE | FAP_READ);
    if (fd < 0) {
        return errno;
    }
    for (i = 0; i < doc->num_lines; ++i) {
        int ll = strlen(doc->lines[i]);
        if (ll > 0 && write(fd, doc->lines[i], ll) != ll) {
            retval = errno ? errno : E$Write;
            break;
        }
        if (write(fd, &cr, 1) != 1) {        /* OS-9 line terminator */
            retval = errno ? errno : E$Write;
            break;
        }
    }
    close(fd);
    return retval;
}


/* ---- queries ------------------------------------------------------------ */

int textdoc_num_lines(const TextDoc *doc) {
    return doc->num_lines;
}

int textdoc_line_len(const TextDoc *doc, int line) {
    if (line < 0 || line >= doc->num_lines) {
        return 0;
    }
    return strlen(doc->lines[line]);
}

const char *textdoc_line(const TextDoc *doc, int line) {
    if (line < 0 || line >= doc->num_lines) {
        return "";
    }
    return doc->lines[line];
}


/* ---- editing primitives ------------------------------------------------- */

int textdoc_insert_char(TextDoc *doc, int line, int col, char c) {
    char *s;
    int len;

    if (line < 0 || line >= doc->num_lines) {
        return 0;
    }
    s = doc->lines[line];
    len = strlen(s);
    if (len >= ED_MAX_COLS) {
        return 0;                       /* line full */
    }
    col = clampi(col, 0, len);
    ed_memmove(s + col + 1, s + col, len - col + 1);   /* +1 moves the NUL too */
    s[col] = c;
    return 1;
}

int textdoc_delete_char(TextDoc *doc, int line, int col) {
    char *s;
    int len;

    if (line < 0 || line >= doc->num_lines) {
        return 0;
    }
    s = doc->lines[line];
    len = strlen(s);
    if (col < 0 || col >= len) {
        return 0;
    }
    ed_memmove(s + col, s + col + 1, len - col);       /* moves the NUL too */
    return 1;
}

/* Make room for one more line at index `at` (lines at/after `at` shift down).
   Caller has checked num_lines < ED_MAX_LINES. */
static void open_line_slot(TextDoc *doc, int at) {
    int i;
    for (i = doc->num_lines; i > at; --i) {
        memcpy(doc->lines[i], doc->lines[i - 1], ED_LINE_SZ);
    }
    ++doc->num_lines;
}

/* Remove line `at` (lines after it shift up). num_lines stays >= 1. */
static void remove_line_slot(TextDoc *doc, int at) {
    int i;
    if (doc->num_lines <= 1) {
        doc->lines[0][0] = 0;
        return;
    }
    for (i = at; i < doc->num_lines - 1; ++i) {
        memcpy(doc->lines[i], doc->lines[i + 1], ED_LINE_SZ);
    }
    --doc->num_lines;
}

int textdoc_split_line(TextDoc *doc, int line, int col) {
    char *s;
    int len;

    if (line < 0 || line >= doc->num_lines) {
        return 0;
    }
    if (doc->num_lines >= ED_MAX_LINES) {
        return 0;
    }
    s = doc->lines[line];
    len = strlen(s);
    col = clampi(col, 0, len);

    open_line_slot(doc, line + 1);
    strcpy(doc->lines[line + 1], s + col);   /* tail -> new line */
    s[col] = 0;                              /* truncate this line at the cut */
    return 1;
}

int textdoc_join_line(TextDoc *doc, int line) {
    int len, nlen;

    if (line < 0 || line >= doc->num_lines - 1) {
        return 0;                            /* no following line */
    }
    len = strlen(doc->lines[line]);
    nlen = strlen(doc->lines[line + 1]);
    if (len + nlen > ED_MAX_COLS) {
        return 0;                            /* would overflow */
    }
    strcpy(doc->lines[line] + len, doc->lines[line + 1]);
    remove_line_slot(doc, line + 1);
    return 1;
}

int textdoc_insert_text(TextDoc *doc, int *line, int *col, const char *text) {
    const char *p;
    int changed = 0;

    for (p = text; *p; ++p) {
        if (*p == ED_NL) {
            if (textdoc_split_line(doc, *line, *col)) {
                ++(*line);
                *col = 0;
                changed = 1;
            }
        } else {
            if (textdoc_insert_char(doc, *line, *col, *p)) {
                ++(*col);
                changed = 1;
            }
        }
    }
    return changed;
}

int textdoc_delete_range(TextDoc *doc, int l0, int c0, int l1, int c1) {
    clamp_pos(doc, &l0, &c0);
    clamp_pos(doc, &l1, &c1);

    if (l0 == l1 && c0 == c1) {
        return 0;                            /* empty range */
    }

    if (l0 == l1) {
        char *s = doc->lines[l0];
        int len = strlen(s);
        if (c1 > len) c1 = len;
        ed_memmove(s + c0, s + c1, len - c1 + 1);   /* close the gap incl. NUL */
        return 1;
    }

    /* Multi-line: keep l0's prefix, graft l1's suffix onto it, drop the
       lines in between (and l1). */
    {
        char tail[ED_LINE_SZ];
        char *s0 = doc->lines[l0];
        int suffix_len;

        strcpy(tail, doc->lines[l1] + c1);
        s0[c0] = 0;
        suffix_len = strlen(tail);
        if (c0 + suffix_len > ED_MAX_COLS) {
            tail[ED_MAX_COLS - c0] = 0;       /* clip an over-long merge */
        }
        strcpy(s0 + c0, tail);

        /* Remove lines l0+1 .. l1 (that many lines). */
        {
            int to_remove = l1 - l0;
            int k;
            for (k = 0; k < to_remove; ++k) {
                remove_line_slot(doc, l0 + 1);
            }
        }
    }
    return 1;
}

void textdoc_get_range(const TextDoc *doc, int l0, int c0, int l1, int c1,
                       char *out, int outsz) {
    int li, w = 0;

    clamp_pos(doc, &l0, &c0);
    clamp_pos(doc, &l1, &c1);

    for (li = l0; li <= l1; ++li) {
        const char *s = doc->lines[li];
        int len = strlen(s);
        int start = (li == l0) ? c0 : 0;
        int end = (li == l1) ? c1 : len;
        int i;
        if (end > len) end = len;
        for (i = start; i < end && w < outsz - 1; ++i) {
            out[w++] = s[i];
        }
        if (li < l1 && w < outsz - 1) {
            out[w++] = ED_NL;
        }
    }
    out[w] = 0;
}
