// SPDX-License-Identifier: GPL-3.0-or-later
// The roomstate mod (docs/mods/roomstate.md, off unless --toggle
// roomstate=on or d2d.cfg's roomstate = 1): offline, a room's own
// population (FUN_0054ec90's groups, its random bosses and their packs,
// the nests) stays as the player left it, as Diablo's levels did: the
// same monsters, where they stood, with the life they had. A save
// keeps each room's living population in <name>.roomstate beside the
// .d2s; the
// next game makes the room as ever (every seed steps the same), then
// puts those in place of what it rolled. Presets (superuniques, act
// bosses, quest monsters) and what's made later (summons, raises, a
// nest's young) aren't population: they come back as ever.
// This part has no game types: the store, the replay, a game's session.
// roomstate_game.hpp hooks it to the Fight.
#pragma once

#include <monsters.hpp>
#include <uniques.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace d2d::mods::roomstate {

// A monster kept: its spawn as the room made it (type, boss, mods, name
// and unit seeds: the same look, name and drops), where it stood
// (level-relative subtiles), its leader (its place in the room's list,
// -1 none) and its life, of kFullLife.
inline constexpr std::uint16_t kFullLife = 1024;
struct Kept {
    int type = -1, x = 0, y = 0, leader = -1, super = -1;
    d2d::rules::Boss boss = d2d::rules::Boss::none;
    std::vector<int> mods;
    int name_seed = 0;
    std::uint32_t seed = 0;
    std::uint16_t life = kFullLife;
    bool operator==(const Kept&) const = default;
};

// A room: the difficulty, its Levels.txt id, its Level::rooms index.
struct RoomKey {
    std::uint8_t difficulty = 0;
    std::uint16_t level = 0, room = 0;
    auto operator<=>(const RoomKey&) const = default;
};

// A character's rooms, each its living population, for one map seed.
struct Store {
    std::uint32_t map_seed = 0;
    std::map<RoomKey, std::vector<Kept>> rooms;
    bool operator==(const Store&) const = default;
};

// The file, little-endian: "D2RS", version, map seed, the room count;
// each room (difficulty u8, level u16, room u16, its count u16) and its
// monsters (type u16, x i32, y i32, leader i16, super i16, boss u8, the
// mods' count u8 and each u16, name seed i32, unit seed u32, life u16);
// then an FNV-1a of every byte before it.
inline constexpr std::uint32_t kVersion = 1;
[[nodiscard]] std::vector<std::byte> encode(const Store& store);
// none when it isn't a store, or it's damaged; `version` the file's when it's one at all.
[[nodiscard]] std::optional<Store> decode(std::span<const std::byte> bytes, std::uint32_t* version = nullptr);

// `other`: a store of another version (an older d2d's): started over.
enum class Read : std::uint8_t { none, ok, other, damaged };
struct Loaded {
    Read read = Read::none;
    Store store;
};
[[nodiscard]] Loaded load(const std::filesystem::path& file);
// "" when written: a temp file beside it, renamed over it.
[[nodiscard]] std::string save(const std::filesystem::path& file, const Store& store);

// A batch's spawns, the level's from `from` on, with each room of `kept`
// given its kept monsters in place of the population it rolled (where
// the first of that stood, else at the end), leaders renumbered.
// `fresh`: a kind whose rolled monsters stay, its kept ones left out
// (the Golden Bird's uniques). `life`: each spawn's, of kFullLife.
struct Replay {
    std::vector<d2d::rules::Spawn> spawns;
    std::vector<std::uint16_t> life;
};
using Fresh = bool (*)(d2d::rules::Boss);
[[nodiscard]] Replay replay(std::span<const d2d::rules::Spawn> tail, std::size_t from, const std::map<int, const std::vector<Kept>*>& kept, Fresh fresh = nullptr);

// A game's: the store as it loaded, the rooms this game has replayed.
struct Session {
    bool on = false;                                  // the toggle
    std::filesystem::path file;                       // empty: nothing this game (no saves, a damaged store)
    int difficulty = 0;
    Store store;
    std::set<std::pair<int, int>> came_up;            // (level id, room): replayed (or rolled) this game

    // A game starts on `map_seed`; `file` empty: none kept (--no-save, a
    // joined game). A store of another map or version starts over; a
    // damaged one turns the mod off for the game and is left as it is.
    void begin(const std::filesystem::path& file, std::uint32_t map_seed, int difficulty);
    [[nodiscard]] bool active() const { return on && !file.empty(); }
    // Rooms of level `level` new this game: each kept one's monsters
    // (none: it rolls as ever), the rooms marked come up.
    [[nodiscard]] std::map<int, const std::vector<Kept>*> rooms_up(int level, std::span<const int> rooms);
    // A save: the rooms this game brought up, each its living population
    // in place of what was kept; "" when written.
    [[nodiscard]] std::string record(const std::map<std::pair<int, int>, std::vector<Kept>>& alive);
};

}  // namespace d2d::mods::roomstate
