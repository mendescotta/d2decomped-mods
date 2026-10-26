#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The roomstate mod end to end (docs/mods/roomstate.md): games of one
character on one map, a headless d2d driven over devctl.

Usage: roomstate_d2d.py <path-to-d2d>

1. A new character, the mod on: the Blood Moor's monsters but 2 killed,
   then the Den of Evil's but 1 (its quest still open); saved, quit.
2. The same character again: the Blood Moor's 2 are back (the same
   monsters, by type and kind); the Den, its
   quest open, is full again and clearing it still clears the quest
   (World::den_count). Everything left killed, the Den quest done
   (debug quest 1), saved, quit.
3. Again: no population left in either (the Den keeps its superunique),
   the levels still play (warps round the Blood Moor, a save, a quit).
4. Again with the mod off: the Blood Moor full as ever, nothing written.
5. The Golden Bird (another character): act 3's first unique carries the
   jade figurine (World::act3_update), so while the quest's open its
   uniques and their minions come back, the rest kept: the Spider Forest
   killed, the figurine drops; the next game its uniques are back (the
   same roll), its groups not (a fetish camp the map places stands
   there every game), and the figurine drops again; the quest done
   (debug quest 20), the next game's uniques stay down too.
Skips without the MPQs or the 1.14d patch.
"""
import os
import platform
import re
import socket
import subprocess
import sys
import tempfile
import time

d2d = sys.argv[1]
data = os.environ.get("D2_MPQ_DIR") or os.path.expanduser("~/Workspace/private/diablo2")
patch = os.environ.get("D2_PATCH_INSTALLER")
if not os.path.exists(os.path.join(data, "d2data.mpq")) or not patch or not os.path.exists(patch):
    print("SKIP: the MPQs or D2_PATCH_INSTALLER not found")
    sys.exit(0)

BLOOD_MOOR, DEN, SPIDER_FOREST = 2, 8, 76
GOLDEN_BIRD = 0x14
NAME, BIRDER = "Keeper", "Birder"
home = tempfile.mkdtemp(prefix="d2d_roomstate_")
user = os.path.join(home, "Library/Preferences/d2d" if platform.system() == "Darwin" else ".config/d2d")
save_dir = os.path.join(user, "save")
os.makedirs(save_dir)
with open(os.path.join(user, "d2d.cfg"), "w") as f:
    f.write(f"patch = {patch}\n")
store = os.path.join(save_dir, NAME + ".roomstate")
sock_path = os.path.join(tempfile.gettempdir(), f"d2d_roomstate_{os.getpid()}.sock")
env = dict(os.environ, HOME=home)
env.pop("D2_MPQ_DIR", None)


def cmd(line):
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(60)
        s.connect(sock_path)
        s.sendall((line + "\n").encode())
        out = ""
        while not out.endswith("\n") or not out.rsplit("\n", 2)[-2].startswith(("ok", "err")):
            chunk = s.recv(65536)
            assert chunk, f"{line!r}: connection closed after {out!r}"
            out += chunk.decode()
    assert not out.rsplit("\n", 2)[-2].startswith("err"), f"{line!r} -> {out!r}"
    return out


def until(what, test, seconds=60):
    deadline = time.time() + seconds
    while True:
        try:
            if test():
                return
        except (OSError, AssertionError):
            pass
        assert time.time() < deadline, f"timed out: {what}"
        time.sleep(0.2)


def state():
    return dict(re.findall(r"(\w+)=(.*?)(?= \w+=|$)", cmd("state").split("\n")[0]))


def level():
    return int(cmd("debug level").split()[1])


def alive(kinds=None):
    """The level's monsters up: (MonStats id, boss kind) of each, hp over 0."""
    rows = [line.split("\t") for line in cmd("monsters").splitlines() if line and not line.startswith("ok")]
    up = [(row[0], row[7]) for row in rows if len(row) > 7 and not row[5].startswith("0/")]
    return [row for row in up if kinds is None or row[1] in kinds]


def placed(kinds):
    """The monsters up of those kinds, each its MonStats id and cell: a map's presets stand where they're put every game."""
    rows = [line.split("\t") for line in cmd("monsters").splitlines() if line and not line.startswith("ok")]
    return sorted((row[0], row[1], row[2]) for row in rows if len(row) > 7 and not row[5].startswith("0/") and row[7] in kinds)


def go(level_id, tour=None):
    """Onto the level, then round it, the same spots every game (the same rooms up, in the same order):
    `tour` "quarters" its four quarters, "sweep" every 8 cells (a small level's every room)."""
    cmd(f"debug goto {level_id}")
    until(f"arrive on level {level_id}", lambda: level() == level_id)
    time.sleep(1.5)                                  # its rooms by the player come up
    width, height = (int(n) for n in cmd("debug level").split()[4:6])
    if tour == "quarters":
        spots = [(width * i // 4, height * j // 4) for j in (1, 3) for i in (1, 3)]
    elif tour == "sweep":
        spots = [(x, y) for y in range(4, height, 8) for x in range(4, width, 8)]
    else:
        spots = []
    for x, y in spots:
        cmd(f"debug warp {x} {y}")
        time.sleep(1 if tour == "quarters" else 0.25)


class Game:
    def __init__(self, label, toggle="on", new=False, name=NAME):
        self.label = label
        self.log_path = os.path.join(home, f"d2d-{label}.log")
        self.log = open(self.log_path, "w")  # noqa: SIM115 - d2d writes to it while it runs
        args = [d2d, "--headless", "--seed", "3", "--data", data, "--devctl", sock_path, "--no-video", "--toggle", f"roomstate={toggle}"]
        if new:
            args += ["--start-screen", "ingame", "--start-class", "0", "--start-name", name]
        else:
            args += ["--start-screen", "charselect", "--start-name", name]
        self.proc = subprocess.Popen(args, env=env, stdout=self.log, stderr=self.log)
        until("devctl", lambda: cmd("ping"))
        if not new:
            until("character select", lambda: state()["screen"] == "charselect")
            cmd("key Return")
        until("in the game", lambda: state()["screen"] == "ingame")
        if new:                                      # sturdy: the level's monsters round it a while
            cmd("debug statpts 400")
            cmd("cmd stat 3 400")
            until("the vitality", lambda: int(state()["life"].split("/")[1]) > 1000)

    def quit(self):
        cmd("save")
        cmd("quit")
        assert self.proc.wait(timeout=60) == 0, f"game {self.label} exited {self.proc.returncode}"
        self.log.close()

    def text(self):
        with open(self.log_path) as text:
            return text.read()


try:
    # 1: a new character; 2 of the Blood Moor's left, 1 of the Den's.
    game = Game("first", new=True)
    go(BLOOD_MOOR, "quarters")
    first_moor = len(alive())
    assert first_moor > 2, f"the Blood Moor came up with {first_moor} monsters"
    cmd("debug kill 2")
    assert len(alive()) == 2
    survivors = sorted(alive())                       # the same two come back: their types and kinds
    go(DEN, "sweep")
    first_den = alive(["-"])                          # its groups (Corpsefire and his pack are presets: they come back anyway)
    assert len(first_den) > 1, f"the Den came up with {alive()}"
    cmd("debug kill 1")
    game.quit()
    assert os.path.exists(store), "no store written"
    assert "roomstate: on, 0 rooms kept" in game.text()

    # 2: the Blood Moor keeps its 2; the open Den comes back full and still clears its quest.
    game = Game("second")
    go(BLOOD_MOOR, "quarters")
    assert sorted(alive()) == survivors, f"the Blood Moor: {alive()} (left: {survivors})"
    go(DEN, "sweep")
    assert len(alive(["-"])) > 1, f"the Den, its quest open, kept to its 1 left: {alive()}"   # kills' drops step the game seed: not the same roll
    cmd("debug kill")
    until("the Den cleared", lambda: "Den of Evil: cleared" in game.text(), 10)
    cmd("debug quest 1")                              # done (bit 0): the Den's population is kept from here
    go(BLOOD_MOOR, "quarters")
    cmd("debug kill")
    game.quit()

    # 3: none left; both levels still play.
    game = Game("third")
    go(BLOOD_MOOR, "quarters")
    assert alive(["-", "champion", "unique"]) == [], f"the Blood Moor: {alive()}"
    assert state()["screen"] == "ingame"
    go(DEN, "sweep")
    assert alive(["-", "champion", "unique"]) == [], f"the Den, its quest done: {alive()}"
    game.quit()
    text = game.text()
    assert "roomstate: level 2:" in text and "warning: roomstate" not in text

    # 4: the mod off: as ever, and the store left alone.
    before = os.path.getmtime(store)
    game = Game("off", toggle="off")
    go(BLOOD_MOOR, "quarters")
    assert len(alive()) == first_moor, f"the Blood Moor, the mod off: {len(alive())} (first game: {first_moor})"
    game.quit()
    assert os.path.getmtime(store) == before and "roomstate: " not in game.text()

    # 5: the Golden Bird.
    drops = "The Golden Bird: the jade figurine drops"
    game = Game("bird-first", new=True, name=BIRDER)
    go(SPIDER_FOREST, "quarters")
    uniques = len(alive(["unique"]))
    assert uniques > 0 and alive(["-"]), f"the Spider Forest came up with {alive()}"
    cmd("debug kill")
    until("the figurine", lambda: drops in game.text(), 10)
    game.quit()
    game = Game("bird-open", name=BIRDER)
    go(SPIDER_FOREST, "quarters")
    assert len(alive(["unique"])) == uniques, f"the Spider Forest, the bird open: {alive()} ({uniques} uniques the first game)"
    presets = placed(["-", "champion"])
    cmd("debug kill")
    until("the figurine again", lambda: drops in game.text(), 10)
    cmd(f"debug quest {GOLDEN_BIRD}")
    game.quit()
    game = Game("bird-done", name=BIRDER)
    go(SPIDER_FOREST, "quarters")
    assert alive(["unique"]) == [] and placed(["-", "champion"]) == presets, f"the Spider Forest, the bird done: {alive()} (the open game's groups: {presets})"
    game.quit()
    print("roomstate_d2d: PASS")
finally:
    if os.path.exists(sock_path):
        os.unlink(sock_path)
