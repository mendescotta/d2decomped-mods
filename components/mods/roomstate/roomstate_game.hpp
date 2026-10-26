// SPDX-License-Identifier: GPL-3.0-or-later
// The roomstate mod's hooks (roomstate.hpp) on the game's types: a batch
// of rooms replayed as the Fight makes its monsters, the living kept as
// the World saves. For the game's sources only.
#pragma once

#include "roomstate.hpp"

#include <fight.hpp>
#include <gamedata.hpp>

#include <monsters.hpp>
#include <quests.hpp>
#include <quests_act3.hpp>
#include <shrines.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace d2d::mods::roomstate {

// Fight::rooms_up's batch, before its monsters are made: the level's
// spawns from `from` on, its rooms new this game replayed; each spawn's
// life (empty: as made). Off in local co-op (who the rooms are kept for
// isn't one player). Two quests count the population; while they're
// open it comes as ever: the Den of Evil's monsters left
// (World::den_count), and the Golden Bird's carrier, the first unique act
// 3 makes (World::act3_update): act 3's uniques and minions as rolled.
[[nodiscard]] inline std::vector<std::uint16_t> replay(Session& session, const d2d::game::Level& level, d2d::game::Spawning::LevelState& state,
                                                       std::size_t from, const d2d::rules::QuestBits& quests, std::size_t heroes) {
    using d2d::rules::quest::bit;
    using Den = d2d::rules::DenQuest;
    using Bird = d2d::rules::GoldenBirdQuest;
    if (!session.active() || heroes > 1 || from > state.spawns.size()) return {};
    std::vector<int> rooms;
    for (const auto room : state.order) rooms.push_back(int(room));
    const auto kept = session.rooms_up(level.id, rooms);
    const bool den_open = level.id == Den::kDen && !bit(quests, Den::kQuest, 0) && !bit(quests, Den::kQuest, 15);
    if (kept.empty() || den_open) return {};
    const bool bird_open = d2d::rules::level_act(level.id) == 2 && !bit(quests, Bird::kQuest, 0) && !bit(quests, Bird::kQuest, 1);
    const Fresh bird = [](d2d::rules::Boss boss) { return boss == d2d::rules::Boss::unique || boss == d2d::rules::Boss::minion; };
    auto replayed = roomstate::replay(std::span(state.spawns).subspan(from), from, kept, bird_open ? bird : nullptr);
    for (auto& spawn : replayed.spawns)            // a spot since shut (a door, a barrel): the nearest free one
        if (spawn.pop_room >= 0 && level.unit_blocked((float(spawn.x) + 0.5f) / 5, (float(spawn.y) + 0.5f) / 5)) {
            const auto [free_x, free_y] = level.nearest_free((float(spawn.x) + 0.5f) / 5, (float(spawn.y) + 0.5f) / 5);
            spawn.x = int(std::floor(free_x * 5));
            spawn.y = int(std::floor(free_y * 5));
        }
    std::size_t rolled = 0, back = 0;
    for (const auto& [room, monsters] : kept) back += monsters->size();
    for (const auto& spawn : std::span(state.spawns).subspan(from)) rolled += spawn.pop_room >= 0 && kept.contains(spawn.pop_room);
    d2d::log::info("roomstate: level {}: {} room(s) kept, {} monster(s) back for the {} they rolled", level.id, kept.size(), back, rolled);
    state.spawns.resize(from);
    state.spawns.insert(state.spawns.end(), replayed.spawns.begin(), replayed.spawns.end());
    return replayed.life;
}

// The batch made from `spawns` (the level's from `from` on): each monster
// its spawn's index (what a save keeps it by; spawn_monsters skips a type
// it can't make) and the life it was kept with.
inline void made(std::span<d2d::game::Monster> made, std::span<const d2d::rules::Spawn> spawns, std::size_t from, std::span<const std::uint16_t> life) {
    std::size_t at = 0;
    for (auto& monster : made) {
        while (at < spawns.size() && spawns[at].type != monster.type) ++at;
        if (at == spawns.size()) return;
        monster.spawn_at = int(from + at);
        if (at < life.size() && life[at] < kFullLife)
            monster.hit_points = std::max(1, int(std::int64_t(monster.stats.hit_points) * life[at] / kFullLife));
        ++at;
    }
}

// Every room this game brought up (towns aside), each its living
// population in spawn order: the level the player's on and those kept.
[[nodiscard]] inline std::map<std::pair<int, int>, std::vector<Kept>> alive(const d2d::game::Fight& fight) {
    std::map<std::pair<int, int>, std::vector<std::pair<int, const d2d::game::Monster*>>> found;
    for (const auto& [level, state] : fight.spawning.levels) {
        if (!level || d2d::game::in_town(level)) continue;
        for (const auto room : state.order) found.try_emplace({ level->id, int(room) });
    }
    const auto gather = [&](const d2d::game::Level* level, const std::vector<d2d::game::Monster>& monsters) {
        if (!level) return;
        for (const auto& monster : monsters)
            if (monster.pop_room >= 0 && monster.spawn_at >= 0 && monster.alive())
                if (const auto room = found.find({ level->id, monster.pop_room }); room != found.end()) room->second.emplace_back(monster.spawn_at, &monster);
    };
    gather(fight.mon_level, fight.monsters);
    for (const auto& [level, monsters] : fight.kept) gather(level, monsters);
    std::map<int, const d2d::game::Spawning::LevelState*> states;
    for (const auto& [level, state] : fight.spawning.levels) if (level) states[level->id] = &state;
    std::map<std::pair<int, int>, std::vector<Kept>> out;
    for (auto& [where, monsters] : found) {
        auto& list = out[where];
        const auto* state = states.at(where.first);
        std::ranges::sort(monsters, {}, &std::pair<int, const d2d::game::Monster*>::first);
        std::map<int, int> index_of;
        for (const auto& [at, monster] : monsters) {
            if (std::size_t(at) >= state->spawns.size()) continue;
            const auto& spawn = state->spawns[std::size_t(at)];
            const auto leader = index_of.find(spawn.leader);
            const int self = int(list.size());
            index_of[at] = self;
            const int max = std::max(monster->stats.hit_points, 1);
            list.push_back({ .type = spawn.type, .x = int(std::floor(monster->unit.x * 5)), .y = int(std::floor(monster->unit.y * 5)),
                             .leader = spawn.leader == at ? self : leader != index_of.end() ? leader->second : -1, .super = spawn.super, .boss = spawn.boss,
                             .mods = spawn.mods, .name_seed = spawn.name_seed, .seed = spawn.seed,
                             .life = std::uint16_t(std::clamp<std::int64_t>((std::int64_t(monster->hit_points) * kFullLife + max - 1) / max, 1, kFullLife)) });
        }
    }
    return out;
}

// World::save's, the .d2s written: "" when kept (or nothing to keep).
[[nodiscard]] inline std::string saved(Session& session, const d2d::game::Fight& fight) {
    if (!session.active() || fight.heroes.size() > 1 || fight.remote_monsters) return {};
    return session.record(alive(fight));
}

}  // namespace d2d::mods::roomstate
