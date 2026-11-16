/* Display options (display.ini): the window's mode and size, the picture's place in it, and the
 * pacing of frames (gpu_display.c). */
#pragma once
#ifndef __OBJC__   /* (macOS's window and renderer are Objective-C, whose BOOL is not the shim's: they need none of it) */
#include <windows.h>
#endif
#include "renderer.h"

typedef enum { PACING_CLOCK, PACING_VRR, PACING_DISPLAY } Pacing;
enum { DISPLAY_WINDOWED, DISPLAY_BORDERLESS, DISPLAY_FULLSCREEN };

void display_dpi_aware(void);           /* first thing in main: window sizes in real pixels */
#ifdef _WIN32
/* The game's window, made by the renderer's window thread with its class and procedure. */
HWND display_create_window(HINSTANCE inst, const wchar_t *cls, const wchar_t *title);
/* Shown on the first presented frame. Until then the window stays hidden, so startup is not a white box. */
void display_show_window(void);
/* The window procedure offers every message here first: nonzero = handled, the answer in *result. */
int display_message(HWND h, UINT msg, WPARAM w, LPARAM l, LRESULT *result);
#else
/* Linux and macOS: linux/window.c or macos/window.m makes the window with SDL from display.ini's
 * settings and reports to it. */
int display_window_mode(void);
void display_note_mode(int mode);   /* the window's thread: its mode changed (F11 too), for the settings menu */
void display_window_size(int *width, int *height);   /* (macOS: in points) */
int display_hide_cursor(void);
void display_note_size(unsigned w, unsigned h);   /* the window's inside changed (pixels) */
void display_note_refresh(int hz);                /* the window's monitor refreshes hz times a second */
#endif
/* The renderer, at a present: the window's client size, when it changed since the last call. */
int display_take_size(unsigned *w, unsigned *h);
/* The picture's shape, width over height, as drawn now: 16:9 unless display.ini's aspect (SSBU_ASPECT)
 * says otherwise, at most 32:9; 16:9 for online matches (gpu_display.c, display_aspect_take). */
float display_aspect(void);
/* 1: always 16:9 this session (macOS: Metal not ported yet). */
int display_shape_locked(void);
/* (the renderer, between two frames) 1 when a new shape is taken now (never in an online fight). */
int display_aspect_take(void);
/* The view (docs/ULTRAWIDE-CONTEXT.md): display.ini view, SSBU_VIEW over it. */
enum { VIEW_WIDE, VIEW_ZOOMED, VIEW_HYBRID };
int display_view(void);
/* (the settings menu) the picture shape as set, and a change to it; both are taken at once (the shape
 * at the next frame, the view now). */
const char *display_aspect_setting(void);
void display_set_aspect(const char *text);
void display_set_view(int view);
/* Where a fight's timer is drawn (display.ini timer; the settings menu changes it at once). */
enum { TIMER_RIGHT, TIMER_CENTER, TIMER_LEFT, TIMER_HIDDEN };
int display_timer(void);
void display_set_timer(int timer);
/* The picture (display_aspect's shape) centred in a w x h back buffer. */
void display_fit(unsigned w, unsigned h, unsigned *x, unsigned *y, unsigned *fw, unsigned *fh);
int display_latency(void);              /* frames the GPU may queue ahead: 1-3 */
int display_low_latency(void);          /* low_latency: the game released late in the frame (ipc_vi.c) */
void display_before_present(void);       /* (the renderers, before each present: the loading animation stops at the first) */
void display_loading_note(const char *note);   /* (the text beside the loading animation, e.g. "Shaders 40%"; NULL: none) */
int display_runahead(void);             /* runahead: frames run ahead in fights (hle_rb_core.c), 0 off */
void display_set_runahead(int frames);  /* (the settings menu: from now on) */
Pacing display_pacing(void);
/* (rollback's time sync, hle_rb_core.c) the game ahead of the other player runs this many ms slower: clock and vrr pacing
 * add it to the next refreshes' schedule (a frame a little later, no refresh left out); display pacing, held to the
 * monitor, leaves a refresh out for each whole frame owed */
int display_tick_stretch(double ms);
void display_tick_stretch_reset(void);   /* (a fight starts) */
const char *display_gpu(void);          /* the graphics card to use: "auto", or part of its name (SSBU_GPU) */
/* render_scale: the game's pictures are drawn this many times its 1920x1080 in each direction
 * (0.5-4; SSBU_RENDER_SCALE). 0 = auto: as many as the picture has pixels in the window at the
 * start (1440p: 1.33), at least 1. */
float display_render_scale(void);
/* renderer: which back end draws (renderer.h: RENDERER_DEFAULT or RENDERER_VULKAN; SSBU_RENDERER). */
int display_renderer(void);
/* The renderer says whether the swap chain can tear (for VRR); the flags each Present gets. */
void display_set_tearing(int supported);
unsigned display_present_flags(void);
/* The vsync thread: returns when the next refresh the game sees is due. */
void display_wait_tick(void);
/* SSBU_SPEED (test runs): 1 = normal; else the vsync thread gives one refresh per frame shown,
 * n x 60 a second (display_speed_tick waits for the next one's time), 0 = uncapped. */
double display_speed(void);
void display_speed_tick(void);

/* The settings menu (gpu_menu.c), which saves display.ini itself: the settings in effect, and
 * changes that take effect now. The window's size and the graphics card count at the next start. */
typedef struct DisplaySettings { int mode, width, height, latency, hide_cursor, overlay; Pacing pacing; char gpu[64]; float render_scale; int renderer; } DisplaySettings;
void display_settings(DisplaySettings *out);
const char *display_ini_path(void);
int display_overlay(void);                /* overlay = 1: the frame-rate overlay shows from the start */
float display_overlay_scale(void);        /* overlay_scale = 0.25-2: the overlay's size */
void display_set_mode(int mode);          /* DISPLAY_*: the window switches now (and F11 goes back to it) */
int display_take_mode(void);              /* (Linux, the window's loop) a mode asked for, or -1 */
void display_set_pacing(Pacing pacing);
void display_set_hide_cursor(int on);
void display_set_latency(int frames);
int display_take_latency(void);           /* the renderer, at a present: a new latency, or 0 */
void display_set_render_scale(float scale);   /* (Direct3D 11 and Vulkan take it at their next present; Metal at the next start) */
void display_set_window_height(int height);   /* the windowed height; the width follows the picture shape (Windows: now) */
int display_window_height_fits(int height);   /* whether that windowed height fits the game window's monitor at the shape now */
#ifndef _WIN32
int display_take_window_size(int *w, int *h);   /* (linux/window.c's loop) 1: the windowed window takes this size now */
#endif
int display_window_sizing(void);   /* whether the window or its edge is being dragged now (Windows) */
/* The settings' Experimental tab: the picture shape kept in online matches (display.ini
 * experimental_ultrawide_online, read at the start; docs/adr/0001, 0002). */
int display_uw_online(void);         /* this session's */
int display_uw_online_saved(void);   /* the one saved for the next start */
void display_set_uw_online(int on);
void display_set_renderer(int renderer);      /* (the same: RENDERER_*, which back end draws from the next start) */
void display_set_gpu(const char *gpu);          /* (the same: the graphics card, "auto" or its name, picked at the next start) */
