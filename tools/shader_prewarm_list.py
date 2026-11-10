"""Write media/shader_prewarm.txt from a played shader cache: the programs the game drew, newest-played first.

The Direct3D 11 prewarm (runtime/gpu_d3d11.c, prewarm_queue_list) also makes these at start when Prepare Shaders
(tools/shader_aot.py) put them in the player's cache: a new player's cache has the files but none dated as played
(issue #227). Use: python tools/shader_prewarm_list.py [cache folder, default dump/shadercache] [output]
"""
import glob
import os
import sys

PLAYED = 1009843200   # (2002-01-01: runtime/gpu_shader_cache.h SHADER_CACHE_PLAYED_UNIX; Prepare Shaders dates its files before it)
VERSION = 10          # (gpu_shader_cache.h SHADER_CACHE_VERSION)


def main():
    cache = sys.argv[1] if len(sys.argv) > 1 else os.path.join("dump", "shadercache")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join("media", "shader_prewarm.txt")
    files = [f for f in glob.glob(os.path.join(cache, f"v{VERSION}_*.dxbc")) if os.path.getmtime(f) >= PLAYED]
    files.sort(key=lambda f: -os.path.getmtime(f))
    with open(out, "w", newline="\n") as o:
        o.write("# Shader programs the game draws in normal play (menus, many fighters and stages), newest-played first: the\n")
        o.write("# Direct3D 11 prewarm also makes these at start when Prepare Shaders put them in the cache (runtime/gpu_d3d11.c).\n")
        o.write("# Made by tools/shader_prewarm_list.py from a played cache. One per line: <stage>_<hash>_<vs outputs>_<tables>.\n")
        for f in files:
            o.write(os.path.basename(f)[len(f"v{VERSION}_"):-len(".dxbc")] + "\n")
    print(f"{out}: {len(files)} programs")


if __name__ == "__main__":
    main()
