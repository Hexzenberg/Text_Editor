// Run Make to recompile.
// ./kilo to run the app.
// Ctrl clears the 6th and 7th bits of the character typed in combination with it!

/************************************************************  Includes ************************************************************/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>

/************************************************************ Defines ************************************************************/
#define KILO_VERSION "0.0.1"
#define CTRL_KEY(K) ((K) & 0x1f) // To map ctrl+[any key] to some other specific function
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3
#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

enum editorKey
{
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    PAGE_UP,
    PAGE_DOWN,
    HOME_KEY,
    END_KEY,
    DEL_KEY,
    BACKSPACE = 127
};

enum editorHighlight
{
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

/************************************************************ Data ************************************************************/
struct editorSyntax
{
    char *filetype;
    char **filematch;
    char **keywords;
    char *singleline_comment_start;
    char *multiline_comment_start;
    char *multiline_comment_end;
    int flags;
};

typedef struct erow
{
    int idx;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;
    int hl_open_comment;
} erow;

struct editorConfig
{
    int cx, cy;
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    struct termios orig_termios;
    int rowoff;
    int coloff;
    int rx;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    int dirty;
    struct editorSyntax *syntax;
};

struct editorConfig E;

/************************************************************ File Types ************************************************************/
char *C_HL_extensions[] = {".c", ".h", ".cpp", NULL};

char *C_HL_keywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "class", "case",

    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", NULL};

struct editorSyntax HLDB[] = {
    {"c",
     C_HL_extensions,
     C_HL_keywords,
     "//", "/*", "*/",
     HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS},
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/************************************************************ Prototypes ************************************************************/
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/************************************************************ Terminal ************************************************************/
void die(const char *s) // In case of error
{
    write(STDOUT_FILENO, "\x1b[2J]", 4);
    write(STDOUT_FILENO, "\x1b[H]", 3);

    perror(s);
    exit(1);
}

void disableRawMode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode()
{
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr");

    atexit(disableRawMode); // Exiting the raw mode while exiting the code

    // Changing the local flag to do the following
    // Removing echo because it annoys in raw mode
    // Turning off canon mode to read byte by byte instead of line by line
    struct termios raw = E.orig_termios;
    // IXON is a input flag hence it affects the input flag and not the local flag
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // Disables ctrl+s and ctrl+q and ctrl+m
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);          // Turning off ctrl+c, ctrl+z, ctrl+v
    raw.c_oflag &= ~(OPOST);                                  // Turning off output processing
    raw.c_cflag |= (CS8);
    raw.c_cc[VMIN] = 0;  // VMIN value sets the minimum number of bytes of input needed before read() can return
    raw.c_cc[VTIME] = 1; // VTIME value sets the maximum amount of time to wait before read() returns

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

int editorReadKey()
{
    int nread;
    char c;

    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
    {
        if (nread == -1 && errno != EAGAIN)
            die("read");
    }

    if (c == '\x1b')
    {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1)
        {
            return '\x1b';
        }
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
        {
            return '\x1b';
        }

        if (seq[0] == '[')
        {
            if (seq[1] >= '0' && seq[1] <= '9')
            {
                if (read(STDIN_FILENO, &seq[2], 1))
                {
                    return '\x1b';
                }
                if (seq[2] == '~')
                {
                    switch (seq[1])
                    {
                    case '1':
                        return HOME_KEY;
                    case '3':
                        return DEL_KEY;
                    case '4':
                        return END_KEY;
                    case '5':
                        return PAGE_UP;
                    case '6':
                        return PAGE_DOWN;
                    case '7':
                        return HOME_KEY;
                    case '8':
                        return END_KEY;
                    }
                }
            }
            else
            {
                switch (seq[1])
                {
                case 'A':
                    return ARROW_UP;
                case 'B':
                    return ARROW_DOWN;
                case 'C':
                    return ARROW_RIGHT;
                case 'D':
                    return ARROW_LEFT;
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
                }
            }
        }
        else if (seq[0] == 'O')
        {
            switch (seq[1])
            {
            case 'H':
                return HOME_KEY;
            case 'F':
                return END_KEY;
            }
        }

        return '\x1b';
    }
    else
    {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols)
{
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }

    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;

    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) // C to move cursor forward and B to move it towards right
            return -1;

        return getCursorPosition(rows, cols);
    }
    else
    {
        *cols = ws.ws_col;
        *rows = ws.ws_row;

        return 0;
    }
}
/************************************************************ Syntax Highlighting ************************************************************/
int is_separator(int c)
{
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(erow *row)
{
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    if (E.syntax == NULL)
    {
        return;
    }

    char **keywords = E.syntax->keywords;
    char *scs = E.syntax->singleline_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;
    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;

    int prev_sep = 1, i = 0, in_string = 0;
    int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);

    while (i < row->rsize)
    {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        if (scs_len && !in_string && !in_comment)
        {
            if (!strncmp(&row->render[i], scs, scs_len))
            {
                memset(&row->hl[i], HL_COMMENT, row->rsize - i);
                break;
            }
        }

        if (mcs_len && mce_len && !in_string)
        {
            if (in_comment)
            {
                row->hl[i] = HL_MLCOMMENT;

                if (!strncmp(&row->render[i], mce, mce_len))
                {
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                }
                else
                {
                    i++;
                    continue;
                }
            }
            else if (!strncmp(&row->render[i], mcs, mcs_len))
            {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS)
        {
            if (in_string)
            {
                row->hl[i] = HL_STRING;

                if (c == '\\' && i + 1 < row->rsize)
                {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }

                if (c == in_string)
                {
                    in_string = 0;
                }

                i++;
                prev_sep = 1;
                continue;
            }
            else
            {
                if (c == '"' || c == '\'')
                {
                    in_string = c;
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS)
        {
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) || (c == '.' && prev_hl == HL_NUMBER))
            {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        if (prev_sep)
        {
            int j;

            for (j = 0; keywords[j]; j++)
            {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';

                if (kw2)
                {
                    klen--;
                }

                if (!strncmp(&row->render[i], keywords[j], klen) && is_separator(row->render[i + klen]))
                {
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }
            }

            if (keywords[j] != NULL)
            {
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = is_separator(c);
        i++;
    }

    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;

    if (changed && row->idx + 1 < E.numrows)
    {
        editorUpdateSyntax(&E.row[row->idx + 1]);
    }
}

int editorSyntaxToColor(int hl)
{
    // Returning ANSCII codes for colours
    switch (hl)
    {
    case HL_NUMBER:
        return 31;
    case HL_MATCH:
        return 34;
    case HL_STRING:
        return 35;
    case HL_KEYWORD1:
        return 33;
    case HL_KEYWORD2:
        return 32;
    case HL_COMMENT:
    case HL_MLCOMMENT:
        return 36;
    default:
        return 37;
    }
}

void editorSelectSyntaxHighlight()
{
    E.syntax = NULL;

    if (E.filename == NULL)
    {
        return;
    }

    char *ext = strrchr(E.filename, '.');

    for (unsigned int j = 0; j < HLDB_ENTRIES; j++)
    {
        struct editorSyntax *s = &HLDB[j];
        unsigned int i = 0;

        while (s->filematch[i])
        {
            int is_ext = (s->filematch[i][0] == '.');

            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) || (!is_ext && strstr(E.filename, s->filematch[i])))
            {
                E.syntax = s;

                for (int filerow = 0; filerow < E.numrows; filerow++)
                {
                    editorUpdateSyntax(&E.row[filerow]);
                }

                return;
            }

            i++;
        }
    }
}

/************************************************************ Row Operations ************************************************************/
int editorRowCxToRx(erow *row, int cx)
{
    int rx = 0;

    for (int j = 0; j < cx; j++)
    {
        if (row->chars[j] == '\t')
        {
            rx += (KILO_TAB_STOP - 1) - rx % KILO_TAB_STOP;
        }

        rx++;
    }
    return rx;
}

int editorRowRxToCx(erow *row, int rx)
{
    int cur_rx = 0, cx;

    for (cx = 0; cx < row->size; cx++)
    {
        if (row->chars[cx] == '\t')
        {
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
        }

        cur_rx++;

        if (cur_rx > rx)
        {
            return cx;
        }
    }

    return cx;
}

void editorUpdateRow(erow *row)
{
    int tabs = 0;

    for (int j = 0; j < row->size; j++)
    {
        if (row->chars[j] == '\t')
        {
            tabs++;
        }
    }

    free(row->render);
    row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);

    int idx = 0;

    for (int j = 0; j < row->size; j++)
    {
        if (row->chars[j] == '\t')
        {
            row->render[idx++] = ' ';
            while (idx % KILO_TAB_STOP != 0)
                row->render[idx++] = ' ';
        }
        else
        {
            row->render[idx++] = row->chars[j];
        }
    }

    row->render[idx] = '\0';
    row->rsize = idx;

    editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len)
{
    if (at < 0 || at > E.numrows)
    {
        return;
    }

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
    for (int j = at + 1; j <= E.numrows; j++)
    {
        E.row[j].idx++;
    }
    E.row[at].idx = at;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    E.row[at].hl_open_comment = 0;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row)
{
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editorDelRow(int at)
{
    if (at < 0 || at >= E.numrows)
    {
        return;
    }

    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));

    for (int j = at; j < E.numrows - 1; j++)
    {
        E.row[j].idx--;
    }

    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c)
{
    if (at < 0 || at > row->size)
    {
        at = row->size;
    }

    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1); // memmove is like memcpy but safer when source and destination arrays overlap
    row->size++;
    row->chars[at] = c;

    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len)
{
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at)
{
    if (at < 0 || at >= row->size)
    {
        return;
    }

    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/************************************************************ Editor Operations ************************************************************/
void editorInsertChar(int c)
{
    if (E.cy == E.numrows)
    {
        editorInsertRow(E.numrows, "", 0);
    }

    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewline()
{
    if (E.cx == 0)
    {
        editorInsertRow(E.cy, "", 0);
    }
    else
    {
        erow *row = &E.row[E.cy];

        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }

    E.cy++;
    E.cx = 0;
}

void editorDelChar()
{
    if (E.cy == E.numrows)
    {
        return;
    }

    if (E.cx == 0 && E.cy == 0)
    {
        return;
    }

    erow *row = &E.row[E.cy];

    if (E.cx > 0)
    {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    }
    else
    {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/************************************************************ File i/o ************************************************************/
char *editorRowsToString(int *buflen)
{
    int totlen = 0;

    for (int j = 0; j < E.numrows; j++)
    {
        totlen += E.row[j].size + 1;
    }

    *buflen = totlen;
    char *buf = malloc(totlen), *p = buf;

    for (int j = 0; j < E.numrows; j++)
    {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorOpen(char *filename)
{
    free(E.filename);
    E.filename = strdup(filename); // makes a copy of the given string, allocating the required memory and assumes that free() will free that allocated memory

    editorSelectSyntaxHighlight();

    FILE *fp = fopen(filename, "r");

    if (!fp)
        die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    while ((linelen = getline(&line, &linecap, fp)) != -1)
    {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;

        editorInsertRow(E.numrows, line, linelen);
    }

    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave()
{
    if (E.filename == NULL)
    {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);

        if (E.filename == NULL)
        {
            editorSetStatusMessage("Save aborted");
            return;
        }

        editorSelectSyntaxHighlight();
    }

    int len;
    char *buf = editorRowsToString(&len);
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);

    if (fd != -1)
    {
        if (ftruncate(fd, len) != -1)
        {
            if (write(fd, buf, len) == len)
            {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/************************************************************ Find ************************************************************/
void editorFindCallback(char *query, int key)
{
    static int last_match = -1, direction = 1, saved_hl_line;
    static char *saved_hl = NULL;

    if (saved_hl)
    {
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }

    if (key == '\r' || key == '\x1b')
    {
        last_match = -1;
        direction = 1;
        return;
    }
    else if (key == ARROW_RIGHT || key == ARROW_DOWN)
    {
        direction = 1;
    }
    else if (key == ARROW_LEFT || key == ARROW_UP)
    {
        direction = -1;
    }
    else
    {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1)
    {
        direction = 1;
    }

    int current = last_match;

    for (int i = 0; i < E.numrows; i++)
    {
        current += direction;

        if (current == -1)
        {
            current = E.numrows - 1;
        }
        else if (current == E.numrows)
        {
            current = 0;
        }

        erow *row = &E.row[current];
        char *match = strstr(row->render, query);

        if (match)
        {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowoff = E.numrows;

            // Saved highlight colours to be loaded after completing the search
            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editorFind()
{
    // Move back to the original position in case the user presses ESC or Enter to exit search
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;
    char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);

    if (query)
    {
        free(query);
    }
    else
    {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
}

/************************************************************ Append Buffer ************************************************************/
struct abuf
{
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

// Removing the many number of calls for the write() function
void abAppend(struct abuf *ab, const char *s, int len)
{
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL)
        return;

    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab)
{
    free(ab->b);
}

/************************************************************ Output ************************************************************/
void editorScroll()
{
    E.rx = 0;
    if (E.cy < E.numrows)
    {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.rowoff)
    {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows)
    {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.rx < E.coloff)
    {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols)
    {
        E.coloff = E.rx - E.screencols + 1;
    }
}

void editorDrawRows(struct abuf *ab)
{
    for (int y = 0; y < E.screenrows; y++)
    {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows)
        {
            if (E.numrows == 0 && y == E.screenrows / 3)
            {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);

                if (welcomelen > E.screencols)
                {
                    welcomelen = E.screencols;
                }

                int padding = (E.screencols - welcomelen) / 2; // Where to start printing the welcome message

                if (padding)
                {
                    abAppend(ab, "~", 1);
                    padding--;
                }

                while (padding--)
                {
                    abAppend(ab, " ", 1);
                }

                abAppend(ab, welcome, welcomelen);
            }
            else
            {
                abAppend(ab, "~", 1); // Writing '~' in every new line
            }
        }
        else
        {
            int len = E.row[filerow].rsize - E.coloff;

            if (len < 0)
            {
                len = 0;
            }

            if (len > E.screencols)
            {
                len = E.screencols;
            }

            char *c = &E.row[filerow].render[E.coloff];
            unsigned char *hl = &E.row[filerow].hl[E.coloff];
            int current_color = -1;

            for (int j = 0; j < len; j++)
            {
                if (iscntrl(c[j]))
                {
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';

                    abAppend(ab, "\x1b[7m", 4);
                    abAppend(ab, &sym, 1);
                    abAppend(ab, "\x1b[m", 3);

                    if (current_color != -1)
                    {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abAppend(ab, buf, clen);
                    }
                }
                else if (hl[j] == HL_NORMAL)
                {
                    if (current_color != -1)
                    {
                        abAppend(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                }
                else
                {
                    int color = editorSyntaxToColor(hl[j]);

                    if (color != current_color)
                    {
                        current_color = color;

                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);

                        abAppend(ab, buf, clen);
                    }

                    abAppend(ab, &c[j], 1);
                }
            }

            abAppend(ab, "\x1b[39m", 5);
        }

        abAppend(ab, "\x1b[K", 3); // K means erase in line; Similar behavious as H command

        abAppend(ab, "\r\n", 2); // If the row is not the final row then we move to a new line
    }
}

void editorDrawStatusBar(struct abuf *ab)
{
    abAppend(ab, "\x1b[7m", 4); // Escape sequence <esc>[7m switches to inverted colors, and <esc>[m switches back to normal formatting

    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[No Name]", E.numrows, E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d", E.syntax ? E.syntax->filetype : "no ft", E.cy + 1, E.numrows);

    if (len > E.screencols)
    {
        len = E.screencols;
    }

    abAppend(ab, status, len);

    while (len < E.screencols)
    {
        if (E.screencols - len == rlen)
        {
            abAppend(ab, rstatus, rlen);
            break;
        }
        else
        {
            abAppend(ab, " ", 1);
            len++;
        }
    }

    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab)
{
    abAppend(ab, "\x1b[K", 3);

    int msglen = strlen(E.statusmsg);

    if (msglen > E.screencols)
    {
        msglen = E.screencols;
    }

    if (msglen && time(NULL) - E.statusmsg_time < 5)
    {
        abAppend(ab, E.statusmsg, msglen);
    }
}

void editorRefreshScreen()
{
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6); // Hide mode for the cursor
    // \x1b is the Esc (escape) character
    // 'J' command clears the screen, escape character sequence takes arguments before the other command; which is 2 here
    // 2 which says to clear the entire screen. <esc>[1J would clear the screen up to where the cursor is, and <esc>[0J from the cursor up to the end of the screen
    // 0 is the default value i.e. <esc>[J = <esc>[0J
    // abAppend(&ab, "\x1b[2J", 4); // 4 means writing 4 bytes out to the terminal

    // H means the positioning of thr cursor, it takes two arguments row and column for the cursor
    // <esc>[H acts as <esc>[1;1H (row;column (numbering starts from 1))
    abAppend(&ab, "\x1b[H", 3); // This esc sequence is only 3 bytes

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    // Specifying the exact position that we have to move to using the x and y co-ordinates
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6); // Set mode for the cursor

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) // '...' means variadic function which can take any number of arguments
{
    va_list ap;
    va_start(ap, fmt);
    // vsnprintf() helps make custom printf()-style function
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/************************************************************ Input ************************************************************/
char *editorPrompt(char *prompt, void (*callback)(char *, int))
{
    size_t bufsize = 128;
    char *buf = malloc(bufsize);
    size_t buflen = 0;
    buf[0] = '\0';

    while (1)
    {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();
        int c = editorReadKey();

        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
        {
            if (buflen != 0)
            {
                buf[--buflen] = '\0';
            }
        }
        else if (c == '\x1b')
        {
            editorSetStatusMessage("");
            if (callback)
            {
                callback(buf, c);
            }
            free(buf);
            return NULL;
        }
        else if (c == '\r')
        {
            if (buflen != 0)
            {
                editorSetStatusMessage("");
                if (callback)
                {
                    callback(buf, c);
                }
                return buf;
            }
        }
        else if (!iscntrl(c) && c < 128)
        {
            if (buflen == bufsize - 1)
            {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }

            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
        if (callback)
        {
            callback(buf, c);
        }
    }
}

void editorMoveCursor(int key)
{
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key)
    {
    case ARROW_LEFT:
        if (E.cx != 0)
        {
            E.cx--; // Move Left; Decrease the horizontal (x) co-ordinate
        }
        else if (E.cy > 0)
        {
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        break;
    case ARROW_UP:
        if (E.cy != 0)
        {
            E.cy--; // Move Up; Decrease the vertical (y) co-ordinate
        }
        break;
    case ARROW_DOWN:
        if (E.cy != E.numrows)
        {
            E.cy++; // Move Down; Increase the vertical (y) co-ordinate
        }
        break;
    case ARROW_RIGHT:
        if (row && E.cx < row->size)
        {
            E.cx++; // Move Right; Increase the horizontal (x) co-ordinate
        }
        else if (row && E.cx == row->size)
        {
            E.cy++;
            E.cx = 0;
        }
        break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;

    if (E.cx > rowlen)
    {
        E.cx = rowlen;
    }
}

void editorProcessKeypress()
{
    static int quit_times = KILO_QUIT_TIMES;
    int c = editorReadKey();

    switch (c)
    {
    case '\r':
        editorInsertNewline();
        break;

    case CTRL_KEY('q'):
        if (E.dirty && quit_times > 0)
        {
            editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                                   "Press Ctrl-Q %d more times to quit.",
                                   quit_times);
            quit_times--;
            return;
        }
        write(STDOUT_FILENO, "\x1b[2J]", 4);
        write(STDOUT_FILENO, "\x1b[H]", 3);
        exit(0);
        break;

    case CTRL_KEY('s'):
        editorSave();
        break;

    case CTRL_KEY('f'):
        editorFind();
        break;

    case PAGE_UP:
    case PAGE_DOWN:
    {
        if (c == PAGE_UP)
        {
            E.cy = E.rowoff;
        }
        else if (c == PAGE_DOWN)
        {
            E.cy = E.rowoff + E.screenrows - 1;

            if (E.cy > E.numrows)
            {
                E.cy = E.numrows;
            }
        }

        int times = E.screenrows;
        while (times--)
        {
            editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
    }
    break;

    case HOME_KEY:
        E.cx = 0;
        break;

    case END_KEY:
        if (E.cy < E.numrows)
        {
            E.cx = E.row[E.cy].size;
        }
        break;

    case ARROW_UP:
    case ARROW_LEFT:
    case ARROW_DOWN:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
        if (c == DEL_KEY)
        {
            editorMoveCursor(ARROW_RIGHT);
        }
        editorDelChar();
        break;

    case CTRL_KEY('l'):
    case '\x1b':
        break;

    default:
        editorInsertChar(c);
        break;
    }

    quit_times = KILO_QUIT_TIMES;
}

/************************************************************ init ************************************************************/
void initEditor()
{
    E.cx = E.cy = E.numrows = E.rowoff = E.coloff = E.rx = E.statusmsg_time = E.dirty = 0;
    E.row = NULL;
    E.filename = NULL;
    E.syntax = NULL;
    E.statusmsg[0] = '\0';

    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    {
        die("getWindowSize");
    }

    E.screenrows -= 2;
}

int main(int argc, char *argv[])
{
    enableRawMode();
    initEditor();
    if (argc >= 2)
    {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    while (1)
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
