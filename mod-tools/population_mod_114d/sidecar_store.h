#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace d2population {

struct RoomKey {
    std::string character_identity;
    std::uint8_t difficulty = 0;
    std::uint32_t map_seed = 0;
    std::uint32_t level_id = 0;
    std::int32_t room_x = 0;
    std::int32_t room_y = 0;

    bool operator<(const RoomKey& other) const noexcept;
};

struct RoomBudget {
    std::uint32_t initial_ordinary_count = 0;
    std::uint32_t living_budget = 0;
};

// Stores population state in a separate sidecar file. It never opens or edits
// character save files. Load reports absent files as an empty store; malformed
// or unsupported files fail with a descriptive error and leave state unchanged.
class SidecarStore {
public:
    bool Load(const std::string& path, std::string* error = nullptr);
    bool Save(const std::string& path, std::string* error = nullptr) const;

    bool Get(const RoomKey& key, RoomBudget* value) const;
    void Set(const RoomKey& key, RoomBudget value);
    bool DecrementLiving(const RoomKey& key);
    std::size_t Size() const;

private:
    mutable std::mutex mutex_;
    std::map<RoomKey, RoomBudget> rooms_;
};

} // namespace d2population
