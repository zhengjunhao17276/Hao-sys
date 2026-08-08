/*
 * vi.c - HaoOS 迷你 vi：模式编辑器（normal / insert / ex 三态）
 *
 * 特性：h/j/k/l + 方向键移动、w/b/e 词跳、0/$/^、gg/G/行号G、
 *       x/X/dd/D/dw 删除、yy 复制、p/P 粘贴、u 撤销、~ 大小写、
 *       i/I/a/A/o/O 进入插入、r 替换、:w/:q/:wq/:q!/:e 命令、
 *       <n> 计数前缀、脏标记、状态栏/消息行。
 *
 * 文件 ≤32KB、≤4000 行（静态缓冲，无堆）；行池 + 行表结构。
 * 撤销：单级快照（进入每次修改会话前保存，u 恢复整个会话）。
 *
 * ⚠️ 用户栈只有 8KB：所有大缓冲必须是 static（.bss），严禁栈上大数组。
 * 系统调用号必须与 include/syscall/syscall.h 一致（shell 无共享头）。
 */

/* ---- 系统调用号 ---- */
#define SYS_PUTCHAR    1
#define SYS_WRITE      3
#define SYS_GETMOUSE   6
#define SYS_SET_CURSOR 7
#define SYS_SET_COLOR  13
#define SYS_WRITE_FILE 17
#define SYS_READ_FILE  18
#define SYS_YIELD      26
#define SYS_GETKEY_NB  31
#define SYS_CURSOR     37

/* ---- 容量 ---- */
#define VI_MAX_BYTES 32768
#define VI_MAX_LINES 4000
#define VI_VIEW_ROWS 22        /* 视口行数（80×25：22 文本 + 状态 + 消息） */

/* ---- 编辑缓冲：池 + 行表（行按顺序连续存放） ---- */
static char vi_pool[VI_MAX_BYTES];
static unsigned int vi_pool_used;
static unsigned int  line_off[VI_MAX_LINES];
static unsigned short line_len[VI_MAX_LINES];
static unsigned int  line_count;

/* 撤销快照（进入修改会话前整份拷贝） */
static char undo_pool[VI_MAX_BYTES];
static unsigned int undo_pool_used;
static unsigned int  undo_off[VI_MAX_LINES];
static unsigned short undo_len[VI_MAX_LINES];
static unsigned int  undo_lines;
static int undo_valid;

/* 保存输出缓冲（行内容 + \n 组装；无追加 syscall，必须一次拼好） */
static char vi_out[VI_MAX_BYTES + VI_MAX_LINES];

/* ---- 视图/光标状态 ---- */
static unsigned int cur_line;
static unsigned int cur_col;
static unsigned int top_line;
static unsigned int top_col;
static int dirty;
static int done;

static char vi_filename[64];

enum { MODE_NORMAL, MODE_INSERT, MODE_EX };
static int mode = MODE_NORMAL;
static char msg[80];
static char exbuf[64];
static unsigned int exlen;
static unsigned int count;
static int have_count;

static char yank_line[256];
static unsigned int yank_len;
static int have_yank;

/* ---- 基础工具（无 libc） ---- */
static void vi_strcpy(char* d, const char* s) {
    while ((*d++ = *s++)) ;
}

static void vi_memmove(char* d, const char* s, unsigned int n) {
    if (d < s) { for (unsigned int i = 0; i < n; i++) d[i] = s[i]; }
    else if (d > s) { for (unsigned int i = n; i > 0; i--) d[i-1] = s[i-1]; }
}

/* ---- 消息 ---- */
static void vi_msg(const char* s) {
    unsigned int i = 0;
    while (s[i] && i < sizeof(msg) - 1) { msg[i] = s[i]; i++; }
    msg[i] = '\0';
}

/* ---- 缓冲行访问 ---- */
static unsigned int line_len_of(unsigned int li) { return line_len[li]; }

/* 光标列（超出行尾 → 行尾） */
static unsigned int cur_col_clamped(void) {
    unsigned int len = line_len_of(cur_line);
    return cur_col > len ? len : cur_col;
}

/* ---- 缓冲操作（修改会话开始前调用 snapshot） ---- */
static void snapshot(void) {
    vi_memmove(undo_pool, vi_pool, vi_pool_used);
    undo_pool_used = vi_pool_used;
    for (unsigned int i = 0; i < line_count; i++) {
        undo_off[i] = line_off[i];
        undo_len[i] = line_len[i];
    }
    undo_lines = line_count;
    undo_valid = 1;
}

static void undo_restore(void) {
    if (!undo_valid) { vi_msg("Nothing to undo"); return; }
    vi_memmove(vi_pool, undo_pool, undo_pool_used);
    vi_pool_used = undo_pool_used;
    for (unsigned int i = 0; i < undo_lines; i++) {
        line_off[i] = undo_off[i];
        line_len[i] = undo_len[i];
    }
    line_count = undo_lines;
    if (cur_line >= line_count) cur_line = line_count ? line_count - 1 : 0;
    if (cur_col > line_len_of(cur_line)) cur_col = line_len_of(cur_line);
    dirty = 1;
    undo_valid = 0;
    vi_msg("Undo");
}

/* 在行 li 的 col 处插入字符（后续行偏移 +1） */
static void line_insert_char(unsigned int li, unsigned int col, char c) {
    if (vi_pool_used >= VI_MAX_BYTES) { vi_msg("Buffer full"); return; }
    unsigned int len = line_len[li];
    if (col > len) col = len;
    unsigned int pos = line_off[li] + col;
    vi_memmove(vi_pool + pos + 1, vi_pool + pos, vi_pool_used - pos);
    vi_pool[pos] = c;
    vi_pool_used++;
    for (unsigned int i = li + 1; i < line_count; i++) line_off[i]++;
    line_len[li]++;
}

/* 删除行 li 的 col 处字符 */
static void line_delete_char(unsigned int li, unsigned int col) {
    unsigned int len = line_len[li];
    if (col >= len) return;
    unsigned int pos = line_off[li] + col;
    vi_memmove(vi_pool + pos, vi_pool + pos + 1, vi_pool_used - pos - 1);
    vi_pool_used--;
    for (unsigned int i = li + 1; i < line_count; i++) line_off[i]--;
    line_len[li]--;
}

/* 在 li 处插入空行 */
static void line_insert_blank(unsigned int li) {
    if (line_count >= VI_MAX_LINES) { vi_msg("Too many lines"); return; }
    for (unsigned int i = line_count; i > li; i--) {
        line_off[i] = line_off[i-1];
        line_len[i] = line_len[i-1];
    }
    line_off[li] = (li == 0) ? 0 : line_off[li-1] + line_len[li-1];
    line_len[li] = 0;
    line_count++;
}

/* 删除行 li（最后一行则清空内容） */
static void line_delete(unsigned int li) {
    if (line_count <= 1) {
        line_len[0] = 0;
        vi_pool_used = 0;
        cur_line = 0; cur_col = 0;
        return;
    }
    unsigned int len = line_len[li];
    unsigned int pos = line_off[li];
    vi_memmove(vi_pool + pos, vi_pool + pos + len, vi_pool_used - pos - len);
    vi_pool_used -= len;
    for (unsigned int i = li + 1; i < line_count; i++) line_off[i] -= len;
    for (unsigned int i = li; i < line_count - 1; i++) {
        line_off[i] = line_off[i+1];
        line_len[i] = line_len[i+1];
    }
    line_count--;
    if (cur_line >= line_count) cur_line = line_count - 1;
    cur_col = 0;
}

/* 拆分行 li 于 col：左侧留 li，右侧成新行 li+1 */
static void line_split(unsigned int li, unsigned int col) {
    if (line_count >= VI_MAX_LINES) { vi_msg("Too many lines"); return; }
    unsigned int len = line_len[li];
    if (col > len) col = len;
    for (unsigned int i = line_count; i > li + 1; i--) {
        line_off[i] = line_off[i-1];
        line_len[i] = line_len[i-1];
    }
    line_off[li+1] = line_off[li] + col;
    line_len[li+1] = len - col;
    line_len[li] = col;
    line_count++;
}

/* ---- 渲染 ---- */
static void vi_set_color(int c) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_SET_COLOR), "b"((unsigned int)c) : "memory");
}
static void vi_putchar(char c) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_PUTCHAR), "b"((unsigned int)c) : "memory");
}
static void vi_write(const char* s) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_WRITE), "b"((unsigned int)s) : "memory");
}
static void vi_set_cursor(int row, int col) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_SET_CURSOR), "b"((unsigned int)row), "c"((unsigned int)col) : "memory");
}
static int vi_getkey(void) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_GETKEY_NB) : "memory");
    return r;
}
static void vi_getmouse(void) {
    int m[3];
    __asm__ volatile ("int $0x80" : : "a"(SYS_GETMOUSE), "b"((unsigned int)m) : "memory");
}
static void vi_yield(void) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_YIELD) : "memory");
}
static void vi_cursor_visible(int on) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_CURSOR), "b"((unsigned int)on) : "memory");
}

static void vi_clear_row(int row) {
    vi_set_cursor(row, 0);
    vi_set_color(0x0F);
    for (int i = 0; i < 80; i++) vi_putchar(' ');
}

/* 画一行文本（行 li → 屏幕 row），光标字符反显 */
static void vi_draw_row(int row, unsigned int li) {
    unsigned int len = line_len[li];
    vi_set_cursor(row, 0);
    unsigned int x = 0;
    for (unsigned int i = top_col; i < len && x < 80; i++, x++) {
        char c = vi_pool[line_off[li] + i];
        if (c == '\t') c = ' ';
        if (c < 32 || c > 126) c = '?';
        vi_set_color((li == cur_line && i == cur_col) ? 0x70 : 0x0F);
        vi_putchar(c);
    }
    vi_set_color(0x0F);
    while (x < 80) { vi_putchar(' '); x++; }
}

static void vi_draw_status(void) {
    vi_set_cursor(22, 0);
    vi_set_color(0x70);
    for (int i = 0; i < 80; i++) vi_putchar(' ');
    vi_set_cursor(22, 1);
    vi_write(vi_filename);
    vi_write(mode == MODE_INSERT ? "  [INSERT]" : mode == MODE_EX ? "  [EX]" : "  [NORMAL]");
    if (dirty) vi_write("  [modified]");
    vi_write("  ");
    /* 行号/列号（十进制手写） */
    {
        char nb[16];
        unsigned int v = cur_line + 1, k = 0;
        do { nb[k++] = '0' + (v % 10); v /= 10; } while (v);
        while (k) vi_putchar(nb[--k]);
        vi_putchar(':');
        v = cur_col_clamped() + 1; k = 0;
        do { nb[k++] = '0' + (v % 10); v /= 10; } while (v);
        while (k) vi_putchar(nb[--k]);
    }
    vi_set_color(0x0F);
}

static void vi_draw_message(void) {
    vi_clear_row(23);
    vi_set_cursor(23, 1);
    if (mode == MODE_EX) {
        vi_putchar(':');
        for (unsigned int i = 0; i < exlen; i++) vi_putchar(exbuf[i]);
    } else {
        vi_write(msg);
    }
}

static void vi_full_draw(void) {
    for (int r = 0; r < VI_VIEW_ROWS; r++) {
        unsigned int li = top_line + r;
        if (li < line_count) vi_draw_row(r, li);
        else vi_clear_row(r);
    }
    vi_draw_status();
    vi_draw_message();
}

/* 滚动：保证光标在视口内 */
static void vi_scroll(void) {
    int moved = 0;
    if (cur_line < top_line) { top_line = cur_line; moved = 1; }
    if (cur_line >= top_line + VI_VIEW_ROWS) { top_line = cur_line - VI_VIEW_ROWS + 1; moved = 1; }
    unsigned int c = cur_col_clamped();
    if (c < top_col) { top_col = c; moved = 1; }
    if (c >= top_col + 79) { top_col = c - 78; moved = 1; }
    if (moved) vi_full_draw();
}

/* ---- 文件 I/O ---- */
static int vi_read_file(const char* name, char* buf, unsigned int max) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_READ_FILE), "b"((unsigned int)name),
                      "c"((unsigned int)buf), "d"(max) : "memory");
    return r;
}
static int vi_write_file(const char* name, const char* data, unsigned int size) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_WRITE_FILE), "b"((unsigned int)name),
                      "c"((unsigned int)data), "d"(size) : "memory");
    return r;
}

/* 从原始字节（已在 vi_pool 中）原地构建行表：跳过 \r，\n 分行 */
static void vi_build_lines(char* data, unsigned int n) {
    line_count = 0;
    unsigned int w = 0;       /* 压缩后写入位置 */
    unsigned int start = 0;   /* 当前行起点（压缩后坐标） */
    for (unsigned int i = 0; i <= n; i++) {
        if (i == n || data[i] == '\n') {
            if (line_count < VI_MAX_LINES) {
                line_off[line_count] = start;
                line_len[line_count] = (unsigned short)(w - start);
                line_count++;
            }
            start = w;
        } else if (data[i] != '\r') {
            data[w++] = data[i];
        }
    }
    vi_pool_used = w;
    if (line_count == 0) { line_off[0] = 0; line_len[0] = 0; line_count = 1; }
}

static void vi_load(const char* name) {
    int n = vi_read_file(name, vi_pool, VI_MAX_BYTES);
    if (n < 0) {
        vi_pool_used = 0;
        line_off[0] = 0; line_len[0] = 0; line_count = 1;
        vi_msg("[New file]");
    } else {
        vi_build_lines(vi_pool, (unsigned int)n);
        char m[80];
        unsigned int k = 0;
        const char* s = "\" loaded, ";
        while (*s && k < 79) m[k++] = *s++;
        unsigned int v = line_count, d = 0;
        char nb[12];
        do { nb[d++] = '0' + (v % 10); v /= 10; } while (v);
        while (d) m[k++] = nb[--d];
        m[k++] = ' '; m[k++] = 'l'; m[k++] = 'i'; m[k++] = 'n'; m[k++] = 'e'; m[k++] = 's';
        m[k] = '\0';
        vi_msg(m);
    }
    cur_line = 0; cur_col = 0; top_line = 0; top_col = 0;
    dirty = 0;
    undo_valid = 0;
}

static void vi_save(const char* name) {
    unsigned int o = 0;
    for (unsigned int i = 0; i < line_count && o < sizeof(vi_out); i++) {
        unsigned int len = line_len[i];
        for (unsigned int j = 0; j < len && o < sizeof(vi_out) - 1; j++) vi_out[o++] = vi_pool[line_off[i] + j];
        if (o < sizeof(vi_out) - 1) vi_out[o++] = '\n';
    }
    if (vi_write_file(name, vi_out, o) == 0) {
        dirty = 0;
        vi_msg("\" saved");
    } else {
        vi_msg("Write failed");
    }
}

/* ---- 移动 ---- */
static int is_word_char(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

static void move_down(unsigned int n) {
    cur_line += n;
    if (cur_line >= line_count) cur_line = line_count - 1;
    unsigned int len = line_len_of(cur_line);
    if (cur_col > len) cur_col = len;
}
static void move_up(unsigned int n) {
    if (cur_line >= n) cur_line -= n; else cur_line = 0;
    unsigned int len = line_len_of(cur_line);
    if (cur_col > len) cur_col = len;
}

static void move_word_fwd(void) {
    unsigned int len = line_len_of(cur_line);
    unsigned int c = cur_col_clamped();
    while (c < len && !is_word_char(vi_pool[line_off[cur_line] + c])) c++;
    if (c >= len) {
        if (cur_line + 1 < line_count) { cur_line++; cur_col = 0; }
        return;
    }
    while (c < len && is_word_char(vi_pool[line_off[cur_line] + c])) c++;
    cur_col = c;
}
static void move_word_back(void) {
    unsigned int c = cur_col_clamped();
    while (c > 0 && !is_word_char(vi_pool[line_off[cur_line] + c - 1])) c--;
    while (c > 0 && is_word_char(vi_pool[line_off[cur_line] + c - 1])) c--;
    if (c == 0 && cur_line > 0) { cur_line--; cur_col = line_len_of(cur_line); return; }
    cur_col = c;
}
static void move_word_end(void) {
    unsigned int len = line_len_of(cur_line);
    unsigned int c = cur_col_clamped();
    if (c >= len) { if (cur_line + 1 < line_count) { cur_line++; cur_col = 0; } return; }
    unsigned int first = vi_pool[line_off[cur_line] + c];
    while (c + 1 < len && is_word_char(vi_pool[line_off[cur_line] + c + 1]) == is_word_char(first)) c++;
    cur_col = c;
}

/* ---- normal 模式 ---- */
static void normal_key(int c) {
    if (c >= '0' && c <= '9') {
        count = count * 10 + (c - '0');
        if (count > 9999) count = 9999;
        have_count = 1;
        vi_draw_message();
        return;
    }
    unsigned int n = have_count ? count : 1;
    have_count = 0;
    count = 0;
    vi_draw_message();

    switch (c) {
        case 'h': case 0x04:
            if (cur_col > 0) cur_col--;
            break;
        case 'l': case 0x03:
            if (cur_col < line_len_of(cur_line)) cur_col++;
            break;
        case 'j': case 0x02:
            move_down(n);
            break;
        case 'k': case 0x01:
            move_up(n);
            break;
        case '0':
            cur_col = 0;
            break;
        case '$': {
            unsigned int len = line_len_of(cur_line);
            cur_col = len > 0 ? len - 1 : 0;
            break;
        }
        case '^': {
            unsigned int len = line_len_of(cur_line);
            unsigned int c = 0;
            while (c < len && vi_pool[line_off[cur_line] + c] == ' ') c++;
            cur_col = c;
            break;
        }
        case 'w': move_word_fwd(); break;
        case 'b': move_word_back(); break;
        case 'e': move_word_end(); break;
        case 'G':
            cur_line = line_count - 1;
            cur_col = 0;
            break;
        case 'g': {
            int k = vi_getkey();
            if (k == 'g' || k == 'G') { cur_line = 0; cur_col = 0; }
            break;
        }
        case '\n': case '\r':
            move_down(1);
            cur_col = 0;
            break;
        case 'x':
            if (line_len_of(cur_line) > 0) {
                snapshot();
                line_delete_char(cur_line, cur_col_clamped());
                dirty = 1;
            }
            break;
        case 'X':
            if (cur_col_clamped() > 0) {
                snapshot();
                line_delete_char(cur_line, cur_col_clamped() - 1);
                if (cur_col > 0) cur_col--;
                dirty = 1;
            }
            break;
        case 'D': {
            snapshot();
            unsigned int len = line_len_of(cur_line);
            unsigned int c = cur_col_clamped();
            while (len > c) { line_delete_char(cur_line, c); len--; }
            dirty = 1;
            break;
        }
        case 'd': {
            int k = vi_getkey();
            if (k == 'd') {
                snapshot();
                for (unsigned int i = 0; i < n && line_count > 1; i++) line_delete(cur_line);
                if (line_count == 1) line_len[0] = 0, vi_pool_used = 0;
                dirty = 1;
            } else if (k == 'w') {
                snapshot();
                unsigned int len = line_len_of(cur_line);
                unsigned int c = cur_col_clamped();
                while (c < len && !is_word_char(vi_pool[line_off[cur_line] + c])) c++;
                while (c < len && is_word_char(vi_pool[line_off[cur_line] + c])) c++;
                while (len > cur_col_clamped()) { line_delete_char(cur_line, cur_col_clamped()); len--; }
                dirty = 1;
            }
            break;
        }
        case 'y': {
            int k = vi_getkey();
            if (k == 'y') {
                unsigned int len = line_len_of(cur_line);
                if (len < sizeof(yank_line)) {
                    for (unsigned int i = 0; i < len; i++) yank_line[i] = vi_pool[line_off[cur_line] + i];
                    yank_len = len;
                    have_yank = 1;
                    vi_msg("Yanked 1 line");
                }
            }
            break;
        }
        case 'p':
            if (have_yank) {
                snapshot();
                line_insert_blank(cur_line + 1);
                for (unsigned int i = 0; i < yank_len; i++) line_insert_char(cur_line + 1, i, yank_line[i]);
                dirty = 1;
                vi_msg("Pasted");
            }
            break;
        case 'P':
            if (have_yank) {
                snapshot();
                line_insert_blank(cur_line);
                for (unsigned int i = 0; i < yank_len; i++) line_insert_char(cur_line, i, yank_line[i]);
                dirty = 1;
                vi_msg("Pasted");
            }
            break;
        case 'u':
            undo_restore();
            break;
        case '~': {
            unsigned int c = cur_col_clamped();
            if (c < line_len_of(cur_line)) {
                snapshot();
                char ch = vi_pool[line_off[cur_line] + c];
                if (ch >= 'a' && ch <= 'z') vi_pool[line_off[cur_line] + c] = ch - 'a' + 'A';
                else if (ch >= 'A' && ch <= 'Z') vi_pool[line_off[cur_line] + c] = ch - 'A' + 'a';
                cur_col++;
                dirty = 1;
            }
            break;
        }
        case 'r': {
            int k = vi_getkey();
            unsigned int c = cur_col_clamped();
            if (k >= 32 && k <= 126 && c < line_len_of(cur_line)) {
                snapshot();
                vi_pool[line_off[cur_line] + c] = (char)k;
                cur_col++;
                dirty = 1;
            }
            break;
        }
        case 'i':
            snapshot();
            mode = MODE_INSERT;
            vi_draw_status();
            break;
        case 'I':
            snapshot();
            cur_col = 0;
            mode = MODE_INSERT;
            vi_draw_status();
            break;
        case 'a':
            snapshot();
            if (line_len_of(cur_line) > 0) cur_col++;
            mode = MODE_INSERT;
            vi_draw_status();
            break;
        case 'A':
            snapshot();
            cur_col = line_len_of(cur_line);
            mode = MODE_INSERT;
            vi_draw_status();
            break;
        case 'o':
            snapshot();
            line_insert_blank(cur_line + 1);
            cur_line++;
            cur_col = 0;
            mode = MODE_INSERT;
            dirty = 1;
            vi_draw_status();
            break;
        case 'O':
            snapshot();
            line_insert_blank(cur_line);
            cur_col = 0;
            mode = MODE_INSERT;
            dirty = 1;
            vi_draw_status();
            break;
        case ':':
            mode = MODE_EX;
            exlen = 0;
            exbuf[0] = '\0';
            vi_draw_message();
            break;
        case 0x1B:
            break;
        default:
            break;
    }
}

/* ---- insert 模式（会话快照在进入时已拍，此处不再快照） ---- */
static void insert_key(int c) {
    if (c == 0x1B) {
        mode = MODE_NORMAL;
        if (cur_col > 0) cur_col--;
        vi_draw_status();
        vi_draw_message();
        return;
    }
    if (c == '\n' || c == '\r') {
        unsigned int col = cur_col_clamped();
        line_split(cur_line, col);
        cur_line++;
        cur_col = 0;
        dirty = 1;
        return;
    }
    if (c == '\b') {
        unsigned int col = cur_col_clamped();
        if (col > 0) {
            line_delete_char(cur_line, col - 1);
            if (cur_col > 0) cur_col--;
            dirty = 1;
        } else if (cur_line > 0) {
            /* 行首退格：当前行并入上一行 */
            unsigned int prev_len = line_len_of(cur_line - 1);
            unsigned int cur_len = line_len_of(cur_line);
            for (unsigned int i = 0; i < cur_len; i++)
                line_insert_char(cur_line - 1, prev_len + i, vi_pool[line_off[cur_line] + i]);
            line_delete(cur_line);
            cur_line--;
            cur_col = prev_len;
            dirty = 1;
        }
        return;
    }
    if (c >= 32 && c <= 126) {
        line_insert_char(cur_line, cur_col_clamped(), (char)c);
        cur_col++;
        dirty = 1;
        return;
    }
    if (c == 0x01 || c == 0x02 || c == 0x03 || c == 0x04) {
        if (c == 0x02) move_down(1);
        else if (c == 0x01) move_up(1);
        else if (c == 0x03 && cur_col < line_len_of(cur_line)) cur_col++;
        else if (c == 0x04 && cur_col > 0) cur_col--;
        return;
    }
}

/* ---- ex 模式 ---- */
static void ex_key(int c) {
    if (c == 0x1B) {
        mode = MODE_NORMAL;
        vi_draw_status();
        vi_draw_message();
        return;
    }
    if (c == '\n' || c == '\r') {
        exbuf[exlen] = '\0';
        const char* e = exbuf;
        if (e[0] == 'w' && (e[1] == 'q' || e[1] == 'Q')) {
            vi_save(vi_filename);
            done = 1;
        } else if (e[0] == 'w') {
            if (e[1] == ' ' && e[2]) vi_save(e + 2);
            else vi_save(vi_filename);
        } else if (e[0] == 'q' && e[1] == '!') {
            done = 1;
        } else if (e[0] == 'q') {
            if (dirty) vi_msg("No write since last change (:q! to discard)");
            else done = 1;
        } else if (e[0] == 'e') {
            if (e[1] == ' ' && e[2]) {
                vi_strcpy(vi_filename, e + 2);
                vi_load(vi_filename);
            }
        } else {
            vi_msg("Unknown command");
        }
        mode = MODE_NORMAL;
        exlen = 0;
        exbuf[0] = '\0';
        vi_draw_status();
        vi_draw_message();
        return;
    }
    if (c == '\b') {
        if (exlen > 0) exlen--;
        exbuf[exlen] = '\0';
        vi_draw_message();
        return;
    }
    if (c >= 32 && c <= 126 && exlen < sizeof(exbuf) - 1) {
        exbuf[exlen++] = (char)c;
        exbuf[exlen] = '\0';
        vi_draw_message();
        return;
    }
}

/* ---- 入口 ---- */
void vi_main(const char* filename) {
    vi_cursor_visible(0);
    done = 0;
    mode = MODE_NORMAL;
    dirty = 0;
    have_count = 0; count = 0;
    have_yank = 0; yank_len = 0;
    msg[0] = '\0';
    exlen = 0; exbuf[0] = '\0';

    if (filename && filename[0]) {
        unsigned int i = 0;
        while (filename[i] && i < sizeof(vi_filename) - 1) { vi_filename[i] = filename[i]; i++; }
        vi_filename[i] = '\0';
    } else {
        vi_strcpy(vi_filename, "newfile");
    }
    vi_load(vi_filename);
    vi_full_draw();

    while (!done) {
        int c = vi_getkey();
        if (c == -1) {
            vi_getmouse();       /* 消费鼠标字节，防键盘被阻塞 */
            vi_yield();
            continue;
        }
        if (mode == MODE_NORMAL) normal_key(c);
        else if (mode == MODE_INSERT) insert_key(c);
        else ex_key(c);

        vi_scroll();
        if (cur_line < line_count &&
            cur_line >= top_line && cur_line < top_line + VI_VIEW_ROWS) {
            vi_draw_row(cur_line - top_line, cur_line);
        }
        vi_draw_status();
        if (mode == MODE_EX) vi_draw_message();
    }

    vi_cursor_visible(1);
}
