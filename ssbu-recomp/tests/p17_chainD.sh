#!/bin/sh
# Round 17: the picture-defect chain. Waits for tests/OWNER_PLAYING to go, links ssbu_p17b.exe from the tree (the test
# instruments of nvn_dt.h, SSBU_VK_CLIP, SSBU_MUTEXSTAT, the present job timed in parts), then runs the same fight on
# zelda_temple and yoshi_cartboard in Vulkan and OpenGL with the tagged draw log, and three Vulkan variants.
cd "$(dirname "$0")/.." || exit 1
while [ -e tests/OWNER_PLAYING ]; do sleep 15; done
RECOMP_DLL=ssbu_game_p15.dll python tests/coord_relink.py ssbu_own3.exe ssbu_p17b.exe nvn_vk.c nvn_vk_store.c nvn_vk_draw.c nvn_vk_m4.c nvn_vk_menu.c nvn_vk_flash.c nvn_vk_wd.c nvn_front.c nvn_gl.c display.c os_threads.c mx_translate.c > tests/logs/p17b_link.txt 2>&1 || { echo "link failed"; tail -30 tests/logs/p17b_link.txt; exit 1; }
ZT="p1=mario,p2=donkey,p2=cpu,cpu=3,stage=zelda_temple"
YC="p1=mario,p2=donkey,p2=cpu,cpu=3,stage=yoshi_cartboard"
sh tests/p17_fast.sh zvk ssbu_p17b.exe "$ZT" SSBU_SHOT_FIGHT=100,700,1200 SSBU_DEBUG_END=1300 SSBU_DRAWTAG=1
sh tests/p17_fast.sh yvk ssbu_p17b.exe "$YC" SSBU_SHOT_FIGHT=100,700,1200 SSBU_DEBUG_END=1300 SSBU_DRAWTAG=1
sh tests/p17_fast.sh zgl ssbu_p17b.exe "$ZT" SSBU_SHOT_FIGHT=100,700,1200 SSBU_DEBUG_END=1300 SSBU_DRAWTAG=1 SSBU_RENDERER=gl SSBU_NO_PAIR_LIST=1 SSBU_DRAWSHOT_BOX=456,760,96,88 P17_SECS=500
sh tests/p17_fast.sh ygl ssbu_p17b.exe "$YC" SSBU_SHOT_FIGHT=100,700,1200 SSBU_DEBUG_END=1300 SSBU_DRAWTAG=1 SSBU_RENDERER=gl SSBU_NO_PAIR_LIST=1 P17_SECS=500
sh tests/p17_fast.sh zeq ssbu_p17b.exe "$ZT" SSBU_SHOT_FIGHT=100,700,1200 SSBU_DEBUG_END=1300 SSBU_VK_TEST=192
mkdir -p ../cache/vk_p17e
sh tests/p17_fast.sh zclip ssbu_p17b.exe "$ZT" SSBU_SHOT_FIGHT=100,700,1200 SSBU_DEBUG_END=1300 SSBU_VK_CLIP=2 SSBU_VK_CACHE=../cache/vk_p17e SSBU_VK_PREPARE=off P17_SECS=600
echo "chain D done"
