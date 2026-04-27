#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

using DirectInput8CreateFn = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using DllCanUnloadNowFn = HRESULT (WINAPI*)();
using DllGetClassObjectFn = HRESULT (WINAPI*)(REFCLSID, REFIID, LPVOID*);
using DllRegisterServerFn = HRESULT (WINAPI*)();
using DllUnregisterServerFn = HRESULT (WINAPI*)();

HMODULE g_real_dinput = nullptr;
DirectInput8CreateFn g_real_direct_input8_create = nullptr;
DllCanUnloadNowFn g_real_dll_can_unload_now = nullptr;
DllGetClassObjectFn g_real_dll_get_class_object = nullptr;
DllRegisterServerFn g_real_dll_register_server = nullptr;
DllUnregisterServerFn g_real_dll_unregister_server = nullptr;

bool g_enabled = true;
bool g_log_enabled = true;
DWORD g_controller_index = 0;
DWORD g_max_strength = 100;
DWORD g_default_duration_ms = 250;
bool g_directional_enabled = true;
DWORD g_directional_bias = 75;
DWORD g_directional_min_motor = 25;
bool g_directional_invert = false;
bool g_synthetic_enabled = true;
DWORD g_synthetic_button_mask = XINPUT_GAMEPAD_RIGHT_SHOULDER;
DWORD g_synthetic_left_strength = 70;
DWORD g_synthetic_right_strength = 100;
DWORD g_synthetic_pulse_ms = 180;
DWORD g_synthetic_require_left_trigger = 0;
DWORD g_synthetic_require_right_trigger = 30;
bool g_events_enabled = true;
bool g_envelope_enabled = true;
DWORD g_envelope_min_strength = 70;
DWORD g_envelope_kick_strength = 100;
DWORD g_envelope_kick_ms = 45;
DWORD g_envelope_gap_ms = 25;
DWORD g_envelope_sustain_percent = 80;
DWORD g_envelope_sustain_min_ms = 220;
DWORD g_envelope_sustain_max_ms = 520;
bool g_damage_enabled = true;
bool g_damage_player_only = true;
LONG g_damage_player_index = -1;
DWORD g_damage_min_delta_per_mille = 3;
DWORD g_damage_full_delta_per_mille = 80;
DWORD g_damage_min_strength = 20;
DWORD g_damage_max_strength = 100;
DWORD g_damage_pulse_ms = 180;
DWORD g_damage_cooldown_ms = 120;
bool g_offense_damage_enabled = true;
DWORD g_offense_damage_window_ms = 600;
DWORD g_offense_damage_min_strength = 70;
DWORD g_offense_damage_max_strength = 100;
DWORD g_offense_damage_pulse_ms = 520;
DWORD g_offense_damage_cooldown_ms = 120;
bool g_contact_enabled = true;
DWORD g_contact_min_delta_per_mille = 8000;
DWORD g_contact_full_delta_per_mille = 220000;
DWORD g_contact_min_strength = 8;
DWORD g_contact_max_strength = 45;
DWORD g_contact_pulse_ms = 80;
DWORD g_contact_cooldown_ms = 220;
DWORD g_contact_car_min_strength = 65;
DWORD g_contact_car_max_strength = 100;
DWORD g_contact_car_pulse_ms = 420;
DWORD g_contact_car_cooldown_ms = 120;
bool g_rubble_contact_enabled = true;
bool g_scrape_enabled = true;
DWORD g_scrape_min_delta_per_mille = 5000;
DWORD g_scrape_full_delta_per_mille = 60000;
DWORD g_scrape_min_strength = 35;
DWORD g_scrape_max_strength = 100;
DWORD g_scrape_pulse_ms = 260;
DWORD g_scrape_cooldown_ms = 50;
DWORD g_scrape_grain_ms = 32;
DWORD g_scrape_low_percent = 20;
DWORD g_scrape_min_side_per_mille = 450;
DWORD g_scrape_steering_threshold = 7000;
DWORD g_scrape_directional_bias = 75;
DWORD g_scrape_directional_min_motor = 30;
bool g_landing_enabled = true;
DWORD g_landing_airborne_gap_ms = 300;
DWORD g_landing_min_delta_per_mille = 15000;
DWORD g_landing_full_delta_per_mille = 180000;
DWORD g_landing_min_strength = 25;
DWORD g_landing_max_strength = 85;
DWORD g_landing_pulse_ms = 160;
DWORD g_landing_cooldown_ms = 600;
bool g_collision_sound_enabled = false;
DWORD g_collision_sound_min_volume = 80;
DWORD g_collision_sound_full_volume = 650;
DWORD g_collision_sound_min_strength = 18;
DWORD g_collision_sound_max_strength = 70;
DWORD g_collision_sound_pulse_ms = 120;
DWORD g_collision_sound_cooldown_ms = 140;
char g_log_path[MAX_PATH] = {};
std::atomic<bool> g_worker_running{false};
std::atomic<DWORD> g_rumble_until{0};
std::atomic<WORD> g_left_motor{0};
std::atomic<WORD> g_right_motor{0};
std::atomic<DWORD> g_envelope_id{0};
std::atomic<DWORD> g_envelope_start{0};
std::atomic<DWORD> g_envelope_kick_until{0};
std::atomic<DWORD> g_envelope_gap_until{0};
std::atomic<DWORD> g_envelope_until{0};
std::atomic<WORD> g_envelope_kick_left{0};
std::atomic<WORD> g_envelope_kick_right{0};
std::atomic<WORD> g_envelope_sustain_left{0};
std::atomic<WORD> g_envelope_sustain_right{0};
std::atomic<bool> g_envelope_gap_active{false};
std::atomic<bool> g_envelope_sustain_active{false};
std::atomic<DWORD> g_last_damage_rumble{0};
std::atomic<DWORD> g_last_offense_damage_rumble{0};
std::atomic<DWORD> g_last_contact_rumble{0};
std::atomic<DWORD> g_last_car_contact_rumble{0};
std::atomic<DWORD> g_last_scrape_rumble{0};
std::atomic<DWORD> g_scrape_until{0};
std::atomic<DWORD> g_scrape_next_grain{0};
std::atomic<WORD> g_scrape_high_left{0};
std::atomic<WORD> g_scrape_high_right{0};
std::atomic<WORD> g_scrape_low_left{0};
std::atomic<WORD> g_scrape_low_right{0};
std::atomic<bool> g_scrape_high_active{false};
std::atomic<DWORD> g_last_ground_contact{0};
std::atomic<DWORD> g_last_landing_rumble{0};
std::atomic<DWORD> g_last_collision_sound_rumble{0};
std::atomic<bool> g_hooks_installed{false};
std::uintptr_t g_contact_continue = 0;
std::uintptr_t g_damage_continue = 0;
std::uintptr_t g_collision_sound_continue = 0;
std::uintptr_t g_collision_sound_set_volume = 0;
std::uintptr_t g_selected_car_index_address = 0;

struct PlayerContactRecord
{
    DWORD time;
    DWORD object;
    DWORD car;
    DWORD type;
    DWORD left_strength;
    DWORD right_strength;
    LONG side_per_mille;
};

std::mutex g_player_contact_mutex;
PlayerContactRecord g_player_contacts[16] = {};
DWORD g_player_contact_index = 0;

void Log(const char* fmt, ...)
{
    if (!g_log_enabled || g_log_path[0] == '\0') {
        return;
    }

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

void BuildGamePath(char* out, DWORD out_size, const char* file_name)
{
    if (GetModuleFileNameA(nullptr, out, out_size) == 0) {
        std::snprintf(out, out_size, "%s", file_name);
        return;
    }

    char* slash = std::strrchr(out, '\\');
    if (slash == nullptr) {
        std::snprintf(out, out_size, "%s", file_name);
        return;
    }

    slash[1] = '\0';
    std::snprintf(out + std::strlen(out), out_size - std::strlen(out), "%s", file_name);
}

DWORD ButtonMaskFromName(const char* name)
{
    if (_stricmp(name, "A") == 0) return XINPUT_GAMEPAD_A;
    if (_stricmp(name, "B") == 0) return XINPUT_GAMEPAD_B;
    if (_stricmp(name, "X") == 0) return XINPUT_GAMEPAD_X;
    if (_stricmp(name, "Y") == 0) return XINPUT_GAMEPAD_Y;
    if (_stricmp(name, "LEFT_SHOULDER") == 0 || _stricmp(name, "LB") == 0) return XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (_stricmp(name, "RIGHT_SHOULDER") == 0 || _stricmp(name, "RB") == 0) return XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (_stricmp(name, "BACK") == 0 || _stricmp(name, "SELECT") == 0) return XINPUT_GAMEPAD_BACK;
    if (_stricmp(name, "START") == 0) return XINPUT_GAMEPAD_START;
    if (_stricmp(name, "LEFT_THUMB") == 0 || _stricmp(name, "LS") == 0) return XINPUT_GAMEPAD_LEFT_THUMB;
    if (_stricmp(name, "RIGHT_THUMB") == 0 || _stricmp(name, "RS") == 0) return XINPUT_GAMEPAD_RIGHT_THUMB;
    if (_stricmp(name, "DPAD_UP") == 0) return XINPUT_GAMEPAD_DPAD_UP;
    if (_stricmp(name, "DPAD_DOWN") == 0) return XINPUT_GAMEPAD_DPAD_DOWN;
    if (_stricmp(name, "DPAD_LEFT") == 0) return XINPUT_GAMEPAD_DPAD_LEFT;
    if (_stricmp(name, "DPAD_RIGHT") == 0) return XINPUT_GAMEPAD_DPAD_RIGHT;
    return 0;
}

DWORD ParseButtonMask(const char* text)
{
    if (text == nullptr || text[0] == '\0') {
        return XINPUT_GAMEPAD_RIGHT_SHOULDER;
    }

    char buffer[256] = {};
    std::snprintf(buffer, sizeof(buffer), "%s", text);

    DWORD mask = 0;
    char* context = nullptr;
    for (char* token = strtok_s(buffer, ",;| ", &context); token != nullptr; token = strtok_s(nullptr, ",;| ", &context)) {
        mask |= ButtonMaskFromName(token);
    }

    return mask != 0 ? mask : XINPUT_GAMEPAD_RIGHT_SHOULDER;
}

void LoadConfig()
{
    char ini_path[MAX_PATH] = {};
    BuildGamePath(ini_path, MAX_PATH, "fo2_xinput_rumble.ini");
    BuildGamePath(g_log_path, MAX_PATH, "fo2_xinput_rumble.log");

    g_enabled = GetPrivateProfileIntA("rumble", "enabled", 1, ini_path) != 0;
    g_controller_index = std::min<DWORD>(GetPrivateProfileIntA("rumble", "controller_index", 0, ini_path), 3);
    g_max_strength = std::min<DWORD>(GetPrivateProfileIntA("rumble", "max_strength", 100, ini_path), 100);
    g_default_duration_ms = std::max<DWORD>(GetPrivateProfileIntA("rumble", "default_duration_ms", 250, ini_path), 1);
    g_log_enabled = GetPrivateProfileIntA("rumble", "log", 1, ini_path) != 0;
    g_directional_enabled = GetPrivateProfileIntA("rumble", "directional_enabled", 1, ini_path) != 0;
    g_directional_bias = std::min<DWORD>(GetPrivateProfileIntA("rumble", "directional_bias", 75, ini_path), 100);
    g_directional_min_motor = std::min<DWORD>(GetPrivateProfileIntA("rumble", "directional_min_motor", 25, ini_path), 100);
    g_directional_invert = GetPrivateProfileIntA("rumble", "directional_invert", 0, ini_path) != 0;
    g_synthetic_enabled = GetPrivateProfileIntA("synthetic", "enabled", 1, ini_path) != 0;
    g_synthetic_left_strength = std::min<DWORD>(GetPrivateProfileIntA("synthetic", "left_strength", 70, ini_path), 100);
    g_synthetic_right_strength = std::min<DWORD>(GetPrivateProfileIntA("synthetic", "right_strength", 100, ini_path), 100);
    g_synthetic_pulse_ms = std::max<DWORD>(GetPrivateProfileIntA("synthetic", "pulse_ms", 180, ini_path), 1);
    g_synthetic_require_left_trigger = std::min<DWORD>(GetPrivateProfileIntA("synthetic", "require_left_trigger", 0, ini_path), 255);
    g_synthetic_require_right_trigger = std::min<DWORD>(GetPrivateProfileIntA("synthetic", "require_right_trigger", 30, ini_path), 255);

    char buttons[256] = {};
    GetPrivateProfileStringA("synthetic", "buttons", "RIGHT_SHOULDER", buttons, sizeof(buttons), ini_path);
    g_synthetic_button_mask = ParseButtonMask(buttons);

    g_events_enabled = GetPrivateProfileIntA("events", "enabled", 1, ini_path) != 0;
    g_envelope_enabled = GetPrivateProfileIntA("events", "envelope_enabled", 1, ini_path) != 0;
    g_envelope_min_strength = std::min<DWORD>(GetPrivateProfileIntA("events", "envelope_min_strength", 70, ini_path), 100);
    g_envelope_kick_strength = std::min<DWORD>(GetPrivateProfileIntA("events", "envelope_kick_strength", 100, ini_path), 100);
    g_envelope_kick_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "envelope_kick_ms", 45, ini_path), 1);
    g_envelope_gap_ms = GetPrivateProfileIntA("events", "envelope_gap_ms", 25, ini_path);
    g_envelope_sustain_percent = std::min<DWORD>(GetPrivateProfileIntA("events", "envelope_sustain_percent", 80, ini_path), 100);
    g_envelope_sustain_min_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "envelope_sustain_min_ms", 220, ini_path), 1);
    g_envelope_sustain_max_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "envelope_sustain_max_ms", 520, ini_path), g_envelope_sustain_min_ms);
    g_damage_enabled = GetPrivateProfileIntA("events", "damage_enabled", 1, ini_path) != 0;
    g_damage_player_only = GetPrivateProfileIntA("events", "damage_player_only", 1, ini_path) != 0;
    g_damage_player_index = GetPrivateProfileIntA("events", "damage_player_index", -1, ini_path);
    g_damage_min_delta_per_mille = std::min<DWORD>(GetPrivateProfileIntA("events", "damage_min_delta_per_mille", 3, ini_path), 1000);
    g_damage_full_delta_per_mille = std::max<DWORD>(GetPrivateProfileIntA("events", "damage_full_delta_per_mille", 80, ini_path), g_damage_min_delta_per_mille + 1);
    g_damage_min_strength = std::min<DWORD>(GetPrivateProfileIntA("events", "damage_min_strength", 20, ini_path), 100);
    g_damage_max_strength = std::min<DWORD>(GetPrivateProfileIntA("events", "damage_max_strength", 100, ini_path), 100);
    g_damage_pulse_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "damage_pulse_ms", 180, ini_path), 1);
    g_damage_cooldown_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "damage_cooldown_ms", 120, ini_path), 1);
    g_offense_damage_enabled = GetPrivateProfileIntA("events", "offense_damage_enabled", 1, ini_path) != 0;
    g_offense_damage_window_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "offense_damage_window_ms", 600, ini_path), 1);
    g_offense_damage_min_strength = std::min<DWORD>(GetPrivateProfileIntA("events", "offense_damage_min_strength", 70, ini_path), 100);
    g_offense_damage_max_strength = std::min<DWORD>(GetPrivateProfileIntA("events", "offense_damage_max_strength", 100, ini_path), 100);
    g_offense_damage_pulse_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "offense_damage_pulse_ms", 520, ini_path), 1);
    g_offense_damage_cooldown_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "offense_damage_cooldown_ms", 120, ini_path), 1);
    g_contact_enabled = GetPrivateProfileIntA("events", "contact_enabled", 1, ini_path) != 0;
    g_contact_min_delta_per_mille = std::min<DWORD>(GetPrivateProfileIntA("events", "contact_min_delta_per_mille", 8000, ini_path), 1000000);
    g_contact_full_delta_per_mille = std::max<DWORD>(GetPrivateProfileIntA("events", "contact_full_delta_per_mille", 220000, ini_path), g_contact_min_delta_per_mille + 1);
    g_contact_min_strength = std::min<DWORD>(GetPrivateProfileIntA("events", "contact_min_strength", 8, ini_path), 100);
    g_contact_max_strength = std::min<DWORD>(GetPrivateProfileIntA("events", "contact_max_strength", 45, ini_path), 100);
    g_contact_pulse_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "contact_pulse_ms", 80, ini_path), 1);
    g_contact_cooldown_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "contact_cooldown_ms", 220, ini_path), 1);
    g_contact_car_min_strength = std::min<DWORD>(GetPrivateProfileIntA("events", "contact_car_min_strength", 65, ini_path), 100);
    g_contact_car_max_strength = std::min<DWORD>(GetPrivateProfileIntA("events", "contact_car_max_strength", 100, ini_path), 100);
    g_contact_car_pulse_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "contact_car_pulse_ms", 420, ini_path), 1);
    g_contact_car_cooldown_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "contact_car_cooldown_ms", 120, ini_path), 1);
    g_rubble_contact_enabled = GetPrivateProfileIntA("events", "rubble_contact_enabled", 1, ini_path) != 0;
    g_scrape_enabled = GetPrivateProfileIntA("events", "scrape_enabled", 1, ini_path) != 0;
    g_scrape_min_delta_per_mille = std::min<DWORD>(GetPrivateProfileIntA("events", "scrape_min_delta_per_mille", 5000, ini_path), 1000000);
    g_scrape_full_delta_per_mille = std::max<DWORD>(GetPrivateProfileIntA("events", "scrape_full_delta_per_mille", 60000, ini_path), g_scrape_min_delta_per_mille + 1);
    g_scrape_min_strength = std::min<DWORD>(GetPrivateProfileIntA("events", "scrape_min_strength", 35, ini_path), 100);
    g_scrape_max_strength = std::min<DWORD>(GetPrivateProfileIntA("events", "scrape_max_strength", 100, ini_path), 100);
    g_scrape_pulse_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "scrape_pulse_ms", 260, ini_path), 1);
    g_scrape_cooldown_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "scrape_cooldown_ms", 50, ini_path), 1);
    g_scrape_grain_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "scrape_grain_ms", 32, ini_path), 1);
    g_scrape_low_percent = std::min<DWORD>(GetPrivateProfileIntA("events", "scrape_low_percent", 20, ini_path), 100);
    g_scrape_min_side_per_mille = std::min<DWORD>(GetPrivateProfileIntA("events", "scrape_min_side_per_mille", 450, ini_path), 1000);
    g_scrape_steering_threshold = std::min<DWORD>(GetPrivateProfileIntA("events", "scrape_steering_threshold", 7000, ini_path), 32767);
    g_scrape_directional_bias = std::min<DWORD>(GetPrivateProfileIntA("events", "scrape_directional_bias", 75, ini_path), 100);
    g_scrape_directional_min_motor = std::min<DWORD>(GetPrivateProfileIntA("events", "scrape_directional_min_motor", 30, ini_path), 100);
    g_landing_enabled = GetPrivateProfileIntA("events", "landing_enabled", 1, ini_path) != 0;
    g_landing_airborne_gap_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "landing_airborne_gap_ms", 300, ini_path), 1);
    g_landing_min_delta_per_mille = std::min<DWORD>(GetPrivateProfileIntA("events", "landing_min_delta_per_mille", 15000, ini_path), 1000000);
    g_landing_full_delta_per_mille = std::max<DWORD>(GetPrivateProfileIntA("events", "landing_full_delta_per_mille", 180000, ini_path), g_landing_min_delta_per_mille + 1);
    g_landing_min_strength = std::min<DWORD>(GetPrivateProfileIntA("events", "landing_min_strength", 25, ini_path), 100);
    g_landing_max_strength = std::min<DWORD>(GetPrivateProfileIntA("events", "landing_max_strength", 85, ini_path), 100);
    g_landing_pulse_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "landing_pulse_ms", 160, ini_path), 1);
    g_landing_cooldown_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "landing_cooldown_ms", 600, ini_path), 1);
    g_collision_sound_enabled = GetPrivateProfileIntA("events", "collision_sound_enabled", 0, ini_path) != 0;
    g_collision_sound_min_volume = static_cast<DWORD>(std::max<LONG>(GetPrivateProfileIntA("events", "collision_sound_min_volume", 80, ini_path), 0));
    g_collision_sound_full_volume = std::max<DWORD>(static_cast<DWORD>(std::max<LONG>(GetPrivateProfileIntA("events", "collision_sound_full_volume", 650, ini_path), 0)), g_collision_sound_min_volume + 1);
    g_collision_sound_min_strength = std::min<DWORD>(GetPrivateProfileIntA("events", "collision_sound_min_strength", 18, ini_path), 100);
    g_collision_sound_max_strength = std::min<DWORD>(GetPrivateProfileIntA("events", "collision_sound_max_strength", 70, ini_path), 100);
    g_collision_sound_pulse_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "collision_sound_pulse_ms", 120, ini_path), 1);
    g_collision_sound_cooldown_ms = std::max<DWORD>(GetPrivateProfileIntA("events", "collision_sound_cooldown_ms", 140, ini_path), 1);
}

bool LoadRealDInput()
{
    if (g_real_dinput != nullptr) {
        return true;
    }

    char system_dir[MAX_PATH] = {};
    if (GetSystemDirectoryA(system_dir, MAX_PATH) == 0) {
        return false;
    }

    char path[MAX_PATH] = {};
    std::snprintf(path, MAX_PATH, "%s\\dinput8.dll", system_dir);
    g_real_dinput = LoadLibraryA(path);
    if (g_real_dinput == nullptr) {
        return false;
    }

    g_real_direct_input8_create = reinterpret_cast<DirectInput8CreateFn>(GetProcAddress(g_real_dinput, "DirectInput8Create"));
    g_real_dll_can_unload_now = reinterpret_cast<DllCanUnloadNowFn>(GetProcAddress(g_real_dinput, "DllCanUnloadNow"));
    g_real_dll_get_class_object = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(g_real_dinput, "DllGetClassObject"));
    g_real_dll_register_server = reinterpret_cast<DllRegisterServerFn>(GetProcAddress(g_real_dinput, "DllRegisterServer"));
    g_real_dll_unregister_server = reinterpret_cast<DllUnregisterServerFn>(GetProcAddress(g_real_dinput, "DllUnregisterServer"));

    Log("Loaded real dinput8.dll from %s", path);
    return g_real_direct_input8_create != nullptr;
}

void SetRumble(WORD left, WORD right, DWORD duration_ms)
{
    if (!g_enabled) {
        return;
    }

    const DWORD now = GetTickCount();
    const DWORD current_until = g_rumble_until.load();
    if (current_until != 0 && static_cast<LONG>(current_until - now) > 0) {
        left = std::max<WORD>(left, g_left_motor.load());
        right = std::max<WORD>(right, g_right_motor.load());
        duration_ms = std::max<DWORD>(duration_ms, current_until - now);
    }

    XINPUT_VIBRATION vibration{};
    vibration.wLeftMotorSpeed = left;
    vibration.wRightMotorSpeed = right;
    XInputSetState(g_controller_index, &vibration);

    g_left_motor.store(left);
    g_right_motor.store(right);
    g_rumble_until.store(now + duration_ms);
}

void StopRumble()
{
    XINPUT_VIBRATION vibration{};
    XInputSetState(g_controller_index, &vibration);
    g_left_motor.store(0);
    g_right_motor.store(0);
    g_rumble_until.store(0);
}

void ForceSetRumble(WORD left, WORD right, DWORD duration_ms)
{
    if (!g_enabled) {
        return;
    }

    XINPUT_VIBRATION vibration{};
    vibration.wLeftMotorSpeed = left;
    vibration.wRightMotorSpeed = right;
    XInputSetState(g_controller_index, &vibration);

    g_left_motor.store(left);
    g_right_motor.store(right);
    g_rumble_until.store(GetTickCount() + duration_ms);
}

void EventRumble(DWORD left_strength, DWORD right_strength, DWORD duration_ms)
{
    const WORD left = static_cast<WORD>(65535 * std::min<DWORD>(left_strength, 100) * g_max_strength / 10000);
    const WORD right = static_cast<WORD>(65535 * std::min<DWORD>(right_strength, 100) * g_max_strength / 10000);
    SetRumble(left, right, duration_ms);
}

void EventScrapeRumble(DWORD left_strength, DWORD right_strength, DWORD duration_ms)
{
    const WORD high_left = static_cast<WORD>(65535 * std::min<DWORD>(left_strength, 100) * g_max_strength / 10000);
    const WORD high_right = static_cast<WORD>(65535 * std::min<DWORD>(right_strength, 100) * g_max_strength / 10000);
    const WORD low_left = static_cast<WORD>(high_left * g_scrape_low_percent / 100);
    const WORD low_right = static_cast<WORD>(high_right * g_scrape_low_percent / 100);
    const DWORD now = GetTickCount();

    g_scrape_high_left.store(high_left);
    g_scrape_high_right.store(high_right);
    g_scrape_low_left.store(low_left);
    g_scrape_low_right.store(low_right);
    g_scrape_until.store(now + duration_ms);
    g_scrape_next_grain.store(now + g_scrape_grain_ms);
    g_scrape_high_active.store(true);
    ForceSetRumble(high_left, high_right, g_scrape_grain_ms);
}

void EventRumbleEnvelope(DWORD left_strength, DWORD right_strength, DWORD duration_ms)
{
    const DWORD peak_strength = std::max<DWORD>(left_strength, right_strength);
    if (!g_envelope_enabled || peak_strength < g_envelope_min_strength) {
        EventRumble(left_strength, right_strength, duration_ms);
        return;
    }

    const DWORD clamped_left = std::min<DWORD>(left_strength, 100);
    const DWORD clamped_right = std::min<DWORD>(right_strength, 100);
    const DWORD kick_scale = std::max<DWORD>(g_envelope_kick_strength, peak_strength);
    const DWORD kick_left_strength = std::min<DWORD>(clamped_left * kick_scale / std::max<DWORD>(peak_strength, 1), 100);
    const DWORD kick_right_strength = std::min<DWORD>(clamped_right * kick_scale / std::max<DWORD>(peak_strength, 1), 100);
    const DWORD sustain_left_strength = clamped_left * g_envelope_sustain_percent / 100;
    const DWORD sustain_right_strength = clamped_right * g_envelope_sustain_percent / 100;
    const DWORD sustain_ms = std::min<DWORD>(
        std::max<DWORD>(duration_ms, g_envelope_sustain_min_ms),
        g_envelope_sustain_max_ms
    );

    const WORD kick_left = static_cast<WORD>(65535 * kick_left_strength * g_max_strength / 10000);
    const WORD kick_right = static_cast<WORD>(65535 * kick_right_strength * g_max_strength / 10000);
    const WORD sustain_left = static_cast<WORD>(65535 * sustain_left_strength * g_max_strength / 10000);
    const WORD sustain_right = static_cast<WORD>(65535 * sustain_right_strength * g_max_strength / 10000);
    const DWORD now = GetTickCount();

    g_envelope_kick_left.store(kick_left);
    g_envelope_kick_right.store(kick_right);
    g_envelope_sustain_left.store(sustain_left);
    g_envelope_sustain_right.store(sustain_right);
    g_envelope_start.store(now);
    g_envelope_kick_until.store(now + g_envelope_kick_ms);
    g_envelope_gap_until.store(now + g_envelope_kick_ms + g_envelope_gap_ms);
    g_envelope_until.store(now + g_envelope_kick_ms + g_envelope_gap_ms + sustain_ms);
    g_envelope_gap_active.store(false);
    g_envelope_sustain_active.store(false);
    g_envelope_id.fetch_add(1);

    ForceSetRumble(kick_left, kick_right, g_envelope_kick_ms);
}

float FloatFromBits(DWORD bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

struct Vec3
{
    float x;
    float y;
    float z;
};

float DotXZ(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.z * b.z;
}

Vec3 Sub(const Vec3& a, const Vec3& b)
{
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

DWORD ClampPercent(int value)
{
    return static_cast<DWORD>(std::min<int>(std::max<int>(value, 0), 100));
}

DWORD ReadSelectedCarIndex()
{
    if (g_selected_car_index_address == 0) {
        return 0xffffffffu;
    }

    return *reinterpret_cast<const DWORD*>(g_selected_car_index_address);
}

DWORD SafeReadDword(DWORD address)
{
    if (address == 0) {
        return 0;
    }

    __try {
        return *reinterpret_cast<const DWORD*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

float SafeReadFloat(DWORD address)
{
    const DWORD bits = SafeReadDword(address);
    return FloatFromBits(bits);
}

Vec3 SafeReadVec3(DWORD address)
{
    Vec3 value{};
    value.x = SafeReadFloat(address);
    value.y = SafeReadFloat(address + 4);
    value.z = SafeReadFloat(address + 8);
    return value;
}

struct CarIdentity
{
    DWORD race_info;
    DWORD race_info_364;
    DWORD race_info_368;
    DWORD race_info_36c;
    DWORD race_info_480;
    DWORD race_info_4e8;
    DWORD race_info_4f0;
    DWORD car_6b10;
    DWORD car_6b18;
};

CarIdentity ReadCarIdentity(DWORD car_ptr)
{
    CarIdentity identity{};
    identity.race_info = SafeReadDword(car_ptr + 0x463c);
    identity.race_info_364 = SafeReadDword(identity.race_info + 0x364);
    identity.race_info_368 = SafeReadDword(identity.race_info + 0x368);
    identity.race_info_36c = SafeReadDword(identity.race_info + 0x36c);
    identity.race_info_480 = SafeReadDword(identity.race_info + 0x480);
    identity.race_info_4e8 = SafeReadDword(identity.race_info + 0x4e8);
    identity.race_info_4f0 = SafeReadDword(identity.race_info + 0x4f0);
    identity.car_6b10 = SafeReadDword(car_ptr + 0x6b10);
    identity.car_6b18 = SafeReadDword(car_ptr + 0x6b18);
    return identity;
}

bool IsLikelyPlayerCar(const CarIdentity& identity)
{
    return identity.race_info != 0 && identity.race_info_36c == 1 && identity.car_6b10 == 0;
}

bool ShouldRumbleForCar(DWORD car_index, const CarIdentity& identity)
{
    if (!g_damage_player_only) {
        return true;
    }

    if (g_damage_player_index >= 0) {
        return car_index == static_cast<DWORD>(g_damage_player_index);
    }

    return IsLikelyPlayerCar(identity);
}

DWORD ScaleStrength(
    DWORD value,
    DWORD min_value,
    DWORD full_value,
    DWORD min_strength,
    DWORD max_strength
)
{
    const DWORD span = std::max<DWORD>(full_value - min_value, 1);
    const DWORD clamped = value > min_value ? std::min<DWORD>(value - min_value, span) : 0;
    const DWORD strength_span = max_strength > min_strength ? max_strength - min_strength : 0;
    return std::min<DWORD>(min_strength + strength_span * clamped / span, 100);
}

struct DirectionalRumble
{
    DWORD left_strength;
    DWORD right_strength;
    LONG side_per_mille;
};

DirectionalRumble ApplySideRumbleWithTuning(DWORD strength, LONG side_per_mille, DWORD directional_bias, DWORD directional_min_motor)
{
    DirectionalRumble rumble{strength, strength, side_per_mille};
    if (!g_directional_enabled || strength == 0) {
        return rumble;
    }

    if (rumble.side_per_mille > 1000) {
        rumble.side_per_mille = 1000;
    } else if (rumble.side_per_mille < -1000) {
        rumble.side_per_mille = -1000;
    }

    const LONG side_abs_int = rumble.side_per_mille < 0 ? -rumble.side_per_mille : rumble.side_per_mille;
    const DWORD effective_bias = directional_bias * static_cast<DWORD>(side_abs_int) / 1000;
    const DWORD weak_from_bias = strength * (100 - effective_bias) / 100;
    const DWORD weak_floor = strength * directional_min_motor / 100;
    const DWORD weak = std::max<DWORD>(weak_from_bias, weak_floor);

    if (rumble.side_per_mille > 50) {
        rumble.left_strength = weak;
        rumble.right_strength = strength;
    } else if (rumble.side_per_mille < -50) {
        rumble.left_strength = strength;
        rumble.right_strength = weak;
    }
    return rumble;
}

LONG ReadSteeringSidePerMille()
{
    XINPUT_STATE state{};
    if (XInputGetState(g_controller_index, &state) != ERROR_SUCCESS) {
        return 0;
    }

    const SHORT thumb = state.Gamepad.sThumbLX;
    const LONG abs_thumb = thumb < 0 ? -static_cast<LONG>(thumb) : static_cast<LONG>(thumb);
    if (abs_thumb < static_cast<LONG>(g_scrape_steering_threshold)) {
        return 0;
    }

    LONG side = abs_thumb * 1000 / 32767;
    if (side > 1000) {
        side = 1000;
    }
    return thumb < 0 ? -side : side;
}

DirectionalRumble ApplyDirectionalRumbleWithTuning(
    DWORD car_ptr,
    DWORD contact_ptr,
    DWORD strength,
    DWORD directional_bias,
    DWORD directional_min_motor
)
{
    DirectionalRumble rumble{strength, strength, 0};
    if (!g_directional_enabled || car_ptr == 0 || contact_ptr == 0 || strength == 0) {
        return rumble;
    }

    const Vec3 hit = SafeReadVec3(contact_ptr);
    const Vec3 p0 = SafeReadVec3(car_ptr + 0x0a70);
    const Vec3 p1 = SafeReadVec3(car_ptr + 0x0a70 + 0x03a0);
    const Vec3 p2 = SafeReadVec3(car_ptr + 0x0a70 + 0x0740);
    const Vec3 p3 = SafeReadVec3(car_ptr + 0x0a70 + 0x0ae0);

    const Vec3 center{
        (p0.x + p1.x + p2.x + p3.x) * 0.25f,
        (p0.y + p1.y + p2.y + p3.y) * 0.25f,
        (p0.z + p1.z + p2.z + p3.z) * 0.25f
    };
    const Vec3 side_axis{
        (p1.x + p3.x - p0.x - p2.x) * 0.5f,
        0.0f,
        (p1.z + p3.z - p0.z - p2.z) * 0.5f
    };

    const float axis_len_sq = DotXZ(side_axis, side_axis);
    if (!(axis_len_sq > 0.001f)) {
        return rumble;
    }

    float side = DotXZ(Sub(hit, center), side_axis) / axis_len_sq;
    if (g_directional_invert) {
        side = -side;
    }
    if (side > 1.0f) {
        side = 1.0f;
    } else if (side < -1.0f) {
        side = -1.0f;
    }

    return ApplySideRumbleWithTuning(strength, static_cast<LONG>(side * 1000.0f), directional_bias, directional_min_motor);
}

DirectionalRumble ApplyDirectionalRumble(DWORD car_ptr, DWORD contact_ptr, DWORD strength)
{
    return ApplyDirectionalRumbleWithTuning(
        car_ptr,
        contact_ptr,
        strength,
        g_directional_bias,
        g_directional_min_motor
    );
}

void RememberPlayerContact(
    DWORD time,
    DWORD object,
    DWORD car,
    DWORD type,
    const DirectionalRumble& rumble
)
{
    if (object == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_player_contact_mutex);
    PlayerContactRecord& record = g_player_contacts[g_player_contact_index % 16];
    record.time = time;
    record.object = object;
    record.car = car;
    record.type = type;
    record.left_strength = rumble.left_strength;
    record.right_strength = rumble.right_strength;
    record.side_per_mille = rumble.side_per_mille;
    ++g_player_contact_index;
}

bool FindRecentPlayerContactForCar(DWORD car_ptr, DWORD now, PlayerContactRecord* match)
{
    if (car_ptr == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_player_contact_mutex);
    for (const PlayerContactRecord& record : g_player_contacts) {
        if (record.time == 0 || now - record.time > g_offense_damage_window_ms) {
            continue;
        }

        const bool same_car = record.car == car_ptr;
        const bool same_body = record.object != 0 && SafeReadDword(record.object + 0x144) == car_ptr;
        if (same_car || same_body) {
            if (match != nullptr) {
                *match = record;
            }
            return true;
        }
    }

    return false;
}

void OnContactEvent(DWORD car_ptr, DWORD contact_ptr, DWORD other_ptr, DWORD contact_bits)
{
    if (!g_enabled || !g_events_enabled || !g_contact_enabled) {
        return;
    }

    const CarIdentity identity = ReadCarIdentity(car_ptr);
    if (g_damage_player_only && !IsLikelyPlayerCar(identity)) {
        return;
    }

    const float contact = FloatFromBits(contact_bits);
    if (!(contact > 0.0f)) {
        return;
    }

    const DWORD now = GetTickCount();
    const DWORD contact_per_mille = static_cast<DWORD>(std::min<float>(contact * 1000.0f, 1000000.0f));
    const DWORD other_type = other_ptr != 0 ? SafeReadDword(other_ptr + 0xa0) : 0;
    const DWORD other_car = other_ptr != 0 ? SafeReadDword(other_ptr + 0x144) : 0;

    const DirectionalRumble contact_direction = ApplyDirectionalRumble(car_ptr, contact_ptr, 100);
    RememberPlayerContact(now, other_ptr, other_car, other_type, contact_direction);

    const DirectionalRumble world_scrape_probe = ApplyDirectionalRumbleWithTuning(
        car_ptr,
        contact_ptr,
        100,
        g_scrape_directional_bias,
        g_scrape_directional_min_motor
    );
    const LONG steering_side = ReadSteeringSidePerMille();
    const LONG world_scrape_side =
        world_scrape_probe.side_per_mille != 0 ? world_scrape_probe.side_per_mille : steering_side;
    const LONG world_scrape_side_abs = world_scrape_side < 0 ? -world_scrape_side : world_scrape_side;

    if (other_ptr == 0 && g_scrape_enabled && contact_per_mille >= g_scrape_min_delta_per_mille && static_cast<DWORD>(world_scrape_side_abs) >= g_scrape_min_side_per_mille) {
        const DWORD last_scrape = g_last_scrape_rumble.load();
        if (last_scrape == 0 || now - last_scrape >= g_scrape_cooldown_ms) {
            const DWORD strength = ScaleStrength(
                contact_per_mille,
                g_scrape_min_delta_per_mille,
                g_scrape_full_delta_per_mille,
                g_scrape_min_strength,
                g_scrape_max_strength
            );
            const DirectionalRumble scrape = ApplySideRumbleWithTuning(
                strength,
                world_scrape_side,
                g_scrape_directional_bias,
                g_scrape_directional_min_motor
            );

            g_last_scrape_rumble.store(now);
            EventScrapeRumble(scrape.left_strength, scrape.right_strength, g_scrape_pulse_ms);
            Log(
                "World scrape rumble: car=0x%08lX contact_ptr=0x%08lX contact=%.4f strength=%lu left=%lu right=%lu side=%ld steering_side=%ld duration_ms=%lu",
                static_cast<unsigned long>(car_ptr),
                static_cast<unsigned long>(contact_ptr),
                contact,
                static_cast<unsigned long>(strength),
                static_cast<unsigned long>(scrape.left_strength),
                static_cast<unsigned long>(scrape.right_strength),
                static_cast<long>(scrape.side_per_mille),
                static_cast<long>(steering_side),
                static_cast<unsigned long>(g_scrape_pulse_ms)
            );
        }
        return;
    }

    if (other_ptr == 0) {
        const DWORD last_ground = g_last_ground_contact.exchange(now);
        if (!g_landing_enabled) {
            return;
        }

        if (last_ground != 0 && now - last_ground < g_landing_airborne_gap_ms) {
            return;
        }

        if (contact_per_mille < g_landing_min_delta_per_mille) {
            return;
        }

        const DWORD last_landing = g_last_landing_rumble.load();
        if (last_landing != 0 && now - last_landing < g_landing_cooldown_ms) {
            return;
        }

        const DWORD span = std::max<DWORD>(g_landing_full_delta_per_mille - g_landing_min_delta_per_mille, 1);
        const DWORD clamped = std::min<DWORD>(contact_per_mille - g_landing_min_delta_per_mille, span);
        const DWORD strength_span = g_landing_max_strength > g_landing_min_strength ? g_landing_max_strength - g_landing_min_strength : 0;
        const DWORD strength = std::min<DWORD>(g_landing_min_strength + strength_span * clamped / span, 100);

        g_last_landing_rumble.store(now);
        EventRumble(strength, strength, g_landing_pulse_ms);
        Log(
            "Landing rumble: car=0x%08lX contact_ptr=0x%08lX contact=%.4f strength=%lu duration_ms=%lu",
            static_cast<unsigned long>(car_ptr),
            static_cast<unsigned long>(contact_ptr),
            contact,
            static_cast<unsigned long>(strength),
            static_cast<unsigned long>(g_landing_pulse_ms)
        );
        return;
    }

    const bool car_contact = other_type == 4;

    if (!car_contact && g_scrape_enabled && contact_per_mille >= g_scrape_min_delta_per_mille) {
        const DirectionalRumble scrape_probe = ApplyDirectionalRumbleWithTuning(
            car_ptr,
            contact_ptr,
            100,
            g_scrape_directional_bias,
            g_scrape_directional_min_motor
        );
        const LONG scrape_side = scrape_probe.side_per_mille != 0 ? scrape_probe.side_per_mille : steering_side;
        const LONG side_abs = scrape_side < 0 ? -scrape_side : scrape_side;
        if (static_cast<DWORD>(side_abs) >= g_scrape_min_side_per_mille) {
            const DWORD last_scrape = g_last_scrape_rumble.load();
            if (last_scrape != 0 && now - last_scrape < g_scrape_cooldown_ms) {
                return;
            }

            const DWORD strength = ScaleStrength(
                contact_per_mille,
                g_scrape_min_delta_per_mille,
                g_scrape_full_delta_per_mille,
                g_scrape_min_strength,
                g_scrape_max_strength
            );
            const DirectionalRumble scrape = ApplySideRumbleWithTuning(
                strength,
                scrape_side,
                g_scrape_directional_bias,
                g_scrape_directional_min_motor
            );

            g_last_scrape_rumble.store(now);
            EventScrapeRumble(scrape.left_strength, scrape.right_strength, g_scrape_pulse_ms);
            Log(
                "Scrape rumble: car=0x%08lX contact_ptr=0x%08lX other=0x%08lX other_car=0x%08lX other_type=%lu contact=%.4f strength=%lu left=%lu right=%lu side=%ld steering_side=%ld duration_ms=%lu",
                static_cast<unsigned long>(car_ptr),
                static_cast<unsigned long>(contact_ptr),
                static_cast<unsigned long>(other_ptr),
                static_cast<unsigned long>(other_car),
                static_cast<unsigned long>(other_type),
                contact,
                static_cast<unsigned long>(strength),
                static_cast<unsigned long>(scrape.left_strength),
                static_cast<unsigned long>(scrape.right_strength),
                static_cast<long>(scrape.side_per_mille),
                static_cast<long>(steering_side),
                static_cast<unsigned long>(g_scrape_pulse_ms)
            );
            return;
        }
    }

    if (contact_per_mille < g_contact_min_delta_per_mille) {
        return;
    }

    if (!car_contact && !g_rubble_contact_enabled) {
        return;
    }

    const DWORD cooldown_ms = car_contact ? g_contact_car_cooldown_ms : g_contact_cooldown_ms;
    auto& last_rumble = car_contact ? g_last_car_contact_rumble : g_last_contact_rumble;
    const DWORD last = last_rumble.load();
    if (last != 0 && now - last < cooldown_ms) {
        return;
    }

    const DWORD span = std::max<DWORD>(g_contact_full_delta_per_mille - g_contact_min_delta_per_mille, 1);
    const DWORD clamped = std::min<DWORD>(contact_per_mille - g_contact_min_delta_per_mille, span);
    const DWORD min_strength = car_contact ? g_contact_car_min_strength : g_contact_min_strength;
    const DWORD max_strength = car_contact ? g_contact_car_max_strength : g_contact_max_strength;
    const DWORD pulse_ms = car_contact ? g_contact_car_pulse_ms : g_contact_pulse_ms;
    const DWORD strength_span = max_strength > min_strength ? max_strength - min_strength : 0;
    const DWORD strength = std::min<DWORD>(min_strength + strength_span * clamped / span, 100);
    const DirectionalRumble directional = ApplyDirectionalRumble(car_ptr, contact_ptr, strength);

    last_rumble.store(now);
    if (car_contact) {
        EventRumbleEnvelope(directional.left_strength, directional.right_strength, pulse_ms);
    } else {
        EventRumble(directional.left_strength, directional.right_strength, pulse_ms);
    }
    Log(
        "Contact event rumble: car=0x%08lX contact_ptr=0x%08lX other=0x%08lX other_car=0x%08lX other_type=%lu info=0x%08lX i36c=%lu c6b10=%lu contact=%.4f strength=%lu left=%lu right=%lu side=%ld duration_ms=%lu",
        static_cast<unsigned long>(car_ptr),
        static_cast<unsigned long>(contact_ptr),
        static_cast<unsigned long>(other_ptr),
        static_cast<unsigned long>(other_car),
        static_cast<unsigned long>(other_type),
        static_cast<unsigned long>(identity.race_info),
        static_cast<unsigned long>(identity.race_info_36c),
        static_cast<unsigned long>(identity.car_6b10),
        contact,
        static_cast<unsigned long>(strength),
        static_cast<unsigned long>(directional.left_strength),
        static_cast<unsigned long>(directional.right_strength),
        static_cast<long>(directional.side_per_mille),
        static_cast<unsigned long>(pulse_ms)
    );
}

void OnCollisionSoundEvent(DWORD volume, DWORD sound_type)
{
    if (!g_enabled || !g_events_enabled || !g_collision_sound_enabled) {
        return;
    }

    if (volume < g_collision_sound_min_volume) {
        return;
    }

    const DWORD now = GetTickCount();
    const DWORD last = g_last_collision_sound_rumble.load();
    if (last != 0 && now - last < g_collision_sound_cooldown_ms) {
        return;
    }

    const DWORD span = std::max<DWORD>(g_collision_sound_full_volume - g_collision_sound_min_volume, 1);
    const DWORD clamped = std::min<DWORD>(volume - g_collision_sound_min_volume, span);
    const DWORD strength_span = g_collision_sound_max_strength > g_collision_sound_min_strength ? g_collision_sound_max_strength - g_collision_sound_min_strength : 0;
    const DWORD strength = std::min<DWORD>(g_collision_sound_min_strength + strength_span * clamped / span, 100);

    g_last_collision_sound_rumble.store(now);
    EventRumble(strength, strength, g_collision_sound_pulse_ms);
    Log(
        "Collision sound rumble: type=0x%lX volume=%lu strength=%lu duration_ms=%lu",
        static_cast<unsigned long>(sound_type),
        static_cast<unsigned long>(volume),
        static_cast<unsigned long>(strength),
        static_cast<unsigned long>(g_collision_sound_pulse_ms)
    );
}

void OnDamageEvent(DWORD car_ptr, DWORD car_index, DWORD old_damage_bits, DWORD new_damage_bits)
{
    if (!g_enabled || !g_events_enabled || !g_damage_enabled) {
        return;
    }

    const float old_damage = FloatFromBits(old_damage_bits);
    const float new_damage = FloatFromBits(new_damage_bits);
    const float delta = new_damage - old_damage;
    if (!(delta > 0.0f)) {
        return;
    }

    const DWORD delta_per_mille = static_cast<DWORD>(std::min<float>(delta * 1000.0f, 1000.0f));
    if (delta_per_mille < g_damage_min_delta_per_mille) {
        return;
    }

    const CarIdentity identity = ReadCarIdentity(car_ptr);
    const bool should_rumble = ShouldRumbleForCar(car_index, identity);
    Log(
        "Damage candidate: rumble=%d car=0x%08lX idx=%lu selected=%lu cfg_idx=%ld info=0x%08lX i364=%lu i368=%lu i36c=%lu i480=%lu i4e8=%lu i4f0=%lu c6b10=%lu c6b18=%lu old=%.4f new=%.4f delta=%.4f",
        should_rumble ? 1 : 0,
        static_cast<unsigned long>(car_ptr),
        static_cast<unsigned long>(car_index),
        static_cast<unsigned long>(ReadSelectedCarIndex()),
        static_cast<long>(g_damage_player_index),
        static_cast<unsigned long>(identity.race_info),
        static_cast<unsigned long>(identity.race_info_364),
        static_cast<unsigned long>(identity.race_info_368),
        static_cast<unsigned long>(identity.race_info_36c),
        static_cast<unsigned long>(identity.race_info_480),
        static_cast<unsigned long>(identity.race_info_4e8),
        static_cast<unsigned long>(identity.race_info_4f0),
        static_cast<unsigned long>(identity.car_6b10),
        static_cast<unsigned long>(identity.car_6b18),
        old_damage,
        new_damage,
        delta
    );

    const DWORD now = GetTickCount();
    if (!should_rumble && g_offense_damage_enabled) {
        PlayerContactRecord contact_match{};
        if (FindRecentPlayerContactForCar(car_ptr, now, &contact_match)) {
            const DWORD last = g_last_offense_damage_rumble.load();
            if (last == 0 || now - last >= g_offense_damage_cooldown_ms) {
                const DWORD strength = ScaleStrength(
                    delta_per_mille,
                    g_damage_min_delta_per_mille,
                    g_damage_full_delta_per_mille,
                    g_offense_damage_min_strength,
                    g_offense_damage_max_strength
                );
                const DWORD left_strength = contact_match.left_strength != 0 ? strength * contact_match.left_strength / 100 : strength;
                const DWORD right_strength = contact_match.right_strength != 0 ? strength * contact_match.right_strength / 100 : strength;

                g_last_offense_damage_rumble.store(now);
                EventRumbleEnvelope(left_strength, right_strength, g_offense_damage_pulse_ms);
                Log(
                    "Offense damage rumble: damaged_car=0x%08lX contact_object=0x%08lX contact_car=0x%08lX contact_type=%lu age_ms=%lu strength=%lu left=%lu right=%lu side=%ld duration_ms=%lu",
                    static_cast<unsigned long>(car_ptr),
                    static_cast<unsigned long>(contact_match.object),
                    static_cast<unsigned long>(contact_match.car),
                    static_cast<unsigned long>(contact_match.type),
                    static_cast<unsigned long>(now - contact_match.time),
                    static_cast<unsigned long>(strength),
                    static_cast<unsigned long>(left_strength),
                    static_cast<unsigned long>(right_strength),
                    static_cast<long>(contact_match.side_per_mille),
                    static_cast<unsigned long>(g_offense_damage_pulse_ms)
                );
            }
        }
        return;
    }

    if (!should_rumble) {
        return;
    }

    const DWORD last = g_last_damage_rumble.load();
    if (last != 0 && now - last < g_damage_cooldown_ms) {
        return;
    }

    const DWORD span = std::max<DWORD>(g_damage_full_delta_per_mille - g_damage_min_delta_per_mille, 1);
    const DWORD clamped = std::min<DWORD>(delta_per_mille - g_damage_min_delta_per_mille, span);
    const DWORD strength_span = g_damage_max_strength > g_damage_min_strength ? g_damage_max_strength - g_damage_min_strength : 0;
    const DWORD strength = std::min<DWORD>(g_damage_min_strength + strength_span * clamped / span, 100);

    g_last_damage_rumble.store(now);
    EventRumbleEnvelope(strength, strength, g_damage_pulse_ms);
    Log("Damage event rumble: car_ptr=0x%08lX car_index=%lu strength=%lu duration_ms=%lu", static_cast<unsigned long>(car_ptr), static_cast<unsigned long>(car_index), static_cast<unsigned long>(strength), static_cast<unsigned long>(g_damage_pulse_ms));
}

__declspec(naked) void ContactHook()
{
    __asm {
        pushfd
        pushad
        mov eax, [ecx + 0x34]
        mov edx, [ecx + 0x24]
        push edx
        push eax
        push ecx
        push ebp
        call OnContactEvent
        add esp, 16
        popad
        popfd
        mov eax, [ecx + 0x34]
        xor edx, edx
        jmp dword ptr [g_contact_continue]
    }
}

__declspec(naked) void DamageHook()
{
    __asm {
        pushfd
        pushad
        mov ecx, [esp + 0x84]
        mov edx, [ebp + 0x6aa0]
        mov eax, [ebp + 0x6b14]
        push edx
        push ecx
        push eax
        push ebp
        call OnDamageEvent
        add esp, 16
        popad
        popfd
        xor edi, edi
        lea esi, [ebp + 0x0a78]
        jmp dword ptr [g_damage_continue]
    }
}

__declspec(naked) void CollisionSoundHook()
{
    __asm {
        pushfd
        pushad
        mov eax, [esp + 0x1c]
        mov ecx, [esp + 0x38]
        push ecx
        push eax
        call OnCollisionSoundEvent
        add esp, 8
        popad
        popfd
        push eax
        push esi
        call dword ptr [g_collision_sound_set_volume]
        jmp dword ptr [g_collision_sound_continue]
    }
}

bool InstallJumpHook(void* target, void* hook, size_t patch_size)
{
    if (target == nullptr || hook == nullptr || patch_size < 5) {
        return false;
    }

    DWORD old_protect = 0;
    if (!VirtualProtect(target, patch_size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        Log("VirtualProtect failed while installing hook at %p", target);
        return false;
    }

    auto* patch = static_cast<std::uint8_t*>(target);
    patch[0] = 0xE9;
    *reinterpret_cast<std::int32_t*>(patch + 1) =
        reinterpret_cast<std::uint8_t*>(hook) - (patch + 5);
    for (size_t i = 5; i < patch_size; ++i) {
        patch[i] = 0x90;
    }

    DWORD ignored = 0;
    VirtualProtect(target, patch_size, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, patch_size);
    return true;
}

void InstallGameHooks()
{
    if (g_hooks_installed.exchange(true)) {
        return;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
    if (base == 0) {
        Log("Could not find game module base for event hooks");
        return;
    }

    g_contact_continue = base + 0x26eab;
    g_damage_continue = base + 0x26f3c;
    g_collision_sound_continue = base + 0x16a8e;
    g_collision_sound_set_volume = base + 0x2155a8;
    g_selected_car_index_address = base + 0x4da4e4;
    const bool contact_ok = InstallJumpHook(
        reinterpret_cast<void*>(base + 0x26ea6),
        reinterpret_cast<void*>(&ContactHook),
        5
    );
    const bool damage_ok = InstallJumpHook(
        reinterpret_cast<void*>(base + 0x26f34),
        reinterpret_cast<void*>(&DamageHook),
        8
    );
    Log("Event hooks installed: contact=%d damage=%d collision_sound=0", contact_ok ? 1 : 0, damage_ok ? 1 : 0);
}

DWORD WINAPI RumbleWorker(LPVOID)
{
    WORD previous_buttons = 0;

    while (g_worker_running.load()) {
        const DWORD envelope_until = g_envelope_until.load();
        if (envelope_until != 0) {
            const DWORD now = GetTickCount();
            if (static_cast<LONG>(now - envelope_until) >= 0) {
                g_envelope_until.store(0);
                g_envelope_gap_active.store(false);
                g_envelope_sustain_active.store(false);
            } else if (!g_envelope_gap_active.load() && static_cast<LONG>(now - g_envelope_kick_until.load()) >= 0) {
                g_envelope_gap_active.store(true);
                if (g_envelope_gap_ms > 0) {
                    ForceSetRumble(0, 0, g_envelope_gap_ms);
                }
            } else if (!g_envelope_sustain_active.load() && static_cast<LONG>(now - g_envelope_gap_until.load()) >= 0) {
                g_envelope_sustain_active.store(true);
                const DWORD remaining = envelope_until > now ? envelope_until - now : 1;
                ForceSetRumble(g_envelope_sustain_left.load(), g_envelope_sustain_right.load(), remaining);
            }
        }

        const DWORD until = g_rumble_until.load();
        if (until != 0 && static_cast<LONG>(GetTickCount() - until) >= 0) {
            StopRumble();
        }

        const DWORD scrape_until = g_scrape_until.load();
        if (scrape_until != 0) {
            const DWORD now = GetTickCount();
            if (static_cast<LONG>(now - scrape_until) >= 0) {
                g_scrape_until.store(0);
                g_scrape_high_active.store(false);
            } else if (g_envelope_until.load() == 0 && static_cast<LONG>(now - g_scrape_next_grain.load()) >= 0) {
                const bool high = !g_scrape_high_active.load();
                g_scrape_high_active.store(high);
                g_scrape_next_grain.store(now + g_scrape_grain_ms);
                if (high) {
                    ForceSetRumble(g_scrape_high_left.load(), g_scrape_high_right.load(), g_scrape_grain_ms);
                } else {
                    ForceSetRumble(g_scrape_low_left.load(), g_scrape_low_right.load(), g_scrape_grain_ms);
                }
            }
        }

        if (g_enabled && g_synthetic_enabled) {
            XINPUT_STATE state{};
            if (XInputGetState(g_controller_index, &state) == ERROR_SUCCESS) {
                const WORD pressed = state.Gamepad.wButtons & static_cast<WORD>(g_synthetic_button_mask);
                const WORD newly_pressed = pressed & ~previous_buttons;
                const bool trigger_gate =
                    state.Gamepad.bLeftTrigger >= g_synthetic_require_left_trigger &&
                    state.Gamepad.bRightTrigger >= g_synthetic_require_right_trigger;
                if (newly_pressed != 0 && trigger_gate) {
                    const WORD left = static_cast<WORD>(65535 * g_synthetic_left_strength * g_max_strength / 10000);
                    const WORD right = static_cast<WORD>(65535 * g_synthetic_right_strength * g_max_strength / 10000);
                    SetRumble(left, right, g_synthetic_pulse_ms);
                    Log("Synthetic rumble pulse: buttons=0x%04X left=%u right=%u duration_ms=%lu", newly_pressed, static_cast<unsigned>(left), static_cast<unsigned>(right), static_cast<unsigned long>(g_synthetic_pulse_ms));
                }
                previous_buttons = pressed;
            } else {
                previous_buttons = 0;
            }
        }

        Sleep(16);
    }

    StopRumble();
    return 0;
}

DWORD DurationToMs(DWORD duration)
{
    if (duration == 0 || duration == INFINITE) {
        return g_default_duration_ms;
    }

    const DWORD ms = std::max<DWORD>(duration / 1000, 1);
    return std::min<DWORD>(ms, 3000);
}

WORD ScaleMagnitude(DWORD magnitude, DWORD gain)
{
    magnitude = std::min<DWORD>(magnitude, DI_FFNOMINALMAX);
    gain = gain == 0 ? DI_FFNOMINALMAX : std::min<DWORD>(gain, DI_FFNOMINALMAX);

    const DWORD scaled = magnitude * gain / DI_FFNOMINALMAX * g_max_strength / 100;
    return static_cast<WORD>(std::min<DWORD>(scaled * 65535 / DI_FFNOMINALMAX, 65535));
}

DWORD ReadMagnitude(REFGUID guid, const DIEFFECT* effect)
{
    if (effect == nullptr || effect->lpvTypeSpecificParams == nullptr || effect->cbTypeSpecificParams == 0) {
        return 6000;
    }

    if (IsEqualGUID(guid, GUID_ConstantForce) && effect->cbTypeSpecificParams >= sizeof(DICONSTANTFORCE)) {
        const auto* force = static_cast<const DICONSTANTFORCE*>(effect->lpvTypeSpecificParams);
        return static_cast<DWORD>(std::min<LONG>(std::abs(force->lMagnitude), DI_FFNOMINALMAX));
    }

    if ((IsEqualGUID(guid, GUID_Sine) ||
         IsEqualGUID(guid, GUID_Square) ||
         IsEqualGUID(guid, GUID_Triangle) ||
         IsEqualGUID(guid, GUID_SawtoothUp) ||
         IsEqualGUID(guid, GUID_SawtoothDown)) &&
        effect->cbTypeSpecificParams >= sizeof(DIPERIODIC)) {
        const auto* periodic = static_cast<const DIPERIODIC*>(effect->lpvTypeSpecificParams);
        return std::min<DWORD>(periodic->dwMagnitude, DI_FFNOMINALMAX);
    }

    if ((IsEqualGUID(guid, GUID_Spring) ||
         IsEqualGUID(guid, GUID_Damper) ||
         IsEqualGUID(guid, GUID_Inertia) ||
         IsEqualGUID(guid, GUID_Friction)) &&
        effect->cbTypeSpecificParams >= sizeof(DICONDITION)) {
        const auto* condition = static_cast<const DICONDITION*>(effect->lpvTypeSpecificParams);
        LONG magnitude = 0;
        magnitude = std::max<LONG>(magnitude, std::abs(condition->lPositiveCoefficient));
        magnitude = std::max<LONG>(magnitude, std::abs(condition->lNegativeCoefficient));
        magnitude = std::max<LONG>(magnitude, static_cast<LONG>(condition->dwPositiveSaturation));
        magnitude = std::max<LONG>(magnitude, static_cast<LONG>(condition->dwNegativeSaturation));
        return static_cast<DWORD>(std::min<LONG>(magnitude, DI_FFNOMINALMAX));
    }

    return 6000;
}

const char* EffectName(REFGUID guid)
{
    if (IsEqualGUID(guid, GUID_ConstantForce)) return "Constant Force";
    if (IsEqualGUID(guid, GUID_Sine)) return "Sine";
    if (IsEqualGUID(guid, GUID_Square)) return "Square";
    if (IsEqualGUID(guid, GUID_Triangle)) return "Triangle";
    if (IsEqualGUID(guid, GUID_SawtoothUp)) return "Sawtooth Up";
    if (IsEqualGUID(guid, GUID_SawtoothDown)) return "Sawtooth Down";
    if (IsEqualGUID(guid, GUID_Spring)) return "Spring";
    if (IsEqualGUID(guid, GUID_Damper)) return "Damper";
    if (IsEqualGUID(guid, GUID_Inertia)) return "Inertia";
    if (IsEqualGUID(guid, GUID_Friction)) return "Friction";
    return "XInput Rumble";
}

DWORD EffectType(REFGUID guid)
{
    if (IsEqualGUID(guid, GUID_ConstantForce)) return DIEFT_CONSTANTFORCE;
    if (IsEqualGUID(guid, GUID_Sine) ||
        IsEqualGUID(guid, GUID_Square) ||
        IsEqualGUID(guid, GUID_Triangle) ||
        IsEqualGUID(guid, GUID_SawtoothUp) ||
        IsEqualGUID(guid, GUID_SawtoothDown)) {
        return DIEFT_PERIODIC;
    }
    if (IsEqualGUID(guid, GUID_Spring) ||
        IsEqualGUID(guid, GUID_Damper) ||
        IsEqualGUID(guid, GUID_Inertia) ||
        IsEqualGUID(guid, GUID_Friction)) {
        return DIEFT_CONDITION;
    }
    return DIEFT_CONSTANTFORCE;
}

void FillEffectInfo(DIEFFECTINFOA* info, REFGUID guid)
{
    if (info == nullptr) {
        return;
    }

    const DWORD requested_size = info->dwSize;
    std::memset(info, 0, requested_size);
    info->dwSize = requested_size;
    info->guid = guid;
    info->dwEffType = EffectType(guid) | DIEFT_FFATTACK | DIEFT_FFFADE | DIEFT_SATURATION;
    info->dwStaticParams = DIEP_ALLPARAMS;
    info->dwDynamicParams = DIEP_ALLPARAMS;
    std::snprintf(info->tszName, MAX_PATH, "%s", EffectName(guid));
}

bool InvokeEffectCallback(LPDIENUMEFFECTSCALLBACKA callback, LPVOID ref, REFGUID guid)
{
    if (callback == nullptr) {
        return false;
    }

    DIEFFECTINFOA info{};
    info.dwSize = sizeof(info);
    FillEffectInfo(&info, guid);
    return callback(&info, ref) != DIENUM_STOP;
}

class FakeEffect final : public IDirectInputEffect {
public:
    FakeEffect(REFGUID guid, const DIEFFECT* effect) : guid_(guid)
    {
        CopyEffect(effect);
        Log("Fake effect created: %s", EffectName(guid_));
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppvObj) override
    {
        if (ppvObj == nullptr) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDirectInputEffect)) {
            *ppvObj = this;
            AddRef();
            return S_OK;
        }
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_count_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG ref = --ref_count_;
        if (ref == 0) {
            delete this;
        }
        return ref;
    }

    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetEffectGuid(LPGUID out) override
    {
        if (out == nullptr) return E_POINTER;
        *out = guid_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetParameters(LPDIEFFECT out, DWORD) override
    {
        if (out == nullptr || !effect_valid_) return DIERR_INVALIDPARAM;
        *out = effect_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetParameters(LPCDIEFFECT effect, DWORD flags) override
    {
        CopyEffect(effect);
        if ((flags & DIEP_START) != 0) {
            Start(1, 0);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Start(DWORD iterations, DWORD) override
    {
        const DWORD magnitude = ReadMagnitude(guid_, effect_valid_ ? &effect_ : nullptr);
        const WORD speed = ScaleMagnitude(magnitude, effect_valid_ ? effect_.dwGain : DI_FFNOMINALMAX);
        const DWORD duration_ms = DurationToMs(effect_valid_ ? effect_.dwDuration : 0);
        SetRumble(speed, speed, duration_ms);
        playing_ = true;
        Log("Fake effect start: type=%s magnitude=%lu speed=%u duration_ms=%lu iterations=%lu", EffectName(guid_), static_cast<unsigned long>(magnitude), static_cast<unsigned>(speed), static_cast<unsigned long>(duration_ms), static_cast<unsigned long>(iterations));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop() override
    {
        playing_ = false;
        StopRumble();
        Log("Fake effect stop");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEffectStatus(LPDWORD out) override
    {
        if (out == nullptr) return E_POINTER;
        *out = playing_ ? DIEGES_PLAYING | DIEGES_EMULATED : DIEGES_EMULATED;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Download() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Unload() override { Stop(); return S_OK; }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }

private:
    void CopyEffect(const DIEFFECT* effect)
    {
        effect_valid_ = false;
        type_specific_.clear();
        axes_.clear();
        directions_.clear();

        if (effect == nullptr) {
            std::memset(&effect_, 0, sizeof(effect_));
            effect_.dwSize = sizeof(effect_);
            effect_.dwDuration = g_default_duration_ms * 1000;
            effect_.dwGain = DI_FFNOMINALMAX;
            effect_valid_ = true;
            return;
        }

        effect_ = *effect;
        if (effect_.dwGain == 0) {
            effect_.dwGain = DI_FFNOMINALMAX;
        }
        if (effect->cAxes > 0 && effect->rgdwAxes != nullptr) {
            axes_.assign(effect->rgdwAxes, effect->rgdwAxes + effect->cAxes);
            effect_.rgdwAxes = axes_.data();
        }
        if (effect->cAxes > 0 && effect->rglDirection != nullptr) {
            directions_.assign(effect->rglDirection, effect->rglDirection + effect->cAxes);
            effect_.rglDirection = directions_.data();
        }
        if (effect->cbTypeSpecificParams > 0 && effect->lpvTypeSpecificParams != nullptr) {
            const auto* bytes = static_cast<const std::uint8_t*>(effect->lpvTypeSpecificParams);
            type_specific_.assign(bytes, bytes + effect->cbTypeSpecificParams);
            effect_.lpvTypeSpecificParams = type_specific_.data();
        }

        effect_valid_ = true;
    }

    std::atomic<ULONG> ref_count_{1};
    GUID guid_{};
    DIEFFECT effect_{};
    bool effect_valid_ = false;
    bool playing_ = false;
    std::vector<std::uint8_t> type_specific_;
    std::vector<DWORD> axes_;
    std::vector<LONG> directions_;
};

class EffectWrapper final : public IDirectInputEffect {
public:
    EffectWrapper(IDirectInputEffect* inner, REFGUID guid, const DIEFFECT* effect)
        : inner_(inner), guid_(guid)
    {
        CopyEffect(effect);
        Log("Effect created");
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppvObj) override
    {
        if (ppvObj == nullptr) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDirectInputEffect)) {
            *ppvObj = this;
            AddRef();
            return S_OK;
        }
        return inner_->QueryInterface(riid, ppvObj);
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        inner_->AddRef();
        return ++ref_count_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        inner_->Release();
        const ULONG ref = --ref_count_;
        if (ref == 0) {
            delete this;
        }
        return ref;
    }

    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE a, DWORD b, REFGUID c) override { return inner_->Initialize(a, b, c); }
    HRESULT STDMETHODCALLTYPE GetEffectGuid(LPGUID a) override { return inner_->GetEffectGuid(a); }
    HRESULT STDMETHODCALLTYPE GetParameters(LPDIEFFECT a, DWORD b) override { return inner_->GetParameters(a, b); }

    HRESULT STDMETHODCALLTYPE SetParameters(LPCDIEFFECT effect, DWORD flags) override
    {
        CopyEffect(effect);
        return inner_->SetParameters(effect, flags);
    }

    HRESULT STDMETHODCALLTYPE Start(DWORD iterations, DWORD flags) override
    {
        const HRESULT hr = inner_->Start(iterations, flags);
        const DWORD magnitude = ReadMagnitude(guid_, effect_valid_ ? &effect_ : nullptr);
        const WORD speed = ScaleMagnitude(magnitude, effect_valid_ ? effect_.dwGain : DI_FFNOMINALMAX);
        const DWORD duration_ms = DurationToMs(effect_valid_ ? effect_.dwDuration : 0);

        SetRumble(speed, speed, duration_ms);
        Log("Effect start: hr=0x%08lX magnitude=%lu speed=%u duration_ms=%lu iterations=%lu", static_cast<unsigned long>(hr), static_cast<unsigned long>(magnitude), static_cast<unsigned>(speed), static_cast<unsigned long>(duration_ms), static_cast<unsigned long>(iterations));
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Stop() override
    {
        const HRESULT hr = inner_->Stop();
        StopRumble();
        Log("Effect stop: hr=0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetEffectStatus(LPDWORD a) override { return inner_->GetEffectStatus(a); }
    HRESULT STDMETHODCALLTYPE Download() override { return inner_->Download(); }
    HRESULT STDMETHODCALLTYPE Unload() override { return inner_->Unload(); }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE a) override { return inner_->Escape(a); }

private:
    void CopyEffect(const DIEFFECT* effect)
    {
        effect_valid_ = false;
        type_specific_.clear();
        axes_.clear();
        directions_.clear();

        if (effect == nullptr) {
            return;
        }

        effect_ = *effect;
        if (effect->cAxes > 0 && effect->rgdwAxes != nullptr) {
            axes_.assign(effect->rgdwAxes, effect->rgdwAxes + effect->cAxes);
            effect_.rgdwAxes = axes_.data();
        }
        if (effect->cAxes > 0 && effect->rglDirection != nullptr) {
            directions_.assign(effect->rglDirection, effect->rglDirection + effect->cAxes);
            effect_.rglDirection = directions_.data();
        }
        if (effect->cbTypeSpecificParams > 0 && effect->lpvTypeSpecificParams != nullptr) {
            const auto* bytes = static_cast<const std::uint8_t*>(effect->lpvTypeSpecificParams);
            type_specific_.assign(bytes, bytes + effect->cbTypeSpecificParams);
            effect_.lpvTypeSpecificParams = type_specific_.data();
        }

        effect_valid_ = true;
    }

    std::atomic<ULONG> ref_count_{1};
    IDirectInputEffect* inner_ = nullptr;
    GUID guid_{};
    DIEFFECT effect_{};
    bool effect_valid_ = false;
    std::vector<std::uint8_t> type_specific_;
    std::vector<DWORD> axes_;
    std::vector<LONG> directions_;
};

template <typename TDevice>
class DeviceWrapper final : public TDevice {
public:
    explicit DeviceWrapper(TDevice* inner) : inner_(inner) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppvObj) override
    {
        if (ppvObj == nullptr) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDirectInputDevice8A) || IsEqualIID(riid, IID_IDirectInputDevice8W)) {
            *ppvObj = this;
            AddRef();
            return S_OK;
        }
        return inner_->QueryInterface(riid, ppvObj);
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        inner_->AddRef();
        return ++ref_count_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        inner_->Release();
        const ULONG ref = --ref_count_;
        if (ref == 0) {
            delete this;
        }
        return ref;
    }

    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS a) override
    {
        const HRESULT hr = inner_->GetCapabilities(a);
        if (SUCCEEDED(hr) && a != nullptr) {
            a->dwFlags |= DIDC_FORCEFEEDBACK | DIDC_FFATTACK | DIDC_FFFADE | DIDC_SATURATION;
            if (a->dwFFSamplePeriod == 0) {
                a->dwFFSamplePeriod = 1000;
            }
            if (a->dwFFMinTimeResolution == 0) {
                a->dwFFMinTimeResolution = 1000;
            }
            Log(
                "GetCapabilities: flags=0x%08lX axes=%lu buttons=%lu ffSamplePeriod=%lu ffMinTimeResolution=%lu",
                static_cast<unsigned long>(a->dwFlags),
                static_cast<unsigned long>(a->dwAxes),
                static_cast<unsigned long>(a->dwButtons),
                static_cast<unsigned long>(a->dwFFSamplePeriod),
                static_cast<unsigned long>(a->dwFFMinTimeResolution)
            );
        } else {
            Log("GetCapabilities: hr=0x%08lX", static_cast<unsigned long>(hr));
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA a, LPVOID b, DWORD c) override { return inner_->EnumObjects(a, b, c); }
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID a, LPDIPROPHEADER b) override { return inner_->GetProperty(a, b); }
    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID a, LPCDIPROPHEADER b) override { return inner_->SetProperty(a, b); }
    HRESULT STDMETHODCALLTYPE Acquire() override { return inner_->Acquire(); }
    HRESULT STDMETHODCALLTYPE Unacquire() override { return inner_->Unacquire(); }
    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD a, LPVOID b) override { return inner_->GetDeviceState(a, b); }
    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD a, LPDIDEVICEOBJECTDATA b, LPDWORD c, DWORD d) override { return inner_->GetDeviceData(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT a) override
    {
        const HRESULT hr = inner_->SetDataFormat(a);
        Log("SetDataFormat: hr=0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE a) override { return inner_->SetEventNotification(a); }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND a, DWORD b) override
    {
        const HRESULT hr = inner_->SetCooperativeLevel(a, b);
        Log("SetCooperativeLevel: flags=0x%08lX hr=0x%08lX", static_cast<unsigned long>(b), static_cast<unsigned long>(hr));
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA a, DWORD b, DWORD c) override { return inner_->GetObjectInfo(a, b, c); }
    HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEA a) override { return inner_->GetDeviceInfo(a); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND a, DWORD b) override { return inner_->RunControlPanel(a, b); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE a, DWORD b, REFGUID c) override { return inner_->Initialize(a, b, c); }

    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID guid, LPCDIEFFECT effect, LPDIRECTINPUTEFFECT* out, LPUNKNOWN outer) override
    {
        const HRESULT hr = inner_->CreateEffect(guid, effect, out, outer);
        Log("CreateEffect: hr=0x%08lX", static_cast<unsigned long>(hr));
        if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
            *out = new EffectWrapper(*out, guid, effect);
            return hr;
        }
        if (out != nullptr) {
            *out = new FakeEffect(guid, effect);
            return S_OK;
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKA a, LPVOID b, DWORD c) override
    {
        const HRESULT hr = inner_->EnumEffects(a, b, c);
        Log("EnumEffects: type=0x%08lX hr=0x%08lX", static_cast<unsigned long>(c), static_cast<unsigned long>(hr));
        if (a == nullptr) {
            return DIERR_INVALIDPARAM;
        }

        if (DIEFT_GETTYPE(c) == DIEFT_ALL || DIEFT_GETTYPE(c) == DIEFT_CONSTANTFORCE) {
            if (!InvokeEffectCallback(a, b, GUID_ConstantForce)) return DI_OK;
        }
        if (DIEFT_GETTYPE(c) == DIEFT_ALL || DIEFT_GETTYPE(c) == DIEFT_PERIODIC) {
            if (!InvokeEffectCallback(a, b, GUID_Sine)) return DI_OK;
            if (!InvokeEffectCallback(a, b, GUID_Square)) return DI_OK;
        }
        if (DIEFT_GETTYPE(c) == DIEFT_ALL || DIEFT_GETTYPE(c) == DIEFT_CONDITION) {
            if (!InvokeEffectCallback(a, b, GUID_Spring)) return DI_OK;
            if (!InvokeEffectCallback(a, b, GUID_Damper)) return DI_OK;
        }

        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOA a, REFGUID b) override
    {
        const HRESULT hr = inner_->GetEffectInfo(a, b);
        Log("GetEffectInfo: hr=0x%08lX", static_cast<unsigned long>(hr));
        if (a != nullptr) {
            FillEffectInfo(a, b);
            return DI_OK;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD a) override
    {
        if (a == nullptr) {
            return DIERR_INVALIDPARAM;
        }
        *a = DIGFFS_ACTUATORSON | DIGFFS_POWERON | DIGFFS_EMPTY;
        Log("GetForceFeedbackState: state=0x%08lX", static_cast<unsigned long>(*a));
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD a) override
    {
        const HRESULT hr = inner_->SendForceFeedbackCommand(a);
        Log("SendForceFeedbackCommand: cmd=0x%08lX hr=0x%08lX", static_cast<unsigned long>(a), static_cast<unsigned long>(hr));
        if (a == DISFFC_STOPALL || a == DISFFC_RESET || a == DISFFC_SETACTUATORSOFF) {
            StopRumble();
        }
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK a, LPVOID b, DWORD c) override { return inner_->EnumCreatedEffectObjects(a, b, c); }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE a) override { return inner_->Escape(a); }
    HRESULT STDMETHODCALLTYPE Poll() override { return inner_->Poll(); }
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD a, LPCDIDEVICEOBJECTDATA b, LPDWORD c, DWORD d) override { return inner_->SendDeviceData(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCSTR a, LPDIENUMEFFECTSINFILECALLBACK b, LPVOID c, DWORD d) override { return inner_->EnumEffectsInFile(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCSTR a, DWORD b, LPDIFILEEFFECT c, DWORD d) override { return inner_->WriteEffectToFile(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATA a, LPCSTR b, DWORD c) override { return inner_->BuildActionMap(a, b, c); }
    HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATA a, LPCSTR b, DWORD c) override { return inner_->SetActionMap(a, b, c); }
    HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA a) override { return inner_->GetImageInfo(a); }

private:
    std::atomic<ULONG> ref_count_{1};
    TDevice* inner_ = nullptr;
};

class DirectInput8WrapperA final : public IDirectInput8A {
public:
    explicit DirectInput8WrapperA(IDirectInput8A* inner) : inner_(inner) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppvObj) override
    {
        if (ppvObj == nullptr) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDirectInput8A)) {
            *ppvObj = this;
            AddRef();
            return S_OK;
        }
        return inner_->QueryInterface(riid, ppvObj);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { inner_->AddRef(); return ++ref_count_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        inner_->Release();
        const ULONG ref = --ref_count_;
        if (ref == 0) delete this;
        return ref;
    }
    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID a, LPDIRECTINPUTDEVICE8A* b, LPUNKNOWN c) override
    {
        const HRESULT hr = inner_->CreateDevice(a, b, c);
        Log("CreateDeviceA: hr=0x%08lX", static_cast<unsigned long>(hr));
        if (SUCCEEDED(hr) && b != nullptr && *b != nullptr) {
            *b = new DeviceWrapper<IDirectInputDevice8A>(*b);
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD a, LPDIENUMDEVICESCALLBACKA b, LPVOID c, DWORD d) override { return inner_->EnumDevices(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID a) override { return inner_->GetDeviceStatus(a); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND a, DWORD b) override { return inner_->RunControlPanel(a, b); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE a, DWORD b) override { return inner_->Initialize(a, b); }
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID a, LPCSTR b, LPGUID c) override { return inner_->FindDevice(a, b, c); }
    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCSTR a, LPDIACTIONFORMATA b, LPDIENUMDEVICESBYSEMANTICSCBA c, LPVOID d, DWORD e) override { return inner_->EnumDevicesBySemantics(a, b, c, d, e); }
    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK a, LPDICONFIGUREDEVICESPARAMSA b, DWORD c, LPVOID d) override { return inner_->ConfigureDevices(a, b, c, d); }

private:
    std::atomic<ULONG> ref_count_{1};
    IDirectInput8A* inner_ = nullptr;
};

}  // namespace

HRESULT WINAPI ProxyDirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    LoadConfig();
    if (!LoadRealDInput()) {
        return DIERR_GENERIC;
    }

    const HRESULT hr = g_real_direct_input8_create(inst, version, riid, out, outer);
    Log("DirectInput8Create: hr=0x%08lX", static_cast<unsigned long>(hr));
    if (FAILED(hr) || out == nullptr || *out == nullptr) {
        return hr;
    }

    if (IsEqualIID(riid, IID_IDirectInput8A)) {
        *out = new DirectInput8WrapperA(static_cast<IDirectInput8A*>(*out));
    }

    return hr;
}

HRESULT WINAPI ProxyDllCanUnloadNow()
{
    return LoadRealDInput() && g_real_dll_can_unload_now ? g_real_dll_can_unload_now() : S_FALSE;
}

HRESULT WINAPI ProxyDllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out)
{
    return LoadRealDInput() && g_real_dll_get_class_object ? g_real_dll_get_class_object(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI ProxyDllRegisterServer()
{
    return LoadRealDInput() && g_real_dll_register_server ? g_real_dll_register_server() : E_NOTIMPL;
}

HRESULT WINAPI ProxyDllUnregisterServer()
{
    return LoadRealDInput() && g_real_dll_unregister_server ? g_real_dll_unregister_server() : E_NOTIMPL;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        LoadConfig();
        InstallGameHooks();
        g_worker_running.store(true);
        const HANDLE thread = CreateThread(nullptr, 0, RumbleWorker, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
        Log("FO2 XInput rumble proxy attached");
    } else if (reason == DLL_PROCESS_DETACH) {
        g_worker_running.store(false);
        StopRumble();
        Log("FO2 XInput rumble proxy detached");
    }
    return TRUE;
}
