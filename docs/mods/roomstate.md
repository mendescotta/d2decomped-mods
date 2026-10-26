# Mod: roomstate — each room's monsters kept as you left them, offline

Status: testing (the `mods-roomstate` branch). Off unless turned on:
`--toggle roomstate=on`, or `roomstate = 1` in `d2d.cfg`.

game.exe fills every area again each time a game starts. Diablo kept its
levels as the player left them. With this mod, each room's own
population is kept monster by monster: the ones you killed stay dead,
and the ones you left come back as the same monsters, where they stood,
with the life they had. Each keeps its type, its boss mods and name,
its look, and its drops (its unit seed).

## What stays, what comes back

**Kept (the room's population).** What `ObjectRooms::populate()`
(FUN_0054ec90) makes for a room: its monster groups, its random champion
and unique packs (minions too), its nests.

**Back every game, as ever.**
- A room's presets: superuniques (Bishibosh, Pindleskin...), act bosses,
  quest monsters, the map's own placed monsters (Corpsefire, a fetish
  camp).
- Whatever the game makes later: summons, raises, shamans' revives, a
  nest's young, traps, Baal's waves, the quests' scripted monsters.

**Quests.** Only two count ordinary population; while they're open their
monsters come as ever:
- The Den of Evil (quest 1) is done when the Den's monsters are all dead
  (`World::den_count`). While it's open (bits 0 and 15 clear), the Den
  rolls as ever. Once done it's kept like anywhere else.
- The Golden Bird (quest 0x14): the first qualifying unique act 3 makes,
  including one in a nearby level, carries the jade figurine. `Fight`
  records boss creation order for the quest hook. While it's open (bits 0 and 1
  clear), act 3's uniques and their minions are the ones the room rolls,
  not the kept ones.

**Off** for a joined game, local co-op with more than one player,
`--no-save`, and a store it can't read (warned, left as it is).

## How

1. `room_objects` tags each spawn `populate()` makes with its room
   (`Spawn::pop_room`, copied to `Monster::pop_room`).
2. A save (`World::save`, the .d2s written): for every room this game
   brought up, its living population, in spawn order, goes into
   `<name>.roomstate` beside the .d2s, per difficulty, level and room, for
   the map seed. Each monster keeps: type, cell, leader (its place in the
   room's list), SuperUniques row, boss kind, mods, name seed, unit seed,
   and life (of 1024). A temp file, then a rename.
3. The next game makes each room as ever (every seed steps the same).
   Then, before the batch's monsters are made, `Fight::rooms_up` rebuilds
   the batch's tail of the level's spawn list. Each kept room's rolled
   population gives way to its kept monsters, where the first of the
   rolled ones stood, else at the end. Leaders are renumbered, so the
   pack indices `Monster::leader` reads hold. Nothing hidden is made: the
   dead aren't there. A survivor whose leader was dead comes with none
   (-1), as game.exe's absent leader: a fallen shaman's rally then sends
   nothing (FUN_0058f730), and a tentacle without its head dies.
4. Each monster made gets its spawn's index (`Monster::spawn_at`, what a
   save keeps it by) and its kept life. A spot since shut (a door) is
   swapped for the nearest free one.

A store of another map seed or file version starts over.

## Files

- `components/mods/roomstate/roomstate.{hpp,cpp}`: the store, the
  replay, a game's session; no game types.
- `components/mods/roomstate/roomstate_game.hpp`: the hooks on the
  game's types.
- Hook points: `rules::Spawn` (`pop_room`), `Monster` (`pop_room`,
  `spawn_at`), `room_objects`, `spawn_monsters`, `Fight::rooms_up`,
  `World::new_game`, `World::save`, `main.cpp`'s toggle.
- Tests: `tests/test_roomstate.cpp` (the store, the replay, a session)
  and `tests/roomstate_d2d.py` (games of one character over devctl: the
  same monsters kept, a level emptied, the Den, the Golden Bird, the mod
  off).
