#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <d3d9.h>
#include <dinput.h>
#include <mmsystem.h>
#include <xinput.h>

#include <algorithm>
#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cmath>

namespace {

bool g_log_enabled = true;
bool g_skip_license_screen = true;
bool g_skip_intro = true;
bool g_uncap_fps = true;
bool g_frame_pacing_fix = true;
bool g_remove_vsync = true;
bool g_borderless_windowed = false;
bool g_widescreen_fix = false;
bool g_widescreen_fov_scaling = false;
bool g_splitscreen_fix = false;
bool g_splitscreen_post_processing_fix = false;
bool g_splitscreen_zoom_input_fix = true;
bool g_splitscreen_vertical_layout = false;
bool g_splitscreen_vertical_capable = false;
bool g_splitscreen_three_player_capable = false;
bool g_splitscreen_four_player_capable = false;
bool g_splitscreen_vertical_render_active = false;
bool g_splitscreen_vertical_hud_pass_active = false;
bool g_splitscreen_grid_hud_pass_active = false;
bool g_menu_car_backface_culling = false;
DWORD g_menu_car_max_model_file_size = 524288;
DWORD g_menu_car_max_skin_file_size = 2097152;
DWORD g_menu_car_max_surfaces = 16;
char g_log_path[MAX_PATH] = {};
char g_splitscreen_layout_state_path[MAX_PATH] = {};
char g_splitscreen_filesystem_name[] = "fo2_splitscreen_filesystem";
HMODULE g_winmm = nullptr;

using XInputGetStateFn = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
HMODULE g_xinput_module = nullptr;
XInputGetStateFn g_xinput_get_state = nullptr;

struct FourPlayerInputSidecar {
    std::uint8_t* manager;
    std::uint8_t* native_backend;
    std::uint8_t* adapter;
    std::uint8_t* shadow_backend;
};

FourPlayerInputSidecar g_four_player_input = {};
std::uint8_t* g_four_player_manager_for_hooks = nullptr;
std::uint8_t* g_four_player_native_backend_for_hooks = nullptr;
std::uint8_t* g_four_player_adapter_for_hooks = nullptr;
std::uintptr_t g_four_player_pad_vtable[54] = {};
DWORD g_four_player_connection_sentinel = 1;
std::uint8_t g_saved_controller_guid[16] = {};
bool g_saved_controller_guid_valid = false;

constexpr size_t kInputManagerDeviceCountOffset = 0x04;
constexpr size_t kInputManagerDeviceArrayOffset = 0x1C;
constexpr size_t kInputManagerBackendOrSlot3Offset = 0x28;
constexpr size_t kNativePadObjectSize = 0x8EC;
constexpr size_t kNativeControllerBackendSize = 0x5098;
constexpr size_t kNativePadVtableEntries = 54;
constexpr DWORD kThirdPadXInputUser = 2;

const GUID kFourPlayerAdapterGuid = {
    0x464F3258, 0x3450, 0x4144, {0x58, 0x49, 0x4E, 0x50, 0x55, 0x54, 0x33, 0x00}
};

static_assert(sizeof(DIJOYSTATE2) == 0x110, "FO2 backend requires DIJOYSTATE2-sized snapshots");
static_assert(offsetof(DIJOYSTATE2, rgbButtons) == 0x30, "FO2 button bindings assume DIJOYSTATE2 button offset");

using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
Direct3DCreate9Fn g_real_direct3d_create9 = nullptr;
using ResetFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
ResetFn g_real_reset = nullptr;
volatile LONG g_device_hooks_installed = 0;
using SendInputFn = UINT (WINAPI*)(UINT, LPINPUT, int);
SendInputFn g_real_zoom_send_input = nullptr;
void** g_zoom_send_input_iat = nullptr;
std::uint8_t* g_zoom_module_base = nullptr;

struct ZoomInputLatch {
    DWORD slot;
    DWORD ordinal;
    DWORD tick;
};

constexpr size_t kZoomInputLatchCapacity = 8;
constexpr DWORD kZoomInputLatchLifetimeMs = 750;
SRWLOCK g_zoom_input_latch_lock = SRWLOCK_INIT;
ZoomInputLatch g_zoom_input_latches[kZoomInputLatchCapacity] = {};
size_t g_zoom_input_latch_head = 0;
size_t g_zoom_input_latch_count = 0;
using TimeBeginPeriodFn = MMRESULT (WINAPI*)(UINT);
TimeBeginPeriodFn g_time_begin_period = nullptr;
DWORD g_widescreen_fov_context = 0;
float g_widescreen_normalized_hud_scale = 0.001171875047f;
float g_widescreen_vertical_scale = 2.25f;
float g_widescreen_fov_scale = 0.001562500023f;
float g_widescreen_aspect = 16.0f / 9.0f;
float g_widescreen_screen_width = 1920.0f;
DWORD g_widescreen_menu_scale_bits = 0x40400000; // 3.0f
const float* g_menu_transform_source = nullptr;
float* g_menu_transform_stack = nullptr;
DWORD g_projection_split_mode = 0;
float g_projection_vertical_aspect_scale = 0.5f;
DWORD FloatBits(float value)
{
    DWORD bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool FloatBitsEqual(float lhs, float rhs)
{
    return FloatBits(lhs) == FloatBits(rhs);
}

bool IsReadableMemory(const void* address, size_t size)
{
    if (address == nullptr || size == 0) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
        return false;
    }

    if ((mbi.Protect & PAGE_GUARD) != 0) {
        return false;
    }

    const DWORD protect = mbi.Protect & 0xFF;
    if (protect == PAGE_NOACCESS || protect == PAGE_EXECUTE) {
        return false;
    }

    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto region_begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    return begin >= region_begin && (begin + size) <= (region_begin + mbi.RegionSize);
}

struct ModuleRange
{
    std::uint8_t* base;
    DWORD size;
};

bool ModuleContains(const ModuleRange& range, const void* address, size_t size)
{
    if (range.base == nullptr || address == nullptr || size == 0) {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(range.base);
    const auto end = begin + range.size;
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    return target >= begin && target <= end && size <= end - target;
}

void BuildGamePath(char* out, DWORD out_size, const char* name)
{
    char module_path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, module_path, MAX_PATH);

    char* slash = std::strrchr(module_path, '\\');
    if (slash != nullptr) {
        slash[1] = '\0';
    } else {
        module_path[0] = '\0';
    }

    std::snprintf(out, out_size, "%s%s", module_path, name);
}

void Log(const char* fmt, ...)
{
    if (!g_log_enabled || g_log_path[0] == '\0') {
        return;
    }

    FILE* file = nullptr;
    fopen_s(&file, g_log_path, "a");
    if (file == nullptr) {
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

bool SaveSplitscreenLayoutState(bool vertical);

void LoadSplitscreenLayoutState()
{
    g_splitscreen_vertical_layout = false;
    HANDLE file = CreateFileA(
        g_splitscreen_layout_state_path,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE) {
        SaveSplitscreenLayoutState(false);
        return;
    }

    const DWORD file_size = GetFileSize(file, nullptr);
    if (file_size != 8) {
        CloseHandle(file);
        SaveSplitscreenLayoutState(false);
        return;
    }

    char contents[9] = {};
    DWORD bytes_read = 0;
    const BOOL read_ok = ReadFile(file, contents, 8, &bytes_read, nullptr);
    CloseHandle(file);
    if (!read_ok) {
        SaveSplitscreenLayoutState(false);
        return;
    }

    if (bytes_read == 8 && std::memcmp(contents, "return 0", 8) == 0) {
        g_splitscreen_vertical_layout = false;
    } else if (bytes_read == 8 && std::memcmp(contents, "return 1", 8) == 0) {
        g_splitscreen_vertical_layout = true;
    } else {
        SaveSplitscreenLayoutState(false);
    }
}

bool SaveSplitscreenLayoutState(bool vertical)
{
    char temporary_path[MAX_PATH] = {};
    std::snprintf(
        temporary_path,
        sizeof(temporary_path),
        "%s.tmp-%lu",
        g_splitscreen_layout_state_path,
        static_cast<unsigned long>(GetCurrentProcessId())
    );

    HANDLE file = CreateFileA(
        temporary_path,
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    const char* contents = vertical ? "return 1" : "return 0";
    DWORD bytes_written = 0;
    const BOOL write_ok = WriteFile(file, contents, 8, &bytes_written, nullptr);
    const BOOL flush_ok = write_ok && bytes_written == 8 && FlushFileBuffers(file);
    CloseHandle(file);
    if (!flush_ok || !MoveFileExA(
            temporary_path,
            g_splitscreen_layout_state_path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(temporary_path);
        return false;
    }
    return true;
}

void LoadConfig()
{
    char ini_path[MAX_PATH] = {};
    BuildGamePath(ini_path, MAX_PATH, "fo2_zpatch_reimpl.ini");
    BuildGamePath(g_log_path, MAX_PATH, "fo2_zpatch_reimpl.log");
    BuildGamePath(g_splitscreen_layout_state_path, MAX_PATH, "fo2_splitscreen_layout.lua");

    g_log_enabled = GetPrivateProfileIntA("General", "Log", 1, ini_path) != 0;
    g_skip_license_screen = GetPrivateProfileIntA("Fixes", "SkipLicenseScreen", 1, ini_path) != 0;
    g_skip_intro = GetPrivateProfileIntA("Fixes", "SkipIntro", 1, ini_path) != 0;
    g_uncap_fps = GetPrivateProfileIntA("Fixes", "UncapFPS", 1, ini_path) != 0;
    g_frame_pacing_fix = GetPrivateProfileIntA("Fixes", "FramePacingFix", 1, ini_path) != 0;
    g_remove_vsync = GetPrivateProfileIntA("Fixes", "RemoveVSync", 1, ini_path) != 0;
    g_borderless_windowed = GetPrivateProfileIntA("Fixes", "BorderlessWindowed", 0, ini_path) != 0;
    g_widescreen_fix = GetPrivateProfileIntA("Fixes", "WidescreenFix", 0, ini_path) != 0;
    g_widescreen_fov_scaling = GetPrivateProfileIntA("Fixes", "WidescreenFix_FOVScaling", 0, ini_path) != 0;
    g_splitscreen_fix = GetPrivateProfileIntA("Fixes", "SplitscreenFix", 0, ini_path) != 0;
    g_splitscreen_post_processing_fix =
        GetPrivateProfileIntA("Fixes", "SplitscreenPostProcessingFix", 0, ini_path) != 0;
    g_splitscreen_zoom_input_fix =
        GetPrivateProfileIntA("Fixes", "SplitscreenZoomInputFix", 1, ini_path) != 0;
    if (g_splitscreen_fix) {
        LoadSplitscreenLayoutState();
    }
    g_menu_car_backface_culling = GetPrivateProfileIntA("Fixes", "MenuCarBackfaceCulling", 0, ini_path) != 0;
    g_menu_car_max_model_file_size = std::max<DWORD>(GetPrivateProfileIntA("Fixes", "MenuCarMaxModelFileSize", 524288, ini_path), 1);
    g_menu_car_max_skin_file_size = std::max<DWORD>(GetPrivateProfileIntA("Fixes", "MenuCarMaxSkinFileSize", 2097152, ini_path), 1);
    g_menu_car_max_surfaces = std::clamp<DWORD>(
        GetPrivateProfileIntA("Fixes", "MenuCarMaxSurfaces", 16, ini_path),
        1,
        4096
    );
}

bool IsWritableMemory(const void* address, size_t size)
{
    if (!IsReadableMemory(address, size)) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    const DWORD protect = mbi.Protect & 0xFF;
    return protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
        protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
}

bool WriteMemory(void* address, const void* data, size_t size)
{
    DWORD old_protect = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }

    std::memcpy(address, data, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);

    DWORD ignored = 0;
    VirtualProtect(address, size, old_protect, &ignored);
    return true;
}

bool WriteJump(void* address, void* destination, size_t size)
{
    if (size < 5) {
        return false;
    }

    std::uint8_t patch[32] = {};
    if (size > sizeof(patch)) {
        return false;
    }

    patch[0] = 0xE9;
    *reinterpret_cast<std::int32_t*>(&patch[1]) =
        static_cast<std::int32_t>(reinterpret_cast<std::uint8_t*>(destination) - reinterpret_cast<std::uint8_t*>(address) - 5);
    for (size_t i = 5; i < size; ++i) {
        patch[i] = 0x90;
    }

    return WriteMemory(address, patch, size);
}

bool WriteCall(void* address, void* destination, size_t size)
{
    if (size < 5) {
        return false;
    }

    std::uint8_t patch[32] = {};
    if (size > sizeof(patch)) {
        return false;
    }

    patch[0] = 0xE8;
    *reinterpret_cast<std::int32_t*>(&patch[1]) =
        static_cast<std::int32_t>(reinterpret_cast<std::uint8_t*>(destination) - reinterpret_cast<std::uint8_t*>(address) - 5);
    for (size_t i = 5; i < size; ++i) {
        patch[i] = 0x90;
    }

    return WriteMemory(address, patch, size);
}

LONG ScaleXInputAxis(SHORT value)
{
    const LONG scaled = value >= 0
        ? static_cast<LONG>(value) * 10000 / 32767
        : static_cast<LONG>(value) * 10000 / 32768;
    return std::max<LONG>(-10000, std::min<LONG>(10000, scaled));
}

DWORD XInputPov(WORD buttons)
{
    const bool up = (buttons & XINPUT_GAMEPAD_DPAD_UP) != 0;
    const bool down = (buttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
    const bool left = (buttons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
    const bool right = (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
    if (up && right && !down && !left) return 4500;
    if (down && right && !up && !left) return 13500;
    if (down && left && !up && !right) return 22500;
    if (up && left && !down && !right) return 31500;
    if (up && !down) return 0;
    if (right && !left) return 9000;
    if (down && !up) return 18000;
    if (left && !right) return 27000;
    return 0xFFFFFFFF;
}

void TranslateXInputState(const XINPUT_GAMEPAD& source, DIJOYSTATE2* destination)
{
    if (destination == nullptr) {
        return;
    }
    std::memset(destination, 0, sizeof(*destination));
    destination->lX = ScaleXInputAxis(source.sThumbLX);
    destination->lY = -ScaleXInputAxis(source.sThumbLY);
    destination->lRx = ScaleXInputAxis(source.sThumbRX);
    destination->lRy = -ScaleXInputAxis(source.sThumbRY);
    destination->lZ =
        (static_cast<LONG>(source.bRightTrigger) - static_cast<LONG>(source.bLeftTrigger)) * 10000 / 255;
    destination->rgdwPOV[0] = XInputPov(source.wButtons);
    destination->rgdwPOV[1] = 0xFFFFFFFF;
    destination->rgdwPOV[2] = 0xFFFFFFFF;
    destination->rgdwPOV[3] = 0xFFFFFFFF;

    const WORD masks[10] = {
        XINPUT_GAMEPAD_A,
        XINPUT_GAMEPAD_B,
        XINPUT_GAMEPAD_X,
        XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_LEFT_SHOULDER,
        XINPUT_GAMEPAD_RIGHT_SHOULDER,
        XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_START,
        XINPUT_GAMEPAD_LEFT_THUMB,
        XINPUT_GAMEPAD_RIGHT_THUMB,
    };
    for (size_t i = 0; i < sizeof(masks) / sizeof(masks[0]); ++i) {
        destination->rgbButtons[i] = (source.wButtons & masks[i]) != 0 ? 0x80 : 0;
    }
}

bool TestFourPlayerInputTranslator()
{
    XINPUT_GAMEPAD source = {};
    DIJOYSTATE2 translated = {};
    TranslateXInputState(source, &translated);
    if (translated.lX != 0 || translated.lY != 0 || translated.lRx != 0 ||
        translated.lRy != 0 || translated.lZ != 0 || translated.rgdwPOV[0] != 0xFFFFFFFF) {
        return false;
    }

    const WORD masks[10] = {
        XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
        XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START,
        XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
    };
    for (size_t i = 0; i < sizeof(masks) / sizeof(masks[0]); ++i) {
        source = {};
        source.wButtons = masks[i];
        TranslateXInputState(source, &translated);
        if (translated.rgbButtons[i] != 0x80) {
            return false;
        }
    }

    const WORD pov_masks[8] = {
        XINPUT_GAMEPAD_DPAD_UP,
        XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT,
        XINPUT_GAMEPAD_DPAD_RIGHT,
        XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_DOWN,
        XINPUT_GAMEPAD_DPAD_DOWN,
        XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT,
        XINPUT_GAMEPAD_DPAD_LEFT,
        XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_UP,
    };
    const DWORD pov_values[8] = {0, 4500, 9000, 13500, 18000, 22500, 27000, 31500};
    for (size_t i = 0; i < 8; ++i) {
        source = {};
        source.wButtons = pov_masks[i];
        TranslateXInputState(source, &translated);
        if (translated.rgdwPOV[0] != pov_values[i]) {
            return false;
        }
    }

    source = {};
    source.sThumbLX = 32767;
    source.sThumbLY = -32768;
    source.sThumbRX = -32768;
    source.sThumbRY = 32767;
    source.bRightTrigger = 255;
    TranslateXInputState(source, &translated);
    if (translated.lX != 10000 || translated.lY != 10000 || translated.lRx != -10000 ||
        translated.lRy != -10000 || translated.lZ != 10000) {
        return false;
    }
    source.bRightTrigger = 0;
    source.bLeftTrigger = 255;
    TranslateXInputState(source, &translated);
    return translated.lZ == -10000;
}

bool LoadThirdPadXInput()
{
    if (g_xinput_get_state != nullptr) {
        return true;
    }
    const char* candidates[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (const char* name : candidates) {
        HMODULE module = LoadLibraryA(name);
        if (module == nullptr) {
            continue;
        }
        const auto get_state = reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState"));
        if (get_state != nullptr) {
            g_xinput_module = module;
            g_xinput_get_state = get_state;
            return true;
        }
        FreeLibrary(module);
    }
    return false;
}

int CallNativePadPoll(std::uint8_t* object)
{
    int result = 0;
    __asm {
        mov ecx, object
        mov eax, 0x0055D480
        call eax
        mov result, eax
    }
    return result;
}

void CallNativeInputDeviceBaseConstructor(std::uint8_t* object)
{
    __asm {
        mov eax, object
        mov ecx, 0x0055B1C0
        call ecx
    }
}

int CallNativePadIdentityInitializer(std::uint8_t* object, DWORD ordinal)
{
    int result = 0;
    __asm {
        mov ecx, object
        push ordinal
        mov eax, 0x0055D1B0
        call eax
        mov result, eax
    }
    return result;
}

char* BuildNativeControllerGuidText(const void* guid)
{
    char* result = nullptr;
    __asm {
        mov ecx, guid
        mov eax, 0x00550420
        call eax
        mov result, eax
    }
    return result;
}

int __fastcall FourPlayerAdapterPoll(std::uint8_t* object, void*)
{
    if (object == nullptr || object != g_four_player_input.adapter ||
        g_four_player_input.shadow_backend == nullptr || g_xinput_get_state == nullptr) {
        return 0;
    }

    auto* shadow = g_four_player_input.shadow_backend;
    auto* current = reinterpret_cast<DIJOYSTATE2*>(shadow + 0x2224);
    auto* previous = reinterpret_cast<DIJOYSTATE2*>(shadow + 0x4684);
    auto* analog = reinterpret_cast<DIJOYSTATE2*>(shadow);
    XINPUT_STATE state = {};
    if (g_xinput_get_state(kThirdPadXInputUser, &state) != ERROR_SUCCESS) {
        std::memset(current, 0, sizeof(*current));
        std::memset(previous, 0, sizeof(*previous));
        std::memset(analog, 0, sizeof(*analog));
        std::memset(object + 0x8C0, 0, 0x28);
        object[0x130] = 0;
        return 0;
    }

    std::memcpy(previous, current, sizeof(*previous));
    TranslateXInputState(state.Gamepad, current);
    std::memcpy(analog, current, sizeof(*analog));
    *reinterpret_cast<void**>(shadow + 0x4674) = &g_four_player_connection_sentinel;
    const int result = CallNativePadPoll(object);
    *reinterpret_cast<void**>(shadow + 0x4674) = nullptr;
    return result;
}

int __fastcall FourPlayerAdapterDefaultMap(std::uint8_t* object, void*)
{
    if (object == nullptr || g_four_player_input.shadow_backend == nullptr) {
        return 0;
    }
    void* borrowed_device = nullptr;
    if (g_four_player_input.native_backend != nullptr &&
        IsReadableMemory(g_four_player_input.native_backend + 0x4674, sizeof(void*))) {
        borrowed_device = *reinterpret_cast<void**>(g_four_player_input.native_backend + 0x4674);
    }
    *reinterpret_cast<void**>(g_four_player_input.shadow_backend + 0x4674) = borrowed_device;
    int result = 0;
    __asm {
        mov ecx, object
        mov eax, 0x0055E190
        call eax
        mov result, eax
    }
    *reinterpret_cast<void**>(g_four_player_input.shadow_backend + 0x4674) = nullptr;
    return result;
}

void __fastcall FourPlayerAdapterDetach(std::uint8_t* object, void*)
{
    if (object == nullptr) {
        return;
    }
    *reinterpret_cast<DWORD*>(object + 0x12C) = 0;
    object[0x130] = 0;
}

void* __fastcall FourPlayerAdapterDeletingDestructor(std::uint8_t* object, void*, DWORD flags)
{
    if (object == nullptr) {
        return nullptr;
    }
    std::uint8_t* shadow = *reinterpret_cast<std::uint8_t**>(object + 0x7A0);
    *reinterpret_cast<std::uint8_t**>(object + 0x7A0) = nullptr;
    if (shadow != nullptr) {
        HeapFree(GetProcessHeap(), 0, shadow);
    }
    if (g_four_player_input.adapter == object) {
        g_four_player_input.adapter = nullptr;
        g_four_player_input.shadow_backend = nullptr;
        g_four_player_adapter_for_hooks = nullptr;
        g_splitscreen_four_player_capable = false;
    }
    if ((flags & 1) != 0) {
        HeapFree(GetProcessHeap(), 0, object);
    }
    return object;
}

void SnapshotSelectedControllerGuid()
{
    if (IsReadableMemory(reinterpret_cast<void*>(0x008D7BB0), sizeof(g_saved_controller_guid))) {
        std::memcpy(g_saved_controller_guid, reinterpret_cast<void*>(0x008D7BB0), sizeof(g_saved_controller_guid));
        g_saved_controller_guid_valid = true;
    } else {
        g_saved_controller_guid_valid = false;
    }
}

bool ValidatePublishedFourPlayerAdapter(std::uint8_t* manager)
{
    if (!IsReadableMemory(manager, 0x2C) || *reinterpret_cast<DWORD*>(manager + 0x04) != 4) {
        return false;
    }
    void* slots[4] = {};
    for (DWORD i = 0; i < 4; ++i) {
        slots[i] = *reinterpret_cast<void**>(manager + kInputManagerDeviceArrayOffset + i * sizeof(void*));
        if (!IsReadableMemory(slots[i], sizeof(void*)) ||
            !IsReadableMemory(*reinterpret_cast<void**>(slots[i]), kNativePadVtableEntries * sizeof(void*))) {
            return false;
        }
        for (DWORD j = 0; j < i; ++j) {
            if (slots[i] == slots[j]) {
                return false;
            }
        }
    }
    return slots[3] == g_four_player_input.adapter &&
        g_four_player_input.native_backend != nullptr &&
        g_four_player_input.native_backend != slots[3] &&
        *reinterpret_cast<DWORD*>(static_cast<std::uint8_t*>(slots[3]) + 0x140) == 3;
}

void RestoreSelectedAdapterGuidIfNeeded()
{
    if (!g_saved_controller_guid_valid ||
        std::memcmp(g_saved_controller_guid, &kFourPlayerAdapterGuid, sizeof(g_saved_controller_guid)) != 0) {
        return;
    }
    std::memcpy(reinterpret_cast<void*>(0x008D7BB0), g_saved_controller_guid, sizeof(g_saved_controller_guid));
    *reinterpret_cast<DWORD*>(0x008D7BE4) = 3;
    char* text = BuildNativeControllerGuidText(reinterpret_cast<void*>(0x008D7BB0));
    if (text != nullptr) {
        std::strcpy(reinterpret_cast<char*>(0x008D7BC0), text);
    }
}

void InstallFourPlayerAdapterAfterNativeConstructor(std::uint8_t* manager)
{
    g_splitscreen_four_player_capable = false;
    if (!IsReadableMemory(manager, 0x2C)) {
        Log("Splitscreen4P: manager unavailable after native construction; maximum remains 3");
        return;
    }

    auto* native_backend = *reinterpret_cast<std::uint8_t**>(manager + kInputManagerBackendOrSlot3Offset);
    g_four_player_input.manager = manager;
    g_four_player_input.native_backend = native_backend;
    g_four_player_input.adapter = nullptr;
    g_four_player_input.shadow_backend = nullptr;
    g_four_player_manager_for_hooks = manager;
    g_four_player_native_backend_for_hooks = native_backend;
    g_four_player_adapter_for_hooks = nullptr;

    const DWORD native_count = *reinterpret_cast<DWORD*>(manager + kInputManagerDeviceCountOffset);
    auto* pad0 = *reinterpret_cast<std::uint8_t**>(manager + 0x20);
    if (native_count != 3 || native_backend == nullptr || pad0 == nullptr ||
        *reinterpret_cast<void**>(manager + 0x1C) == nullptr ||
        *reinterpret_cast<void**>(manager + 0x24) == nullptr ||
        !IsReadableMemory(native_backend, kNativeControllerBackendSize) ||
        !IsReadableMemory(pad0, kNativePadObjectSize)) {
        Log("Splitscreen4P: stock keyboard+two-pad topology unavailable; maximum remains 3 count=%lu", static_cast<unsigned long>(native_count));
        return;
    }
    if (!LoadThirdPadXInput() || !TestFourPlayerInputTranslator()) {
        Log("Splitscreen4P: XInput loader or translator self-test failed; maximum remains 3");
        return;
    }
    XINPUT_STATE state = {};
    if (g_xinput_get_state(kThirdPadXInputUser, &state) != ERROR_SUCCESS) {
        Log("Splitscreen4P: XInput user 2 is not connected at startup; maximum remains 3");
        return;
    }

    auto* object = static_cast<std::uint8_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, kNativePadObjectSize));
    auto* shadow = static_cast<std::uint8_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, kNativeControllerBackendSize));
    if (object == nullptr || shadow == nullptr) {
        if (shadow != nullptr) HeapFree(GetProcessHeap(), 0, shadow);
        if (object != nullptr) HeapFree(GetProcessHeap(), 0, object);
        Log("Splitscreen4P: adapter allocation failed; maximum remains 3");
        return;
    }

    std::strcpy(reinterpret_cast<char*>(shadow + 0x4444), "XInput Controller 3");
    std::memcpy(shadow + 0x464C, &kFourPlayerAdapterGuid, sizeof(kFourPlayerAdapterGuid));
    shadow[0x466C] = native_backend[0x466C];
    shadow[0x466E] = native_backend[0x466E];
    shadow[0x4670] = native_backend[0x4670];
    CallNativeInputDeviceBaseConstructor(object);
    std::memcpy(g_four_player_pad_vtable, reinterpret_cast<void*>(0x0067B920), sizeof(g_four_player_pad_vtable));
    g_four_player_pad_vtable[0] = reinterpret_cast<std::uintptr_t>(&FourPlayerAdapterDeletingDestructor);
    g_four_player_pad_vtable[0x14 / sizeof(void*)] = reinterpret_cast<std::uintptr_t>(&FourPlayerAdapterPoll);
    g_four_player_pad_vtable[0x5C / sizeof(void*)] = reinterpret_cast<std::uintptr_t>(&FourPlayerAdapterDefaultMap);
    g_four_player_pad_vtable[0xD4 / sizeof(void*)] = reinterpret_cast<std::uintptr_t>(&FourPlayerAdapterDetach);
    *reinterpret_cast<void***>(object) = reinterpret_cast<void**>(g_four_player_pad_vtable);
    *reinterpret_cast<std::uint8_t**>(object + 0x7A0) = shadow;
    *reinterpret_cast<DWORD*>(object + 0x7A4) = 0;
    *reinterpret_cast<DWORD*>(object + 0x04) = *reinterpret_cast<DWORD*>(pad0 + 0x04);
    *reinterpret_cast<DWORD*>(object + 0x124) = 0;
    *reinterpret_cast<DWORD*>(object + 0x8BC) = 0;
    *reinterpret_cast<DWORD*>(object + 0x8E8) = 0;
    // The custom default-map entry used by D0 resolves the shadow through the
    // sidecar, but the manager slot and public capability are not published yet.
    g_four_player_input.adapter = object;
    g_four_player_input.shadow_backend = shadow;
    const int identity_result = CallNativePadIdentityInitializer(object, 3);
    if (identity_result != 1) {
        g_four_player_input.adapter = nullptr;
        g_four_player_input.shadow_backend = nullptr;
        HeapFree(GetProcessHeap(), 0, shadow);
        HeapFree(GetProcessHeap(), 0, object);
        Log("Splitscreen4P: native pad identity initialization failed result=%d; maximum remains 3", identity_result);
        return;
    }

    g_four_player_adapter_for_hooks = object;
    *reinterpret_cast<std::uint8_t**>(manager + kInputManagerBackendOrSlot3Offset) = object;
    *reinterpret_cast<DWORD*>(manager + kInputManagerDeviceCountOffset) = 4;
    g_splitscreen_four_player_capable = ValidatePublishedFourPlayerAdapter(manager);
    if (!g_splitscreen_four_player_capable) {
        *reinterpret_cast<DWORD*>(manager + kInputManagerDeviceCountOffset) = 3;
        *reinterpret_cast<std::uint8_t**>(manager + kInputManagerBackendOrSlot3Offset) = native_backend;
        FourPlayerAdapterDeletingDestructor(object, nullptr, 1);
        Log("Splitscreen4P: post-install invariants failed; adapter removed and maximum remains 3");
        return;
    }

    RestoreSelectedAdapterGuidIfNeeded();
    Log("Splitscreen4P: installed keyboard+three-pad topology count=4 slot3=%p backend=%p name='%s' xinput_user=2",
        object, native_backend, object + 0x18);
}

void ClearFourPlayerSidecarForManager(std::uint8_t* manager)
{
    if (g_four_player_input.manager != manager) {
        return;
    }
    g_four_player_input = {};
    g_four_player_manager_for_hooks = nullptr;
    g_four_player_native_backend_for_hooks = nullptr;
    g_four_player_adapter_for_hooks = nullptr;
    g_splitscreen_four_player_capable = false;
}

__declspec(naked) void SplitscreenFourPlayerConstructorCall521134()
{
    __asm {
        push ebp
        mov ebp, esp
        pushfd
        pushad
        call SnapshotSelectedControllerGuid
        popad
        popfd
        push dword ptr [ebp + 8]
        mov eax, 0x0054FF10
        call eax
        pushfd
        pushad
        push dword ptr [ebp + 8]
        call InstallFourPlayerAdapterAfterNativeConstructor
        add esp, 4
        popad
        popfd
        pop ebp
        ret 4
    }
}

__declspec(naked) void SplitscreenFourPlayerBackendUpdate55B490()
{
    __asm {
        pushfd
        cmp eax, dword ptr [g_four_player_adapter_for_hooks]
        jne stock
        mov eax, dword ptr [g_four_player_native_backend_for_hooks]
    stock:
        popfd
        push ebx
        push esi
        push edi
        mov edi, eax
        push 0x0055B495
        ret
    }
}

__declspec(naked) void SplitscreenFourPlayerBackendShutdown55011D()
{
    __asm {
        mov esi, dword ptr [g_four_player_native_backend_for_hooks]
        cmp edi, dword ptr [g_four_player_manager_for_hooks]
        je selected
        mov esi, dword ptr [edi + 0x28]
    selected:
        test esi, esi
        push 0x00550122
        ret
    }
}

__declspec(naked) void SplitscreenFourPlayerBackendClear550175()
{
    __asm {
        pushfd
        pushad
        push edi
        call ClearFourPlayerSidecarForManager
        add esp, 4
        popad
        popfd
        mov dword ptr [edi + 0x28], 0
        push 0x0055017C
        ret
    }
}

bool WriteFloatOperand(std::uintptr_t address, const float* value);

__declspec(naked) void FovContextProjectionCallHook()
{
    __asm {
        mov dword ptr [g_widescreen_fov_context], 1
        fsubr dword ptr [esp + 0x68]
        fstp dword ptr [esp]
        mov eax, 0x004B4BF0
        call eax
        mov dword ptr [g_widescreen_fov_context], 0
        mov eax, 0x004C0AED
        jmp eax
    }
}

__declspec(naked) void FovContextChainedHook()
{
    __asm {
        mov dword ptr [g_widescreen_fov_context], 1
        fsubr dword ptr [esp + 0x68]
        fstp dword ptr [esp]
        mov eax, 0x004C0AE8
        jmp eax
    }
}

void RecalculateProjectionMatrix(void* projection)
{
    if (projection == nullptr) {
        return;
    }

    auto* bytes = static_cast<std::uint8_t*>(projection);
    const float near_scale = *reinterpret_cast<float*>(bytes + 0x10C);
    const float fov_radians = *reinterpret_cast<float*>(bytes + 0x114);
    const float viewport_aspect = *reinterpret_cast<float*>(bytes + 0x118);
    if (!std::isfinite(near_scale) || !std::isfinite(fov_radians) ||
        !std::isfinite(viewport_aspect) || viewport_aspect <= 0.0f) {
        return;
    }
    const double half_tangent = std::tan(static_cast<double>(fov_radians) * 0.5);
    const float vertical = static_cast<float>(
        half_tangent * static_cast<double>(near_scale) * 0.75
    );
    const float horizontal = vertical * viewport_aspect;

    *reinterpret_cast<float*>(bytes + 0x0F4) = horizontal;
    *reinterpret_cast<float*>(bytes + 0x0F0) = -horizontal;
    *reinterpret_cast<float*>(bytes + 0x0FC) = vertical;
    *reinterpret_cast<float*>(bytes + 0x0F8) = -vertical;
}

__declspec(naked) void ProjectionMatrixExtentsHook()
{
    __asm {
        fmul dword ptr [esp + 0x4C]
        fstp dword ptr [esi + 0x0C]
        fmul dword ptr [esp + 0x4C]
        fstp dword ptr [esi + 0x08]
        pushfd
        pushad
        push ebp
        call RecalculateProjectionMatrix
        add esp, 4
        popad
        popfd
        mov eax, 0x004CBDFD
        jmp eax
    }
}

__declspec(naked) void ProjectionMatrixObjectHook()
{
    __asm {
        pushfd
        pushad
        push eax
        call RecalculateProjectionMatrix
        add esp, 4
        popad
        popfd
        fstp dword ptr [eax + 0x100]
        fstp dword ptr [eax + 0x104]
        mov eax, 0x004C9F6F
        jmp eax
    }
}

__declspec(naked) void ProjectionSplitModeHook()
{
    __asm {
        mov dword ptr [g_projection_split_mode], 0
        mov eax, ebx
        sub eax, 2
        jne not_split
        mov dword ptr [g_projection_split_mode], 1
        cmp byte ptr [g_splitscreen_vertical_render_active], 0
        je horizontal_split
        fmul dword ptr [g_projection_vertical_aspect_scale]
        mov dword ptr [g_projection_split_mode], 2
        jmp not_split
    horizontal_split:
        fadd st(0), st(0)
    not_split:
        mov eax, 0x004C9E30
        jmp eax
    }
}

void RecalculateProjectionStack(void* projection, void* stack)
{
    if (projection == nullptr || stack == nullptr) {
        return;
    }

    auto* projection_bytes = static_cast<std::uint8_t*>(projection);
    auto* stack_bytes = static_cast<std::uint8_t*>(stack);
    const float fov_radians = *reinterpret_cast<float*>(projection_bytes + 0x114);
    const float viewport_aspect = *reinterpret_cast<float*>(projection_bytes + 0x118);
    if (!std::isfinite(fov_radians) || !std::isfinite(viewport_aspect) || viewport_aspect <= 0.0f) {
        return;
    }
    const double half_tangent = std::tan(static_cast<double>(fov_radians) * 0.5);
    const float vertical = static_cast<float>(half_tangent * 0.75);
    const float horizontal = vertical * viewport_aspect;

    *reinterpret_cast<float*>(stack_bytes + 0x18) = horizontal;
    *reinterpret_cast<float*>(stack_bytes + 0x14) = -horizontal;
    *reinterpret_cast<float*>(stack_bytes + 0x20) = vertical;
    *reinterpret_cast<float*>(stack_bytes + 0x1C) = -vertical;
}

__declspec(naked) void ProjectionStackHook59964A()
{
    __asm {
        pushfd
        pushad
        lea eax, [esp + 36]
        push eax
        push ebp
        call RecalculateProjectionStack
        add esp, 8
        popad
        popfd
        fstp st(0)
        call dword ptr [ebx + 0xB0]
        mov eax, 0x00599652
        jmp eax
    }
}

bool IsOldZPatchWideAnchor(const float* source)
{
    return (FloatBitsEqual(source[1], 397.0f) && FloatBitsEqual(source[3], 16.0f)) ||
        (FloatBitsEqual(source[1], 397.5f) && FloatBitsEqual(source[3], 15.0f));
}

bool IsOldZPatchCenteredAnchor(const float* source)
{
    const float x = source[2];
    const float width = source[3];

    return (x >= 351.0f && FloatBitsEqual(width, 51.0f)) ||
        (x >= 342.0f && FloatBitsEqual(width, 97.0f)) ||
        (x >= 340.0f && FloatBitsEqual(width, 78.0f)) ||
        (x >= 343.0f && FloatBitsEqual(width, 79.0f)) ||
        (x >= 337.0f && FloatBitsEqual(width, 107.0f)) ||
        (x >= 343.5f && FloatBitsEqual(width, 93.0f));
}

bool IsOldZPatchScaleOnlyAnchor(const float* source)
{
    const float x = source[2];
    const float width = source[3];

    return (FloatBitsEqual(x, 45.0f) && FloatBitsEqual(width, 64.0f)) ||
        (FloatBitsEqual(x, 36.0f) && FloatBitsEqual(width, 147.0f)) ||
        (FloatBitsEqual(x, 31.0f) && FloatBitsEqual(width, 188.0f)) ||
        (FloatBitsEqual(x, 23.0f) && FloatBitsEqual(width, 61.0f)) ||
        (FloatBitsEqual(x, 23.0f) && FloatBitsEqual(width, 48.0f)) ||
        (FloatBitsEqual(x, 343.0f) && FloatBitsEqual(width, 18.0f)) ||
        (FloatBitsEqual(x, 366.0f) && FloatBitsEqual(width, 18.0f)) ||
        (FloatBitsEqual(x, 294.0f) && FloatBitsEqual(width, 18.0f));
}

void AdjustOldZPatchMenuTransform(const float* source, float* stack_values)
{
    if (!IsReadableMemory(source, sizeof(float) * 5) || stack_values == nullptr || source[3] > 639.0f) {
        return;
    }

    const float scale = g_widescreen_menu_scale_bits != 0
        ? g_widescreen_vertical_scale / *reinterpret_cast<float*>(&g_widescreen_menu_scale_bits)
        : 1.0f;

    if (IsOldZPatchWideAnchor(source)) {
        stack_values[7] *= scale;
        stack_values[5] += static_cast<float>(((static_cast<double>(g_widescreen_aspect) - 1.3333333) / 0.44444444) * 10.0);
        return;
    }

    if (IsOldZPatchCenteredAnchor(source)) {
        stack_values[7] *= scale;
        stack_values[5] = static_cast<float>(
            (((static_cast<double>(stack_values[5]) - static_cast<double>(g_widescreen_screen_width * 0.5f)) * 1.33333333) /
                static_cast<double>(g_widescreen_aspect)) +
            static_cast<double>(g_widescreen_screen_width * 0.5f)
        );
        return;
    }

    if (IsOldZPatchScaleOnlyAnchor(source)) {
        stack_values[7] *= scale;
    }
}

void AdjustCurrentOldZPatchMenuTransform()
{
    AdjustOldZPatchMenuTransform(g_menu_transform_source, g_menu_transform_stack);
}

__declspec(naked) void MenuTransformHeightHook533690()
{
    __asm {
        mov dword ptr [g_menu_transform_source], esi
        mov dword ptr [g_menu_transform_stack], esp
        fmul dword ptr [esi + 0x0C]
        fstp dword ptr [esp + 0x1C]
        pushfd
        pushad
        call AdjustCurrentOldZPatchMenuTransform
        popad
        popfd
        test edx, edx
        push 0x00533697
        ret
    }
}

void EnableMenuCarBackfaceCulling()
{
    // ZPatchFO2 v2.4 stores 0x008DA788 in its own pointer-to-slot global,
    // dereferences that slot exactly once, and invokes vtable offset 0xE4
    // (IDirect3DDevice9::SetRenderState) on the resulting interface. The
    // previous reimplementation dereferenced the interface a second time,
    // mistaking its vtable pointer for the object and crashing on dispatch.
    auto* device = *reinterpret_cast<IDirect3DDevice9**>(0x008DA788);
    if (device == nullptr) {
        return;
    }

    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
}

__declspec(naked) void MenuCarBackfaceCullingHook5AACB0()
{
    __asm {
        pushfd
        pushad
        call EnableMenuCarBackfaceCulling
        popad
        popfd

        // Replay the seven bytes overwritten at 0x005AACB0.
        mov eax, dword ptr [ecx]
        push 3
        call dword ptr [eax + 0x5C]

        mov eax, 0x005AACB7
        jmp eax
    }
}

constexpr DWORD kVanillaMenuCarSurfaceCapacity = 16;
constexpr DWORD kMenuCarSurfaceRecordBytes = 0x34;
constexpr DWORD kMenuCarSurfaceArrayOffset = 0xA20;
constexpr DWORD kVanillaMenuCarMaterialCapacity = 16;
constexpr DWORD kMenuCarMaterialRecordBytes = 0xA0;
constexpr DWORD kMenuCarMaterialArrayOffset = 0x20;
constexpr DWORD kMenuCarObjectBytes = 0xEE0;
constexpr DWORD kMenuCarNestedResourceOffset = 0xDA0;

struct MenuCarSurfaceStorage
{
    void* owner;
    std::uint8_t* materials;
    std::uint8_t* records;
    DWORD material_retained_count;
    DWORD retained_count;
    bool material_reference_clamp_logged;
    bool reference_clamp_logged;
    bool parse_check_logged;
    bool resource_link_guard_logged;
    MenuCarSurfaceStorage* next;
};

MenuCarSurfaceStorage* g_menu_car_surface_storage = nullptr;
bool g_menu_car_surface_hooks_installed = false;
bool g_menu_car_static_reference_coverage_verified = false;
thread_local MenuCarSurfaceStorage* g_current_menu_car_surface_storage = nullptr;
thread_local void* g_current_menu_car_surface_owner = nullptr;
thread_local DWORD g_current_menu_car_serialized_surface_count = 0;
alignas(DWORD) thread_local std::uint8_t g_menu_car_surface_scratch[kMenuCarSurfaceRecordBytes] = {};
alignas(DWORD) thread_local std::uint8_t g_menu_car_material_scratch[kMenuCarMaterialRecordBytes] = {};
thread_local bool g_menu_car_material_scratch_initialized = false;

void MenuCarMaterialBeginHook4A4DFF();
void MenuCarMaterialLoopHook4A4E1F();
void MenuCarMaterialLinkHook4A537C();
void MenuCarSurfaceBeginHook4A535B();
void MenuCarSurfaceLoopHook4A536F();
void MenuCarSurfaceReaderHook4A5604();
void MenuCarParseEndHook4A57CB();
void MenuCarResourceTraversalGuardHook54D0D4();
void FormatBytes(const std::uint8_t* bytes, size_t size, char* output, size_t output_size);

void InitializeMenuCarMaterialRecord(std::uint8_t* record, bool preserve_string)
{
    std::uint8_t saved_string[0x1C] = {};
    if (preserve_string) {
        std::memcpy(saved_string, record + 0x6C, sizeof(saved_string));
    }

    std::memset(record, 0, kMenuCarMaterialRecordBytes);
    *reinterpret_cast<DWORD*>(record + 0x18) = 8;
    for (DWORD offset = 0x20; offset <= 0x2C; offset += sizeof(DWORD)) {
        *reinterpret_cast<DWORD*>(record + offset) = 0x3F800000;
    }
    for (DWORD offset = 0x50; offset <= 0x5C; offset += sizeof(DWORD)) {
        *reinterpret_cast<DWORD*>(record + offset) = 0x3F800000;
    }
    *reinterpret_cast<DWORD*>(record + 0x64) = 0xFFFFFFFF;
    *reinterpret_cast<DWORD*>(record + 0x8C) = 15;

    if (preserve_string) {
        std::memcpy(record + 0x6C, saved_string, sizeof(saved_string));
    } else {
        *reinterpret_cast<DWORD*>(record + 0x80) = 0;
        *reinterpret_cast<DWORD*>(record + 0x84) = 15;
    }
}

void InitializeMenuCarSurfaceRecord(std::uint8_t* record)
{
    std::memset(record, 0, kMenuCarSurfaceRecordBytes);
    *reinterpret_cast<DWORD*>(record + 0x08) = 0x00000300;
    *reinterpret_cast<DWORD*>(record + 0x14) = 4;
}

MenuCarSurfaceStorage* FindMenuCarSurfaceStorage(void* owner)
{
    for (MenuCarSurfaceStorage* storage = g_menu_car_surface_storage; storage != nullptr; storage = storage->next) {
        if (storage->owner == owner) {
            return storage;
        }
    }
    return nullptr;
}

std::uint8_t* EmbeddedMenuCarSurfaceRecord(void* owner, DWORD index)
{
    return static_cast<std::uint8_t*>(owner) + kMenuCarSurfaceArrayOffset + index * kMenuCarSurfaceRecordBytes;
}

std::uint8_t* EmbeddedMenuCarMaterialRecord(void* owner, DWORD index)
{
    return static_cast<std::uint8_t*>(owner) + kMenuCarMaterialArrayOffset + index * kMenuCarMaterialRecordBytes;
}

MenuCarSurfaceStorage* GetOrCreateMenuCarSurfaceStorage(void* owner)
{
    MenuCarSurfaceStorage* storage = FindMenuCarSurfaceStorage(owner);
    if (storage != nullptr) {
        return storage;
    }

    storage = static_cast<MenuCarSurfaceStorage*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MenuCarSurfaceStorage))
    );
    if (storage != nullptr) {
        storage->owner = owner;
        storage->next = g_menu_car_surface_storage;
        g_menu_car_surface_storage = storage;
    } else {
        Log(
            "MenuCarMaxSurfaces: storage metadata allocation failed for object=0x%p; using bounded scratch fallback",
            owner
        );
    }
    return storage;
}

std::uint8_t* SelectCurrentMenuCarMaterialRecord(DWORD index)
{
    if (index < g_menu_car_max_surfaces) {
        if (g_current_menu_car_surface_storage != nullptr &&
            g_current_menu_car_surface_storage->materials != nullptr) {
            return g_current_menu_car_surface_storage->materials + index * kMenuCarMaterialRecordBytes;
        }
        if (g_current_menu_car_surface_owner != nullptr && index < kVanillaMenuCarMaterialCapacity) {
            return EmbeddedMenuCarMaterialRecord(g_current_menu_car_surface_owner, index);
        }
    }

    InitializeMenuCarMaterialRecord(
        g_menu_car_material_scratch,
        g_menu_car_material_scratch_initialized
    );
    g_menu_car_material_scratch_initialized = true;
    return g_menu_car_material_scratch;
}

std::uint8_t* __cdecl BeginMenuCarMaterialParse(void* owner, DWORD serialized_count)
{
    MenuCarSurfaceStorage* storage = GetOrCreateMenuCarSurfaceStorage(owner);
    if (storage != nullptr && storage->materials == nullptr) {
        const SIZE_T allocation_bytes =
            static_cast<SIZE_T>(g_menu_car_max_surfaces) * kMenuCarMaterialRecordBytes;
        storage->materials = static_cast<std::uint8_t*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, allocation_bytes)
        );
        if (storage->materials != nullptr) {
            for (DWORD index = 0; index < g_menu_car_max_surfaces; ++index) {
                InitializeMenuCarMaterialRecord(
                    storage->materials + index * kMenuCarMaterialRecordBytes,
                    false
                );
            }
            Log(
                "MenuCarMaxSurfaces: allocated %lu material records (%lu bytes) for object=0x%p",
                static_cast<unsigned long>(g_menu_car_max_surfaces),
                static_cast<unsigned long>(allocation_bytes),
                owner
            );
        } else {
            Log(
                "MenuCarMaxSurfaces: material allocation failed for object=0x%p; retaining at most %lu embedded materials",
                owner,
                static_cast<unsigned long>(std::min<DWORD>(g_menu_car_max_surfaces, kVanillaMenuCarMaterialCapacity))
            );
        }
    } else if (storage != nullptr && storage->materials != nullptr) {
        for (DWORD index = 0; index < g_menu_car_max_surfaces; ++index) {
            InitializeMenuCarMaterialRecord(
                storage->materials + index * kMenuCarMaterialRecordBytes,
                true
            );
        }
    }

    g_current_menu_car_surface_storage = storage;
    g_current_menu_car_surface_owner = owner;

    const DWORD available_capacity = storage != nullptr && storage->materials != nullptr
        ? g_menu_car_max_surfaces
        : std::min<DWORD>(g_menu_car_max_surfaces, kVanillaMenuCarMaterialCapacity);
    const DWORD retained_count = std::min<DWORD>(serialized_count, available_capacity);
    if (storage != nullptr) {
        storage->material_retained_count = retained_count;
        storage->material_reference_clamp_logged = false;
        storage->parse_check_logged = false;
        storage->resource_link_guard_logged = false;
    }

    // +0x08 is the stock material count. Clamp it to storage that is actually
    // addressable so no later count consumer can walk beyond retained data.
    *reinterpret_cast<DWORD*>(static_cast<std::uint8_t*>(owner) + 0x08) = retained_count;

    if (serialized_count > available_capacity) {
        Log(
            "MenuCarMaxSurfaces: object=0x%p declares %lu materials; retaining %lu and safely discarding %lu",
            owner,
            static_cast<unsigned long>(serialized_count),
            static_cast<unsigned long>(retained_count),
            static_cast<unsigned long>(serialized_count - retained_count)
        );
    }
    return SelectCurrentMenuCarMaterialRecord(0);
}

std::uint8_t* __cdecl SelectMenuCarMaterialParseDestination(DWORD index)
{
    return SelectCurrentMenuCarMaterialRecord(index);
}

std::uint8_t* __cdecl ResolveMenuCarMaterialRecord(void* owner, DWORD index)
{
    MenuCarSurfaceStorage* storage = FindMenuCarSurfaceStorage(owner);
    if (storage == nullptr || storage->material_retained_count == 0) {
        InitializeMenuCarMaterialRecord(
            g_menu_car_material_scratch,
            g_menu_car_material_scratch_initialized
        );
        g_menu_car_material_scratch_initialized = true;
        return g_menu_car_material_scratch;
    }

    if (index >= storage->material_retained_count) {
        if (!storage->material_reference_clamp_logged) {
            Log(
                "MenuCarMaxSurfaces: object=0x%p surface material reference %lu exceeds retained material count %lu; clamping",
                owner,
                static_cast<unsigned long>(index),
                static_cast<unsigned long>(storage->material_retained_count)
            );
            storage->material_reference_clamp_logged = true;
        }
        index = storage->material_retained_count - 1;
    }

    if (storage->materials != nullptr) {
        return storage->materials + index * kMenuCarMaterialRecordBytes;
    }
    return EmbeddedMenuCarMaterialRecord(owner, index);
}

std::uint8_t* SelectCurrentMenuCarSurfaceRecord(DWORD remaining_count)
{
    DWORD index = 0;
    if (remaining_count <= g_current_menu_car_serialized_surface_count) {
        index = g_current_menu_car_serialized_surface_count - remaining_count;
    }

    if (index < g_menu_car_max_surfaces) {
        if (g_current_menu_car_surface_storage != nullptr &&
            g_current_menu_car_surface_storage->records != nullptr) {
            return g_current_menu_car_surface_storage->records + index * kMenuCarSurfaceRecordBytes;
        }
        if (g_current_menu_car_surface_owner != nullptr && index < kVanillaMenuCarSurfaceCapacity) {
            return EmbeddedMenuCarSurfaceRecord(g_current_menu_car_surface_owner, index);
        }
    }

    // The parser must still consume every variable-length serialized surface
    // or the following BMOD/MESH records become misaligned. A freshly reset
    // scratch record safely absorbs each configured-capacity overflow record.
    InitializeMenuCarSurfaceRecord(g_menu_car_surface_scratch);
    return g_menu_car_surface_scratch;
}

std::uint8_t* __cdecl BeginMenuCarSurfaceParse(void* owner, DWORD serialized_count)
{
    MenuCarSurfaceStorage* storage = GetOrCreateMenuCarSurfaceStorage(owner);

    if (storage != nullptr && storage->records == nullptr) {
        const SIZE_T allocation_bytes =
            static_cast<SIZE_T>(g_menu_car_max_surfaces) * kMenuCarSurfaceRecordBytes;
        storage->records = static_cast<std::uint8_t*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, allocation_bytes)
        );
        if (storage->records != nullptr) {
            for (DWORD index = 0; index < g_menu_car_max_surfaces; ++index) {
                InitializeMenuCarSurfaceRecord(storage->records + index * kMenuCarSurfaceRecordBytes);
            }
            Log(
                "MenuCarMaxSurfaces: allocated %lu records (%lu bytes) for object=0x%p",
                static_cast<unsigned long>(g_menu_car_max_surfaces),
                static_cast<unsigned long>(allocation_bytes),
                owner
            );
        } else {
            Log(
                "MenuCarMaxSurfaces: allocation failed for object=0x%p; retaining at most %lu embedded records",
                owner,
                static_cast<unsigned long>(std::min<DWORD>(g_menu_car_max_surfaces, kVanillaMenuCarSurfaceCapacity))
            );
        }
    } else if (storage != nullptr && storage->records != nullptr) {
        for (DWORD index = 0; index < g_menu_car_max_surfaces; ++index) {
            InitializeMenuCarSurfaceRecord(storage->records + index * kMenuCarSurfaceRecordBytes);
        }
    }

    if (storage != nullptr) {
        const DWORD available_capacity = storage->records != nullptr
            ? g_menu_car_max_surfaces
            : std::min<DWORD>(g_menu_car_max_surfaces, kVanillaMenuCarSurfaceCapacity);
        storage->retained_count = std::min<DWORD>(serialized_count, available_capacity);
        storage->reference_clamp_logged = false;
        storage->parse_check_logged = false;
    }

    g_current_menu_car_surface_storage = storage;
    g_current_menu_car_surface_owner = owner;
    g_current_menu_car_serialized_surface_count = serialized_count;

    if (serialized_count > g_menu_car_max_surfaces) {
        Log(
            "MenuCarMaxSurfaces: object=0x%p declares %lu surfaces; retaining %lu and safely discarding %lu",
            owner,
            static_cast<unsigned long>(serialized_count),
            static_cast<unsigned long>(g_menu_car_max_surfaces),
            static_cast<unsigned long>(serialized_count - g_menu_car_max_surfaces)
        );
    }

    return SelectCurrentMenuCarSurfaceRecord(serialized_count) + 0x0C;
}

std::uint8_t* __cdecl SelectMenuCarSurfaceParseDestination(DWORD remaining_count)
{
    return SelectCurrentMenuCarSurfaceRecord(remaining_count) + 0x0C;
}

std::uint8_t* __cdecl ResolveMenuCarSurfaceRecord(void* owner, DWORD index)
{
    MenuCarSurfaceStorage* storage = FindMenuCarSurfaceStorage(owner);
    if (storage == nullptr || storage->retained_count == 0) {
        InitializeMenuCarSurfaceRecord(g_menu_car_surface_scratch);
        return g_menu_car_surface_scratch;
    }

    if (index >= storage->retained_count) {
        if (!storage->reference_clamp_logged) {
            Log(
                "MenuCarMaxSurfaces: object=0x%p model surface reference %lu exceeds retained count %lu; clamping",
                owner,
                static_cast<unsigned long>(index),
                static_cast<unsigned long>(storage->retained_count)
            );
            storage->reference_clamp_logged = true;
        }
        index = storage->retained_count - 1;
    }

    if (storage->records != nullptr) {
        return storage->records + index * kMenuCarSurfaceRecordBytes;
    }
    return EmbeddedMenuCarSurfaceRecord(owner, index);
}

bool MenuCarJumpTargets(std::uintptr_t address, const void* target)
{
    const auto* site = reinterpret_cast<const std::uint8_t*>(address);
    if (site[0] != 0xE9) {
        return false;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, site + 1, sizeof(displacement));
    return site + 5 + displacement == reinterpret_cast<const std::uint8_t*>(target);
}

bool VerifyMenuCarStaticReferenceCoverage()
{
    struct ReferencePattern
    {
        std::uintptr_t address;
        const std::uint8_t* bytes;
        size_t size;
        const char* label;
    };

    const std::uint8_t material_constructor_base[] = {0x8D, 0x46, 0x20};
    const std::uint8_t material_constructor_advance_eax[] = {0x05, 0xA0, 0x00, 0x00, 0x00};
    const std::uint8_t material_constructor_advance_edi[] = {0x81, 0xC7, 0xA0, 0x00, 0x00, 0x00};
    const std::uint8_t surface_constructor_base[] = {0x8D, 0x86, 0x20, 0x0A, 0x00, 0x00};
    const std::uint8_t surface_constructor_advance[] = {0x83, 0xC0, 0x34};
    const std::uint8_t material_destructor_end[] = {0x81, 0xC6, 0xA4, 0x0A, 0x00, 0x00};
    const std::uint8_t material_destructor_stride[] = {0x81, 0xEE, 0xA0, 0x00, 0x00, 0x00};
    const std::uint8_t material_parser_advance[] = {0x81, 0xC6, 0xA0, 0x00, 0x00, 0x00};
    const std::uint8_t surface_parser_advance[] = {0x83, 0xC2, 0x34};
    const std::uint8_t model_surface_scale[] = {0x6B, 0xC0, 0x34};
    const std::uint8_t manager_allocation[] = {0x68, 0xD0, 0x1E, 0x00, 0x00};
    const std::uint8_t owner_stride[] = {0x81, 0xC7, 0xE0, 0x0E, 0x00, 0x00};
    const std::uint8_t nested_resource_base[] = {0x8D, 0xBE, 0xA0, 0x0D, 0x00, 0x00};
    const std::uint8_t trailing_name[] = {0x81, 0xC1, 0x6C, 0x0E, 0x00, 0x00};

    const ReferencePattern patterns[] = {
        {0x004A3F65, material_constructor_base, sizeof(material_constructor_base), "material constructor base +0x20"},
        {0x004A3F91, material_constructor_advance_eax, sizeof(material_constructor_advance_eax), "material constructor EAX stride 0xA0"},
        {0x004A3F96, material_constructor_advance_edi, sizeof(material_constructor_advance_edi), "material constructor EDI stride 0xA0"},
        {0x004A3F9F, surface_constructor_base, sizeof(surface_constructor_base), "surface constructor base +0xA20"},
        {0x004A3FE9, surface_constructor_advance, sizeof(surface_constructor_advance), "surface constructor stride 0x34"},
        {0x004A40F1, material_destructor_end, sizeof(material_destructor_end), "material destructor end +0xAA4"},
        {0x004A4106, material_destructor_stride, sizeof(material_destructor_stride), "material destructor stride 0xA0"},
        {0x004A509E, material_parser_advance, sizeof(material_parser_advance), "material parser stride 0xA0"},
        {0x004A54AD, surface_parser_advance, sizeof(surface_parser_advance), "surface parser stride 0x34"},
        {0x004A55FB, model_surface_scale, sizeof(model_surface_scale), "BMOD surface scale 0x34"},
        {0x004ABAAA, manager_allocation, sizeof(manager_allocation), "manager allocation 0x1ED0"},
        {0x004A4177, owner_stride, sizeof(owner_stride), "two-owner stride 0xEE0"},
        {0x004A4004, nested_resource_base, sizeof(nested_resource_base), "trailing nested resource +0xDA0"},
        {0x004A56CE, trailing_name, sizeof(trailing_name), "trailing resource name +0xE6C"},
    };

    for (const ReferencePattern& pattern : patterns) {
        const auto* address = reinterpret_cast<const std::uint8_t*>(pattern.address);
        if (std::memcmp(address, pattern.bytes, pattern.size) != 0) {
            char actual[64] = {};
            FormatBytes(address, pattern.size, actual, sizeof(actual));
            Log(
                "*** WARNING *** MenuCarMaxSurfaces full-exe reference coverage mismatch at 0x%08lX (%s) actual=%s",
                static_cast<unsigned long>(pattern.address),
                pattern.label,
                actual
            );
            return false;
        }
    }

    Log(
        "MenuCarMaxSurfaces: full-exe reference coverage verified for 10 array base/stride consumers plus manager allocation=0x1ED0, owner stride=0xEE0, nested resource=+0xDA0, and name=+0xE6C"
    );
    return true;
}

bool MenuCarMemoryRangeHasAccess(const void* address, size_t bytes, bool require_write)
{
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(address);
    if (begin < 0x10000 || (begin & (alignof(DWORD) - 1)) != 0 || bytes == 0 ||
        begin > static_cast<std::uintptr_t>(-1) - bytes) {
        return false;
    }

    MEMORY_BASIC_INFORMATION region = {};
    if (VirtualQuery(address, &region, sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    const DWORD protection = region.Protect & 0xFF;
    const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE ||
        protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    const bool writable = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    const std::uintptr_t region_end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
    return readable && (!require_write || writable) && begin + bytes <= region_end;
}

bool IsSaneMenuCarResourceNodeAddress(const void* node)
{
    // This traversal reads through node+0xC8. VirtualQuery makes the assertion
    // catch invalid mapped/unmapped values as well as the observed low
    // sentinel 0x0F without probing the address first.
    return MenuCarMemoryRangeHasAccess(node, 0xCC, false);
}

bool __cdecl ValidateMenuCarResourceTraversalNode(
    void* node,
    DWORD depth,
    const std::uint8_t* traversal_stack
)
{
    if (IsSaneMenuCarResourceNodeAddress(node)) {
        return true;
    }

    void* root = node;
    void* parent = nullptr;
    if (depth > 0 && depth <= 128 && traversal_stack != nullptr) {
        // 0x0054D0B0 stores the root at local stack +0x14 on its first
        // descent and the current parent at +0x10+depth*4. These are the
        // same slots read by stock 0x0054D17A while unwinding.
        root = *reinterpret_cast<void* const*>(traversal_stack + 0x14);
        parent = *reinterpret_cast<void* const*>(traversal_stack + 0x10 + depth * sizeof(void*));
    }

    MenuCarSurfaceStorage* storage = nullptr;
    const std::uintptr_t root_address = reinterpret_cast<std::uintptr_t>(root);
    if (root_address >= kMenuCarNestedResourceOffset) {
        auto* possible_owner = reinterpret_cast<void*>(root_address - kMenuCarNestedResourceOffset);
        storage = FindMenuCarSurfaceStorage(possible_owner);
    }

    // This detour sits in a shared traversal routine. Preserve stock behavior
    // for every caller except a menucar owner registered by the capacity
    // patch; the dump-proven failure is inside that owner's +0xDA0 object.
    if (storage == nullptr) {
        return true;
    }

    const char* link = "unknown";
    bool cleared = false;
    if (parent != nullptr && MenuCarMemoryRangeHasAccess(parent, 0xCC, true)) {
        auto** child = reinterpret_cast<void**>(static_cast<std::uint8_t*>(parent) + 0xC4);
        auto** sibling = reinterpret_cast<void**>(static_cast<std::uint8_t*>(parent) + 0xC8);
        if (*child == node) {
            *child = nullptr;
            link = "+0xC4 child";
            cleared = true;
        } else if (*sibling == node) {
            *sibling = nullptr;
            link = "+0xC8 sibling";
            cleared = true;
        }
    }

    if (!storage->resource_link_guard_logged) {
        Log(
            "*** WARNING *** MenuCarMaxSurfaces runtime reference guard at 0x0054D0D4 rejected node=0x%p depth=%lu root=0x%p parent=0x%p link=%s action=%s owner=0x%p. This is the detectable unpatched-reference assertion.",
            node,
            static_cast<unsigned long>(depth),
            root,
            parent,
            link,
            cleared ? "cleared-and-skipped" : "skipped",
            storage->owner
        );
        storage->resource_link_guard_logged = true;
    }
    return false;
}

void __cdecl FinishMenuCarParse(void* owner)
{
    MenuCarSurfaceStorage* storage = FindMenuCarSurfaceStorage(owner);
    const bool coverage_ok = g_menu_car_surface_hooks_installed &&
        g_menu_car_static_reference_coverage_verified &&
        MenuCarJumpTargets(0x004A4DFF, reinterpret_cast<const void*>(&MenuCarMaterialBeginHook4A4DFF)) &&
        MenuCarJumpTargets(0x004A4E1F, reinterpret_cast<const void*>(&MenuCarMaterialLoopHook4A4E1F)) &&
        MenuCarJumpTargets(0x004A537C, reinterpret_cast<const void*>(&MenuCarMaterialLinkHook4A537C)) &&
        MenuCarJumpTargets(0x004A535B, reinterpret_cast<const void*>(&MenuCarSurfaceBeginHook4A535B)) &&
        MenuCarJumpTargets(0x004A536F, reinterpret_cast<const void*>(&MenuCarSurfaceLoopHook4A536F)) &&
        MenuCarJumpTargets(0x004A5604, reinterpret_cast<const void*>(&MenuCarSurfaceReaderHook4A5604)) &&
        MenuCarJumpTargets(0x004A57CB, reinterpret_cast<const void*>(&MenuCarParseEndHook4A57CB)) &&
        MenuCarJumpTargets(0x0054D0D4, reinterpret_cast<const void*>(&MenuCarResourceTraversalGuardHook54D0D4));

    bool pointers_ok = storage != nullptr && storage->materials != nullptr && storage->records != nullptr;
    if (pointers_ok) {
        const std::uintptr_t material_begin = reinterpret_cast<std::uintptr_t>(storage->materials);
        const std::uintptr_t material_end = material_begin +
            static_cast<std::uintptr_t>(storage->material_retained_count) * kMenuCarMaterialRecordBytes;
        for (DWORD index = 0; index < storage->retained_count; ++index) {
            const auto* surface = storage->records + index * kMenuCarSurfaceRecordBytes;
            const std::uintptr_t material = *reinterpret_cast<const std::uintptr_t*>(surface);
            if (material < material_begin || material >= material_end ||
                ((material - material_begin) % kMenuCarMaterialRecordBytes) != 0) {
                pointers_ok = false;
                break;
            }
        }
    }

    if (!coverage_ok || !pointers_ok) {
        Log(
            "*** WARNING *** MenuCarMaxSurfaces parse-end self-check FAILED for object=0x%p: hooks=%s material/surface pointers=%s. Rendering is not considered safe.",
            owner,
            coverage_ok ? "complete" : "INCOMPLETE",
            pointers_ok ? "bounded" : "INVALID"
        );
        return;
    }

    if (!storage->parse_check_logged) {
        Log(
            "MenuCarMaxSurfaces: parse-end self-check passed for object=0x%p materials=%lu surfaces=%lu; all eight detours and static full-exe reference coverage are active",
            owner,
            static_cast<unsigned long>(storage->material_retained_count),
            static_cast<unsigned long>(storage->retained_count)
        );
        storage->parse_check_logged = true;
    }
}

__declspec(naked) void MenuCarMaterialBeginHook4A4DFF()
{
    __asm {
        pushfd
        pushad
        push dword ptr [esp + 0x68]
        push dword ptr [esp + 0x40]
        call BeginMenuCarMaterialParse
        add esp, 8
        mov dword ptr [esp + 0x14], eax
        popad
        popfd

        push 0x004A4E06
        ret
    }
}

__declspec(naked) void MenuCarMaterialLoopHook4A4E1F()
{
    __asm {
        pushfd
        pushad
        push dword ptr [esp + 0x48]
        call SelectMenuCarMaterialParseDestination
        add esp, 4
        mov dword ptr [esp + 0x04], eax
        mov dword ptr [esp + 0x40], eax
        popad
        popfd

        sub eax, edx
        push 0x004A4E25
        ret
    }
}

__declspec(naked) void MenuCarMaterialLinkHook4A537C()
{
    __asm {
        pushfd
        pushad
        mov eax, dword ptr [esp + 0x98]
        mov ecx, dword ptr [esp + 0x3C]
        push eax
        push ecx
        call ResolveMenuCarMaterialRecord
        add esp, 8
        mov dword ptr [esp + 0x18], eax
        popad
        popfd

        mov esi, dword ptr [esp + 0x18]
        mov dword ptr [edx - 0x0C], ecx
        push 0x004A5391
        ret
    }
}

__declspec(naked) void MenuCarSurfaceBeginHook4A535B()
{
    __asm {
        // Preserve the serialized count in EAX. At this site object is
        // [ESP+0x18]; BeginMenuCarSurfaceParse returns record zero +0x0C.
        push eax
        push eax
        push dword ptr [esp + 0x20]
        call BeginMenuCarSurfaceParse
        add esp, 8
        mov ecx, eax
        pop eax
        mov dword ptr [esp + 0x1C], ecx

        mov edx, 0x004A5369
        jmp edx
    }
}

__declspec(naked) void MenuCarSurfaceLoopHook4A536F()
{
    __asm {
        // Select the configured heap slot, or the scratch record after the
        // limit. Preserve the flags and registers that the two stolen MOVs
        // would have left unchanged.
        pushfd
        pushad
        push dword ptr [esp + 0x38]
        call SelectMenuCarSurfaceParseDestination
        add esp, 4
        mov dword ptr [esp + 0x14], eax
        mov dword ptr [esp + 0x40], eax
        popad
        popfd

        mov esi, ebx
        mov ecx, 7
        push 0x004A5376
        ret
    }
}

__declspec(naked) void MenuCarSurfaceReaderHook4A5604()
{
    __asm {
        // EAX is surface_id*0x34 and EDX is the menucar object. Convert the
        // byte offset back to an ID before resolving the replacement record.
        push ecx
        push edx
        mov ecx, 0x34
        xor edx, edx
        div ecx
        push eax
        push dword ptr [esp + 0x04]
        call ResolveMenuCarSurfaceRecord
        add esp, 8
        pop edx
        pop ecx

        push 0x004A560B
        ret
    }
}

__declspec(naked) void MenuCarParseEndHook4A57CB()
{
    __asm {
        pushfd
        pushad
        push eax
        call FinishMenuCarParse
        add esp, 4
        popad
        popfd

        mov ecx, dword ptr [eax + 0x0D8C]
        pop esi
        push 0x004A57D2
        ret
    }
}

__declspec(naked) void MenuCarResourceTraversalGuardHook54D0D4()
{
    __asm {
        // The observed crash reached stock 0x0054D0D4 with EBP=0x0F after
        // following the menucar nested resource root's +0xC4 child link.
        // Validate the node before replaying `test byte ptr [ebp+10h],4`.
        pushfd
        pushad
        lea eax, [esp + 0x24]
        push eax
        push ebx
        push ebp
        call ValidateMenuCarResourceTraversalNode
        add esp, 0x0C
        mov dword ptr [esp + 0x1C], eax
        popad
        popfd

        test eax, eax
        jz invalid_node
        test byte ptr [ebp + 0x10], 4
        jnz stock_flag_set
        push 0x0054D0DA
        ret

    stock_flag_set:
        push 0x0054D14D
        ret

    invalid_node:
        // Stock 0x0054D176 unwinds the saved parent and continues at its
        // sibling, or exits when the bad node was already at depth zero.
        push 0x0054D176
        ret
    }
}

__declspec(naked) void SplitscreenFilesystemHook520F7E()
{
    __asm {
        // Parse the dedicated split-screen filesystem list before replaying
        // the stock "-binarydb" argument setup overwritten at 0x00520F7E.
        // The added parser call is isolated so its register/flag changes are
        // invisible to the surrounding startup routine.
        pushfd
        pushad
        lea esi, g_splitscreen_filesystem_name
        mov eax, 0x00520E10
        call eax
        popad
        popfd

        push 0x00677DF8
        mov eax, 0x00520F83
        jmp eax
    }
}

bool IsSplitscreenMode()
{
    auto* game_flow = *reinterpret_cast<std::uint8_t**>(0x008E8410);
    return game_flow != nullptr && *reinterpret_cast<DWORD*>(game_flow + 0x464) == 10;
}

DWORD GetSplitscreenLayoutStateBits()
{
    return (g_splitscreen_vertical_capable ? 2u : 0u) |
        (g_splitscreen_vertical_capable && g_splitscreen_vertical_layout ? 1u : 0u);
}

int PushSplitscreenLayoutState(void* lua_state)
{
    using LuaPushIntegerFn = void (__cdecl*)(void*, int);
    reinterpret_cast<LuaPushIntegerFn>(0x005B46E0)(
        lua_state,
        static_cast<int>(GetSplitscreenLayoutStateBits())
    );
    return 1;
}

int SetSplitscreenLayoutFromLua(void* lua_state, bool requested_vertical)
{
    const bool requested_active = requested_vertical && g_splitscreen_vertical_capable;
    const bool saved = SaveSplitscreenLayoutState(requested_active);
    if (saved) {
        g_splitscreen_vertical_layout = requested_active;
    }
    Log(
        "Splitscreen layout UI: requested=%s active=%s persistence=%s",
        requested_vertical ? "Vertical" : "Horizontal",
        g_splitscreen_vertical_layout ? "Vertical" : "Horizontal",
        saved ? "saved" : "failed"
    );
    return PushSplitscreenLayoutState(lua_state);
}

int __cdecl LuaGetSplitscreenLayoutState(void* lua_state)
{
    return PushSplitscreenLayoutState(lua_state);
}

int __cdecl LuaGetSplitscreenMaxPlayers(void* lua_state)
{
    using LuaPushIntegerFn = void (__cdecl*)(void*, int);
    reinterpret_cast<LuaPushIntegerFn>(0x005B46E0)(
        lua_state,
        g_splitscreen_four_player_capable ? 4 :
            (g_splitscreen_three_player_capable ? 3 : 2)
    );
    return 1;
}

int __cdecl LuaSetSplitscreenLayoutHorizontal(void* lua_state)
{
    return SetSplitscreenLayoutFromLua(lua_state, false);
}

int __cdecl LuaSetSplitscreenLayoutVertical(void* lua_state)
{
    return SetSplitscreenLayoutFromLua(lua_state, true);
}

void __cdecl RegisterSplitscreenLayoutBinding(void* lua_state, int input_table_index)
{
    using LuaPushStringFn = void (__cdecl*)(void*, const char*);
    using LuaPushCClosureFn = void (__cdecl*)(void*, void*, int);
    using LuaSetTableFn = void (__cdecl*)(void*, int);

    struct Binding {
        const char* name;
        void* function;
    };
    const Binding bindings[] = {
        {"GetSplitscreenLayoutState", reinterpret_cast<void*>(&LuaGetSplitscreenLayoutState)},
        {"GetSplitscreenMaxPlayers", reinterpret_cast<void*>(&LuaGetSplitscreenMaxPlayers)},
        {"SetSplitscreenLayoutHorizontal", reinterpret_cast<void*>(&LuaSetSplitscreenLayoutHorizontal)},
        {"SetSplitscreenLayoutVertical", reinterpret_cast<void*>(&LuaSetSplitscreenLayoutVertical)},
    };

    for (const Binding& binding : bindings) {
        reinterpret_cast<LuaPushStringFn>(0x005B4790)(lua_state, binding.name);
        reinterpret_cast<LuaPushCClosureFn>(0x005B48A0)(lua_state, binding.function, 0);
        reinterpret_cast<LuaSetTableFn>(0x005B4E20)(lua_state, input_table_index);
    }
}

__declspec(naked) void SplitscreenInputRegistrationHook4A738A()
{
    __asm {
        pushfd
        pushad
        push esi
        push ebp
        call RegisterSplitscreenLayoutBinding
        add esp, 8
        popad
        popfd

        // Replay the first stock controller-constant registration push.
        push 0x00671AEC
        mov eax, 0x004A738F
        jmp eax
    }
}

bool IsLiveVerticalSplitscreenVisualState()
{
    auto* registry = *reinterpret_cast<std::uint8_t**>(0x00696DC8);
    auto* renderer = *reinterpret_cast<std::uint8_t**>(0x008DA718);
    if (!IsReadableMemory(registry, 0x64) || !IsReadableMemory(renderer, 0x10)) {
        return false;
    }

    const DWORD device_width = *reinterpret_cast<DWORD*>(renderer + 0x08);
    const DWORD device_height = *reinterpret_cast<DWORD*>(renderer + 0x0C);
    if (device_width < 2 || device_height == 0 ||
        *reinterpret_cast<DWORD*>(registry + 0x30) != 2) {
        return false;
    }

    const DWORD left_width = device_width / 2;
    const DWORD right_width = device_width - left_width;
    auto* left = registry + 0x34;
    auto* right = left + 0x18;
    return
        *reinterpret_cast<DWORD*>(left + 0x00) == 0 &&
        *reinterpret_cast<DWORD*>(left + 0x04) == 0 &&
        *reinterpret_cast<DWORD*>(left + 0x08) == left_width &&
        *reinterpret_cast<DWORD*>(left + 0x0C) == device_height &&
        *reinterpret_cast<DWORD*>(right + 0x00) == left_width &&
        *reinterpret_cast<DWORD*>(right + 0x04) == 0 &&
        *reinterpret_cast<DWORD*>(right + 0x08) == right_width &&
        *reinterpret_cast<DWORD*>(right + 0x0C) == device_height;
}

bool IsLiveGridSplitscreenState()
{
    auto* registry = *reinterpret_cast<std::uint8_t**>(0x00696DC8);
    auto* renderer = *reinterpret_cast<std::uint8_t**>(0x008DA718);
    if (!IsReadableMemory(registry, 0x34) || !IsReadableMemory(renderer, 0x10)) {
        return false;
    }

    const DWORD viewport_count = *reinterpret_cast<DWORD*>(registry + 0x30);
    if (viewport_count != 3 && viewport_count != 4) {
        return false;
    }
    if (!IsReadableMemory(registry, 0x34 + static_cast<size_t>(viewport_count) * 0x18)) {
        return false;
    }

    const DWORD device_width = *reinterpret_cast<DWORD*>(renderer + 0x08);
    const DWORD device_height = *reinterpret_cast<DWORD*>(renderer + 0x0C);
    if (device_width < 2 || device_height < 2) {
        return false;
    }

    const DWORD half_width = device_width / 2;
    const DWORD half_height = device_height / 2;
    const DWORD expected[4][4] = {
        {0, 0, half_width, half_height},
        {half_width, 0, half_width, half_height},
        {0, half_height, half_width, half_height},
        {half_width, half_height, half_width, half_height},
    };
    for (DWORD i = 0; i < viewport_count; ++i) {
        auto* record = registry + 0x34 + i * 0x18;
        for (DWORD field = 0; field < 4; ++field) {
            if (*reinterpret_cast<DWORD*>(record + field * sizeof(DWORD)) != expected[i][field]) {
                return false;
            }
        }
    }
    return true;
}

void RefreshVerticalSplitscreenVisualState()
{
    g_splitscreen_vertical_render_active = IsLiveVerticalSplitscreenVisualState();
}

void RewriteSplitscreenLayout(void* layout_object, DWORD viewport_count)
{
    // Only the world currently published as the renderer's viewport registry
    // owns the live split layout. Other layout builders must not clear the
    // render latch for that registry.
    auto* current_registry = *reinterpret_cast<void**>(0x00696DC8);
    if (layout_object == nullptr || layout_object != current_registry) {
        return;
    }

    g_splitscreen_vertical_render_active = false;
    if (viewport_count == 3 && g_splitscreen_three_player_capable) {
        if (!IsSplitscreenMode()) {
            return;
        }
        if (!IsWritableMemory(layout_object, 0x7C)) {
            g_splitscreen_three_player_capable = false;
            Log("Splitscreen 3P: viewport layout is not writable; disabling three-player capability");
            return;
        }
        auto* renderer = *reinterpret_cast<std::uint8_t**>(0x008DA718);
        if (!IsReadableMemory(renderer, 0x10)) {
            g_splitscreen_three_player_capable = false;
            Log("Splitscreen 3P: renderer dimensions unavailable; disabling three-player capability");
            return;
        }
        const DWORD device_width = *reinterpret_cast<DWORD*>(renderer + 0x08);
        const DWORD device_height = *reinterpret_cast<DWORD*>(renderer + 0x0C);
        if (device_width < 2 || device_height < 2) {
            g_splitscreen_three_player_capable = false;
            Log("Splitscreen 3P: invalid device dimensions %lux%lu; disabling three-player capability",
                static_cast<unsigned long>(device_width),
                static_cast<unsigned long>(device_height));
            return;
        }
        const DWORD half_width = device_width / 2;
        const DWORD half_height = device_height / 2;
        const DWORD geometry[3][4] = {
            {0, 0, half_width, half_height},
            {half_width, 0, half_width, half_height},
            {0, half_height, half_width, half_height},
        };
        auto* layout = static_cast<std::uint8_t*>(layout_object);
        for (DWORD i = 0; i < 3; ++i) {
            auto* record = layout + 0x34 + i * 0x18;
            for (DWORD field = 0; field < 4; ++field) {
                *reinterpret_cast<DWORD*>(record + field * sizeof(DWORD)) = geometry[i][field];
            }
        }
        return;
    }
    if (!g_splitscreen_vertical_capable || !g_splitscreen_vertical_layout || viewport_count != 2) {
        return;
    }

    const bool split_setup_mode = IsSplitscreenMode();
    if (!IsReadableMemory(layout_object, 0x64)) {
        if (split_setup_mode) {
            g_splitscreen_vertical_layout = false;
            SaveSplitscreenLayoutState(false);
            Log("SplitscreenOrientation: invalid viewport layout object; reverting to Horizontal");
        }
        return;
    }

    auto* renderer = *reinterpret_cast<std::uint8_t**>(0x008DA718);
    if (!IsReadableMemory(renderer, 0x10)) {
        if (split_setup_mode) {
            g_splitscreen_vertical_layout = false;
            SaveSplitscreenLayoutState(false);
            Log("SplitscreenOrientation: renderer dimensions unavailable; reverting to Horizontal");
        }
        return;
    }

    const DWORD device_width = *reinterpret_cast<DWORD*>(renderer + 0x08);
    const DWORD device_height = *reinterpret_cast<DWORD*>(renderer + 0x0C);
    if (device_width < 2 || device_height == 0) {
        if (split_setup_mode) {
            g_splitscreen_vertical_layout = false;
            SaveSplitscreenLayoutState(false);
            Log("SplitscreenOrientation: invalid device dimensions %lux%lu; reverting to Horizontal",
                static_cast<unsigned long>(device_width),
                static_cast<unsigned long>(device_height));
        }
        return;
    }

    auto* layout = static_cast<std::uint8_t*>(layout_object);
    const DWORD left_width = device_width / 2;
    const DWORD right_width = device_width - left_width;
    auto* left = layout + 0x34;
    auto* right = left + 0x18;

    // The stock builder early-outs when the requested count has not changed.
    // Re-derive the latch from the live records so a later no-op call after the
    // setup mode field clears cannot disable projection and HUD corrections.
    if (IsLiveVerticalSplitscreenVisualState()) {
        g_splitscreen_vertical_render_active = true;
        return;
    }
    if (!split_setup_mode) {
        return;
    }
    if (!IsWritableMemory(layout_object, 0x64)) {
        g_splitscreen_vertical_layout = false;
        SaveSplitscreenLayoutState(false);
        Log("SplitscreenOrientation: viewport layout object is not writable; reverting to Horizontal");
        return;
    }

    *reinterpret_cast<DWORD*>(layout + 0x30) = 2;
    *reinterpret_cast<DWORD*>(left + 0x00) = 0;
    *reinterpret_cast<DWORD*>(left + 0x04) = 0;
    *reinterpret_cast<DWORD*>(left + 0x08) = left_width;
    *reinterpret_cast<DWORD*>(left + 0x0C) = device_height;
    *reinterpret_cast<DWORD*>(left + 0x10) = 0;
    *reinterpret_cast<DWORD*>(left + 0x14) = 0;
    *reinterpret_cast<DWORD*>(right + 0x00) = left_width;
    *reinterpret_cast<DWORD*>(right + 0x04) = 0;
    *reinterpret_cast<DWORD*>(right + 0x08) = right_width;
    *reinterpret_cast<DWORD*>(right + 0x0C) = device_height;
    *reinterpret_cast<DWORD*>(right + 0x10) = 0;
    *reinterpret_cast<DWORD*>(right + 0x14) = 1;
    g_splitscreen_vertical_render_active = true;
}

__declspec(naked) void SplitscreenViewportLayoutCall45CF1B()
{
    __asm {
        // Replay the stock builder first so all non-viewport metadata retains
        // its normal initialization, then rewrite only the two records when
        // the requested vertical layout is live.
        push eax
        push ecx
        mov edx, 0x00470820
        call edx
        pop ecx
        pop eax

        pushfd
        pushad
        push eax
        push ecx
        call RewriteSplitscreenLayout
        add esp, 8
        call RefreshVerticalSplitscreenVisualState
        popad
        popfd
        ret
    }
}

__declspec(naked) void ProjectionSplitModeSecondaryHook4CBD5D()
{
    __asm {
        mov dword ptr [g_projection_split_mode], 0
        mov eax, esi
        sub eax, 2
        jne not_split
        mov dword ptr [g_projection_split_mode], 1
        cmp byte ptr [g_splitscreen_vertical_render_active], 0
        je horizontal_split
        fmul dword ptr [g_projection_vertical_aspect_scale]
        mov dword ptr [g_projection_split_mode], 2
        jmp not_split
    horizontal_split:
        fadd st(0), st(0)
    not_split:
        mov eax, 0x004CBD66
        jmp eax
    }
}

__declspec(noinline) void CallOriginalSplitRaceMap(
    void* map_state,
    float* position,
    const float* map_size,
    DWORD icon_size)
{
    __asm {
        push icon_size
        push map_size
        push position
        mov eax, map_state
        mov ecx, 0x004C6750
        call ecx
    }
}

bool FlushCurrentRendererBatch(std::uint8_t* renderer)
{
    void** vtable = IsReadableMemory(renderer, sizeof(void*)) ?
        *reinterpret_cast<void***>(renderer) : nullptr;
    if (!IsReadableMemory(vtable, 0x60)) {
        return false;
    }

    using FlushBatch = void(__thiscall*)(void*, DWORD);
    auto flush = reinterpret_cast<FlushBatch>(vtable[0x5C / sizeof(void*)]);
    if (!IsReadableMemory(reinterpret_cast<void*>(flush), 1)) {
        return false;
    }

    // Renderer +0x5C (stock 0x005AACD0) synchronously drains queued HUD quads;
    // its single stack argument is ignored by the stock implementation.
    flush(renderer, 0);
    return true;
}

void DrawVerticalSplitRaceMap(
    void* map_state,
    float* position,
    const float* size,
    DWORD icon_size)
{
    if (!IsLiveVerticalSplitscreenVisualState()) {
        CallOriginalSplitRaceMap(map_state, position, size, icon_size);
        return;
    }

    auto* renderer = *reinterpret_cast<std::uint8_t**>(0x008DA718);
    auto* display = *reinterpret_cast<std::uint8_t**>(0x008DA7A0);
    auto* device = *reinterpret_cast<IDirect3DDevice9**>(0x008DA788);
    void** device_vtable = IsReadableMemory(device, sizeof(void*)) ?
        *reinterpret_cast<void***>(device) : nullptr;
    if (!IsReadableMemory(renderer, 0x10) ||
        !IsReadableMemory(display, 0x0C) ||
        !IsReadableMemory(device_vtable, 0x134) ||
        !IsWritableMemory(position, sizeof(float) * 2) ||
        !IsReadableMemory(size, sizeof(float) * 2)) {
        CallOriginalSplitRaceMap(map_state, position, size, icon_size);
        return;
    }

    const auto* x_scale_instruction =
        reinterpret_cast<const std::uint8_t*>(0x004C6EEB);
    if (!IsReadableMemory(x_scale_instruction, 6) ||
        x_scale_instruction[0] != 0xD8 || x_scale_instruction[1] != 0x0D) {
        CallOriginalSplitRaceMap(map_state, position, size, icon_size);
        return;
    }

    // 0x004C6ED0 multiplies map X coordinates by renderer width and the
    // floating-point operand referenced by this instruction. Widescreen mods
    // may repoint the operand at runtime, so follow the live pointer rather
    // than assuming the stock 0x0067DBE4 address.
    const auto* x_normalization_address =
        *reinterpret_cast<const float* const*>(0x004C6EED);
    if (!IsReadableMemory(x_normalization_address, sizeof(float))) {
        CallOriginalSplitRaceMap(map_state, position, size, icon_size);
        return;
    }

    const DWORD full_width = *reinterpret_cast<DWORD*>(display + 0x04);
    const DWORD full_height = *reinterpret_cast<DWORD*>(display + 0x08);
    const DWORD map_renderer_width = *reinterpret_cast<DWORD*>(renderer + 0x08);
    const float x_normalization = *x_normalization_address;
    const float map_width = size[0];
    if (full_width == 0 || full_height == 0 ||
        map_renderer_width == 0 ||
        full_width > static_cast<DWORD>(LONG_MAX) ||
        full_height > static_cast<DWORD>(LONG_MAX) ||
        !std::isfinite(x_normalization) || x_normalization <= 0.0f ||
        !std::isfinite(map_width) || map_width <= 0.0f) {
        CallOriginalSplitRaceMap(map_state, position, size, icon_size);
        return;
    }

    const float x_scale = static_cast<float>(map_renderer_width) * x_normalization;
    const float centered_x =
        static_cast<float>(full_width) / (2.0f * x_scale) - map_width * 0.5f;
    if (!std::isfinite(x_scale) || x_scale <= 0.0f ||
        !std::isfinite(centered_x) || centered_x < 0.0f) {
        CallOriginalSplitRaceMap(map_state, position, size, icon_size);
        return;
    }

    D3DVIEWPORT9 saved_viewport = {};
    RECT saved_scissor = {};
    DWORD scissor_enabled = FALSE;
    if (FAILED(device->GetViewport(&saved_viewport)) ||
        FAILED(device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor_enabled)) ||
        (scissor_enabled && FAILED(device->GetScissorRect(&saved_scissor)))) {
        CallOriginalSplitRaceMap(map_state, position, size, icon_size);
        return;
    }

    if (!FlushCurrentRendererBatch(renderer)) {
        CallOriginalSplitRaceMap(map_state, position, size, icon_size);
        return;
    }

    const D3DVIEWPORT9 full_viewport = {
        0,
        0,
        full_width,
        full_height,
        saved_viewport.MinZ,
        saved_viewport.MaxZ
    };
    const RECT full_scissor = {
        0,
        0,
        static_cast<LONG>(full_width),
        static_cast<LONG>(full_height)
    };

    if (FAILED(device->SetViewport(&full_viewport))) {
        CallOriginalSplitRaceMap(map_state, position, size, icon_size);
        return;
    }
    if (scissor_enabled && FAILED(device->SetScissorRect(&full_scissor))) {
        device->SetViewport(&saved_viewport);
        CallOriginalSplitRaceMap(map_state, position, size, icon_size);
        return;
    }

    // Mutate only after the full-device state is active. 0x004C6750 reuses this
    // position for the background and every marker, so they remain aligned.
    position[0] = centered_x;
    CallOriginalSplitRaceMap(map_state, position, size, icon_size);

    // Flush queued map markers while the full-device state is still active, then
    // restore the caller's P1 viewport/scissor for subsequent HUD rendering.
    FlushCurrentRendererBatch(renderer);
    if (scissor_enabled) {
        device->SetScissorRect(&saved_scissor);
    }
    device->SetViewport(&saved_viewport);
}

__declspec(naked) void VerticalSplitRaceMapHook4C1889()
{
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        push dword ptr [ebp + 0x10]
        push dword ptr [ebp + 0x0C]
        push dword ptr [ebp + 0x08]
        push eax
        call DrawVerticalSplitRaceMap
        add esp, 0x10
        pop edi
        pop esi
        pop ebx
        mov esp, ebp
        pop ebp
        ret 0x0C
    }
}

bool BeginVerticalHudPass()
{
    g_splitscreen_vertical_hud_pass_active = IsLiveVerticalSplitscreenVisualState();
    g_splitscreen_grid_hud_pass_active = IsLiveGridSplitscreenState();
    return g_splitscreen_vertical_hud_pass_active;
}
__declspec(naked) void SplitscreenHudBackgroundHook4B8CA0()
{
    __asm {
        pushfd
        pushad
        call BeginVerticalHudPass
        test al, al
        jz normal
        popad
        popfd
        // The two stock BGBar draws are intentionally skipped in vertical
        // mode. They are batched before POSITION text but overlap it visually
        // when both viewport HUD passes share the top anchor.
        push 0x004B8E7B
        ret
    normal:
        popad
        popfd
        mov eax, dword ptr [esp + 0x4C8]
        push 0x004B8CA7
        ret
    }
}

__declspec(naked) void SplitscreenPositionTitleHook4B9F43()
{
    __asm {
        je normal
        cmp dword ptr [esp + 0x20], 2
        jne normal
        cmp byte ptr [g_splitscreen_vertical_hud_pass_active], 0
        jne normal
        cmp byte ptr [g_splitscreen_grid_hud_pass_active], 0
        jne normal
        push 0x004B9F4C
        ret
    normal:
        push 0x004B9F6A
        ret
    }
}

__declspec(naked) void SplitscreenPositionValueHook4BA08F()
{
    __asm {
        je normal
        cmp dword ptr [esp + 0x20], 2
        jne normal
        cmp byte ptr [g_splitscreen_vertical_hud_pass_active], 0
        jne normal
        cmp byte ptr [g_splitscreen_grid_hud_pass_active], 0
        jne normal
        push 0x004BA098
        ret
    normal:
        push 0x004BA0B6
        ret
    }
}

__declspec(naked) void SplitscreenCameraConfigHook4D6BCC()
{
    __asm {
        cmp eax, 2
        jl stock_camera
        push 0x004D6BD1
        ret
    stock_camera:
        push 0x004D6BDD
        ret
    }
}

bool IsSplitscreenReadyInputState()
{
    if (!g_splitscreen_fix || !g_splitscreen_zoom_input_fix) {
        return false;
    }

    auto* game_flow = *reinterpret_cast<std::uint8_t**>(0x008E8410);
    return IsReadableMemory(game_flow, 0x920) &&
        *reinterpret_cast<DWORD*>(game_flow + 0x464) == 10 &&
        *reinterpret_cast<DWORD*>(game_flow + 0x91C) == 6;
}

void ClearZoomInputLatches()
{
    AcquireSRWLockExclusive(&g_zoom_input_latch_lock);
    g_zoom_input_latch_head = 0;
    g_zoom_input_latch_count = 0;
    ReleaseSRWLockExclusive(&g_zoom_input_latch_lock);
}

void EnqueueZoomInputLatch(DWORD slot, DWORD ordinal, DWORD tick)
{
    AcquireSRWLockExclusive(&g_zoom_input_latch_lock);
    if (g_zoom_input_latch_count == kZoomInputLatchCapacity) {
        g_zoom_input_latch_head = (g_zoom_input_latch_head + 1) % kZoomInputLatchCapacity;
        --g_zoom_input_latch_count;
    }
    const size_t tail = (g_zoom_input_latch_head + g_zoom_input_latch_count) % kZoomInputLatchCapacity;
    g_zoom_input_latches[tail] = {slot, ordinal, tick};
    ++g_zoom_input_latch_count;
    ReleaseSRWLockExclusive(&g_zoom_input_latch_lock);
}

bool ConsumeFreshZoomInputLatch(DWORD* slot, DWORD* ordinal)
{
    if (slot == nullptr || ordinal == nullptr) {
        return false;
    }

    const DWORD now = GetTickCount();
    bool found = false;
    AcquireSRWLockExclusive(&g_zoom_input_latch_lock);
    while (g_zoom_input_latch_count != 0) {
        const ZoomInputLatch item = g_zoom_input_latches[g_zoom_input_latch_head];
        g_zoom_input_latch_head = (g_zoom_input_latch_head + 1) % kZoomInputLatchCapacity;
        --g_zoom_input_latch_count;
        if (now - item.tick <= kZoomInputLatchLifetimeMs) {
            *slot = item.slot;
            *ordinal = item.ordinal;
            found = true;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_zoom_input_latch_lock);
    return found;
}

bool ResolveZoomControllerSlot(const void* captured_esi, DWORD* slot)
{
    if (captured_esi == nullptr || slot == nullptr || g_zoom_module_base == nullptr) {
        return false;
    }

    auto** begin_pointer = reinterpret_cast<std::uint8_t**>(g_zoom_module_base + 0xFECFC);
    auto** end_pointer = reinterpret_cast<std::uint8_t**>(g_zoom_module_base + 0xFED00);
    if (!IsReadableMemory(begin_pointer, sizeof(*begin_pointer)) ||
        !IsReadableMemory(end_pointer, sizeof(*end_pointer))) {
        return false;
    }

    auto* begin = *begin_pointer;
    auto* end = *end_pointer;
    const auto begin_address = reinterpret_cast<std::uintptr_t>(begin);
    const auto end_address = reinterpret_cast<std::uintptr_t>(end);
    const auto edge_address = reinterpret_cast<std::uintptr_t>(captured_esi);
    constexpr size_t kRecordStride = 0x3C;
    if (begin == nullptr || end == nullptr || end_address < begin_address ||
        (end_address - begin_address) % kRecordStride != 0 ||
        (end_address - begin_address) / kRecordStride > 16) {
        return false;
    }

    for (auto* record = begin; record < end; record += kRecordStride) {
        const auto record_address = reinterpret_cast<std::uintptr_t>(record);
        if (edge_address < record_address + 0x0C || edge_address > record_address + 0x17) {
            continue;
        }
        if (!IsReadableMemory(record, kRecordStride)) {
            return false;
        }

        const DWORD resolved_slot = *reinterpret_cast<DWORD*>(record + 0x34);
        auto* input_manager = *reinterpret_cast<std::uint8_t**>(0x008E844C);
        if (!IsReadableMemory(input_manager, 0x20)) {
            return false;
        }
        const DWORD device_count = *reinterpret_cast<DWORD*>(input_manager + 0x04);
        if (resolved_slot == 0 || resolved_slot >= device_count) {
            return false;
        }
        *slot = resolved_slot;
        return true;
    }
    return false;
}

bool GetValidatedSplitscreenLocalPlayerCount(std::uint8_t* game_flow, DWORD* player_count)
{
    if (player_count == nullptr || !IsReadableMemory(game_flow, 0x9C0)) {
        return false;
    }

    DWORD count = 0;
    bool found_non_local = false;
    for (DWORD i = 0; i < 8; ++i) {
        const DWORD type = *reinterpret_cast<DWORD*>(game_flow + 0x620 + i * 0x44);
        if (type == 1) {
            if (found_non_local) {
                return false;
            }
            ++count;
        } else {
            found_non_local = true;
        }
    }
    if (count < 2 || count > 4 ||
        (count == 3 && !g_splitscreen_three_player_capable) ||
        (count == 4 && !g_splitscreen_four_player_capable)) {
        return false;
    }

    auto* layout = *reinterpret_cast<std::uint8_t**>(game_flow + 0x9B8);
    const size_t layout_size = 0x34 + static_cast<size_t>(count) * 0x18;
    if (!IsReadableMemory(layout, layout_size) ||
        *reinterpret_cast<DWORD*>(layout + 0x30) != count) {
        return false;
    }
    *player_count = count;
    return true;
}

bool GetCurrentSplitscreenReadySelection(DWORD* ordinal, DWORD* desired_controller)
{
    if (ordinal == nullptr || desired_controller == nullptr || !IsSplitscreenReadyInputState()) {
        return false;
    }
    auto* game_flow = *reinterpret_cast<std::uint8_t**>(0x008E8410);
    auto* input_manager = *reinterpret_cast<std::uint8_t**>(0x008E844C);
    if (!IsReadableMemory(game_flow, 0x9D0) || !IsReadableMemory(input_manager, 0x20)) {
        return false;
    }

    DWORD active_players = 0;
    if (!GetValidatedSplitscreenLocalPlayerCount(game_flow, &active_players)) {
        return false;
    }

    const DWORD current_ordinal = *reinterpret_cast<DWORD*>(game_flow + 0x9CC);
    const DWORD device_count = *reinterpret_cast<DWORD*>(input_manager + 0x04);
    if (current_ordinal >= active_players) {
        return false;
    }
    const DWORD current_desired = *reinterpret_cast<DWORD*>(
        game_flow + 0x624 + current_ordinal * 0x44
    );
    if (current_desired >= device_count) {
        return false;
    }
    *ordinal = current_ordinal;
    *desired_controller = current_desired;
    return true;
}

UINT __cdecl HandleZoomSendInput(
    UINT input_count,
    LPINPUT inputs,
    int input_size,
    const void* captured_esi,
    const void* return_address
)
{
    if (g_real_zoom_send_input == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return 0;
    }

    if (!IsSplitscreenReadyInputState()) {
        ClearZoomInputLatches();
        return g_real_zoom_send_input(input_count, inputs, input_size);
    }
    if (input_count == 0 || inputs == nullptr || input_size != sizeof(INPUT) ||
        input_count > (static_cast<UINT>(~static_cast<size_t>(0)) / sizeof(INPUT)) ||
        !IsReadableMemory(inputs, static_cast<size_t>(input_count) * sizeof(INPUT))) {
        return g_real_zoom_send_input(input_count, inputs, input_size);
    }

    DWORD resolved_slot = 0xFFFFFFFF;
    DWORD target_ordinal = 0xFFFFFFFF;
    DWORD desired_controller = 0xFFFFFFFF;
    const bool ready_selection_valid = GetCurrentSplitscreenReadySelection(
        &target_ordinal,
        &desired_controller
    );
    const auto down_return = g_zoom_module_base + 0x4711;
    const auto up_return = g_zoom_module_base + 0x4799;
    const bool supported_down_call = return_address == down_return;
    const bool supported_up_call = return_address == up_return;
    UINT reported_inserted = 0;
    for (UINT i = 0; i < input_count; ++i) {
        const INPUT& input = inputs[i];
        const bool return_key = input.type == INPUT_KEYBOARD && input.ki.wVk == VK_RETURN;
        const bool return_key_up = return_key && (input.ki.dwFlags & KEYEVENTF_KEYUP) != 0;
        if (!return_key || return_key_up) {
            // A stray key-up cannot claim a ready prompt. Always forward it
            // (including the verified +0x4799 call) so a valid injected
            // key-down cannot become stuck if controller state changes.
            reported_inserted += g_real_zoom_send_input(1, &inputs[i], input_size);
            continue;
        }

        const bool identity_valid = supported_down_call &&
            !supported_up_call &&
            ResolveZoomControllerSlot(captured_esi, &resolved_slot) &&
            ready_selection_valid &&
            resolved_slot == desired_controller;
        if (!identity_valid) {
            // Fail closed only for an unowned Zoom-generated Return key-down.
            // Reporting it as inserted prevents retries while ensuring an
            // unknown or wrong pad cannot masquerade as keyboard slot zero.
            ++reported_inserted;
            continue;
        }

        EnqueueZoomInputLatch(resolved_slot, target_ordinal, GetTickCount());
        reported_inserted += g_real_zoom_send_input(1, &inputs[i], input_size);
    }
    return reported_inserted;
}

__declspec(naked) UINT WINAPI ProxyZoomSendInput(UINT, LPINPUT, int)
{
    __asm {
        // Zoom's current x86 key-edge helper keeps the exact record edge
        // pointer in ESI across its SendInput IAT call. Capture it and the IAT
        // return address before a C prologue can reuse them, then preserve
        // SendInput's stdcall ABI.
        mov eax, esp
        push dword ptr [eax]
        push esi
        push dword ptr [eax + 0x0C]
        push dword ptr [eax + 0x08]
        push dword ptr [eax + 0x04]
        call HandleZoomSendInput
        add esp, 0x14
        ret 0x0C
    }
}

bool __cdecl ShouldAcceptSplitscreenReadyOwner(void* event_object, void*)
{
    if (!IsSplitscreenMode()) {
        ClearZoomInputLatches();
        return true;
    }

    auto* event_bytes = static_cast<std::uint8_t*>(event_object);
    auto* game_flow = *reinterpret_cast<std::uint8_t**>(0x008E8410);
    auto* input_manager = *reinterpret_cast<std::uint8_t**>(0x008E844C);
    if (!IsReadableMemory(event_bytes, 0x14) ||
        !IsReadableMemory(game_flow, 0x9D0) ||
        !IsReadableMemory(input_manager, 0x20)) {
        return false;
    }

    DWORD active_players = 0;
    if (!GetValidatedSplitscreenLocalPlayerCount(game_flow, &active_players)) {
        return false;
    }

    DWORD source_controller = *reinterpret_cast<DWORD*>(event_bytes + 0x10);
    const DWORD target_ordinal = *reinterpret_cast<DWORD*>(game_flow + 0x9CC);
    const DWORD device_count = *reinterpret_cast<DWORD*>(input_manager + 0x04);
    if (target_ordinal >= active_players || source_controller >= device_count) {
        return false;
    }

    // GameFlow.PlayerInfo[i] has a 0x44 stride. Controller is the zero-based
    // DWORD at record +0x10, or game-flow +0x624.
    const DWORD desired_controller = *reinterpret_cast<DWORD*>(
        game_flow + 0x624 + target_ordinal * 0x44
    );
    if (desired_controller >= device_count) {
        return false;
    }

    // Zoom translates pad A/Start into a synthetic keyboard Return, so the
    // stock event arrives as source zero. A fresh version-gated latch restores
    // the originating pad before the stock duplicate-source check.
    DWORD zoom_controller = 0xFFFFFFFF;
    DWORD zoom_ordinal = 0xFFFFFFFF;
    if (source_controller == 0 &&
        ConsumeFreshZoomInputLatch(&zoom_controller, &zoom_ordinal)) {
        if (zoom_controller != desired_controller || zoom_ordinal != target_ordinal) {
            return false;
        }
        *reinterpret_cast<DWORD*>(event_bytes + 0x10) = zoom_controller;
        source_controller = zoom_controller;
    }
    return source_controller == desired_controller;
}

using RendererSetViewportFn = void(__thiscall*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD);
using RendererPostProcessFn = void(__thiscall*)(void*);

void RunSplitscreenPostProcessing(void* renderer, void** renderer_vtable, DWORD viewport_count)
{
    if (!IsReadableMemory(renderer, 0x10) || !IsReadableMemory(renderer_vtable, 0x154)) {
        return;
    }

    const auto post_process = reinterpret_cast<RendererPostProcessFn>(renderer_vtable[0x150 / sizeof(void*)]);
    if (!g_splitscreen_post_processing_fix || !IsSplitscreenMode() ||
        viewport_count < 2 || viewport_count > 4) {
        post_process(renderer);
        return;
    }

    auto* layout = *reinterpret_cast<std::uint8_t**>(0x00696DC8);
    const size_t layout_size = 0x34 + static_cast<size_t>(viewport_count) * 0x18;
    if (!IsReadableMemory(layout, layout_size)) {
        post_process(renderer);
        return;
    }

    const auto set_viewport =
        reinterpret_cast<RendererSetViewportFn>(renderer_vtable[0x30 / sizeof(void*)]);
    const DWORD device_width = *reinterpret_cast<DWORD*>(static_cast<std::uint8_t*>(renderer) + 0x08);
    const DWORD device_height = *reinterpret_cast<DWORD*>(static_cast<std::uint8_t*>(renderer) + 0x0C);
    auto* last_viewport = layout + 0x34 + static_cast<size_t>(viewport_count - 1) * 0x18;
    const DWORD last_x = *reinterpret_cast<DWORD*>(last_viewport + 0x00);
    const DWORD last_y = *reinterpret_cast<DWORD*>(last_viewport + 0x04);
    const DWORD last_width = *reinterpret_cast<DWORD*>(last_viewport + 0x08);
    const DWORD last_height = *reinterpret_cast<DWORD*>(last_viewport + 0x0C);
    const DWORD last_ordinal = viewport_count - 1;
    if (device_width == 0 || device_height == 0 || last_width == 0 || last_height == 0 ||
        last_x > device_width || last_y > device_height ||
        last_width > device_width - last_x || last_height > device_height - last_y) {
        post_process(renderer);
        return;
    }

    // The stock site runs one full-device post-process call while the last
    // player's viewport is still selected. Snapshot the last layout record,
    // temporarily select the composed device surface, process it once, then
    // restore that record for either horizontal or vertical geometry.
    set_viewport(renderer, 0, 0, device_width, device_height, 0);
    post_process(renderer);
    set_viewport(renderer, last_x, last_y, last_width, last_height, last_ordinal);
}

__declspec(naked) void SplitscreenPostProcessingHook4CBB26()
{
    __asm {
        // Original stack slots before saving context: viewport count at
        // +0x24 and the renderer vtable at +0x50. ESI is the renderer.
        mov eax, dword ptr [esp + 0x24]
        mov edx, dword ptr [esp + 0x50]
        pushfd
        pushad
        push eax
        push edx
        push esi
        call RunSplitscreenPostProcessing
        add esp, 0x0C
        popad
        popfd
        push 0x004CBB32
        ret
    }
}

__declspec(naked) void SplitscreenPlayerCountHook54FF2E()
{
    __asm {
        // This function runs once during input-manager startup, before a
        // game-flow mode exists. Preserve stock EDI=1 for the mutable current
        // player/action context at +0x14. Manager +0x08 is only a proven
        // local-multiplayer threshold/state: value 2 enables the split ready
        // and UI branches, while PlayerInfo records define participant count.
        mov dword ptr [ebp + 0x14], edi
        mov dword ptr [ebp + 0x08], 2
        push 0x0054FF34
        ret
    }
}

__declspec(naked) void SplitscreenReadyOwnerHook45BC5E()
{
    __asm {
        pushfd
        pushad
        push esi
        push ebx
        call ShouldAcceptSplitscreenReadyOwner
        add esp, 8
        test al, al
        jz rejected

        popad
        popfd
        // Replay the stock source load and game-flow context. The following
        // 0x0045F960 call still performs the stock duplicate-source check.
        mov edx, dword ptr [ebx + 0x10]
        mov eax, esi
        push 0x0045BC63
        ret

    rejected:
        popad
        popfd
        push 0x0045C2E7
        ret
    }
}

__declspec(naked) void SplitscreenHeldRoutingHook55D557()
{
    __asm {
        pushfd
        pushad
        call IsSplitscreenMode
        test al, al
        jz stock_path
        popad
        popfd
        push 0x0055D574
        ret

    stock_path:
        popad
        popfd
        cmp dword ptr [edx + 4], 0
        jne stock_forward
        push 0x0055D55D
        ret

    stock_forward:
        push 0x0055D561
        ret
    }
}

__declspec(naked) void SplitscreenPressedRoutingHook55D627()
{
    __asm {
        pushfd
        pushad
        call IsSplitscreenMode
        test al, al
        jz stock_path
        popad
        popfd
        push 0x0055D644
        ret

    stock_path:
        popad
        popfd
        cmp dword ptr [edx + 4], 0
        jne stock_forward
        push 0x0055D62D
        ret

    stock_forward:
        push 0x0055D631
        ret
    }
}

__declspec(naked) void SplitscreenPressedControllerHook55D705()
{
    __asm {
        pushfd
        pushad
        call IsSplitscreenMode
        test al, al
        jz stock_path
        popad
        popfd
        push 0x0055D740
        ret

    stock_path:
        popad
        popfd
        cmp al, 0xFF
        je stock_no_controller
        push ebp
        push 0x0055D70A
        ret

    stock_no_controller:
        push 0x0055D740
        ret
    }
}

__declspec(naked) void SplitscreenHeldControllerHook55D785()
{
    __asm {
        pushfd
        pushad
        call IsSplitscreenMode
        test al, al
        jz stock_path
        popad
        popfd
        push 0x0055D7BF
        ret

    stock_path:
        popad
        popfd
        cmp al, 0xFF
        je stock_no_controller
        push ebp
        push 0x0055D78A
        ret

    stock_no_controller:
        push 0x0055D7BF
        ret
    }
}

HWND GetPresentationWindow(D3DPRESENT_PARAMETERS* presentation_parameters, HWND fallback)
{
    if (presentation_parameters != nullptr && presentation_parameters->hDeviceWindow != nullptr) {
        return presentation_parameters->hDeviceWindow;
    }
    if (fallback != nullptr) {
        return fallback;
    }
    return GetActiveWindow();
}

void ApplyBorderlessWindow(HWND window, D3DPRESENT_PARAMETERS* presentation_parameters)
{
    if (!g_borderless_windowed || window == nullptr || presentation_parameters == nullptr) {
        return;
    }

    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info = {};
    monitor_info.cbSize = sizeof(monitor_info);
    if (monitor == nullptr || !GetMonitorInfoA(monitor, &monitor_info)) {
        monitor_info.rcMonitor.left = 0;
        monitor_info.rcMonitor.top = 0;
        monitor_info.rcMonitor.right = GetSystemMetrics(SM_CXSCREEN);
        monitor_info.rcMonitor.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    const RECT rect = monitor_info.rcMonitor;
    const UINT width = static_cast<UINT>(rect.right - rect.left);
    const UINT height = static_cast<UINT>(rect.bottom - rect.top);

    presentation_parameters->Windowed = TRUE;
    presentation_parameters->FullScreen_RefreshRateInHz = 0;
    presentation_parameters->BackBufferWidth = width;
    presentation_parameters->BackBufferHeight = height;
    if (presentation_parameters->hDeviceWindow == nullptr) {
        presentation_parameters->hDeviceWindow = window;
    }

    SetWindowLongA(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowLongA(window, GWL_EXSTYLE, 0);
    SetWindowPos(
        window,
        HWND_TOP,
        rect.left,
        rect.top,
        static_cast<int>(width),
        static_cast<int>(height),
        SWP_FRAMECHANGED | SWP_SHOWWINDOW
    );

    Log("BorderlessWindowed: applied %ux%u at (%ld,%ld)", width, height, rect.left, rect.top);
}

HRESULT STDMETHODCALLTYPE ProxyReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* presentation_parameters)
{
    if (g_borderless_windowed && presentation_parameters != nullptr) {
        ApplyBorderlessWindow(GetPresentationWindow(presentation_parameters, nullptr), presentation_parameters);
    }

    if (g_remove_vsync && presentation_parameters != nullptr) {
        const UINT old_interval = presentation_parameters->PresentationInterval;
        presentation_parameters->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        Log("RemoveVSync: Reset PresentationInterval %u -> %u", old_interval, presentation_parameters->PresentationInterval);
    }

    return g_real_reset(device, presentation_parameters);
}

void HookDeviceMethods(IDirect3DDevice9* device)
{
    if (device == nullptr || InterlockedCompareExchange(&g_device_hooks_installed, 1, 0) != 0) {
        return;
    }

    auto*** object = reinterpret_cast<void***>(device);
    void** vtable = *object;

    if (g_remove_vsync || g_borderless_windowed) {
        void* original_reset = vtable[16];
        void* reset_replacement = reinterpret_cast<void*>(&ProxyReset);
        g_real_reset = reinterpret_cast<ResetFn>(original_reset);

        if (!WriteMemory(&vtable[16], &reset_replacement, sizeof(reset_replacement))) {
            Log("D3D patch: failed to hook IDirect3DDevice9::Reset");
            g_real_reset = nullptr;
            InterlockedExchange(&g_device_hooks_installed, 0);
            return;
        }

        Log("D3D patch: hooked IDirect3DDevice9::Reset, original=0x%p", original_reset);
    }

}

class Direct3D9Proxy final : public IDirect3D9
{
public:
    explicit Direct3D9Proxy(IDirect3D9* real) : m_real(real) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override
    {
        if (ppvObj == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IDirect3D9) {
            *ppvObj = static_cast<IDirect3D9*>(this);
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, ppvObj);
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        m_real->AddRef();
        return InterlockedIncrement(&m_refs);
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        m_real->Release();
        const ULONG refs = InterlockedDecrement(&m_refs);
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* pInitializeFunction) override { return m_real->RegisterSoftwareDevice(pInitializeFunction); }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return m_real->GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) override { return m_real->GetAdapterIdentifier(Adapter, Flags, pIdentifier); }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override { return m_real->GetAdapterModeCount(Adapter, Format); }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override { return m_real->EnumAdapterModes(Adapter, Format, Mode, pMode); }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) override { return m_real->GetAdapterDisplayMode(Adapter, pMode); }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed) override { return m_real->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override { return m_real->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat); }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels) override { return m_real->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels); }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override { return m_real->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) override { return m_real->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override { return m_real->GetDeviceCaps(Adapter, DeviceType, pCaps); }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override { return m_real->GetAdapterMonitor(Adapter); }

    HRESULT STDMETHODCALLTYPE CreateDevice(
        UINT Adapter,
        D3DDEVTYPE DeviceType,
        HWND hFocusWindow,
        DWORD BehaviorFlags,
        D3DPRESENT_PARAMETERS* pPresentationParameters,
        IDirect3DDevice9** ppReturnedDeviceInterface
    ) override
    {
        if (g_borderless_windowed && pPresentationParameters != nullptr) {
            ApplyBorderlessWindow(GetPresentationWindow(pPresentationParameters, hFocusWindow), pPresentationParameters);
        }

        if (g_remove_vsync && pPresentationParameters != nullptr) {
            const UINT old_interval = pPresentationParameters->PresentationInterval;
            pPresentationParameters->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
            Log("RemoveVSync: CreateDevice PresentationInterval %u -> %u", old_interval, pPresentationParameters->PresentationInterval);
        }
        const HRESULT result = m_real->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
        if (SUCCEEDED(result) && ppReturnedDeviceInterface != nullptr) {
            HookDeviceMethods(*ppReturnedDeviceInterface);
        }
        return result;
    }

private:
    IDirect3D9* m_real = nullptr;
    volatile LONG m_refs = 1;
};

IDirect3D9* WINAPI ProxyDirect3DCreate9(UINT sdk_version)
{
    if (g_real_direct3d_create9 == nullptr) {
        return nullptr;
    }

    IDirect3D9* real = g_real_direct3d_create9(sdk_version);
    if (real == nullptr || (!g_remove_vsync && !g_borderless_windowed)) {
        return real;
    }

    Log("D3D patch: wrapping IDirect3D9 from Direct3DCreate9(%u)", sdk_version);
    return new Direct3D9Proxy(real);
}

ModuleRange GetModuleRange(HMODULE module)
{
    auto* base = reinterpret_cast<std::uint8_t*>(module);
    if (!IsReadableMemory(base, sizeof(IMAGE_DOS_HEADER))) {
        return ModuleRange{};
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) {
        return ModuleRange{};
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (!IsReadableMemory(nt, sizeof(IMAGE_NT_HEADERS)) ||
        nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->OptionalHeader.SizeOfImage < sizeof(IMAGE_DOS_HEADER)) {
        return ModuleRange{};
    }

    return ModuleRange{base, nt->OptionalHeader.SizeOfImage};
}

ModuleRange GetExeRange()
{
    return GetModuleRange(GetModuleHandleA(nullptr));
}

std::uint8_t* FindBytes(const ModuleRange& range, const std::uint8_t* bytes, size_t size)
{
    if (range.base == nullptr || range.size < size || size == 0) {
        return nullptr;
    }

    for (DWORD i = 0; i <= range.size - size; ++i) {
        if (std::memcmp(range.base + i, bytes, size) == 0) {
            return range.base + i;
        }
    }
    return nullptr;
}

bool PatchSkipLicenseScreen()
{
    // Preserve the legal-screen render/initialization path, but make its
    // texture transparent and reduce the mandatory 5000 ms wait to zero.
    auto* opacity = reinterpret_cast<std::uint8_t*>(0x005213EA);
    const std::uint8_t expected_opacity[] = {0x00, 0x00, 0x80, 0x3F}; // 1.0f
    const std::uint8_t zero_opacity[] = {0x00, 0x00, 0x00, 0x00};

    auto* timeout = reinterpret_cast<std::uint8_t*>(0x00521457);
    const std::uint8_t expected_timeout[] = {0x88, 0x13, 0x00, 0x00}; // 5000 ms
    const std::uint8_t zero_timeout[] = {0x00, 0x00, 0x00, 0x00};

    if (std::memcmp(opacity, expected_opacity, sizeof(expected_opacity)) != 0 ||
        std::memcmp(timeout, expected_timeout, sizeof(expected_timeout)) != 0) {
        Log("SkipLicenseScreen: instruction pattern mismatch");
        return false;
    }

    if (!WriteMemory(opacity, zero_opacity, sizeof(zero_opacity)) ||
        !WriteMemory(timeout, zero_timeout, sizeof(zero_timeout))) {
        Log("SkipLicenseScreen: failed to patch legal screen");
        return false;
    }

    Log("SkipLicenseScreen: changed legal texture opacity and timeout to zero at 0x%p and 0x%p", opacity, timeout);
    return true;
}

bool PatchSkipIntro(const ModuleRange& range)
{
    // Current Steam exe startup video routine starts with this prologue and
    // references data/video/nvidia.avi, empire.avi, bugbear.avi and intro%c%c%c.avi.
    const std::uint8_t pattern[] = {
        0x81, 0xEC, 0x04, 0x01, 0x00, 0x00,
        0x8B, 0x15, 0x3C, 0xC1, 0x69, 0x00,
        0x53,
        0x56,
        0x83, 0xFA, 0x02,
        0x0F, 0x95, 0xC3,
        0x33, 0xF6,
        0x56,
        0x68, 0x84, 0x7E, 0x67, 0x00
    };

    std::uint8_t* address = FindBytes(range, pattern, sizeof(pattern));
    if (address == nullptr) {
        Log("SkipIntro: startup video routine pattern not found");
        return false;
    }

    const std::uint8_t patch[] = {0xC3};
    if (!WriteMemory(address, patch, sizeof(patch))) {
        Log("SkipIntro: failed to patch 0x%p", address);
        return false;
    }

    Log("SkipIntro: patched startup video routine at 0x%p", address);
    return true;
}

bool PatchImportByName(const ModuleRange& range, const char* dll_name, const char* import_name, void* replacement, void** original)
{
    if (range.base == nullptr) {
        return false;
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(range.base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(range.base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports_dir.VirtualAddress == 0 || imports_dir.Size == 0) {
        return false;
    }

    auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(range.base + imports_dir.VirtualAddress);
    for (; imports->Name != 0; ++imports) {
        const char* current_dll = reinterpret_cast<const char*>(range.base + imports->Name);
        if (_stricmp(current_dll, dll_name) != 0) {
            continue;
        }

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(range.base + imports->FirstThunk);
        auto* original_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(range.base + imports->OriginalFirstThunk);
        for (; original_thunk->u1.AddressOfData != 0; ++thunk, ++original_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(original_thunk->u1.Ordinal)) {
                continue;
            }

            auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(range.base + original_thunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), import_name) != 0) {
                continue;
            }

            if (original != nullptr) {
                *original = reinterpret_cast<void*>(thunk->u1.Function);
            }

            void* patch_address = &thunk->u1.Function;
            return WriteMemory(patch_address, &replacement, sizeof(replacement));
        }
    }

    return false;
}

bool PatchRemoveVSync(const ModuleRange& range)
{
    void* original = nullptr;
    if (!PatchImportByName(range, "d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&ProxyDirect3DCreate9), &original)) {
        Log("RemoveVSync: failed to patch Direct3DCreate9 import");
        return false;
    }

    g_real_direct3d_create9 = reinterpret_cast<Direct3DCreate9Fn>(original);
    Log("RemoveVSync: patched Direct3DCreate9 import, original=0x%p", original);
    return true;
}

void ApplyTimerResolution()
{
    g_winmm = LoadLibraryA("winmm.dll");
    if (g_winmm == nullptr) {
        Log("FramePacingFix: failed to load winmm.dll");
        return;
    }

    g_time_begin_period = reinterpret_cast<TimeBeginPeriodFn>(GetProcAddress(g_winmm, "timeBeginPeriod"));
    if (g_time_begin_period == nullptr) {
        Log("FramePacingFix: failed to resolve timeBeginPeriod");
        return;
    }

    const MMRESULT result = g_time_begin_period(1);
    Log("FramePacingFix: timeBeginPeriod(1) returned %u", result);
}

bool PatchFramePacing()
{
    // The stock 100 FPS limiter yields with Sleep(1) while waiting for the
    // next 10ms frame. On modern Windows that can oversleep and look like
    // 50 FPS. Remove the yield completely for the most stable 100 FPS pacing.
    std::uint8_t* sleep_call = reinterpret_cast<std::uint8_t*>(0x0045F8DC);
    const std::uint8_t expected[] = {
        0x6A, 0x01,
        0xFF, 0x15, 0xC8, 0x20, 0x65, 0x00
    };
    const std::uint8_t patch[] = {
        0x90, 0x90,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    };
    if (std::memcmp(sleep_call, expected, sizeof(expected)) == 0 && WriteMemory(sleep_call, patch, sizeof(patch))) {
        Log("FramePacingFix: removed limiter Sleep(1) call at 0x%p", sleep_call);
        return true;
    }

    Log("FramePacingFix: limiter Sleep call pattern mismatch at 0x%p", sleep_call);
    return false;
}

bool PatchUncapFPS(const ModuleRange& range)
{
    // Keep the original 100 FPS timing interval intact, but turn the frame-gate
    // branch into an unconditional "ready" result. Do not patch the interval
    // itself: lowering it speeds up simulation.
    std::uint8_t* frame_gate_branch = reinterpret_cast<std::uint8_t*>(0x0045F8CF);
    const std::uint8_t expected[] = {0x73};
    const std::uint8_t patch[] = {0xEB};
    if (std::memcmp(frame_gate_branch, expected, sizeof(expected)) == 0 && WriteMemory(frame_gate_branch, patch, sizeof(patch))) {
        Log("UncapFPS: patched frame gate branch at 0x%p", frame_gate_branch);
        return true;
    }

    Log("UncapFPS: frame gate branch pattern mismatch at 0x%p", frame_gate_branch);
    return false;
}

bool WriteFloat(std::uintptr_t address, float value)
{
    return WriteMemory(reinterpret_cast<void*>(address), &value, sizeof(value));
}

bool WriteFloatOperand(std::uintptr_t address, const float* value)
{
    const DWORD pointer = reinterpret_cast<DWORD>(value);
    return WriteMemory(reinterpret_cast<void*>(address), &pointer, sizeof(pointer));
}

void FormatBytes(const std::uint8_t* bytes, size_t size, char* output, size_t output_size)
{
    if (output_size == 0) {
        return;
    }

    output[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < size && used + 4 < output_size; ++i) {
        const int written = std::snprintf(output + used, output_size - used, "%02X%s", bytes[i], i + 1 < size ? " " : "");
        if (written <= 0) {
            break;
        }
        used += static_cast<size_t>(written);
    }
}

bool GameFileExists(const char* name)
{
    char path[MAX_PATH] = {};
    BuildGamePath(path, MAX_PATH, name);
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool GameFileHasExactBytes(const char* name, const char* expected)
{
    char path[MAX_PATH] = {};
    BuildGamePath(path, MAX_PATH, name);
    HANDLE file = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size = {};
    const size_t expected_size = std::strlen(expected);
    if (!GetFileSizeEx(file, &size) || size.QuadPart != static_cast<LONGLONG>(expected_size)) {
        CloseHandle(file);
        return false;
    }

    char contents[64] = {};
    DWORD bytes_read = 0;
    const bool ok = expected_size < sizeof(contents) &&
        ReadFile(file, contents, static_cast<DWORD>(expected_size), &bytes_read, nullptr) &&
        bytes_read == expected_size &&
        std::memcmp(contents, expected, expected_size) == 0;
    CloseHandle(file);
    return ok;
}

struct SplitscreenPatchSite
{
    std::uint8_t* address;
    const std::uint8_t* expected;
    size_t expected_size;
    size_t patch_size;
    void* hook;
    const char* label;
    bool call;
};

bool SplitscreenBranchMatches(const SplitscreenPatchSite& site)
{
    std::uint8_t expected_branch[32] = {};
    if (site.patch_size < 5 || site.patch_size > sizeof(expected_branch)) {
        return false;
    }

    expected_branch[0] = site.call ? 0xE8 : 0xE9;
    *reinterpret_cast<std::int32_t*>(&expected_branch[1]) =
        static_cast<std::int32_t>(
            reinterpret_cast<std::uint8_t*>(site.hook) - site.address - 5
        );
    for (size_t i = 5; i < site.patch_size; ++i) {
        expected_branch[i] = 0x90;
    }
    return std::memcmp(site.address, expected_branch, site.patch_size) == 0;
}

bool PatchFourPlayerInputLifecycle(const ModuleRange& range)
{
    g_splitscreen_four_player_capable = false;
    if (!g_splitscreen_three_player_capable) {
        Log("Splitscreen4P: three-player presentation capability unavailable; lifecycle hooks skipped");
        return false;
    }
    if (IsReadableMemory(reinterpret_cast<void*>(0x008E844C), sizeof(void*)) &&
        *reinterpret_cast<void**>(0x008E844C) != nullptr) {
        Log("Splitscreen4P: input manager already exists; refusing asynchronous late installation");
        return false;
    }

    const std::uint8_t backend_update_expected[] = {0x53, 0x56, 0x57, 0x8B, 0xF8};
    const std::uint8_t backend_shutdown_expected[] = {0x8B, 0x77, 0x28, 0x85, 0xF6};
    const std::uint8_t backend_clear_expected[] = {0xC7, 0x47, 0x28, 0x00, 0x00, 0x00, 0x00};
    const std::uint8_t constructor_call_expected[] = {0xE8, 0xD7, 0xED, 0x02, 0x00};
    // Infrastructure is installed first. The constructor CALL is the only
    // activation point and is deliberately installed last (and rolled back first).
    const SplitscreenPatchSite sites[] = {
        {reinterpret_cast<std::uint8_t*>(0x0055B490), backend_update_expected, sizeof(backend_update_expected), sizeof(backend_update_expected), reinterpret_cast<void*>(&SplitscreenFourPlayerBackendUpdate55B490), "four-player-backend-update", false},
        {reinterpret_cast<std::uint8_t*>(0x0055011D), backend_shutdown_expected, sizeof(backend_shutdown_expected), sizeof(backend_shutdown_expected), reinterpret_cast<void*>(&SplitscreenFourPlayerBackendShutdown55011D), "four-player-backend-shutdown", false},
        {reinterpret_cast<std::uint8_t*>(0x00550175), backend_clear_expected, sizeof(backend_clear_expected), sizeof(backend_clear_expected), reinterpret_cast<void*>(&SplitscreenFourPlayerBackendClear550175), "four-player-backend-clear", false},
        {reinterpret_cast<std::uint8_t*>(0x00521134), constructor_call_expected, sizeof(constructor_call_expected), sizeof(constructor_call_expected), reinterpret_cast<void*>(&SplitscreenFourPlayerConstructorCall521134), "four-player-post-constructor", true},
    };

    for (const SplitscreenPatchSite& site : sites) {
        if (!ModuleContains(range, site.address, site.expected_size) ||
            std::memcmp(site.address, site.expected, site.expected_size) != 0) {
            char actual[48] = {};
            if (ModuleContains(range, site.address, site.expected_size)) {
                FormatBytes(site.address, site.expected_size, actual, sizeof(actual));
            } else {
                std::snprintf(actual, sizeof(actual), "outside executable");
            }
            Log("Splitscreen4P: signature mismatch for %s at 0x%p actual=%s; maximum remains 3",
                site.label, site.address, actual);
            return false;
        }
    }

    size_t installed = 0;
    for (; installed < sizeof(sites) / sizeof(sites[0]); ++installed) {
        const SplitscreenPatchSite& site = sites[installed];
        const bool wrote = site.call
            ? WriteCall(site.address, site.hook, site.patch_size)
            : WriteJump(site.address, site.hook, site.patch_size);
        if (!wrote || !SplitscreenBranchMatches(site)) {
            Log("Splitscreen4P: failed to install/verify %s; rolling back lifecycle hooks", site.label);
            if (std::memcmp(site.address, site.expected, site.patch_size) != 0) {
                WriteMemory(site.address, site.expected, site.patch_size);
            }
            break;
        }
    }
    if (installed != sizeof(sites) / sizeof(sites[0])) {
        while (installed > 0) {
            --installed;
            const SplitscreenPatchSite& site = sites[installed];
            WriteMemory(site.address, site.expected, site.patch_size);
        }
        return false;
    }

    Log("Splitscreen4P: lifecycle hooks installed; capability will activate only if XInput user 2 is connected during native input construction");
    return true;
}

bool PatchVerticalSplitscreenLayout(const ModuleRange& range)
{
    const bool selected_vertical = g_splitscreen_vertical_layout;
    g_splitscreen_vertical_layout = false;
    g_splitscreen_vertical_capable = false;
    g_splitscreen_three_player_capable = false;
    g_splitscreen_four_player_capable = false;
    g_splitscreen_vertical_render_active = false;
    g_splitscreen_vertical_hud_pass_active = false;
    g_splitscreen_grid_hud_pass_active = false;

    const std::uint8_t layout_context_expected[] = {
        0x8B, 0x44, 0x24, 0x14,
        0x8B, 0x8D, 0xB8, 0x09, 0x00, 0x00,
        0xE8, 0x00, 0x39, 0x01, 0x00,
        0x8B, 0x0D, 0x14, 0x84, 0x8E, 0x00
    };
    if (!ModuleContains(range, reinterpret_cast<void*>(0x0045CF11), sizeof(layout_context_expected)) ||
        std::memcmp(reinterpret_cast<void*>(0x0045CF11), layout_context_expected, sizeof(layout_context_expected)) != 0) {
        Log("SplitscreenOrientation: viewport-builder caller mismatch; falling back to Horizontal");
        SaveSplitscreenLayoutState(false);
        return false;
    }

    const std::uint8_t layout_call_expected[] = {0xE8, 0x00, 0x39, 0x01, 0x00};
    const std::uint8_t primary_aspect_expected[] = {
        0x8B, 0xC3, 0x83, 0xE8, 0x02, 0x75, 0x02, 0xDC, 0xC0
    };
    const std::uint8_t secondary_aspect_expected[] = {
        0x8B, 0xC6, 0x83, 0xE8, 0x02, 0x75, 0x02, 0xDC, 0xC0
    };
    const std::uint8_t hud_background_expected[] = {
        0x8B, 0x84, 0x24, 0xC8, 0x04, 0x00, 0x00
    };
    const std::uint8_t hud_background_context_expected[] = {
        0x8B, 0x88, 0x7C, 0x03, 0x00, 0x00
    };
    const std::uint8_t position_title_expected[] = {
        0x74, 0x25, 0x83, 0x7C, 0x24, 0x20, 0x02, 0x75, 0x1E
    };
    const std::uint8_t position_value_expected[] = {
        0x74, 0x25, 0x83, 0x7C, 0x24, 0x20, 0x02, 0x75, 0x1E
    };
    const std::uint8_t split_race_map_expected[] = {0xE8, 0xC2, 0x4E, 0x00, 0x00};
    const std::uint8_t split_camera_expected[] = {0x83, 0xF8, 0x02, 0x75, 0x0C};
    if (!ModuleContains(range, reinterpret_cast<void*>(0x004B8CA7), sizeof(hud_background_context_expected)) ||
        std::memcmp(reinterpret_cast<void*>(0x004B8CA7), hud_background_context_expected,
            sizeof(hud_background_context_expected)) != 0) {
        Log("SplitscreenOrientation: HUD background-loop context mismatch; falling back to Horizontal");
        SaveSplitscreenLayoutState(false);
        return false;
    }
    const SplitscreenPatchSite sites[] = {
        {reinterpret_cast<std::uint8_t*>(0x004C9E27), primary_aspect_expected, sizeof(primary_aspect_expected), sizeof(primary_aspect_expected), reinterpret_cast<void*>(&ProjectionSplitModeHook), "primary-projection-aspect", false},
        {reinterpret_cast<std::uint8_t*>(0x004CBD5D), secondary_aspect_expected, sizeof(secondary_aspect_expected), sizeof(secondary_aspect_expected), reinterpret_cast<void*>(&ProjectionSplitModeSecondaryHook4CBD5D), "secondary-projection-aspect", false},
        {reinterpret_cast<std::uint8_t*>(0x004B8CA0), hud_background_expected, sizeof(hud_background_expected), sizeof(hud_background_expected), reinterpret_cast<void*>(&SplitscreenHudBackgroundHook4B8CA0), "vertical-position-background-loop", false},
        {reinterpret_cast<std::uint8_t*>(0x004B9F43), position_title_expected, sizeof(position_title_expected), sizeof(position_title_expected), reinterpret_cast<void*>(&SplitscreenPositionTitleHook4B9F43), "vertical-position-title", false},
        {reinterpret_cast<std::uint8_t*>(0x004BA08F), position_value_expected, sizeof(position_value_expected), sizeof(position_value_expected), reinterpret_cast<void*>(&SplitscreenPositionValueHook4BA08F), "vertical-position-value", false},
        {reinterpret_cast<std::uint8_t*>(0x004C1889), split_race_map_expected, sizeof(split_race_map_expected), sizeof(split_race_map_expected), reinterpret_cast<void*>(&VerticalSplitRaceMapHook4C1889), "vertical-race-map-full-device", true},
        {reinterpret_cast<std::uint8_t*>(0x004D6BCC), split_camera_expected, sizeof(split_camera_expected), sizeof(split_camera_expected), reinterpret_cast<void*>(&SplitscreenCameraConfigHook4D6BCC), "three-player-split-camera", false},
        {reinterpret_cast<std::uint8_t*>(0x0045CF1B), layout_call_expected, sizeof(layout_call_expected), sizeof(layout_call_expected), reinterpret_cast<void*>(&SplitscreenViewportLayoutCall45CF1B), "viewport-layout-builder", true},
    };

    for (const SplitscreenPatchSite& site : sites) {
        if (!ModuleContains(range, site.address, site.expected_size) ||
            std::memcmp(site.address, site.expected, site.expected_size) != 0) {
            char actual[64] = {};
            if (ModuleContains(range, site.address, site.expected_size)) {
                FormatBytes(site.address, site.expected_size, actual, sizeof(actual));
            } else {
                std::snprintf(actual, sizeof(actual), "outside executable");
            }
            Log(
                "SplitscreenOrientation: signature mismatch for %s at 0x%p actual=%s; falling back to Horizontal",
                site.label,
                site.address,
                actual
            );
            SaveSplitscreenLayoutState(false);
            return false;
        }
    }

    size_t installed = 0;
    for (; installed < sizeof(sites) / sizeof(sites[0]); ++installed) {
        const SplitscreenPatchSite& site = sites[installed];
        const bool wrote = site.call
            ? WriteCall(site.address, site.hook, site.patch_size)
            : WriteJump(site.address, site.hook, site.patch_size);
        if (!wrote || !SplitscreenBranchMatches(site)) {
            Log(
                "SplitscreenOrientation: failed to install/verify %s; rolling back to Horizontal",
                site.label
            );
            if (std::memcmp(site.address, site.expected, site.patch_size) != 0) {
                WriteMemory(site.address, site.expected, site.patch_size);
            }
            break;
        }
    }

    if (installed != sizeof(sites) / sizeof(sites[0])) {
        while (installed > 0) {
            --installed;
            const SplitscreenPatchSite& site = sites[installed];
            WriteMemory(site.address, site.expected, site.patch_size);
        }
        SaveSplitscreenLayoutState(false);
        return false;
    }

    g_splitscreen_vertical_capable = true;
    g_splitscreen_three_player_capable = true;
    g_splitscreen_vertical_layout = selected_vertical;
    Log(
        "Splitscreen layout runtime: capable=1 selected=%s",
        g_splitscreen_vertical_layout ? "Vertical" : "Horizontal"
    );
    return true;
}

bool PatchSplitscreenFix(const ModuleRange& range)
{
    constexpr DWORD kSupportedTimestamp = 0x451D02BD;
    if (reinterpret_cast<std::uintptr_t>(range.base) != 0x00400000 || range.size != 0x00541000) {
        Log(
            "SplitscreenFix: unsupported executable layout base=0x%p size=0x%lX",
            range.base,
            static_cast<unsigned long>(range.size)
        );
        return false;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(range.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(range.base + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.TimeDateStamp != kSupportedTimestamp) {
        Log("SplitscreenFix: unsupported executable PE signature/timestamp");
        return false;
    }

    if (!GameFileExists("fo2_splitscreen.bfs") ||
        !GameFileHasExactBytes(g_splitscreen_filesystem_name, "fo2_splitscreen.bfs")) {
        Log(
            "SplitscreenFix: missing BFS or filesystem list is not exactly 'fo2_splitscreen.bfs' with no line ending; no hooks installed"
        );
        return false;
    }

    const std::uint8_t count_expected[] = {0x89, 0x7D, 0x14, 0x89, 0x7D, 0x08};
    const std::uint8_t ready_owner_expected[] = {
        0x8B, 0x53, 0x10, 0x8B, 0xC6,
        0xE8, 0xF8, 0x3C, 0x00, 0x00, 0x85, 0xC0, 0x0F, 0x84, 0x77, 0x06, 0x00, 0x00
    };
    const std::uint8_t held_route_expected[] = {0x83, 0x7A, 0x04, 0x00, 0x75, 0x04};
    const std::uint8_t pressed_route_expected[] = {0x83, 0x7A, 0x04, 0x00, 0x75, 0x04};
    const std::uint8_t pressed_controller_expected[] = {0x3C, 0xFF, 0x74, 0x37, 0x55};
    const std::uint8_t held_controller_expected[] = {0x3C, 0xFF, 0x74, 0x36, 0x55};
    const std::uint8_t post_processing_expected[] = {
        0x8B, 0x54, 0x24, 0x50, 0x8B, 0xCE, 0xFF, 0x92, 0x50, 0x01, 0x00, 0x00
    };
    const std::uint8_t input_registration_expected[] = {
        0x68, 0xEC, 0x1A, 0x67, 0x00,
        0x55, 0xE8, 0xFB, 0xD3, 0x10, 0x00
    };
    const std::uint8_t filesystem_expected[] = {0x68, 0xF8, 0x7D, 0x67, 0x00};

    // Install the startup mount last. The replacement script therefore
    // cannot select GM_SPLITSCREEN until the feature-scoped multiplayer threshold
    // and every mode-scoped routing detour are already live. All signatures
    // are checked before the first write.
    const SplitscreenPatchSite sites[] = {
        {reinterpret_cast<std::uint8_t*>(0x0054FF2E), count_expected, sizeof(count_expected), sizeof(count_expected), reinterpret_cast<void*>(&SplitscreenPlayerCountHook54FF2E), "local-multiplayer-threshold", false},
        {reinterpret_cast<std::uint8_t*>(0x0045BC5E), ready_owner_expected, sizeof(ready_owner_expected), 5, reinterpret_cast<void*>(&SplitscreenReadyOwnerHook45BC5E), "ready-owner-guard", false},
        {reinterpret_cast<std::uint8_t*>(0x0055D557), held_route_expected, sizeof(held_route_expected), sizeof(held_route_expected), reinterpret_cast<void*>(&SplitscreenHeldRoutingHook55D557), "held-routing", false},
        {reinterpret_cast<std::uint8_t*>(0x0055D627), pressed_route_expected, sizeof(pressed_route_expected), sizeof(pressed_route_expected), reinterpret_cast<void*>(&SplitscreenPressedRoutingHook55D627), "pressed-routing", false},
        {reinterpret_cast<std::uint8_t*>(0x0055D705), pressed_controller_expected, sizeof(pressed_controller_expected), sizeof(pressed_controller_expected), reinterpret_cast<void*>(&SplitscreenPressedControllerHook55D705), "pressed-controller", false},
        {reinterpret_cast<std::uint8_t*>(0x0055D785), held_controller_expected, sizeof(held_controller_expected), sizeof(held_controller_expected), reinterpret_cast<void*>(&SplitscreenHeldControllerHook55D785), "held-controller", false},
        {reinterpret_cast<std::uint8_t*>(0x004CBB26), post_processing_expected, sizeof(post_processing_expected), sizeof(post_processing_expected), reinterpret_cast<void*>(&SplitscreenPostProcessingHook4CBB26), "post-processing-viewport", false},
        {reinterpret_cast<std::uint8_t*>(0x004A738A), input_registration_expected, sizeof(input_registration_expected), 5, reinterpret_cast<void*>(&SplitscreenInputRegistrationHook4A738A), "split-layout-query-setter-binding", false},
        {reinterpret_cast<std::uint8_t*>(0x00520F7E), filesystem_expected, sizeof(filesystem_expected), sizeof(filesystem_expected), reinterpret_cast<void*>(&SplitscreenFilesystemHook520F7E), "filesystem-mount", false},
    };

    for (const SplitscreenPatchSite& site : sites) {
        if (std::memcmp(site.address, site.expected, site.expected_size) != 0) {
            char actual[64] = {};
            FormatBytes(site.address, site.expected_size, actual, sizeof(actual));
            Log(
                "SplitscreenFix: signature mismatch for %s at 0x%p actual=%s; no hooks installed",
                site.label,
                site.address,
                actual
            );
            return false;
        }
    }

    size_t installed = 0;
    for (; installed < sizeof(sites) / sizeof(sites[0]); ++installed) {
        const SplitscreenPatchSite& site = sites[installed];
        const bool wrote = site.call
            ? WriteCall(site.address, site.hook, site.patch_size)
            : WriteJump(site.address, site.hook, site.patch_size);
        if (!wrote || !SplitscreenBranchMatches(site)) {
            Log("SplitscreenFix: failed to install/verify %s; rolling back", site.label);
            if (std::memcmp(site.address, site.expected, site.patch_size) != 0) {
                WriteMemory(site.address, site.expected, site.patch_size);
            }
            break;
        }
    }

    if (installed != sizeof(sites) / sizeof(sites[0])) {
        while (installed > 0) {
            --installed;
            const SplitscreenPatchSite& site = sites[installed];
            WriteMemory(site.address, site.expected, site.patch_size);
        }
        return false;
    }

    Log(
        "SplitscreenFix: mounted fo2_splitscreen.bfs; local-multiplayer threshold=2 with stock current-player context=1; ready/start ownership follows explicit PlayerInfo.Controller selections; four input-routing hooks are mode-scoped; explicit script-selected input indices are active; full-device post-processing=%d",
        g_splitscreen_post_processing_fix ? 1 : 0
    );
    return true;
}

bool PatchMenuCarAllocationOperand(
    const char* label,
    std::uintptr_t operand_address,
    DWORD stock_size,
    DWORD configured_size
)
{
    auto* operand = reinterpret_cast<std::uint8_t*>(operand_address);
    DWORD actual_size = 0;
    std::memcpy(&actual_size, operand, sizeof(actual_size));

    if (actual_size != stock_size) {
        char actual[32] = {};
        FormatBytes(operand, sizeof(actual_size), actual, sizeof(actual));
        Log(
            "%s: allocation operand mismatch at 0x%08lX actual=%s",
            label,
            static_cast<unsigned long>(operand_address),
            actual
        );
        return false;
    }

    if (configured_size == stock_size) {
        Log(
            "%s: stock allocation retained at 0x%08lX (%lu bytes)",
            label,
            static_cast<unsigned long>(operand_address),
            static_cast<unsigned long>(stock_size)
        );
        return true;
    }

    const bool ok = WriteMemory(operand, &configured_size, sizeof(configured_size));
    Log(
        "%s: %s allocation operand at 0x%08lX (%lu -> %lu bytes)",
        label,
        ok ? "applied" : "failed to patch",
        static_cast<unsigned long>(operand_address),
        static_cast<unsigned long>(stock_size),
        static_cast<unsigned long>(configured_size)
    );
    return ok;
}

bool PatchMenuCarFileSizeLimits()
{
    // ZPatchFO2 v2.4 patches exactly these two PUSH imm32 operands in the
    // menu-preview object constructor. The adjacent object fields retain
    // their stock values in the original implementation as well.
    const bool model_ok = PatchMenuCarAllocationOperand(
        "MenuCarMaxModelFileSize",
        0x004ABAD1,
        0x00080000,
        g_menu_car_max_model_file_size
    );
    const bool skin_ok = PatchMenuCarAllocationOperand(
        "MenuCarMaxSkinFileSize",
        0x004ABAEB,
        0x00200000,
        g_menu_car_max_skin_file_size
    );
    return model_ok && skin_ok;
}

bool PatchMenuCarSurfaceLimit(const ModuleRange& range)
{
    g_menu_car_surface_hooks_installed = false;
    g_menu_car_static_reference_coverage_verified = false;
    if (g_menu_car_max_surfaces == kVanillaMenuCarSurfaceCapacity) {
        Log("MenuCarMaxSurfaces: stock 16 material/16 surface embedded arrays retained; hooks inactive");
        return true;
    }

    if (reinterpret_cast<std::uintptr_t>(range.base) != 0x00400000 || range.size != 0x00541000) {
        Log(
            "MenuCarMaxSurfaces: unsupported executable layout base=0x%p size=0x%lX",
            range.base,
            static_cast<unsigned long>(range.size)
        );
        return false;
    }

    const std::uint8_t material_begin_expected[] = {
        0x8B, 0x54, 0x24, 0x18,
        0x83, 0xC2, 0x20
    };
    const std::uint8_t material_loop_expected[] = {
        0x8B, 0x74, 0x24, 0x1C,
        0x2B, 0xC2
    };
    const std::uint8_t material_link_expected[] = {
        0x8B, 0x4C, 0x24, 0x74,
        0x8B, 0x74, 0x24, 0x18,
        0x8D, 0x0C, 0x89,
        0xC1, 0xE1, 0x05,
        0x8D, 0x4C, 0x31, 0x20,
        0x89, 0x4A, 0xF4
    };
    const std::uint8_t begin_expected[] = {
        0x8B, 0x4C, 0x24, 0x18,
        0x81, 0xC1, 0x2C, 0x0A, 0x00, 0x00,
        0x89, 0x4C, 0x24, 0x1C
    };
    const std::uint8_t loop_expected[] = {
        0x8B, 0xF3,
        0xB9, 0x07, 0x00, 0x00, 0x00
    };
    const std::uint8_t reader_expected[] = {
        0x8D, 0x84, 0x10, 0x20, 0x0A, 0x00, 0x00
    };
    const std::uint8_t parse_end_expected[] = {
        0x8B, 0x88, 0x8C, 0x0D, 0x00, 0x00,
        0x5E
    };
    const std::uint8_t resource_guard_expected[] = {
        0xF6, 0x45, 0x10, 0x04,
        0x75, 0x73
    };

    struct MenuCarPatchSite
    {
        std::uint8_t* address;
        const std::uint8_t* expected;
        size_t size;
        void* hook;
        const char* label;
    };

    // Consumers and the parse-end auditor are installed before parser entry
    // points. All signatures are validated before the first executable write.
    const MenuCarPatchSite sites[] = {
        {reinterpret_cast<std::uint8_t*>(0x0054D0D4), resource_guard_expected, sizeof(resource_guard_expected), reinterpret_cast<void*>(&MenuCarResourceTraversalGuardHook54D0D4), "resource-tree-invalid-link-guard"},
        {reinterpret_cast<std::uint8_t*>(0x004A537C), material_link_expected, sizeof(material_link_expected), reinterpret_cast<void*>(&MenuCarMaterialLinkHook4A537C), "surface-to-material-link"},
        {reinterpret_cast<std::uint8_t*>(0x004A5604), reader_expected, sizeof(reader_expected), reinterpret_cast<void*>(&MenuCarSurfaceReaderHook4A5604), "model-surface-resolver"},
        {reinterpret_cast<std::uint8_t*>(0x004A57CB), parse_end_expected, sizeof(parse_end_expected), reinterpret_cast<void*>(&MenuCarParseEndHook4A57CB), "parse-end-self-check"},
        {reinterpret_cast<std::uint8_t*>(0x004A4E1F), material_loop_expected, sizeof(material_loop_expected), reinterpret_cast<void*>(&MenuCarMaterialLoopHook4A4E1F), "material-parser-loop"},
        {reinterpret_cast<std::uint8_t*>(0x004A536F), loop_expected, sizeof(loop_expected), reinterpret_cast<void*>(&MenuCarSurfaceLoopHook4A536F), "surface-parser-loop"},
        {reinterpret_cast<std::uint8_t*>(0x004A4DFF), material_begin_expected, sizeof(material_begin_expected), reinterpret_cast<void*>(&MenuCarMaterialBeginHook4A4DFF), "material-parser-begin"},
        {reinterpret_cast<std::uint8_t*>(0x004A535B), begin_expected, sizeof(begin_expected), reinterpret_cast<void*>(&MenuCarSurfaceBeginHook4A535B), "surface-parser-begin"},
    };

    for (const MenuCarPatchSite& site : sites) {
        if (std::memcmp(site.address, site.expected, site.size) != 0) {
            char actual[96] = {};
            FormatBytes(site.address, site.size, actual, sizeof(actual));
            Log(
                "MenuCarMaxSurfaces: signature mismatch for %s at 0x%p actual=%s; no hooks installed",
                site.label,
                site.address,
                actual
            );
            return false;
        }
    }

    if (!VerifyMenuCarStaticReferenceCoverage()) {
        return false;
    }
    g_menu_car_static_reference_coverage_verified = true;

    size_t installed = 0;
    for (; installed < sizeof(sites) / sizeof(sites[0]); ++installed) {
        const MenuCarPatchSite& site = sites[installed];
        if (!WriteJump(site.address, site.hook, site.size) ||
            !MenuCarJumpTargets(reinterpret_cast<std::uintptr_t>(site.address), site.hook)) {
            Log("MenuCarMaxSurfaces: failed to install/verify %s; rolling back", site.label);
            if (std::memcmp(site.address, site.expected, site.size) != 0) {
                WriteMemory(site.address, site.expected, site.size);
            }
            break;
        }
    }

    if (installed != sizeof(sites) / sizeof(sites[0])) {
        while (installed > 0) {
            --installed;
            const MenuCarPatchSite& site = sites[installed];
            WriteMemory(site.address, site.expected, site.size);
        }
        return false;
    }

    g_menu_car_surface_hooks_installed = true;
    Log(
        "MenuCarMaxSurfaces: applied complete material+surface heap redirect with capacity %lu; parser/link sites=0x004A4DFF,0x004A4E1F,0x004A537C,0x004A535B,0x004A536F,0x004A5604,0x004A57CB; invalid nested-resource link guard=0x0054D0D4",
        static_cast<unsigned long>(g_menu_car_max_surfaces)
    );
    return true;
}

bool PatchMenuCarBackfaceCulling()
{
    auto* hook_address = reinterpret_cast<std::uint8_t*>(0x005AACB0);
    const std::uint8_t expected[] = {
        0x8B, 0x01,
        0x6A, 0x03,
        0xFF, 0x50, 0x5C
    };

    if (std::memcmp(hook_address, expected, sizeof(expected)) == 0) {
        const bool ok = WriteJump(
            hook_address,
            reinterpret_cast<void*>(&MenuCarBackfaceCullingHook5AACB0),
            sizeof(expected)
        );
        Log(
            "MenuCarBackfaceCulling: %s D3DRS_CULLMODE hook at 0x005AACB0",
            ok ? "applied" : "failed to patch"
        );
        return ok;
    }

    char actual[48] = {};
    FormatBytes(hook_address, sizeof(expected), actual, sizeof(actual));
    Log("MenuCarBackfaceCulling: hook mismatch at 0x005AACB0 actual=%s", actual);
    return false;
}

bool ModuleRvaContains(const ModuleRange& range, DWORD rva, size_t size)
{
    return range.base != nullptr && rva <= range.size && size <= range.size - rva;
}

bool ModuleStringEquals(const ModuleRange& range, DWORD rva, const char* expected, bool ignore_case)
{
    if (expected == nullptr) {
        return false;
    }
    for (size_t i = 0;; ++i) {
        if (i > MAX_PATH || !ModuleRvaContains(range, rva, i + 1)) {
            return false;
        }
        const unsigned char actual = range.base[rva + i];
        const unsigned char wanted = static_cast<unsigned char>(expected[i]);
        const unsigned char folded_actual = ignore_case && actual >= 'A' && actual <= 'Z'
            ? static_cast<unsigned char>(actual - 'A' + 'a')
            : actual;
        const unsigned char folded_wanted = ignore_case && wanted >= 'A' && wanted <= 'Z'
            ? static_cast<unsigned char>(wanted - 'A' + 'a')
            : wanted;
        if (folded_actual != folded_wanted) {
            return false;
        }
        if (wanted == '\0') {
            return true;
        }
    }
}

void** FindImportIatSlotByName(const ModuleRange& range, const char* dll_name, const char* import_name)
{
    if (range.base == nullptr || !IsReadableMemory(range.base, sizeof(IMAGE_DOS_HEADER))) {
        return nullptr;
    }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(range.base);
    if (!ModuleRvaContains(range, static_cast<DWORD>(dos->e_lfanew), sizeof(IMAGE_NT_HEADERS))) {
        return nullptr;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(range.base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
        !ModuleRvaContains(range, directory.VirtualAddress, directory.Size)) {
        return nullptr;
    }

    const DWORD descriptor_count = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (DWORD descriptor_index = 0; descriptor_index < descriptor_count; ++descriptor_index) {
        auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
            range.base + directory.VirtualAddress + descriptor_index * sizeof(IMAGE_IMPORT_DESCRIPTOR)
        );
        if (descriptor->Name == 0) {
            break;
        }
        if (!ModuleStringEquals(range, descriptor->Name, dll_name, true) ||
            descriptor->OriginalFirstThunk == 0 || descriptor->FirstThunk == 0) {
            continue;
        }

        for (DWORD thunk_index = 0;; ++thunk_index) {
            const size_t thunk_offset = static_cast<size_t>(thunk_index) * sizeof(IMAGE_THUNK_DATA);
            const size_t original_rva = static_cast<size_t>(descriptor->OriginalFirstThunk) + thunk_offset;
            const size_t iat_rva = static_cast<size_t>(descriptor->FirstThunk) + thunk_offset;
            if (original_rva > MAXDWORD || iat_rva > MAXDWORD ||
                !ModuleRvaContains(range, static_cast<DWORD>(original_rva), sizeof(IMAGE_THUNK_DATA)) ||
                !ModuleRvaContains(range, static_cast<DWORD>(iat_rva), sizeof(IMAGE_THUNK_DATA))) {
                break;
            }
            auto* original_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
                range.base + original_rva
            );
            auto* iat_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
                range.base + iat_rva
            );
            if (original_thunk->u1.AddressOfData == 0) {
                break;
            }
            if (IMAGE_SNAP_BY_ORDINAL(original_thunk->u1.Ordinal)) {
                continue;
            }
            const size_t name_rva = static_cast<size_t>(original_thunk->u1.AddressOfData) +
                offsetof(IMAGE_IMPORT_BY_NAME, Name);
            if (name_rva <= MAXDWORD && ModuleStringEquals(range, static_cast<DWORD>(name_rva), import_name, false)) {
                return reinterpret_cast<void**>(&iat_thunk->u1.Function);
            }
        }
    }
    return nullptr;
}

bool AtomicReplacePointer(void** slot, void* expected, void* replacement)
{
    if (slot == nullptr) {
        return false;
    }
    DWORD old_protect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
        return false;
    }
    void* previous = InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(slot),
        replacement,
        expected
    );
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old_protect, &ignored);
    return previous == expected;
}

void RestoreSplitscreenZoomInputFix()
{
    void** slot = g_zoom_send_input_iat;
    void* original = reinterpret_cast<void*>(g_real_zoom_send_input);
    if (slot != nullptr && original != nullptr && *slot == reinterpret_cast<void*>(&ProxyZoomSendInput)) {
        AtomicReplacePointer(slot, reinterpret_cast<void*>(&ProxyZoomSendInput), original);
    }
    ClearZoomInputLatches();
    g_zoom_send_input_iat = nullptr;
    g_real_zoom_send_input = nullptr;
    g_zoom_module_base = nullptr;
}

void PatchSplitscreenZoomInputFix()
{
    HMODULE zoom_module = GetModuleHandleA("fo2_zoom.dll");
    if (zoom_module == nullptr) {
        Log("SplitscreenZoomInputFix: fo2_zoom.dll absent; compatibility hook not needed");
        return;
    }

    const ModuleRange zoom = GetModuleRange(zoom_module);
    if (zoom.base == nullptr) {
        Log("SplitscreenZoomInputFix: fo2_zoom.dll has an invalid PE image; compatibility hook skipped");
        return;
    }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(zoom.base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(zoom.base + dos->e_lfanew);
    constexpr DWORD kSupportedZoomTimestamp = 0x692D78D4;
    constexpr DWORD kSupportedZoomImageSize = 0x00106000;
    if (nt->FileHeader.TimeDateStamp != kSupportedZoomTimestamp ||
        zoom.size != kSupportedZoomImageSize) {
        Log(
            "SplitscreenZoomInputFix: unsupported fo2_zoom.dll timestamp=0x%08lX image_size=0x%lX; identity translation not installed",
            static_cast<unsigned long>(nt->FileHeader.TimeDateStamp),
            static_cast<unsigned long>(zoom.size)
        );
        return;
    }

    void** slot = FindImportIatSlotByName(zoom, "USER32.dll", "SendInput");
    if (slot == nullptr) {
        Log(
            "SplitscreenZoomInputFix: USER32!SendInput import absent in fo2_zoom.dll timestamp=0x%08lX image_size=0x%lX; compatibility hook skipped",
            static_cast<unsigned long>(nt->FileHeader.TimeDateStamp),
            static_cast<unsigned long>(zoom.size)
        );
        return;
    }

    HMODULE user32 = GetModuleHandleA("user32.dll");
    void* real_send_input = user32 != nullptr
        ? reinterpret_cast<void*>(GetProcAddress(user32, "SendInput"))
        : nullptr;
    void* current_target = *slot;
    if (real_send_input == nullptr || current_target != real_send_input) {
        Log(
            "SplitscreenZoomInputFix: unexpected SendInput IAT target slot=0x%p target=0x%p user32=0x%p; compatibility hook skipped",
            slot,
            current_target,
            real_send_input
        );
        return;
    }

    g_real_zoom_send_input = reinterpret_cast<SendInputFn>(current_target);
    g_zoom_send_input_iat = slot;
    g_zoom_module_base = zoom.base;
    ClearZoomInputLatches();
    if (!AtomicReplacePointer(slot, current_target, reinterpret_cast<void*>(&ProxyZoomSendInput))) {
        g_zoom_send_input_iat = nullptr;
        g_real_zoom_send_input = nullptr;
        g_zoom_module_base = nullptr;
        Log("SplitscreenZoomInputFix: failed atomic USER32!SendInput IAT replacement; compatibility hook skipped");
        return;
    }

    Log(
        "SplitscreenZoomInputFix: installed version-gated Zoom pad-identity translation slot=0x%p rva=0x%lX original=0x%p timestamp=0x%08lX image_size=0x%lX vector_rvas=0xFECFC/0xFED00 stride=0x3C",
        slot,
        static_cast<unsigned long>(reinterpret_cast<std::uint8_t*>(slot) - zoom.base),
        current_target,
        static_cast<unsigned long>(nt->FileHeader.TimeDateStamp),
        static_cast<unsigned long>(zoom.size)
    );
}

bool PatchWidescreenFix()
{
    int screen_width = GetSystemMetrics(SM_CXSCREEN);
    int screen_height = GetSystemMetrics(SM_CYSCREEN);
    if (screen_width <= 0 || screen_height <= 0) {
        screen_width = 1920;
        screen_height = 1080;
    }

    const float aspect = static_cast<float>(screen_width) / static_cast<float>(screen_height);
    const float virtual_width = 480.0f * aspect;
    g_widescreen_aspect = aspect;
    g_widescreen_screen_width = static_cast<float>(screen_width);
    g_widescreen_normalized_hud_scale = 1.0f / virtual_width;
    g_widescreen_vertical_scale = g_widescreen_screen_width * g_widescreen_normalized_hud_scale;
    g_widescreen_fov_scale = 1.0f / 640.0f;
    g_widescreen_menu_scale_bits = FloatBits(g_widescreen_screen_width * g_widescreen_fov_scale);

    Log(
        "WidescreenFix: computed widescreen values for %dx%d aspect=%.4f",
        screen_width,
        screen_height,
        aspect
    );
    return true;
}

bool PatchWidescreenFovScaling()
{
    if (!g_widescreen_fov_scaling) {
        return true;
    }

    const bool ok = WriteFloatOperand(0x004B4C6E, &g_widescreen_normalized_hud_scale);
    Log(
        "WidescreenFix_FOVScaling: %s default scale %.9f operand at 0x004B4C6E",
        ok ? "applied" : "failed",
        static_cast<double>(g_widescreen_normalized_hud_scale)
    );
    return ok;
}

bool PatchWidescreenFovContextHook()
{
    if (!g_widescreen_fov_scaling) {
        return true;
    }

    std::uint8_t* hook_address = reinterpret_cast<std::uint8_t*>(0x004C0AE1);
    const std::uint8_t expected[] = {
        0xD8, 0x6C, 0x24, 0x68,
        0xD9, 0x1C, 0x24,
        0xE8, 0x03, 0x41, 0xFF, 0xFF
    };

    if (std::memcmp(hook_address, expected, sizeof(expected)) == 0) {
        const bool ok = WriteJump(hook_address, reinterpret_cast<void*>(&FovContextProjectionCallHook), sizeof(expected));
        Log(
            "WidescreenFix_FOVScaling: %s FOV context hook at 0x004C0AE1",
            ok ? "applied" : "failed"
        );
        return ok;
    }

    const std::uint8_t chained_expected_prefix[] = {
        0xD8, 0x6C, 0x24, 0x68,
        0xD9, 0x1C, 0x24,
        0xE9
    };
    if (std::memcmp(hook_address, chained_expected_prefix, sizeof(chained_expected_prefix)) == 0) {
        const bool ok = WriteJump(hook_address, reinterpret_cast<void*>(&FovContextChainedHook), 7);
        Log(
            "WidescreenFix_FOVScaling: %s chained FOV context hook at 0x004C0AE1",
            ok ? "applied" : "failed"
        );
        return ok;
    }

    char actual[64] = {};
    FormatBytes(hook_address, sizeof(expected), actual, sizeof(actual));
    Log("WidescreenFix_FOVScaling: FOV context hook bytes mismatch at 0x004C0AE1 actual=%s", actual);
    Log("WidescreenFix_FOVScaling: failed FOV context hook at 0x004C0AE1");
    return false;
}

bool PatchWidescreenFovMatrixHooks()
{
    if (!g_widescreen_fov_scaling) {
        return true;
    }

    bool ok = true;

    {
        std::uint8_t* hook_address = reinterpret_cast<std::uint8_t*>(0x004CBDEF);
        const std::uint8_t expected[] = {
            0xD8, 0x4C, 0x24, 0x4C,
            0xD9, 0x5E, 0x0C,
            0xD8, 0x4C, 0x24, 0x4C,
            0xD9, 0x5E, 0x08
        };
        if (std::memcmp(hook_address, expected, sizeof(expected)) == 0) {
            ok = WriteJump(hook_address, reinterpret_cast<void*>(&ProjectionMatrixExtentsHook), sizeof(expected)) && ok;
        } else {
            char actual[64] = {};
            FormatBytes(hook_address, sizeof(expected), actual, sizeof(actual));
            Log("WidescreenFix_FOVScaling: matrix extents hook mismatch at 0x004CBDEF actual=%s", actual);
            ok = false;
        }
    }

    {
        std::uint8_t* hook_address = reinterpret_cast<std::uint8_t*>(0x004C9F63);
        const std::uint8_t expected[] = {
            0xD9, 0x98, 0x00, 0x01, 0x00, 0x00,
            0xD9, 0x98, 0x04, 0x01, 0x00, 0x00
        };
        if (std::memcmp(hook_address, expected, sizeof(expected)) == 0) {
            ok = WriteJump(hook_address, reinterpret_cast<void*>(&ProjectionMatrixObjectHook), sizeof(expected)) && ok;
        } else {
            char actual[64] = {};
            FormatBytes(hook_address, sizeof(expected), actual, sizeof(actual));
            Log("WidescreenFix_FOVScaling: matrix object hook mismatch at 0x004C9F63 actual=%s", actual);
            ok = false;
        }
    }

    {
        std::uint8_t* hook_address = reinterpret_cast<std::uint8_t*>(0x004C9E27);
        const std::uint8_t expected[] = {
            0x8B, 0xC3,
            0x83, 0xE8, 0x02,
            0x75, 0x02,
            0xDC, 0xC0
        };
        if (std::memcmp(hook_address, expected, sizeof(expected)) == 0) {
            ok = WriteJump(hook_address, reinterpret_cast<void*>(&ProjectionSplitModeHook), sizeof(expected)) && ok;
        } else {
            const SplitscreenPatchSite shared_site = {
                hook_address,
                expected,
                sizeof(expected),
                sizeof(expected),
                reinterpret_cast<void*>(&ProjectionSplitModeHook),
                "primary-projection-aspect",
                false
            };
            if (g_splitscreen_vertical_capable && SplitscreenBranchMatches(shared_site)) {
                Log("WidescreenFix_FOVScaling: retained shared vertical split-mode hook at 0x004C9E27");
            } else {
                char actual[48] = {};
                FormatBytes(hook_address, sizeof(expected), actual, sizeof(actual));
                Log("WidescreenFix_FOVScaling: split-mode hook mismatch at 0x004C9E27 actual=%s", actual);
                ok = false;
            }
        }
    }

    {
        std::uint8_t* hook_address = reinterpret_cast<std::uint8_t*>(0x0059964A);
        const std::uint8_t expected[] = {
            0xDD, 0xD8,
            0xFF, 0x93, 0xB0
        };
        if (std::memcmp(hook_address, expected, sizeof(expected)) == 0) {
            ok = WriteJump(hook_address, reinterpret_cast<void*>(&ProjectionStackHook59964A), sizeof(expected)) && ok;
        } else {
            char actual[48] = {};
            FormatBytes(hook_address, sizeof(expected), actual, sizeof(actual));
            Log("WidescreenFix_FOVScaling: projection-stack hook mismatch at 0x0059964A actual=%s", actual);
            ok = false;
        }
    }

    Log(
        "WidescreenFix_FOVScaling: %s FOV matrix hooks, aspect=%.4f",
        ok ? "applied" : "failed one or more",
        static_cast<double>(g_widescreen_aspect)
    );
    return ok;
}

bool PatchWidescreenMenuScaleHooks()
{
    std::uint8_t* hook_address = reinterpret_cast<std::uint8_t*>(0x00533690);
    const std::uint8_t expected[] = {
        0xD8, 0x4E, 0x0C,
        0xD9, 0x5C, 0x24, 0x1C
    };
    if (std::memcmp(hook_address, expected, sizeof(expected)) == 0) {
        const bool ok = WriteJump(hook_address, reinterpret_cast<void*>(&MenuTransformHeightHook533690), sizeof(expected));
        Log("WidescreenFix: %s menu transform-height hook at 0x00533690", ok ? "applied" : "failed");
        return ok;
    }

    char actual[48] = {};
    FormatBytes(hook_address, sizeof(expected), actual, sizeof(actual));
    Log("WidescreenFix: menu transform-height hook mismatch at 0x00533690 actual=%s", actual);
    return false;
}

void ApplyPatches()
{
    const ModuleRange exe = GetExeRange();
    Log("FO2 ZPatch reimplementation attached: exe=0x%p size=0x%lX", exe.base, static_cast<unsigned long>(exe.size));
    Log(
        "Config: Log=%d SkipLicenseScreen=%d SkipIntro=%d UncapFPS=%d FramePacingFix=%d RemoveVSync=%d BorderlessWindowed=%d WidescreenFix=%d FOVScaling=%d SplitscreenFix=%d SplitscreenLayoutState=%s SplitscreenPostProcessingFix=%d SplitscreenZoomInputFix=%d MenuCarBackfaceCulling=%d MenuCarModelMax=%lu MenuCarSkinMax=%lu MenuCarMaxSurfaces=%lu",
        g_log_enabled ? 1 : 0,
        g_skip_license_screen ? 1 : 0,
        g_skip_intro ? 1 : 0,
        g_uncap_fps ? 1 : 0,
        g_frame_pacing_fix ? 1 : 0,
        g_remove_vsync ? 1 : 0,
        g_borderless_windowed ? 1 : 0,
        g_widescreen_fix ? 1 : 0,
        g_widescreen_fov_scaling ? 1 : 0,
        g_splitscreen_fix ? 1 : 0,
        g_splitscreen_vertical_layout ? "Vertical" : "Horizontal",
        g_splitscreen_post_processing_fix ? 1 : 0,
        g_splitscreen_zoom_input_fix ? 1 : 0,
        g_menu_car_backface_culling ? 1 : 0,
        static_cast<unsigned long>(g_menu_car_max_model_file_size),
        static_cast<unsigned long>(g_menu_car_max_skin_file_size),
        static_cast<unsigned long>(g_menu_car_max_surfaces)
    );

    if (g_splitscreen_fix) {
        const bool splitscreen_installed = PatchSplitscreenFix(exe);
        bool splitscreen_presentation_installed = false;
        if (splitscreen_installed) {
            splitscreen_presentation_installed = PatchVerticalSplitscreenLayout(exe);
        }
        if (splitscreen_installed && splitscreen_presentation_installed) {
            PatchFourPlayerInputLifecycle(exe);
        }
        if (splitscreen_installed && g_splitscreen_zoom_input_fix) {
            PatchSplitscreenZoomInputFix();
        }
    }
    if (g_skip_license_screen) {
        PatchSkipLicenseScreen();
    }
    if (g_skip_intro) {
        PatchSkipIntro(exe);
    }
    if (g_frame_pacing_fix) {
        ApplyTimerResolution();
        PatchFramePacing();
    }
    if (g_uncap_fps) {
        PatchUncapFPS(exe);
    }
    PatchMenuCarFileSizeLimits();
    PatchMenuCarSurfaceLimit(exe);
    if (g_menu_car_backface_culling) {
        PatchMenuCarBackfaceCulling();
    }
    if (g_widescreen_fix) {
        PatchWidescreenFix();
        PatchWidescreenMenuScaleHooks();
        PatchWidescreenFovScaling();
        PatchWidescreenFovContextHook();
        PatchWidescreenFovMatrixHooks();
    }
    if (g_remove_vsync || g_borderless_windowed) {
        PatchRemoveVSync(exe);
    }
}

DWORD WINAPI InitThread(LPVOID)
{
    LoadConfig();
    ApplyPatches();
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        const HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        RestoreSplitscreenZoomInputFix();
    }
    return TRUE;
}
