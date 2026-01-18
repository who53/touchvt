#include "def_font.h"
#include "stb_truetype.h"
#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <libtsm.h>
#include <libudev.h>
#include <linux/fb.h>
#include <linux/vt.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static int fb_fd, pty_master;
static pid_t child_pid;
static uint32_t *fb_mem;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static int fb_w, fb_h, fb_stride;
static int fb_stride_pixels;
static stbtt_fontinfo font;
static float font_scale;
static unsigned char *font_data = NULL;

static struct tsm_screen *tsm_screen;
static struct tsm_vte *tsm_vte;
static int term_cols, term_rows;
static int cell_w, cell_h;
static int term_height;

#define ROWS 5
#define COLS 12
static int kh = 52;
static int current_font_size = 26;
static int kw;
static int kb_y, kb_height;

static int shift_on, ctrl_on, alt_on;
static int pressed_row = -1, pressed_col = -1;
static int last_touch_y = -1;
static int sb_count = 0;

struct key_info {
    const char *label;
    const char *label_shift;
    uint32_t keysym;
    uint32_t keysym_shift;
    int label_w;
    int label_shift_w;
};

static struct key_info keyboard_layout[ROWS][COLS] = {
    {{"Esc", "Esc", 0xff1b, 0xff1b, 0, 0}, {"1", "!", '1', '!', 0, 0}, {"2", "@", '2', '@', 0, 0}, {"3", "#", '3', '#', 0, 0}, {"4", "$", '4', '$', 0, 0}, {"5", "%", '5', '%', 0, 0}, {"6", "^", '6', '^', 0, 0}, {"7", "&", '7', '&', 0, 0}, {"8", "*", '8', '*', 0, 0}, {"9", "(", '9', '(', 0, 0}, {"0", ")", '0', ')', 0, 0}, {"Bksp", "Bksp", 0x007f, 0x007f, 0, 0}},
    {{"Tab", "Tab", 0xff09, 0xff09, 0, 0}, {"q", "Q", 'q', 'Q', 0, 0}, {"w", "W", 'w', 'W', 0, 0}, {"e", "E", 'e', 'E', 0, 0}, {"r", "R", 'r', 'R', 0, 0}, {"t", "T", 't', 'T', 0, 0}, {"y", "Y", 'y', 'Y', 0, 0}, {"u", "U", 'u', 'U', 0, 0}, {"i", "I", 'i', 'I', 0, 0}, {"o", "O", 'o', 'O', 0, 0}, {"p", "P", 'p', 'P', 0, 0}, {"\\", "|", '\\', '|', 0, 0}},
    {{"Ctrl", "Ctrl", 0xffe3, 0xffe3, 0, 0}, {"a", "A", 'a', 'A', 0, 0}, {"s", "S", 's', 'S', 0, 0}, {"d", "D", 'd', 'D', 0, 0}, {"f", "F", 'f', 'F', 0, 0}, {"g", "G", 'g', 'G', 0, 0}, {"h", "H", 'h', 'H', 0, 0}, {"j", "J", 'j', 'J', 0, 0}, {"k", "K", 'k', 'K', 0, 0}, {"l", "L", 'l', 'L', 0, 0}, {";", ":", ';', ':', 0, 0}, {"Ent", "Ent", 0xff0d, 0xff0d, 0, 0}},
    {{"Shft", "Shft", 0xffe1, 0xffe1, 0, 0}, {"z", "Z", 'z', 'Z', 0, 0}, {"x", "X", 'x', 'X', 0, 0}, {"c", "C", 'c', 'C', 0, 0}, {"v", "V", 'v', 'V', 0, 0}, {"b", "B", 'b', 'B', 0, 0}, {"n", "N", 'n', 'N', 0, 0}, {"m", "M", 'm', 'M', 0, 0}, {",", "<", ',', '<', 0, 0}, {".", ">", '.', '>', 0, 0}, {"/", "?", '/', '?', 0, 0}, {"Shft", "Shft", 0xffe2, 0xffe2, 0, 0}},
    {{"Alt", "Alt", 0xffe9, 0xffe9, 0, 0}, {"-", "_", '-', '_', 0, 0}, {"=", "+", '=', '+', 0, 0}, {"[", "{", '[', '{', 0, 0}, {"]", "}", ']', '}', 0, 0}, {"Space", "Space", ' ', ' ', 0, 0}, {"", "", ' ', ' ', 0, 0}, {"'", "\"", '\'', '"', 0, 0}, {"Up", "Up", 0xff52, 0xff52, 0, 0}, {"Dn", "Dn", 0xff54, 0xff54, 0, 0}, {"Lt", "Lt", 0xff51, 0xff51, 0, 0}, {"Rt", "Rt", 0xff53, 0xff53, 0, 0}}
};

static inline uint32_t blend_alpha(uint32_t src, uint32_t dst, unsigned char alpha) {
    uint32_t sr = (src >> 16) & 0xff, sg = (src >> 8) & 0xff, sb = src & 0xff;
    uint32_t dr = (dst >> 16) & 0xff, dg = (dst >> 8) & 0xff, db = dst & 0xff;
    uint32_t r = (sr * alpha + dr * (255 - alpha)) / 255;
    uint32_t g = (sg * alpha + dg * (255 - alpha)) / 255;
    uint32_t b = (sb * alpha + db * (255 - alpha)) / 255;
    return 0xff000000 | (r << 16) | (g << 8) | b;
}

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;

    for (int j = 0; j < h; j++) {
        int screen_y = y + j;
        uint32_t *dst = fb_mem + screen_y * fb_stride_pixels;
        for (int i = 0; i < w; i++) {
            dst[x + i] = color;
        }
    }
}

static void draw_bitmap(int x, int y, const unsigned char *bmp, int bw, int bh, uint32_t fg_color) {
    for (int j = 0; j < bh; j++) {
        int screen_y = y + j;
        if (screen_y < 0 || screen_y >= fb_h) continue;
        
        uint32_t *dst = fb_mem + screen_y * fb_stride_pixels;
        for (int i = 0; i < bw; i++) {
            int dpx = x + i;
            if (dpx < 0 || dpx >= fb_w) continue;
            
            unsigned char alpha = bmp[j * bw + i];
            if (alpha == 0) continue;
            
            if (alpha == 255) {
                dst[dpx] = fg_color;
            } else {
                dst[dpx] = blend_alpha(fg_color, dst[dpx], alpha);
            }
        }
    }
}

static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

static void sigchld_handler(int sig) {
    (void)sig;
    int status;
    if (waitpid(child_pid, &status, WNOHANG) > 0)
        running = 0;
}

static unsigned char *load_font_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    unsigned char *data = malloc(size);
    if (!data || fread(data, 1, size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return data;
}

static void vte_write_cb(struct tsm_vte *vte, const char *u8, size_t len, void *data) {
    (void)vte; (void)data;
    if (write(pty_master, u8, len) < 0) {
        perror("write to pty_master failed");
    }
}

static int text_width(const char *str) {
    int w = 0;
    int advance, lsb;
    for (const char *p = str; *p; p++) {
        stbtt_GetCodepointHMetrics(&font, *p, &advance, &lsb);
        w += (int)(advance * font_scale);
    }
    return w;
}

static void draw_keyboard(void) {
    int ascent, descent, linegap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &linegap);
    int font_height = (int)((ascent - descent) * font_scale);
    int baseline_offset = (font_height / 2) + (int)(ascent * font_scale) - font_height;

    for (int r = 0; r < ROWS; r++) {
        for (int col = 0; col < COLS; col++) {
            int kx = col * kw, ky = kb_y + r * kh;
            int cur_w = kw;
            
            if (r == 4 && col == 5) {
                cur_w = kw * 2;
            } else if (r == 4 && col == 6) {
                continue;
            }

            const struct key_info *ki = &keyboard_layout[r][col];
            uint32_t ksym = shift_on ? ki->keysym_shift : ki->keysym;
            
            int is_pressed = (r == pressed_row && col == pressed_col);
            int is_mod = (ksym == 0xffe1 || ksym == 0xffe2 || ksym == 0xffe3 || ksym == 0xffe9);
            int is_active = (is_mod && ((ksym == 0xffe1 || ksym == 0xffe2) ? shift_on : (ksym == 0xffe3 ? ctrl_on : alt_on)));
            
            uint32_t bg = is_pressed ? 0xff404040 : (is_active ? 0xff303060 : 0xff000000);
            
            fill_rect(kx + 1, ky + 1, cur_w - 2, kh - 2, bg);

            const char *txt = shift_on ? ki->label_shift : ki->label;
            int tw = shift_on ? ki->label_shift_w : ki->label_w;
            int tx = kx + (cur_w - tw) / 2;
            int ty = ky + kh / 2;
            int baseline = ty + baseline_offset;
            uint32_t c = 0xffffffff;

            int x_cursor = tx;
            for (const char *p = txt; *p; p++) {
                int gw, gh, xoff, yoff;
                unsigned char *bmp = stbtt_GetCodepointBitmap(&font, font_scale, font_scale,
                                                              *p, &gw, &gh, &xoff, &yoff);
                if (bmp) {
                    draw_bitmap(x_cursor + xoff, baseline + yoff, bmp, gw, gh, c);
                    stbtt_FreeBitmap(bmp, NULL);
                }
                int advance, lsb;
                stbtt_GetCodepointHMetrics(&font, *p, &advance, &lsb);
                x_cursor += (int)(advance * font_scale);
            }
        }
    }
}

static void draw_glyph(int px, int py, uint32_t ch, uint32_t fg, uint32_t bg, unsigned int width) {
    int total_w = width * cell_w;
    
    fill_rect(px, py, total_w, cell_h, bg);

    if (ch == 0 || ch == ' ')
        return;

    int w, h, xoff, yoff;
    unsigned char *bmp = stbtt_GetCodepointBitmap(&font, font_scale, font_scale,
                                                  ch, &w, &h, &xoff, &yoff);
    if (!bmp) return;

    int ascent, descent, linegap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &linegap);
    int baseline = py + (int)(ascent * font_scale);
    
    draw_bitmap(px + xoff, baseline + yoff, bmp, w, h, fg);
    stbtt_FreeBitmap(bmp, NULL);
}

static int term_draw_cb(struct tsm_screen *con, uint64_t id, const uint32_t *ch,
                        size_t len, unsigned int width, unsigned int posx,
                        unsigned int posy, const struct tsm_screen_attr *attr,
                        tsm_age_t age, void *data) {
    (void)con; (void)id; (void)age; (void)data;
    
    uint32_t fg = 0xff000000 | (attr->fr << 16) | (attr->fg << 8) | attr->fb;
    uint32_t bg = 0xff000000 | (attr->br << 16) | (attr->bg << 8) | attr->bb;

    if (attr->inverse) {
        uint32_t tmp = fg;
        fg = bg;
        bg = tmp;
    }
    
    uint32_t c = (len > 0) ? ch[0] : ' ';
    int px = posx * cell_w;
    int py = posy * cell_h;
    
    draw_glyph(px, py, c, fg, bg, width);

    unsigned int cx = tsm_screen_get_cursor_x(tsm_screen);
    unsigned int cy = tsm_screen_get_cursor_y(tsm_screen);
    if (posx == cx && posy == cy && !sb_count) {
        for (int j = 0; j < cell_h; j++) {
            int screen_y = py + j;
            if (screen_y >= term_height) break;
            if (screen_y < 0) continue;
            uint32_t *dst = fb_mem + screen_y * fb_stride_pixels;
            for (int i = 0; i < cell_w; i++) {
                int tx = px + i;
                if (tx >= 0 && tx < fb_w) {
                    dst[tx] ^= 0x00ffffff;
                }
            }
        }
    }
    return 0;
}

static void draw_terminal(void) {
    tsm_screen_draw(tsm_screen, term_draw_cb, NULL);
}

static void resize_layout(int size) {
    if (size < 8) size = 8;
    if (size > 64) size = 64;
    current_font_size = size;

    font_scale = stbtt_ScaleForPixelHeight(&font, current_font_size);

    int ascent, descent, linegap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &linegap);
    cell_h = (int)((ascent - descent + linegap) * font_scale) + 2;
    
    int advance, lsb;
    stbtt_GetCodepointHMetrics(&font, 'M', &advance, &lsb);
    cell_w = (int)(advance * font_scale);

    kh = (int)(2.6 * current_font_size);
    kw = fb_w / COLS;
    kb_height = ROWS * kh;
    kb_y = fb_h - kb_height;

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            keyboard_layout[r][c].label_w = text_width(keyboard_layout[r][c].label);
            keyboard_layout[r][c].label_shift_w = text_width(keyboard_layout[r][c].label_shift);
        }
    }

    term_height = kb_y;
    term_cols = fb_w / cell_w;
    term_rows = term_height / cell_h;

    if (tsm_screen) {
        tsm_screen_resize(tsm_screen, term_cols, term_rows);
        struct winsize ws = {.ws_row = term_rows,
            .ws_col = term_cols,
            .ws_xpixel = term_cols * cell_w,
            .ws_ypixel = term_rows * cell_h};
        ioctl(pty_master, TIOCSWINSZ, &ws);
        fill_rect(0, 0, fb_w, fb_h, 0xff000000);
        draw_keyboard();
        draw_terminal();
    }
}

static void handle_key(int r, int c, int down) {
    const struct key_info *ki = &keyboard_layout[r][c];
    uint32_t ksym = shift_on ? ki->keysym_shift : ki->keysym;

    if (down && ctrl_on) {
        if (!shift_on && ksym == '-') {
            resize_layout(current_font_size - 2);
            return;
        }
        if (shift_on && ksym == '+') {
            resize_layout(current_font_size + 2);
            return;
        }
    }

    if (ksym == 0xffe1 || ksym == 0xffe2) {
        if (down) shift_on = !shift_on;
        return;
    }
    if (ksym == 0xffe3) {
        if (down) ctrl_on = !ctrl_on;
        return;
    }
    if (ksym == 0xffe9) {
        if (down) alt_on = !alt_on;
        return;
    }
    if (!down) return;

    tsm_screen_sb_reset(tsm_screen);
    sb_count = 0;

    unsigned int mods = 0;
    if (shift_on) mods |= TSM_SHIFT_MASK;
    if (ctrl_on) mods |= TSM_CONTROL_MASK;
    if (alt_on) mods |= TSM_ALT_MASK;
    
    uint32_t unicode = (ksym < 0x100) ? ksym : TSM_VTE_INVALID;
    tsm_vte_handle_keyboard(tsm_vte, ksym, 0, mods, unicode);
}

static int get_key_at(int tx, int ty, int *row, int *col) {
    if (ty < kb_y || ty >= kb_y + ROWS * kh)
        return 0;
    *col = tx / kw;
    *row = (ty - kb_y) / kh;
    if (*col >= COLS) *col = COLS - 1;
    if (*row == 4 && *col == 6) *col = 5;
    return 1;
}

int main(int argc, char **argv) {
    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);
    signal(SIGHUP, SIG_IGN);
    signal(SIGCHLD, sigchld_handler);

    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        perror("touchvt: /dev/fb0");
        return 1;
    }
    ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
    ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo);
    fb_w = vinfo.xres;
    fb_h = vinfo.yres;
    fb_stride = finfo.line_length;
    fb_stride_pixels = fb_stride / 4;
    
    size_t fb_size = fb_h * fb_stride;
    fb_mem = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        perror("touchvt: mmap");
        return 1;
    }
    
    memset(fb_mem, 0, fb_size);

    font_data = font_ttf;
    int cmd_start_index = argc;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--font") == 0) {
            if (i + 1 < argc) {
                unsigned char *loaded = load_font_file(argv[i + 1]);
                if (loaded) {
                    if (font_data != font_ttf && font_data != NULL)
                        free(font_data);
                    font_data = loaded;
                } else {
                    fprintf(stderr, "touchvt: failed to load font %s\n", argv[i + 1]);
                }
                i++;
            }
            continue;
        }
        if (strcmp(argv[i], "--vt") == 0) {
            if (i + 1 < argc) {
                int vt_num = 0;
                if (sscanf(argv[i + 1], "tty%d", &vt_num) == 1 ||
                    sscanf(argv[i + 1], "%d", &vt_num) == 1) {
                    int tty0 = open("/dev/tty0", O_RDWR);
                    if (tty0 >= 0) {
                        ioctl(tty0, VT_ACTIVATE, vt_num);
                        ioctl(tty0, VT_WAITACTIVE, vt_num);
                        close(tty0);
                    }
                }
                i++;
            }
            continue;
        }
        cmd_start_index = i;
        break;
    }

    if (!stbtt_InitFont(&font, font_data, 0)) {
        fprintf(stderr, "touchvt: failed to init font\n");
        return 1;
    }

    if (tsm_screen_new(&tsm_screen, NULL, NULL) < 0) {
        fprintf(stderr, "touchvt: tsm_screen_new failed\n");
        return 1;
    }
    tsm_screen_set_max_sb(tsm_screen, 1000);

    if (tsm_vte_new(&tsm_vte, tsm_screen, vte_write_cb, NULL, NULL, NULL) < 0) {
        fprintf(stderr, "touchvt: tsm_vte_new failed\n");
        return 1;
    }

    resize_layout(20);

    struct winsize ws = {.ws_row = term_rows,
        .ws_col = term_cols,
        .ws_xpixel = term_cols * cell_w,
        .ws_ypixel = term_rows * cell_h};
    child_pid = forkpty(&pty_master, NULL, NULL, &ws);
    if (child_pid < 0) {
        perror("touchvt: forkpty");
        return 1;
    }
    if (child_pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        if (cmd_start_index < argc) {
            execvp(argv[cmd_start_index], &argv[cmd_start_index]);
            perror("touchvt: execvp");
            _exit(127);
        }
        char *shell = getenv("SHELL");
        if (!shell) shell = "/bin/sh";
        execlp(shell, shell, NULL);
        _exit(127);
    }
    fcntl(pty_master, F_SETFL, O_NONBLOCK);

    struct udev *udev = udev_new();
    struct libinput *li = libinput_udev_create_context(
        &(struct libinput_interface){
            .open_restricted = (int (*)(const char *, int, void *))open,
            .close_restricted = (void (*)(int, void *))(void (*)(void))close},
        NULL, udev);
    libinput_udev_assign_seat(li, "seat0");
    int li_fd = libinput_get_fd(li);

    draw_keyboard();
    draw_terminal();

    struct pollfd pfds[2] = {
        {.fd = li_fd, .events = POLLIN},
        {.fd = pty_master, .events = POLLIN}
    };

    while (running) {
        int ret = poll(pfds, 2, -1);
        if (ret < 0 && errno != EINTR)
            break;

        int input_processed = 0;

        if (pfds[1].revents & POLLIN) {
            char buf[4096];
            ssize_t n;
            while ((n = read(pty_master, buf, sizeof(buf))) > 0) {
                tsm_vte_input(tsm_vte, buf, n);
                input_processed = 1;
            }
        }

        if (pfds[0].revents & POLLIN) {
            libinput_dispatch(li);
            struct libinput_event *ev;
            while ((ev = libinput_get_event(li))) {
                enum libinput_event_type t = libinput_event_get_type(ev);
                if (t == LIBINPUT_EVENT_TOUCH_DOWN) {
                    struct libinput_event_touch *te = libinput_event_get_touch_event(ev);
                    int tx = libinput_event_touch_get_x_transformed(te, fb_w);
                    int ty = libinput_event_touch_get_y_transformed(te, fb_h);
                    if (get_key_at(tx, ty, &pressed_row, &pressed_col)) {
                        handle_key(pressed_row, pressed_col, 1);
                        draw_keyboard();
                        last_touch_y = -1;
                    } else {
                        last_touch_y = ty;
                    }
                } else if (t == LIBINPUT_EVENT_TOUCH_MOTION) {
                    struct libinput_event_touch *te = libinput_event_get_touch_event(ev);
                    int ty = libinput_event_touch_get_y_transformed(te, fb_h);
                    if (last_touch_y != -1) {
                        int dy = ty - last_touch_y;
                        if (abs(dy) > cell_h) {
                            if (dy > 0) {
                                int lines = dy / cell_h;
                                tsm_screen_sb_up(tsm_screen, lines);
                                sb_count += lines;
                                if (sb_count > 1000) sb_count = 1000;
                            } else {
                                int lines = -dy / cell_h;
                                tsm_screen_sb_down(tsm_screen, lines);
                                sb_count -= lines;
                                if (sb_count < 0) sb_count = 0;
                            }
                            last_touch_y = ty;
                            input_processed = 1;
                        }
                    }
                } else if (t == LIBINPUT_EVENT_TOUCH_UP) {
                    last_touch_y = -1;
                    if (pressed_row >= 0) {
                        handle_key(pressed_row, pressed_col, 0);
                        pressed_row = -1;
                        pressed_col = -1;
                        draw_keyboard();
                    }
                }
                libinput_event_destroy(ev);
            }
        }

        if (input_processed) {
            draw_terminal();
        }
    }

    kill(child_pid, SIGHUP);
    close(pty_master);
    tsm_vte_unref(tsm_vte);
    tsm_screen_unref(tsm_screen);
    libinput_unref(li);
    udev_unref(udev);
    if (font_data != font_ttf)
        free(font_data);
    munmap(fb_mem, fb_h * fb_stride);
    close(fb_fd);
    return 0;
}
