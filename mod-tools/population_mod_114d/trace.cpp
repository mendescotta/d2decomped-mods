#include <windows.h>
#include <wincrypt.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {

constexpr uintptr_t kSpawnRva = 0x14ec90;
constexpr unsigned char kExpectedPrologue[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x38};
constexpr uintptr_t kCreateUnitRva = 0x155230;
constexpr unsigned char kExpectedCreatePrologue[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x08};
constexpr uintptr_t kModeCountRva = 0x147dd0;
constexpr unsigned char kExpectedModeCountPrologue[] = {0x55, 0x8b, 0xec, 0x56, 0x8b, 0xf2};
constexpr char kExpectedGameSha256[] =
    "631066c1649c4ea9ffe48bf97e24c00bca1f7a6759c21150f1a79982589adaaf";

using SpawnRoomFn = void(__fastcall *)(void *, void *);
using CreateUnitFn = void *(__fastcall *)(int, int, int, int, void *, void *, int, int, void *);
using ModeCountFn = void(__fastcall *)(void *, void *, int, int);

HMODULE g_self = nullptr;
SpawnRoomFn g_original_spawn = nullptr;
CreateUnitFn g_original_create = nullptr;
ModeCountFn g_original_mode_count = nullptr;
wchar_t g_log_path[MAX_PATH] = {};
volatile LONG g_spawn_calls = 0;
volatile LONG g_create_logs = 0;
volatile LONG g_mode_logs = 0;

// 1.14d layouts, used for observation only. Source: redivivus D2RoomStrc,
// D2RoomExStrc, D2DrlgLevelStrc and D2UnitStrc at revision 1b24c756.
uint32_t Field32(const void *base, size_t offset) {
    uint32_t value = 0;
    memcpy(&value, static_cast<const unsigned char *>(base) + offset, sizeof(value));
    return value;
}

struct RoomObservation {
    int level;
    int x;
    int y;
    int monsters;
    int other_units;
};

struct GameObservation {
    unsigned difficulty;
    unsigned map_seed;
    unsigned character_class;
    unsigned create_time;
    char character_name[17];
};

GameObservation ObserveGame(void *game) {
    GameObservation result = {};
    auto *bytes = static_cast<unsigned char *>(game);
    result.difficulty = bytes[0x6d];
    result.map_seed = Field32(game, 0x7c);
    auto *client = reinterpret_cast<unsigned char *>(Field32(game, 0x88));
    if (client) {
        result.character_class = client[0x08];
        result.create_time = Field32(client, 0x488);
        memcpy(result.character_name, client + 0x0d, 16);
        result.character_name[16] = '\0';
        for (char &c : result.character_name) {
            if (!c) break;
            if (static_cast<unsigned char>(c) < 0x20 ||
                static_cast<unsigned char>(c) > 0x7e) c = '?';
        }
    }
    return result;
}

RoomObservation ObserveRoom(void *room) {
    RoomObservation result = {-1, 0, 0, 0, 0};
    auto *room_bytes = static_cast<unsigned char *>(room);
    result.x = static_cast<int>(Field32(room, 0x4c));
    result.y = static_cast<int>(Field32(room, 0x50));
    auto *room_ex = reinterpret_cast<unsigned char *>(Field32(room, 0x10));
    if (room_ex) {
        auto *level = reinterpret_cast<unsigned char *>(Field32(room_ex, 0x58));
        if (level) result.level = static_cast<int>(Field32(level, 0x1d0));
    }
    auto *unit = reinterpret_cast<unsigned char *>(Field32(room_bytes, 0x74));
    for (int seen = 0; unit && seen < 4096; ++seen) {
        if (Field32(unit, 0) == 1) ++result.monsters;
        else ++result.other_units;
        unit = reinterpret_cast<unsigned char *>(Field32(unit, 0xe8));
    }
    return result;
}

void Log(const char *format, ...) {
    if (!g_log_path[0]) return;
    char line[512] = {};
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(line, sizeof(line) - 2, format, args);
    va_end(args);
    if (length < 0) return;
    size_t used = static_cast<size_t>(length);
    if (used > sizeof(line) - 2) used = sizeof(line) - 2;
    line[used++] = '\n';
    HANDLE file = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(used), &written, nullptr);
    CloseHandle(file);
}

bool InitLogPath() {
    const DWORD length = GetModuleFileNameW(g_self, g_log_path, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    wchar_t *last_slash = wcsrchr(g_log_path, L'\\');
    if (!last_slash) return false;
    static constexpr wchar_t kName[] = L"population_trace.log";
    if (static_cast<size_t>(last_slash - g_log_path) + 1 +
            (sizeof(kName) / sizeof(kName[0])) > MAX_PATH) return false;
    wcscpy(last_slash + 1, kName);
    return true;
}

bool HashGame(char output[65]) {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool ok = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
              CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash);
    unsigned char buffer[64 * 1024];
    DWORD count = 0;
    while (ok) {
        if (!ReadFile(file, buffer, sizeof(buffer), &count, nullptr)) {
            ok = false;
            break;
        }
        if (!count) break;
        ok = CryptHashData(hash, buffer, count, 0) != FALSE;
    }
    unsigned char digest[32] = {};
    DWORD digest_length = sizeof(digest);
    if (ok) ok = CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_length, 0) &&
                 digest_length == sizeof(digest);
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    CloseHandle(file);
    if (!ok) return false;
    for (size_t i = 0; i < sizeof(digest); ++i) {
        snprintf(output + i * 2, 3, "%02x", digest[i]);
    }
    output[64] = '\0';
    return true;
}

void WriteJump(unsigned char *output, const void *instruction_address, const void *destination) {
    output[0] = 0xe9;
    const auto delta = static_cast<int32_t>(reinterpret_cast<uintptr_t>(destination) -
                                            (reinterpret_cast<uintptr_t>(instruction_address) + 5));
    memcpy(output + 1, &delta, sizeof(delta));
}

__attribute__((noinline)) bool CalledFromOrdinaryGroup() {
    // Game.exe 1.14d keeps EBP frames in the normal-group call chain. This is
    // only an observation hint; no gameplay decision depends on it.
    auto *frame = static_cast<uintptr_t *>(__builtin_frame_address(0));
    const uintptr_t game_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t low = game_base + 0x14df80;
    const uintptr_t high = game_base + 0x14e08d;
    const uintptr_t stack_top = reinterpret_cast<uintptr_t>(frame) + 1024 * 1024;
    for (int depth = 0; frame && depth < 24; ++depth) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(frame);
        if (address >= stack_top || address % alignof(uintptr_t)) break;
        MEMORY_BASIC_INFORMATION region = {};
        if (!VirtualQuery(frame, &region, sizeof(region)) ||
            region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            address + 2 * sizeof(uintptr_t) >
                reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize) break;
        const uintptr_t return_address = frame[1];
        if (return_address >= low && return_address < high) return true;
        auto *next = reinterpret_cast<uintptr_t *>(frame[0]);
        if (reinterpret_cast<uintptr_t>(next) <= address) break;
        frame = next;
    }
    return false;
}

void *__fastcall TraceCreateUnit(int type, int class_id, int x, int y, void *game,
                                 void *room, int flag, int mode, void *extra) {
    const bool ordinary_group = type == 1 && CalledFromOrdinaryGroup();
    void *unit = g_original_create(type, class_id, x, y, game, room, flag, mode, extra);
    if (type == 1 && InterlockedIncrement(&g_create_logs) <= 200) {
        const unsigned guid = unit ? Field32(unit, 0x0c) : 0;
        Log("monster-create class=%d guid=%u unit=%p room=%p ordinary_group=%d",
            class_id, guid, unit, room, ordinary_group ? 1 : 0);
    }
    return unit;
}

bool InstallCreateHook() {
    auto *target = reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr)) + kCreateUnitRva;
    if (memcmp(target, kExpectedCreatePrologue, sizeof(kExpectedCreatePrologue)) != 0) {
        Log("create-unit prologue mismatch at %p; hook disabled", target);
        return false;
    }
    auto *trampoline = static_cast<unsigned char *>(
        VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    memcpy(trampoline, target, sizeof(kExpectedCreatePrologue));
    WriteJump(trampoline + sizeof(kExpectedCreatePrologue),
              trampoline + sizeof(kExpectedCreatePrologue),
              target + sizeof(kExpectedCreatePrologue));
    DWORD old_protect = 0;
    if (!VirtualProtect(target, sizeof(kExpectedCreatePrologue), PAGE_EXECUTE_READWRITE,
                        &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    g_original_create = reinterpret_cast<CreateUnitFn>(trampoline);
    unsigned char patch[sizeof(kExpectedCreatePrologue)] = {};
    WriteJump(patch, target, reinterpret_cast<const void *>(&TraceCreateUnit));
    patch[5] = 0x90;
    memcpy(target, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
    DWORD ignored = 0;
    VirtualProtect(target, sizeof(patch), old_protect, &ignored);
    Log("create-unit hook installed at %p trampoline=%p", target, trampoline);
    return true;
}

void __fastcall TraceModeCount(void *regions, void *unit, int previous, int current) {
    g_original_mode_count(regions, unit, previous, current);
    if (unit && Field32(unit, 0) == 1 &&
        InterlockedIncrement(&g_mode_logs) <= 200) {
        Log("monster-mode-count guid=%u class=%u previous=%d current=%d",
            Field32(unit, 0x0c), Field32(unit, 0x04), previous, current);
    }
}

bool InstallModeCountHook() {
    auto *target = reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr)) + kModeCountRva;
    if (memcmp(target, kExpectedModeCountPrologue, sizeof(kExpectedModeCountPrologue)) != 0) {
        Log("mode-count prologue mismatch at %p; hook disabled", target);
        return false;
    }
    auto *trampoline = static_cast<unsigned char *>(
        VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    memcpy(trampoline, target, sizeof(kExpectedModeCountPrologue));
    WriteJump(trampoline + sizeof(kExpectedModeCountPrologue),
              trampoline + sizeof(kExpectedModeCountPrologue),
              target + sizeof(kExpectedModeCountPrologue));
    DWORD old_protect = 0;
    if (!VirtualProtect(target, sizeof(kExpectedModeCountPrologue), PAGE_EXECUTE_READWRITE,
                        &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    g_original_mode_count = reinterpret_cast<ModeCountFn>(trampoline);
    unsigned char patch[sizeof(kExpectedModeCountPrologue)] = {};
    WriteJump(patch, target, reinterpret_cast<const void *>(&TraceModeCount));
    patch[5] = 0x90;
    memcpy(target, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
    DWORD ignored = 0;
    VirtualProtect(target, sizeof(patch), old_protect, &ignored);
    Log("mode-count hook installed at %p trampoline=%p", target, trampoline);
    return true;
}

void __fastcall TraceSpawnRoom(void *game, void *room) {
    const LONG number = InterlockedIncrement(&g_spawn_calls);
    RoomObservation before = {};
    if (number <= 100) {
        before = ObserveRoom(room);
        const GameObservation observed_game = ObserveGame(game);
        Log("room-population begin #%ld game=%p room=%p level=%d x=%d y=%d monsters=%d other=%d difficulty=%u map_seed=%u class=%u created=%u character=%s",
            number, game, room, before.level, before.x, before.y,
            before.monsters, before.other_units, observed_game.difficulty,
            observed_game.map_seed, observed_game.character_class,
            observed_game.create_time, observed_game.character_name);
    }
    g_original_spawn(game, room);
    if (number <= 100) {
        const RoomObservation after = ObserveRoom(room);
        Log("room-population end #%ld level=%d x=%d y=%d monsters=%d delta=%d other=%d",
            number, after.level, after.x, after.y, after.monsters,
            after.monsters - before.monsters, after.other_units);
    }
}

bool InstallSpawnHook() {
    HMODULE game_module = GetModuleHandleW(nullptr);
    if (!game_module) return false;
    auto *target = reinterpret_cast<unsigned char *>(game_module) + kSpawnRva;
    if (memcmp(target, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("spawn prologue mismatch at %p; hook disabled", target);
        return false;
    }
    auto *trampoline = static_cast<unsigned char *>(
        VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    memcpy(trampoline, target, sizeof(kExpectedPrologue));
    WriteJump(trampoline + sizeof(kExpectedPrologue),
              trampoline + sizeof(kExpectedPrologue), target + sizeof(kExpectedPrologue));

    DWORD old_protect = 0;
    if (!VirtualProtect(target, sizeof(kExpectedPrologue), PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    g_original_spawn = reinterpret_cast<SpawnRoomFn>(trampoline);
    unsigned char patch[sizeof(kExpectedPrologue)] = {};
    WriteJump(patch, target, reinterpret_cast<const void *>(&TraceSpawnRoom));
    patch[5] = 0x90;
    memcpy(target, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
    DWORD ignored = 0;
    VirtualProtect(target, sizeof(patch), old_protect, &ignored);
    Log("spawn hook installed at %p trampoline=%p", target, trampoline);
    return true;
}

}  // namespace

extern "C" __declspec(dllexport) DWORD WINAPI StartTrace(void *) {
    if (!InitLogPath()) return 1;
    char digest[65] = {};
    if (!HashGame(digest)) {
        Log("cannot hash Game.exe; no hook installed");
        return 2;
    }
    Log("Game.exe SHA-256 %s", digest);
    if (strcmp(digest, kExpectedGameSha256) != 0) {
        Log("unsupported executable; no hook installed");
        return 3;
    }
    if (!InstallSpawnHook()) return 4;
    if (!InstallCreateHook()) return 5;
    if (!InstallModeCountHook()) return 6;
    return 0;
}

BOOL WINAPI DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = self;
        DisableThreadLibraryCalls(self);
    }
    return TRUE;
}
