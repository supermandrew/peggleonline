#include "present.h"

#include <windows.h>
#include <d3d9.h>
#include <ddraw.h>

#include <cstdio>

#include "display.h"
#include "game.h"
#include "hook.h"
#include "log.h"

namespace {

constexpr int kGameW = 800, kGameH = 600;
constexpr DWORD kRetryMs = 2000;  // after Direct3D fails, GDI until then

using BltFn = HRESULT(__stdcall*)(IDirectDrawSurface* self, RECT* dst, IDirectDrawSurface* src, RECT* src_rect,
                                  DWORD flags, DDBLTFX* fx);
BltFn g_orig_blt;

// ---- Scaling on the graphics card: Direct3D 9's StretchRect filters smoothly without shaders ----
struct Scaler {
    IDirect3D9* d3d;
    IDirect3DDevice9* device;
    IDirect3DSurface9* frame;  // the game's 800x600 picture, copied in with GDI
    HWND wnd;
    // Direct3D draws into a window of ours over the game window's client area: what it presents to the game window
    // itself never reaches the screen on some computers (like DirectDraw's own present). The mouse goes through it.
    HWND child;
    SIZE size;                 // the back buffer: the window's client area
    DWORD retry_at;
    int frames, failures;
};
Scaler g_scaler;
const char* g_path = "none";  // how the last frame went out: copy, direct3d, gdi
int g_partial;                // Blts of less than a frame (the custom cursor), left out

LRESULT CALLBACK ChildProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCHITTEST)
        return HTTRANSPARENT;  // the mouse goes to the game window underneath
    if (msg == WM_ERASEBKGND)
        return 1;
    return DefWindowProcW(wnd, msg, wp, lp);
}

HWND MakeChild(HWND parent, SIZE size)
{
    static bool registered;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = ChildProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PeggleNetplayPicture";
        RegisterClassW(&wc);
        registered = true;
    }
    return CreateWindowExW(0, L"PeggleNetplayPicture", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, size.cx,
                           size.cy, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}

void ReleaseScaler()
{
    if (g_scaler.frame)
        g_scaler.frame->Release();
    if (g_scaler.device)
        g_scaler.device->Release();
    if (g_scaler.child)
        DestroyWindow(g_scaler.child);
    g_scaler.frame = nullptr;
    g_scaler.device = nullptr;
    g_scaler.wnd = nullptr;
    g_scaler.child = nullptr;
}

UINT AdapterFor(HWND wnd)
{
    HMONITOR monitor = MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
    for (UINT i = 0; i < g_scaler.d3d->GetAdapterCount(); ++i)
        if (g_scaler.d3d->GetAdapterMonitor(i) == monitor)
            return i;
    return D3DADAPTER_DEFAULT;
}

bool CreateScaler(HWND wnd, SIZE size)
{
    if (!g_scaler.d3d) {
        HMODULE dll = LoadLibraryA("d3d9.dll");
        using CreateFn = IDirect3D9*(WINAPI*)(UINT);
        auto create = dll ? reinterpret_cast<CreateFn>(GetProcAddress(dll, "Direct3DCreate9")) : nullptr;
        g_scaler.d3d = create ? create(D3D_SDK_VERSION) : nullptr;
        if (!g_scaler.d3d)
            return false;
    }
    g_scaler.child = MakeChild(wnd, size);
    if (!g_scaler.child)
        return false;
    D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth = size.cx;
    pp.BackBufferHeight = size.cy;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;  // the desktop's
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = g_scaler.child;
    pp.Windowed = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    // Direct3D must leave the FPU as the game set it (an online duel's simulation depends on its precision) and
    // the window as it is.
    DWORD flags = D3DCREATE_FPU_PRESERVE | D3DCREATE_NOWINDOWCHANGES;
    UINT adapter = AdapterFor(wnd);
    HRESULT hr = g_scaler.d3d->CreateDevice(adapter, D3DDEVTYPE_HAL, g_scaler.child,
                                            flags | D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_scaler.device);
    if (FAILED(hr))
        hr = g_scaler.d3d->CreateDevice(adapter, D3DDEVTYPE_HAL, g_scaler.child,
                                        flags | D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &g_scaler.device);
    if (FAILED(hr)) {
        Log("present: no Direct3D 9 device (%08lX), scaling with GDI", hr);
        g_scaler.device = nullptr;
        return false;
    }
    hr = g_scaler.device->CreateRenderTarget(kGameW, kGameH, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, TRUE,
                                             &g_scaler.frame, nullptr);
    if (FAILED(hr)) {
        Log("present: no Direct3D 9 render target (%08lX), scaling with GDI", hr);
        g_scaler.frame = nullptr;
        ReleaseScaler();
        return false;
    }
    g_scaler.wnd = wnd;
    g_scaler.size = size;
    Log("present: scaling with Direct3D 9 (adapter %u) into %ldx%ld", adapter, size.cx, size.cy);
    return true;
}

bool ScaleWithDirect3D(HWND wnd, HDC src, const RECT& s, const RECT& picture, SIZE client)
{
    if (g_scaler.device &&
        (g_scaler.wnd != wnd || g_scaler.size.cx != client.cx || g_scaler.size.cy != client.cy))
        ReleaseScaler();  // the window changed size (borderless, another monitor): a new back buffer
    if (!g_scaler.device) {
        if (static_cast<int>(GetTickCount() - g_scaler.retry_at) < 0 || !CreateScaler(wnd, client)) {
            g_scaler.retry_at = GetTickCount() + kRetryMs;
            return false;
        }
    }
    HDC frame_dc;
    HRESULT hr = g_scaler.frame->GetDC(&frame_dc);
    if (SUCCEEDED(hr)) {
        BitBlt(frame_dc, 0, 0, kGameW, kGameH, src, s.left, s.top, SRCCOPY);
        g_scaler.frame->ReleaseDC(frame_dc);
        IDirect3DSurface9* back = nullptr;
        hr = g_scaler.device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
        if (SUCCEEDED(hr)) {
            g_scaler.device->ColorFill(back, nullptr, D3DCOLOR_XRGB(0, 0, 0));
            RECT whole = {0, 0, kGameW, kGameH};
            hr = g_scaler.device->StretchRect(g_scaler.frame, &whole, back, &picture, D3DTEXF_LINEAR);
            back->Release();
        }
        if (SUCCEEDED(hr))
            hr = g_scaler.device->Present(nullptr, nullptr, nullptr, nullptr);
    }
    if (FAILED(hr)) {  // lost (the screen was locked, the display changed, ...): start over in a moment
        Log("present: Direct3D 9 failed (%08lX), GDI for now", hr);
        ++g_scaler.failures;
        ReleaseScaler();
        g_scaler.retry_at = GetTickCount() + kRetryMs;
        return false;
    }
    ++g_scaler.frames;
    return true;
}

void ScaleWithGdi(HWND wnd, HDC src, const RECT& s, const RECT& picture, SIZE client)
{
    HDC dc = GetDC(wnd);
    if (!dc)
        return;
    auto black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    const RECT bars[4] = {{0, 0, picture.left, client.cy},
                          {picture.right, 0, client.cx, client.cy},
                          {picture.left, 0, picture.right, picture.top},
                          {picture.left, picture.bottom, picture.right, client.cy}};
    for (const RECT& bar : bars)
        if (bar.right > bar.left && bar.bottom > bar.top)
            FillRect(dc, &bar, black);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchBlt(dc, picture.left, picture.top, picture.right - picture.left, picture.bottom - picture.top, src, s.left,
               s.top, s.right - s.left, s.bottom - s.top, SRCCOPY);
    ReleaseDC(wnd, dc);
}

HRESULT __stdcall HookBlt(IDirectDrawSurface* self, RECT* dst, IDirectDrawSurface* src, RECT* src_rect, DWORD flags,
                          DDBLTFX* fx)
{
    void* app = game::App();
    void* dd = app ? game::At<void*>(app, game::kAppOffDDInterface) : nullptr;
    if (!dd || self != game::At<IDirectDrawSurface*>(dd, game::kDDOffPrimary) || !src || !dst ||
        !game::At<bool>(app, game::kAppOffIsWindowed))
        return g_orig_blt(self, dst, src, src_rect, flags, fx);

    HWND wnd = game::At<HWND>(dd, game::kDDOffHwnd);
    RECT s = src_rect ? *src_rect : RECT{0, 0, dst->right - dst->left, dst->bottom - dst->top};
    if (s.right - s.left != kGameW || s.bottom - s.top != kGameH) {
        // Not a frame: the custom cursor drawn straight onto the screen (the cursor thread does that while the game
        // isn't drawing), where the game supposes its frame is, unscaled. A frame with the cursor in it comes instead.
        ++g_partial;
        game::At<bool>(app, game::kAppOffCustomCursorDirty) = true;
        return DD_OK;
    }
    RECT picture;
    SIZE client;
    if (!display::PictureRect(wnd, picture, client))
        return DD_OK;  // minimized
    HDC sdc;
    HRESULT hr = src->GetDC(&sdc);
    if (FAILED(hr))
        return hr;
    if (picture.left == 0 && picture.top == 0 && client.cx == s.right - s.left && client.cy == s.bottom - s.top) {
        ReleaseScaler();  // the window is the picture's size: a plain copy
        if (HDC wdc = GetDC(wnd)) {
            BitBlt(wdc, 0, 0, client.cx, client.cy, sdc, s.left, s.top, SRCCOPY);
            ReleaseDC(wnd, wdc);
        }
        g_path = "copy";
    } else if (ScaleWithDirect3D(wnd, sdc, s, picture, client)) {
        g_path = "direct3d";
    } else {
        ScaleWithGdi(wnd, sdc, s, picture, client);
        g_path = "gdi";
    }
    src->ReleaseDC(sdc);
    return DD_OK;
}

}  // namespace

void PresentFixUpdate()
{
    if (g_orig_blt)
        return;
    void* app = game::App();
    void* dd = app ? game::At<void*>(app, game::kAppOffDDInterface) : nullptr;
    auto* primary = dd ? game::At<IDirectDrawSurface*>(dd, game::kDDOffPrimary) : nullptr;
    if (!primary)
        return;
    // All DirectDraw surfaces share this vtable; HookBlt only intervenes for the primary.
    g_orig_blt = reinterpret_cast<BltFn>(HookVtable(*reinterpret_cast<uintptr_t*>(primary), 5, HookBlt));
    Log("present: windowed present installed");
}

std::string PresentState()
{
    char buf[128];
    snprintf(buf, sizeof buf, "present=%s d3dframes=%d d3dfailures=%d partial=%d", g_path, g_scaler.frames,
             g_scaler.failures, g_partial);
    return buf;
}
