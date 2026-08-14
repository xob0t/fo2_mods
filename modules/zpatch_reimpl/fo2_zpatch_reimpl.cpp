#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <mmsystem.h>

#include <algorithm>
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
bool g_menu_car_backface_culling = true;
DWORD g_menu_car_max_model_file_size = 524288;
DWORD g_menu_car_max_skin_file_size = 2097152;
char g_log_path[MAX_PATH] = {};
HMODULE g_winmm = nullptr;

using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
Direct3DCreate9Fn g_real_direct3d_create9 = nullptr;
using ResetFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
ResetFn g_real_reset = nullptr;
volatile LONG g_device_hooks_installed = 0;
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

void LoadConfig()
{
    char ini_path[MAX_PATH] = {};
    BuildGamePath(ini_path, MAX_PATH, "fo2_zpatch_reimpl.ini");
    BuildGamePath(g_log_path, MAX_PATH, "fo2_zpatch_reimpl.log");

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
    g_menu_car_backface_culling = GetPrivateProfileIntA("Fixes", "MenuCarBackfaceCulling", 1, ini_path) != 0;
    g_menu_car_max_model_file_size = std::max<DWORD>(GetPrivateProfileIntA("Fixes", "MenuCarMaxModelFileSize", 524288, ini_path), 1);
    g_menu_car_max_skin_file_size = std::max<DWORD>(GetPrivateProfileIntA("Fixes", "MenuCarMaxSkinFileSize", 2097152, ini_path), 1);
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
    const double half_tangent = std::tan(static_cast<double>(fov_radians) * 0.5);
    const float horizontal = static_cast<float>(half_tangent * static_cast<double>(near_scale) * static_cast<double>(g_widescreen_aspect) * 0.75);
    const float vertical = static_cast<float>(half_tangent * static_cast<double>(near_scale));

    *reinterpret_cast<float*>(bytes + 0x0F4) = horizontal;
    *reinterpret_cast<float*>(bytes + 0x0F0) = -horizontal;
    *reinterpret_cast<float*>(bytes + 0x0FC) = g_projection_split_mode != 0 ? vertical * 0.5f : vertical;
    *reinterpret_cast<float*>(bytes + 0x0F8) = g_projection_split_mode != 0 ? -vertical * 0.5f : -vertical;
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
        fadd st(0), st(0)
        mov dword ptr [g_projection_split_mode], 1
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
    const double half_tangent = std::tan(static_cast<double>(fov_radians) * 0.5);
    const float horizontal = static_cast<float>(half_tangent * static_cast<double>(g_widescreen_aspect) * 0.75);
    const float vertical = static_cast<float>(half_tangent);

    *reinterpret_cast<float*>(stack_bytes + 0x18) = horizontal;
    *reinterpret_cast<float*>(stack_bytes + 0x14) = -horizontal;
    *reinterpret_cast<float*>(stack_bytes + 0x20) = g_projection_split_mode != 0 ? vertical * 0.5f : vertical;
    *reinterpret_cast<float*>(stack_bytes + 0x1C) = g_projection_split_mode != 0 ? -vertical * 0.5f : -vertical;
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

ModuleRange GetExeRange()
{
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleA(nullptr));
    if (base == nullptr) {
        return ModuleRange{};
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return ModuleRange{};
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return ModuleRange{};
    }

    return ModuleRange{base, nt->OptionalHeader.SizeOfImage};
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
            char actual[48] = {};
            FormatBytes(hook_address, sizeof(expected), actual, sizeof(actual));
            Log("WidescreenFix_FOVScaling: split-mode hook mismatch at 0x004C9E27 actual=%s", actual);
            ok = false;
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
        "Config: Log=%d SkipLicenseScreen=%d SkipIntro=%d UncapFPS=%d FramePacingFix=%d RemoveVSync=%d BorderlessWindowed=%d WidescreenFix=%d FOVScaling=%d SplitscreenFix=%d MenuCarBackfaceCulling=%d MenuCarModelMax=%lu MenuCarSkinMax=%lu",
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
        g_menu_car_backface_culling ? 1 : 0,
        static_cast<unsigned long>(g_menu_car_max_model_file_size),
        static_cast<unsigned long>(g_menu_car_max_skin_file_size)
    );

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
    }
    return TRUE;
}
