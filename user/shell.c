/**
 * =========================================================================
 * shell.c - HaoOS 用户态交互式 Shell
 *
 * 这是运行在 ring 3（用户态）的命令行解释器，通过 int 0x80 系统调用
 * 与内核通信。所有 I/O 操作都必须通过系统调用，不能直接访问硬件。
 *
 * 系统调用接口（详见 syscall.h）：
 *   SYS_PUTCHAR (1) - 输出一个字符
 *   SYS_GETCHAR (2) - 获取一个键盘输入（阻塞）
 *   SYS_WRITE   (3) - 输出字符串
 *   SYS_EXIT    (4) - 退出程序
 *   SYS_READ_SECT (5) - 读扇区（预留）
 *
 * 调用约定：eax=系统调用号, ebx/ecx=参数, int $0x80, 返回在 eax
 *
 * 功能：
 *   - 显示 "$ " 提示符
 *   - 支持退格（Backspace）删除
 *   - 支持方向键历史（待实现）
 *   - 内置命令：help, echo, clear, exit, tasks
 *
 * 注意：内核加载扁平二进制时跳到文件开头，所以入口函数必须放在最前面。
 * _entry 作为引导，直接跳转到 main。
 * =========================================================================
 */

/* 裸机环境：没有 stddef.h，手动定义 NULL */
#define NULL ((void*)0)

/* 系统调用号（与内核保持一致） */
#define SYS_GETMOUSE  6   /* 获取鼠标状态 */
#define SYS_SET_CURSOR 7  /* 定位光标 */
#define SYS_GET_CURSOR 8  /* 读光标位置 */
#define SYS_CLEAR      9  /* 清屏 */
#define SYS_GET_SENS  10  /* 读鼠标灵敏度 */
#define SYS_SET_SENS  11  /* 写鼠标灵敏度 */
#define SYS_GET_COLOR 12  /* 读默认颜色 */
#define SYS_SET_COLOR 13  /* 写默认颜色 */
#define SYS_PROTECT   14  /* 锁定保护区域（编辑区边界） */
#define SYS_GET_PGLYPH 15  /* 读鼠标指针图案 */
#define SYS_SET_PGLYPH 16  /* 写鼠标指针图案 */
#define SYS_WRITE_FILE 17  /* 写文件 */
#define SYS_READ_FILE  18  /* 读文件 */
#define SYS_TASKS      19  /* 打印任务列表 */
#define SYS_GET_TIME   20  /* 读时间 */
#define SYS_GET_DATE   21  /* 读日期 */
#define SYS_DELETE_FILE 22  /* 删除文件 */
#define SYS_UPTIME     23  /* 读开机时长 */
#define SYS_MKDIR      24  /* 创建子目录 */
#define SYS_LIST       25  /* 列目录 */
#define SYS_YIELD      26  /* 让出 CPU（协作式调度测试） */

/* FAT 目录项（与内核 fat_dirent_t 布局一致，32 字节） */
struct dirent {
    unsigned char name[11];
    unsigned char attributes;
    unsigned char nt_reserved;
    unsigned char creation_time_tenths;
    unsigned short creation_time;
    unsigned short creation_date;
    unsigned short last_access_date;
    unsigned short cluster_high;
    unsigned short last_write_time;
    unsigned short last_write_date;
    unsigned short cluster_low;
    unsigned int file_size;
};

/* ==================== 引导入口 ==================== */

/** _entry - 引导入口，跳转到真正的 main 函数 */
__attribute__((naked)) void _entry(void) {
    __asm__ volatile (
        "jmp main"
    );
}

/* ==================== 系统调用内联函数 ==================== */

/** putchar - 通过系统调用输出一个字符 */
static void putchar(char c) {
    __asm__ volatile ("int $0x80" : : "a"(1), "b"((unsigned int)c) : "memory");
}

/** getchar - 通过系统调用获取一个字符（阻塞） */
static char getchar(void) {
    char c;
    __asm__ volatile ("int $0x80" : "=a"(c) : "a"(2) : "memory");
    return c;
}

/** write - 通过系统调用输出字符串 */
static void write(const char* s) {
    __asm__ volatile ("int $0x80" : : "a"(3), "b"((unsigned int)s) : "memory");
}

/** exit - 通过系统调用退出程序 */
static void exit(int status) {
    __asm__ volatile ("int $0x80" : : "a"(4), "b"(status) : "memory");
}

/** yield - 通过系统调用让出 CPU（协作式调度测试） */
static void yield(void) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_YIELD) : "memory");
}

/** getmouse - 通过系统调用获取鼠标状态 */
static int getmouse(int *x, int *y, int *buttons) {
    /* 用一个 3 × int32 的缓冲区接收鼠标数据 */
    unsigned int buf[3];
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_GETMOUSE), "b"((unsigned int)buf) : "memory");
    if (ret == 0) {
        *x = (int)buf[0];
        *y = (int)buf[1];
        *buttons = (int)buf[2];
    }
    return ret;
}

/* ==================== 设置类系统调用封装（settings TUI 用） ==================== */

/** set_cursor - 定位 VGA 光标 */
static void set_cursor(int row, int col) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_SET_CURSOR), "b"((unsigned int)row), "c"((unsigned int)col) : "memory");
}

/** get_cursor - 读光标位置（(行<<16)|列） */
static int get_cursor(void) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_GET_CURSOR) : "memory");
    return r;
}

/** clear_screen - 清屏 */
static void clear_screen(void) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_CLEAR) : "memory");
}

/** get_sens - 读鼠标灵敏度 */
static int get_sens(void) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_GET_SENS) : "memory");
    return r;
}

/** set_sens - 写鼠标灵敏度 */
static void set_sens(int s) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_SET_SENS), "b"((unsigned int)s) : "memory");
}

/** get_color - 读默认颜色 */
static int get_color(void) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_GET_COLOR) : "memory");
    return r;
}

/** set_color - 写默认颜色 */
static void set_color(int c) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_SET_COLOR), "b"((unsigned int)c) : "memory");
}

/** protect - 锁定保护区域：文本光标只能在当前行以下移动 */
static void protect(void) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_PROTECT) : "memory");
}

/** get_pglyph - 读鼠标指针图案 */
static int get_pglyph(void) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_GET_PGLYPH) : "memory");
    return r;
}

/** set_pglyph - 写鼠标指针图案 */
static void set_pglyph(int g) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_SET_PGLYPH), "b"((unsigned int)g) : "memory");
}

/** write_file - 写文件（新建或覆盖） */
static int write_file(const char* name, const void* data, unsigned int size) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_WRITE_FILE), "b"((unsigned int)name), "c"((unsigned int)data), "d"(size) : "memory");
    return r;
}

/** delete_file - 删除文件 */
static int delete_file(const char* name) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_DELETE_FILE), "b"((unsigned int)name) : "memory");
    return r;
}

/** read_file - 读文件 */
static int read_file(const char* name, void* buf, unsigned int max) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_READ_FILE), "b"((unsigned int)name), "c"((unsigned int)buf), "d"(max) : "memory");
    return r;
}

/** mkdir - 创建子目录 */
static int mkdir_sys(const char* path) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_MKDIR), "b"((unsigned int)path) : "memory");
    return r;
}

/** list_dir - 列目录，返回条目数（-1 失败） */
static int list_dir(const char* path, struct dirent* buf, unsigned int max) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_LIST), "b"((unsigned int)path), "c"((unsigned int)buf), "d"(max) : "memory");
    return r;
}

/** tasks_sys - 打印任务列表（内核输出） */
static void tasks_sys(void) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_TASKS) : "memory");
}

/** get_time - 读时间（(时<<16)|(分<<8)|秒） */
static int get_time(void) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_GET_TIME) : "memory");
    return r;
}

/** get_date - 读日期（((年-2000)<<16)|(月<<8)|日） */
static int get_date(void) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_GET_DATE) : "memory");
    return r;
}

/** get_uptime - 读开机时长（秒） */
static int get_uptime(void) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_UPTIME) : "memory");
    return r;
}

/* 前置声明：print_num 定义在后面的 settings TUI 部分 */
static void print_num(int v);

/* ==================== 工具函数 ==================== */

/** strlen - 计算字符串长度 */
static unsigned int strlen(const char* s) {
    unsigned int len = 0;
    while (s[len]) len++;
    return len;
}

/** strcmp - 比较两个字符串 */
static int strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}

/** strncmp - 比较两个字符串的前 n 个字符 */
static int strncmp(const char* a, const char* b, unsigned int n) {
    while (n-- && *a && *b && *a == *b) { a++; b++; }
    if (n == (unsigned int)-1) return 0;
    return *a - *b;
}

/**
 * readline - 读取一行输入（支持命令历史）
 * @buf:  缓冲区
 * @size: 缓冲区大小
 * 返回值：读取的字符数（不含 '\0'）
 *
 * 支持：
 *   - 普通字符：回显并存入缓冲区
 *   - Backspace（\b）：擦除上一个字符
 *   - Enter（\n）：结束输入，保存到历史
 *   - Up/Down（特殊码 0x01/0x02）：翻页命令历史（内核拦截方向键后返回）
 */

/* 命令历史（环形，最多 HIST_MAX 条） */
#define HIST_MAX 8
static char history[HIST_MAX][128];
static int hist_count = 0;    /* 历史条数 */
static int hist_pos = 0;      /* 0=正在编辑，>0=浏览中的历史位置 */

/**
 * hist_apply - 用历史第 i 条（0=最新）填充当前编辑行并重绘
 * 返回新行长度
 */
static unsigned int hist_apply(char* buf, unsigned int size, int i,
                               int start_row, int start_col, unsigned int disp_len) {
    /* 清掉当前显示内容 */
    set_cursor(start_row, start_col);
    for (unsigned int k = 0; k < disp_len; k++) putchar(' ');
    set_cursor(start_row, start_col);
    /* 填入历史条目 */
    const char* h = history[hist_count - i];
    unsigned int k = 0;
    while (h[k] && k < size - 1) { buf[k] = h[k]; putchar(h[k]); k++; }
    buf[k] = '\0';
    return k;
}

/**
 * hist_clear_line - 清掉当前编辑行的显示内容
 */
static void hist_clear_line(int start_row, int start_col, unsigned int disp_len) {
    set_cursor(start_row, start_col);
    for (unsigned int k = 0; k < disp_len; k++) putchar(' ');
    set_cursor(start_row, start_col);
}

/**
 * hist_add - 把一条命令存入历史（去重，环形）
 */
static void hist_add(const char* line) {
    unsigned int len = 0;
    while (line[len]) len++;
    if (len == 0) return;
    if (hist_count > 0 && strcmp(history[hist_count-1], line) == 0) return;

    if (hist_count < HIST_MAX) {
        unsigned int k = 0;
        while (line[k] && k < sizeof(history[hist_count]) - 1) { history[hist_count][k] = line[k]; k++; }
        history[hist_count][k] = '\0';
        hist_count++;
    } else {
        /* 环形：整体前移，丢弃最旧 */
        for (int h = 0; h < HIST_MAX - 1; h++) {
            unsigned int k = 0;
            while (history[h+1][k] && k < sizeof(history[h]) - 1) { history[h][k] = history[h+1][k]; k++; }
            history[h][k] = '\0';
        }
        unsigned int k = 0;
        while (line[k] && k < sizeof(history[HIST_MAX-1]) - 1) { history[HIST_MAX-1][k] = line[k]; k++; }
        history[HIST_MAX-1][k] = '\0';
    }
}

static unsigned int readline(char* buf, unsigned int size) {
    unsigned int pos = 0;
    unsigned int disp_len = 0;   /* 当前屏幕上显示的行长度 */
    buf[0] = '\0';

    /* 记录提示符位置（历史翻页重绘用） */
    unsigned int cur = (unsigned int)get_cursor();
    int start_row = (int)(cur >> 16);
    int start_col = (int)(cur & 0xFFFF);

    while (1) {
        char c = getchar();

        if (c == 0x01) {
            /* Up：历史向前翻 */
            if (hist_count > 0 && hist_pos < hist_count) {
                hist_pos++;
                disp_len = hist_apply(buf, size, hist_pos, start_row, start_col, disp_len);
                pos = disp_len;
            }
        } else if (c == 0x02) {
            /* Down：历史向后翻 */
            if (hist_pos > 0) {
                hist_pos--;
                if (hist_pos == 0) {
                    hist_clear_line(start_row, start_col, disp_len);
                    pos = 0;
                    disp_len = 0;
                    buf[0] = '\0';
                } else {
                    disp_len = hist_apply(buf, size, hist_pos, start_row, start_col, disp_len);
                    pos = disp_len;
                }
            }
        } else if (c == '\n' || c == '\r') {
            /* Enter：结束输入，输出换行，保存历史 */
            putchar('\n');
            buf[pos] = '\0';
            hist_add(buf);
            hist_pos = 0;
            return pos;
        } else if (c == '\b') {
            /* Backspace：回退一格 */
            if (pos > 0) {
                pos--;
                putchar('\b');   /* 光标左移 */
                putchar(' ');    /* 擦除字符 */
                putchar('\b');   /* 光标回到擦除位置 */
                disp_len--;
            }
        } else if (c >= ' ' && c <= '~') {
            /* 可打印字符 */
            if (pos < size - 1) {
                buf[pos++] = c;
                putchar(c);      /* 回显 */
                disp_len++;
            }
            /* 缓冲区满——简单忽略 */
        }
        /* 其他控制字符忽略 */
    }
}

/* ==================== 命令实现 ==================== */

/** cmd_help - 显示帮助信息 */
static void cmd_help(const char* args) { (void)args;
    write("\nHaoOS Shell v0.1 - Available commands:\n");
    write("  help      - Show this help message\n");
    write("  echo      - Echo text back\n");
    write("  clear     - Clear the screen\n");
    write("  tasks     - Show running tasks\n");
    write("  mouse     - Show mouse position\n");
    write("  save      - Save text to a file (overwrites)\n");
    write("  cat       - Show file contents\n");
    write("  mkdir     - Create a directory\n");
    write("  ls        - List directory contents\n");
    write("  rm        - Delete a file\n");
    write("  time      - Show date and time\n");
    write("  uptime    - Show time since boot\n");
    write("  settings  - Open settings TUI\n");
    write("  exit      - Halt the system\n");
    write("\n");
}

/** cmd_echo - 回显参数 */
static void cmd_echo(const char* args) {
    write(args);
    putchar('\n');
}

/** cmd_clear - 清屏（直接写 VGA 需要用户态页映射，暂用换行模拟） */
static void cmd_clear(const char* args) { (void)args;
    /* 通过输出大量换行来"清屏" */
    for (int i = 0; i < 30; i++) putchar('\n');
}

/** cmd_tasks - 显示任务列表（内核打印） */
static void cmd_tasks(const char* args) { (void)args;
    tasks_sys();
}

/** cmd_exit - 关闭系统（暂停 CPU） */
static void cmd_exit(const char* args) { (void)args;
    write("Halting system.\n");
    exit(0);
}

/** cmd_time - 显示当前日期时间（RTC） */
static void cmd_time(const char* args) { (void)args;
    unsigned int t = (unsigned int)get_time();
    unsigned int d = (unsigned int)get_date();
    print_num((int)(2000 + ((d >> 16) & 0xFF))); putchar('-');
    if (((d >> 8) & 0xFF) < 10) putchar('0');
    print_num((int)((d >> 8) & 0xFF)); putchar('-');
    if ((d & 0xFF) < 10) putchar('0');
    print_num((int)(d & 0xFF)); putchar(' ');
    if (((t >> 16) & 0xFF) < 10) putchar('0');
    print_num((int)((t >> 16) & 0xFF)); putchar(':');
    if (((t >> 8) & 0xFF) < 10) putchar('0');
    print_num((int)((t >> 8) & 0xFF)); putchar(':');
    if ((t & 0xFF) < 10) putchar('0');
    print_num((int)(t & 0xFF));
    putchar('\n');
}

/** cmd_uptime - 显示开机时长（秒 → 天/时/分/秒） */
static void cmd_uptime(const char* args) { (void)args;
    unsigned int s = (unsigned int)get_uptime();
    unsigned int days = s / 86400;
    unsigned int h = (s % 86400) / 3600;
    unsigned int m = (s % 3600) / 60;
    unsigned int sec = s % 60;
    write("Up ");
    print_num((int)days); write("d ");
    if (h < 10) putchar('0');
    print_num((int)h); putchar(':');
    if (m < 10) putchar('0');
    print_num((int)m); putchar(':');
    if (sec < 10) putchar('0');
    print_num((int)sec);
    putchar('\n');
}
static void cmd_save(const char* args) {
    /* 第一个词是文件名，其余是内容 */
    char fname[32];
    unsigned int i = 0;
    while (args[i] && args[i] != ' ' && i < sizeof(fname) - 1) { fname[i] = args[i]; i++; }
    fname[i] = '\0';
    while (args[i] == ' ') i++;
    const char* content = args + i;

    if (fname[0] == '\0' || content[0] == '\0') {
        write("Usage: save <filename> <content>\n");
        return;
    }
    if (write_file(fname, content, (unsigned int)strlen(content)) == 0) {
        write("Saved ");
        write(fname);
        write(" (");
        print_num((int)strlen(content));
        write(" bytes)\n");
    } else {
        write("Save failed (no space).\n");
    }
}

/** cmd_mkdir - 创建子目录 */
static void cmd_mkdir(const char* args) {
    char path[64];
    unsigned int i = 0;
    while (args[i] && args[i] != ' ' && i < sizeof(path) - 1) { path[i] = args[i]; i++; }
    path[i] = '\0';
    if (path[0] == '\0') {
        write("Usage: mkdir <path>\n");
        return;
    }
    if (mkdir_sys(path) == 0) {
        write("Created directory ");
        write(path);
        write("\n");
    } else {
        write("mkdir failed (exists / bad path / no space).\n");
    }
}

/** cmd_ls - 列出目录内容 */
static void cmd_ls(const char* args) {
    char path[64];
    unsigned int i = 0;
    while (args[i] && args[i] != ' ' && i < sizeof(path) - 1) { path[i] = args[i]; i++; }
    path[i] = '\0';

    struct dirent ents[64];
    int n = list_dir(path[0] ? path : NULL, ents, 64);
    if (n < 0) {
        write("Directory not found.\n");
        return;
    }
    if (n == 0) {
        write("(empty)\n");
        return;
    }
    for (int k = 0; k < n; k++) {
        /* 跳过 '.' 和 '..' */
        if (ents[k].name[0] == 0x2E) continue;
        /* 8.3 名 → 可读名：基本名去尾空格，有扩展名加点输出 */
        char disp[16];
        int d = 0;
        int main_len = 8;
        while (main_len > 0 && ents[k].name[main_len - 1] == ' ') main_len--;
        for (int j = 0; j < main_len; j++) disp[d++] = (char)ents[k].name[j];
        int ext_len = 3;
        while (ext_len > 0 && ents[k].name[8 + ext_len - 1] == ' ') ext_len--;
        if (ext_len > 0) {
            disp[d++] = '.';
            for (int j = 0; j < ext_len; j++) disp[d++] = (char)ents[k].name[8 + j];
        }
        disp[d] = '\0';
        write(disp);
        if (ents[k].attributes & 0x10) {
            write("  <DIR>\n");
        } else {
            write("  ");
            print_num((int)ents[k].file_size);
            write(" bytes\n");
        }
    }
}

/** cmd_rm - 删除文件 */
static void cmd_rm(const char* args) {
    char fname[32];
    unsigned int i = 0;
    while (args[i] && args[i] != ' ' && i < sizeof(fname) - 1) { fname[i] = args[i]; i++; }
    fname[i] = '\0';

    if (fname[0] == '\0') {
        write("Usage: rm <filename>\n");
        return;
    }
    if (delete_file(fname) == 0) {
        write("Deleted ");
        write(fname);
        write("\n");
    } else {
        write("File not found.\n");
    }
}

/** cmd_cat - 显示文件内容 */
static void cmd_cat(const char* args) {
    char fname[32];
    unsigned int i = 0;
    while (args[i] && args[i] != ' ' && i < sizeof(fname) - 1) { fname[i] = args[i]; i++; }
    fname[i] = '\0';

    if (fname[0] == '\0') {
        write("Usage: cat <filename>\n");
        return;
    }
    char buf[512];
    int n = read_file(fname, buf, sizeof(buf));
    if (n < 0) {
        write("File not found.\n");
        return;
    }
    for (int k = 0; k < n; k++) putchar(buf[k]);
    putchar('\n');
}

/** cmd_mouse - 显示鼠标位置 */
static void cmd_mouse(const char* args) { (void)args;
    int x, y, btn;
    if (getmouse(&x, &y, &btn) == 0) {
        write("Mouse: x=");
        {
            /* 简单数字转字符串输出 */
            char nbuf[16];
            int tmp = x, idx = 0;
            if (tmp < 0) { putchar('-'); tmp = -tmp; }
            if (tmp == 0) { putchar('0'); }
            else {
                while (tmp > 0) { nbuf[idx++] = '0' + (tmp % 10); tmp /= 10; }
                while (idx > 0) putchar(nbuf[--idx]);
            }
        }
        write(" y=");
        {
            char nbuf[16];
            int tmp = y, idx = 0;
            if (tmp < 0) { putchar('-'); tmp = -tmp; }
            if (tmp == 0) { putchar('0'); }
            else {
                while (tmp > 0) { nbuf[idx++] = '0' + (tmp % 10); tmp /= 10; }
                while (idx > 0) putchar(nbuf[--idx]);
            }
        }
        write(" buttons=0x");
        {
            /* 十六进制输出 */
            char hex[3];
            hex[0] = "0123456789ABCDEF"[(btn >> 4) & 0xF];
            hex[1] = "0123456789ABCDEF"[btn & 0xF];
            hex[2] = '\0';
            write(hex);
        }
        putchar('\n');
    } else {
        write("Mouse: no mouse available.\n");
    }
}

/* ==================== 设置 TUI ==================== */

/** 主题颜色表：名字 + 属性字节（低4位前景色，高4位背景色） */
static const char* color_names[] = { "White", "Cyan", "Green", "Red", "Yellow", "Blue", "Pink" };
static const int   color_values[] = { 0x0F, 0x0B, 0x0A, 0x0C, 0x0E, 0x09, 0x0D };
#define COLOR_COUNT 7

/** 指针图案表：名字 + CP437 字形码 */
static const char* glyph_names[] = { "Block", "Dark", "Mid", "Light", "Right", "Left", "Up", "Down" };
static const int   glyph_values[] = { 0xDB, 0xB2, 0xB1, 0xB0, 0x10, 0x11, 0x1E, 0x1F };
#define GLYPH_COUNT 8

/** print_num - 十进制整数输出 */
static void print_num(int v) {
    char buf[8];
    int idx = 0;
    if (v == 0) { putchar('0'); return; }
    if (v < 0) { putchar('-'); v = -v; }
    while (v > 0) { buf[idx++] = '0' + (v % 10); v /= 10; }
    while (idx > 0) putchar(buf[--idx]);
}

/** tui_pad - 输出 n 个空格，清除值变短时残留的旧字符 */
static void tui_pad(int n) {
    for (int i = 0; i < n; i++) putchar(' ');
}

/** tui_frame - 绘制全屏边框（上下双线 + 左右边线 + 标题） */
static void tui_frame(void) {
    /* 顶边框：┌──── HaoOS Settings ────┐ */
    set_cursor(0, 0);
    putchar(0xDA);                      /* ┌ */
    for (int i = 1; i < 79; i++) putchar(0xC4);   /* ─ */
    putchar(0xBF);                      /* ┐ */
    set_cursor(0, 30);
    write("HaoOS Settings");

    /* 左右边框 */
    for (int r = 1; r < 24; r++) {
        set_cursor(r, 0);
        putchar(0xB3);                  /* │ */
        set_cursor(r, 79);
        putchar(0xB3);
    }

    /* 底边框：└──────┘ */
    set_cursor(24, 0);
    putchar(0xC0);                      /* └ */
    for (int i = 1; i < 79; i++) putchar(0xC4);
    putchar(0xD9);                      /* ┘ */
}

/** tui_draw - 绘制设置界面（全屏边框，用 ASCII 标签保证 CP437 对齐） */
static void tui_draw(int sel, int sens, int color_idx, int glyph_idx) {
    int filled = sens > 10 ? 10 : sens;

    tui_frame();

    /* 行 3：鼠标灵敏度 */
    set_cursor(3, 8);
    write(sel == 0 ? "> " : "  ");
    write("Mouse sensitivity  [");
    for (int i = 0; i < filled; i++) putchar('#');
    for (int i = filled; i < 10; i++) putchar('.');
    write("] ");
    print_num(sens);
    tui_pad(2);                    /* 数字变短（16→2）时清除残留 */

    /* 行 5：主题颜色 */
    set_cursor(5, 8);
    write(sel == 1 ? "> " : "  ");
    write("Theme color        [");
    for (int i = 0; i < 10; i++) putchar('#');
    write("] ");
    write(color_names[color_idx]);
    tui_pad(6 - (int)strlen(color_names[color_idx]));   /* 名字变短（Yellow→Red）时清除残留 */
    putchar(' ');
    putchar('\xDB');   /* 颜色预览块（以当前主题色渲染） */

    /* 行 7：指针图案 */
    set_cursor(7, 8);
    write(sel == 2 ? "> " : "  ");
    write("Pointer style      [");
    for (int i = 0; i < 10; i++) putchar('#');
    write("] ");
    write(glyph_names[glyph_idx]);
    tui_pad(5 - (int)strlen(glyph_names[glyph_idx]));   /* 名字变短（Light→Up）时清除残留 */
    putchar(' ');
    putchar((char)glyph_values[glyph_idx]);   /* 指针图案预览 */

    /* 行 20：操作提示（底部区域） */
    set_cursor(20, 20);
    write("W/S select    A/D adjust    Q/Esc exit");
}

/**
 * settings_tui - 全屏设置界面
 *
 * 支持：
 *   W/S     切换选中项
 *   A/D（或 +/-）调整数值，改动实时生效
 *   Q/Esc   退出并返回 shell
 */
static void settings_tui(void) {
    int sel = 0;
    int sens = get_sens();
    int color_idx = 0;
    int cur = get_color();
    for (int i = 0; i < COLOR_COUNT; i++) {
        if (color_values[i] == cur) { color_idx = i; break; }
    }
    int glyph_idx = 0;
    int cur_glyph = get_pglyph();
    for (int i = 0; i < GLYPH_COUNT; i++) {
        if (glyph_values[i] == cur_glyph) { glyph_idx = i; break; }
    }

    clear_screen();
    while (1) {
        set_color(color_values[color_idx]);   /* 主题色实时生效 */
        set_sens(sens);                       /* 灵敏度实时生效 */
        set_pglyph(glyph_values[glyph_idx]);  /* 指针图案实时生效 */
        tui_draw(sel, sens, color_idx, glyph_idx);

        char c = getchar();
        if (c == 'w' || c == 'W' || c == 's' || c == 'S') {
            sel = (sel + 1) % 3;              /* 三项之间循环切换 */
        } else if (c == 'a' || c == 'A' || c == '-' || c == '_') {
            if (sel == 0) { if (sens > 1) sens--; }
            else if (sel == 1) { color_idx = (color_idx + COLOR_COUNT - 1) % COLOR_COUNT; }
            else { glyph_idx = (glyph_idx + GLYPH_COUNT - 1) % GLYPH_COUNT; }
        } else if (c == 'd' || c == 'D' || c == '+' || c == '=') {
            if (sel == 0) { if (sens < 16) sens++; }
            else if (sel == 1) { color_idx = (color_idx + 1) % COLOR_COUNT; }
            else { glyph_idx = (glyph_idx + 1) % GLYPH_COUNT; }
        } else if (c == 'q' || c == 'Q' || c == 0x1B) {
            break;
        }
    }
    clear_screen();
}

/** cmd_settings - settings 命令入口 */
static void cmd_settings(const char* args) { (void)args;
    settings_tui();
}

/* ==================== 命令表 ==================== */
typedef struct {
    const char* name;       /* 命令名 */
    void (*handler)(const char* args);  /* 处理函数 */
} command_t;

static command_t commands[] = {
    { "help",  cmd_help },
    { "?",     cmd_help },
    { "echo",  cmd_echo },
    { "clear", cmd_clear },
    { "cls",   cmd_clear },
    { "tasks", cmd_tasks },
    { "mouse", cmd_mouse },
    { "save",  cmd_save },
    { "cat",   cmd_cat },
    { "rm",    cmd_rm },
    { "mkdir", cmd_mkdir },
    { "ls",    cmd_ls },
    { "time",  cmd_time },
    { "uptime", cmd_uptime },
    { "settings", cmd_settings },
    { "set",   cmd_settings },
    { "exit",  cmd_exit },
    { NULL,    NULL }
};

/**
 * execute - 解析并执行命令
 * @line: 输入的命令行
 *
 * 按空格分割命令名和参数，在命令表中查找匹配并执行。
 * 如果未找到，提示未知命令。
 */
static void execute(const char* line) {
    /* 跳过前导空格 */
    while (*line == ' ') line++;
    if (*line == '\0') return;  /* 空行 */

    /* 提取命令名（第一个空格前的部分） */
    char cmd_name[32];
    unsigned int i = 0;
    while (line[i] && line[i] != ' ' && i < sizeof(cmd_name) - 1) {
        cmd_name[i] = line[i];
        i++;
    }
    cmd_name[i] = '\0';

    /* 提取参数（命令名之后的部分） */
    const char* args = line + i;
    while (*args == ' ') args++;

    /* 在命令表中查找 */
    for (i = 0; commands[i].name != NULL; i++) {
        if (strcmp(cmd_name, commands[i].name) == 0) {
            commands[i].handler(args);
            return;
        }
    }

    /* 未找到 */
    write("Unknown command: ");
    write(cmd_name);
    write("\nType 'help' for available commands.\n");
}

/* ==================== Shell 主循环 ==================== */

/**
 * main - Shell 入口
 *
 * 无限循环：
 *   1. 输出提示符 "$ "
 *   2. 读取一行输入
 *   3. 解析并执行命令
 */
void main(void) {
    char line[128];

    /* 启动提示 */
    write("HaoOS Shell v0.1\n");
    write("Type 'help' for commands.\n");

    while (1) {
        /* 输出提示符，并锁定编辑区：
         * 文本光标（含鼠标左键放置）只能在提示行及以下移动，
         * 历史输出不可被覆盖。鼠标指针本身不受限制，可自由移动。 */
        write("$ ");
        protect();

        /* 读取用户输入 */
        unsigned int len = readline(line, sizeof(line));

        /* 执行命令 */
        execute(line);

        /* 让出 CPU：触发一次 shell→demo→shell 切换（验证调度器） */
        yield();
    }
}
