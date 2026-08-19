#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr uintptr_t kCameraDefinitionVtable = 0x00673FF8;
constexpr uintptr_t kCameraUpdateSlot = 0x00673FFC;
constexpr uintptr_t kCameraControllerVtable = 0x006742EC;
constexpr uintptr_t kPosition2Vtable = 0x0067415C;
constexpr uintptr_t kTarget2Vtable = 0x00674040;
constexpr uintptr_t kZoomType1Vtable = 0x00674204;
constexpr uintptr_t kNativePadVtable = 0x0067B920;
constexpr uintptr_t kCameraContextGlobal = 0x008E8424;
constexpr uintptr_t kInputManagerGlobal = 0x008E844C;
constexpr uintptr_t kNativePadDisabled = 0x008D8160;
constexpr uintptr_t kExpectedUpdate = 0x004CE040;

constexpr DWORD kZoomTimestamp = 0x692D78D4;
constexpr DWORD kZoomImageSize = 0x00106000;
constexpr uintptr_t kZoomVectorBegin = 0x000FECFC;
constexpr uintptr_t kZoomVectorEnd = 0x000FED00;
constexpr size_t kZoomRecordSize = 0x3C;
constexpr size_t kMatrixSize = 64;
constexpr float kPi = 3.14159265358979323846f;

enum class InputSource { Auto, ZoomSDL, Native };

struct Config {
    bool enabled = true;
    InputSource source = InputSource::Auto;
    float deadzone_enter = 0.20f;
    float deadzone_exit = 0.15f;
    float curve = 1.0f;
    bool invert_x = false;
    bool invert_y = false;
    int zoom_axis_x = 2;
    int zoom_axis_y = 3;
    float native_center = 0.0f;
    float native_range = 10000.0f;
    bool debug = false;
    unsigned generation = 1;
};

struct OrbitState {
    void* controller = nullptr;
    void* active = nullptr;
    void* device = nullptr;
    unsigned config_generation = 0;
    DWORD last_tick = 0;
    bool stick_active = false;
};

struct MatrixFrame {
    float target[16]{};
    float position[16]{};
};

using CameraUpdateFn = void(__thiscall*)(void*, float);
using SDLGetGamepadAxisFn = std::int16_t(__cdecl*)(void*, int);
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

Config g_config;
OrbitState g_states[8]{};
CameraUpdateFn g_original = reinterpret_cast<CameraUpdateFn>(kExpectedUpdate);
HMODULE g_module = nullptr;
std::atomic<bool> g_installed{false};
char g_ini_path[MAX_PATH]{};
char g_log_path[MAX_PATH]{};
FILETIME g_ini_write_time{};
DWORD g_last_config_check = 0;
HMODULE g_zoom_module = nullptr;
SDLGetGamepadAxisFn g_sdl_axis = nullptr;
XInputGetStateFn g_xinput_get_state = nullptr;
thread_local MatrixFrame g_matrix_frames[4];
thread_local unsigned g_matrix_depth = 0;

bool IsMemory(const void* pointer, size_t size, bool write = false)
{
    if (pointer == nullptr || size == 0) return false;
    const auto start = reinterpret_cast<uintptr_t>(pointer);
    const auto end = start + size;
    if (end < start) return false;
    uintptr_t cursor = start;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD p = mbi.Protect & 0xFF;
        const bool readable = p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
                              p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
        const bool writable = p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
        if (!readable || (write && !writable)) return false;
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}

template <typename T> bool ReadAt(uintptr_t address, T& value)
{
    if (!IsMemory(reinterpret_cast<const void*>(address), sizeof(T))) return false;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return true;
}

void Log(const char* format, ...)
{
    if (g_log_path[0] == '\0') return;
    FILE* file = nullptr;
    if (fopen_s(&file, g_log_path, "a") != 0 || file == nullptr) return;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::fprintf(file, "%02u:%02u:%02u.%03u ", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fclose(file);
}

float ClampFloat(float value, float lo, float hi) { return std::max(lo, std::min(hi, value)); }
bool Finite(float value) { return std::isfinite(value) != 0; }

float ReadFloat(const char* key, float fallback)
{
    char text[64]{};
    char fallback_text[32]{};
    std::snprintf(fallback_text, sizeof(fallback_text), "%.6g", fallback);
    GetPrivateProfileStringA("orbit_camera", key, fallback_text, text, sizeof(text), g_ini_path);
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    return end != text && Finite(value) ? value : fallback;
}

void LoadConfig(bool first)
{
    Config next = g_config;
    next.enabled = GetPrivateProfileIntA("orbit_camera", "Enabled", 1, g_ini_path) != 0;
    char source[32]{};
    GetPrivateProfileStringA("orbit_camera", "Source", "Auto", source, sizeof(source), g_ini_path);
    next.source = _stricmp(source, "ZoomSDL") == 0 ? InputSource::ZoomSDL :
                  _stricmp(source, "Native") == 0 ? InputSource::Native : InputSource::Auto;
    next.deadzone_enter = ClampFloat(ReadFloat("DeadzoneEnter", 0.20f), 0.01f, 0.95f);
    next.deadzone_exit = ClampFloat(ReadFloat("DeadzoneExit", 0.15f), 0.0f, next.deadzone_enter);
    next.curve = ClampFloat(ReadFloat("AxisCurveExponent", 1.0f), 0.25f, 4.0f);
    next.invert_x = GetPrivateProfileIntA("orbit_camera", "InvertX", 0, g_ini_path) != 0;
    next.invert_y = GetPrivateProfileIntA("orbit_camera", "InvertY", 0, g_ini_path) != 0;
    next.zoom_axis_x = std::max(0, std::min(8, static_cast<int>(GetPrivateProfileIntA("orbit_camera", "ZoomAxisX", 2, g_ini_path))));
    next.zoom_axis_y = std::max(0, std::min(8, static_cast<int>(GetPrivateProfileIntA("orbit_camera", "ZoomAxisY", 3, g_ini_path))));
    next.native_center = ReadFloat("NativeAxisCenter", 0.0f);
    next.native_range = ClampFloat(std::fabs(ReadFloat("NativeAxisRange", 10000.0f)), 1.0f, 1000000.0f);
    next.debug = GetPrivateProfileIntA("orbit_camera", "DebugLog", 0, g_ini_path) != 0;
    next.generation = first ? 1u : g_config.generation + 1u;
    g_config = next;
    if (!first) Log("OrbitCamera: configuration reloaded");
}

void MaybeReloadConfig()
{
    const DWORD now = GetTickCount();
    if (now - g_last_config_check < 1000) return;
    g_last_config_check = now;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(g_ini_path, GetFileExInfoStandard, &data)) return;
    if (CompareFileTime(&data.ftLastWriteTime, &g_ini_write_time) != 0) {
        g_ini_write_time = data.ftLastWriteTime;
        LoadConfig(false);
    }
}

void BuildPaths()
{
    char path[MAX_PATH]{};
    GetModuleFileNameA(g_module, path, MAX_PATH);
    char* slash = std::strrchr(path, '\\');
    if (slash != nullptr) slash[1] = '\0';
    std::snprintf(g_ini_path, sizeof(g_ini_path), "%sfo2_orbit_camera.ini", path);
    std::snprintf(g_log_path, sizeof(g_log_path), "%sfo2_orbit_camera.log", path);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExA(g_ini_path, GetFileExInfoStandard, &data)) g_ini_write_time = data.ftLastWriteTime;
}

bool GetPeInfo(HMODULE module, DWORD& timestamp, DWORD& image_size)
{
    auto* base = reinterpret_cast<std::uint8_t*>(module);
    if (!IsMemory(base, sizeof(IMAGE_DOS_HEADER))) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (!IsMemory(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) return false;
    timestamp = nt->FileHeader.TimeDateStamp;
    image_size = nt->OptionalHeader.SizeOfImage;
    return true;
}

void* FindImport(HMODULE module, const char* dll_name, const char* proc_name)
{
    auto* base = reinterpret_cast<std::uint8_t*>(module);
    DWORD ts = 0, size = 0;
    if (!GetPeInfo(module, ts, size)) return nullptr;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) || dir.VirtualAddress >= size) return nullptr;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; IsMemory(descriptor, sizeof(*descriptor)) && descriptor->Name != 0; ++descriptor) {
        const char* name = reinterpret_cast<const char*>(base + descriptor->Name);
        if (!IsMemory(name, 1) || _stricmp(name, dll_name) != 0) continue;
        const DWORD original_rva = descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk : descriptor->FirstThunk;
        auto* original = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + original_rva);
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->FirstThunk);
        for (; IsMemory(original, sizeof(*original)) && IsMemory(thunk, sizeof(*thunk)) && original->u1.AddressOfData; ++original, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL32(original->u1.Ordinal)) continue;
            auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + original->u1.AddressOfData);
            if (IsMemory(by_name, sizeof(*by_name)) && std::strcmp(reinterpret_cast<char*>(by_name->Name), proc_name) == 0) {
                return reinterpret_cast<void*>(thunk->u1.Function);
            }
        }
    }
    return nullptr;
}

bool ResolveZoom()
{
    HMODULE zoom = GetModuleHandleA("fo2_zoom.dll");
    if (zoom == g_zoom_module) return g_sdl_axis != nullptr;
    g_zoom_module = zoom;
    g_sdl_axis = nullptr;
    if (zoom == nullptr) return false;
    DWORD timestamp = 0, image_size = 0;
    if (!GetPeInfo(zoom, timestamp, image_size) || timestamp != kZoomTimestamp || image_size != kZoomImageSize) {
        Log("OrbitCamera: unsupported fo2_zoom.dll; using native input fallback");
        return false;
    }
    void* function = FindImport(zoom, "SDL3.dll", "SDL_GetGamepadAxis");
    MEMORY_BASIC_INFORMATION mbi{};
    if (function == nullptr || VirtualQuery(function, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) {
        Log("OrbitCamera: supported Zoom found but SDL_GetGamepadAxis resolution failed");
        return false;
    }
    char owner[MAX_PATH]{};
    if (GetModuleFileNameA(reinterpret_cast<HMODULE>(mbi.AllocationBase), owner, MAX_PATH) == 0) return false;
    const char* filename = std::strrchr(owner, '\\');
    filename = filename == nullptr ? owner : filename + 1;
    if (_stricmp(filename, "SDL3.dll") != 0) return false;
    g_sdl_axis = reinterpret_cast<SDLGetGamepadAxisFn>(function);
    Log("OrbitCamera: Zoom SDL right-stick source available");
    return true;
}

void ResolveXInput()
{
    if (g_xinput_get_state != nullptr) return;
    for (const char* name : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
        HMODULE module = LoadLibraryA(name);
        if (module != nullptr) {
            auto function = reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState"));
            if (function != nullptr) { g_xinput_get_state = function; return; }
        }
    }
}

bool ResolveDevice(void* controller, void*& device, int& slot, std::uint8_t*& manager)
{
    device = nullptr; slot = -1; manager = nullptr;
    if (!IsMemory(controller, 0x370)) return false;
    auto* entity = *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(controller) + 0x14);
    if (!IsMemory(entity, 0x330)) return false;
    device = *reinterpret_cast<void**>(entity + 0x32C);
    if (device == nullptr) return false;
    if (!ReadAt(kInputManagerGlobal, manager) || !IsMemory(manager, 0x2C)) return false;
    const unsigned count = *reinterpret_cast<unsigned*>(manager + 4);
    if (count == 0 || count > 4) return false;
    for (unsigned i = 0; i < count; ++i) {
        void* candidate = *reinterpret_cast<void**>(manager + 0x1C + i * 4);
        if (candidate == device) { slot = static_cast<int>(i); break; }
    }
    if (slot < 0 || !IsMemory(device, 0x8EC)) return false;
    return *reinterpret_cast<int*>(reinterpret_cast<std::uint8_t*>(device) + 0x140) == slot;
}

float NormalizeAxis(long value, float center, float range)
{
    return ClampFloat((static_cast<float>(value) - center) / range, -1.0f, 1.0f);
}

bool SampleZoom(int slot, float& x, float& y)
{
    if (!ResolveZoom() || g_sdl_axis == nullptr || g_zoom_module == nullptr) return false;
    auto* base = reinterpret_cast<std::uint8_t*>(g_zoom_module);
    std::uint8_t* begin = nullptr;
    std::uint8_t* end = nullptr;
    if (!ReadAt(reinterpret_cast<uintptr_t>(base) + kZoomVectorBegin, begin) ||
        !ReadAt(reinterpret_cast<uintptr_t>(base) + kZoomVectorEnd, end) || begin == nullptr || end < begin) return false;
    const size_t bytes = static_cast<size_t>(end - begin);
    if (bytes % kZoomRecordSize != 0 || bytes / kZoomRecordSize > 16 || !IsMemory(begin, bytes)) return false;
    for (std::uint8_t* record = begin; record < end; record += kZoomRecordSize) {
        if (*reinterpret_cast<int*>(record + 0x34) != slot) continue;
        void* gamepad = *reinterpret_cast<void**>(record + 4);
        if (gamepad == nullptr) return false;
        const std::int16_t raw_x = g_sdl_axis(gamepad, g_config.zoom_axis_x);
        const std::int16_t raw_y = g_sdl_axis(gamepad, g_config.zoom_axis_y);
        x = raw_x >= 0 ? raw_x / 32767.0f : raw_x / 32768.0f;
        y = raw_y >= 0 ? raw_y / 32767.0f : raw_y / 32768.0f;
        return Finite(x) && Finite(y);
    }
    return false;
}

bool IsAdapterGuid(const std::uint8_t* device)
{
    static const std::uint8_t expected[16] = {0x58,0x32,0x4F,0x46,0x50,0x34,0x44,0x41,0x58,0x49,0x4E,0x50,0x55,0x54,0x33,0x00};
    return std::memcmp(device + 8, expected, sizeof(expected)) == 0;
}

bool SampleNative(void* device_ptr, int slot, std::uint8_t* manager, float& x, float& y)
{
    auto* device = reinterpret_cast<std::uint8_t*>(device_ptr);
    const unsigned count = *reinterpret_cast<unsigned*>(manager + 4);
    if (slot == 3) {
        if (count < 4 || device != *reinterpret_cast<void**>(manager + 0x28) || !IsAdapterGuid(device) ||
            *reinterpret_cast<int*>(device + 0x140) != 3 || device[0x130] == 0) return false;
        // The adapter's shadow is updated from XInput user 2 before camera work.
        auto* shadow = *reinterpret_cast<std::uint8_t**>(device + 0x7A0);
        if (IsMemory(shadow, 0x5098) && *reinterpret_cast<unsigned*>(device + 0x7A4) == 0) {
            const long rx = *reinterpret_cast<long*>(shadow + 0x2224 + 0x0C);
            const long ry = *reinterpret_cast<long*>(shadow + 0x2224 + 0x10);
            x = NormalizeAxis(rx, g_config.native_center, g_config.native_range);
            y = NormalizeAxis(ry, g_config.native_center, g_config.native_range);
            return true;
        }
        ResolveXInput();
        XINPUT_STATE state{};
        if (g_xinput_get_state == nullptr || g_xinput_get_state(2, &state) != ERROR_SUCCESS) return false;
        x = state.Gamepad.sThumbRX >= 0 ? state.Gamepad.sThumbRX / 32767.0f : state.Gamepad.sThumbRX / 32768.0f;
        y = -(state.Gamepad.sThumbRY >= 0 ? state.Gamepad.sThumbRY / 32767.0f : state.Gamepad.sThumbRY / 32768.0f);
        return true;
    }
    if (slot < 1 || slot > 2 || *reinterpret_cast<uintptr_t*>(device) != kNativePadVtable ||
        *reinterpret_cast<unsigned*>(device + 0x12C) != 1 || device[0x130] == 0) return false;
    auto* backend = *reinterpret_cast<std::uint8_t**>(device + 0x7A0);
    const unsigned index = *reinterpret_cast<unsigned*>(device + 0x7A4);
    if (index != static_cast<unsigned>(slot - 1) || index >= 2 || !IsMemory(backend, 0x5098)) return false;
    if (count <= 3) {
        if (backend != *reinterpret_cast<std::uint8_t**>(manager + 0x28)) return false;
    } else {
        auto* pad1 = *reinterpret_cast<std::uint8_t**>(manager + 0x20);
        auto* pad2 = *reinterpret_cast<std::uint8_t**>(manager + 0x24);
        if (!IsMemory(pad1, 0x8EC) || !IsMemory(pad2, 0x8EC) ||
            *reinterpret_cast<uintptr_t*>(pad1) != kNativePadVtable || *reinterpret_cast<uintptr_t*>(pad2) != kNativePadVtable ||
            *reinterpret_cast<void**>(pad1 + 0x7A0) != *reinterpret_cast<void**>(pad2 + 0x7A0) ||
            backend != *reinterpret_cast<std::uint8_t**>(pad1 + 0x7A0)) return false;
    }
    if (*reinterpret_cast<void**>(backend + 0x4674 + index * 4) == nullptr ||
        !IsMemory(reinterpret_cast<void*>(kNativePadDisabled + index), 1) || *reinterpret_cast<std::uint8_t*>(kNativePadDisabled + index) != 0) return false;
    auto* state = backend + 0x2224 + index * 0x110;
    if (!IsMemory(state, 0x14)) return false;
    x = NormalizeAxis(*reinterpret_cast<long*>(state + 0x0C), g_config.native_center, g_config.native_range);
    y = NormalizeAxis(*reinterpret_cast<long*>(state + 0x10), g_config.native_center, g_config.native_range);
    return Finite(x) && Finite(y);
}

bool SampleInput(void* device, int slot, std::uint8_t* manager, float& x, float& y)
{
    x = y = 0.0f;
    if (slot <= 0) return false;
    if (g_config.source == InputSource::ZoomSDL) return SampleZoom(slot, x, y);
    if (g_config.source == InputSource::Native) return SampleNative(device, slot, manager, x, y);
    if (slot == 3 && SampleNative(device, slot, manager, x, y)) return true;
    if (SampleZoom(slot, x, y)) return true;
    return SampleNative(device, slot, manager, x, y);
}

OrbitState& GetState(void* controller)
{
    const DWORD now = GetTickCount();
    for (auto& state : g_states) if (state.controller == controller) return state;
    for (auto& state : g_states) {
        if (state.controller == nullptr || now - state.last_tick > 3000) {
            state = OrbitState{};
            state.controller = controller;
            return state;
        }
    }
    g_states[0] = OrbitState{};
    g_states[0].controller = controller;
    return g_states[0];
}

void ResetState(void* controller)
{
    for (auto& state : g_states) if (state.controller == controller) state = OrbitState{};
}

float StickTargetYaw(float x, float y)
{
    // Pulling the stick back looks behind. Pushing it forward returns to the
    // normal chase direction; horizontal directions remain fixed side views.
    return std::atan2(x, -y);
}

void RotateMatrix(const float* source, float* destination, float yaw)
{
    std::memcpy(destination, source, kMatrixSize);
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    for (int i = 0; i < 3; ++i) {
        const float x = source[i];
        const float z = source[8 + i];
        destination[i] = c * x - s * z;
        destination[8 + i] = s * x + c * z;
    }
}

class MatrixScope {
public:
    MatrixScope(std::uint8_t* context, float yaw) : context_(context)
    {
        if (g_matrix_depth >= 4 || !IsMemory(context, 0x60, true)) return;
        target_slot_ = reinterpret_cast<float**>(context + 0x58);
        position_slot_ = reinterpret_cast<float**>(context + 0x5C);
        original_target_ = *target_slot_;
        original_position_ = *position_slot_;
        if (!IsMemory(original_target_, kMatrixSize) || !IsMemory(original_position_, kMatrixSize)) return;
        frame_ = &g_matrix_frames[g_matrix_depth++];
        RotateMatrix(original_target_, frame_->target, yaw);
        if (original_position_ == original_target_) std::memcpy(frame_->position, frame_->target, kMatrixSize);
        else RotateMatrix(original_position_, frame_->position, yaw);
        *target_slot_ = frame_->target;
        *position_slot_ = frame_->position;
        active_ = true;
    }
    ~MatrixScope()
    {
        if (!active_) return;
        if (IsMemory(context_, 0x60, true)) {
            if (*target_slot_ == frame_->target) *target_slot_ = original_target_;
            if (*position_slot_ == frame_->position) *position_slot_ = original_position_;
        }
        --g_matrix_depth;
    }
    bool active() const { return active_; }
private:
    std::uint8_t* context_ = nullptr;
    float** target_slot_ = nullptr;
    float** position_slot_ = nullptr;
    float* original_target_ = nullptr;
    float* original_position_ = nullptr;
    MatrixFrame* frame_ = nullptr;
    bool active_ = false;
};

void __fastcall CameraUpdateHook(void* active_ptr, void*, float dt)
{
    auto call_original = [&]() { g_original(active_ptr, dt); };
    MaybeReloadConfig();
    auto* active = reinterpret_cast<std::uint8_t*>(active_ptr);
    if (!g_config.enabled || !IsMemory(active, 0x2C) || *reinterpret_cast<uintptr_t*>(active) != kCameraDefinitionVtable) {
        call_original(); return;
    }

    std::uint8_t* context = nullptr;
    if (!ReadAt(kCameraContextGlobal, context) || !IsMemory(context, 0x60)) { call_original(); return; }
    auto* renderer_camera = *reinterpret_cast<std::uint8_t**>(context + 0x50);
    if (renderer_camera == nullptr) { call_original(); return; }
    auto* controller = renderer_camera - 0x20;
    if (!IsMemory(controller, 0x370) || *reinterpret_cast<uintptr_t*>(controller) != kCameraControllerVtable ||
        *reinterpret_cast<void**>(controller + 0x364) != active_ptr || controller + 0x20 != renderer_camera) {
        call_original(); return;
    }
    auto* position = *reinterpret_cast<std::uint8_t**>(active + 0x0C);
    auto* target_component = *reinterpret_cast<std::uint8_t**>(active + 0x10);
    auto* zoom = *reinterpret_cast<std::uint8_t**>(active + 0x14);
    if (!IsMemory(position, sizeof(uintptr_t)) || !IsMemory(target_component, sizeof(uintptr_t)) ||
        !IsMemory(zoom, sizeof(uintptr_t)) || *reinterpret_cast<uintptr_t*>(position) != kPosition2Vtable ||
        *reinterpret_cast<uintptr_t*>(target_component) != kTarget2Vtable ||
        *reinterpret_cast<uintptr_t*>(zoom) != kZoomType1Vtable) {
        ResetState(controller);
        call_original(); return;
    }

    void* device = nullptr;
    int slot = -1;
    std::uint8_t* manager = nullptr;
    if (!ResolveDevice(controller, device, slot, manager)) { ResetState(controller); call_original(); return; }
    OrbitState& state = GetState(controller);
    if (state.active != active_ptr || state.device != device || state.config_generation != g_config.generation) {
        state.stick_active = false;
        state.active = active_ptr;
        state.device = device;
        state.config_generation = g_config.generation;
    }
    state.last_tick = GetTickCount();

    float x = 0.0f, y = 0.0f;
    const bool sampled = SampleInput(device, slot, manager, x, y);
    if (g_config.invert_x) x = -x;
    if (g_config.invert_y) y = -y;
    const float raw_magnitude = sampled ? std::sqrt(x * x + y * y) : 0.0f;
    float magnitude = raw_magnitude;
    if (raw_magnitude > 0.0f && g_config.curve != 1.0f) {
        // A radial curve changes engagement response without distorting the absolute stick angle.
        magnitude = std::pow(ClampFloat(raw_magnitude, 0.0f, 1.0f), g_config.curve);
        const float radial_scale = magnitude / raw_magnitude;
        x *= radial_scale;
        y *= radial_scale;
    }
    if (state.stick_active) { if (magnitude <= g_config.deadzone_exit) state.stick_active = false; }
    else if (magnitude >= g_config.deadzone_enter) state.stick_active = true;
    if (!state.stick_active) {
        call_original(); return;
    }
    MatrixScope scope(context, StickTargetYaw(x, y));
    call_original();
}

bool RunSelfTests()
{
    float matrix[16]{};
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
    float rotated[16]{};
    RotateMatrix(matrix, rotated, kPi * 0.5f);
    if (std::fabs(rotated[0]) > 0.001f || std::fabs(rotated[2] + 1.0f) > 0.001f ||
        std::fabs(rotated[8] - 1.0f) > 0.001f || std::fabs(rotated[5] - 1.0f) > 0.001f) return false;
    if (std::fabs(StickTargetYaw(1.0f, 0.0f) - kPi * 0.5f) > 0.001f ||
        std::fabs(std::fabs(StickTargetYaw(0.0f, 1.0f)) - kPi) > 0.001f ||
        std::fabs(StickTargetYaw(0.0f, -1.0f)) > 0.001f) return false;
    return true;
}

bool InstallHook()
{
    const uintptr_t expected[5] = {0x004CDE80, 0x004CE040, 0x004CDE20, 0x004CE210, 0x004CDF90};
    auto* table = reinterpret_cast<uintptr_t*>(kCameraDefinitionVtable);
    if (!IsMemory(table, sizeof(expected)) || std::memcmp(table, expected, sizeof(expected)) != 0) {
        Log("OrbitCamera: unsupported executable camera vtable signature; hook not installed");
        return false;
    }
    DWORD old_protect = 0;
    auto* slot = reinterpret_cast<void**>(kCameraUpdateSlot);
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect)) return false;
    g_original = reinterpret_cast<CameraUpdateFn>(InterlockedExchangePointer(slot, reinterpret_cast<void*>(&CameraUpdateHook)));
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old_protect, &ignored);
    if (reinterpret_cast<uintptr_t>(g_original) != kExpectedUpdate) {
        DWORD restore = 0;
        VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &restore);
        InterlockedCompareExchangePointer(slot, reinterpret_cast<void*>(g_original), reinterpret_cast<void*>(&CameraUpdateHook));
        VirtualProtect(slot, sizeof(void*), restore, &ignored);
        Log("OrbitCamera: camera update slot was already modified; hook not installed");
        return false;
    }
    g_installed.store(true);
    Log("OrbitCamera: installed chase-camera matrix hook source=%d", static_cast<int>(g_config.source));
    return true;
}

void UninstallHook()
{
    if (!g_installed.exchange(false)) return;
    auto* slot = reinterpret_cast<void**>(kCameraUpdateSlot);
    DWORD old_protect = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect)) {
        InterlockedCompareExchangePointer(slot, reinterpret_cast<void*>(g_original), reinterpret_cast<void*>(&CameraUpdateHook));
        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(void*), old_protect, &ignored);
    }
}

DWORD WINAPI InitThread(LPVOID)
{
    BuildPaths();
    LoadConfig(true);
    if (!RunSelfTests()) { Log("OrbitCamera: self-test failed; hook not installed"); return 0; }
    ResolveXInput();
    ResolveZoom();
    InstallHook();
    return 0;
}

} // namespace

#ifdef ORBIT_CAMERA_SELF_TEST_EXE
int main()
{
    if (!RunSelfTests()) return 1;
    if (std::fabs(NormalizeAxis(10000, 0.0f, 10000.0f) - 1.0f) > 0.0001f) return 2;
    if (std::fabs(NormalizeAxis(-10000, 0.0f, 10000.0f) + 1.0f) > 0.0001f) return 3;
    if (std::fabs(StickTargetYaw(-1.0f, 0.0f) + kPi * 0.5f) > 0.0001f) return 4;
    return 0;
}
#else
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (thread != nullptr) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        UninstallHook();
    }
    return TRUE;
}
#endif
