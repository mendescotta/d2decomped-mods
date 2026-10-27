#!/usr/bin/env python3
"""Read a running 1.14d Game.exe room and its level's warp coordinates.

Usage: sudo python3 inspect_level_warps.py PID ROOM_ADDRESS [--game GAME_ADDRESS]
The process is never paused or written. Run only against the isolated test
process; layouts match the pinned 1.14d executable/redivivus revision.
"""

import argparse
import os
import struct


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pid", type=int)
    parser.add_argument("room", type=lambda value: int(value, 0))
    parser.add_argument("--game", type=lambda value: int(value, 0))
    args = parser.parse_args()
    with open(f"/proc/{args.pid}/cmdline", "rb") as command_file:
        command = command_file.read().split(b"\0", 1)[0].lower()
    if not command.endswith(b"game.exe"):
        parser.error(f"PID {args.pid} is not Game.exe: {command!r}")
    with open(f"/proc/{args.pid}/mem", "rb", buffering=0) as memory:
        def u32(address: int) -> int:
            data = os.pread(memory.fileno(), 4, address)
            if len(data) != 4:
                raise RuntimeError(f"short read at 0x{address:08x}")
            return struct.unpack("<I", data)[0]

        def s32(address: int) -> int:
            value = u32(address)
            return value if value < 0x80000000 else value - 0x100000000

        def u16(address: int) -> int:
            data = os.pread(memory.fileno(), 2, address)
            if len(data) != 2:
                raise RuntimeError(f"short read at 0x{address:08x}")
            return struct.unpack("<H", data)[0]

        room_ex = u32(args.room + 0x10)
        level = u32(room_ex + 0x58)
        level_id = s32(level + 0x1D0)
        if not 1 <= level_id <= 200:
            raise RuntimeError(f"implausible level ID {level_id}; check room address/version")
        print(f"room=0x{args.room:08x} room_ex=0x{room_ex:08x} "
              f"level=0x{level:08x} id={level_id}")
        print(f"room subtile origin=({s32(args.room+0x4c)}, {s32(args.room+0x50)})")
        print(f"level tile origin=({s32(level+0x1c)}, {s32(level+0x20)})")
        count = s32(level + 0x228)
        player_xy = None
        if args.game is not None:
            client = u32(args.game + 0x88)
            player = u32(client + 0x174)
            if u32(player) != 0:
                raise RuntimeError("client player is not a player unit")
            path = u32(player + 0x2c)
            player_xy = (u16(path + 2), u16(path + 6))
            print(f"player=0x{player:08x} path=0x{path:08x} "
                  f"world subtile=({player_xy[0]}, {player_xy[1]})")
        print(f"warp-coordinate entries={count}")
        if not 0 <= count <= 9:
            raise RuntimeError("implausible warp count; check layout/version")
        for index in range(count):
            x = s32(level + 0x1E0 + 4 * index)
            y = s32(level + 0x204 + 4 * index)
            line = f"  {index}: world subtile=({x}, {y})"
            if player_xy is not None:
                dx, dy = x - player_xy[0], y - player_xy[1]
                line += f" delta=({dx}, {dy}) projected screen delta≈({16*(dx-dy)}, {8*(dx+dy)})"
            print(line)


if __name__ == "__main__":
    main()
