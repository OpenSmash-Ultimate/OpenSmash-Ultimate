/* Offline checks of the settings files' writer (runtime/hle_settings.c), as the settings menu
 * uses it on display.ini, controllers.ini and the online_*.txt files: values changed in place
 * with their comments kept, keys added to and removed from sections, line ends kept, nothing
 * written when nothing changes, and the one-line files read as OpenSmash.exe reads them.
 *
 *   python -c "import sys; sys.path.insert(0,'tools'); import cc, subprocess; subprocess.run([cc.CLANG, '/nologo', '/O1', *cc.CFLAGS, 'tools/dev/settingstest.c', '/Febuild/settingstest.exe'], check=True)"
 *   build\settingstest.exe
 *
 * (tests/test_settings.py builds and runs it.) Exit code 0 when every check passes. */
#include "../../runtime/hle_settings.c"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char path[MAX_PATH];

static void put(const char *text) {
    FILE *f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}

static const char *got(void) {
    static char buf[8192];
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = 0;
    return buf;
}

#define SAME(want) do { const char *g_ = got(); CHECK(!strcmp(g_, want), "file is\n---\n%s---\nwanted\n---\n%s---", g_, want); } while (0)

static const char DISPLAY[] =
    "; display.ini\n"
    "\n"
    "; window = windowed | borderless | fullscreen\n"
    "window = windowed\n"
    "pacing = clock   ; the clock\n"
    "latency = 3\n";

static const char PADS[] =
    "[general]\n"
    "keyboard_player = 1\n"
    "background = 1\n"
    "\n"
    "[players]\n"
    "; 2 = DualSense Wireless Controller\n"
    "\n"
    "[device default]\n"
    "layout = positional\n"
    "deadzone = 0\n"
    "; R = righttrigger\n"
    "\n"
    "[device  ps5]\n"
    "rumble = 60\n";

int main(void) {
    char dir[MAX_PATH], v[128];
    GetTempPathA(sizeof dir, dir);
    GetTempFileNameA(dir, "set", 0, path);

    /* a flat file: in place, its comment kept; added at the end; removed */
    put(DISPLAY);
    CHECK(settings_ini_set(path, NULL, "pacing", "vrr"), "set pacing");
    CHECK(settings_ini_set(path, NULL, "window", "borderless"), "set window");
    CHECK(settings_ini_set(path, NULL, "overlay", "0"), "add overlay");
    SAME("; display.ini\n\n; window = windowed | borderless | fullscreen\nwindow = borderless\npacing = vrr   ; the clock\nlatency = 3\noverlay = 0\n");
    CHECK(settings_ini_get(path, NULL, "pacing", v, sizeof v) && !strcmp(v, "vrr"), "get pacing: '%s'", v);
    CHECK(settings_ini_get(path, NULL, "WINDOW", v, sizeof v) && !strcmp(v, "borderless"), "keys any case: '%s'", v);
    CHECK(!settings_ini_get(path, NULL, "gpu", v, sizeof v), "no gpu key");
    CHECK(settings_ini_set(path, NULL, "latency", NULL), "remove latency");
    SAME("; display.ini\n\n; window = windowed | borderless | fullscreen\nwindow = borderless\npacing = vrr   ; the clock\noverlay = 0\n");
    CHECK(settings_ini_set(path, NULL, "gpu", NULL), "removing a missing key is fine");

    /* nothing changes: the file is not written again (its time stays) */
    WIN32_FILE_ATTRIBUTE_DATA a0, a1;
    GetFileAttributesExA(path, GetFileExInfoStandard, &a0);
    Sleep(30);
    CHECK(settings_ini_set(path, NULL, "pacing", "vrr"), "same value");
    GetFileAttributesExA(path, GetFileExInfoStandard, &a1);
    CHECK(!memcmp(&a0.ftLastWriteTime, &a1.ftLastWriteTime, sizeof a0.ftLastWriteTime), "unchanged file rewritten");

    /* sections */
    put(PADS);
    CHECK(settings_ini_set(path, "device default", "deadzone", "10"), "deadzone");
    CHECK(settings_ini_set(path, "players", "2", "050000004c050000e60c000000810000"), "pin");
    CHECK(settings_ini_set(path, "device ps5", "rumble", "40"), "ps5 rumble (spaces in the header)");
    CHECK(settings_ini_set(path, "device default", "ZR", "rightshoulder"), "remap added at the section's end");
    CHECK(settings_ini_set(path, "device Xbox Series X Controller", "deadzone", "15"), "a new section");
    CHECK(settings_ini_set(path, "general", "keyboard_player", "0"), "keyboard");
    SAME("[general]\nkeyboard_player = 0\nbackground = 1\n\n[players]\n; 2 = DualSense Wireless Controller\n2 = 050000004c050000e60c000000810000\n\n"
         "[device default]\nlayout = positional\ndeadzone = 10\n; R = righttrigger\nZR = rightshoulder\n\n[device  ps5]\nrumble = 40\n\n"
         "[device Xbox Series X Controller]\ndeadzone = 15\n");
    CHECK(settings_ini_get(path, "players", "2", v, sizeof v) && !strcmp(v, "050000004c050000e60c000000810000"), "get pin: '%s'", v);
    CHECK(!settings_ini_get(path, "general", "deadzone", v, sizeof v), "a key of another section is not found");
    CHECK(settings_ini_set(path, "players", "2", NULL), "unpin");
    CHECK(!settings_ini_get(path, "players", "2", v, sizeof v), "unpinned");

    /* CRLF stays CRLF; a BOM goes */
    put("\xEF\xBB\xBFwindow = windowed\r\npacing = clock\r\n");
    CHECK(settings_ini_set(path, NULL, "pacing", "display"), "crlf");
    SAME("window = windowed\r\npacing = display\r\n");
    CHECK(settings_ini_set(path, NULL, "hide_cursor", "0"), "crlf add");
    SAME("window = windowed\r\npacing = display\r\nhide_cursor = 0\r\n");

    /* a missing file is made */
    DeleteFileA(path);
    CHECK(settings_ini_set(path, "general", "background", "0"), "new file");
#ifdef _WIN32
    SAME("[general]\r\nbackground = 0\r\n");
#endif

    /* one-line files */
    put("# The online lobby\n# host:port\n\nsmash.example:39421 friends\n");
    CHECK(settings_line_read(path, v, sizeof v) && !strcmp(v, "smash.example:39421 friends"), "line read: '%s'", v);
    CHECK(settings_line_write(path, "off"), "line write");
    SAME("# The online lobby\n# host:port\n\noff\n");
    put("# only a comment\n");
    CHECK(!settings_line_read(path, v, sizeof v) && !v[0], "no line");
    CHECK(settings_line_write(path, "Player1234"), "line added");
    SAME("# only a comment\nPlayer1234\n");
    DeleteFileA(path);
    CHECK(!settings_line_read(path, v, sizeof v), "missing file");
    CHECK(settings_line_write(path, "on"), "line file made");
    CHECK(settings_line_read(path, v, sizeof v) && !strcmp(v, "on"), "read back: '%s'", v);

    char tmp[MAX_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    CHECK(GetFileAttributesA(tmp) == INVALID_FILE_ATTRIBUTES, "a .tmp left behind");
    DeleteFileA(path);
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
