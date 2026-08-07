/*
 * shell.c - HaoOS 用户态 Shell（ring 3）
 * 所有 I/O 走 int 0x80 系统调用（eax=调用号，ebx/ecx=参数）。
 * 内核按扁平二进制加载、跳到文件开头执行，所以 _entry 必须放最前。
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
#define SYS_USB_INFO   27  /* 打印 USB 设备信息 */
#define SYS_MOUNT      28  /* 挂载设备（ebx=设备名, ecx=挂载点） */
#define SYS_UMOUNT     29  /* 卸载（ebx=挂载点） */
#define SYS_DEVICES    30  /* 打印设备/挂载表 */
#define SYS_GETKEY_NB  31  /* 非阻塞取键：有键返回键值，无键返回 -1 */

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

/* ---- 引导入口 ---- */

/* 引导入口：跳到 main */
__attribute__((naked)) void _entry(void) {
    __asm__ volatile (
        "jmp main"
    );
}

/* ---- 系统调用封装 ---- */

/* putchar - 通过系统调用输出一个字符 */
static void putchar(char c) {
    __asm__ volatile ("int $0x80" : : "a"(1), "b"((unsigned int)c) : "memory");
}

/* getchar - 通过系统调用获取一个字符（阻塞） */
static char getchar(void) {
    char c;
    __asm__ volatile ("int $0x80" : "=a"(c) : "a"(2) : "memory");
    return c;
}

/* getkey_nb - 非阻塞取键：有键返回键值，无键返回 -1（TUI 事件循环用） */
static int getkey_nb(void) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_GETKEY_NB) : "memory");
    return r;
}

/* write - 通过系统调用输出字符串 */
static void write(const char* s) {
    __asm__ volatile ("int $0x80" : : "a"(3), "b"((unsigned int)s) : "memory");
}

/* exit - 通过系统调用退出程序 */
static void exit(int status) {
    __asm__ volatile ("int $0x80" : : "a"(4), "b"(status) : "memory");
}

/* yield - 通过系统调用让出 CPU（协作式调度测试） */
static void yield(void) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_YIELD) : "memory");
}

/* getmouse - 通过系统调用获取鼠标状态 */
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

/* ---- 设置类系统调用封装（settings TUI 用） ---- */

/* set_cursor - 定位 VGA 光标 */
static void set_cursor(int row, int col) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_SET_CURSOR), "b"((unsigned int)row), "c"((unsigned int)col) : "memory");
}

/* get_cursor - 读光标位置（(行<<16)|列） */
static int get_cursor(void) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_GET_CURSOR) : "memory");
    return r;
}

/* clear_screen - 清屏 */
static void clear_screen(void) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_CLEAR) : "memory");
}

/* get_sens - 读鼠标灵敏度 */
static int get_sens(void) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_GET_SENS) : "memory");
    return r;
}

/* set_sens - 写鼠标灵敏度 */
static void set_sens(int s) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_SET_SENS), "b"((unsigned int)s) : "memory");
}

/* get_color - 读默认颜色 */
static int get_color(void) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_GET_COLOR) : "memory");
    return r;
}

/* set_color - 写默认颜色 */
static void set_color(int c) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_SET_COLOR), "b"((unsigned int)c) : "memory");
}

/* protect - 锁定保护区域：文本光标只能在当前行以下移动 */
static void protect(void) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_PROTECT) : "memory");
}

/* get_pglyph - 读鼠标指针图案 */
static int get_pglyph(void) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_GET_PGLYPH) : "memory");
    return r;
}

/* set_pglyph - 写鼠标指针图案 */
static void set_pglyph(int g) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_SET_PGLYPH), "b"((unsigned int)g) : "memory");
}

/* write_file - 写文件（新建或覆盖） */
static int write_file(const char* name, const void* data, unsigned int size) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_WRITE_FILE), "b"((unsigned int)name), "c"((unsigned int)data), "d"(size) : "memory");
    return r;
}

/* delete_file - 删除文件 */
static int delete_file(const char* name) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_DELETE_FILE), "b"((unsigned int)name) : "memory");
    return r;
}

/* read_file - 读文件 */
static int read_file(const char* name, void* buf, unsigned int max) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_READ_FILE), "b"((unsigned int)name), "c"((unsigned int)buf), "d"(max) : "memory");
    return r;
}

/* mkdir - 创建子目录 */
static int mkdir_sys(const char* path) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_MKDIR), "b"((unsigned int)path) : "memory");
    return r;
}

/* list_dir - 列目录，返回条目数（-1 失败） */
static int list_dir(const char* path, struct dirent* buf, unsigned int max) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_LIST), "b"((unsigned int)path), "c"((unsigned int)buf), "d"(max) : "memory");
    return r;
}

/* tasks_sys - 打印任务列表（内核输出） */
static void tasks_sys(void) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_TASKS) : "memory");
}

/* mount_sys - 挂载设备（dev=设备名, point=挂载点） */
static int mount_sys(const char* dev, const char* point) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_MOUNT), "b"((unsigned int)dev), "c"((unsigned int)point) : "memory");
    return r;
}

/* umount_sys - 卸载挂载点 */
static int umount_sys(const char* point) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_UMOUNT), "b"((unsigned int)point) : "memory");
    return r;
}

/* devices_sys - 打印设备/挂载表（内核输出） */
static void devices_sys(void) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_DEVICES) : "memory");
}

/* get_time - 读时间（(时<<16)|(分<<8)|秒） */
static int get_time(void) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_GET_TIME) : "memory");
    return r;
}

/* get_date - 读日期（((年-2000)<<16)|(月<<8)|日） */
static int get_date(void) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_GET_DATE) : "memory");
    return r;
}

/* get_uptime - 读开机时长（秒） */
static int get_uptime(void) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(SYS_UPTIME) : "memory");
    return r;
}

/* 前置声明：print_num 定义在后面的 settings TUI 部分 */
static void print_num(int v);

/* ---- 工具函数 ---- */

/* strlen - 计算字符串长度 */
static unsigned int strlen(const char* s) {
    unsigned int len = 0;
    while (s[len]) len++;
    return len;
}

/* strcmp - 比较两个字符串 */
static int strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}

/* strncmp - 比较两个字符串的前 n 个字符 */
static int strncmp(const char* a, const char* b, unsigned int n) {
    while (n-- && *a && *b && *a == *b) { a++; b++; }
    if (n == (unsigned int)-1) return 0;
    return *a - *b;
}

/* ---- 行编辑（readline）：标准终端语义 ----
 * ←/→ 移动光标，字符插入光标处，Backspace 删光标前字符，整行重绘。
 * 内核把方向键转成特殊码（与 ASCII 控制码不冲突）：
 *   0x01 = Up（历史向前翻），0x02 = Down（历史向后翻），
 *   0x03 = ←（光标左移），0x04 = →（光标右移）。 */

/* 命令历史（环形，最多 HIST_MAX 条） */
#define HIST_MAX 8
#define LINE_WIDTH 80          /* VGA 文本模式行宽（与内核 vga.h 一致） */
#define VGA_HEIGHT 25          /* VGA 文本模式行数 */
static char history[HIST_MAX][128];
static int hist_count = 0;    /* 历史条数 */
static int hist_pos = 0;      /* 0=正在编辑，>0=浏览中的历史位置 */

/* 把 VGA 光标定位到编辑行内第 pos 个字符处：
 * 屏幕位置 = 输入起点(start_row,start_col) + pos，超宽自动换行 */
static void line_set_cursor(int start_row, int start_col, unsigned int pos) {
    set_cursor(start_row + (start_col + (int)pos) / LINE_WIDTH,
               (start_col + (int)pos) % LINE_WIDTH);
}

/* 保证编辑行有足够显示空间：编辑行若会触底（写到 row 24 以下），
 * 光标先跳到底行发 \n 触发内核整屏上滚（编辑行内容随之整体上移，
 * protected_row 同步减），start_row 跟着 -1。可多次调用逐行腾空。
 * ⚠️ 不能直接 set_cursor 越界：vga 普通字符输出在底行右角是
 * 钳制的（TUI 修复），只有 \n 能触发 scroll。 */
static void ensure_edit_space(int* start_row, int start_col, unsigned int len) {
    while (*start_row + (start_col + (int)len) / LINE_WIDTH >= VGA_HEIGHT) {
        set_cursor(VGA_HEIGHT - 1, 0);
        putchar('\n');   /* 底行 \n → 内核 scroll() 整屏上移一行 */
        (*start_row)--;  /* 编辑行内容整体上移了一行 */
    }
}

/* 整行重绘：定位行首 → 重打 buf[0..len) → 行尾残留补空格 →
 * 光标定位到 pos。disp_len 记屏幕当前显示长度（残留清除用）。 */
static void line_redraw(int start_row, int start_col, const char* buf,
                        unsigned int len, unsigned int* disp_len, unsigned int pos) {
    set_cursor(start_row, start_col);
    for (unsigned int i = 0; i < len; i++) putchar(buf[i]);
    /* 新行比旧行短时，把尾部残留字符清成空格 */
    for (unsigned int i = len; i < *disp_len; i++) putchar(' ');
    *disp_len = len;
    line_set_cursor(start_row, start_col, pos);
}

/* 用历史第 i 条填充编辑行并整行重绘，返回新行长度 */
static unsigned int hist_apply(char* buf, unsigned int size, int i,
                               int start_row, int start_col, unsigned int* disp_len) {
    const char* h = history[hist_count - i];
    unsigned int k = 0;
    while (h[k] && k < size - 1) { buf[k] = h[k]; k++; }
    buf[k] = '\0';
    line_redraw(start_row, start_col, buf, k, disp_len, k);
    return k;
}

/* 清掉当前编辑行显示 */
static void hist_clear_line(int start_row, int start_col, unsigned int* disp_len) {
    line_redraw(start_row, start_col, "", 0, disp_len, 0);
}

/* buf[from..from+n) 整体右移一格（插入用；从尾往前复制防覆盖） */
static void shift_right(char* buf, unsigned int from, unsigned int n) {
    unsigned int i = n;
    while (i > 0) { i--; buf[from + i + 1] = buf[from + i]; }
}

/* buf[from..from+n) 整体左移一格（退格用；从头往后复制防覆盖） */
static void shift_left(char* buf, unsigned int from, unsigned int n) {
    for (unsigned int i = 0; i < n; i++) buf[from + i - 1] = buf[from + i];
}

/* 存一条命令进历史（去重；满则整体前移丢最旧） */
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

/* 读一行：字符插入光标处、←→ 移光标、Backspace 删光标前字符、
 * ↑↓ 翻历史、鼠标点击编辑行内定位光标、Enter 返回。
 * 事件循环：非阻塞取键 + 鼠标边沿检测（按下→抬起且位置不变=点击）。 */
static unsigned int readline(char* buf, unsigned int size) {
    unsigned int pos = 0;      /* 光标位置（buf 内偏移） */
    unsigned int len = 0;      /* 行长度 */
    unsigned int disp_len = 0; /* 屏幕当前显示的行长度（残留清除用） */
    buf[0] = '\0';

    /* 记录输入起点（提示符后），重绘/光标定位用 */
    unsigned int cur = (unsigned int)get_cursor();
    int start_row = (int)(cur >> 16);
    int start_col = (int)(cur & 0xFFFF);

    /* 鼠标点击边沿检测状态：上一帧左键状态 + 按下时的位置 */
    int prev_lbtn = 0;
    int press_x = 0, press_y = 0;

    while (1) {
        /* ---- 鼠标轮询：点击边沿检测 ---- */
        int mx, my, mb;
        if (getmouse(&mx, &my, &mb) == 0) {
            if (mb & 1) {
                /* 左键按下：记录按下位置（只记一次，按住不重复记） */
                if (!prev_lbtn) { press_x = mx; press_y = my; }
            } else if (prev_lbtn) {
                /* 左键抬起：与按下位置相同 = 一次点击（拖动不算） */
                if (mx == press_x && my == press_y) {
                    /* 点击定位：线性映射屏幕坐标 → 编辑行内偏移。
                     * 编辑行可能超宽换行（占多行），所以用
                     *   offset = (my - start_row) * 80 + (mx - start_col)
                     * 统一映射；点击编辑行区域外（历史区/更下方）忽略。
                     * ⚠️ 历史教训（f173359）：全局跳光标会误触——
                     * 只接受编辑行实际占用的屏幕行。 */
                    int edit_last_row = start_row +
                        (start_col + (int)len) / LINE_WIDTH;  /* 编辑行占用的最后一行 */
                    if (my >= start_row && my <= edit_last_row) {
                        int off = (my - start_row) * LINE_WIDTH + (mx - start_col);
                        if (off < 0) off = 0;
                        if (off > (int)len) off = (int)len;  /* 行尾右侧 = 行尾 */
                        pos = (unsigned int)off;
                        line_set_cursor(start_row, start_col, pos);
                    }
                }
            }
            prev_lbtn = mb & 1;
        }

        /* ---- 键盘：非阻塞取键 ---- */
        int c = getkey_nb();
        if (c == -1) {
            /* 无键无点击：让出 CPU（抢占式调度会按时唤醒，不忙转） */
            yield();
            continue;
        }

        if (c == 0x01) {
            /* Up：历史向前翻 */
            if (hist_count > 0 && hist_pos < hist_count) {
                hist_pos++;
                ensure_edit_space(&start_row, start_col, 127); /* 历史最长 127，提前腾位 */
                len = hist_apply(buf, size, hist_pos, start_row, start_col, &disp_len);
                pos = len;
            }
        } else if (c == 0x02) {
            /* Down：历史向后翻 */
            if (hist_pos > 0) {
                hist_pos--;
                if (hist_pos == 0) {
                    ensure_edit_space(&start_row, start_col, 0);
                    hist_clear_line(start_row, start_col, &disp_len);
                    pos = 0;
                    len = 0;
                    buf[0] = '\0';
                } else {
                    ensure_edit_space(&start_row, start_col, 127);
                    len = hist_apply(buf, size, hist_pos, start_row, start_col, &disp_len);
                    pos = len;
                }
            }
        } else if (c == 0x03) {
            /* ←：光标左移一格（只移光标，不重绘） */
            if (pos > 0) {
                pos--;
                line_set_cursor(start_row, start_col, pos);
            }
        } else if (c == 0x04) {
            /* →：光标右移一格（只移光标，不重绘） */
            if (pos < len) {
                pos++;
                line_set_cursor(start_row, start_col, pos);
            }
        } else if (c == '\n' || c == '\r') {
            /* Enter：结束输入，输出换行，保存历史 */
            putchar('\n');
            buf[len] = '\0';
            hist_add(buf);
            hist_pos = 0;
            return len;
        } else if (c == '\b') {
            /* Backspace：删光标前字符，整行重绘 */
            if (pos > 0) {
                shift_left(buf, pos, len - pos);
                pos--;
                len--;
                ensure_edit_space(&start_row, start_col, len);
                line_redraw(start_row, start_col, buf, len, &disp_len, pos);
            }
        } else if (c >= ' ' && c <= '~') {
            /* 可打印字符：插入光标处，整行重绘 */
            if (len < size - 1) {
                shift_right(buf, pos, len - pos);
                buf[pos] = c;
                pos++;
                len++;
                ensure_edit_space(&start_row, start_col, len);
                line_redraw(start_row, start_col, buf, len, &disp_len, pos);
            }
            /* 缓冲区满——简单忽略 */
        }
        /* 其他控制字符忽略 */
    }
}

/* ---- 命令实现 ---- */

/* 当前工作目录（Linux 风格，/ 开头）；文件命令相对路径基于它 */
static char cwd[64] = "/";

/* 把相对路径解析为绝对路径（基于 cwd）；绝对路径原样规范化。
 * 处理 .（当前段）、..（上级段）、连续斜杠；结果写入 out（≤out_size）。
 * 返回 out 本身，便于链式调用。 */
static const char* resolve_path(const char* in, char* out, unsigned int out_size) {
    /* 暂存拼接后的原始路径（相对 + cwd） */
    char raw[128];
    unsigned int r = 0;
    if (in[0] == '/') {
        while (in[r] && r < sizeof(raw) - 1) { raw[r] = in[r]; r++; }
        raw[r] = '\0';
    } else {
        unsigned int c = 0;
        while (cwd[c] && c < sizeof(raw) - 1) { raw[r++] = cwd[c++]; }
        if (r > 1 && in[0] != '\0') raw[r++] = '/';
        unsigned int i = 0;
        while (in[i] && r < sizeof(raw) - 1) { raw[r++] = in[i++]; }
        raw[r] = '\0';
    }

    /* 逐段解析：. 丢弃，.. 弹栈，其余入栈 */
    char segs[16][16];
    int n = 0;
    unsigned int i = 0;
    while (raw[i]) {
        while (raw[i] == '/') i++;
        if (!raw[i]) break;
        unsigned int s = 0;
        while (raw[i] && raw[i] != '/' && s < 15) { segs[n][s++] = raw[i++]; }
        segs[n][s] = '\0';
        if (s == 1 && segs[n][0] == '.') {
            /* 当前段，丢弃 */
        } else if (s == 2 && segs[n][0] == '.' && segs[n][1] == '.') {
            if (n > 0) n--;              /* 回上级 */
        } else {
            n++;
        }
    }

    /* 拼回绝对路径 */
    unsigned int o = 0;
    out[o++] = '/';
    for (int k = 0; k < n && o < out_size - 2; k++) {
        unsigned int s = 0;
        while (segs[k][s] && o < out_size - 2) out[o++] = segs[k][s++];
        if (k < n - 1) out[o++] = '/';
    }
    out[o] = '\0';
    return out;
}
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
    write("  cd        - Change directory (cd <path>, or cd for root)\n");
    write("  pwd       - Print working directory\n");
    write("  rm        - Delete a file\n");
    write("  mount     - Mount a device (mount <device> <point>)\n");
    write("  umount    - Unmount a filesystem (umount <point>)\n");
    write("  devices   - List devices and mounts\n");
    write("  time      - Show date and time\n");
    write("  uptime    - Show time since boot\n");
    write("  settings  - Open settings TUI\n");
    write("  exit      - Halt the system\n");
    write("\n");
}

/* cmd_echo - 回显参数 */
static void cmd_echo(const char* args) {
    write(args);
    putchar('\n');
}

/* cmd_clear - 清屏（直接写 VGA 需要用户态页映射，暂用换行模拟） */
static void cmd_clear(const char* args) { (void)args;
    /* 通过输出大量换行来"清屏" */
    for (int i = 0; i < 30; i++) putchar('\n');
}

/* cmd_tasks - 显示任务列表（内核打印） */
static void cmd_tasks(const char* args) { (void)args;
    tasks_sys();
}

/* cmd_exit - 关闭系统（暂停 CPU） */
static void cmd_exit(const char* args) { (void)args;
    write("Halting system.\n");
    exit(0);
}

/* cmd_time - 显示当前日期时间（RTC） */
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

/* cmd_busy - 用户态忙循环（验证抢占式调度）
 * 不调系统调用（避免内核态 IF=0 屏蔽 IRQ0）；运行中应被 PIT
 * 抢占切到 demo 任务，屏幕出现 demo tick 即抢占生效的证据。 */
static void cmd_busy(const char* args) { (void)args;
    volatile unsigned int x = 0;
    write("busy: computing in user mode...\n");
    for (unsigned int i = 0; i < 30000000; i++) x += i;  /* ~1-2 秒 */
    (void)x;
    write("busy done.\n");
}

/* cmd_uptime - 显示开机时长（秒 → 天/时/分/秒） */
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
    char abs[64];
    resolve_path(fname, abs, sizeof(abs));
    if (write_file(abs, content, (unsigned int)strlen(content)) == 0) {
        write("Saved ");
        write(fname);
        write(" (");
        print_num((int)strlen(content));
        write(" bytes)\n");
    } else {
        write("Save failed (no space).\n");
    }
}

/* cmd_mkdir - 创建子目录 */
static void cmd_mkdir(const char* args) {
    char path[64];
    unsigned int i = 0;
    while (args[i] && args[i] != ' ' && i < sizeof(path) - 1) { path[i] = args[i]; i++; }
    path[i] = '\0';
    if (path[0] == '\0') {
        write("Usage: mkdir <path>\n");
        return;
    }
    char abs[64];
    resolve_path(path, abs, sizeof(abs));
    if (mkdir_sys(abs) == 0) {
        write("Created directory ");
        write(path);
        write("\n");
    } else {
        write("mkdir failed (exists / bad path / no space).\n");
    }
}

/* cmd_pwd - 打印当前工作目录 */
static void cmd_pwd(const char* args) { (void)args;
    write(cwd);
    putchar('\n');
}

/* cmd_cd - 切换目录（支持相对/绝对路径，. 和 ..） */
static void cmd_cd(const char* args) {
    char path[64];
    unsigned int i = 0;
    while (args[i] && args[i] != ' ' && i < sizeof(path) - 1) { path[i] = args[i]; i++; }
    path[i] = '\0';
    if (path[0] == '\0') {
        /* 无参数回根目录 */
        cwd[0] = '/'; cwd[1] = '\0';
        return;
    }

    char abs[64];
    resolve_path(path, abs, sizeof(abs));
    /* 验证目标存在且是目录（列目录成功即目录） */
    struct dirent ents[4];
    int n = list_dir(abs, ents, 4);
    if (n < 0) {
        write("cd: no such directory: ");
        write(path);
        putchar('\n');
        return;
    }
    /* 拷回 cwd（保留尾斜杠与否无所谓，resolve 统一处理） */
    unsigned int k = 0;
    while (abs[k] && k < sizeof(cwd) - 1) { cwd[k] = abs[k]; k++; }
    cwd[k] = '\0';
}

/* cmd_ls - 列出目录内容 */
static void cmd_ls(const char* args) {
    char path[64];
    unsigned int i = 0;
    while (args[i] && args[i] != ' ' && i < sizeof(path) - 1) { path[i] = args[i]; i++; }
    path[i] = '\0';

    char abs[64];
    if (path[0] == '\0') {
        /* 无参数 = 列当前目录 */
        unsigned int k = 0;
        while (cwd[k] && k < sizeof(abs) - 1) { abs[k] = cwd[k]; k++; }
        abs[k] = '\0';
    } else {
        resolve_path(path, abs, sizeof(abs));
    }

    struct dirent ents[64];
    int n = list_dir(abs, ents, 64);
    if (n < 0) {
        write("Directory not found.\n");
        return;
    }

    int shown = 0;
    for (int k = 0; k < n; k++) {
        /* 跳过 '.' 和 '..' */
        if (ents[k].name[0] == 0x2E) continue;
        shown++;
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
    /* 只有 . / .. 的目录 = 空目录 */
    if (shown == 0) {
        write("(empty)\n");
    }
}

/* cmd_mount - 挂载设备：两个空白分隔 token（设备名 + 挂载点） */
static void cmd_mount(const char* args) {
    char dev[16], point[32];
    unsigned int i = 0;
    while (args[i] && args[i] != ' ' && args[i] != '\t' && i < sizeof(dev) - 1) { dev[i] = args[i]; i++; }
    dev[i] = '\0';
    while (args[i] == ' ' || args[i] == '\t') i++;
    unsigned int j = 0;
    while (args[i] && args[i] != ' ' && args[i] != '\t' && j < sizeof(point) - 1) { point[j] = args[i]; i++; j++; }
    point[j] = '\0';

    if (dev[0] == '\0' || point[0] == '\0') {
        write("Usage: mount <device> <mountpoint>\n");
        return;
    }
    if (mount_sys(dev, point) == 0) {
        write("Mounted ");
        write(dev);
        write(" on ");
        write(point);
        write("\n");
    } else {
        write("Mount failed (unknown device / bad filesystem).\n");
    }
}

/* cmd_umount - 卸载挂载点 */
static void cmd_umount(const char* args) {
    char point[32];
    unsigned int i = 0;
    while (args[i] && args[i] != ' ' && args[i] != '\t' && i < sizeof(point) - 1) { point[i] = args[i]; i++; }
    point[i] = '\0';

    if (point[0] == '\0') {
        write("Usage: umount <mountpoint>\n");
        return;
    }
    if (umount_sys(point) == 0) {
        write("Unmounted ");
        write(point);
        write("\n");
    } else {
        write("Unmount failed (not mounted).\n");
    }
}

/* cmd_devices - 打印设备/挂载表 */
static void cmd_devices(const char* args) { (void)args;
    devices_sys();
}

/* cmd_rm - 删除文件 */
static void cmd_rm(const char* args) {
    char fname[32];
    unsigned int i = 0;
    while (args[i] && args[i] != ' ' && i < sizeof(fname) - 1) { fname[i] = args[i]; i++; }
    fname[i] = '\0';

    if (fname[0] == '\0') {
        write("Usage: rm <filename>\n");
        return;
    }
    char abs[64];
    resolve_path(fname, abs, sizeof(abs));
    if (delete_file(abs) == 0) {
        write("Deleted ");
        write(fname);
        write("\n");
    } else {
        write("File not found.\n");
    }
}

/* cmd_cat - 显示文件内容 */
static void cmd_cat(const char* args) {
    char fname[32];
    unsigned int i = 0;
    while (args[i] && args[i] != ' ' && i < sizeof(fname) - 1) { fname[i] = args[i]; i++; }
    fname[i] = '\0';

    if (fname[0] == '\0') {
        write("Usage: cat <filename>\n");
        return;
    }
    char abs[64];
    resolve_path(fname, abs, sizeof(abs));
    char buf[512];
    int n = read_file(abs, buf, sizeof(buf));
    if (n < 0) {
        write("File not found.\n");
        return;
    }
    for (int k = 0; k < n; k++) putchar(buf[k]);
    putchar('\n');
}

/* cmd_mouse - 显示鼠标位置 */
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

/* ---- 设置 TUI ---- */

/* 主题颜色表：名字 + 属性字节（低4位前景色，高4位背景色） */
static const char* color_names[] = { "White", "Cyan", "Green", "Red", "Yellow", "Blue", "Pink" };
static const int   color_values[] = { 0x0F, 0x0B, 0x0A, 0x0C, 0x0E, 0x09, 0x0D };
#define COLOR_COUNT 7

/* 指针图案表：名字 + CP437 字形码 */
static const char* glyph_names[] = { "Block", "Dark", "Mid", "Light", "Right", "Left", "Up", "Down" };
static const int   glyph_values[] = { 0xDB, 0xB2, 0xB1, 0xB0, 0x10, 0x11, 0x1E, 0x1F };
#define GLYPH_COUNT 8

/* print_num - 十进制整数输出 */
static void print_num(int v) {
    char buf[8];
    int idx = 0;
    if (v == 0) { putchar('0'); return; }
    if (v < 0) { putchar('-'); v = -v; }
    while (v > 0) { buf[idx++] = '0' + (v % 10); v /= 10; }
    while (idx > 0) putchar(buf[--idx]);
}

/* tui_pad - 输出 n 个空格，清除值变短时残留的旧字符 */
static void tui_pad(int n) {
    for (int i = 0; i < n; i++) putchar(' ');
}

/* tui_frame - 绘制全屏边框（上下双线 + 左右边线 + 标题） */
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

/* tui_draw - 绘制设置界面（全屏边框，用 ASCII 标签保证 CP437 对齐） */
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

/*
 * fileman_tui - TUI 文件管理器（P1：单栏列表 + 鼠标/键盘导航）
 *
 * 布局：
 *   row 0   顶栏：┌─ HaoOS FM ─ <当前路径> ─┐
 *   row 1..22  文件列表（选中行高亮）
 *   row 23  底栏：↑↓ 选择 Enter 进入 Esc 返回 Q 退出
 *
 * 交互：
 *   键盘：↑↓ 移动选中；Enter 进入目录/无操作；Esc 返回上级；Q 退出
 *   鼠标：点击列表行选中；双击进入目录；点击顶栏/底栏无效
 *   文件：选中 .bin 文件时底栏显示大小；目录显示 [DIR]
 */
static void fileman_tui(void) {
    /* 当前目录（绝对路径，基于 shell 的 cwd） */
    char fm_cwd[64];
    unsigned int ci = 0;
    while (cwd[ci] && ci < sizeof(fm_cwd) - 1) { fm_cwd[ci] = cwd[ci]; ci++; }
    fm_cwd[ci] = '\0';

    /* 目录项缓冲（最多 128 项） */
    struct dirent ents[128];
    int count = 0;
    int sel = 0;             /* 当前选中项索引 */
    int scroll = 0;          /* 列表滚动偏移（列表区 1..22，22 行） */
    int dirty = 1;           /* 需要重绘 */

    /* 鼠标点击检测状态（边沿） */
    int prev_lbtn = 0;
    int press_x = 0, press_y = 0;
    int click_cnt = 0;       /* 双击计数（点不同项自动重置） */

    clear_screen();

    while (1) {
        /* ---- 加载当前目录 ---- */
        if (dirty) {
            int raw_count = list_dir(fm_cwd[0] ? fm_cwd : NULL, ents, 128);
            if (raw_count < 0) raw_count = 0;
            /* 过滤掉 "."（8.3 名 ".          "，点后是空格填充；
             * 保留 ".." 作为返回入口） */
            count = 0;
            for (int i = 0; i < raw_count; i++) {
                if (ents[i].name[0] == 0x2E && (ents[i].name[1] == '\0' || ents[i].name[1] == ' ')) continue;  /* "." */
                if (count < i) ents[count] = ents[i];
                count++;
            }
            if (sel >= count) sel = count > 0 ? count - 1 : 0;
            if (scroll > sel) scroll = sel;
            if (sel >= scroll + 22) scroll = sel - 21;

            /* 顶栏：路径 */
            set_cursor(0, 0);
            putchar(0xDA);
            for (int i = 1; i < 79; i++) putchar(0xC4);
            putchar(0xBF);
            set_cursor(0, 2);
            write("HaoOS FM  [");
            write(fm_cwd);
            write("]");

            /* 列表区：先清 1..22 行 */
            for (int r = 1; r <= 22; r++) {
                set_cursor(r, 0);
                for (int c = 0; c < 80; c++) putchar(' ');
            }

            /* 绘制列表 */
            for (int r = 1; r <= 22; r++) {
                int idx = scroll + (r - 1);
                if (idx >= count) break;
                set_cursor(r, 1);
                if (idx == sel) set_color(0x70);  /* 选中：白底黑字反显 */
                else set_color(0x0F);

                if (ents[idx].attributes & 0x10) {
                    write("[DIR] ");
                } else {
                    write("      ");
                }
                /* 8.3 名 → 可读名 */
                char disp[16];
                int d = 0;
                int main_len = 8;
                while (main_len > 0 && ents[idx].name[main_len - 1] == ' ') main_len--;
                for (int j = 0; j < main_len; j++) disp[d++] = (char)ents[idx].name[j];
                int ext_len = 3;
                while (ext_len > 0 && ents[idx].name[8 + ext_len - 1] == ' ') ext_len--;
                if (ext_len > 0) {
                    disp[d++] = '.';
                    for (int j = 0; j < ext_len; j++) disp[d++] = (char)ents[idx].name[8 + j];
                }
                disp[d] = '\0';
                write(disp);
                /* 文件大小 */
                if (!(ents[idx].attributes & 0x10)) {
                    for (int p = d; p < 20; p++) putchar(' ');
                    print_num((int)ents[idx].file_size);
                    write(" B");
                }
            }
            set_color(0x0F);

            /* 底栏 */
            set_cursor(23, 0);
            putchar(0xC0);
            for (int i = 1; i < 79; i++) putchar(0xC4);
            putchar(0xD9);
            set_cursor(23, 2);
            write("Up/Down select  Enter open  Esc up  Q quit");

            dirty = 0;
        }

        /* ---- 鼠标轮询：点击选中 + 双击进入 ---- */
        int mx, my, mb;
        if (getmouse(&mx, &my, &mb) == 0) {
            int lbtn = mb & 1;
            if (lbtn) {
                if (!prev_lbtn) { press_x = mx; press_y = my; }
            } else if (prev_lbtn) {
                if (mx == press_x && my == press_y && my >= 1 && my <= 22) {
                    int idx = scroll + (my - 1);
                    if (idx < count) {
                        if (idx == sel) {
                            /* 双击同一项 → 进入（两次点击间隔需在超时内） */
                            click_cnt++;
                            if (click_cnt >= 2) {
                                click_cnt = 0;
                                if (ents[idx].attributes & 0x10) {
                                    /* ".." 双击 = 返回上级 */
                                    if (ents[idx].name[0] == 0x2E && ents[idx].name[1] == 0x2E) {
                                        if (fm_cwd[1] != '\0') {
                                            char parent[64];
                                            resolve_path("..", parent, sizeof(parent));
                                            unsigned int m = 0;
                                            while (parent[m] && m < sizeof(fm_cwd) - 1) { fm_cwd[m] = parent[m]; m++; }
                                            fm_cwd[m] = '\0';
                                            sel = 0; scroll = 0; dirty = 1;
                                        }
                                    } else {
                                        /* 进入目录（resolve 规范化） */
                                        char entry_name[16];
                                        unsigned int en = 0;
                                        while (ents[idx].name[en] != ' ' && en < 8 && en < 14) { entry_name[en] = (char)ents[idx].name[en]; en++; }
                                        entry_name[en] = '\0';
                                        char rel[80];
                                        unsigned int rk = 0;
                                        while (fm_cwd[rk] && rk < sizeof(rel) - 1) { rel[rk] = fm_cwd[rk]; rk++; }
                                        if (rel[rk-1] != '/') rel[rk++] = '/';
                                        unsigned int rn = 0;
                                        while (entry_name[rn] && rk < sizeof(rel) - 1) { rel[rk++] = entry_name[rn++]; }
                                        rel[rk] = '\0';
                                        char norm[64];
                                        resolve_path(rel, norm, sizeof(norm));
                                        unsigned int m = 0;
                                        while (norm[m] && m < sizeof(fm_cwd) - 1) { fm_cwd[m] = norm[m]; m++; }
                                        fm_cwd[m] = '\0';
                                        sel = 0; scroll = 0; dirty = 1;
                                    }
                                }
                            }
                        } else {
                            sel = idx;
                            click_cnt = 0;
                            dirty = 1;
                        }
                    }
                }
            }
            prev_lbtn = lbtn;
        }

        /* ---- 键盘 ---- */
        int c = getkey_nb();
        if (c == -1) {
            /* 无事件：让出 CPU */
            yield();
            continue;
        }

        if (c == 0x01) {
            /* Up */
            if (sel > 0) { sel--; dirty = 1; }
        } else if (c == 0x02) {
            /* Down */
            if (sel < count - 1) { sel++; dirty = 1; }
        } else if (c == 0x03) {
            /* Left = 返回上级 */
            if (fm_cwd[1] != '\0') {  /* 非根目录 */
                char parent[64];
                resolve_path("..", parent, sizeof(parent));
                unsigned int k = 0;
                while (parent[k] && k < sizeof(fm_cwd) - 1) { fm_cwd[k] = parent[k]; k++; }
                fm_cwd[k] = '\0';
                sel = 0; scroll = 0; dirty = 1;
            }
        } else if (c == '\n' || c == '\r') {
            /* Enter：进入选中目录（".." = 返回上级） */
            if (sel < count && (ents[sel].attributes & 0x10)) {
                /* ".." 特判：返回上级 */
                if (ents[sel].name[0] == 0x2E && ents[sel].name[1] == 0x2E) {
                    if (fm_cwd[1] != '\0') {
                        char parent[64];
                        resolve_path("..", parent, sizeof(parent));
                        unsigned int k = 0;
                        while (parent[k] && k < sizeof(fm_cwd) - 1) { fm_cwd[k] = parent[k]; k++; }
                        fm_cwd[k] = '\0';
                        sel = 0; scroll = 0; dirty = 1;
                    }
                    continue;
                }
                /* 普通目录：拼相对路径 → resolve 规范化 */
                char entry_name[16];
                unsigned int en = 0;
                while (ents[sel].name[en] != ' ' && en < 8 && en < 14) { entry_name[en] = (char)ents[sel].name[en]; en++; }
                entry_name[en] = '\0';
                /* 先拼 fm_cwd + / + name 成相对路径，再 resolve */
                char rel[80];
                unsigned int rk = 0;
                while (fm_cwd[rk] && rk < sizeof(rel) - 1) { rel[rk] = fm_cwd[rk]; rk++; }
                if (rel[rk-1] != '/') rel[rk++] = '/';
                unsigned int rn = 0;
                while (entry_name[rn] && rk < sizeof(rel) - 1) { rel[rk++] = entry_name[rn++]; }
                rel[rk] = '\0';
                char norm[64];
                resolve_path(rel, norm, sizeof(norm));
                unsigned int m = 0;
                while (norm[m] && m < sizeof(fm_cwd) - 1) { fm_cwd[m] = norm[m]; m++; }
                fm_cwd[m] = '\0';
                sel = 0; scroll = 0; dirty = 1;
            }
        } else if (c == 0x1B) {
            /* Esc = 返回上级 */
            if (fm_cwd[1] != '\0') {
                char parent[64];
                resolve_path("..", parent, sizeof(parent));
                unsigned int k = 0;
                while (parent[k] && k < sizeof(fm_cwd) - 1) { fm_cwd[k] = parent[k]; k++; }
                fm_cwd[k] = '\0';
                sel = 0; scroll = 0; dirty = 1;
            }
        } else if (c == 'q' || c == 'Q') {
            break;
        }
    }

    /* 退出：清屏 + 同步 cwd 到 shell */
    clear_screen();
    unsigned int k = 0;
    while (fm_cwd[k] && k < sizeof(cwd) - 1) { cwd[k] = fm_cwd[k]; k++; }
    cwd[k] = '\0';
    set_cursor(24, 0);
}

/*
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
        if (c == 'w' || c == 'W') {
            sel = (sel + 2) % 3;              /* 上移（循环） */
        } else if (c == 's' || c == 'S') {
            sel = (sel + 1) % 3;              /* 下移 */
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
    /* ⚠️ 修复：退出后光标定位到尾行——shell 提示符从最后一行
     * 开始新的一行，而不是回到顶部与历史输出混在一起。 */
    set_cursor(24, 0);
}

/* cmd_usb - 显示 USB 设备列表与 MSC 状态 */
static void cmd_usb(const char* args) { (void)args;
    __asm__ volatile ("int $0x80" : : "a"(SYS_USB_INFO) : "memory");
}

/* cmd_settings - settings 命令入口 */
static void cmd_settings(const char* args) { (void)args;
    settings_tui();
}

/* cmd_fm - TUI 文件管理器入口 */
static void cmd_fm(const char* args) { (void)args;
    fileman_tui();
}

/* ---- 命令表 ---- */
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
    { "cd",    cmd_cd },
    { "pwd",   cmd_pwd },
    { "mount", cmd_mount },
    { "umount", cmd_umount },
    { "devices", cmd_devices },
    { "time",  cmd_time },
    { "busy",  cmd_busy },
    { "uptime", cmd_uptime },
    { "usb",   cmd_usb },
    { "settings", cmd_settings },
    { "set",   cmd_settings },
    { "fm",    cmd_fm },
    { "exit",  cmd_exit },
    { NULL,    NULL }
};

/* 按空格拆命令名和参数，查命令表执行；未找到报 Unknown command */
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

/* ---- Shell 主循环 ---- */

/* Shell 主循环：提示符 → 读一行 → 执行，循环 */
void main(void) {
    char line[128];

    /* 启动提示 */
    write("HaoOS Shell v0.1\n");
    write("Type 'help' for commands.\n");

    while (1) {
        /* 输出提示符（含当前目录），并锁定编辑区：
         * 文本光标（含鼠标左键放置）只能在提示行及以下移动，
         * 历史输出不可被覆盖。鼠标指针本身不受限制，可自由移动。 */
        write(cwd);
        write(" $ ");
        protect();

        /* 读取用户输入（返回长度未使用，readline 已写入 line） */
        readline(line, sizeof(line));

        /* 执行命令 */
        execute(line);

        /* 让出 CPU：触发一次 shell→demo→shell 切换（验证调度器） */
        yield();
    }
}
