/* Settings written back (hle_settings.c): the settings menu (gpu_menu.c) changes display.ini,
 * controllers.ini and the online_*.txt files (in config/: config_folder.h) in place, keeping their
 * comments and every line it does not change, so the files stay what players and the docs edit
 * by hand.
 *
 * Started with --online (Play SSBU.bat, play-ssbu.sh), the game takes its netplay settings from
 * online_server.txt, online_name.txt and online_direct.txt itself (net_relay.c); SSBU_NET_*
 * variables still win. */
#pragma once
#include <stddef.h>

void settings_args(int argc, char **argv);   /* host.c's main, first: --online */
int settings_online(void);                   /* started with --online */

/* key = value in an ini file. section NULL: a flat file (display.ini), any line with the key.
 * Returns 1 when found (value copied, its comment after it dropped). */
int settings_ini_get(const char *path, const char *section, const char *key, char *out, int size);
/* Sets key = value in place (its comment after the value kept), or adds it at the end of its
 * section (made at the end of the file when missing). value NULL removes the key. The file is
 * replaced in one step (a .tmp written first). Returns 1 on success. */
int settings_ini_set(const char *path, const char *section, const char *key, const char *value);

/* The one-line files (online_*.txt): the first line that is neither empty nor a '#' comment, as
 * Play SSBU.bat reads it. write replaces that line (or adds it), keeping the comments. */
int settings_line_read(const char *path, char *out, int size);
int settings_line_write(const char *path, const char *line);

/* A whole file in one step, as the writes above (<file>.tmp first). Returns 1 on success. */
int settings_file_write(const char *path, const void *data, size_t size);

/* Starts the game again, with the same command line and folder, and ends this one. With
 * --online, the SSBU_NET_* variables are dropped first, so the files' new values count. */
void settings_restart(void);
