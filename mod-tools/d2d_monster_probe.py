#!/usr/bin/env python3
"""Dump a level's monsters from a headless d2d, game after game, to compare them.

Usage: d2d_monster_probe.py <d2d> <level-id> <games> [--toggle norespawn=on] [--kill]

Each game reopens one character (made in the first) in a throwaway HOME,
goes to <level-id> by `debug goto`, tours its four quarters (the same
spots each game), then prints every monster up: MonStats id, cell, boss
kind, unit id. --kill kills them all before the save that ends each game.
Needs --data and --patch, or D2_MPQ_DIR and D2_PATCH_INSTALLER.
Set D2D_PROBE_KEEP_LOGS=1 to retain the isolated game logs.
"""
import os
import argparse
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("d2d")
parser.add_argument("level_id", type=int)
parser.add_argument("games", type=int)
parser.add_argument("--toggle", default="norespawn=off")
parser.add_argument("--kill", action="store_true")
parser.add_argument("--data", default=os.environ.get("D2_MPQ_DIR"))
parser.add_argument("--patch", default=os.environ.get("D2_PATCH_INSTALLER"))
args = parser.parse_args()
if args.games < 1 or not args.data or not os.path.isfile(os.path.join(args.data, "d2data.mpq")) or not args.patch or not os.path.isfile(args.patch):
    parser.error("need GAMES >= 1, --data with d2data.mpq, and --patch")
d2d, level_id, games = args.d2d, args.level_id, args.games
toggle, kill = args.toggle, args.kill
data, patch = args.data, args.patch
home = tempfile.mkdtemp(prefix="d2d_probe_")
user = os.path.join(home, ".config/d2d")
os.makedirs(os.path.join(user, "save"))
with open(os.path.join(user, "d2d.cfg"), "w") as f:
    f.write(f"patch = {patch}\n")
sock = os.path.join(tempfile.gettempdir(), f"d2d_probe_{os.getpid()}.sock")
env = dict(os.environ, HOME=home)
env.pop("D2_MPQ_DIR", None)


def cmd(line):
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(60)
        s.connect(sock)
        s.sendall((line + "\n").encode())
        out = ""
        while not out.endswith("\n") or not out.rsplit("\n", 2)[-2].startswith(("ok", "err")):
            chunk = s.recv(65536)
            if not chunk:
                raise OSError("closed")
            out += chunk.decode()
    if out.rsplit("\n", 2)[-2].startswith("err"):
        raise RuntimeError(f"{line!r}: {out.strip()}")
    return out


def until(test, seconds=60):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            if test():
                return
        except OSError:
            pass
        time.sleep(0.2)
    sys.exit("timed out")


def screen():
    return re.search(r"screen=(\w+)", cmd("state")).group(1)


completed = False
try:
    for game in range(games):
        command = [d2d, "--headless", "--seed", "3", "--data", data, "--devctl", sock, "--no-video", "--toggle", toggle]
        command += ["--start-screen", "ingame", "--start-class", "0", "--start-name", "Probe"] if game == 0 else ["--start-screen", "charselect", "--start-name", "Probe"]
        with open(os.path.join(home, f"game{game}.log"), "w") as log:
            proc = subprocess.Popen(command, env=env, stdout=log, stderr=log)
            print(f"game {game}: pid {proc.pid}")
            try:
                until(lambda: cmd("ping"))
                if game:
                    until(lambda: screen() == "charselect")
                    cmd("key Return")
                until(lambda: screen() == "ingame")
                if game == 0:
                    cmd("debug statpts 400")
                    cmd("cmd stat 3 400")
                cmd(f"debug goto {level_id}")
                until(lambda: int(cmd("debug level").split()[1]) == level_id)
                time.sleep(1.5)
                width, height = (int(n) for n in cmd("debug level").split()[4:6])
                for x, y in [(width * i // 4, height * j // 4) for j in (1, 3) for i in (1, 3)]:
                    cmd(f"debug warp {x} {y}")
                    time.sleep(1)
                print(f"== game {game} ({toggle})")
                for line in cmd("monsters").splitlines():
                    row = line.split("\t")
                    if len(row) > 12 and not row[5].startswith("0/"):
                        print(f"  {row[0]:16} ({float(row[1]):6.1f},{float(row[2]):6.1f}) {row[7]:11} {row[12]}")
                if kill:
                    cmd("debug kill")
                cmd("save")
                cmd("quit")
                if proc.wait(timeout=60) != 0:
                    raise RuntimeError(f"game {game} exited {proc.returncode}")
            finally:
                if proc.poll() is None:
                    try:
                        cmd("quit")
                        proc.wait(timeout=5)
                    except (OSError, RuntimeError, subprocess.TimeoutExpired):
                        if proc.poll() is None:
                            proc.terminate()
                            try:
                                proc.wait(timeout=5)
                            except subprocess.TimeoutExpired:
                                proc.kill()
                                proc.wait(timeout=5)
                if os.path.exists(sock):
                    os.unlink(sock)
    completed = True
finally:
    if os.path.exists(sock):
        os.unlink(sock)
    if completed and os.environ.get("D2D_PROBE_KEEP_LOGS") != "1":
        shutil.rmtree(home)
    else:
        print(f"logs: {home}", file=sys.stderr)
