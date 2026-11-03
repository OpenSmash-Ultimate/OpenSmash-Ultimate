/* Native lib_paramconfig toml path (docs/SKYLINE.md): dump/skyline/paramconfig.json. */
#pragma once

struct Cpu;

/* Load dump/skyline/paramconfig.json and hook get_param_int/float/int64. Call before plugins. */
void paramconfig_install(struct Cpu *c);
