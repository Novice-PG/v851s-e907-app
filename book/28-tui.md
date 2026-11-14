# 第 28 章 TUI 终端应用：不打开浏览器也能用

> 本章目标：
> 1. 理解终端不是“一行行打印”，而是一块可以寻址的字符网格。
> 2. 会用 ANSI 转义序列做清屏、定位、颜色、光标控制。
> 3. 理解 raw mode、非规范输入、`select`/`poll` 事件循环。
> 4. 知道什么时候用 curses/ncurses，什么时候自己画。
> 5. 能把 TUI 接到同一套 `@` 命令协议上，并能排查“按键没反应”“画面错乱”。

---

## 28.1 为什么要一个 TUI

网页终端很好，但有些场景不适合浏览器：

```text
- 只有 SSH，没有图形界面
- 想在脚本里跑一次检查
- 网络受限，只要一条 TCP/串口
- 想把板子接到一台老机器上
```

TUI（Text User Interface）就是“在终端里画界面”：

```text
┌─ Linkr Bee ─────────────────────────────┐
│ target: 192.168.0.121   uart: 115200 8N1│
├─────────────────────────────────────────┤
│ # ls                                    │
│ bin  dev  etc  mnt  proc  root  sys     │
│ # _                                     │
├─────────────────────────────────────────┤
│ F1 help  F2 claim  F3 diag  F10 quit    │
└─────────────────────────────────────────┘
```

它和网页用的是同一套数据通道，只是渲染方式不同。

### TUI 与 CLI 的区别

| | CLI | TUI |
|---|---|---|
| 交互 | 一次一个命令 | 持续交互、实时刷新 |
| 输出 | 追加 | 可重绘区域 |
| 状态 | 通常无 | 有界面状态 |
| 适合 | 脚本、一次性操作 | 人工调试、长时间观察 |

> 第 29 章讲 CLI。本章讲“住在终端里的小程序”。

---

## 28.2 终端到底是什么

### 历史与现状

```text
真实串口终端（VT100） → 终端模拟器（xterm、gnome-terminal、iTerm）
                              ↓
                        一块 80×24 的字符网格 + 控制序列
```

现代终端**不是**串口，但它模仿 VT100 的行为。所以“终端控制”
本质上就是“向 stdout 写一些特殊字节”。

### 三个流

```text
stdin    键盘输入
stdout   正常输出（可被重定向）
stderr   错误输出（也常用来画界面）
```

TUI 通常把界面画到 **stdout**，把日志写到 stderr 或文件。
这样 `tui > /dev/null` 时界面消失但日志还在。

### 终端尺寸

```c
#include <sys/ioctl.h>

struct winsize ws;
ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
int cols = ws.ws_col;
int rows = ws.ws_row;
```

或者 shell 里：

```sh
tput cols
tput lines
stty size      # 输出 "24 80"
```

尺寸会在窗口缩放时变。TUI 必须处理 `SIGWINCH`。

---

## 28.3 ANSI 转义序列

### 基本格式

```text
ESC [ 参数 ; 参数 m
```

`ESC` 是 `0x1B`，通常写作 `\x1b` 或 `\033`。

### 最常用的几个

| 序列 | 作用 |
|---|---|
| `\x1b[2J` | 清屏 |
| `\x1b[H` | 光标回左上角 |
| `\x1b[<row>;<col>H` | 光标定位（1 起） |
| `\x1b[K` | 清除光标到行尾 |
| `\x1b[?25l` / `\x1b[?25h` | 隐藏/显示光标 |
| `\x1b[31m` | 前景红 |
| `\x1b[42m` | 背景绿 |
| `\x1b[1m` | 加粗 |
| `\x1b[0m` | 重置所有属性 |
| `\x1b[?1049h` / `\x1b[?1049l` | 进入/退出备用屏幕 |

### 备用屏幕：为什么不弄乱用户的历史

```text
正常屏幕：你敲的命令都在
备用屏幕：TUI 独占，退出后恢复原样
```

```c
fputs("\x1b[?1049h", stdout);   // 进入
/* 画界面 */
fputs("\x1b[?1049l", stdout);   // 退出
```

> 退出时一定要还原，否则用户退出程序后看到一片空白或错位。

### 定位与重绘

```c
void move_to(int row, int col)   /* row/col 从 0 开始，转成 1 起 */
{
    printf("\x1b[%d;%dH", row + 1, col + 1);
}
```

画一行的稳妥做法：

```c
move_to(row, 0);
fputs(text, stdout);
fputs("\x1b[K", stdout);   /* 清掉这一行剩余部分 */
```

如果没有 `\x1b[K`，新内容比旧的短，会留下残字：

```text
旧: "target: 192.168.0.121"
新: "target: 192.168.0.19"
显示: "target: 192.168.0.191"   ← 多出一个 1
```

**这是自绘 TUI 最常见的 bug。**

### 颜色

```c
#define RESET   "\x1b[0m"
#define RED     "\x1b[31m"
#define GREEN   "\x1b[32m"
#define YELLOW  "\x1b[33m"
#define BOLD    "\x1b[1m"
#define DIM     "\x1b[2m"
#define REVERSE "\x1b[7m"

printf(GREEN "connected" RESET);
printf(REVERSE " selected " RESET);
```

256 色：

```c
printf("\x1b[38;5;%dm", color_index);   /* 前景 */
printf("\x1b[48;5;%dm", color_index);   /* 背景 */
```

> 不要假设终端支持真彩色。用 16 色最安全，256 色次之，
> 真彩色只在检测到支持时用。

### 一个完整的“画框”函数

```c
void draw_box(int top, int left, int height, int width)
{
    move_to(top, left);
    fputs("┌", stdout);
    for (int i = 0; i < width - 2; i++) fputs("─", stdout);
    fputs("┐", stdout);

    for (int r = 1; r < height - 1; r++) {
        move_to(top + r, left);
        fputs("│", stdout);
        move_to(top + r, left + width - 1);
        fputs("│", stdout);
    }

    move_to(top + height - 1, left);
    fputs("└", stdout);
    for (int i = 0; i < width - 2; i++) fputs("─", stdout);
    fputs("┘", stdout);
}
```

注意：边框字符是多字节 UTF-8。终端按**显示宽度**算，不是字节数。
如果终端不是 UTF-8，会显示乱码——这是环境问题，不是代码问题。

---

## 28.4 termios 与 raw mode

### 默认的“熟模式”（canonical mode）

默认情况下终端会：

- 等你按回车才把一行交给程序。
- 处理退格、`Ctrl-C`、`Ctrl-Z`。
- 把 `\n` 转成 `\r\n`。

TUI 要“按一个键立刻响应”，就必须关闭这些：

```c
#include <termios.h>
#include <unistd.h>

struct termios orig;

void raw_on(void)
{
    struct termios t;

    tcgetattr(STDIN_FILENO, &orig);
    t = orig;

    t.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    t.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    t.c_oflag &= ~(OPOST);
    t.c_cflag |= CS8;

    t.c_cc[VMIN]  = 1;    /* 至少 1 字节就返回 */
    t.c_cc[VTIME] = 0;    /* 不等 */

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
}

void raw_off(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
}
```

逐项：

| 标志 | 关掉后 |
|---|---|
| `ECHO` | 你按的键不会自动显示（TUI 自己画） |
| `ICANON` | 不等回车，按一个字节就返回 |
| `ISIG` | `Ctrl-C` 不再产生 `SIGINT`，变成普通字节 `0x03` |
| `IEXTEN` | 关掉 `Ctrl-V` 之类的扩展处理 |
| `IXON` | 关掉 `Ctrl-S`/`Ctrl-Q` 流控 |
| `ICRNL` | 回车不再自动变 `\n` |
| `OPOST` | 关掉输出后处理（`\n` 不再自动补 `\r`） |
| `VMIN=1, VTIME=0` | `read()` 至少等 1 字节，不等超时 |

### 关掉 `ISIG` 的后果

`Ctrl-C` 变成普通字节后，程序必须自己处理：

```c
if (ch == 0x03) {          /* Ctrl-C */
    quit_requested = 1;
}
```

否则用户按 `Ctrl-C` 什么都不会发生，只能 `kill`。

> 这是 TUI 的“责任转移”：**终端不再替你处理控制键，你就得自己处理。**

### 为什么退出时必须还原

如果程序崩溃且没还原 `termios`：

```text
- 输入不回显
- 按回车没反应
- 需要 `stty sane` 或 `reset` 才能恢复
```

所以：

```c
#include <signal.h>

static void on_signal(int sig)
{
    raw_off();
    fputs("\x1b[?1049l", stdout);
    _exit(128 + sig);
}

signal(SIGINT, on_signal);
signal(SIGTERM, on_signal);
signal(SIGHUP, on_signal);
```

还要处理 `atexit`：

```c
atexit(raw_off);
```

> “崩溃后终端废了”是新手写 TUI 的必经之坑。用这两个钩子就能避免。

### 为什么用 `_exit` 而不是 `exit`

在信号处理器里调用 `exit` 会跑 `atexit` 和 flush，不是异步安全的。
`_exit` 直接结束进程。

---

## 28.5 事件循环

### 阻塞读的局限

```c
char ch;
while (read(STDIN_FILENO, &ch, 1) == 1) {
    handle(ch);
}
```

这会卡在 `read` 上：

- 没有键盘输入时，无法刷新状态。
- 无法同时处理网络 socket。

### 用 `poll` 同时等键盘和网络

```c
#include <poll.h>

struct pollfd fds[2] = {
    { .fd = STDIN_FILENO, .events = POLLIN },
    { .fd = ws_fd,        .events = POLLIN },
};

for (;;) {
    int n = poll(fds, 2, 250);   /* 最多等 250 ms */

    if (n < 0) {
        if (errno == EINTR) continue;
        break;
    }

    if (n == 0) {
        tick();                  /* 超时：刷新状态、重连、动画 */
        continue;
    }

    if (fds[0].revents & POLLIN) {
        if (read_key() < 0) break;
    }
    if (fds[1].revents & POLLIN) {
        if (read_ws() < 0) reconnect();
    }
}
```

要点：

1. **超时参数**让循环能“定期醒来看一眼”。250 ms 是手感的折中。
2. **`EINTR` 必须重试**，否则信号会打断循环。
3. **`n == 0`** 不是错误，是“超时”。
4. 一次 `poll` 可能两个 fd 都就绪，都要处理。

### 一个最小可用循环

```c
static int running = 1;

while (running) {
    struct pollfd fds[2] = {
        { STDIN_FILENO, POLLIN, 0 },
        { ws_fd, POLLIN, 0 },
    };

    if (poll(fds, 2, 250) < 0) {
        if (errno == EINTR) continue;
        break;
    }

    if (fds[0].revents & POLLIN) {
        unsigned char buf[64];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n > 0) handle_keys(buf, n);
    }

    if (fds[1].revents & (POLLIN | POLLHUP)) {
        handle_ws();
    }

    if (need_redraw) {
        redraw();
        need_redraw = 0;
    }
}
```

> 关键设计：**输入处理只改状态，重绘统一在循环末尾做。**
> 这样一帧内多次按键只画一次，不会闪。

### 转义序列的解析

方向键发的是多个字节：

```text
↑  \x1b[A
↓  \x1b[B
→  \x1b[C
←  \x1b[D
F1 \x1bOP  （或 \x1b[11~，因终端而异）
```

所以 `ESC` 不能立刻当作“退出键”处理，要先看后面有没有 `[`：

```c
enum { KEY_NONE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ESC, KEY_CHAR };

static unsigned char pend[8];
static int pend_len;

int read_key(void)
{
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return KEY_NONE;

    if (pend_len == 0 && c != 0x1b) {
        last_char = c;
        return KEY_CHAR;
    }

    /* 有 ESC，可能是序列的开始 */
    pend[pend_len++] = c;
    if (pend_len < 2) return KEY_NONE;      /* 等更多字节 */

    if (pend[0] == 0x1b && pend[1] == '[') {
        if (pend_len < 3) return KEY_NONE;
        switch (pend[2]) {
        case 'A': pend_len = 0; return KEY_UP;
        case 'B': pend_len = 0; return KEY_DOWN;
        case 'C': pend_len = 0; return KEY_RIGHT;
        case 'D': pend_len = 0; return KEY_LEFT;
        }
    }
    pend_len = 0;
    return KEY_ESC;
}
```

实际实现还需要超时：用户单独按 `ESC` 时，后面不会有字节。
通常设 50 ms 窗口：

```c
/* ESC 后等 50 ms，没后续字节就当作单独的 ESC */
```

> 很多终端程序在 `ESC` 上有半秒延迟，就是这个原因。
> 可以用 `ESC ESC` 或 `Ctrl-[` 作为无歧义的退出键。

### 处理 `SIGWINCH`

```c
static volatile sig_atomic_t resized;

static void on_winch(int sig) { (void)sig; resized = 1; }
...
signal(SIGWINCH, on_winch);
...
if (resized) {
    resized = 0;
    get_size(&rows, &cols);
    clamp_layout();
    need_redraw = 1;
}
```

信号处理里只设标志，实际工作放主循环——这是通用规则。

---

## 28.6 curses / ncurses：什么时候用

### curses 帮你做什么

```text
- 维护字符网格（你只改变化的部分）
- 处理终端能力差异（terminfo）
- 提供窗口、颜色、输入解析
- 优化输出（最小化重绘）
```

### 最小 curses 程序

```c
#include <ncurses.h>

int main(void)
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    int ch;
    while ((ch = getch()) != 'q') {
        clear();
        mvprintw(1, 2, "Linkr Bee TUI  rows=%d cols=%d", LINES, COLS);
        mvprintw(3, 2, "press q to quit");
        refresh();
    }

    endwin();
    return 0;
}
```

编译：

```sh
cc -o tui tui.c -lncurses
```

### curses 的优缺点

| 优点 | 缺点 |
|---|---|
| 不用手写 ANSI | 增加依赖（`libncurses`） |
| 自动处理尺寸/能力 | API 是全局状态，测试难 |
| 有现成的窗口/菜单 | 交叉编译要带 terminfo |
| 重绘优化 | 某些极简环境没有 |

### 这块板适不适合 curses

```text
板子侧：不跑 TUI（它跑 daemon）
主机侧：TUI 跑在你的笔记本上
```

所以依赖问题不大——是你的电脑，不是 64 MB 的板子。

> 结论：**如果是给人在电脑上用的调试工具，用 ncurses 完全合理。**

### 什么时候自己画

| 场景 | 选择 |
|---|---|
| 只画几行状态，不需要窗口 | 自己画 ANSI |
| 要把 TUI 静态编译进极简镜像 | 自己画 |
| 需要复杂布局、滚动、菜单 | ncurses |
| 已经有一套网页 UI，想要一致观感 | 自己画（共享样式常量） |

---

## 28.7 把 TUI 接到协议上

### 三种传输

```text
TUI ──TCP/WS──► daemon ──rpmsg──► E907 ──UART3──► 目标板
TUI ──串口────► 目标板（直连）
TUI ──Unix socket──► daemon（本机）
```

推荐顺序：

```text
1. 先做“直连串口”的 TUI（不依赖网络和 daemon）
2. 再加 WebSocket（复用第 20 章的帧逻辑）
3. 最后加 BLE（如果有需要）
```

### 复用 `@` 命令

TUI 里的按钮应该发同样的文本命令：

```text
@c?              查询所有者
@c claim         申请控制权
@i?              诊断
@u=115200,8,n,1,n  改波特率
@w?              查 Wi-Fi
```

这样：

- 网页、TUI、CLI、手机 App 行为一致。
- 后端只需要实现一次。
- 文档只需要写一份。

### 界面状态机

```text
CONNECTING ──成功──► CONNECTED ──断开──► RECONNECTING
     │                   │                    │
     │                   ├─ 用户按 F10 ──► QUIT
     │                   └─ 收到 @c? 回复 ──► 更新 owner 显示
     └──失败──► ERROR（显示原因，按键重试）
```

不要在 `redraw()` 里做连接操作。**重绘只读状态，不改状态。**

### 串口输出与界面共存

难点：目标板的输出会随时到来，可能带 ANSI 转义。

两种做法：

```text
A. 把目标输出限制在一个“滚动区域”，自己解析 ANSI
B. 只保留最近 N 行纯文本，丢弃转义序列
```

B 简单但丢颜色；A 复杂但保真。

折中：**保留 `\r`/`\n`/`\b`/`\x1b[K` 和常见颜色，其余丢弃**。

```c
void feed_terminal(const unsigned char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = data[i];
        if (c == '\r') { col = 0; }
        else if (c == '\n') { newline(); }
        else if (c == '\b') { if (col) col--; }
        else if (c == 0x1b) { skip_escape(data, &i, len); }
        else if (c >= 0x20) { put_char(c); }
    }
}
```

> 完全正确地解析 ANSI 很难。TUI 里的“迷你终端”只要够用即可，
> 真要完整终端体验，用 xterm.js 或 `tmux`。

---

## 28.8 实验：写一个最小 TUI

### 目标

一个程序，显示连接状态和最近 5 行串口输出，按 `q` 退出。

### 代码骨架

```c
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static struct termios orig;
static volatile sig_atomic_t quit_flag;

static void restore(void)
{
    fputs("\x1b[?25h\x1b[?1049l", stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
}

static void on_sig(int sig)
{
    (void)sig;
    quit_flag = 1;
}

static void raw_on(void)
{
    struct termios t;
    tcgetattr(STDIN_FILENO, &orig);
    t = orig;
    t.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    t.c_iflag &= ~(IXON | ICRNL);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    fputs("\x1b[?1049h\x1b[?25l\x1b[2J", stdout);
}

int main(void)
{
    atexit(restore);
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    raw_on();

    while (!quit_flag) {
        struct pollfd fds[1] = { { STDIN_FILENO, POLLIN, 0 } };

        if (poll(fds, 1, 250) > 0 && (fds[0].revents & POLLIN)) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1 && c == 'q') break;
        }

        fputs("\x1b[H", stdout);
        fputs("\x1b[1;1H\x1b[1mLinkr Bee TUI\x1b[0m", stdout);
        fputs("\x1b[2;1Hstatus: connected", stdout);
        fputs("\x1b[3;1Hq = quit", stdout);
        fflush(stdout);
    }

    return 0;
}
```

编译运行：

```sh
cc -O2 -Wall -o minitui minitui.c
./minitui
```

> 用 `fflush` 很重要：`stdout` 接终端时是行缓冲，但重定位后会变成
> 全缓冲。显式 flush 保证立即显示。

### 扩展实验

1. 加一个“模拟数据源”线程，每 200 ms 追加一行。
2. 显示 `rows`/`cols`，缩窗口看是否更新。
3. 加 `F2` 发送 `@i?` 并显示回复。
4. 故意在 `restore()` 前 `abort()`，看看终端怎么坏、怎么恢复。

---

## 28.9 常见错误清单

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| 按键要回车才生效 | 没关 `ICANON` | termios |
| 按键重复显示两遍 | 没关 `ECHO` | termios |
| `Ctrl-C` 没反应 | 关了 `ISIG` 但没处理 | 自己处理 `0x03` |
| 退出后终端乱 | 没还原 termios/备用屏 | `atexit`+信号 |
| 画面残字 | 没清行尾 | `\x1b[K` |
| 画面闪烁 | 每键都全屏重绘 | 标记脏区、统一重绘 |
| 缩放后错位 | 没处理 `SIGWINCH` | 重新取尺寸 |
| 方向键没反应 | 没解析 `\x1b[A` | 序列解析 |
| 单独 `ESC` 卡顿 | 等后续字节超时太长 | 缩短窗口 |
| 日志打断界面 | 用了 stdout | 日志走文件/stderr |
| UTF-8 边框乱码 | 终端编码不是 UTF-8 | `locale`、`LANG` |
| `poll` 一直返回 | fd 没清 `revents` | 每次重设 |
| 重连后画面不动 | 状态没标记脏 | 改状态时设 `need_redraw` |

---

## 28.10 练习

### 练习 28.1（必做）

写一个程序，用 ANSI 在屏幕中央画一个 40×10 的框，标题写
“Linkr Bee”。要求：

- 处理 `SIGWINCH`，窗口变小时裁掉多余部分。
- 按 `q` 退出并还原终端。

### 练习 28.2（必做）

给 28.8 的最小 TUI 加：

- `Ctrl-C` 退出（因为关了 `ISIG`）。
- `r` 手动触发一次“重连”（只打印一行模拟）。
- 底部状态栏显示“最后按键”。

### 练习 28.3（必做）

回答：

1. 为什么 TUI 要关掉 `ICANON`？
2. 关了 `ISIG` 后 `Ctrl-C` 变成什么？
3. 为什么退出时要还原 `termios`？
4. `poll` 的超时参数有什么用？
5. `\x1b[K` 解决什么问题？
6. 方向键为什么不能只读一个字节？

### 练习 28.4（选做）

用 ncurses 重写 28.8 的界面，比较代码量和行为差异。

### 练习 28.5（选做）

做一个“迷你终端”：把收到的字节写进一个 20 行的滚动区，
支持 `\r`、`\n`、`\b`，其余转义序列丢弃。用一份录制好的
串口日志回放测试。

---

## 28.11 本章验收

1. 能解释终端是字符网格，以及 ANSI 序列的作用。
2. 会用 `termios` 进入 raw mode，并在退出时正确还原。
3. 能写 `poll` 驱动的输入/网络事件循环。
4. 能解析方向键等转义序列，并处理 `ESC` 歧义。
5. 知道 curses 和自己画的取舍。
6. 能把 TUI 接到 `@` 命令协议上，复用同一套后端。

### 自测题

1. `\x1b[2J` 和 `\x1b[H` 分别做什么？
2. 备用屏幕的进入/退出序列是什么？
3. `VMIN=1, VTIME=0` 表示什么？
4. 为什么信号处理里只设标志？
5. `poll` 返回 0 表示什么？
6. 为什么方向键需要多字节解析？

> 下一章把交互压到最小：一个可以放进脚本的 CLI 工具。
