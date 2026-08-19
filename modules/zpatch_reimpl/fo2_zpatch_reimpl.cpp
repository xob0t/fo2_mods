#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <mmsystem.h>

#include <algorithm>
#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
bool g_splitscreen_vertical_layout = true;
bool g_splitscreen_vertical_capable = false;
bool g_splitscreen_vertical_render_active = false;
bool g_splitscreen_vertical_hud_pass_active = false;
bool g_menu_car_backface_culling = true;
DWORD g_menu_car_max_model_file_size = 524288;
DWORD g_menu_car_max_skin_file_size = 2097152;
char g_log_path[MAX_PATH] = {};
char g_splitscreen_layout_state_path[MAX_PATH] = {};
char g_splitscreen_filesystem_name[] = "fo2_splitscreen_filesystem";
HMODULE g_winmm = nullptr;

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
    g_splitscreen_vertical_layout = true;
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
        SaveSplitscreenLayoutState(true);
        return;
    }

    const DWORD file_size = GetFileSize(file, nullptr);
    if (file_size != 8) {
        CloseHandle(file);
        SaveSplitscreenLayoutState(true);
        return;
    }

    char contents[9] = {};
    DWORD bytes_read = 0;
    const BOOL read_ok = ReadFile(file, contents, 8, &bytes_read, nullptr);
    CloseHandle(file);
    if (!read_ok) {
        SaveSplitscreenLayoutState(true);
        return;
    }

    if (bytes_read == 8 && std::memcmp(contents, "return 0", 8) == 0) {
        g_splitscreen_vertical_layout = false;
    } else if (bytes_read == 8 && std::memcmp(contents, "return 1", 8) == 0) {
        g_splitscreen_vertical_layout = true;
    } else {
        SaveSplitscreenLayoutState(true);
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
    g_menu_car_backface_culling = GetPrivateProfileIntA("Fixes", "MenuCarBackfaceCulling", 1, ini_path) != 0;
    g_menu_car_max_model_file_size = std::max<DWORD>(GetPrivateProfileIntA("Fixes", "MenuCarMaxModelFileSize", 524288, ini_path), 1);
    g_menu_car_max_skin_file_size = std::max<DWORD>(GetPrivateProfileIntA("Fixes", "MenuCarMaxSkinFileSize", 2097152, ini_path), 1);
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

void RefreshVerticalSplitscreenVisualState()
{
    g_splitscreen_vertical_render_active = IsLiveVerticalSplitscreenVisualState();
}

void RewriteVerticalSplitscreenLayout(void* layout_object, DWORD viewport_count)
{
    // Only the world currently published as the renderer's viewport registry
    // owns the live split layout. Other layout builders must not clear the
    // render latch for that registry.
    auto* current_registry = *reinterpret_cast<void**>(0x00696DC8);
    if (layout_object == nullptr || layout_object != current_registry) {
        return;
    }

    g_splitscreen_vertical_render_active = false;
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
        call RewriteVerticalSplitscreenLayout
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

__declspec(noinline) void CallOriginalRaceMinimap(
    void* map_this,
    float* position,
    const float* map_size,
    DWORD arg3,
    DWORD arg4,
    void* map_context,
    DWORD map_type)
{
    __asm {
        push map_type
        push map_context
        push arg4
        push arg3
        push map_size
        push position
        mov eax, map_this
        mov ecx, 0x004C5AE0
        call ecx
    }
}

void DrawVerticalRaceMinimap(
    void* map_this,
    float* position,
    const float* size,
    DWORD arg3,
    DWORD arg4,
    void* context,
    DWORD map_type)
{
    bool scissor_changed = false;
    IDirect3DDevice9* device = nullptr;
    D3DVIEWPORT9 saved_viewport = {};
    RECT saved_scissor = {};
    float original_x = 0.0f;

    const bool position_writable = IsWritableMemory(position, sizeof(float) * 2);
    const bool size_readable = IsReadableMemory(size, sizeof(float) * 2);
    const float width = size_readable ? size[0] : 0.0f;
    auto* renderer = *reinterpret_cast<std::uint8_t**>(0x008DA718);
    const bool renderer_readable = IsReadableMemory(renderer, 0x10);
    const DWORD device_width = renderer_readable ? *reinterpret_cast<DWORD*>(renderer + 0x08) : 0;
    const DWORD device_height = renderer_readable ? *reinterpret_cast<DWORD*>(renderer + 0x0C) : 0;

    if (IsLiveVerticalSplitscreenVisualState() && map_type == 1 &&
        position_writable && size_readable && std::isfinite(width) &&
        width > 0.0f && width <= 640.0f && device_width > 0 && device_height > 0) {
        device = *reinterpret_cast<IDirect3DDevice9**>(0x008DA788);
        void** device_vtable = IsReadableMemory(device, sizeof(void*)) ?
            *reinterpret_cast<void***>(device) : nullptr;
        if (IsReadableMemory(device_vtable, 0x134)) {
            DWORD scissor_enabled = FALSE;
            if (SUCCEEDED(device->GetViewport(&saved_viewport)) &&
                SUCCEEDED(device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor_enabled)) &&
                (!scissor_enabled || SUCCEEDED(device->GetScissorRect(&saved_scissor)))) {
                D3DVIEWPORT9 full_viewport = {
                    0,
                    0,
                    device_width,
                    device_height,
                    saved_viewport.MinZ,
                    saved_viewport.MaxZ
                };
                RECT full_scissor = {
                    0,
                    0,
                    static_cast<LONG>(device_width),
                    static_cast<LONG>(device_height)
                };
                if (SUCCEEDED(device->SetViewport(&full_viewport))) {
                    if (!scissor_enabled || SUCCEEDED(device->SetScissorRect(&full_scissor))) {
                        scissor_changed = scissor_enabled != FALSE;
                        original_x = position[0];
                        position[0] = (640.0f - width) * 0.5f;
                        CallOriginalRaceMinimap(
                            map_this, position, size, arg3, arg4, context, map_type
                        );
                        position[0] = original_x;
                        if (scissor_changed) {
                            device->SetScissorRect(&saved_scissor);
                        }
                        device->SetViewport(&saved_viewport);
                        return;
                    }
                    device->SetViewport(&saved_viewport);
                }
            }
        }
    }

    // Every validation/query/state failure preserves the stock position and
    // calls the original renderer under its inherited viewport exactly once.
    CallOriginalRaceMinimap(map_this, position, size, arg3, arg4, context, map_type);
}

__declspec(naked) void SplitscreenRaceMapHook4B9BEB()
{
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        push dword ptr [ebp + 0x1C]
        push dword ptr [ebp + 0x18]
        push dword ptr [ebp + 0x14]
        push dword ptr [ebp + 0x10]
        push dword ptr [ebp + 0x0C]
        push dword ptr [ebp + 0x08]
        push eax
        call DrawVerticalRaceMinimap
        add esp, 0x1C
        pop edi
        pop esi
        pop ebx
        mov esp, ebp
        pop ebp
        ret 0x18
    }
}

bool BeginVerticalHudPass()
{
    g_splitscreen_vertical_hud_pass_active = IsLiveVerticalSplitscreenVisualState();
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
        // Skip the complete two-entry BGBar loop for a live vertical HUD.
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
        push 0x004BA098
        ret
    normal:
        push 0x004BA0B6
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

    const DWORD current_ordinal = *reinterpret_cast<DWORD*>(game_flow + 0x9CC);
    const DWORD device_count = *reinterpret_cast<DWORD*>(input_manager + 0x04);
    if (current_ordinal >= 2) {
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

    DWORD source_controller = *reinterpret_cast<DWORD*>(event_bytes + 0x10);
    const DWORD target_ordinal = *reinterpret_cast<DWORD*>(game_flow + 0x9CC);
    const DWORD device_count = *reinterpret_cast<DWORD*>(input_manager + 0x04);
    if (target_ordinal >= 2 || source_controller >= device_count) {
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
        // player/action context at +0x14, but configure the independent
        // logical local-player count at +0x08 for two-player split screen.
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

bool PatchVerticalSplitscreenLayout(const ModuleRange& range)
{
    const bool selected_vertical = g_splitscreen_vertical_layout;
    g_splitscreen_vertical_layout = false;
    g_splitscreen_vertical_capable = false;
    g_splitscreen_vertical_render_active = false;
    g_splitscreen_vertical_hud_pass_active = false;

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
    const std::uint8_t race_map_expected[] = {0xE8, 0xF0, 0xBE, 0x00, 0x00};
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
        {reinterpret_cast<std::uint8_t*>(0x004B9BEB), race_map_expected, sizeof(race_map_expected), sizeof(race_map_expected), reinterpret_cast<void*>(&SplitscreenRaceMapHook4B9BEB), "vertical-race-map-center", true},
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
    // cannot select GM_SPLITSCREEN until the feature-scoped two-slot setup
    // and every mode-scoped routing detour are already live. All signatures
    // are checked before the first write.
    const SplitscreenPatchSite sites[] = {
        {reinterpret_cast<std::uint8_t*>(0x0054FF2E), count_expected, sizeof(count_expected), sizeof(count_expected), reinterpret_cast<void*>(&SplitscreenPlayerCountHook54FF2E), "logical-player-count", false},
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
        "SplitscreenFix: mounted fo2_splitscreen.bfs; logical local-player count=2 with stock current-player context=1; ready/start ownership follows explicit PlayerInfo.Controller selections; four input-routing hooks are mode-scoped; explicit script-selected input indices are active; full-device post-processing=%d",
        g_splitscreen_post_processing_fix ? 1 : 0
    );
    return true;
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
        "Config: Log=%d SkipLicenseScreen=%d SkipIntro=%d UncapFPS=%d FramePacingFix=%d RemoveVSync=%d BorderlessWindowed=%d WidescreenFix=%d FOVScaling=%d SplitscreenFix=%d SplitscreenLayoutState=%s SplitscreenPostProcessingFix=%d SplitscreenZoomInputFix=%d MenuCarBackfaceCulling=%d MenuCarModelMax=%lu MenuCarSkinMax=%lu",
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
        static_cast<unsigned long>(g_menu_car_max_skin_file_size)
    );

    if (g_splitscreen_fix) {
        const bool splitscreen_installed = PatchSplitscreenFix(exe);
        if (splitscreen_installed) {
            PatchVerticalSplitscreenLayout(exe);
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
