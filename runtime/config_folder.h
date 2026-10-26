/* The settings files' folder (hle_config.c): config/ in the port's folder (the working folder
 * every launcher starts the game in), with config/version.ini saying which layout it has.
 *
 * The first start that has it makes the folder and copies the settings files in from where they
 * were before: WindowSmash's from the port's folder itself, and ssbu-recomp's from its build
 * folder's config/ when this folder holds an ssbu-recomp install. The old files stay where they
 * were, a file already in config/ is never replaced, and version.ini, written last, records what
 * was copied: the copy is made once. Where the folder cannot be made or written (a read-only
 * folder), the files stay where they were for that start, as before.
 *
 * SSBU_DISPLAY and SSBU_CONTROLLERS still name a file of their own (test runs).
 *
 * (Not config.h: the builds compile third-party sources with -I runtime, and vkd3d's include a
 * config.h of their own, which this file would stand in for.) */
#pragma once

enum ConfigFile {
    CONFIG_DISPLAY,         /* display.ini (gpu_display.c) */
    CONFIG_CONTROLLERS,     /* controllers.ini (hle_pad.c) */
    CONFIG_LAUNCHER,        /* launcher.ini (tools/launcher.ps1 only) */
    CONFIG_ONLINE_SERVER,   /* online_server.txt: config/'s once there is one, else the port's own */
    CONFIG_ONLINE_NAME,     /* online_name.txt */
    CONFIG_ONLINE_DIRECT,   /* online_direct.txt */
    CONFIG_ONLINE_PROTOCOL, /* online_protocol.txt (net_relay.c: the netplay version) */
    CONFIG_FILES
};

enum { CONFIG_VERSION = 1 };   /* version.ini's version = ... */

/* Where the file is read and written (relative to the working folder). The first call makes
 * the folder and copies the old files in. */
const char *config_path(enum ConfigFile file);

/* (hle_pad.c) An ssbu-recomp controllers.ini rewritten in this port's terms: what has an
 * equivalent here as this file's keys, the rest kept as it was. Returns 1 when written. */
int pad_ini_convert(const char *from, const char *to);
/* (gpu_display.c, hle_pad.c) the files as they are written when missing, with their help */
const char *display_default_ini(void);
const char *pad_default_ini(void);
