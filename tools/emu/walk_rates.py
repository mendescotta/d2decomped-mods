# SPDX-License-Identifier: GPL-3.0-or-later
"""game.exe's walk / run animation rate (FUN_00623f50's moving-mode branch) as the oracle.

    uv run python walk_rates.py > game.txt
    ../../build/tests/test_combat --walk-rates > d2d.txt
    diff game.txt d2d.txt

For a player (unit type 0) in WL (2), RN (3) and TW (6), every walk % from
0 to 400, runs game.exe's own code from 0x624161 to 0x624196: FUN_006213d0's
base x the % (ESI, already at least 25 there) / 100, clamped to 0..0x7fff,
the value FUN_00623f50 stores at unit +0x4c. Prints `mode percent rate`.
The % here skips the at-least-25 clamp at 0x62414e, so 0..24 print as 25.
"""
import emu


def main():
    e = emu.Emu()
    for mode in (2, 3, 6):
        for percent in range(0, 401):
            e.mu.reg_write(emu.UC_X86_REG_EBX, 0)
            e.mu.reg_write(emu.UC_X86_REG_ECX, 0)        # class (unread for a player)
            e.mu.reg_write(emu.UC_X86_REG_EDX, 0)        # unit type: a player
            e.mu.reg_write(emu.UC_X86_REG_EAX, mode)
            e.mu.reg_write(emu.UC_X86_REG_ESI, max(percent, 25))
            e.mu.reg_write(emu.UC_X86_REG_ESP, emu.STACK + emu.STACK_SIZE - 0x1000)
            e.mu.emu_start(0x624163, 0x624196)           # from the call to FUN_006213d0 to the store
            print(mode, percent, e.mu.reg_read(emu.UC_X86_REG_EAX) & 0xffff)


if __name__ == "__main__":
    main()
