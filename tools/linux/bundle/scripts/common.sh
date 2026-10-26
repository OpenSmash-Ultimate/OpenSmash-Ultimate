# Shared by the launchers (sourced from the folder the game is in).
#
# They serve two kinds of folder: this package, copied over from Windows (the game is ./ssbu), and
# a WindowSmash folder the game was built in on Linux (install-windowsmash.sh), whose launchers
# (play-ssbu.sh and so on) set SSBU_GAME_DIR to it and SSBU_EXE to build-linux/ssbu, and run
# these. There the game data is where the build found it: dump/modules/romfs.cfg says where.
#
# The game's data - its RomFS, about 17 GB - stays in the files it came in, read in place. For
# the original game that is one NCA. For an update (tools/update.py) it is pieced together from
# several files by the layer map dump/modules/romfs.layers: the base game's NCA and the update's
# own data, romfs_patch.bin. dump/modules/romfs.cfg says where those files are. On the first run
# (or when one has moved) the launchers look for each by name and size, and write that down:
# they search this folder, ./game, VMware's shared folders (/mnt/hgfs), /media, /mnt and your
# home folder. SSBU_NCA=<path> (the NCA) and SSBU_ROMFS_PATCH=<path> (romfs_patch.bin) name a
# file outright.

# Filled in by tools/linux/package_linux.py: "plain" (the RomFS in place in the NCA, from byte
# ROMFS_OFFSET on) or "layered", and the files, in romfs.cfg's order, one per line:
#   <variable naming it outright>|<name>|<size in bytes>|<its folder on Windows>
ROMFS_KIND="@ROMFS_KIND@"
ROMFS_OFFSET="@ROMFS_OFFSET@"
ROMFS_FILES='@ROMFS_FILES@'
ROMFS_MAP=dump/modules/romfs.layers
SSBU_EXE=${SSBU_EXE:-./ssbu}
SSBU_DEPS_HINT=${SSBU_DEPS_HINT:-bash install-deps.sh}

# romfs.cfg (runtime/hle_fs.c) for the files at the given paths
romfs_cfg_text() {
    if [ "$ROMFS_KIND" = layered ]; then
        printf 'layered\n%s\n' "$PWD/$ROMFS_MAP"
        printf '%s\n' "$@"
    else
        printf '%s\n%s\n' "$1" "$ROMFS_OFFSET"
    fi
}

file_size() {
    stat -L -c %s "$1" 2>/dev/null
}

# The files' paths romfs.cfg names now, one per line, if it is of this package's kind
romfs_cfg_paths() {
    local cfg=dump/modules/romfs.cfg
    [ -f "$cfg" ] || return 0
    if [ "$ROMFS_KIND" = layered ]; then
        [ "$(head -n 1 "$cfg")" = layered ] && tail -n +3 "$cfg"
    else
        [ "$(head -n 1 "$cfg")" != layered ] && head -n 1 "$cfg"
    fi
}

find_game_data() {
    local cfg=dump/modules/romfs.cfg old=() paths=() i=0 var name size windir path d text
    if [ -n "$SSBU_GAME_DIR" ]; then
        built_game_data
        return
    fi
    if [ "$ROMFS_KIND" = layered ] && [ ! -f "$ROMFS_MAP" ]; then
        echo "$ROMFS_MAP is missing: copy the whole ssbu-linux folder again."
        return 1
    fi
    mapfile -t old < <(romfs_cfg_paths)
    while IFS='|' read -r var name size windir; do
        path="${!var}"
        # (where romfs.cfg already has it, if it is still there)
        if [ -z "$path" ] && [ -f "${old[i]}" ] && [ "$(file_size "${old[i]}")" = "$size" ]; then
            path="${old[i]}"
        fi
        if [ -z "$path" ]; then
            echo "Looking for the game data ($name)..."
            for d in "$PWD" "$PWD/game" /mnt/hgfs /media /mnt "$HOME"; do
                [ -d "$d" ] || continue
                path=$(find -L "$d" -maxdepth 9 -name "$name" -type f -size "${size}c" -print -quit 2>/dev/null)
                [ -n "$path" ] && break
            done
        fi
        if [ -z "$path" ] || [ ! -f "$path" ]; then
            echo
            echo "Cannot find the game data: $name ($(awk "BEGIN { printf \"%.1f\", $size / 1e9 }") GB)."
            echo "On Windows it is in  $windir\\"
            echo "Copy it into $PWD/game/, or share that folder with this VM (VMware: VM > Settings >"
            echo "Options > Shared Folders), or run with $var=/path/to/$name bash run.sh"
            return 1
        fi
        if [ "$(file_size "$path")" != "$size" ]; then
            echo "$path is not the game data this folder needs: $name of $size bytes."
            return 1
        fi
        paths+=("$(readlink -f "$path")")
        i=$((i + 1))
    done <<< "$ROMFS_FILES"
    text=$(romfs_cfg_text "${paths[@]}")
    if [ "$text" != "$(cat "$cfg" 2>/dev/null)" ]; then
        printf '%s\n' "$text" > "$cfg"
        printf 'Game data: %s\n' "${paths[@]}"
    fi
    return 0
}

# A folder the game was built in: the files its romfs.cfg reads (plain: the file; layered: the
# layer map and its sources) are there
built_game_data() {
    local cfg=dump/modules/romfs.cfg path
    if [ ! -f "$cfg" ]; then
        echo "The game is not set up yet: run  $SSBU_DEPS_HINT"
        return 1
    fi
    while IFS= read -r path; do
        if [ -n "$path" ] && [ ! -f "$path" ]; then
            echo "The game data is not where it was when the game was built: $path"
            echo "Make it reachable there again (a disk or shared folder that is not mounted?), or run"
            echo "  $SSBU_DEPS_HINT"
            echo "to set it up again from where it is now."
            return 1
        fi
    done < <(if [ "$(head -n 1 "$cfg")" = layered ]; then tail -n +2 "$cfg"; else head -n 1 "$cfg"; fi)
    return 0
}

check_libraries() {
    if [ ! -f "$SSBU_EXE" ]; then
        echo "The game is not built yet ($SSBU_EXE): run  $SSBU_DEPS_HINT"
        return 1
    fi
    chmod +x "$SSBU_EXE" 2>/dev/null
    local missing lib
    lib="$(cd "$(dirname "$SSBU_EXE")" && pwd)/lib"
    missing=$(LD_LIBRARY_PATH="$lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd "$SSBU_EXE" 2>/dev/null | grep "not found")
    if [ -n "$missing" ]; then
        echo "Some libraries the game needs are not installed:"
        echo "$missing"
        echo "Install them with:  $SSBU_DEPS_HINT"
        return 1
    fi
    if ! vulkan_installed; then
        echo "No Vulkan driver is installed (the renderer needs one). Install them with:  $SSBU_DEPS_HINT"
        return 1
    fi
    return 0
}

# The Vulkan loader, and a driver for it: a manifest in any of the loader's folders (most systems
# have only some of them), or one named outright
vulkan_installed() {
    local f
    { ldconfig -p 2>/dev/null || /sbin/ldconfig -p 2>/dev/null; } | grep -q "libvulkan.so.1" ||
        [ -e /usr/lib/x86_64-linux-gnu/libvulkan.so.1 ] || [ -e /usr/lib64/libvulkan.so.1 ] || return 1
    [ -n "$VK_ICD_FILENAMES$VK_DRIVER_FILES" ] && return 0
    for f in /usr/share/vulkan/icd.d/*.json /usr/local/share/vulkan/icd.d/*.json /etc/vulkan/icd.d/*.json; do
        [ -f "$f" ] && return 0
    done
    return 1
}

# Where the game reads a settings file (runtime/config_folder.h): config/ once the game has made it
# (config/version.ini), else here; online_server.txt is config/'s only once there is one there.
config_file() {
    if [ -f config/version.ini ] && { [ "$1" != online_server.txt ] || [ -f "config/$1" ]; }; then
        printf '%s\n' "config/$1"
    else
        printf '%s\n' "$1"
    fi
}

# Online play (see docs/MULTIPLAYER.md): the lobby is the first line of online_server.txt that is not a
# comment - host:port, then an optional lobby code (only players with the same code see each
# other's rooms) - or "off". SSBU_NET_RELAY (and SSBU_NET_ROOM) set beforehand name a lobby
# outright. The name the other players see is kept in online_name.txt, asked for the first time.
go_online() {
    local relay code rest name pick
    local server_file name_file direct_file
    server_file=$(config_file online_server.txt) name_file=$(config_file online_name.txt) direct_file=$(config_file online_direct.txt)
    unset SSBU_NET_HOST
    if [ -z "$SSBU_NET_RELAY" ] && [ -f "$server_file" ]; then
        while read -r relay code rest || [ -n "$relay" ]; do
            relay=${relay%$'\r'} code=${code%$'\r'}
            case "$relay" in '' | '#'*) relay= code= ; continue ;; esac
            break
        done < "$server_file"
        [ "${relay,,}" = off ] && relay=
        SSBU_NET_RELAY=$relay
        [ -n "$code" ] && SSBU_NET_ROOM=$code
    fi
    if [ -z "$SSBU_NET_RELAY" ]; then
        unset SSBU_NET_RELAY
        echo "Playing offline (no lobby in online_server.txt)."
        return 0
    fi
    if [ -z "$SSBU_NET_NAME" ]; then
        if [ ! -s "$name_file" ]; then
            pick="Player$((RANDOM % 9000 + 1000))"
            echo "Online, other players see your name (it is kept in $name_file)."
            read -r -p "Your online name [$pick]: " name || echo
            printf '%s\n' "${name:-$pick}" > "$name_file"
        fi
        IFS= read -r SSBU_NET_NAME < "$name_file"
        SSBU_NET_NAME=${SSBU_NET_NAME%$'\r'}
    fi
    # "off" in online_direct.txt: all of a room's traffic through the lobby (no direct connections,
    # so the players in your rooms don't learn your internet address)
    if [ -z "$SSBU_NET_DIRECT" ] && [ -f "$direct_file" ]; then
        local direct
        while read -r direct rest || [ -n "$direct" ]; do
            direct=${direct%$'\r'}
            case "$direct" in '' | '#'*) continue ;; esac
            [ "${direct,,}" = off ] && export SSBU_NET_DIRECT=0
            break
        done < "$direct_file"
    fi
    export SSBU_NET_RELAY SSBU_NET_NAME
    [ -n "$SSBU_NET_ROOM" ] && export SSBU_NET_ROOM
    echo "Online: lobby $SSBU_NET_RELAY${SSBU_NET_ROOM:+ (lobby code $SSBU_NET_ROOM)}, as $SSBU_NET_NAME. To play offline: bash run-offline.sh"
}
