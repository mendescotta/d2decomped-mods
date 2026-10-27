# d2decomped-mods

Gameplay mods for [d2decomped](https://github.com/psi29a/d2decomped), kept
apart from the upstream work. The repo carries d2decomped's history, so a
mod rebases onto upstream like any branch. Every mod is off by default and
switched on per game (`--toggle <name>=on`, or `<name> = 1` in `d2d.cfg`).

## Branches

| Branch | What it is |
|---|---|
| `main` | The **roomstate** mod on d2decomped: each room's monsters come back as they were left (same units, place, life, mods, name and drops; the dead stay dead). Code in `components/mods/roomstate/`, doc in `docs/mods/roomstate.md`, tests `test_roomstate` and `roomstate_d2d`. |
| `norespawn-mod` | A play build: `main` merged with d2decomped fixes that were still in review when it was made. |
| `archive/norespawn` | Archived: the first mod, which kept each room's monster **count** instead of the monsters themselves. Superseded by roomstate. |

Bring a branch up to date with `git fetch upstream && git merge upstream/main`
(upstream: `https://github.com/psi29a/d2decomped`).

## mod-tools

- `play-d2-roomstate.sh`: plays with roomstate on, with its own saves and settings (see its header).
- `d2d_monster_probe.py`: dumps a level's monsters from a headless d2d, game after game, to compare them (`--toggle`, `--kill`).
- `population_mod_114d/`: the first approach, on the original game: a read-only trace DLL and launcher for 1.14d under Wine, and a tested sidecar store (its README).
