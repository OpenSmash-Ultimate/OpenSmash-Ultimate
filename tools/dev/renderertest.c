/* display.ini's renderer (runtime/renderer.h, read by runtime/gpu_display.c, acted on by
 * runtime/gpu_renderer.c) with no game around it, for tests/test_renderer_option.py.
 *
 *   Windows: clang-cl /O1 <cc.CFLAGS> tools/dev/renderertest.c (cc.CFLAGS has the Vulkan headers when cc.py found them)
 *
 * The display.ini read is SSBU_DISPLAY's (the test's own file); SSBU_RENDERER is read as the game
 * reads it. Arguments, each answered on a line of its own:
 *   parse VALUE   renderer_parse's answer: "parse VALUE -> N NAME" (N -1: not a renderer's name)
 *   chosen        display.ini's choice, as the renderer's start sees it: "chosen NAME"
 *   start         gfx_renderer_start (no window): "start NAME", the renderer that draws
 *   default       the default display.ini's text, as the game writes it
 *   mode N        display_set_mode(N), as the settings menu does; no answer
 *   shown         the mode the menu shows and the one F11 goes back to: "shown M FULL" */
#include "../../runtime/gpu_display.c"
#include "../../runtime/gpu_renderer.c"
#include "../../runtime/gpu_vulkan.c"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winmm.lib")

/* ---- what gpu_display.c reaches outside itself (none of it runs here) ---------------------- */
const char *config_path(enum ConfigFile file) { (void)file; return "display.ini"; }   /* (SSBU_DISPLAY names the test's) */
void *gfx_window_handle(void) { return NULL; }
void *gfx_display_output(void) { return NULL; }

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "parse") && i + 1 < argc) {
            int r = renderer_parse(argv[i + 1]);
            printf("parse %s -> %d %s\n", argv[i + 1], r, r < 0 ? "-" : renderer_name(r));
            i++;
        } else if (!strcmp(argv[i], "chosen")) printf("chosen %s\n", renderer_name(display_renderer()));
        else if (!strcmp(argv[i], "start")) printf("start %s\n", renderer_name(gfx_renderer_start(NULL)));
        else if (!strcmp(argv[i], "default")) fputs(display_default_ini(), stdout);
        else if (!strcmp(argv[i], "mode") && i + 1 < argc) display_set_mode(atoi(argv[++i]));
        else if (!strcmp(argv[i], "shown")) {
            DisplaySettings ds;
            display_settings(&ds);
            printf("shown %d %d\n", ds.mode, cfg.mode);
        }
        else { printf("error %s\n", argv[i]); return 2; }
        fflush(stdout);
    }
    return 0;
}
