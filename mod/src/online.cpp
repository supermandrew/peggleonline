#include "online.h"

#include <windows.h>

#include <cstring>
#include <set>
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
#include "packs.h"
#include "protocol.h"
#include "upnp.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

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
    kDlgPackWait,      // host: asking, then sending
    kDlgPackOffer,     // joiner: do you want it?
    kDlgPackProgress,  // joiner: downloading
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
constexpr int kLevelScreenButtonMainMenu = 100, kLevelScreenButtonRandom = 0x67;
constexpr uintptr_t kOptionsButtonListenerVtable = 0x68F0E0;  // the in-game Menu
constexpr int kOptionsButtonMainMenu = 9;
constexpr int kLeaveGameDialog = 4;  // "Leave Game?", opened by the Menu's Main Menu button
constexpr uint32_t kChatFromTick = 20;  // the online board's screen transition lasts this long (lockstep.cpp)

// Profile fields that feed board creation (the duel's last masters, "play the computer", its difficulty).
// They differ between the two players' profiles, so both games use the same values during an online duel
// and the player's own values are put back afterwards.
constexpr int kProfileDuelFields[] = {0x3C, 0x40, 0x48, 0x4C};
int g_saved_profile[4];
constexpr int kProfileOffDuelRules = 0x10C;  // bool: a player's first duel opens with a dialog explaining the rules
uint8_t g_saved_duel_rules;
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
    if (!g_profile_saved)
        g_saved_duel_rules = game::At<uint8_t>(profile, kProfileOffDuelRules);
    game::At<uint8_t>(profile, kProfileOffDuelRules) = 0;
    g_profile_saved = true;
}

void RestoreProfile()
{
    void* profile = game::At<void*>(game::App(), game::kAppOffProfile);
    if (!profile || !g_profile_saved)
        return;
    for (int i = 0; i < 4; ++i)
        game::At<int>(profile, kProfileDuelFields[i]) = g_saved_profile[i];
    game::At<uint8_t>(profile, kProfileOffDuelRules) = g_saved_duel_rules;
    game::AssignString(static_cast<char*>(profile) + kProfileOffPlayer2Name, g_saved_player2_name.c_str());
    g_profile_saved = false;
}

// ---- Which levels can be played together ----
// Level packs add stages, so a stage number means nothing on the other computer and a level may not be there at
// all: a board for a level that doesn't exist stays empty and black. The two games swap the names of their levels.
std::set<std::string> g_peer_levels;
bool g_peer_levels_known;
bool g_random_pick;       // the level screen's Random button is being pressed
int g_hide_from_stage = -1;  // testing: leave these stages out, as if a level pack weren't installed

std::string Lowered(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

void SendLevelList()
{
    std::string text;
    auto stages = game::LevelIds();
    int count = 0;
    for (size_t s = 0; s < stages.size(); ++s) {
        if (g_hide_from_stage >= 0 && static_cast<int>(s) >= g_hide_from_stage)
            break;
        for (const std::string& id : stages[s]) {
            text += id + "\n";
            ++count;
        }
    }
    Log("online: %d levels in %d stages here", count, static_cast<int>(stages.size()));
    net::Send(proto::kLevelList, text.data(), text.size());
}

void OnLevelList(const net::Message& m)
{
    g_peer_levels.clear();
    g_peer_levels_known = true;
    std::string line;
    for (uint8_t c : m.data) {
        if (c == '\n') {
            g_peer_levels.insert(Lowered(line));
            line.clear();
        } else {
            line += static_cast<char>(c);
        }
    }
    int missing = 0;
    for (const auto& stage : game::LevelIds())
        for (const std::string& id : stage)
            if (!g_peer_levels.count(Lowered(id)))
                ++missing;
    Log("online: %s has %d levels, %d of ours aren't among them", g_peer_name.c_str(),
        static_cast<int>(g_peer_levels.size()), missing);
}

std::string LevelIdAt(int stage, int level)
{
    auto stages = game::LevelIds();
    if (stage < 0 || level < 0 || stage >= static_cast<int>(stages.size()) ||
        level >= static_cast<int>(stages[stage].size()))
        return {};
    return stages[stage][level];
}

bool PeerHasLevel(const std::string& id) { return !g_peer_levels_known || g_peer_levels.count(Lowered(id)) != 0; }

// The Random button came up with a level the opponent doesn't have: one both have, out of those the player has
// reached, as it would have picked.
bool PickSharedLevel(void* app)
{
    void* profile = game::At<void*>(app, game::kAppOffProfile);
    int reached = profile ? game::At<int>(profile, game::kProfileOffStageReached) : 0;
    std::vector<std::pair<int, int>> shared;
    auto stages = game::LevelIds();
    for (int s = 0; s < static_cast<int>(stages.size()) && s <= reached; ++s)
        for (int l = 0; l < static_cast<int>(stages[s].size()); ++l)
            if (PeerHasLevel(stages[s][l]))
                shared.push_back({s, l});
    if (shared.empty())
        return false;
    auto pick = shared[(GetTickCount() * 2654435761u >> 8) % shared.size()];
    game::At<int>(app, game::kAppOffStage) = pick.first;
    game::At<int>(app, game::kAppOffLevel) = pick.second;
    return true;
}

void CreateBoardSynced(void* app) { g_orig_new_board(app, 1, 0); }
void StartChosenLevel(void* app);
uint32_t DuelClockStart();
void AnswerPackOffer(bool yes);
void SendPackReply(bool yes);

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

uint32_t BuildStamp()
{
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(&__ImageBase);
    return reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(dos) + dos->e_lfanew)
        ->FileHeader.TimeDateStamp;
}

// The host starts the level that's in the app's stage and level.
void StartChosenLevel(void* app)
{
    proto::Level lv = {};
    lv.stage = game::At<int>(app, game::kAppOffStage);
    lv.level = game::At<int>(app, game::kAppOffLevel);
    strncpy(lv.id, LevelIdAt(lv.stage, lv.level).c_str(), sizeof lv.id - 1);
    lv.seed = GetTickCount() ^ (GetCurrentProcessId() << 16);
    lv.fpu_control = lockstep::CurrentFpuControl();
    lv.arena_start = static_cast<uint32_t>(arena::BeginDuel(0));
    lv.clock = DuelClockStart();
    Log("online: starting %s, stage %d level %d (seed %08X, fpu %04X, clock %u)", lv.id, lv.stage, lv.level, lv.seed,
        lv.fpu_control, lv.clock);
    net::Send(proto::kLevel, &lv, sizeof lv);
    g_phase = Phase::InGame;
    StartSyncedBoard(app, lv);
}

void SendHello()
{
    proto::Hello h = {};
    h.version = proto::kVersion;
    h.game_timestamp = game::kTimestamp;
    strncpy(h.name, ProfileName().c_str(), sizeof h.name - 1);
    h.clock = game::At<uint32_t>(game::App(), game::kAppOffUpdateCount);
    h.build = BuildStamp();
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
    for (int id : {kDlgHosting, kDlgJoining, kDlgWaitLevel, kDlgPortWarning, kDlgPackWait, kDlgPackOffer,
                   kDlgPackProgress})
        dialogs::Close(id);
}

// End the session. `notice` (if any) is shown to the player afterwards.
void Leave(const std::string& notice, bool tell_peer)
{
    if (g_phase == Phase::None)
        return;
    Log("online: leaving session (%s)", notice.empty() ? "by choice" : notice.c_str());
    packs::StopSending();
    packs::StopReceiving();
    if (tell_peer)
        net::Send(proto::kBye);
    net::Shutdown();
    packs::WaitForSender();
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
    if (r.mapped || r.reachable) {
        Log("online: port %u %s, external address %s", port, r.mapped ? "opened" : "is open", r.external_ip.c_str());
        dialogs::SetText(kDlgHosting, "Waiting for your opponent...\n\nTell them to join:\n^FFFF00^" + address +
                                          "^oldclr^" + copied_note);
        return;
    }
    Log("online: port mapping failed: %s", r.error.c_str());
    const char* whose = r.external_ip.empty() ? "This computer's address:" : "Tell them to join:";
    dialogs::SetText(kDlgHosting, std::string("Waiting for your opponent...\n\n") + whose + "\n^FFFF00^" + address +
                                      "^oldclr^" + copied_note);
    dialogs::Message(kDlgPortWarning, "CHECK YOUR PORT",
                     r.error + "\n\nIf you've forwarded TCP port " + std::to_string(port) + " to this computer (" +
                         r.local_ip + ") in your router's settings, you can ignore this. If not, do that, or let "
                                      "your opponent host.",
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
    Log("online: opponent is \"%s\" at %s (build %08X, ours %08X)", g_peer_name.c_str(), net::PeerAddress().c_str(),
        h.build, BuildStamp());
    g_peer_levels.clear();
    g_peer_levels_known = false;
    SendLevelList();
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
    lv.id[sizeof lv.id - 1] = 0;
    Log("online: host picked %s, stage %d level %d (seed %08X, clock %u, ours %u)", lv.id, lv.stage, lv.level, lv.seed,
        lv.clock, game::At<uint32_t>(game::App(), game::kAppOffUpdateCount));
    if (!game::FindLevel(lv.id, lv.stage, lv.level)) {
        Leave(g_peer_name + " picked a level this game doesn't have (" + lv.id +
                  "). Both players need the same level packs to play it.",
              true);
        return;
    }
    dialogs::Close(kDlgWaitLevel);
    arena::BeginDuel(lv.arena_start);
    g_phase = Phase::InGame;
    StartSyncedBoard(game::App(), lv);
}

// ---- A level pack going over ----
// The host picked a level from a pack the joiner lacks. The joiner is asked; if they say yes the pack comes over, the
// game loads it (the joiner is on the main menu), and the level the host picked starts.
struct Offer {
    bool active = false;
    packs::Info pack;
    int stage = 0, level = 0;
} g_offer;
packs::Info g_incoming;
DWORD g_pack_text_time;

void SendPackReply(bool yes)
{
    uint8_t reply = yes ? 1 : 0;
    net::Send(proto::kPackReply, &reply, 1);
}

void OfferPack(void* app, const packs::Info& pack)
{
    g_offer.active = true;
    g_offer.pack = pack;
    g_offer.stage = game::At<int>(app, game::kAppOffStage);
    g_offer.level = game::At<int>(app, game::kAppOffLevel);
    proto::PackOffer offer = {};
    strncpy(offer.file, pack.file.c_str(), sizeof offer.file - 1);
    offer.size = pack.size;
    net::Send(proto::kPackOffer, &offer, sizeof offer);
    Log("online: offering %s (%u bytes)", pack.file.c_str(), pack.size);
    dialogs::Message(kDlgPackWait, "LEVEL PACK",
                     g_peer_name + " doesn't have this level.\n\nAsking if they want its level pack, " + pack.file +
                         " (" + packs::SizeText(pack.size) + ")...",
                     "CANCEL");
}

void OnPackOffer(const net::Message& m)
{
    if (g_is_host || g_phase != Phase::ClientLobby || m.data.size() < sizeof(proto::PackOffer))
        return;
    proto::PackOffer offer;
    memcpy(&offer, m.data.data(), sizeof offer);
    offer.file[sizeof offer.file - 1] = 0;
    g_incoming.file = offer.file;
    g_incoming.size = offer.size;
    std::string why;
    if (!packs::Acceptable(g_incoming, why)) {
        Log("online: %s's offer of %s (%u bytes) can't be taken: %s", g_peer_name.c_str(), offer.file, offer.size,
            why.c_str());
        SendPackReply(false);
        return;
    }
    Log("online: %s offers %s (%u bytes)", g_peer_name.c_str(), offer.file, offer.size);
    dialogs::YesNo(kDlgPackOffer, "LEVEL PACK",
                   g_peer_name + " picked a level from a pack you don't have:\n\n^FFFF00^" + g_incoming.file +
                       "^oldclr^ (" + packs::SizeText(g_incoming.size) +
                       ")\n\nDownload and install it? It stays in your game. Only say yes to people you trust.");
}

void AnswerPackOffer(bool yes)
{
    if (!yes) {
        Log("online: no to %s", g_incoming.file.c_str());
        SendPackReply(false);
        return;
    }
    if (!packs::StartReceiving(g_incoming)) {
        SendPackReply(false);
        dialogs::Message(kDlgNotice, "LEVEL PACK", "The level pack can't be saved in your game's levelpacks folder.",
                         "OK");
        return;
    }
    SendPackReply(true);
    g_pack_text_time = 0;
    dialogs::Message(kDlgPackProgress, "LEVEL PACK", "Downloading " + g_incoming.file + "...\n\n", "CANCEL");
}

void PackRefused(const std::string& why)
{
    g_offer.active = false;
    dialogs::Close(kDlgPackWait);
    dialogs::Message(kDlgNotice, "PICK ANOTHER LEVEL", why + "\n\nPick a level you both have.", "OK");
}

void OnPackReply(const net::Message& m)
{
    if (!g_is_host || !g_offer.active || m.data.empty())
        return;
    if (m.data[0]) {
        Log("online: %s said yes to %s", g_peer_name.c_str(), g_offer.pack.file.c_str());
        g_pack_text_time = 0;
        packs::StartSending(g_offer.pack);
        return;
    }
    Log("online: %s said no to %s", g_peer_name.c_str(), g_offer.pack.file.c_str());
    packs::StopSending();
    PackRefused(g_peer_name + " doesn't want the level pack.");
}

void OnPackEnd(const net::Message& m)
{
    if (!packs::Receiving() || m.data.size() < sizeof(proto::PackEnd))
        return;
    proto::PackEnd end;
    memcpy(&end, m.data.data(), sizeof end);
    bool installed = packs::FinishReceiving(end.crc);
    if (installed) {
        game::ScanLevelPacks();
        SendLevelList();
    }
    dialogs::Close(kDlgPackProgress);
    uint8_t done = installed ? 1 : 0;
    net::Send(proto::kPackDone, &done, 1);
    if (!installed)
        dialogs::Message(kDlgNotice, "LEVEL PACK", "The level pack didn't arrive in one piece, so it wasn't installed.",
                         "OK");
}

void OnPackDone(const net::Message& m)
{
    if (!g_is_host || !g_offer.active || m.data.empty())
        return;
    if (!m.data[0]) {
        PackRefused(g_peer_name + " couldn't install the level pack.");
        return;
    }
    Log("online: %s has %s now", g_peer_name.c_str(), g_offer.pack.file.c_str());
    g_offer.active = false;
    dialogs::Close(kDlgPackWait);
    void* app = game::App();
    if (g_phase != Phase::HostLobby)
        return;
    game::At<int>(app, game::kAppOffStage) = g_offer.stage;
    game::At<int>(app, game::kAppOffLevel) = g_offer.level;
    StartChosenLevel(app);
}

void OnPackCancel()
{
    if (g_is_host)
        return;
    packs::StopReceiving();
    dialogs::Close(kDlgPackOffer);
    dialogs::Close(kDlgPackProgress);
}

void OnPackData(const net::Message& m)
{
    if (!packs::Receiving() || packs::ReceivePiece(m.data.data(), m.data.size()))
        return;
    packs::StopReceiving();
    dialogs::Close(kDlgPackProgress);
    SendPackReply(false);
    dialogs::Message(kDlgNotice, "LEVEL PACK", "The level pack couldn't be saved. Is the disk full?", "OK");
}

void UpdatePackProgress()
{
    DWORD now = GetTickCount();
    if (now - g_pack_text_time < 250)
        return;
    if (packs::Receiving()) {
        g_pack_text_time = now;
        dialogs::SetText(kDlgPackProgress, "Downloading " + g_incoming.file + "...\n\n" +
                                               packs::SizeText(packs::BytesReceived()) + " of " +
                                               packs::SizeText(g_incoming.size));
    } else if (g_offer.active && packs::Sending()) {
        g_pack_text_time = now;
        dialogs::SetText(kDlgPackWait, "Sending " + g_offer.pack.file + " to " + g_peer_name + "...\n\n" +
                                           packs::SizeText(packs::BytesSent()) + " of " +
                                           packs::SizeText(g_offer.pack.size));
    }
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
    case kDlgPackOffer:
        dialogs::Close(kDlgPackOffer);
        AnswerPackOffer(button == dialogs::kFooter);
        break;
    case kDlgPackProgress:  // the joiner changed their mind
        packs::StopReceiving();
        dialogs::Close(kDlgPackProgress);
        SendPackReply(false);
        break;
    case kDlgPackWait:  // the host changed theirs
        packs::StopSending();
        g_offer.active = false;
        dialogs::Close(kDlgPackWait);
        net::Send(proto::kPackCancel);
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
    g_random_pick = id == kLevelScreenButtonRandom;
    struct Done {
        ~Done() { g_random_pick = false; }
    } done;
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
    if (g_phase != Phase::HostLobby || game::At<int>(app, game::kAppOffGameMode) != game::kModeDuel) {
        g_orig_new_board(app, a, b);
        return;
    }
    std::string id = LevelIdAt(game::At<int>(app, game::kAppOffStage), game::At<int>(app, game::kAppOffLevel));
    if (PeerHasLevel(id) || (g_random_pick && PickSharedLevel(app))) {
        StartChosenLevel(app);
        return;
    }
    Log("online: %s doesn't have %s", g_peer_name.c_str(), id.c_str());
    packs::Info pack;
    if (packs::FindWithLevel(id, pack) && pack.size <= packs::kMaxSize) {
        OfferPack(app, pack);
        return;
    }
    dialogs::Message(kDlgNotice, "PICK ANOTHER LEVEL", g_peer_name + " doesn't have this level.\n\nPick another one.",
                     "OK");
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
        case proto::kLevelList:
            OnLevelList(m);
            break;
        case proto::kPackOffer:
            OnPackOffer(m);
            break;
        case proto::kPackReply:
            OnPackReply(m);
            break;
        case proto::kPackData:
            OnPackData(m);
            break;
        case proto::kPackEnd:
            OnPackEnd(m);
            break;
        case proto::kPackDone:
            OnPackDone(m);
            break;
        case proto::kPackCancel:
            OnPackCancel();
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
    UpdatePackProgress();
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
void HideLevelsFromStage(int stage) { g_hide_from_stage = stage; }
bool IsHost() { return g_is_host; }
std::string PlayerName(int player) { return (player == 0) == g_is_host ? ProfileName() : g_peer_name; }

}  // namespace online
