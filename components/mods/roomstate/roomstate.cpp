// SPDX-License-Identifier: GPL-3.0-or-later
#include "roomstate.hpp"

#include <log.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <ios>
#include <iterator>
#include <system_error>

namespace d2d::mods::roomstate {

namespace {

constexpr std::array<std::byte, 4> kMagic{ std::byte{ 'D' }, std::byte{ '2' }, std::byte{ 'R' }, std::byte{ 'S' } };
constexpr std::size_t kHeader = 16, kChecksum = 4;

std::uint32_t fnv1a(std::span<const std::byte> bytes) {
    std::uint32_t hash = 2166136261u;
    for (const auto byte : bytes) hash = (hash ^ std::uint32_t(byte)) * 16777619u;
    return hash;
}

void put(std::vector<std::byte>& out, std::uint32_t value, int size) {
    for (int i = 0; i < size; ++i) out.push_back(std::byte(value >> (8 * i) & 0xff));
}

// Reads the body in order; `bad` once a read runs past its end.
struct Reader {
    std::span<const std::byte> bytes;
    std::size_t at = 0;
    bool bad = false;
    std::uint32_t get(int size) {
        if (bad || at + std::size_t(size) > bytes.size()) { bad = true; return 0; }
        std::uint32_t value = 0;
        for (int i = 0; i < size; ++i) value |= std::uint32_t(bytes[at + std::size_t(i)]) << (8 * i);
        at += std::size_t(size);
        return value;
    }
    int get_signed(int size) {
        const auto value = get(size);
        const auto sign = std::uint32_t(1) << (8 * size - 1);
        return size == 4 ? int(value) : int(value ^ sign) - int(sign);
    }
};

}  // namespace

std::vector<std::byte> encode(const Store& store) {
    std::vector<std::byte> out(kMagic.begin(), kMagic.end());
    put(out, kVersion, 4);
    put(out, store.map_seed, 4);
    put(out, std::uint32_t(store.rooms.size()), 4);
    for (const auto& [key, monsters] : store.rooms) {
        put(out, key.difficulty, 1);
        put(out, key.level, 2);
        put(out, key.room, 2);
        put(out, std::uint32_t(monsters.size()), 2);
        for (const auto& monster : monsters) {
            put(out, std::uint32_t(monster.type), 2);
            put(out, std::uint32_t(monster.x), 4);
            put(out, std::uint32_t(monster.y), 4);
            put(out, std::uint32_t(monster.leader), 2);
            put(out, std::uint32_t(monster.super), 2);
            put(out, std::uint32_t(monster.boss), 1);
            put(out, std::uint32_t(monster.mods.size()), 1);
            for (const int mod : monster.mods) put(out, std::uint32_t(mod), 2);
            put(out, std::uint32_t(monster.name_seed), 4);
            put(out, monster.seed, 4);
            put(out, monster.life, 2);
        }
    }
    put(out, fnv1a(out), 4);
    return out;
}

std::optional<Store> decode(std::span<const std::byte> bytes, std::uint32_t* version) {
    if (bytes.size() < kHeader + kChecksum || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) return std::nullopt;
    Reader head{ bytes, 4 };
    const auto file_version = head.get(4);
    if (version) *version = file_version;
    if (file_version != kVersion) return std::nullopt;
    const auto body = bytes.first(bytes.size() - kChecksum);
    if (Reader{ bytes, bytes.size() - kChecksum }.get(4) != fnv1a(body)) return std::nullopt;
    Reader in{ body, 8 };
    Store store{ .map_seed = in.get(4) };
    const auto rooms = in.get(4);
    for (std::uint32_t room = 0; room < rooms && !in.bad; ++room) {
        RoomKey key;
        key.difficulty = std::uint8_t(in.get(1));
        key.level = std::uint16_t(in.get(2));
        key.room = std::uint16_t(in.get(2));
        std::vector<Kept> monsters(in.get(2));
        for (auto& monster : monsters) {
            monster.type = int(in.get(2));
            monster.x = in.get_signed(4);
            monster.y = in.get_signed(4);
            monster.leader = in.get_signed(2);
            monster.super = in.get_signed(2);
            monster.boss = d2d::rules::Boss(in.get(1));
            monster.mods.resize(in.get(1));
            for (auto& mod : monster.mods) mod = int(in.get(2));
            monster.name_seed = in.get_signed(4);
            monster.seed = in.get(4);
            monster.life = std::uint16_t(in.get(2));
            if (monster.boss > d2d::rules::Boss::minion || monster.leader >= int(monsters.size()) || monster.life == 0 || monster.life > kFullLife) in.bad = true;
        }
        if (!store.rooms.emplace(key, std::move(monsters)).second) in.bad = true;   // a room twice: not one encode() wrote
    }
    if (in.bad || in.at != body.size()) return std::nullopt;
    return store;
}

Loaded load(const std::filesystem::path& file) {
    std::error_code error;
    if (!std::filesystem::exists(file, error)) return { Read::none, {} };
    std::ifstream in(file, std::ios::binary);
    if (!in) return { Read::damaged, {} };
    std::vector<char> raw{ std::istreambuf_iterator<char>(in), {} };
    std::uint32_t version = 0;
    auto store = decode(std::as_bytes(std::span(raw)), &version);
    if (store) return { Read::ok, std::move(*store) };
    return { version && version != kVersion ? Read::other : Read::damaged, {} };
}

std::string save(const std::filesystem::path& file, const Store& store) {
    namespace fs = std::filesystem;
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    auto tmp = file;
    tmp += ".tmp";
    const auto bytes = encode(store);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!out.flush()) return "couldn't write " + tmp.string();
    }
    fs::rename(tmp, file, error);
    return error ? "couldn't replace " + file.string() + ": " + error.message() : std::string{};
}

Replay replay(std::span<const d2d::rules::Spawn> tail, std::size_t from, const std::map<int, const std::vector<Kept>*>& kept, Fresh fresh) {
    Replay out;
    std::vector<int> moved(tail.size(), -1);          // a rolled spawn's new index
    std::vector<bool> rolled;                         // out.spawns' own: a rolled spawn (its leader renumbered after)
    std::set<int> placed;
    const auto put_kept = [&](int room) {
        const auto& monsters = *kept.at(room);
        std::vector<int> index_of(monsters.size(), -1);
        for (std::size_t i = 0; i < monsters.size(); ++i) {
            const auto& monster = monsters[i];
            if (fresh && fresh(monster.boss)) continue;
            const int at = int(from + out.spawns.size());
            index_of[i] = at;
            const int leader = monster.leader == int(i) ? at : monster.leader >= 0 && monster.leader < int(i) ? index_of[std::size_t(monster.leader)] : -1;
            out.spawns.push_back({ monster.type, monster.x, monster.y, leader, monster.super, monster.boss, monster.mods, monster.name_seed, monster.seed, {}, false, room });
            out.life.push_back(monster.life);
            rolled.push_back(false);
        }
        placed.insert(room);
    };
    for (std::size_t i = 0; i < tail.size(); ++i) {
        const auto& spawn = tail[i];
        if (spawn.pop_room >= 0 && kept.contains(spawn.pop_room)) {
            if (!placed.contains(spawn.pop_room)) put_kept(spawn.pop_room);
            if (!fresh || !fresh(spawn.boss)) continue;
        }
        moved[i] = int(from + out.spawns.size());
        out.spawns.push_back(spawn);
        out.life.push_back(kFullLife);
        rolled.push_back(true);
    }
    for (const auto& [room, monsters] : kept)
        if (!placed.contains(room)) put_kept(room);
    for (std::size_t i = 0; i < out.spawns.size(); ++i)
        if (auto& leader = out.spawns[i].leader; rolled[i] && leader >= int(from))
            leader = std::size_t(leader) - from < moved.size() ? moved[std::size_t(leader) - from] : -1;
    return out;
}

void Session::begin(const std::filesystem::path& to, std::uint32_t map_seed, int game_difficulty) {
    file.clear();
    came_up.clear();
    store = { .map_seed = map_seed };
    difficulty = game_difficulty;
    if (!on || to.empty()) return;
    auto loaded = load(to);
    if (loaded.read == Read::damaged) {
        d2d::log::warn("roomstate: {} isn't a store this d2d reads; monsters respawn this game and it's left as it is", to.string());
        return;
    }
    file = to;
    if (loaded.read == Read::ok && loaded.store.map_seed == map_seed) store = std::move(loaded.store);
    else if (loaded.read == Read::ok) d2d::log::info("roomstate: {} is of another map, starting over", to.string());
    else if (loaded.read == Read::other) d2d::log::info("roomstate: {} is of another version, starting over", to.string());
    d2d::log::info("roomstate: on, {} rooms kept", store.rooms.size());
}

std::map<int, const std::vector<Kept>*> Session::rooms_up(int level, std::span<const int> rooms) {
    std::map<int, const std::vector<Kept>*> out;
    for (const int room : rooms) {
        if (!came_up.insert({ level, room }).second) continue;
        if (const auto found = store.rooms.find({ std::uint8_t(difficulty), std::uint16_t(level), std::uint16_t(room) }); found != store.rooms.end())
            out.emplace(room, &found->second);
    }
    return out;
}

std::string Session::record(const std::map<std::pair<int, int>, std::vector<Kept>>& alive) {
    if (!active()) return {};
    for (const auto& [where, monsters] : alive) store.rooms[{ std::uint8_t(difficulty), std::uint16_t(where.first), std::uint16_t(where.second) }] = monsters;
    auto why = save(file, store);
    if (why.empty()) d2d::log::info("roomstate: {} rooms kept", store.rooms.size());
    return why;
}

}  // namespace d2d::mods::roomstate
