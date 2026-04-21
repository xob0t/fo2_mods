#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace {

constexpr DWORD kPollSleepMs = 16;
constexpr DWORD kCooldownMs = 600;
constexpr DWORD kDefaultHotkey = 'N';
constexpr WORD kDefaultControllerMask = XINPUT_GAMEPAD_LEFT_SHOULDER;
constexpr uintptr_t kStopMusicRva = 0x20270;
constexpr uintptr_t kPlayTitleMusicRva = 0x20230;
constexpr uintptr_t kPlayGameplayMusicRva = 0x202C8;

std::atomic<bool> g_running{true};
DWORD g_hotkey = kDefaultHotkey;
WORD g_controller_mask = kDefaultControllerMask;
char g_log_path[MAX_PATH] = {};

using XInputGetStateFn = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_xinput_get_state = nullptr;

using VoidFn = void(__cdecl*)();

void Log(const char* fmt, ...)
{
    FILE* file = nullptr;
    if (fopen_s(&file, g_log_path, "a") != 0 || file == nullptr) {
        return;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::fprintf(
        file,
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds
    );

    va_list args;
    va_start(args, fmt);
    std::vfprintf(file, fmt, args);
    va_end(args);
    std::fprintf(file, "\n");
    std::fclose(file);
}

DWORD LoadHotkey(const char* key_name, DWORD default_value)
{
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0) {
        return default_value;
    }

    char* slash = strrchr(path, '\\');
    if (slash == nullptr) {
        return default_value;
    }

    slash[1] = '\0';
    std::snprintf(path + strlen(path), MAX_PATH - strlen(path), "fo2_skip_track.ini");
    const UINT value = GetPrivateProfileIntA("skip_track", key_name, default_value, path);
    return value == 0 ? default_value : value;
}

WORD LoadControllerMask()
{
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0) {
        return kDefaultControllerMask;
    }

    char* slash = strrchr(path, '\\');
    if (slash == nullptr) {
        return kDefaultControllerMask;
    }

    slash[1] = '\0';
    std::snprintf(path + strlen(path), MAX_PATH - strlen(path), "fo2_skip_track.ini");

    char buffer[128] = {};
    GetPrivateProfileStringA("skip_track", "controller_buttons", "LEFT_SHOULDER", buffer, sizeof(buffer), path);

    WORD mask = 0;
    char* context = nullptr;
    for (char* token = strtok_s(buffer, "+ ,|", &context); token != nullptr; token = strtok_s(nullptr, "+ ,|", &context)) {
        if (_stricmp(token, "DPAD_UP") == 0) mask |= XINPUT_GAMEPAD_DPAD_UP;
        else if (_stricmp(token, "DPAD_DOWN") == 0) mask |= XINPUT_GAMEPAD_DPAD_DOWN;
        else if (_stricmp(token, "DPAD_LEFT") == 0) mask |= XINPUT_GAMEPAD_DPAD_LEFT;
        else if (_stricmp(token, "DPAD_RIGHT") == 0) mask |= XINPUT_GAMEPAD_DPAD_RIGHT;
        else if (_stricmp(token, "START") == 0) mask |= XINPUT_GAMEPAD_START;
        else if (_stricmp(token, "BACK") == 0) mask |= XINPUT_GAMEPAD_BACK;
        else if (_stricmp(token, "LEFT_THUMB") == 0) mask |= XINPUT_GAMEPAD_LEFT_THUMB;
        else if (_stricmp(token, "RIGHT_THUMB") == 0) mask |= XINPUT_GAMEPAD_RIGHT_THUMB;
        else if (_stricmp(token, "LEFT_SHOULDER") == 0) mask |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        else if (_stricmp(token, "RIGHT_SHOULDER") == 0) mask |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        else if (_stricmp(token, "A") == 0) mask |= XINPUT_GAMEPAD_A;
        else if (_stricmp(token, "B") == 0) mask |= XINPUT_GAMEPAD_B;
        else if (_stricmp(token, "X") == 0) mask |= XINPUT_GAMEPAD_X;
        else if (_stricmp(token, "Y") == 0) mask |= XINPUT_GAMEPAD_Y;
    }

    return mask == 0 ? kDefaultControllerMask : mask;
}

bool InitXInput()
{
    const char* dll_names[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (const char* dll_name : dll_names) {
        HMODULE module = LoadLibraryA(dll_name);
        if (module == nullptr) {
            continue;
        }

        auto proc = reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState"));
        if (proc != nullptr) {
            g_xinput_get_state = proc;
            Log("XInput initialized from %s", dll_name);
            return true;
        }
    }

    Log("XInput unavailable");
    return false;
}

void RestartGameplayMusic()
{
    auto* const base = reinterpret_cast<std::uint8_t*>(GetModuleHandleA(nullptr));
    if (base == nullptr) {
        Log("Gameplay restart aborted: game module base not found");
        return;
    }

    auto stop_music = reinterpret_cast<VoidFn>(base + kStopMusicRva);
    auto play_gameplay_music = reinterpret_cast<VoidFn>(base + kPlayGameplayMusicRva);
    auto play_title_music = reinterpret_cast<VoidFn>(base + kPlayTitleMusicRva);

    Log(
        "Gameplay restart requested: base=%p stop=%p gameplay=%p title=%p",
        base,
        stop_music,
        play_gameplay_music,
        play_title_music
    );

    stop_music();
    Sleep(25);
    play_gameplay_music();
}

void RestartTitleMusic()
{
    auto* const base = reinterpret_cast<std::uint8_t*>(GetModuleHandleA(nullptr));
    if (base == nullptr) {
        Log("Title restart aborted: game module base not found");
        return;
    }

    auto stop_music = reinterpret_cast<VoidFn>(base + kStopMusicRva);
    auto play_title_music = reinterpret_cast<VoidFn>(base + kPlayTitleMusicRva);

    Log("Title restart requested: base=%p stop=%p title=%p", base, stop_music, play_title_music);

    stop_music();
    Sleep(25);
    play_title_music();
}

DWORD WINAPI HotkeyThread(LPVOID)
{
    Log(
        "Hotkey thread started, hotkey_vk=%lu controller_mask=0x%04X",
        static_cast<unsigned long>(g_hotkey),
        static_cast<unsigned>(g_controller_mask)
    );

    DWORD last_trigger = 0;
    bool was_down = false;
    bool controller_was_down[4] = {false, false, false, false};

    while (g_running.load()) {
        const DWORD now = GetTickCount();
        const bool is_down = (GetAsyncKeyState(static_cast<int>(g_hotkey)) & 0x8000) != 0;

        if (is_down && !was_down && now - last_trigger >= kCooldownMs) {
            last_trigger = now;
            RestartGameplayMusic();
            RestartTitleMusic();
        }

        was_down = is_down;

        if (g_xinput_get_state != nullptr && g_controller_mask != 0) {
            for (DWORD i = 0; i < 4; ++i) {
                XINPUT_STATE state{};
                const DWORD result = g_xinput_get_state(i, &state);
                if (result != ERROR_SUCCESS) {
                    controller_was_down[i] = false;
                    continue;
                }

                const bool controller_down = (state.Gamepad.wButtons & g_controller_mask) == g_controller_mask;
                if (controller_down && !controller_was_down[i] && now - last_trigger >= kCooldownMs) {
                    last_trigger = now;
                    Log("Controller trigger on pad %lu", static_cast<unsigned long>(i));
                    RestartGameplayMusic();
                    RestartTitleMusic();
                }

                controller_was_down[i] = controller_down;
            }
        }

        Sleep(kPollSleepMs);
    }

    Log("Hotkey thread stopping");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(module);

        if (GetModuleFileNameA(nullptr, g_log_path, MAX_PATH) == 0) {
            std::snprintf(g_log_path, MAX_PATH, "fo2_skip_track.log");
        } else {
            char* slash = std::strrchr(g_log_path, '\\');
            if (slash != nullptr) {
                slash[1] = '\0';
                std::snprintf(
                    g_log_path + std::strlen(g_log_path),
                    MAX_PATH - std::strlen(g_log_path),
                    "fo2_skip_track.log"
                );
            }
        }

        g_hotkey = LoadHotkey("hotkey_vk", kDefaultHotkey);
        g_controller_mask = LoadControllerMask();
        InitXInput();
        Log("DLL attached");
        const HANDLE thread = CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        } else {
            Log("CreateThread failed: %lu", static_cast<unsigned long>(GetLastError()));
        }
        return TRUE;
    }
    case DLL_PROCESS_DETACH:
        g_running.store(false);
        Log("DLL detached");
        return TRUE;
    default:
        return TRUE;
    }
}
