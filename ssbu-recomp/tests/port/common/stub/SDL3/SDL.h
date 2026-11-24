/* tests/port/common: stand-in for <SDL3/SDL.h>, the declarations plat_posix.c names, so the tests/port suites build where
 * SDL3's headers are not (a device without the deps tree, a Mac without Homebrew's SDL3). No test reaches SDL: the suites
 * link abort() stubs for every SDL_ symbol plat_posix.o names (native_env.sh nat_stubs). */
#ifndef PLAT_TEST_SDL_STUB_H
#define PLAT_TEST_SDL_STUB_H
#include <stdbool.h>
#include <stdint.h>
#define SDLCALL
typedef struct SDL_Window SDL_Window;
typedef struct SDL_AudioStream SDL_AudioStream;
typedef uint64_t SDL_WindowFlags;
typedef uint32_t SDL_InitFlags;
typedef uint32_t SDL_AudioDeviceID;
typedef enum { SDL_AUDIO_U8 = 0x0008, SDL_AUDIO_S16LE = 0x8010 } SDL_AudioFormat;
typedef struct { SDL_AudioFormat format; int channels; int freq; } SDL_AudioSpec;
typedef void (SDLCALL *SDL_AudioStreamCallback)(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount);
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ((SDL_AudioDeviceID)0xFFFFFFFFu)
#define SDL_INIT_AUDIO 0x00000010u
#define SDL_WINDOW_HIDDEN 0x0000000000000008ULL
#define SDL_WINDOW_MINIMIZED 0x0000000000000040ULL
typedef enum {
    SDL_SCANCODE_UNKNOWN = 0, SDL_SCANCODE_A = 4, SDL_SCANCODE_1 = 30, SDL_SCANCODE_0 = 39, SDL_SCANCODE_RETURN = 40,
    SDL_SCANCODE_ESCAPE, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_TAB, SDL_SCANCODE_SPACE, SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS,
    SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_BACKSLASH, SDL_SCANCODE_SEMICOLON = 51,
    SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_GRAVE, SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH,
    SDL_SCANCODE_F1 = 58, SDL_SCANCODE_INSERT = 73, SDL_SCANCODE_HOME, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_DELETE,
    SDL_SCANCODE_END, SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_RIGHT, SDL_SCANCODE_LEFT, SDL_SCANCODE_DOWN, SDL_SCANCODE_UP,
    SDL_SCANCODE_KP_1 = 89, SDL_SCANCODE_KP_0 = 98, SDL_SCANCODE_LCTRL = 224, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LALT,
    SDL_SCANCODE_RCTRL = 228, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RALT
} SDL_Scancode;
typedef enum { SDL_MESSAGEBOX_ERROR = 0x10, SDL_MESSAGEBOX_WARNING = 0x20, SDL_MESSAGEBOX_INFORMATION = 0x40 } SDL_MessageBoxFlags;
typedef enum { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT = 1, SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT = 2 } SDL_MessageBoxButtonFlags;
typedef struct { SDL_MessageBoxButtonFlags flags; int buttonID; const char *text; } SDL_MessageBoxButtonData;
typedef struct { SDL_MessageBoxFlags flags; SDL_Window *window; const char *title; const char *message; int numbuttons;
                 const SDL_MessageBoxButtonData *buttons; const void *colorScheme; } SDL_MessageBoxData;
typedef void (*SDL_FunctionPointer)(void);
const bool *SDL_GetKeyboardState(int *numkeys);
SDL_Window *SDL_GetKeyboardFocus(void);
bool SDL_ShowMessageBox(const SDL_MessageBoxData *d, int *buttonid);
bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags f, const char *title, const char *message, SDL_Window *w);
SDL_FunctionPointer SDL_GL_GetProcAddress(const char *proc);
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *w);
bool SDL_SetWindowTitle(SDL_Window *w, const char *title);
const char *SDL_GetWindowTitle(SDL_Window *w);
bool SDL_HideWindow(SDL_Window *w);
bool SDL_ShowWindow(SDL_Window *w);
bool SDL_MinimizeWindow(SDL_Window *w);
bool SDL_RestoreWindow(SDL_Window *w);
bool SDL_DisableScreenSaver(void);
bool SDL_EnableScreenSaver(void);
bool SDL_GetWindowSizeInPixels(SDL_Window *w, int *x, int *y);
bool SDL_PutAudioStreamData(SDL_AudioStream *s, const void *buf, int len);
SDL_InitFlags SDL_WasInit(SDL_InitFlags f);
bool SDL_InitSubSystem(SDL_InitFlags f);
const char *SDL_GetError(void);
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID d, const SDL_AudioSpec *spec, SDL_AudioStreamCallback cb, void *user);
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *s);
bool SDL_PauseAudioStreamDevice(SDL_AudioStream *s);
bool SDL_LockAudioStream(SDL_AudioStream *s);
bool SDL_UnlockAudioStream(SDL_AudioStream *s);
bool SDL_ClearAudioStream(SDL_AudioStream *s);
void SDL_DestroyAudioStream(SDL_AudioStream *s);
int SDL_GetAudioStreamQueued(SDL_AudioStream *s);
bool SDL_SetAudioStreamGain(SDL_AudioStream *s, float gain);
int SDL_GetVersion(void);
#endif
