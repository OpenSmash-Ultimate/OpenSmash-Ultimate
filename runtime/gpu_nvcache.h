/* NVIDIA's own shader caches on Windows (gpu_nvcache.c): where the Vulkan driver keeps the game's,
 * how big they may grow, and a line in the log about the driver's cache size setting. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* (gpu_vk.c, before the Vulkan instance: the driver reads it as it loads) NVIDIA's Vulkan driver keeps
 * the game's GPU code in <shader_cache_dir>\nvidia instead of %LOCALAPPDATA%\NVIDIA\GLCache, trimmed to
 * SSBU_NV_CACHE_MB (oldest sets first). Nothing when __GL_SHADER_DISK_CACHE_PATH is set already or
 * SSBU_NV_CACHE=0. */
void nv_cache_vulkan_setup(const char *shader_cache_dir);

/* (either renderer, once its NVIDIA device is made) a thread that reads the driver's "Shader Cache Size"
 * and how much its cache folders hold, logs both, and warns when they are over it. vulkan: the Vulkan
 * renderer draws (its own folder, GLCache), else Direct3D 11 (DXCache). */
void nv_cache_check(int vulkan);

/* The folder's sets of files (a name with any extensions: NVIDIA's .bin and .toc), oldest first, removed
 * until the folder holds at most `cap` bytes; a set written in the last `keep_ms` stays (a game running
 * now). Returns the bytes left; *removed, *removed_bytes what went (any may be NULL). For the test. */
uint64_t nv_cache_trim(const char *dir, uint64_t cap, uint64_t keep_ms, uint32_t *removed, uint64_t *removed_bytes);

/* "10 GB", "Unlimited", "Driver Default"... for the driver's setting value (DRS 0x00AC8497, megabytes);
 * found 0: the setting is not in the profile. For the test. */
void nv_cache_setting_words(int found, uint32_t mb, char *out, size_t size);
