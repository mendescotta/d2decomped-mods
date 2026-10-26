// SPDX-License-Identifier: GPL-3.0-or-later
// The roomstate mod's store and replay (components/mods/roomstate): the
// file round trip and what it refuses, a batch's rolled population
// swapped for what was kept (leaders renumbered, presets left, a room
// that rolled none, the Golden Bird's fresh kinds), and a session's save.
#include <roomstate.hpp>

#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

using namespace d2d::mods::roomstate;
using d2d::rules::Boss;
using d2d::rules::Spawn;

namespace {

Kept kept(int type, int leader = -1, Boss boss = Boss::none, std::uint16_t life = kFullLife) {
    return { .type = type, .x = 10 * type, .y = 20 * type, .leader = leader, .boss = boss, .mods = boss == Boss::unique ? std::vector<int>{ 5, 17 } : std::vector<int>{},
             .name_seed = type, .seed = std::uint32_t(1000 + type), .life = life };
}

Spawn rolled(int type, int leader, int room, Boss boss = Boss::none) {
    Spawn spawn;
    spawn.type = type;
    spawn.leader = leader;
    spawn.boss = boss;
    spawn.pop_room = room;
    return spawn;
}

Store sample_store() {
    Store store{ .map_seed = 0xabcdef };
    store.rooms[{ 0, 2, 7 }] = { kept(3, 0, Boss::unique, 512), kept(4, 0, Boss::minion), kept(5) };
    store.rooms[{ 2, 8, 0 }] = {};
    return store;
}

void test_store_round_trip_reads_back_equal() {
    const auto store = sample_store();
    const auto back = decode(encode(store));
    assert(back && *back == store);
}

void test_store_damaged_or_other_version_refused() {
    const auto bytes = encode(sample_store());
    auto flipped = bytes;
    flipped[30] ^= std::byte{ 1 };
    assert(!decode(flipped));
    assert(!decode(std::span(bytes).first(bytes.size() - 1)));
    auto other = bytes;
    other[4] = std::byte{ 9 };
    std::uint32_t version = 0;
    assert(!decode(other, &version) && version == 9);
}

void test_replay_swaps_rolled_population_for_kept() {
    // The tail from spawn 100: a preset pack (100, 101), room 3's rolled group (102..104), room 4's rolled one (105).
    const std::vector<Spawn> tail = { rolled(50, 100, -1), rolled(51, 100, -1), rolled(60, 102, 3), rolled(60, 102, 3), rolled(60, 102, 3), rolled(70, 105, 4) };
    const std::vector<Kept> room3 = { kept(3, 0, Boss::unique, 300), kept(4, 0, Boss::minion) };
    const auto out = replay(tail, 100, { { 3, &room3 } });
    assert(out.spawns.size() == 5 && out.life.size() == 5);
    assert(out.spawns[0].type == 50 && out.spawns[0].leader == 100 && out.spawns[1].leader == 100);   // presets as they were
    assert(out.spawns[2].type == 3 && out.spawns[2].leader == 102 && out.spawns[2].boss == Boss::unique && out.life[2] == 300);
    assert(out.spawns[2].x == 30 && out.spawns[2].seed == 1003 && (out.spawns[2].mods == std::vector<int>{ 5, 17 }) && out.spawns[2].pop_room == 3);
    assert(out.spawns[3].type == 4 && out.spawns[3].leader == 102);                                  // its minion follows it
    assert(out.spawns[4].type == 70 && out.spawns[4].leader == 104 && out.life[4] == kFullLife);     // room 4 rolled, renumbered
}

void test_replay_room_that_rolled_none_gets_its_kept() {
    const std::vector<Spawn> tail = { rolled(70, 10, 4) };
    const std::vector<Kept> room3 = { kept(3, 0) };
    const auto out = replay(tail, 10, { { 3, &room3 } });
    assert(out.spawns.size() == 2 && out.spawns[0].type == 70 && out.spawns[1].type == 3 && out.spawns[1].leader == 11);
}

void test_replay_emptied_room_makes_none() {
    const std::vector<Spawn> tail = { rolled(60, 0, 3), rolled(60, 0, 3) };
    const std::vector<Kept> emptied;
    assert(replay(tail, 0, { { 3, &emptied } }).spawns.empty());
}

void test_replay_fresh_kind_rolled_not_kept() {
    // The Golden Bird open: the rolled unique and its minion stay, the kept unique and minion don't, the kept regular does.
    const std::vector<Spawn> tail = { rolled(80, 0, 3, Boss::unique), rolled(81, 0, 3, Boss::minion), rolled(60, 2, 3) };
    const std::vector<Kept> room3 = { kept(3, 0, Boss::unique), kept(4, 0, Boss::minion), kept(5, 2) };
    const auto out = replay(tail, 0, { { 3, &room3 } }, [](Boss boss) { return boss == Boss::unique || boss == Boss::minion; });
    assert(out.spawns.size() == 3);
    assert(out.spawns[0].type == 5 && out.spawns[0].leader == 0);
    assert(out.spawns[1].type == 80 && out.spawns[1].leader == 1 && out.spawns[2].type == 81 && out.spawns[2].leader == 1);
}

void test_session_save_then_next_game_replays() {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "d2d_test_roomstate";
    fs::remove_all(dir);
    const auto file = dir / "Hero.roomstate";
    Session first{ .on = true };
    first.begin(file, 99, 1);
    assert(first.active() && first.rooms_up(2, std::vector<int>{ 0, 1 }).empty());
    assert(first.record({ { { 2, 0 }, { kept(3, 0) } }, { { 2, 1 }, {} } }).empty());
    Session next{ .on = true };
    next.begin(file, 99, 1);
    const auto up = next.rooms_up(2, std::vector<int>{ 0, 1, 5 });
    assert(up.size() == 2 && up.at(0)->size() == 1 && up.at(1)->empty());
    assert(next.rooms_up(2, std::vector<int>{ 0 }).empty());                 // once a game
    Session other_map{ .on = true };
    other_map.begin(file, 100, 1);
    assert(other_map.active() && other_map.store.rooms.empty());
    Session off;
    off.begin(file, 99, 1);
    assert(!off.active() && off.record({}).empty());
    { std::ofstream bad(file, std::ios::binary | std::ios::trunc); bad << "not a store"; }
    Session damaged{ .on = true };
    damaged.begin(file, 99, 1);
    assert(!damaged.active() && fs::file_size(file) == 11);
    std::error_code error;
    fs::remove_all(dir, error);
}

}  // namespace

int main() {
    test_store_round_trip_reads_back_equal();
    test_store_damaged_or_other_version_refused();
    test_replay_swaps_rolled_population_for_kept();
    test_replay_room_that_rolled_none_gets_its_kept();
    test_replay_emptied_room_makes_none();
    test_replay_fresh_kind_rolled_not_kept();
    test_session_save_then_next_game_replays();
}
