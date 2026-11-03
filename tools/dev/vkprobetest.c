/* The Vulkan check (runtime/gpu_vulkan.c's vk_probe) with no game around it, for
 * tests/test_renderer_option.py: what display.ini's renderer = vulkan finds on this PC.
 *
 *   Windows: clang-cl /O1 <cc.CFLAGS, with cc.py's Vulkan flags> tools/dev/vkprobetest.c
 *
 * Arguments: [--window] [--gpu NAME]. --window checks presenting to a window of its own, made
 * and never shown (the game's is its HWND); --gpu is display.ini's gpu (default auto).
 * Prints "probe OK WHAT" (OK 1: Vulkan can run the back end here; 0: not, WHAT says why), and the
 * "[vulkan]" lines on stderr. Exit status 0, or 2 for a wrong argument. */
#include "../../runtime/gpu_vulkan.c"

#pragma comment(lib, "user32.lib")

static const char *gpu = "auto";
const char *display_gpu(void) { return gpu; }

int main(int argc, char **argv) {
    void *window = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--gpu") && i + 1 < argc) gpu = argv[++i];
        else if (!strcmp(argv[i], "--window")) {
#ifdef _WIN32
            WNDCLASSW wc;
            memset(&wc, 0, sizeof wc);
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(NULL);
            wc.lpszClassName = L"vkprobetest";
            RegisterClassW(&wc);
            window = CreateWindowExW(0, L"vkprobetest", L"vkprobetest", WS_OVERLAPPEDWINDOW, 0, 0, 320, 180, NULL, NULL, wc.hInstance, NULL);
            if (!window) { printf("error no window\n"); return 2; }
#endif
        } else { printf("error %s\n", argv[i]); return 2; }
    }
    char what[600];
    int ok = vk_probe(window, what, sizeof what);
    printf("probe %d %s\n", ok, what);
    fflush(stdout);
#ifdef _WIN32
    if (window) DestroyWindow((HWND)window);
#endif
    return 0;
}
