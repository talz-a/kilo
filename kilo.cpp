#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

constexpr std::string_view kilo_version = "0.0.1";
constexpr int kilo_tab_stop = 8;
constexpr int kilo_quit_times = 3;
constexpr int ctrl_key(int k) {
    return k & 0x1f;
}
constexpr int HL_HIGHLIGHT_NUMBERS = 1 << 0;
constexpr int HL_HIGHLIGHT_STRINGS = 1 << 1;

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

enum class Highlight : unsigned char {
    Normal = 0,
    Comment,
    MlComment,
    Keyword1,
    Keyword2,
    String,
    Number,
    Match
};

constexpr std::string_view C_HL_extensions[] = {".c", ".h", ".cpp"};
constexpr std::string_view C_HL_keywords[] = {"switch",    "if",      "while",   "for",    "break",
                                              "continue",  "return",  "else",    "struct", "union",
                                              "typedef",   "static",  "enum",    "class",  "case",

                                              "int|",      "long|",   "double|", "float|", "char|",
                                              "unsigned|", "signed|", "void|"};

struct editorSyntax {
    std::string_view filetype;
    std::span<const std::string_view> filematch;
    std::span<const std::string_view> keywords;
    std::string_view singleline_comment_start;
    std::string_view multiline_comment_start;
    std::string_view multiline_comment_end;
    int flags;
};

struct erow {
    int idx = 0;
    std::string chars;
    std::string render;
    std::vector<Highlight> hl;
    bool hl_open_comment = false;
};

struct editorConfig {
    int cx = 0, cy = 0;
    int rx = 0;
    int rowoff = 0, coloff = 0;
    int screenrows = 0, screencols = 0;
    std::vector<erow> row;
    int dirty = 0;
    std::string filename;
    std::string statusmsg;
    time_t statusmsg_time = 0;
    const editorSyntax* syntax = nullptr;
    struct termios orig_termios{};
};

struct editorConfig E;

editorSyntax HLDB[] = {
    {"c",
     C_HL_extensions,
     C_HL_keywords,
     "//",
     "/*",
     "*/",
     HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS},
};

int numrows() {
    return static_cast<int>(E.row.size());
}

/*** terminal ***/

void die(const char* s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) die("tcsetattr");
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
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
            } else {
                switch (seq[1]) {
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
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

int getCursorPosition(int* rows, int* cols) {
    char buf[32];
    unsigned int i = 0;
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

int getWindowSize(int* rows, int* cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** syntax highlighting ***/

int is_separator(int c) {
    return std::isspace(c) || c == '\0' || std::strchr(",.()+-/*=~%<>[];", c) != nullptr;
}

void editorUpdateSyntax(erow& row) {
    row.hl.assign(row.render.size(), Highlight::Normal);

    if (E.syntax == nullptr) return;

    auto keywords = E.syntax->keywords;

    auto scs = E.syntax->singleline_comment_start;
    auto mcs = E.syntax->multiline_comment_start;
    auto mce = E.syntax->multiline_comment_end;

    int prev_sep = 1;
    int in_string = 0;
    int in_comment = (row.idx > 0 && E.row[row.idx - 1].hl_open_comment);

    int i = 0;
    int rsize = static_cast<int>(row.render.size());
    while (i < rsize) {
        char c = row.render[i];
        Highlight prev_hl = (i > 0) ? row.hl[i - 1] : Highlight::Normal;

        if (!scs.empty() && !in_string && !in_comment) {
            if (std::string_view(row.render).substr(i, scs.size()) == scs) {
                std::fill(row.hl.begin() + i, row.hl.end(), Highlight::Comment);
                break;
            }
        }

        if (!mcs.empty() && !mce.empty() && !in_string) {
            if (in_comment) {
                row.hl[i] = Highlight::MlComment;
                if (std::string_view(row.render).substr(i, mce.size()) == mce) {
                    std::fill(
                        row.hl.begin() + i,
                        row.hl.begin() + i + static_cast<int>(mce.size()),
                        Highlight::Comment
                    );
                    i += static_cast<int>(mce.size());
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if (std::string_view(row.render).substr(i, mcs.size()) == mcs) {
                std::fill(
                    row.hl.begin() + i,
                    row.hl.begin() + i + static_cast<int>(mcs.size()),
                    Highlight::Comment
                );
                i += static_cast<int>(mcs.size());
                in_comment = 1;
                continue;
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
            if (in_string) {
                row.hl[i] = Highlight::String;
                if (c == '\\' && i + 1 < rsize) {
                    row.hl[i + 1] = Highlight::String;
                    i += 2;
                    continue;
                }
                if (c == in_string) in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            } else {
                if (c == '"' || c == '\'') {
                    in_string = c;
                    row.hl[i] = Highlight::String;
                    i++;
                    continue;
                }
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if ((std::isdigit(c) && (prev_sep || prev_hl == Highlight::Number)) ||
                (c == '.' && prev_hl == Highlight::Number)) {
                row.hl[i] = Highlight::Number;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        if (prev_sep) {
            bool matched = false;
            for (auto kw : keywords) {
                bool kw2 = kw.ends_with('|');
                auto keyword = kw2 ? kw.substr(0, kw.size() - 1) : kw;
                int klen = static_cast<int>(keyword.size());

                if (std::string_view(row.render).substr(i, klen) == keyword &&
                    is_separator(i + klen < rsize ? row.render[i + klen] : '\0')) {
                    std::fill(
                        row.hl.begin() + i,
                        row.hl.begin() + i + klen,
                        kw2 ? Highlight::Keyword2 : Highlight::Keyword1
                    );
                    i += klen;
                    matched = true;
                    break;
                }
            }
            if (matched) {
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = is_separator(c);
        i++;
    }

    bool changed = (row.hl_open_comment != (in_comment != 0));
    row.hl_open_comment = in_comment;
    if (changed && row.idx + 1 < numrows()) editorUpdateSyntax(E.row[row.idx + 1]);
}

int editorSyntaxToColor(Highlight hl) {
    switch (hl) {
        case Highlight::Comment:
        case Highlight::MlComment:
            return 36;
        case Highlight::Keyword1:
            return 33;
        case Highlight::Keyword2:
            return 32;
        case Highlight::String:
            return 35;
        case Highlight::Number:
            return 31;
        case Highlight::Match:
            return 34;
        default:
            return 37;
    }
}

void editorSelectSyntaxHighlight() {
    E.syntax = nullptr;
    if (E.filename.empty()) return;

    auto dot = E.filename.rfind('.');

    for (auto& s : HLDB) {
        for (auto& pattern : s.filematch) {
            bool is_ext = (pattern[0] == '.');
            if (is_ext && dot != std::string::npos && E.filename.substr(dot) == pattern) {
                E.syntax = &s;

                for (int filerow = 0; filerow < numrows(); filerow++) {
                    editorUpdateSyntax(E.row[filerow]);
                }
                return;
            }
            if (!is_ext && E.filename.find(pattern) != std::string::npos) {
                E.syntax = &s;

                for (int filerow = 0; filerow < numrows(); filerow++) {
                    editorUpdateSyntax(E.row[filerow]);
                }
                return;
            }
        }
    }
}

/*** row operations ***/

int editorRowCxToRx(const erow& row, int cx) {
    int rx = 0;
    for (int j = 0; j < cx; j++) {
        if (row.chars[j] == '\t') {
            rx += (kilo_tab_stop - 1) - (rx % kilo_tab_stop);
        }
        rx++;
    }
    return rx;
}

int editorRowRxToCx(const erow& row, int rx) {
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < static_cast<int>(row.chars.size()); cx++) {
        if (row.chars[cx] == '\t') {
            cur_rx += (kilo_tab_stop - 1) - (cur_rx % kilo_tab_stop);
        }
        cur_rx++;
        if (cur_rx > rx) return cx;
    }

    return cx;
}

void editorUpdateRow(erow& row) {
    row.render.clear();
    for (auto c : row.chars) {
        if (c == '\t') {
            row.render += ' ';
            while (row.render.size() % kilo_tab_stop != 0)
                row.render += ' ';
        } else {
            row.render += c;
        }
    }

    editorUpdateSyntax(row);
}

void editorInsertRow(int at, const std::string& s) {
    if (at < 0 || at > numrows()) return;

    E.row.insert(E.row.begin() + at, erow{});
    E.row[at].idx = at;
    E.row[at].chars = s;

    for (int j = at + 1; j <= numrows() - 1; j++)
        E.row[j].idx++;

    editorUpdateRow(E.row[at]);

    E.dirty++;
}

void editorDelRow(int at) {
    if (at < 0 || at >= numrows()) return;
    E.row.erase(E.row.begin() + at);
    for (int j = at; j < numrows(); j++)
        E.row[j].idx--;
    E.dirty++;
}

void editorRowInsertChar(erow& row, int at, int c) {
    if (at < 0 || at > static_cast<int>(row.chars.size())) at = static_cast<int>(row.chars.size());
    row.chars.insert(row.chars.begin() + at, c);
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow& row, const std::string& s) {
    row.chars += s;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow& row, int at) {
    if (at < 0 || at >= static_cast<int>(row.chars.size())) return;
    row.chars.erase(at, 1);
    editorUpdateRow(row);
    E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c) {
    if (E.cy == numrows()) {
        editorInsertRow(numrows(), "");
    }
    editorRowInsertChar(E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewLine() {
    if (E.cx == 0) {
        editorInsertRow(E.cy, "");
    } else {
        std::string tail = E.row[E.cy].chars.substr(E.cx);
        editorInsertRow(E.cy + 1, tail);
        E.row[E.cy].chars.resize(E.cx);
        editorUpdateRow(E.row[E.cy]);
    }
    E.cy++;
    E.cx = 0;
}

void editorDelChar() {
    if (E.cy == numrows()) return;
    if (E.cx == 0 && E.cy == 0) return;

    if (E.cx > 0) {
        editorRowDelChar(E.row[E.cy], E.cx - 1);
        E.cx--;
    } else {
        E.cx = static_cast<int>(E.row[E.cy - 1].chars.size());
        editorRowAppendString(E.row[E.cy - 1], E.row[E.cy].chars);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/*** file i/o ***/

std::string editorRowsToString() {
    std::string result;
    for (const auto& r : E.row) {
        result += r.chars;
        result += '\n';
    }
    return result;
}

void editorOpen(const std::string& filename) {
    E.filename = filename;

    editorSelectSyntaxHighlight();

    std::ifstream ifs(filename);
    if (!ifs) die("fopen");

    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        editorInsertRow(numrows(), line);
    }
    E.dirty = 0;
}

/*** forward declarations ***/

void editorRefreshScreen();
template <typename... Args>
void editorSetStatusMessage(std::format_string<Args...> fmt, Args&&... args);
std::optional<std::string>
editorPrompt(std::string_view prompt, void (*callback)(const std::string&, int));

/*** save ***/

void editorSave() {
    if (E.filename.empty()) {
        auto fname = editorPrompt("Save as: {}", nullptr);
        if (!fname) {
            editorSetStatusMessage("Save aborted");
            return;
        }
        E.filename = std::move(*fname);
        editorSelectSyntaxHighlight();
    }

    auto buf = editorRowsToString();
    std::ofstream ofs(E.filename, std::ios::trunc);
    if (ofs && ofs.write(buf.data(), buf.size())) {
        E.dirty = 0;
        editorSetStatusMessage("{} bytes written to disk", buf.size());
        return;
    }

    editorSetStatusMessage("Can't save! I/O error");
}

/*** find ***/

void editorFindCallback(const std::string& query, int key) {
    static int last_match = -1;
    static int direction = 1;

    static int saved_hl_line;
    static std::vector<Highlight> saved_hl;

    if (!saved_hl.empty()) {
        E.row[saved_hl_line].hl = saved_hl;
        saved_hl.clear();
    }

    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = -1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1) direction = 1;
    int current = last_match;

    for (int i = 0; i < numrows(); i++) {
        current += direction;
        if (current == -1)
            current = numrows() - 1;
        else if (current == numrows())
            current = 0;

        auto& row = E.row[current];
        auto match = row.render.find(query);
        if (match != std::string::npos) {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, static_cast<int>(match));
            E.rowoff = numrows();

            saved_hl_line = current;
            saved_hl = row.hl;
            std::fill(
                row.hl.begin() + match, row.hl.begin() + match + query.size(), Highlight::Match
            );
            break;
        }
    }
}

void editorFind() {
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    auto query = editorPrompt("Search : {} (use ESC/Arrows/Enter)", editorFindCallback);

    if (!query) {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
}

/*** output ***/

void editorScroll() {
    E.rx = 0;
    if (E.cy < numrows()) {
        E.rx = editorRowCxToRx(E.row[E.cy], E.cx);
    }

    if (E.cy < E.rowoff) E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
    if (E.rx < E.coloff) E.coloff = E.cx;
    if (E.rx >= E.coloff + E.screencols) E.coloff = E.cx - E.screencols + 1;
}

void editorDrawRows(std::string& buf) {
    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= numrows()) {
            if (numrows() == 0 && y == E.screenrows / 3) {
                auto welcome = std::format("Kilo editor -- version {}", kilo_version);
                int welcomelen = static_cast<int>(welcome.size());
                if (welcomelen > E.screencols) welcomelen = E.screencols;
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    buf += '~';
                    padding--;
                }
                while (padding--)
                    buf += ' ';
                buf.append(welcome, 0, welcomelen);
            } else {
                buf += '~';
            }
        } else {
            int len = static_cast<int>(E.row[filerow].render.size()) - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            const char* c = E.row[filerow].render.data() + E.coloff;
            const Highlight* hl = E.row[filerow].hl.data() + E.coloff;
            int current_color = -1;
            for (int j = 0; j < len; j++) {
                if (std::iscntrl(static_cast<unsigned char>(c[j]))) {
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    buf += "\x1b[7m";
                    buf += sym;
                    buf += "\x1b[m";
                    if (current_color != -1) {
                        buf += std::format("\x1b[{}m", current_color);
                    }
                } else if (hl[j] == Highlight::Normal) {
                    if (current_color != -1) {
                        buf += "\x1b[39m";
                        current_color = -1;
                    }
                    buf += c[j];
                } else {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) {
                        current_color = color;
                        buf += std::format("\x1b[{}m", color);
                    }
                    buf += c[j];
                }
            }
            buf += "\x1b[39m";
        }

        buf += "\x1b[K";
        buf += "\r\n";
    }
}

void editorDrawStatusBar(std::string& buf) {
    buf += "\x1b[7m";

    auto status = std::format(
        "{:.20} - {} lines {}",
        E.filename.empty() ? "[No Name]" : E.filename,
        numrows(),
        E.dirty ? "(modified)" : ""
    );

    auto rstatus =
        std::format("{} | {}/{}", E.syntax ? E.syntax->filetype : "no ft", E.cy + 1, numrows());

    int len = static_cast<int>(status.size());
    int rlen = static_cast<int>(rstatus.size());

    if (len > E.screencols) len = E.screencols;
    buf.append(status, 0, len);

    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            buf += rstatus;
            break;
        } else {
            buf += ' ';
            len++;
        }
    }

    buf += "\x1b[m";
    buf += "\r\n";
}

void editorDrawMessageBar(std::string& buf) {
    buf += "\x1b[K";
    int msglen = static_cast<int>(E.statusmsg.size());
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(nullptr) - E.statusmsg_time < 5) buf.append(E.statusmsg, 0, msglen);
}

void editorRefreshScreen() {
    editorScroll();

    std::string buf;

    buf += "\x1b[?25l";
    buf += "\x1b[H";

    editorDrawRows(buf);
    editorDrawStatusBar(buf);
    editorDrawMessageBar(buf);

    buf += std::format("\x1b[{};{}H", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);

    buf += "\x1b[?25h";

    write(STDOUT_FILENO, buf.data(), buf.size());
}

template <typename... Args>
void editorSetStatusMessage(std::format_string<Args...> fmt, Args&&... args) {
    E.statusmsg = std::format(fmt, std::forward<Args>(args)...);
    E.statusmsg_time = time(nullptr);
}

std::optional<std::string>
editorPrompt(std::string_view prompt, void (*callback)(const std::string&, int)) {
    std::string buf;

    while (1) {
        E.statusmsg = std::vformat(prompt, std::make_format_args(buf));
        E.statusmsg_time = time(nullptr);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || c == ctrl_key('h') || c == BACKSPACE) {
            if (!buf.empty()) buf.pop_back();
        } else if (c == '\x1b') {
            E.statusmsg.clear();
            E.statusmsg_time = time(nullptr);
            if (callback) callback(buf, c);
            return std::nullopt;
        } else if (c == '\r') {
            if (!buf.empty()) {
                E.statusmsg.clear();
                E.statusmsg_time = time(nullptr);
                if (callback) callback(buf, c);
                return buf;
            }
        } else if (!std::iscntrl(c) && c < 128) {
            buf += static_cast<char>(c);
        }

        if (callback) callback(buf, c);
    }
}

/*** input ***/

void editorMoveCursor(int key) {
    bool has_row = E.cy < numrows();

    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = static_cast<int>(E.row[E.cy].chars.size());
            }
            break;
        case ARROW_RIGHT:
            if (has_row && E.cx < static_cast<int>(E.row[E.cy].chars.size())) {
                E.cx++;
            } else if (has_row && E.cx == static_cast<int>(E.row[E.cy].chars.size())) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) E.cy--;
            break;
        case ARROW_DOWN:
            if (E.cy < numrows()) E.cy++;
            break;
    }

    has_row = E.cy < numrows();
    int rowlen = has_row ? static_cast<int>(E.row[E.cy].chars.size()) : 0;
    if (E.cx > rowlen) E.cx = rowlen;
}

void editorProcessKeypress() {
    static int quit_times = kilo_quit_times;

    int c = editorReadKey();
    switch (c) {
        case '\r':
            editorInsertNewLine();
            break;

        case ctrl_key('q'):
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage(
                    "WARNING!!! File has unsaved changes. "
                    "Press Ctrl-Q {} more times to quit.",
                    quit_times
                );
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case ctrl_key('s'):
            editorSave();
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            if (E.cy < numrows()) E.cx = static_cast<int>(E.row[E.cy].chars.size());
            break;

        case ctrl_key('f'):
            editorFind();
            break;

        case BACKSPACE:
        case ctrl_key('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;

        case PAGE_UP:
        case PAGE_DOWN: {
            if (c == PAGE_UP) {
                E.cy = E.rowoff;
            } else if (c == PAGE_DOWN) {
                E.cy = E.rowoff + E.screenrows - 1;
                if (E.cy > numrows()) E.cy = numrows();
            }
            int times = E.screenrows;
            while (times--) {
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
        } break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        case ctrl_key('l'):
        case '\x1b':
            break;

        default:
            editorInsertChar(c);
            break;
    }

    quit_times = kilo_quit_times;
}

/*** init ***/

void initEditor() {
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -= 2;
}

int main(int argc, char* argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) editorOpen(argv[1]);

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
