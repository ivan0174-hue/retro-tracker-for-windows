/*
 * Platform layer interface (implemented by main_win32.c, and by the
 * headless test harness on other systems).
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

enum { FT_MODULE = 0, FT_SAMPLE, FT_WAV_EXPORT, FT_SAMPLE_SAVE };

void     plat_lock(void);
void     plat_unlock(void);
uint64_t plat_audio_played_frames(void);
bool     plat_audio_ok(void);
bool     plat_open_dialog(int kind, char *path, int pathSize);
bool     plat_save_dialog(int kind, const char *defName, char *path, int pathSize);
void     plat_quit(void);
void     plat_set_title(const char *title);
uint32_t plat_ms(void);
const char *plat_config_path(void);
FILE    *plat_fopen(const char *utf8path, const char *mode);   /* UTF-8 aware fopen */

#endif
