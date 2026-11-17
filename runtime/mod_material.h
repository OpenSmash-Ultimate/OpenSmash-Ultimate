/* Queue live numeric material changes for the normal render-update thread.
 * File data stays owned by the caller; queued values are copied. */
#pragma once
#include "recomp.h"
typedef struct ModMaterialFile { const u8 *data; u32 size; } ModMaterialFile;
int mod_material_refresh(Cpu *c, const ModMaterialFile *files, u32 count, u32 target,
                         const u8 *data, u32 size, u32 *updated, u64 *request, char *reason, u32 reason_size);
enum { MOD_MATERIAL_PENDING = 0, MOD_MATERIAL_APPLIED = 1, MOD_MATERIAL_FAILED = -1, MOD_MATERIAL_PARTIAL = -2 };
/* Terminal requests are released when acknowledge is nonzero. */
int mod_material_result(u64 request, int acknowledge, char *reason, u32 reason_size);

void mod_material_init(void);
