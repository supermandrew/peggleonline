#include "online.h"

#include <windows.h>

#include <cstring>
#include <thread>

#include "arena.h"
#include "chaos.h"
#include "chat.h"
#include "config.h"
#include "dialogs.h"
#include "game.h"
#include "hook.h"
#include "lockstep.h"
#include "log.h"
#include "net.h"
#include "protocol.h"
#include "upnp.h"

namespace online {

namespace {

enum class Phase {
    None,
    Hosting,      // listening for the opponent
    Joining,      // connecting to the host
    Handshake,    // connected, waiting for the other side's Hello
    HostLobby,    // host is choosing a level
    ClientLobby,  // joiner waits for the host's level
    InGame,
};

enum DialogId {
    kDlgMode = 9001,
    kDlgRole,
    kDlgAddress,
    kDlgHosting,
    kDlgJoining,
    kDlgWaitLevel,
    kDlgNotice,
    kDlgPortWarning,
};

Phase g_phase = Phase::None;
bool g_is_host;
bool g_upnp_reported;
std::string g_peer_name;
std::string g_join_text;
void* g_main_menu;  // MainMenu ButtonListener sub-object, captured when Duel is pressed
DWORD g_last_ping;
uint32_t g_peer_clock, g_peer_clock_seen;  // opponent's update counter from their Hello; ours when it arrived

using ButtonDepressFn = void(__thiscall*)(void* listener, int id);
ButtonDepressFn g_orig_main_menu_button;
ButtonDepressFn g_orig_level_screen_button;
ButtonDepressFn g_orig_options_button;
using NewBoardFn = void(__thiscall*)(void* app, int a, int b);
NewBoardFn g_orig_new_board;

constexpr uintptr_t kLevelScreenButtonListenerVtable = 0x68D5A0;
constexpr int kLevelScreenButtonMainMenu = 100;
constexpr uintptr_t kOptionsButtonListenerVtable = 0x68F0E0;  // the in-game Menu
constexpr int kOptionsButtonMainMenu = 9;
constexpr int kLeaveGameDialog = 4;  // "Leave Game?", opened by the Menu's Main Menu button
constexpr uint32_t kChatFromTick = 20;  // the online board's screen transition lasts this long (lockstep.cpp)

// Profile fields that feed board creation (the duel's last masters, "play the computer", its difficulty).
// They differ between the two players' profiles, so both games use the same values during an online duel
// and the player's own values are put back afterwards.
constexpr int kProfileDuelFields[] = {0x3C, 0x40, 0x48, 0x4C};
int g_saved_profile[4];
std::string g_saved_player2_name;  // profile +0xB4: the duel copies the opponent's name there
bool g_profile_saved;
constexpr int kProfileOffPlayer2Name = 0xB4;

std::string g_name_override;  // testing: two copies on one PC share a profile

std::string ProfileName()
{
    if (!g_name_override.empty())
        return g_name_override;
    void* profile = game::At<void*>(game::App(), game::kAppOffProfile);
    std::string name = profile ? game::ReadString(static_cast<char*>(profile) + 4) : std::string();
    return name.empty() ? "Player" : name;
}

void NormalizeProfile()
{
    void* profile = game::At<void*>(game::App(), game::kAppOffProfile);
    if (!profile)
        return;
    if (!g_profile_saved)
        g_saved_player2_name = game::ReadString(static_cast<char*>(profile) + kProfileOffPlayer2Name);
    for (int i = 0; i < 4; ++i) {
        if (!g_profile_saved)
            g_saved_profile[i] = game::At<int>(profile, kProfileDuelFields[i]);
        game::At<int>(profile, kProfileDuelFields[i]) = 0;
    }
    g_profile_saved = true;
}

void RestoreProfile()
{
    void* profile = game::At<void*>(game::App(), game::kAppOffProfile);
    if (!profile || !g_profile_saved)
        return;
    for (int i = 0; i < 4; ++i)
        game::At<int>(profile, kProfileDuelFields[i]) = g_saved_profile[i];
    game::AssignString(static_cast<char*>(profile) + kProfileOffPlayer2Name, g_saved_player2_name.c_str());
    g_profile_saved = false;
}

void CreateBoardSynced(void* app) { g_orig_new_board(app, 1, 0); }

// Both games build the duel board from the same level and seed, then run it in lockstep.
void StartSyncedBoard(void* app, const proto::Level& lv)
{
    NormalizeProfile();
    game::At<int>(app, game::kAppOffGameMode) = game::kModeDuel;
    game::At<int>(app, game::kAppOffStage) = lv.stage;
    game::At<int>(app, game::kAppOffLevel) = lv.level;
    lockstep::SeedAndRunSynced(lv.seed, lv.fpu_control, lv.clock, CreateBoardSynced, app);
    void* board = game::At<void*>(app, game::kAppOffBoard);
    std::string me = ProfileName();
    lockstep::Begin(board, g_is_host, g_is_host ? me.c_str() : g_peer_name.c_str(),
                    g_is_host ? g_peer_name.c_str() : me.c_str());
    chat::SetNames(me, g_peer_name);
}

void SendHello()
{
    proto::Hello h = {};
    h.version = proto::kVersion;
    h.game_timestamp = game::kTimestamp;
    strncpy(h.name, ProfileName().c_str(), sizeof h.name - 1);
    h.clock = game::At<uint32_t>(game::App(), game::kAppOffUpdateCount);
    net::Send(proto::kHello, &h, sizeof h);
}

// The duel's clock starts at the same value on both computers and must not run backwards on either (screens
// still animating would stall), so start above both counters. Both count 100 updates a second.
uint32_t DuelClockStart()
{
    uint32_t ours = game::At<uint32_t>(game::App(), game::kAppOffUpdateCount);
    uint32_t theirs = g_peer_clock + (ours - g_peer_clock_seen);
    return (ours > theirs ? ours : theirs) + 360000;  // an hour of slack
}

void CloseSessionDialogs()
{
    for (int id : {kDlgHosting, kDlgJoining, kDlgWaitLevel, kDlgPortWarning})
        dialogs::Close(id);
}

// End the session. `notice` (if any) is shown to the player afterwards.
void Leave(const std::string& notice, bool tell_peer)
{
    if (g_phase == Phase::None)
        return;
    Log("online: leaving session (%s)", notice.empty() ? "by choice" : notice.c_str());
    if (tell_peer)
        net::Send(proto::kBye);
    net::Shutdown();
    lockstep::End();
    chat::End();
    RestoreProfile();
    if (g_is_host)
        std::thread(upnp::RemoveMapping).detach();
    CloseSessionDialogs();
    g_phase = Phase::None;
    if (!notice.empty())
        dialogs::Message(kDlgNotice, "ONLINE DUEL", notice, "OK");
}

void StartHosting()
{
    uint16_t port = config::Port();
    g_is_host = true;
    g_upnp_reported = false;
    g_phase = Phase::Hosting;
    net::StartHost(port);
    upnp::StartMapping(port);
    dialogs::Message(kDlgHosting, "HOST GAME",
                     "Opening port " + std::to_string(port) + " on your router...\n\n\n", "CANCEL");
}

void StartJoining(const std::string& host, uint16_t port)
{
    g_is_host = false;
    g_phase = Phase::Joining;
    net::StartJoin(host, port);
    dialogs::Message(kDlgJoining, "JOIN GAME", "Connecting to\n" + g_join_text + "...", "CANCEL");
}

std::string AddressForOpponent(const upnp::Result& r, uint16_t port)
{
    std::string ip = !r.external_ip.empty() ? r.external_ip : r.local_ip;
    if (ip.empty())
        return "this computer's address";
    return port == net::kDefaultPort ? ip : ip + ":" + std::to_string(port);
}

bool CopyToClipboard(const std::string& text)
{
    if (!OpenClipboard(nullptr))
        return false;
    EmptyClipboard();
    bool ok = false;
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1)) {
        memcpy(GlobalLock(mem), text.c_str(), text.size() + 1);
        GlobalUnlock(mem);
        ok = SetClipboardData(CF_TEXT, mem) != nullptr;
        if (!ok)
            GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

void UpdateHosting()
{
    upnp::Result r = upnp::GetResult();
    if (!r.done || g_upnp_reported)
        return;
    g_upnp_reported = true;
    uint16_t port = config::Port();
    std::string address = AddressForOpponent(r, port);
    bool copied = CopyToClipboard(address);
    std::string copied_note = copied ? "\n(copied, ready to paste)" : "";
    if (r.mapped) {
        Log("online: port %u opened, external address %s", port, r.external_ip.c_str());
        dialogs::SetText(kDlgHosting, "Waiting for your opponent...\n\nTell them to join:\n^FFFF00^" + address +
                                          "^oldclr^" + copied_note);
        return;
    }
    Log("online: port mapping failed: %s", r.error.c_str());
    dialogs::SetText(kDlgHosting, "Waiting for your opponent...\n\nThis computer's address:\n^FFFF00^" + address +
                                      "^oldclr^" + copied_note);
    dialogs::Message(kDlgPortWarning, "PORT NOT OPENED",
                     r.error + "\n\nYour opponent may not be able to join over the internet. Turn on UPnP in your "
                               "router's settings, or forward TCP port " +
                         std::to_string(port) + " to this computer (" + r.local_ip + "). Or let your opponent host.",
                     "OK");
}

void OnHello(const net::Message& m)
{
    proto::Hello h = {};
    memcpy(&h, m.data.data(), m.data.size() < sizeof h ? m.data.size() : sizeof h);
    h.name[sizeof h.name - 1] = 0;
    if (h.version != proto::kVersion || h.game_timestamp != game::kTimestamp) {
        Leave("Your opponent has a different version of the online mod or the game. Both players need the same "
              "version.",
              true);
        return;
    }
    g_peer_name = h.name[0] ? h.name : "Your opponent";
    g_peer_clock = h.clock;
    g_peer_clock_seen = game::At<uint32_t>(game::App(), game::kAppOffUpdateCount);
    Log("online: opponent is \"%s\" at %s", g_peer_name.c_str(), net::PeerAddress().c_str());
    CloseSessionDialogs();
    if (g_is_host) {
        g_phase = Phase::HostLobby;
        game::ShowLevelScreenFromMainMenu(g_main_menu);
    } else {
        g_phase = Phase::ClientLobby;
        dialogs::Message(kDlgWaitLevel, "CONNECTED",
                         "Connected to " + g_peer_name + "!\n\nWaiting for them to pick a level...", "LEAVE");
    }
}

std::string OpponentLeftText() { return g_peer_name + " left the game."; }

void ShowWaitForLevel(const char* header)
{
    dialogs::Message(kDlgWaitLevel, header, "Playing with " + g_peer_name + ".\n\nWaiting for them to pick a level...",
                     "LEAVE");
}

// Host chose "Play New Level" on the results screen: the joiner waits on the main menu, the host goes on
// to the level screen (the game does that right after this).
void OnHostNewLevel()
{
    net::Send(proto::kNewLevel);
    g_phase = Phase::HostLobby;
}

void OnNewLevel()
{
    if (g_is_host || g_phase != Phase::InGame)
        return;
    lockstep::End();
    g_phase = Phase::ClientLobby;
    game::ShowMainMenu(game::App());
    ShowWaitForLevel("NEXT LEVEL");
}

void OnLevel(const net::Message& m)
{
    if (g_is_host || g_phase != Phase::ClientLobby || m.data.size() < sizeof(proto::Level))
        return;
    proto::Level lv;
    memcpy(&lv, m.data.data(), sizeof lv);
    Log("online: host picked stage %d level %d (seed %08X, clock %u, ours %u)", lv.stage, lv.level, lv.seed, lv.clock,
        game::At<uint32_t>(game::App(), game::kAppOffUpdateCount));
    dialogs::Close(kDlgWaitLevel);
    arena::BeginDuel(lv.arena_start);
    g_phase = Phase::InGame;
    StartSyncedBoard(game::App(), lv);
}

void OnDialogResult(int id, int button)
{
    switch (id) {
    case kDlgMode:
        dialogs::Close(kDlgMode);
        if (button == dialogs::kChoiceA) {
            g_orig_main_menu_button(g_main_menu, game::kMainMenuButtonDuel);  // the game's own local duel
        } else if (button == dialogs::kChoiceB) {
            dialogs::Choice(kDlgRole, "ONLINE DUEL", "Host a game for your opponent,\nor join their game.",
                            "HOST GAME", "JOIN GAME", "CANCEL");
        }
        break;
    case kDlgRole:
        dialogs::Close(kDlgRole);
        if (button == dialogs::kChoiceA)
            StartHosting();
        else if (button == dialogs::kChoiceB)
            dialogs::TextEntry(kDlgAddress, "JOIN GAME", config::LastAddress());
        break;
    case kDlgAddress: {
        if (button != dialogs::kFooter) {
            dialogs::Close(kDlgAddress);
            break;
        }
        std::string text = dialogs::TextEntryValue(kDlgAddress);
        std::string host;
        uint16_t port;
        if (!net::ParseAddress(text, host, port))
            break;  // empty: leave the box open
        dialogs::Close(kDlgAddress);
        config::SetLastAddress(text);
        g_join_text = text;
        StartJoining(host, port);
        break;
    }
    case kDlgHosting:
    case kDlgJoining:
    case kDlgWaitLevel:
        Leave("", true);
        break;
    default:
        dialogs::Close(id);
        break;
    }
}

bool DuelLocked()
{
    void* profile = game::At<void*>(game::App(), game::kAppOffProfile);
    return profile && game::At<int>(profile, 0x2C) < 1;
}

void __fastcall HookMainMenuButton(void* listener, void*, int id)
{
    bool menu_busy = game::At<uint8_t>(listener, 0xAA) != 0;  // the original ignores presses while this is set
    if (id == game::kMainMenuButtonDuel && !menu_busy && !DuelLocked() && g_phase == Phase::None) {
        g_main_menu = listener;
        dialogs::Choice(kDlgMode, "DUEL", "How would you like to play?", "LOCAL PLAY", "ONLINE PLAY", "CANCEL");
        return;
    }
    g_orig_main_menu_button(listener, id);
}

void __fastcall HookLevelScreenButton(void* listener, void*, int id)
{
    g_orig_level_screen_button(listener, id);
    if (id == kLevelScreenButtonMainMenu && g_phase == Phase::HostLobby)
        Leave("", true);
}

void __fastcall HookOptionsButton(void* listener, void*, int id)
{
    g_orig_options_button(listener, id);
    if (id == kOptionsButtonMainMenu && g_phase == Phase::InGame)  // online duels aren't saved for later
        dialogs::SetText(kLeaveGameDialog, "Leaving ends the online duel for both players.  Do you want to continue?");
}

// The level screen's call to ThunderballApp::NewBoard_Other once a level is chosen.
void __fastcall HookLevelChosen(void* app, void*, int a, int b)
{
    if (g_phase == Phase::HostLobby && game::At<int>(app, game::kAppOffGameMode) == game::kModeDuel) {
        proto::Level lv = {};
        lv.stage = game::At<int>(app, game::kAppOffStage);
        lv.level = game::At<int>(app, game::kAppOffLevel);
        lv.seed = GetTickCount() ^ (GetCurrentProcessId() << 16);
        lv.fpu_control = lockstep::CurrentFpuControl();
        lv.arena_start = static_cast<uint32_t>(arena::BeginDuel(0));
        lv.clock = DuelClockStart();
        Log("online: starting stage %d level %d (seed %08X, fpu %04X, clock %u)", lv.stage, lv.level, lv.seed,
            lv.fpu_control, lv.clock);
        net::Send(proto::kLevel, &lv, sizeof lv);
        g_phase = Phase::InGame;
        StartSyncedBoard(app, lv);
        return;
    }
    g_orig_new_board(app, a, b);
}

}  // namespace

void Install()
{
    lockstep::Install();
    lockstep::SetNewLevelHandler(OnHostNewLevel);
    dialogs::Install(OnDialogResult);
    g_orig_main_menu_button = reinterpret_cast<ButtonDepressFn>(
        HookVtable(game::kMainMenuButtonListenerVtable, 2, reinterpret_cast<void*>(HookMainMenuButton)));
    g_orig_level_screen_button = reinterpret_cast<ButtonDepressFn>(
        HookVtable(kLevelScreenButtonListenerVtable, 2, reinterpret_cast<void*>(HookLevelScreenButton)));
    g_orig_options_button = reinterpret_cast<ButtonDepressFn>(  // after lockstep's hook on the same slot
        HookVtable(kOptionsButtonListenerVtable, 2, reinterpret_cast<void*>(HookOptionsButton)));
    g_orig_new_board = reinterpret_cast<NewBoardFn>(
        HookCall(game::kLevelScreenNewBoardCall, reinterpret_cast<void*>(HookLevelChosen)));
}

void Update()
{
    if (g_phase == Phase::None)
        return;
    net::State st = net::GetState();
    if (st == net::State::Closed) {
        std::string why = net::GetError();
        net::Message m;
        while (net::Receive(m))  // the opponent's goodbye arrives just before their side closes
            if (m.type == proto::kBye)
                why = OpponentLeftText();
        Leave(why.empty() ? "The connection was closed." : why, false);
        return;
    }
    if (g_phase == Phase::Hosting)
        UpdateHosting();
    if ((g_phase == Phase::Hosting || g_phase == Phase::Joining) && st == net::State::Connected) {
        g_phase = Phase::Handshake;
        g_last_ping = GetTickCount();
        SendHello();
    }
    if (st != net::State::Connected)
        return;
    net::Message m;
    while (g_phase != Phase::None && net::Receive(m)) {
        if (lockstep::HandleMessage(m) || chaos::HandleMessage(m))
            continue;
        switch (m.type) {
        case proto::kHello:
            OnHello(m);
            break;
        case proto::kBye:
            Leave(OpponentLeftText(), false);
            break;
        case proto::kNewLevel:
            OnNewLevel();
            break;
        case proto::kLevel:
            OnLevel(m);
            break;
        case proto::kChat:
            chat::OnMessage(m);
            break;
        default:
            break;
        }
    }
    if (g_phase == Phase::None)
        return;
    lockstep::Update();
    if (lockstep::TakeDesyncNotice())
        dialogs::Message(kDlgNotice, "OUT OF SYNC",
                         "The two games no longer match, so what you see may differ from what " + g_peer_name +
                             " sees.\n\nFinish this level, then pick a new one to get back in sync.",
                         "OK");
    if (g_phase == Phase::InGame && !lockstep::Active()) {
        Leave("", true);  // this player left the duel (Menu > Main Menu): let the opponent know
        return;
    }
    if (GetTickCount() - g_last_ping > 1000) {
        g_last_ping = GetTickCount();
        net::Send(proto::kPing);
    }
    if (net::MillisSinceReceive() > 15000)
        Leave("Lost contact with " + g_peer_name + ".", false);
    // Chat appears once the board is in (its first ticks are the wipe from the previous screen).
    chat::Update(g_phase == Phase::InGame && lockstep::Active() && lockstep::CurrentTick() >= kChatFromTick);
}

bool InSession() { return g_phase != Phase::None; }
void SetNameOverride(const std::string& name) { g_name_override = name; }
bool IsHost() { return g_is_host; }
std::string PlayerName(int player) { return (player == 0) == g_is_host ? ProfileName() : g_peer_name; }

}  // namespace online
