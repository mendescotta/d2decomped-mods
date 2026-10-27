#!/bin/sh
# Launch Diablo II natively (d2d) with the roomstate mod on: each room's
# monsters come back as they were left (docs/mods/roomstate.md).
#
# Usage: mod-tools/play-d2-roomstate.sh [extra d2d args...]
# Its saves and settings are its own, under ~/.local/share/d2native/roomstate/d2d/
# (d2d reads $XDG_CONFIG_HOME/d2d). On the first launch the vanilla saves
# (~/.config/d2d/save) are copied in once; after that the two never share a save.
# Needs D2_PATCH_INSTALLER (the 1.14d LODPatch_114d.exe). Overrides: D2D_BIN
# (default: this repo's build/apps/d2d/d2d), D2D_ROOMSTATE_HOME, D2_MPQ_DIR
# (default: the Wine install under ~/Games/diablo-ii; d2d never writes there).
ROOT=$(cd "$(dirname "$0")/.." && pwd)
D2D_BIN=${D2D_BIN:-$ROOT/build/apps/d2d/d2d}
D2_MPQ_DIR=${D2_MPQ_DIR:-"$HOME/Games/diablo-ii/drive_c/Program Files (x86)/Diablo II"}
home=${D2D_ROOMSTATE_HOME:-$HOME/.local/share/d2native/roomstate}
fail() { msg="play-d2-roomstate: $1"; echo "$msg" >&2; command -v notify-send >/dev/null && notify-send "Diablo II" "$msg"; exit 1; }
[ -x "$D2D_BIN" ] || fail "no d2d binary at $D2D_BIN (cmake -B build && cmake --build build)"
[ -f "$D2_MPQ_DIR/d2data.mpq" ] || fail "no d2data.mpq in $D2_MPQ_DIR"
[ -f "${D2_PATCH_INSTALLER:-}" ] || fail "set D2_PATCH_INSTALLER to the 1.14d patch (LODPatch_114d.exe)"
if [ ! -d "$home/d2d/save" ]; then
    mkdir -p "$home/d2d/save" || fail "can't create $home/d2d/save"
    [ -d "$HOME/.config/d2d/save" ] && cp -p "$HOME/.config/d2d/save/"* "$home/d2d/save/" 2>/dev/null
fi
export D2_MPQ_DIR D2_PATCH_INSTALLER
XDG_CONFIG_HOME=$home exec "$D2D_BIN" --data "$D2_MPQ_DIR" --toggle roomstate=on "$@"
