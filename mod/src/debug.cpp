#include "debug.h"

#include <windows.h>
#include <ddraw.h>

#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <sstream>
#include <vector>

#include "chaos.h"
#include "chat.h"
#include "controls.h"
#include "display.h"
#include "game.h"
#include "hook.h"
#include "keyboard.h"
#include "lockstep.h"
#include "log.h"
#include "online.h"
#include "packs.h"
#include "pad.h"
#include "padhid.h"
#include "padmenu.h"
#include "replays.h"
#include "ui.h"

namespace {

struct Request {
    std::string command;
    std::string reply;
    bool taken = false;  // the game thread is on it
    bool done = false;
};

std::mutex g_mutex;
std::condition_variable g_cv;
Request* g_pending;

// ---- A pretend mouse position (the `cursor` command): what the game reads with GetCursorPos (its every-10-ms check
// that the mouse is in the window, the cursor thread) and WindowFromPoint say, so a test needn't move the real mouse.
constexpr uintptr_t kImportGetCursorPos = 0x6703BC, kImportWindowFromPoint = 0x67035C;  // the game's imports
using GetCursorPosFn = BOOL(WINAPI*)(POINT*);
using WindowFromPointFn = HWND(WINAPI*)(POINT);
GetCursorPosFn g_real_get_cursor_pos;
WindowFromPointFn g_real_window_from_point;
volatile bool g_pretend_cursor;
POINT g_cursor;  // on the screen
HWND g_cursor_window;

BOOL WINAPI PretendGetCursorPos(POINT* p)
{
    if (!g_pretend_cursor)
        return g_real_get_cursor_pos(p);
    *p = g_cursor;
    return TRUE;
}

HWND WINAPI PretendWindowFromPoint(POINT p)
{
    if (g_pretend_cursor && p.x == g_cursor.x && p.y == g_cursor.y)
        return g_cursor_window;  // as if nothing covered the game there
    return g_real_window_from_point(p);
}

void PretendCursor(HWND wnd, int x, int y)
{
    if (!g_real_get_cursor_pos) {
        g_real_get_cursor_pos = *reinterpret_cast<GetCursorPosFn*>(kImportGetCursorPos);
        g_real_window_from_point = *reinterpret_cast<WindowFromPointFn*>(kImportWindowFromPoint);
        GetCursorPosFn get = PretendGetCursorPos;
        WindowFromPointFn from = PretendWindowFromPoint;
        PatchBytes(kImportGetCursorPos, &get, 4);
        PatchBytes(kImportWindowFromPoint, &from, 4);
    }
    POINT p = {x, y};
    ClientToScreen(wnd, &p);
    g_cursor_window = wnd;
    g_cursor = p;
    g_pretend_cursor = true;
}

// Runs a command on the game thread. Mouse input is answered first and done in `later`: a click can open one of
// the game's dialogs that loops until it's closed, and the pipe has to keep answering meanwhile.
std::string RunCommand(const std::string& line, std::function<void()>& later);

DWORD WINAPI PipeThread(void*)
{
    char name[64];
    snprintf(name, sizeof name, "\\\\.\\pipe\\peggle-netplay-%lu", GetCurrentProcessId());
    for (;;) {
        HANDLE pipe = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Log("debug: CreateNamedPipe failed %lu", GetLastError());
            return 0;
        }
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            char buf[1024];
            DWORD got = 0;
            if (ReadFile(pipe, buf, sizeof buf - 1, &got, nullptr) && got) {
                buf[got] = 0;
                std::string line(buf);
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                    line.pop_back();
                Request req;
                req.command = line;
                {
                    std::unique_lock<std::mutex> lock(g_mutex);
                    g_pending = &req;
                    if (!g_cv.wait_for(lock, std::chrono::seconds(10), [&] { return req.taken; })) {
                        g_pending = nullptr;
                        req.reply = "error: game thread did not respond";
                    } else {
                        g_cv.wait(lock, [&] { return req.done; });
                    }
                }
                DWORD wrote;
                WriteFile(pipe, req.reply.data(), static_cast<DWORD>(req.reply.size()), &wrote, nullptr);
                FlushFileBuffers(pipe);
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

// Windows messages posted to an unfocused game window are ignored, so tests feed input to the WidgetManager.
void DumpWidget(void* w, int depth, std::string& out)
{
    ui::Rect r = ui::ScreenRect(w);
    char line[256];
    snprintf(line, sizeof line, "%*s%s %d,%d %dx%d z%d %s%s%s", depth * 2, "", ui::ClassName(w).c_str(), r.x, r.y, r.w,
             r.h, game::At<int>(w, game::kWidgetOffZOrder), game::At<uint8_t>(w, 0x54) ? "v" : "-",
             game::At<uint8_t>(w, game::kWidgetOffMouseVisible) ? "m" : "-", game::At<uint8_t>(w, 0x56) ? "d" : "-");
    out += line;
    if (ui::IsA(w, "ButtonWidget"))
        out += " id=" + std::to_string(game::At<int>(w, 0x88)) + " \"" +
               game::ReadString(static_cast<char*>(w) + game::kButtonOffLabel) + "\"";
    out += "\n";
    for (void* c : ui::Children(w))
        DumpWidget(c, depth + 1, out);
}

std::string RunCommand(const std::string& line, std::function<void()>& later)
{
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "ping")
        return "pong";
    if (cmd == "move" || cmd == "click" || cmd == "down" || cmd == "up") {
        int x = 0, y = 0, clicks = 1;
        in >> x >> y >> clicks;
        pad::NoteMouseUsed();  // this is the mouse
        later = [=] {
            if (cmd == "move" || cmd == "click")
                ui::MouseMove(x, y);
            if (cmd == "down" || cmd == "click")
                ui::MouseDown(x, y, clicks);
            if (cmd == "up" || cmd == "click")
                ui::MouseUp(x, y, clicks);
        };
        return "ok";
    }
    if (cmd == "widgets") {  // everything on screen, front last, with screen rects and flags (visible, mouse, disabled)
        std::string out;
        for (void* w : ui::Children(ui::WidgetManager()))
            DumpWidget(w, 0, out);
        return out;
    }
    // pad [n] <A+LB|-> [lx ly rx ry]: pretend controller n (1, the default, or 2...) in that state;
    // pad n off: unplug controller n; pad off: the real controllers again
    if (cmd == "pad") {
        std::string buttons;
        float lx = 0, ly = 0, rx = 0, ry = 0;
        in >> buttons;
        int controller = 0;
        if (buttons.size() == 1 && buttons[0] >= '1' && buttons[0] <= '4') {
            controller = buttons[0] - '1';
            in >> buttons;
        }
        in >> lx >> ly >> rx >> ry;
        if (buttons == "off") {
            if (controller)
                pad::Unplug(controller);
            else
                pad::EndTest();
            return "ok";
        }
        uint32_t bits = 0;
        size_t start = 0;
        while (buttons != "-" && start <= buttons.size()) {
            size_t end = buttons.find('+', start);
            if (end == std::string::npos)
                end = buttons.size();
            int b = pad::ButtonByKey(buttons.substr(start, end - start));
            if (b < 0)
                return "error: no button " + buttons.substr(start, end - start);
            bits |= 1u << b;
            start = end + 1;
        }
        pad::SetTestState(controller, bits, lx, ly, rx, ry);
        return "ok";
    }
    if (cmd == "hidreport") {  // hidreport <ds4|ds5> <input report length> <hex>: what a controller's report says
        std::string kind, hex;
        size_t length = 0;
        in >> kind >> length >> hex;
        std::vector<uint8_t> report;
        for (size_t i = 0; i + 1 < hex.size(); i += 2)
            report.push_back(static_cast<uint8_t>(strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
        padhid::State s;
        if (report.empty() || !padhid::ParseReport(report.data(), report.size(), kind == "ds5", length, s))
            return "none";
        char buf[80];
        snprintf(buf, sizeof buf, "buttons=%X sticks=%d,%d,%d,%d triggers=%d,%d", s.buttons, s.lx, s.ly, s.rx, s.ry,
                 s.l2, s.r2);
        return buf;
    }
    if (cmd == "hidmotors") {  // hidmotors <controller> <heavy> <light>: a PlayStation controller's motors, 0..255
        int controller = 0, heavy = 0, light = 0;
        in >> controller >> heavy >> light;
        padhid::SetMotors(controller, static_cast<uint8_t>(heavy), static_cast<uint8_t>(light));
        return "ok";
    }
    if (cmd == "padstate")
        return controls::DebugState() + " " + pad::DebugState() + " " + padmenu::DebugState() + " keyboard:" +
               keyboard::DebugState();
    if (cmd == "hit") {  // hit <x> <y>: what a click there would reach
        int x = 0, y = 0;
        in >> x >> y;
        void* w = ui::WidgetAt(x, y);
        return w ? ui::ClassName(w) : "none";
    }
    if (cmd == "dialog") {  // dialog <id>: is that dialog open?
        int id = 0;
        in >> id;
        return game::GetDialog(id) ? "open" : "closed";
    }
    if (cmd == "findstr") {  // findstr <dialog id> <text>: offsets of std::strings holding text in a dialog
        int id = 0;
        std::string text;
        in >> id;
        std::getline(in >> std::ws, text);
        char* dlg = static_cast<char*>(game::GetDialog(id));
        if (!dlg)
            return "error: no such dialog";
        std::string found;
        for (int off = 0; off < 0x400; off += 4) {
            auto* s = reinterpret_cast<game::GameString*>(dlg + off);
            if (s->size == text.size() && s->capacity >= s->size && s->capacity < 0x1000 &&
                memcmp(s->c_str(), text.data(), text.size()) == 0)
                found += " " + std::to_string(off);
        }
        return found.empty() ? "none" : "offsets:" + found;
    }
    if (cmd == "packthrottle") {  // packthrottle <ms>: a pause after each piece of a level pack, to watch it go
        int ms = 0;
        in >> ms;
        packs::SetThrottle(ms);
        return "ok";
    }
    if (cmd == "packcorrupt") {  // packcorrupt <0|1>: damage level packs on their way out
        int on = 0;
        in >> on;
        packs::SetCorrupt(on != 0);
        return "ok";
    }
    if (cmd == "profilename") {  // profilename <name>: the player's real name (two copies here share a profile)
        std::string name;
        std::getline(in >> std::ws, name);
        void* profile = game::App() ? game::At<void*>(game::App(), game::kAppOffProfile) : nullptr;
        if (!profile)
            return "error: no profile";
        game::AssignString(static_cast<char*>(profile) + 4, name.c_str());
        return "ok";
    }
    if (cmd == "hidelevels") {  // hidelevels <stage>: tell the opponent this game stops before that stage
        int stage = -1;
        in >> stage;
        online::HideLevelsFromStage(stage);
        return "ok";
    }
    if (cmd == "setname") {
        std::string name;
        std::getline(in >> std::ws, name);
        online::SetNameOverride(name);
        return "ok";
    }
    if (cmd == "state")
        return lockstep::DebugState();
    if (cmd == "display")  // window mode, sizes, the picture's place, the mouse mapping, how frames go out
        return display::DebugState();
    if (cmd == "winmouse") {  // winmouse <x> <y>: the mouse moved to this point of the window, as Windows says it
        int x = 0, y = 0;
        in >> x >> y;
        HWND wnd = game::App() ? game::At<HWND>(game::App(), 0x350) : nullptr;
        if (!wnd)
            return "error: no window";
        SendMessageW(wnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
        return "ok";
    }
    // cursor <x> <y>: the game reads the mouse at that point of its window's client area (and nothing covering it);
    // cursor off: where the real one is
    if (cmd == "cursor") {
        std::string word;
        in >> word;
        if (word == "off") {
            g_pretend_cursor = false;
            return "ok";
        }
        int x = atoi(word.c_str()), y = 0;
        in >> y;
        HWND wnd = game::App() ? game::At<HWND>(game::App(), 0x350) : nullptr;
        if (!wnd)
            return "error: no window";
        PretendCursor(wnd, x, y);
        return "ok";
    }
    // peek <app|wm|board|dd|profile|0x address> <hex offset> [count]: ints from a game object, for reverse
    // engineering (peekf: floats)
    if (cmd == "peek" || cmd == "peekf") {
        std::string base;
        int off = 0, count = 1;
        in >> base >> std::hex >> off >> std::dec >> count;
        void* app = game::App();
        void* p = base.rfind("0x", 0) == 0 ? reinterpret_cast<void*>(strtoul(base.c_str(), nullptr, 16))
                : !app ? nullptr
                : base == "app" ? app
                : base == "wm" ? ui::WidgetManager()
                : base == "board" ? game::At<void*>(app, game::kAppOffBoard)
                : base == "dd" ? game::At<void*>(app, game::kAppOffDDInterface)
                : base == "profile" ? game::At<void*>(app, game::kAppOffProfile)
                : nullptr;
        if (!p)
            return "error: no " + base;
        std::string out;
        char num[32];
        for (int i = 0; i < count && i < 64; ++i) {
            if (cmd == "peekf")
                snprintf(num, sizeof num, "%g", game::At<float>(p, off + 4 * i));
            else
                snprintf(num, sizeof num, "%d", game::At<int>(p, off + 4 * i));
            out += (i ? " " : "") + std::string(num);
        }
        return out;
    }
    if (cmd == "poke") {  // poke <app|board|logic|0x address> <hex offset> <int>: change a game object, for tests
        std::string base;
        int off = 0, value = 0;
        in >> base >> std::hex >> off >> std::dec >> value;
        void* app = game::App();
        void* board = app ? game::At<void*>(app, game::kAppOffBoard) : nullptr;
        void* p = base.rfind("0x", 0) == 0 ? reinterpret_cast<void*>(strtoul(base.c_str(), nullptr, 16))
                : base == "app" ? app
                : base == "board" ? board
                : base == "logic" && board ? game::At<void*>(board, 0x720)
                : base == "profile" && app ? game::At<void*>(app, game::kAppOffProfile)
                : nullptr;
        if (!p)
            return "error: no " + base;
        game::At<int>(p, off) = value;
        return "ok";
    }
    if (cmd == "objs") {  // objs [id]: the board's objects but pegs, or those with that id from the level
        std::string id;
        in >> id;
        return chaos::DebugObjects(id);
    }
    if (cmd == "board") {  // the board in play, online or not: logic state, whose turn, logic steps run so far
        void* board = game::App() ? game::At<void*>(game::App(), game::kAppOffBoard) : nullptr;
        void* lm = board ? game::At<void*>(board, 0x720) : nullptr;
        if (!lm)
            return "none";
        int picked[2] = {-1, -1};  // on the master selection: each player's choice
        if (char* sel = static_cast<char*>(game::GetDialog(1))) {
            for (int p = 0; p < 2; ++p) {
                char* begin = game::At<char*>(sel + 0x22C + p * 0x10, 4);
                char* end = game::At<char*>(sel + 0x22C + p * 0x10, 8);
                for (int i = 0; begin && begin + i * 0xC < end; ++i)
                    if (begin[i * 0xC + 4])
                        picked[p] = i;
            }
        }
        char buf[240];
        snprintf(buf, sizeof buf,
                 "mode=%d state=%d player=%d twoplayer=%d computer=%d,%d balls=%d,%d steps=%u replay=%d picked=%d,%d",
                 game::At<int>(game::App(), game::kAppOffGameMode), game::At<int>(lm, 0x04), game::At<int>(lm, 0x1AC),
                 game::At<uint8_t>(lm, 0x1A8), game::At<uint8_t>(lm, 0x2D8), game::At<uint8_t>(lm, 0x2D9),
                 game::At<int>(lm, 0x208), game::At<int>(lm, 0x20C), game::At<uint32_t>(board, 0x794),
                 game::At<uint8_t>(lm, 0x101), picked[0], picked[1]);
        return buf;
    }
    if (cmd == "chat")
        return chat::DebugState();
    if (cmd == "chaos") {  // chaos [next <effects>|now <effect>|box|names]
        std::string rest;
        std::getline(in >> std::ws, rest);
        return chaos::DebugCommand(rest);
    }
    if (cmd == "replay") {  // replay [save|button]
        std::string rest;
        std::getline(in >> std::ws, rest);
        return replays::DebugCommand(rest);
    }
    if (cmd == "chatrow") {  // chatrow <i>: where phrase i is in the open chat list
        int row = -1, x = 0, y = 0;
        in >> row;
        if (!chat::RowCenter(row, x, y))
            return "error: the chat list is not open";
        return std::to_string(x) + " " + std::to_string(y);
    }
    if (cmd == "dump") {  // dump <path>: raw memory of the online board and its main objects, for diffing
        std::string path;
        std::getline(in >> std::ws, path);
        return lockstep::DumpBoard(path) ? "ok" : "error: no online board";
    }
    if (cmd == "tick")
        return std::to_string(lockstep::CurrentTick());
    if (cmd == "hold") {  // hold <tick>: pause the duel there; hold with no tick: run again
        uint32_t tick = 0xFFFFFFFF;
        in >> tick;
        lockstep::HoldAt(tick);
        return "ok";
    }
    if (cmd == "settext") {  // settext <dialog id> <text>: fill a text-entry dialog
        int id = 0;
        std::string text;
        in >> id;
        std::getline(in >> std::ws, text);
        void* dlg = game::GetDialog(id);
        if (!dlg)
            return "error: no such dialog";
        void* edit = game::At<void*>(dlg, game::kEditDialogOffEdit);
        game::GameString s(text.c_str());
        using SetText = void(__thiscall*)(void*, game::GameString*, int);
        reinterpret_cast<SetText>((*reinterpret_cast<void***>(edit))[game::kEditSlotSetText])(edit, &s, 1);
        return "ok";
    }
    if (cmd == "shot") {  // shot <surface offset in DDInterface, hex> <path>
        int off = 0;
        std::string path;
        in >> std::hex >> off;
        std::getline(in >> std::ws, path);
        return SaveSurfaceBmp(off ? off : game::kDDOffDrawSurface, path) ? "ok" : "error: capture failed (see log)";
    }
    return "error: unknown command";
}

}  // namespace

bool DebugEnabled()
{
    static bool enabled = GetFileAttributesA((ModDir() + "mods\\debug.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
    return enabled;
}

void DebugStart()
{
    if (!DebugEnabled())
        return;
    Log("debug: pipe enabled");
    CloseHandle(CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr));
}

void DebugPump()
{
    Request* req;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        req = g_pending;
        if (!req)
            return;
        g_pending = nullptr;
        req->taken = true;
    }
    std::function<void()> later;
    std::string reply = RunCommand(req->command, later);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        req->reply = reply;
        req->done = true;
    }
    g_cv.notify_all();
    if (later)
        later();
}

bool SaveFrameBmp(const std::string& path)
{
    return SaveSurfaceBmp(game::kDDOffDrawSurface, path);
}

bool SaveSurfaceBmp(int dd_offset, const std::string& path)
{
    void* app = game::App();
    void* dd = app ? game::At<void*>(app, game::kAppOffDDInterface) : nullptr;
    // The game holds the original IDirectDrawSurface interface (DDSURFACEDESC, not DDSURFACEDESC2).
    auto* surf = dd ? game::At<IDirectDrawSurface*>(dd, dd_offset) : nullptr;
    if (!surf) {
        Log("shot: no surface at DDInterface+%X (app %p, dd %p)", dd_offset, app, dd);
        return false;
    }
    DDSURFACEDESC desc = {};
    desc.dwSize = sizeof desc;
    HRESULT hr = surf->GetSurfaceDesc(&desc);
    if (FAILED(hr)) {
        Log("shot: GetSurfaceDesc failed %08lX", hr);
        return false;
    }
    int w = static_cast<int>(desc.dwWidth), h = static_cast<int>(desc.dwHeight);
    Log("shot: surface +%X %dx%d caps %08lX bpp %lu", dd_offset, w, h, desc.ddsCaps.dwCaps,
        desc.ddpfPixelFormat.dwRGBBitCount);

    HDC sdc;
    hr = surf->GetDC(&sdc);
    if (FAILED(hr)) {
        Log("shot: GetDC failed %08lX", hr);
        return false;
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;  // bottom-up, as BMP files store it
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    void* bits = nullptr;
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP dib = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mdc, dib);
    BitBlt(mdc, 0, 0, w, h, sdc, 0, 0, SRCCOPY);
    surf->ReleaseDC(sdc);
    GdiFlush();

    int stride = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
    fh.bfSize = fh.bfOffBits + stride * h;
    bool ok = false;
    if (FILE* f = fopen(path.c_str(), "wb")) {
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f);
        fwrite(bits, stride, h, f);
        fclose(f);
        ok = true;
    } else {
        Log("shot: cannot write %s", path.c_str());
    }
    SelectObject(mdc, old);
    DeleteObject(dib);
    DeleteDC(mdc);
    return ok;
}
