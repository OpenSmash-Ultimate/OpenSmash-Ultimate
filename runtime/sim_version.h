/* The simulation version: a number of the port's own, which the online lobby's protocol version 3
 * parts players by, with the game's build id, its mods and the lobby code (tools/ssbu_relay.py,
 * "Version 3's key"): two games of different simulation versions never share a room or a match.
 * net_relay.c sends it in version 3's JOIN.
 *
 * Bump it whenever the lifter or the runtime changes what the game computes: anything that can make
 * two games' matches differ - an instruction lifted otherwise, the compiler flags that pin floating
 * point, a system call or a guest function answered otherwise, a fix that changes a function's
 * results. Drawing, sound, loading and logging alone change nothing here. When unsure, bump it: a
 * bump only keeps players of different versions apart, a missed one lets their matches drift apart.
 * Never a number per OS or per CPU: Windows, Linux and macOS builds of one version, avx2 and
 * x86-64-v2 alike, must compute the same and meet. */
#pragma once

#define SIM_VERSION 1u
