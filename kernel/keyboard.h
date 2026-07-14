#ifndef KEYBOARD_H
#define KEYBOARD_H
#include <stdint.h>

/* Values above ASCII represent navigation keys. */
#define KEY_UP    ((char)0x80)
#define KEY_DOWN  ((char)0x81)
#define KEY_LEFT  ((char)0x82)
#define KEY_RIGHT ((char)0x83)
#define KEY_PGUP  ((char)0x84)
#define KEY_PGDN  ((char)0x85)
#define KEY_HOME  ((char)0x86)
#define KEY_END   ((char)0x87)
#define KEY_DEL   ((char)0x88)
#define KEY_SCROLL_UP   ((char)0x89)
#define KEY_SCROLL_DOWN ((char)0x8A)
#define KEY_F12         ((char)0x8B)

/* Blocking and non-blocking access to the shared decoder. */
char kbd_getchar(void);

char kbd_poll(void);

void kbd_init(void);

#endif
