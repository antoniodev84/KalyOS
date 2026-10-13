#include "gfx.h"
#include <kaly/wm.h>
#include <kaly/theme.h>
#define WINDOW_LIMIT 24
#define TITLE_HEIGHT 26
#define DOCK_HEIGHT 38
#define MEMORY_LIMIT (32UL * 1024 * 1024)
struct event_queue {
    struct kwm_event events[KWM_EVENT_CAPACITY];
    unsigned head, count;
    bool overflow;
};
struct window {
    uint64_t id;
    int owner, x, y, width, height;
    int restore_x, restore_y, restore_width, restore_height;
    bool minimized, maximized;
    char title[64];
    uint32_t *pixels;
    struct event_queue queue;
};
struct raw_input {
    int type, x, y, value;
    unsigned buttons;
};
static struct window windows[WINDOW_LIMIT], *order[WINDOW_LIMIT], *tabs[WINDOW_LIMIT], *focus,
    *capture;
static unsigned count;
static struct lock wm_lock, input_lock;
static struct raw_input inputs[512];
static unsigned input_head, input_count;
static bool input_overflow, enabled, ready, dirty;
static struct surface screen;
static int dirty_left, dirty_top, dirty_right, dirty_bottom;
static int mouse_x = 400, mouse_y = 300, drag_x, drag_y, drag_mode;
static unsigned mouse_buttons;
static uint64_t next_id = 1;
static size_t allocated;
static int session_pid;
static struct event_queue session_queue;
static void damage(int x, int y, int w, int h) {
    if (!dirty) {
        dirty_left = x;
        dirty_top = y;
        dirty_right = x + w;
        dirty_bottom = y + h;
        dirty = true;
    } else {
        dirty_left = MIN(dirty_left, x);
        dirty_top = MIN(dirty_top, y);
        dirty_right = MAX(dirty_right, x + w);
        dirty_bottom = MAX(dirty_bottom, y + h);
    }
}
static void damage_window(struct window *w) {
    damage(w->x - 2, w->y - TITLE_HEIGHT - 2,
           w->width + 8, w->height + TITLE_HEIGHT + 8);
}
static void enqueue(struct event_queue *q, struct kwm_event e) {
    if (e.type == KWM_SIZE) {
        for (unsigned i = 0; i < q->count; i++) {
            unsigned slot = (q->head + i) % KWM_EVENT_CAPACITY;
            if (q->events[slot].type == KWM_SIZE) {
                q->events[slot] = e;
                return;
            }
        }
    }
    if (e.type == KWM_MOUSE && q->count) {
        unsigned last = (q->head + q->count - 1) % KWM_EVENT_CAPACITY;
        if (q->events[last].type == KWM_MOUSE && q->events[last].buttons == e.buttons) {
            q->events[last] = e;
            return;
        }
    }
    if (q->count == KWM_EVENT_CAPACITY) {
        q->overflow = true;
        if (e.type != KWM_SIZE && e.type != KWM_CLOSE) {
            return;
        }
        q->head = (q->head + 1) % KWM_EVENT_CAPACITY;
        q->count--;
    }
    q->events[(q->head + q->count++) % KWM_EVENT_CAPACITY] = e;
}
static void activate(struct window *w) {
    if (focus && focus != w) {
        enqueue(&focus->queue, (struct kwm_event){.type = KWM_ACTIVATE, .code = 0});
        damage_window(focus);
    }
    focus = w;
    if (!w) {
        return;
    }
    w->minimized = false;
    for (unsigned i = 0; i < count; i++) {
        if (order[i] == w) {
            memmove(order + i, order + i + 1, (count - i - 1) * sizeof(*order));
            order[count - 1] = w;
            break;
        }
    }
    enqueue(&w->queue, (struct kwm_event){.type = KWM_ACTIVATE, .code = 1});
    damage_window(w);
    damage(0, screen.height - DOCK_HEIGHT, screen.width, DOCK_HEIGHT);
}
static struct window *find(uint64_t id) {
    for (unsigned i = 0; i < count; i++) {
        if (order[i]->id == id) {
            return order[i];
        }
    }
    return NULL;
}
static void remove_window(struct window *w) {
    damage_window(w);
    if (capture == w) {
        capture = NULL;
        drag_mode = 0;
    }
    if (focus == w) {
        focus = NULL;
    }
    for (unsigned i = 0; i < count; i++) {
        if (order[i] == w) {
            memmove(order + i, order + i + 1, (--count - i) * sizeof(*order));
            break;
        }
    }
    for (unsigned i = 0; i <= count; i++) {
        if (tabs[i] == w) {
            memmove(tabs + i, tabs + i + 1, (count - i) * sizeof(*tabs));
            tabs[count] = NULL;
            break;
        }
    }
    allocated -= (size_t)w->width * w->height * 4;
    kfree(w->pixels);
    memset(w, 0, sizeof(*w));
    if (!focus) {
        for (unsigned i = count; i; i--) {
            if (!order[i - 1]->minimized) {
                activate(order[i - 1]);
                break;
            }
        }
    }
    damage(0, screen.height - DOCK_HEIGHT, screen.width, DOCK_HEIGHT);
}
static long resize(struct window *w, int width, int height) {
    if (width < 160 || height < 80 || width > screen.width - 4 ||
        height > screen.height - TITLE_HEIGHT - DOCK_HEIGHT - 4) {
        return -EINVAL;
    }
    size_t old = (size_t)w->width * w->height * 4, size = (size_t)width * height * 4;
    size_t owned = 0;
    for (unsigned i = 0; i < count; i++) {
        if (order[i]->owner == w->owner) {
            owned += (size_t)order[i]->width * order[i]->height * 4;
        }
    }
    if (allocated - old + size > MEMORY_LIMIT || owned - old + size > MEMORY_LIMIT / 4) {
        return -ENOMEM;
    }
    uint32_t *pixels = kalloc(size);
    if (!pixels) {
        return -ENOMEM;
    }
    for (size_t i = 0; i < size / 4; i++) {
        pixels[i] = THEME_WINDOW_BG;
    }
    for (int j = 0; j < MIN(height, w->height); j++) {
        memcpy(pixels + j * width, w->pixels + j * w->width, MIN(width, w->width) * 4UL);
    }
    damage_window(w);
    kfree(w->pixels);
    w->pixels = pixels;
    allocated = allocated - old + size;
    w->width = width;
    w->height = height;
    damage_window(w);
    enqueue(&w->queue, (struct kwm_event){.type = KWM_SIZE, .width = width, .height = height});
    return 0;
}
static void maximize(struct window *w) {
    if (w->maximized) {
        if (!resize(w, w->restore_width, w->restore_height)) {
            w->x = w->restore_x;
            w->y = w->restore_y;
            w->maximized = false;
        }
    } else {
        int x = w->x, y = w->y, width = w->width, height = w->height;
        if (!resize(w, screen.width - 4, screen.height - TITLE_HEIGHT - DOCK_HEIGHT - 4)) {
            w->restore_x = x;
            w->restore_y = y;
            w->restore_width = width;
            w->restore_height = height;
            w->x = 2;
            w->y = TITLE_HEIGHT + 2;
            w->maximized = true;
        }
    }
    damage_window(w);
}
static struct kwm_application applications[KWM_APP_LIMIT];
static unsigned application_count, application_generation;
static char menu_status[64];
static bool menu_open, menu_capture;
static unsigned menu_tab, menu_escape, menu_escape_ticks;
static int menu_offset, menu_selection = -1;
static int menu_width(void) {
    return MIN(360, screen.width - 16);
}
static int menu_height(void) {
    return MIN(420, screen.height - DOCK_HEIGHT - 16);
}
static int menu_y(void) {
    return screen.height - DOCK_HEIGHT - 8 - menu_height();
}
static int menu_rows(void) {
    return MAX(1, (menu_height() - 112) / 28);
}
static unsigned menu_count(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < application_count; i++) {
        if (!!(applications[i].flags & KWM_APP_SYSTEM) == !menu_tab) {
            n++;
        }
    }
    return n;
}
static struct kwm_application *menu_entry(int index) {
    for (unsigned i = 0; i < application_count; i++) {
        if (!!(applications[i].flags & KWM_APP_SYSTEM) == !menu_tab && !index--) {
            return &applications[i];
        }
    }
    return NULL;
}
static void menu_damage(void) {
    damage(6, menu_y() - 2, menu_width() + 6, menu_height() + 6);
    damage(0, screen.height - DOCK_HEIGHT, 108, DOCK_HEIGHT);
}
static void menu_close(void) {
    menu_damage();
    menu_open = false;
    menu_escape = menu_escape_ticks = 0;
}
static void menu_bounds(void) {
    int n = menu_count(), rows = menu_rows();
    menu_offset = MAX(0, MIN(menu_offset, MAX(0, n - rows)));
    menu_selection = n ? MAX(-1, MIN(menu_selection, n - 1)) : -1;
    if (menu_selection >= 0 &&
        (menu_selection < menu_offset || menu_selection >= menu_offset + rows)) {
        menu_selection = menu_offset;
    }
}
static void menu_launch(void) {
    struct kwm_application *app = menu_entry(menu_selection);
    if (app && session_pid) {
        enqueue(&session_queue, (struct kwm_event){.type = KWM_LAUNCH,
                                                   .code = app->id,
                                                   .reserved = application_generation});
        menu_close();
    }
}
static void menu_key(unsigned key) {
    if (key == 27) {
        menu_escape = 1;
        menu_escape_ticks = 3;
        return;
    }
    if (menu_escape == 1) {
        if (key == '[') {
            menu_escape = 2;
            menu_escape_ticks = 3;
            return;
        }
        menu_close();
        return;
    }
    if (menu_escape == 2) {
        key = key == 'A'   ? KWM_UP
              : key == 'B' ? KWM_DOWN
              : key == 'C' ? KWM_RIGHT
              : key == 'D' ? KWM_LEFT
                           : key;
        menu_escape = menu_escape_ticks = 0;
    }
    if (key == '\t' || key == KWM_LEFT || key == KWM_RIGHT) {
        menu_tab ^= 1;
        menu_offset = 0;
        menu_selection = menu_count() ? 0 : -1;
    } else if (key == KWM_UP || key == KWM_DOWN) {
        int n = menu_count();
        if (n) {
            menu_selection =
                key == KWM_DOWN ? MIN(menu_selection + 1, n - 1) : MAX(menu_selection - 1, 0);
            if (menu_selection < menu_offset) {
                menu_offset = menu_selection;
            } else if (menu_selection >= menu_offset + menu_rows()) {
                menu_offset = menu_selection - menu_rows() + 1;
            }
        }
    } else if (key == '\n') {
        menu_launch();
    }
    menu_damage();
}
static void menu_pointer(bool down) {
    int x = mouse_x - 8, y = mouse_y - menu_y();
    int width = menu_width(), height = menu_height();
    if (down) {
        menu_capture = true;
    }
    if (x < 0 || x >= width || y < 0 || y >= height) {
        if (down) {
            menu_close();
        }
        return;
    }
    if (down && y < 32 && x >= width - 28) {
        menu_close();
    } else if (down && y >= 36 && y < 66) {
        menu_tab = x >= width / 2;
        menu_offset = 0;
        menu_selection = menu_count() ? 0 : -1;
        menu_damage();
    } else if (y >= 76 && y < 76 + menu_rows() * 28) {
        int selected = menu_offset + (y - 76) / 28;
        if (selected >= (int)menu_count()) {
            selected = -1;
        }
        if (selected != menu_selection) {
            menu_selection = selected;
            menu_damage();
        }
        if (down) {
            menu_launch();
        }
    } else if (down && y >= height - 32) {
        if (x < width / 3) {
            menu_offset -= menu_rows();
        } else if (x >= width * 2 / 3) {
            menu_offset += menu_rows();
        } else if (session_pid) {
            enqueue(&session_queue, (struct kwm_event){.type = KWM_MENU_REFRESH});
        }
        menu_bounds();
        menu_damage();
    }
}
static void menu_render(void) {
    int x = 8, y = menu_y(), width = menu_width(), height = menu_height();
    gfx_rect(&screen, x + 4, y + 4, width, height, THEME_MENU_SHADOW);
    gfx_rect(&screen, x - 1, y - 1, width + 2, height + 2, THEME_MENU_BORDER);
    gfx_rect(&screen, x, y, width, height, THEME_MENU_BG);
    gfx_rect(&screen, x, y, width, 32, THEME_MENU_HEADER);
    gfx_text(&screen, x + 12, y + 9, "Menu", THEME_MENU_TITLE);
    int title_clip = screen.right;
    screen.right = MIN(screen.right, x + width - 32);
    gfx_text(&screen, x + 96, y + 9, menu_status, THEME_MENU_STATUS);
    screen.right = title_clip;
    gfx_text(&screen, x + width - 21, y + 9, "x", THEME_MENU_CLOSE);
    int half = width / 2;
    gfx_rect(&screen, x + 6, y + 36, half - 8, 30, menu_tab ? THEME_MENU_TAB_IDLE : THEME_MENU_TAB_ACTIVE);
    gfx_rect(&screen, x + half + 2, y + 36, half - 8, 30, menu_tab ? THEME_MENU_TAB_ACTIVE : THEME_MENU_TAB_IDLE);
    gfx_text(&screen, x + 16, y + 43, "System", THEME_MENU_TAB_TEXT);
    gfx_text(&screen, x + half + 12, y + 43, "Apps", THEME_MENU_TAB_TEXT);
    int clip = screen.right;
    screen.right = MIN(clip, x + width - 12);
    for (int i = 0; i < menu_rows(); i++) {
        struct kwm_application *app = menu_entry(menu_offset + i);
        if (!app) {
            break;
        }
        int top = y + 76 + i * 28;
        if (menu_selection == menu_offset + i) {
            gfx_rect(&screen, x + 6, top, width - 12, 27, THEME_MENU_SELECTION);
        }
        gfx_text(&screen, x + 16, top + 6, app->label, THEME_MENU_ITEM_TEXT);
    }
    if (!menu_count()) {
        gfx_text(&screen, x + 16, y + 82, "None app", THEME_MENU_EMPTY_TEXT);
    }
    screen.right = clip;
    int footer = y + height - 32;
    gfx_rect(&screen, x + 6, footer, width / 3 - 8, 26, THEME_MENU_FOOTER_BTN);
    gfx_rect(&screen, x + width / 3 + 2, footer, width / 3 - 4, 26, THEME_MENU_FOOTER_BTN);
    gfx_rect(&screen, x + width * 2 / 3 + 2, footer, width / 3 - 8, 26, THEME_MENU_FOOTER_BTN);
    gfx_text(&screen, x + 12, footer + 5, "< Back", THEME_MENU_FOOTER_TEXT);
    gfx_text(&screen, x + width / 3 + 8, footer + 5, "Update", THEME_MENU_FOOTER_TEXT);
    gfx_text(&screen, x + width * 2 / 3 + 8, footer + 5, "More >", THEME_MENU_FOOTER_TEXT);
}
static void render(void) {
    if (!dirty) {
        return;
    }
    int x = MAX(0, dirty_left), y = MAX(0, dirty_top);
    int right = MIN(screen.width, dirty_right), bottom = MIN(screen.height, dirty_bottom);
    if (right <= x || bottom <= y) {
        dirty = false;
        return;
    }
    gfx_clip(&screen, x, y, right - x, bottom - y);
    for (int j = y; j < bottom; j++) {
        unsigned shade = 25 + (unsigned)j * 20 / screen.height;
        gfx_rect(&screen, x, j, right - x, 1, (shade << 16) | ((shade + 14) << 8) | (shade + 27));
    }
    for (unsigned i = 0; i < count; i++) {
        struct window *w = order[i];
        if (w->minimized) {
            continue;
        }
        gfx_rect(&screen, w->x + 4, w->y - TITLE_HEIGHT + 4, w->width + 2,
                 w->height + TITLE_HEIGHT + 2, THEME_WINDOW_SHADOW);
        gfx_rect(&screen, w->x - 2, w->y - TITLE_HEIGHT - 2, w->width + 4,
                 w->height + TITLE_HEIGHT + 4, focus == w ? THEME_BORDER_FOCUS : THEME_BORDER_IDLE);
        gfx_rect(&screen, w->x, w->y - TITLE_HEIGHT, w->width, TITLE_HEIGHT,
                 focus == w ? THEME_TITLE_FOCUS : THEME_TITLE_IDLE);
        int title_right = screen.right;
        screen.right = MIN(screen.right, w->x + w->width - 76);
        gfx_text(&screen, w->x + 9, w->y - TITLE_HEIGHT + 5, w->title, THEME_TITLE_TEXT);
        screen.right = title_right;
        gfx_text(&screen, w->x + w->width - 69, w->y - 21, "_", THEME_BTN_MIN);
        gfx_text(&screen, w->x + w->width - 45, w->y - 21, "+", THEME_BTN_MAX);
        gfx_text(&screen, w->x + w->width - 21, w->y - 21, "x", THEME_BTN_CLOSE);
        gfx_blit(&screen, w->x, w->y, w->pixels, w->width, w->height);
        for (int n = 3; n < 12; n += 3) {
            gfx_line(&screen, w->x + w->width - n, w->y + w->height - 2, w->x + w->width - 2,
                     w->y + w->height - n, THEME_RESIZE_GRIP);
        }
    }
    int dock_y = screen.height - DOCK_HEIGHT;
    gfx_rect(&screen, 0, dock_y, screen.width, DOCK_HEIGHT, THEME_DOCK_BG);
    gfx_rect(&screen, 8, dock_y + 5, 92, 28, menu_open ? THEME_DOCK_BTN_ACTIVE : THEME_DOCK_BTN_IDLE);
    gfx_text(&screen, 20, dock_y + 11, "Menu", THEME_DOCK_BTN_TEXT);
    for (unsigned i = 0; i < count; i++) {
        int tx = 112 + (int)i * 112;
        if (tx + 108 > screen.width) {
            break;
        }
        struct window *w = tabs[i];
        gfx_rect(&screen, tx, dock_y + 5, 108, 28, w == focus ? THEME_DOCK_TAB_ACTIVE : THEME_DOCK_TAB_IDLE);
        int clip = screen.right;
        screen.right = MIN(clip, tx + 104);
        gfx_text(&screen, tx + 5, dock_y + 11, w->title, w->minimized ? THEME_DOCK_TAB_TEXT_MIN : THEME_DOCK_TAB_TEXT);
        screen.right = clip;
    }
    if (menu_open) {
        menu_render();
    }
    for (int j = 0; j < 16; j++) {
        for (int i = 0; i <= j / 2; i++) {
            gfx_rect(&screen, mouse_x + i, mouse_y + j, 1, 1,
                     i == 0 || i == j / 2 || j == 15 ? THEME_CURSOR_OUTLINE : THEME_CURSOR_FILL);
        }
    }
    framebuffer_present(screen.pixels, x, y, right - x, bottom - y);
    dirty = false;
}
static void pointer(struct raw_input e) {
    damage(mouse_x, mouse_y, 10, 17);
    mouse_x = e.x;
    mouse_y = e.y;
    damage(mouse_x, mouse_y, 10, 17);
    bool down = (e.buttons & 1) && !(mouse_buttons & 1);
    bool up = !(e.buttons & 1) && (mouse_buttons & 1);
    mouse_buttons = e.buttons;
    if (capture && drag_mode) {
        if (drag_mode == 1 && !capture->maximized) {
            damage_window(capture);
            capture->x = MAX(0, MIN(mouse_x - drag_x, screen.width - capture->width));
            capture->y =
                MAX(TITLE_HEIGHT + 2, MIN(mouse_y - drag_y, screen.height - DOCK_HEIGHT - 20));
            damage_window(capture);
        } else if (drag_mode == 2 && up) {
            resize(capture, MAX(160, MIN(mouse_x - capture->x + 1, screen.width - 4)),
                   MAX(80, MIN(mouse_y - capture->y + 1,
                               screen.height - TITLE_HEIGHT - DOCK_HEIGHT - 4)));
        }
        if (up) {
            capture = NULL;
            drag_mode = 0;
        }
        return;
    }
    if (menu_capture) {
        if (up) {
            menu_capture = false;
        }
        return;
    }
    if (down && mouse_y >= screen.height - DOCK_HEIGHT && mouse_x >= 8 && mouse_x < 100) {
        menu_capture = true;
        if (menu_open) {
            menu_close();
        } else {
            menu_open = true;
            menu_escape = menu_escape_ticks = 0;
            menu_damage();
            if (session_pid) {
                enqueue(&session_queue, (struct kwm_event){.type = KWM_MENU_REFRESH});
            }
        }
        return;
    }
    if (menu_open) {
        menu_pointer(down);
        return;
    }
    if (down && mouse_y >= screen.height - DOCK_HEIGHT) {
        if (mouse_x >= 112) {
            unsigned i = (mouse_x - 112) / 112;
            if (i < count) {
                activate(tabs[i]);
            }
        }
        return;
    }
    struct window *w = capture;
    if (!w) {
        for (unsigned i = count; i; i--) {
            struct window *candidate = order[i - 1];
            if (!candidate->minimized && mouse_x >= candidate->x &&
                mouse_x < candidate->x + candidate->width &&
                mouse_y >= candidate->y - TITLE_HEIGHT &&
                mouse_y < candidate->y + candidate->height) {
                w = candidate;
                break;
            }
        }
    }
    if (down) {
        activate(w);
        if (w && mouse_y < w->y) {
            int distance = w->x + w->width - mouse_x;
            if (distance <= 24) {
                enqueue(&w->queue, (struct kwm_event){.type = KWM_CLOSE});
            } else if (distance <= 48) {
                maximize(w);
            } else if (distance <= 72) {
                damage_window(w);
                w->minimized = true;
                activate(NULL);
            } else {
                capture = w;
                drag_mode = 1;
                drag_x = mouse_x - w->x;
                drag_y = mouse_y - w->y;
            }
            return;
        }
        if (w && mouse_x >= w->x + w->width - 14 && mouse_y >= w->y + w->height - 14 &&
            !w->maximized) {
            capture = w;
            drag_mode = 2;
            return;
        }
        capture = w;
    }
    if (w) {
        enqueue(&w->queue, (struct kwm_event){.type = KWM_MOUSE,
                                              .x = mouse_x - w->x,
                                              .y = mouse_y - w->y,
                                              .buttons = e.buttons});
    }
    if (up) {
        capture = NULL;
    }
}
static void input_append(struct raw_input e) {
    lock(&input_lock);
    if (input_count < 512) {
        inputs[(input_head + input_count++) % 512] = e;
    } else {
        input_overflow = true;
    }
    unlock(&input_lock);
}
bool wm_active(void) {
    return enabled;
}
void wm_key(uint8_t key) {
    if (enabled) {
        input_append((struct raw_input){.type = KWM_KEY, .value = key});
    }
}
void wm_pointer(int x, int y, unsigned buttons) {
    if (enabled) {
        input_append((struct raw_input){.type = KWM_MOUSE, .x = x, .y = y, .buttons = buttons});
    }
}
void wm_wheel(int value) {
    if (enabled) {
        input_append((struct raw_input){.type = KWM_WHEEL, .value = value});
    }
}
void wm_poll(void) {
    if (!enabled) {
        return;
    }
    keyboard_poll();
    if (__atomic_exchange_n(&wm_lock.value, 1, __ATOMIC_ACQUIRE)) {
        return;
    }
    for (unsigned i = 0; i < 512; i++) {
        lock(&input_lock);
        if (!input_count) {
            if (input_overflow) {
                if (focus) {
                    enqueue(&focus->queue, (struct kwm_event){.type = KWM_OVERFLOW});
                }
                capture = NULL;
                drag_mode = 0;
                menu_capture = false;
                if (menu_open) {
                    menu_close();
                }
                input_overflow = false;
            }
            unlock(&input_lock);
            break;
        }
        struct raw_input e = inputs[input_head];
        input_head = (input_head + 1) % 512;
        input_count--;
        unlock(&input_lock);
        if (e.type == KWM_MOUSE) {
            pointer(e);
        } else if (menu_open) {
            if (e.type == KWM_KEY) {
                menu_key(e.value);
            } else if (e.type == KWM_WHEEL) {
                menu_offset -= e.value * 3;
                menu_bounds();
                menu_damage();
            }
        } else if (focus) {
            enqueue(&focus->queue, (struct kwm_event){.type = e.type,
                                                      .code = e.value,
                                                      .x = mouse_x - focus->x,
                                                      .y = mouse_y - focus->y,
                                                      .buttons = mouse_buttons});
        }
    }
    if (menu_open && menu_escape_ticks && !--menu_escape_ticks) {
        menu_close();
    }
    render();
    unlock(&wm_lock);
}
void wm_init(void) {
    if (!framebuffer_available() || framebuffer_width() > 1920 || framebuffer_height() > 1200 ||
        framebuffer_width() < 320 || framebuffer_height() < 240) {
        return;
    }
    screen.width = framebuffer_width();
    screen.height = framebuffer_height();
    screen.pixels = kalloc((size_t)screen.width * screen.height * 4);
    if (!screen.pixels) {
        kprintf("WM: insufficient memory\n");
        return;
    }
    ready = true;
    kprintf("WM: kernel compositor %ux%u IPC /dev/wm\n", (uint64_t)screen.width,
            (uint64_t)screen.height);
}
void wm_process_exit(int pid) {
    if (!enabled) {
        return;
    }
    lock(&wm_lock);
    for (unsigned i = 0; i < count;) {
        if (order[i]->owner == pid) {
            remove_window(order[i]);
            i = 0;
        } else {
            i++;
        }
    }
    if (session_pid == pid) {
        session_pid = 0;
        application_count = application_generation = 0;
        if (menu_open) {
            menu_close();
        }
        memset(&session_queue, 0, sizeof(session_queue));
    }
    unlock(&wm_lock);
}
static long event_read(struct event_queue *q, uint64_t address, int width, int height) {
    if (user_range(address, sizeof(struct kwm_event), true)) {
        return -EFAULT;
    }
    struct kwm_event e;
    if (q->overflow) {
        e = (struct kwm_event){.type = KWM_OVERFLOW, .width = width, .height = height};
    } else if (!q->count) {
        return -EAGAIN;
    } else {
        e = q->events[q->head];
    }
    if (copy_to_user(address, &e, sizeof(e))) {
        return -EFAULT;
    }
    if (q->overflow) {
        q->overflow = false;
    } else {
        q->head = (q->head + 1) % KWM_EVENT_CAPACITY;
        q->count--;
    }
    return 0;
}
static long command(struct kwm_request *r, int pid) {
    if (r->operation == KWM_INFO) {
        r->width = screen.width;
        r->height = screen.height;
        r->x = framebuffer_font_width();
        r->y = framebuffer_font_height();
        return 0;
    }
    if (r->operation == KWM_SESSION) {
        if (this_cpu()->current->uid || (session_pid && session_pid != pid)) {
            return -EPERM;
        }
        session_pid = pid;
        if (!enabled) {
            framebuffer_graphics(true);
            enabled = true;
            damage(0, 0, screen.width, screen.height);
            render();
        }
        return 0;
    }
    if (r->operation == KWM_APPLICATIONS) {
        if (session_pid != pid) {
            return -EPERM;
        }
        if (r->length % sizeof(struct kwm_application) ||
            r->length > KWM_APP_LIMIT * sizeof(struct kwm_application) || !r->argument ||
            r->argument > 0xffffffff || r->title[63]) {
            return -EINVAL;
        }
        unsigned n = r->length / sizeof(struct kwm_application);
        struct kwm_application *entries = kalloc(MAX(r->length, 1));
        if (!entries) {
            return -ENOMEM;
        }
        if (copy_from_user(entries, r->data, r->length)) {
            kfree(entries);
            return -EFAULT;
        }
        for (unsigned i = 0; i < n; i++) {
            if (entries[i].flags & ~KWM_APP_SYSTEM || !entries[i].label[0] ||
                entries[i].label[63]) {
                kfree(entries);
                return -EINVAL;
            }
            for (unsigned j = 0; j < i; j++) {
                if (entries[j].id == entries[i].id) {
                    kfree(entries);
                    return -EINVAL;
                }
            }
        }
        memcpy(applications, entries, r->length);
        application_count = n;
        application_generation = r->argument;
        memcpy(menu_status, r->title, sizeof(menu_status));
        kfree(entries);
        menu_bounds();
        if (menu_open) {
            menu_damage();
        }
        return 0;
    }
    if (r->operation == KWM_EVENT && !r->window) {
        return session_pid == pid ? event_read(&session_queue, r->data, 0, 0) : -EPERM;
    }
    if (r->operation == KWM_CREATE) {
        if (!enabled) {
            return -EAGAIN;
        }
        unsigned owned = 0;
        struct window *w = NULL;
        for (unsigned i = 0; i < WINDOW_LIMIT; i++) {
            if (windows[i].id && windows[i].owner == pid) {
                owned++;
            } else if (!windows[i].id) {
                w = &windows[i];
            }
        }
        if (!w || owned >= 8 || !next_id) {
            return -ENOMEM;
        }
        w->owner = pid;
        long e = resize(w, r->width, r->height);
        if (e) {
            memset(w, 0, sizeof(*w));
            return e;
        }
        w->id = next_id++;
        w->x = MAX(2, MIN(r->x, screen.width - w->width - 2));
        w->y = MAX(TITLE_HEIGHT + 2, MIN(r->y, screen.height - DOCK_HEIGHT - w->height - 2));
        memcpy(w->title, r->title, 63);
        w->title[63] = 0;
        order[count] = w;
        tabs[count++] = w;
        r->window = w->id;
        activate(w);
        return 0;
    }
    struct window *w = find(r->window);
    if (!w) {
        return -ENOENT;
    }
    if (w->owner != pid) {
        return -EPERM;
    }
    struct surface s = {.pixels = w->pixels, .width = w->width, .height = w->height};
    gfx_clip(&s, 0, 0, w->width, w->height);
    switch (r->operation) {
    case KWM_DESTROY:
        remove_window(w);
        return 0;
    case KWM_EVENT:
        return event_read(&w->queue, r->data, w->width, w->height);
    case KWM_MOVE:
        damage_window(w);
        w->x = MAX(2, MIN(r->x, screen.width - w->width - 2));
        w->y = MAX(TITLE_HEIGHT + 2, MIN(r->y, screen.height - DOCK_HEIGHT - 20));
        w->maximized = false;
        damage_window(w);
        return 0;
    case KWM_RESIZE: {
        long result = resize(w, r->width, r->height);
        if (!result) {
            w->maximized = false;
        }
        return result;
    }
    case KWM_FOCUS:
        activate(w);
        return 0;
    case KWM_TITLE:
        memcpy(w->title, r->title, 63);
        w->title[63] = 0;
        damage_window(w);
        return 0;
    case KWM_LINE:
        if (r->x < -4096 || r->y < -4096 || r->x > 4096 || r->y > 4096 || r->width < -4096 ||
            r->height < -4096 || r->width > 4096 || r->height > 4096) {
            return -EINVAL;
        }
        gfx_line(&s, r->x, r->y, r->width, r->height, r->color);
        break;
    case KWM_FILL:
        if (r->x < -4096 || r->y < -4096 || r->x > 4096 || r->y > 4096 || r->width < 0 ||
            r->height < 0 || r->width > 4096 || r->height > 4096) {
            return -EINVAL;
        }
        gfx_rect(&s, r->x, r->y, r->width, r->height, r->color);
        break;
    case KWM_TEXT: {
        if (r->length > 4096 || r->x < -4096 || r->x > 4096 || r->y < -4096 || r->y > 4096) {
            return -EINVAL;
        }
        char *text = kalloc(r->length + 1);
        if (!text) {
            return -ENOMEM;
        }
        long e = copy_from_user(text, r->data, r->length);
        if (!e) {
            text[r->length] = 0;
            gfx_text(&s, r->x, r->y, text, r->color);
        }
        kfree(text);
        if (e) {
            return -EFAULT;
        }
        break;
    }
    case KWM_BLIT:
    case KWM_READ: {
        if (r->x < 0 || r->y < 0 || r->width <= 0 || r->height <= 0 || r->width > w->width ||
            r->height > w->height || r->x > w->width - r->width || r->y > w->height - r->height ||
            r->argument < (uint64_t)r->width || r->argument > 4096) {
            return -EINVAL;
        }
        size_t bytes = ((uint64_t)(r->height - 1) * r->argument + r->width) * 4;
        if (r->length < bytes || user_range(r->data, bytes, r->operation == KWM_READ)) {
            return -EFAULT;
        }
        uint32_t *staging = kalloc((size_t)r->width * r->height * 4);
        if (!staging) {
            return -ENOMEM;
        }
        long e = 0;
        for (int j = 0; j < r->height; j++) {
            if (r->operation == KWM_BLIT) {
                e = copy_from_user(staging + j * r->width, r->data + j * r->argument * 4,
                                   r->width * 4UL);
            } else {
                memcpy(staging + j * r->width, w->pixels + (r->y + j) * w->width + r->x,
                       r->width * 4UL);
                e = copy_to_user(r->data + j * r->argument * 4, staging + j * r->width,
                                 r->width * 4UL);
            }
            if (e) {
                break;
            }
        }
        if (!e && r->operation == KWM_BLIT) {
            gfx_blit(&s, r->x, r->y, staging, r->width, r->height);
        }
        kfree(staging);
        if (e) {
            return -EFAULT;
        }
        if (r->operation == KWM_READ) {
            return 0;
        }
        break;
    }
    default:
        return -EINVAL;
    }
    damage_window(w);
    return 0;
}
long wm_ioctl(unsigned long request, uint64_t address) {
    if (request != KWM_IOCTL) {
        return -25;
    }
    if (!ready) {
        return -19;
    }
    struct kwm_request r;
    if (user_range(address, sizeof(r), true) || copy_from_user(&r, address, sizeof(r))) {
        return -EFAULT;
    }
    if (r.version != KWM_VERSION) {
        return -EINVAL;
    }
    lock(&wm_lock);
    long e = command(&r, this_cpu()->current->pid);
    if (!e && copy_to_user(address, &r, sizeof(r))) {
        e = -EFAULT;
    }
    unlock(&wm_lock);
    return e;
}
