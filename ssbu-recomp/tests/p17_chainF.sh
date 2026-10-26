#!/bin/sh
# Round 17: the tagged draw logs with the corrected tag (armed by the present of fight frame n-1: SSBU_SHOT_FIGHT names
# n-1 and n). Links ssbu_p17c.exe from the tree, then zelda_temple and yoshi_cartboard in Vulkan (NVIDIA), OpenGL and
# Vulkan on the Intel device (SSBU_VK_DEVICE=1). Compare with: python tests/p17_dt.py <gl log> <vk log> <fight frame> box
cd "$(dirname "$0")/.." || exit 1
while [ -e tests/OWNER_PLAYING ]; do sleep 15; done
RECOMP_DLL=ssbu_game_p15.dll python tests/coord_relink.py ssbu_own3.exe ssbu_p17c.exe nvn_vk.c nvn_vk_store.c nvn_vk_draw.c nvn_vk_m4.c nvn_vk_menu.c nvn_vk_flash.c nvn_vk_wd.c nvn_front.c nvn_gl.c display.c os_threads.c mx_translate.c > tests/logs/p17c_link.txt 2>&1 || { echo "link failed"; tail -30 tests/logs/p17c_link.txt; exit 1; }
ZT="p1=mario,p2=donkey,p2=cpu,cpu=3,stage=zelda_temple"
YC="p1=mario,p2=donkey,p2=cpu,cpu=3,stage=yoshi_cartboard"
F=SSBU_SHOT_FIGHT=699,700,1199,1200
sh tests/p17_fast.sh zvk2 ssbu_p17c.exe "$ZT" $F SSBU_DEBUG_END=1300 SSBU_DRAWTAG=1
sh tests/p17_fast.sh zgl2 ssbu_p17c.exe "$ZT" $F SSBU_DEBUG_END=1300 SSBU_DRAWTAG=1 SSBU_RENDERER=gl SSBU_NO_PAIR_LIST=1 SSBU_DRAWSHOT_BOX=456,760,96,88 P17_SECS=500
sh tests/p17_fast.sh zint2 ssbu_p17c.exe "$ZT" $F SSBU_DEBUG_END=1300 SSBU_DRAWTAG=1 SSBU_VK_DEVICE=1 SSBU_VK_PREPARE=off P17_SECS=500
sh tests/p17_fast.sh yvk2 ssbu_p17c.exe "$YC" $F SSBU_DEBUG_END=1300 SSBU_DRAWTAG=1
sh tests/p17_fast.sh ygl2 ssbu_p17c.exe "$YC" $F SSBU_DEBUG_END=1300 SSBU_DRAWTAG=1 SSBU_RENDERER=gl SSBU_NO_PAIR_LIST=1 P17_SECS=500
sh tests/p17_fast.sh yint2 ssbu_p17c.exe "$YC" $F SSBU_DEBUG_END=1300 SSBU_DRAWTAG=1 SSBU_VK_DEVICE=1 SSBU_VK_PREPARE=off P17_SECS=500
echo "chain F done"
