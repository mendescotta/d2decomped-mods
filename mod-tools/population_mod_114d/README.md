# Diablo II population mod build tools

This directory holds a **read-only tracing build** for official
Diablo II classic/LoD 1.14d. It does not yet change monster counts or saves.
Its purpose is to verify the exact game binary, test Windows/Wine loading,
and observe calls to the ordinary room population function before adding
stateful behavior.

## Build

Requires Void Linux packages `cross-i686-w64-mingw32`,
`cross-i686-w64-mingw32-crt`, and `cmake` (the Makefile itself only needs `make`
and the cross compiler). Run `make` here. Outputs are
`build/population_launcher.exe` and `build/population_trace.dll`.

The reusable population state implementation is in
[`sidecar_store.h`](sidecar_store.h) and [`sidecar_store.cpp`](sidecar_store.cpp).
It stores room snapshots by character identity, difficulty, stable map seed,
level ID, and room coordinates. Each snapshot retains the initial ordinary
monster count and the current living budget. The binary format is versioned,
checksummed, validates input before changing in-memory state, and saves through
a same-directory temporary file followed by atomic replacement. Store it next
to the mod data, never over a `.d2s` character save. Hook code can use `Load`,
`Get`, `Set`, `DecrementLiving`, and `Save`; a missing sidecar loads as an empty
store. Corrupt or unsupported data makes `Load` fail and leaves the current
store unchanged.

Run the native Linux serialization and corruption checks with `make test-native`.
Build the Windows version with `make build/sidecar_store_test.exe`, then run it
in an isolated Wine prefix to verify Windows file replacement and parsing:

```sh
WINEPREFIX="$HOME/Games/diablo-ii-nospawn-test" wine \
  mod-tools/population_mod_114d/build/sidecar_store_test.exe
```

Both test variants passed on this machine on 2026-10-24.

For a live *read-only* look at a level's warp-room centers during isolated
Wine testing, use [`inspect_level_warps.py`](inspect_level_warps.py). It reads
the pinned 1.14d room/level/player layouts from `/proc/PID/mem`; root access
is needed for a Wine process started by another shell. Example:

```sh
sudo python3 mod-tools/population_mod_114d/inspect_level_warps.py \
  PID ROOM_POINTER --game GAME_POINTER
```

The tracer log supplies the room and game pointers. The script validates the
process command and plausible level/warp counts, then prints the player-to-warp
delta. It does not write to the game process. Room pointers can become stale
after unloading; use one from a recently populated room.

The launcher starts `Game.exe`, waits for its menu, loads the DLL into that
new process, calls its `StartTrace` export, and closes the new process if
initialization fails. The DLL checks the full SHA-256 of the installed test
`Game.exe` and six original bytes at each hook before patching memory. It
observes the first 100 room population calls, 200 monster unit creation calls,
and 200 monster mode count changes in `population_trace.log` next to the DLL.
Room logs include level, coordinates, monster count before and after, plus
character identity fields from the live game. Unit creation logs mark whether
the call stack passed through the ordinary group helper. The group marker is
diagnostic only. All hooked calls continue to the original functions.

Windows example, with both build files in one directory:

```text
population_launcher.exe "C:\Games\Diablo II\Game.exe" -3dfx
```

Wine example, using a **copy** of the game prefix and installation:

```sh
export WINEPREFIX="$HOME/Games/diablo-ii-nospawn-test"
wine mod-tools/population_mod_114d/build/population_launcher.exe \
  'C:\Program Files (x86)\Diablo II\Game.exe' -3dfx
```

The test prefix is a full copy of the game's own Wine prefix; original
files and saves are not touched. The trace build is intentionally limited to
the exact game executable SHA-256
`631066c1649c4ea9ffe48bf97e24c00bca1f7a6759c21150f1a79982589adaaf`.
The hook RVA `0x14ec90` was checked against its x86 prologue in that binary
and the independently traced 1.14d room population address
[`0x54ec90`](https://github.com/psi29a/d2decomped/blob/main/docs/research/re/monsters.md#room-population--fun_0054ec90).
The other observed functions are the server unit maker at VA `0x555230`
(RVA `0x155230`) and the monster region mode count updater at VA `0x547dd0`
(RVA `0x147dd0`). The [1.14d reverse engineering notes](https://github.com/psi29a/d2decomped/blob/main/docs/research/re/monsters.md#when-a-room-populates)
identify the unit maker, and the [Den of Evil notes](https://github.com/psi29a/d2decomped/blob/main/docs/research/re/quests.md#the-quest-record)
identify the mode count functions. Their exact prologues and calling
conventions were checked in the local official executable with `objdump`.

## Review and current limits

The nearby `research/d2-no-respawn/upstream/d2inject` was inspected as a
possible tool. It installs unrelated maphack patches, has no executable hash
guard, does its startup work under `DllMain`, and its patch helper does not
flush the instruction cache. It was **not run or copied**. This build uses
only the observed room hook and guards it with both hash and byte checks.

The tracing hooks still change process code at runtime. Each 6-byte trampoline
is safe only because its exact six bytes were checked and contain complete
x86 instructions without relative addressing. Hook installation is timed for
the game menu, before entering a world. The launcher and DLL must be tested
under both Windows and Wine before population suppression is added.
