#ifndef GUI_H
#define GUI_H
#include <kaly/wm.h>
#include <stddef.h>
struct gui {
    int fd, width, height, font_width, font_height, escape, escape_wait, has_pending_key;
    uint32_t pending_key;
    uint64_t window;
    uint32_t *pixels;
};
int gui_open(struct gui *, const char *, int, int, int, int);
void gui_close(struct gui *);
int gui_event(struct gui *, struct kwm_event *);
int gui_resize_buffer(struct gui *, int, int);
int gui_present(struct gui *);
int gui_request(struct gui *, struct kwm_request *);
int gui_read_pixels(struct gui *, int, int, int, int, uint32_t *);
int gui_move(struct gui *, int, int);
int gui_resize(struct gui *, int, int);
int gui_title(struct gui *, const char *);
void gui_rect(struct gui *, int, int, int, int, uint32_t);
void gui_line(struct gui *, int, int, int, int, uint32_t);
void gui_text(struct gui *, int, int, const char *, uint32_t);
void gui_glyph(struct gui *, int, int, unsigned, uint32_t);
void gui_button(struct gui *, int, int, int, const char *, int);
void gui_pause(void);
int gui_launch(const char *, const char *);
int gui_reserve_pid(unsigned);
int gui_launch_system(const char *, const char *, unsigned);
#endif
