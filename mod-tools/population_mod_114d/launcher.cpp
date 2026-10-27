#include <windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

bool Error(const wchar_t *stage) {
    fwprintf(stderr, L"%ls failed (Windows error %lu).\n", stage, GetLastError());
    return false;
}

std::wstring Quote(const wchar_t *value) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (const wchar_t *p = value; *p; ++p) {
        if (*p == L'\\') {
            ++slashes;
        } else {
            out.append(*p == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            out += *p;
        }
    }
    out.append(slashes * 2, L'\\');
    out += L'\"';
    return out;
}

bool SiblingPath(const wchar_t *file_name, std::wstring &path) {
    wchar_t module[MAX_PATH] = {};
    const DWORD size = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (!size || size >= MAX_PATH) return false;
    wchar_t *slash = wcsrchr(module, L'\\');
    if (!slash) return false;
    path.assign(module, slash + 1);
    path += file_name;
    return path.size() < MAX_PATH;
}

uintptr_t RemoteModuleBase(DWORD process_id, const wchar_t *module_name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                                process_id);
    if (snapshot == INVALID_HANDLE_VALUE) {
        Error(L"module snapshot");
        return 0;
    }
    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    uintptr_t base = 0;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, module_name) == 0) {
                base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (!base) fwprintf(stderr, L"Remote module not found: %ls\n", module_name);
    return base;
}

uintptr_t RemoteAddress(HANDLE process, DWORD process_id, FARPROC local_function) {
    HMODULE local_owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(local_function), &local_owner)) {
        Error(L"local module lookup");
        return 0;
    }
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(local_owner, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return 0;
    const wchar_t *name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    const uintptr_t remote_base = RemoteModuleBase(process_id, name);
    if (remote_base) {
        return remote_base + reinterpret_cast<uintptr_t>(local_function) -
               reinterpret_cast<uintptr_t>(local_owner);
    }
    // Wine may deny Toolhelp module snapshots of a new child. Shared system
    // DLL mappings can still use the same address; verify code bytes first.
    unsigned char remote_bytes[16] = {};
    SIZE_T bytes_read = 0;
    if (ReadProcessMemory(process, reinterpret_cast<void *>(local_function), remote_bytes,
                          sizeof(remote_bytes), &bytes_read) &&
        bytes_read == sizeof(remote_bytes) &&
        memcmp(remote_bytes, reinterpret_cast<const void *>(local_function),
               sizeof(remote_bytes)) == 0) {
        fwprintf(stderr, L"Using verified same-address %ls mapping.\n", name);
        return reinterpret_cast<uintptr_t>(local_function);
    }
    Error(L"same-address function verification");
    return 0;
}

bool RunRemote(HANDLE process, uintptr_t function, void *argument, DWORD &result) {
    HANDLE thread = CreateRemoteThread(process, nullptr, 0,
                                       reinterpret_cast<LPTHREAD_START_ROUTINE>(function),
                                       argument, 0, nullptr);
    if (!thread) return Error(L"CreateRemoteThread");
    const DWORD wait = WaitForSingleObject(thread, 30000);
    const bool ok = wait == WAIT_OBJECT_0 && GetExitCodeThread(thread, &result);
    if (!ok) fwprintf(stderr, L"Remote thread failed or timed out (wait=%lu, error=%lu).\n",
                      wait, GetLastError());
    CloseHandle(thread);
    return ok;
}

bool Inject(HANDLE process, DWORD process_id, const std::wstring &dll_path) {
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel) return false;
    FARPROC local_load = GetProcAddress(kernel, "LoadLibraryW");
    if (!local_load) return false;
    const uintptr_t remote_load = RemoteAddress(process, process_id, local_load);
    if (!remote_load) return false;

    const SIZE_T bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    void *remote_path = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
    if (!remote_path) return Error(L"VirtualAllocEx");
    SIZE_T written = 0;
    bool ok = WriteProcessMemory(process, remote_path, dll_path.c_str(), bytes, &written) &&
              written == bytes;
    DWORD remote_dll = 0;
    if (!ok) Error(L"WriteProcessMemory");
    if (ok) ok = RunRemote(process, remote_load, remote_path, remote_dll) && remote_dll != 0;
    if (ok == false && remote_dll == 0)
        fwprintf(stderr, L"Remote LoadLibraryW returned null.\n");
    VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    if (!ok) return false;

    HMODULE local_dll = LoadLibraryExW(dll_path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!local_dll) return Error(L"LoadLibraryExW local DLL inspection");
    FARPROC local_start = GetProcAddress(local_dll, "StartTrace");
    const uintptr_t start_rva = local_start ? reinterpret_cast<uintptr_t>(local_start) -
                                               reinterpret_cast<uintptr_t>(local_dll) : 0;
    FreeLibrary(local_dll);
    if (!start_rva) {
        fwprintf(stderr, L"StartTrace export missing.\n");
        return false;
    }
    DWORD trace_status = 0xffffffff;
    if (!RunRemote(process, static_cast<uintptr_t>(remote_dll) + start_rva, nullptr,
                   trace_status)) return false;
    if (trace_status != 0) {
        fwprintf(stderr, L"Trace initialization failed (code %lu).\n", trace_status);
        return false;
    }
    return true;
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc < 2) {
        fwprintf(stderr, L"Usage: population_launcher.exe <Game.exe> [game options]\n");
        return 2;
    }
    wchar_t full_game[MAX_PATH] = {};
    const DWORD full_length = GetFullPathNameW(argv[1], MAX_PATH, full_game, nullptr);
    if (!full_length || full_length >= MAX_PATH) {
        fwprintf(stderr, L"Invalid Game.exe path.\n");
        return 2;
    }
    std::wstring dll_path;
    if (!SiblingPath(L"population_trace.dll", dll_path)) {
        fwprintf(stderr, L"Cannot find plugin directory.\n");
        return 2;
    }
    if (GetFileAttributesW(dll_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"Plugin missing: %ls\n", dll_path.c_str());
        return 2;
    }
    std::wstring command = Quote(full_game);
    for (int i = 2; i < argc; ++i) {
        command += L' ';
        command += Quote(argv[i]);
    }
    std::vector<wchar_t> command_buffer(command.begin(), command.end());
    command_buffer.push_back(L'\0');
    std::wstring working_dir(full_game);
    const size_t separator = working_dir.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return 2;
    working_dir.resize(separator);

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child = {};
    if (!CreateProcessW(full_game, command_buffer.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, working_dir.c_str(), &startup, &child)) {
        fwprintf(stderr, L"Could not start Game.exe (Windows error %lu).\n", GetLastError());
        return 1;
    }
    const DWORD idle = WaitForInputIdle(child.hProcess, 30000);
    if (idle != 0) Sleep(2000);  // Some Wine window modes do not report input idle.
    DWORD child_status = STILL_ACTIVE;
    if (!GetExitCodeProcess(child.hProcess, &child_status) || child_status != STILL_ACTIVE) {
        fwprintf(stderr, L"Game.exe exited before plugin injection (status %lu).\n", child_status);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        return 1;
    }

    const bool injected = Inject(child.hProcess, child.dwProcessId, dll_path);
    if (!injected) {
        fwprintf(stderr, L"Plugin injection failed; closing the new game process.\n");
        TerminateProcess(child.hProcess, 1);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        return 1;
    }
    wprintf(L"Trace active in Game.exe (PID %lu).\n", child.dwProcessId);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return 0;
}
