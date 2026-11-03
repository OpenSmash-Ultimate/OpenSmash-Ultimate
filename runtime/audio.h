/* The game's sound, as the rest of the runtime sees it (ipc_audio.c). */
#pragma once

/* The master volume, 0 to 100 (%), over what the game sets itself: SSBU_VOLUME, else display.ini's
 * volume, else 100. The settings menu changes it while playing (and writes display.ini). */
int audio_master_volume(void);
void audio_set_master_volume(int percent);
