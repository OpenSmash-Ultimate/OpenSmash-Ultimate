/* The settings menu, drawn over the game (gpu_menu.c): Esc, the pad's Home / PS / Xbox button,
 * or Minus and Plus held together for a second open and close it. The game keeps running; while
 * the menu is open it sees every pad and the keyboard pressing nothing. */
#pragma once

int menu_is_open(void);
int menu_blocks_input(void);         /* open, or just closed and its keys or buttons still held */
int menu_visible(void);              /* open, or a message showing */

/* The window's thread (Win32 window procedure, Linux's SDL loop): keys while the menu is open
 * (Esc also while it is closed), and typed characters (printable ASCII). */
enum { MENU_KEY_ESC = 1, MENU_KEY_UP, MENU_KEY_DOWN, MENU_KEY_LEFT, MENU_KEY_RIGHT, MENU_KEY_ENTER, MENU_KEY_BACKSPACE,
       MENU_KEY_TAB, MENU_KEY_SHIFT_TAB, MENU_KEY_DELETE, MENU_KEY_PAGE_UP, MENU_KEY_PAGE_DOWN };
void menu_key(int key);
void menu_char(unsigned ch);
/* The pointer while the menu is open: where it is, in any units as long as the window's size w x h
 * is in the same ones, and its button presses and wheel notches through menu_key. */
enum { MENU_MOUSE_LEFT = 32, MENU_MOUSE_RIGHT, MENU_WHEEL_UP, MENU_WHEEL_DOWN };
void menu_mouse_move(int x, int y, int w, int h);

/* The renderer, at each present before the overlay: the pads, the queued keys and what they
 * change. overlay is the frame-rate overlay's switch (F1). Returns MENU_SHOT when a screenshot
 * with the menu on it is wanted (F3 while it is open; SSBU_MENU_KEYS' SHOT). */
enum { MENU_SHOT = 1 };
int menu_frame(int *overlay, int f3);
/* (gpu_overlay.c) draws the menu or the message, in a w x h picture. */
void menu_draw(float w, float h);
/* A message at the bottom of the picture for a few seconds (from any thread). */
void menu_toast(const char *text);
