#include "chaos.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <sstream>

#include "chaos_effects.h"
#include "config.h"
#include "game.h"
#include "hook.h"
#include "lockstep.h"
#include "log.h"
#include "online.h"
#include "protocol.h"
#include "ui.h"

namespace chaos {

namespace {

// ---- Game addresses ----
constexpr uintptr_t kBoardVtable = 0x6864A4;
constexpr int kBoardSlotDraw = 26;  // Draw(Graphics*)
constexpr int kBoardOffLogic = 0x720;
constexpr int kLogicOffState = 0x04, kLogicOffReplaying = 0x101, kLogicOffFever = 0x102,
              kLogicOffCurrentPlayer = 0x1AC;
constexpr int kLogicOffBalls = 0x208;  // int per player (a duel): balls left
constexpr int kLogicOffZenShots = 0x298;  // int per player: Zen Balls to come (Master Hu's power)
enum LogicState {
    kAiming = 1,
    kBallInPlay = 2,
    kPegsClearing = 3,
    kOver = 5,
    kTurnPassing = 6,
    kIntro = 8,
    kSelection = 9,
    kZenShot = 10,  // fired with Master Hu's Zen Ball: the game chooses the shot, then the ball is in play
};

// Graphics: drawing is scaled about an origin (images, and so the image fonts' letters).
constexpr int kGraphicsOffScaleX = 0x10, kGraphicsOffScaleY = 0x14, kGraphicsOffScaleOrigX = 0x18,
              kGraphicsOffScaleOrigY = 0x1C;

// Fonts and sounds the game loads at startup.
constexpr uintptr_t kFontBig = 0x6CA76C;    // FONT_OVERLOAD24OUTLINE
constexpr uintptr_t kFontSmall = 0x6CA94C;  // FONT_OVERLOAD16OUTLINE
constexpr uintptr_t kSoundGong = 0x6CA8E0;  // SOUND_GONG

// The Two Player Selection (dialog 1). Its middle column is free under the computer's difficulty: the boxes go
// there, lined up with Play a Friend (175, 110, 170 wide, relative to the dialog), RED PEG MODE above CHAOS MODE.
constexpr int kSelectionId = 1;
constexpr int kBoxId = 9103, kRedBoxId = 9104;
constexpr int kBoxX = 175, kBoxY = 400, kRedBoxY = 362, kBoxW = 170;

struct EffectInfo {
    const char* name;
    const char* hint;
};
const EffectInfo kInfo[kEffectCount] = {
    {"Big Balls!", "Balls are 50% bigger!"},
    {"Pinball Madness!", "Pegs kick the ball harder!"},
    {"Double Bucket!", "Two buckets to aim for!"},
    {"Teleporting Bucket!", "The bucket won't stay put!"},
    {"Purple Party!", "Three purple pegs!"},
    {"Peg Shuffle!", "The pegs' colors are shuffled!"},
    {"Invisible Pegs!", "Pegs show up when the ball hits them!"},
    {"Blind Shot!", "No aiming guide!"},
    {"Double Shot!", "Two balls every shot!"},
    {"Wormholes!", "In one, out the other!"},
    {"Wrap Around!", "No walls: out one side, in the other!"},
    {"Bomb Peg!", "Hit the bomb to clear everything around it!"},
    {"Lights Out!", "Follow the spotlights!"},
    {"Upside Down!", "The ball falls up!"},
    {"Portal Storm!", "Six wormholes!"},
    {"Bullet Time!", "Slow motion near orange pegs!"},
    {"Mirage!", "Some of the pegs are fake!"},
    {"Silhouette!", "Pegs are gray until the ball hits them!"},
};

// What hitting the red peg can give the other player.
const Effect kRedEffects[] = {kMirage, kSilhouette, kBlindShot, kLightsOut};

// ---- Shared random numbers (xorshift32), advanced only inside the simulation ----
uint32_t g_rng = 1;

uint32_t NextRandom()
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

// ---- The duel on the board ----
struct Duel {
    void* board = nullptr;
    bool on = false;       // Chaos Mode in this duel (kept when the level is replayed: no selection then)
    bool red_on = false;   // Red Peg Mode (likewise)
    bool running = false;  // from the first turn until the duel is over
    int last_state = -1;
    int round = 0;         // 0: both players' first shots, without an effect
    uint8_t shots = 0;     // players who have shot this round (bits)
    Effect effect = kNone;
    Effect last_effect = kNone;
    uint32_t effect_ticks = 0;
    uint8_t bag[kChaosEffects] = {};  // the effects not dealt yet, in a shuffled order (dealt from the end)
    int bag_left = 0;
    // Red Peg Mode
    bool red_hit = false;       // the red peg was hit: none for the rest of the duel
    int red_victim = -1;        // the player whose next turn gets its effect
    Effect red_effect = kNone;  // that effect, in play for this turn
    uint32_t red_ticks = 0;
};
Duel g_duel;

// Online, Play Now hands both computers this duel's settings at the same point of the game.
bool g_online_pending, g_online_on, g_online_red;
uint32_t g_online_seed;

// Testing: effects to deal next, before the shuffled ones (a round's, and a red peg's); an effect to switch to at
// the next tick; the effect kept through the rounds.
std::deque<Effect> g_queue, g_red_queue;
bool g_switch_pending;
Effect g_switch_to = kNone;
bool g_hold;
uint32_t g_n_zen;  // testing: shots a Zen Ball chose
// Testing: the bomb moved to the peg nearest a point, at a tick (online: the same tick on both computers, inside
// the simulation, or the two would differ meanwhile); the red peg likewise.
bool g_bomb_pending, g_red_pending;
float g_bomb_x, g_bomb_y, g_red_x, g_red_y;
uint32_t g_bomb_tick, g_red_tick;
int g_red_kind;  // (testing: 0 any, 1 round and still, 2 a brick, 3 moving)

// The announcements, timed by the board's ticks (10 ms each) since their effects began: a round's and a red peg's
// (both at once when they begin together: one above the other).
Effect g_announced = kNone, g_red_announced = kNone;
constexpr DWORD kPopMs = 260, kHoldMs = 2600, kFadeMs = 450;

void* Logic(void* board) { return board ? game::At<void*>(board, kBoardOffLogic) : nullptr; }

bool InTurn(int st) { return st == kAiming || st == kZenShot || st == kBallInPlay || st == kPegsClearing; }

int CurrentPlayer(void* board) { return game::At<int>(Logic(board), kLogicOffCurrentPlayer) & 1; }

void Refill()
{
    for (int i = 0; i < kChaosEffects; ++i)
        g_duel.bag[i] = static_cast<uint8_t>(i);
    for (int i = kChaosEffects - 1; i > 0; --i) {
        int j = static_cast<int>(NextRandom() % static_cast<uint32_t>(i + 1));
        uint8_t t = g_duel.bag[i];
        g_duel.bag[i] = g_duel.bag[j];
        g_duel.bag[j] = t;
    }
    if (g_duel.bag[kChaosEffects - 1] == g_duel.effect) {  // never the same effect twice in a row
        g_duel.bag[kChaosEffects - 1] = g_duel.bag[0];
        g_duel.bag[0] = static_cast<uint8_t>(g_duel.effect);
    }
    g_duel.bag_left = kChaosEffects;
}

Effect Deal(void* board)
{
    while (!g_queue.empty()) {
        Effect e = g_queue.front();
        g_queue.pop_front();
        if (EffectPossible(board, e))
            return e;
    }
    for (int tries = 0; tries < 2 * kChaosEffects; ++tries) {
        if (g_duel.bag_left == 0)
            Refill();
        auto e = static_cast<Effect>(g_duel.bag[--g_duel.bag_left]);
        if (e != g_duel.effect && EffectPossible(board, e))
            return e;
    }
    return kNone;
}

// A red peg's effect may join the round's: never the same one, nor one with nothing to show (Mirage's fakes and
// Silhouette's gray pegs are hidden by Invisible Pegs too).
bool RedPossible(void* board, Effect e, Effect round)
{
    if (e == round)
        return false;
    if (round == kInvisiblePegs && (e == kMirage || e == kSilhouette))
        return false;
    return EffectPossible(board, e);
}

Effect DealRed(void* board, Effect round)
{
    while (!g_red_queue.empty()) {
        Effect e = g_red_queue.front();
        g_red_queue.pop_front();
        if (RedPossible(board, e, round))
            return e;
    }
    Effect options[4];
    uint32_t n = 0;
    for (Effect e : kRedEffects)
        if (RedPossible(board, e, round))
            options[n++] = e;
    return n ? options[NextRandom() % n] : kNone;
}

// The next turn's effects are decided as the turn passes (so Mirage's blackout can begin before it): the new
// round's, if both players have shot, and the red peg's, if the next turn is its victim's. They begin with it.
struct NextTurn {
    bool ready = false;
    bool new_round = false;
    Effect effect = kNone;  // the round's effect from then on
    Effect red = kNone;
};
NextTurn g_next;

int NextPlayer(void* board)  // as the game will choose: the other player, unless they have no balls left
{
    void* lm = Logic(board);
    int cur = game::At<int>(lm, kLogicOffCurrentPlayer) & 1, other = cur ^ 1;
    return game::At<int>(lm, kLogicOffBalls + 4 * other) >= 1 ? other : cur;
}

void EndRed(void* board)
{
    if (g_duel.red_effect == kNone)
        return;
    EndEffect(board, g_duel.red_effect);
    Log("chaos: the red peg's %s is over (tick %u)", kInfo[g_duel.red_effect].name, lockstep::CurrentTick());
    g_duel.red_effect = kNone;
}

void BeginRed(void* board, Effect e)
{
    if (e == kNone)
        return;
    g_duel.red_effect = e;
    g_duel.red_ticks = 0;
    BeginEffect(board, e);
    g_red_announced = e;
    if (g_duel.effect == kNone || g_duel.effect_ticks != 0)
        ui::PlaySound(kSoundGong);  // (a round's effect beginning with it had one: one gong for both)
    Log("chaos: red peg: %s for player %d (tick %u)", kInfo[e].name, CurrentPlayer(board) + 1,
        lockstep::CurrentTick());
}

void Switch(void* board, Effect e)
{
    if (g_duel.effect != kNone) {
        EndEffect(board, g_duel.effect);
        g_duel.last_effect = g_duel.effect;
    }
    if (e != kNone && e == g_duel.red_effect)
        EndRed(board);  // (testing: forced to the red peg's)
    g_duel.effect = e;
    g_duel.effect_ticks = 0;
    Log("chaos: round %d: %s (tick %u)", g_duel.round, e == kNone ? "no effect" : kInfo[e].name,
        lockstep::CurrentTick());
    if (e != kNone) {
        BeginEffect(board, e);
        g_announced = e;
        ui::PlaySound(kSoundGong);
    }
}

// The masters are chosen: this duel's settings.
void Latch(void* board)
{
    if (lockstep::Active()) {
        if (g_online_pending) {
            g_duel.on = g_online_on;
            g_duel.red_on = g_online_red;
            g_rng = g_online_seed ? g_online_seed : 1;
        }
    } else {
        g_duel.on = config::ChaosMode();
        g_duel.red_on = config::RedPegMode();
        g_rng = (GetTickCount() ^ static_cast<uint32_t>(reinterpret_cast<uintptr_t>(board))) | 1;
    }
    g_online_pending = false;
    g_duel.bag_left = 0;
    Log("chaos: the duel %s Chaos Mode, %s Red Peg Mode", g_duel.on ? "has" : "doesn't have",
        g_duel.red_on ? "has" : "doesn't have");
}

void TurnEnding(void* board)
{
    g_next = NextTurn();
    g_next.ready = true;
    g_next.new_round = g_duel.on && g_duel.shots == 3 && !g_hold;
    g_next.effect = g_next.new_round ? Deal(board) : g_duel.effect;
    if (g_duel.red_on && g_duel.red_victim >= 0 && NextPlayer(board) == g_duel.red_victim) {
        g_next.red = DealRed(board, g_next.effect);
        if (g_next.red == kMirage)
            StartBlackout(board);
    }
}

void TurnStarted(void* board)
{
    int player = CurrentPlayer(board);
    if (!g_duel.running) {  // the duel's first turn
        g_duel.running = true;
        g_duel.round = 0;
        g_duel.shots = 0;
        g_duel.last_effect = kNone;
        g_duel.red_hit = false;
        g_duel.red_victim = -1;
        g_next = NextTurn();
        if (g_duel.red_on) {
            ChooseRedPeg(board);
            Log("chaos: the red peg is chosen (tick %u)", lockstep::CurrentTick());
        }
        return;
    }
    EndRed(board);  // a red peg's effect lasts one turn
    if (!g_next.ready)
        TurnEnding(board);  // (no turn passing came between)
    NextTurn next = g_next;
    g_next = NextTurn();
    if (g_duel.on && g_duel.shots == 3) {
        g_duel.shots = 0;
        if (next.new_round) {
            ++g_duel.round;  // both players have shot: a new effect, the old one goes
            Switch(board, next.effect);
        }
    }
    if (next.red != kNone && g_duel.red_victim == player) {
        g_duel.red_victim = -1;
        BeginRed(board, next.red);
    } else if (next.red != kNone) {
        CancelBlackout();  // (the turn went to someone else after all: the victim's own comes later)
    }
}

void DuelOver(void* board)
{
    if (!g_duel.running)
        return;
    g_duel.running = false;
    g_duel.red_victim = -1;
    g_next = NextTurn();
    CancelBlackout();
    EndRed(board);
    if (g_duel.effect != kNone)
        Switch(board, kNone);
}

// A board went or came. The effects (some change the game's code: gravity, the speed limit) and the red peg go
// with the old one, unless they're the new one's already (a saved replay put them in place as it began); online
// this happens on the same tick on both computers.
void NewBoard(void* board)
{
    DropEffects(board);
    ForgetRedPeg(board);
    g_duel = Duel();
    g_duel.board = board;
    g_next = NextTurn();
    g_rng = 1;
    g_announced = g_red_announced = kNone;
}

bool Replaying(void* board)
{
    void* lm = Logic(board);
    return lm && game::At<uint8_t>(lm, kLogicOffReplaying);
}

// ---- The boxes on the Two Player Selection ----
struct CheckboxListener {
    void* const* vtable;
};

void* g_box_dialog;  // the selection the boxes are on (which deletes them with its other widgets)
void* g_box;
void* g_red_box;
bool g_host_choice, g_host_red;  // online, joining: the host's boxes
int g_sent_choice = -1;          // online, hosting: what the joiner was last told (bits: 1 chaos, 2 red)

bool Joining() { return lockstep::Active() && !online::IsHost(); }

void __fastcall OnBoxChecked(CheckboxListener*, void*, int id, int checked)
{
    bool on = (checked & 0xFF) != 0;
    if (Joining())
        return;  // (the joiner's boxes don't take clicks)
    if (id == kRedBoxId) {
        config::SetRedPegMode(on);
        Log("chaos: Red Peg Mode %s", on ? "chosen" : "off");
    } else {
        config::SetChaosMode(on);
        Log("chaos: Chaos Mode %s", on ? "chosen" : "off");
    }
}

void* const g_listener_vtable[1] = {reinterpret_cast<void*>(OnBoxChecked)};
CheckboxListener g_listener = {g_listener_vtable};

void* DuelSelection()
{
    void* app = game::App();
    if (!app || game::At<int>(app, game::kAppOffGameMode) != game::kModeDuel)
        return nullptr;
    return game::GetDialog(kSelectionId);
}

void* NewBox(void* sel, int id, const char* label, int y, bool checked)
{
    void* box = ui::NewCheckbox(id, &g_listener, label);
    ui::Resize(box, kBoxX, y, kBoxW, game::At<int>(box, game::kWidgetOffHeight));
    ui::AddChild(sel, box);
    ui::SetChecked(box, checked, false);
    return box;
}

void UpdateBoxes()
{
    void* sel = DuelSelection();
    if (sel != g_box_dialog) {
        g_box = g_red_box = nullptr;  // the selection that had them deletes them
        g_box_dialog = sel;
        g_sent_choice = -1;
    }
    if (!sel)
        return;
    if (!g_box) {
        g_red_box = NewBox(sel, kRedBoxId, "Red Peg Mode", kRedBoxY, RedWanted());
        g_box = NewBox(sel, kBoxId, "Chaos Mode", kBoxY, Wanted());
    }
    bool joining = Joining();
    for (void* box : {g_box, g_red_box})
        game::At<uint8_t>(box, game::kWidgetOffMouseVisible) = !joining;  // the host decides
    if (joining) {
        if (ui::IsChecked(g_box) != g_host_choice)
            ui::SetChecked(g_box, g_host_choice, false);
        if (ui::IsChecked(g_red_box) != g_host_red)
            ui::SetChecked(g_red_box, g_host_red, false);
    } else if (lockstep::Active()) {
        int choice = (Wanted() ? 1 : 0) | (RedWanted() ? 2 : 0);
        if (g_sent_choice != choice) {
            proto::DuelOptions o = {static_cast<uint8_t>(choice & 1), static_cast<uint8_t>(choice >> 1)};
            net::Send(proto::kDuelOptions, &o, sizeof o);
            g_sent_choice = choice;
        }
    }
}

// ---- Drawing ----
float EaseOutBack(float t)
{
    const float c = 1.7f;
    t -= 1.0f;
    return 1.0f + (c + 1.0f) * t * t * t + c * t * t;
}

void SetScale(void* g, float scale, int x, int y)
{
    game::At<float>(g, kGraphicsOffScaleX) = scale;
    game::At<float>(g, kGraphicsOffScaleY) = scale;
    game::At<float>(g, kGraphicsOffScaleOrigX) = static_cast<float>(x);
    game::At<float>(g, kGraphicsOffScaleOrigY) = static_cast<float>(y);
}

void Centered(void* g, void* font, const std::string& text, int cx, int baseline, const ui::Color& c)
{
    ui::SetFont(g, font);
    ui::SetColor(g, c);
    ui::DrawString(g, text, cx - ui::StringWidth(font, text) / 2, baseline);
}

bool Showing(Effect announced, Effect effect, uint32_t ticks)
{
    return announced != kNone && announced == effect && ticks * 10 < kHoldMs + kFadeMs;
}

// One announcement: a band across the board, the mode's word above the effect's name, its hint below.
void DrawBand(void* g, Effect e, uint32_t ticks, int cy, bool red)
{
    void* big = ui::Resource<void>(kFontBig);
    void* small = ui::Resource<void>(kFontSmall);
    if (!big || !small)
        return;
    DWORD age = ticks * 10;
    float scale = 1.0f;
    int alpha = 255;
    if (age < kPopMs) {
        float t = static_cast<float>(age) / kPopMs;
        scale = 0.3f + 0.7f * EaseOutBack(t);
        alpha = static_cast<int>(255 * (t < 0.5f ? t * 2 : 1.0f));
    } else if (age > kHoldMs) {
        float t = static_cast<float>(age - kHoldMs) / kFadeMs;
        scale = 1.0f + 0.15f * t;
        alpha = static_cast<int>(255 * (1.0f - t));
    }
    const int cx = 400;
    ui::SetColor(g, {red ? 40 : 0, 0, 0, alpha * 150 / 255});
    ui::FillRect(g, 0, cy - 62, 800, 112);
    ui::SetColor(g, red ? ui::Color{255, 60, 60, alpha * 200 / 255} : ui::Color{200, 120, 255, alpha * 200 / 255});
    ui::FillRect(g, 0, cy - 62, 800, 2);
    ui::FillRect(g, 0, cy + 48, 800, 2);
    std::string name = kInfo[e].name;
    for (char& ch : name)
        ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
    int big_h = ui::Ascent(big);
    if (red)
        Centered(g, small, "RED PEG!", cx, cy - 30, {255, 70, 70, alpha});
    else
        Centered(g, small, "CHAOS!", cx, cy - 30, {235, 85, 220, alpha});
    SetScale(g, scale * 1.5f, cx, cy - big_h / 2);
    Centered(g, big, name, cx, cy + big_h / 2 - 4, {255, 214, 60, alpha});
    SetScale(g, 1.0f, 0, 0);
    Centered(g, ui::Resource<void>(ui::kFontPlain), kInfo[e].hint, cx, cy + 36, {255, 255, 255, alpha});
}

void DrawAnnouncements(void* g)
{
    bool chaos = Showing(g_announced, g_duel.effect, g_duel.effect_ticks);
    bool red = Showing(g_red_announced, g_duel.red_effect, g_duel.red_ticks);
    if (!chaos)
        g_announced = kNone;
    if (!red)
        g_red_announced = kNone;
    if (chaos)
        DrawBand(g, g_announced, g_duel.effect_ticks, red ? 232 : 292, false);
    if (red)
        DrawBand(g, g_red_announced, g_duel.red_ticks, chaos ? 352 : 292, true);
}

using DrawFn = void(__thiscall*)(void* board, void* g);
DrawFn g_orig_board_draw;

void __fastcall HookBoardDraw(void* board, void*, void* g)
{
    BoardDrawBegins(board);
    g_orig_board_draw(board, g);
    BoardDrawEnds();
    if (board == g_duel.board && !Replaying(board))
        DrawAnnouncements(g);
}

Effect ParseEffect(const std::string& s)
{
    if (!s.empty() && isdigit(static_cast<unsigned char>(s[0]))) {
        int n = atoi(s.c_str());
        return n >= 0 && n < kEffectCount ? static_cast<Effect>(n) : kNone;
    }
    for (int i = 0; i < kEffectCount; ++i) {
        std::string name;
        for (const char* p = kInfo[i].name; *p; ++p)
            if (isalnum(static_cast<unsigned char>(*p)))
                name += static_cast<char>(tolower(static_cast<unsigned char>(*p)));
        std::string want;
        for (char ch : s)
            if (isalnum(static_cast<unsigned char>(ch)))
                want += static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        if (name == want)
            return static_cast<Effect>(i);
    }
    return kNone;
}

bool IsRedEffect(Effect e)
{
    for (Effect r : kRedEffects)
        if (r == e)
            return true;
    return false;
}

std::string BoxState(void* box)
{
    if (!box)
        return "none";
    ui::Rect r = ui::ScreenRect(box);
    char buf[96];
    snprintf(buf, sizeof buf, "%d %d %d %d checked=%d", r.x, r.y, r.w, r.h, ui::IsChecked(box) ? 1 : 0);
    return buf;
}

}  // namespace

void Install()
{
    g_orig_board_draw =
        reinterpret_cast<DrawFn>(HookVtable(kBoardVtable, kBoardSlotDraw, reinterpret_cast<void*>(HookBoardDraw)));
    InstallEffects();
}

void Update()
{
    UpdateBoxes();
    void* board = game::App() ? game::At<void*>(game::App(), game::kAppOffBoard) : nullptr;
    if (g_duel.board && board != g_duel.board)
        NewBoard(board);  // back to the menus with an effect on (or on to a board of saved replays)
    UpdateEffects();
}

void BoardReloaded(void* board)
{
    void* lm = Logic(board);
    if (board == g_duel.board && lm)
        g_duel.last_state = game::At<int>(lm, kLogicOffState);  // (no turn begins or ends by a reload)
}

uint32_t RandomState() { return g_rng; }
void SetRandomState(uint32_t s) { g_rng = s ? s : 1; }

bool Wanted() { return Joining() ? g_host_choice : config::ChaosMode(); }
bool RedWanted() { return Joining() ? g_host_red : config::RedPegMode(); }

bool HandleMessage(const net::Message& m)
{
    if (m.type != proto::kDuelOptions)
        return false;
    proto::DuelOptions o;
    if (m.data.size() >= sizeof o) {
        memcpy(&o, m.data.data(), sizeof o);
        g_host_choice = o.chaos != 0;
        g_host_red = o.red != 0;
        Log("chaos: the host's choices: Chaos Mode %s, Red Peg Mode %s", g_host_choice ? "on" : "off",
            g_host_red ? "on" : "off");
    }
    return true;
}

void SetOnlineDuel(bool chaos, bool red, uint32_t seed)
{
    g_online_pending = true;
    g_online_on = chaos;
    g_online_red = red;
    g_online_seed = seed;
}

void AfterTick(void* board)
{
    void* app = game::App();
    void* lm = Logic(board);
    if (!app || !lm)
        return;
    if (board != g_duel.board)  // a new board (of any kind): nothing left over from the last one
        NewBoard(board);
    if (game::At<int>(app, game::kAppOffGameMode) != game::kModeDuel)
        return;
    int st = game::At<int>(lm, kLogicOffState);
    int last = g_duel.last_state;
    g_duel.last_state = st;
    if (last == kSelection && st != kSelection)
        Latch(board);
    if (!g_duel.on && !g_duel.red_on)
        return;
    bool replaying = game::At<uint8_t>(lm, kLogicOffReplaying) != 0;
    if (!replaying && st != last) {  // (an Instant Replay is a shot shown again)
        bool turn = InTurn(st), was_turn = InTurn(last);
        if (turn && !was_turn)
            TurnStarted(board);
        if ((st == kBallInPlay || st == kPegsClearing) && (last == kAiming || last == kZenShot))
            g_duel.shots |= 1 << CurrentPlayer(board);
        if (st == kZenShot)
            ++g_n_zen;
        if (st == kTurnPassing && was_turn && g_duel.running)
            TurnEnding(board);
        if (st == kOver || st == kSelection || st == kIntro)
            DuelOver(board);
    }
    if (g_switch_pending && g_duel.running) {
        g_switch_pending = false;
        Switch(board, g_switch_to);
    }
    if (g_bomb_pending && (!lockstep::Active() || lockstep::CurrentTick() >= g_bomb_tick)) {
        g_bomb_pending = false;
        DebugBombAt(g_bomb_x, g_bomb_y);
    }
    if (g_red_pending && g_duel.running && (!lockstep::Active() || lockstep::CurrentTick() >= g_red_tick)) {
        g_red_pending = false;
        g_duel.red_hit = false;  // (testing: a red peg again)
        g_duel.red_victim = -1;
        bool found = DebugRedAt(board, g_red_x, g_red_y, g_red_kind);
        Log("chaos: the red peg is moved%s (tick %u)", found ? "" : ": none of that kind", lockstep::CurrentTick());
    }
    if (g_duel.effect != kNone && !replaying)
        ++g_duel.effect_ticks;
    if (g_duel.red_effect != kNone && !replaying)
        ++g_duel.red_ticks;
    // The red peg lit: the player in turn hit it. (In Extreme Fever the duel is over: nobody's next turn.)
    if (g_duel.red_on && g_duel.running && !g_duel.red_hit && RedPegHit() && !replaying) {
        g_duel.red_hit = true;
        int shooter = CurrentPlayer(board);
        if (!game::At<uint8_t>(lm, kLogicOffFever))
            g_duel.red_victim = shooter ^ 1;
        Log("chaos: player %d hit the red peg (tick %u)", shooter + 1, lockstep::CurrentTick());
    }
}

uint32_t Random(uint32_t below) { return below ? NextRandom() % below : 0; }

uint32_t StateHash()
{
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) {
        h ^= v;
        h *= 16777619u;
    };
    mix(g_duel.on);
    mix(g_duel.running);
    mix(static_cast<uint32_t>(g_duel.round));
    mix(g_duel.shots);
    mix(static_cast<uint32_t>(g_duel.effect));
    mix(g_duel.effect_ticks);
    mix(g_duel.red_on);
    mix(g_duel.red_hit);
    mix(static_cast<uint32_t>(g_duel.red_victim));
    mix(static_cast<uint32_t>(g_duel.red_effect));
    mix(g_duel.red_ticks);
    mix(g_rng);
    mix(EffectHash());
    return h;
}

std::string DebugState()
{
    char buf[640];
    void* lm = Logic(g_duel.board);
    snprintf(buf, sizeof buf,
             "on=%d running=%d round=%d shots=%d effect=%d (%s) ticks=%u announced=%d box=%d,%d,%d wanted=%d "
             "host=%d queue=%d rng=%08X red=%d redhit=%d victim=%d redeffect=%d redticks=%u redannounced=%d "
             "redbox=%d,%d redwanted=%d hostred=%d player=%d zen=%u zenleft=%d",
             g_duel.on ? 1 : 0, g_duel.running ? 1 : 0, g_duel.round, g_duel.shots, g_duel.effect,
             g_duel.effect == kNone ? "none" : kInfo[g_duel.effect].name, g_duel.effect_ticks,
             g_announced == kNone ? -1 : static_cast<int>(g_duel.effect_ticks * 10), g_box ? 1 : 0,
             g_box ? (ui::IsChecked(g_box) ? 1 : 0) : -1,
             g_box ? game::At<uint8_t>(g_box, game::kWidgetOffMouseVisible) : -1, Wanted() ? 1 : 0,
             g_host_choice ? 1 : 0, static_cast<int>(g_queue.size()), g_rng, g_duel.red_on ? 1 : 0,
             g_duel.red_hit ? 1 : 0, g_duel.red_victim, g_duel.red_effect, g_duel.red_ticks,
             g_red_announced == kNone ? -1 : static_cast<int>(g_duel.red_ticks * 10), g_red_box ? 1 : 0,
             g_red_box ? (ui::IsChecked(g_red_box) ? 1 : 0) : -1, RedWanted() ? 1 : 0, g_host_red ? 1 : 0,
             lm ? CurrentPlayer(g_duel.board) : -1, g_n_zen,
             lm ? game::At<int>(lm, kLogicOffZenShots + 4 * CurrentPlayer(g_duel.board)) : -1);
    return buf;
}

// chaos: the state. chaos next <effect>...: deal these next (name or number). chaos now <effect|none>: switch to
// it at the next tick (one computer only: for local tests). chaos hold [off]: keep the effect through the rounds.
// chaos box / redbox: where the boxes are. chaos fx: what the effects' hooks did. chaos bombat <x> <y> [tick]: the
// bomb is the peg nearest that point from that tick on (online: the same on both). chaos dropin: the next ball steps
// into the second bucket (one computer only). chaos spooky: the next shot is a Spooky Ball, coming back once (with
// or without an effect; one computer only). chaos rednext <effect>...: the red peg's effects to deal next. chaos
// redat <x> <y> [tick] [round|brick|moving]: the red peg is the blue peg (of that kind) nearest that point from that
// tick on, as if none had been hit. chaos zen [n]: the player in turn has n Zen Balls to come (1; 0: none), as Master
// Hu's power gives them (one computer only). chaos power <power> [turns] [player]: the player in turn (or that one:
// 0, 1) has that master's power (the game's number) for that many turns (1; 0: off), from now on (one computer only).
// chaos ballat <x> <y> <vx> <vy>: the next ball to move is put there (screen), at that speed (one computer only).
std::string DebugCommand(const std::string& args)
{
    std::istringstream in(args);
    std::string sub;
    in >> sub;
    if (sub.empty())
        return DebugState();
    if (sub == "next" || sub == "rednext") {
        bool red = sub == "rednext";
        std::deque<Effect>& queue = red ? g_red_queue : g_queue;
        queue.clear();
        std::string word;
        while (in >> word) {
            Effect e = ParseEffect(word);
            if (e == kNone || (red ? !IsRedEffect(e) : e >= kChaosEffects))
                return "error: no " + std::string(red ? "red peg " : "") + "effect " + word;
            queue.push_back(e);
        }
        return "ok";
    }
    if (sub == "now") {
        std::string word;
        in >> word;
        Effect e = word == "none" ? kNone : ParseEffect(word);
        if ((e == kNone && word != "none") || e >= kChaosEffects)
            return "error: no effect " + word;
        g_switch_to = e;
        g_switch_pending = true;
        return "ok";
    }
    if (sub == "box")
        return BoxState(g_box);
    if (sub == "redbox")
        return BoxState(g_red_box);
    if (sub == "fx")
        return EffectDebug();
    if (sub == "dropin")
        return DebugDropIn() ? "ok" : "error: no Double Bucket";
    if (sub == "spooky") {
        DebugSpooky();
        return "ok";
    }
    if (sub == "zen") {
        void* lm = Logic(g_duel.board);
        if (!lm)
            return "error: no duel";
        int n;
        if (!(in >> n))
            n = 1;
        game::At<int>(lm, kLogicOffZenShots + 4 * CurrentPlayer(g_duel.board)) = n;
        return "ok";
    }
    if (sub == "ballat") {
        float x = 0, y = 0, vx = 0, vy = 0;
        in >> x >> y >> vx >> vy;
        DebugPlaceBall(x, y, vx, vy);
        return "ok";
    }
    if (sub == "power") {
        int power, turns = 1, player = -1;
        if (!(in >> power))
            return "error: chaos power <power> [turns] [player]";
        in >> turns >> player;
        return DebugPower(g_duel.board, power, turns, player) ? "ok" : "error: no duel, or no such power";
    }
    if (sub == "bombat") {
        g_bomb_x = g_bomb_y = 0;
        g_bomb_tick = 0;
        in >> g_bomb_x >> g_bomb_y >> g_bomb_tick;
        if (g_duel.effect != kBombPeg)
            return "error: no Bomb Peg";
        g_bomb_pending = true;
        return "ok";
    }
    if (sub == "redat") {
        g_red_x = g_red_y = 0;
        g_red_tick = 0;
        g_red_kind = 0;
        in >> g_red_x >> g_red_y;
        std::string word;
        while (in >> word) {
            if (word == "round" || word == "brick" || word == "moving")
                g_red_kind = word == "round" ? 1 : word == "brick" ? 2 : 3;
            else
                g_red_tick = static_cast<uint32_t>(strtoul(word.c_str(), nullptr, 10));
        }
        if (!g_duel.red_on)
            return "error: no Red Peg Mode";
        g_red_pending = true;
        return "ok";
    }
    if (sub == "hold") {
        std::string word;
        in >> word;
        g_hold = word != "off";
        return "ok";
    }
    if (sub == "names") {
        std::string out;
        for (int i = 0; i < kEffectCount; ++i)
            out += std::to_string(i) + "=" + kInfo[i].name + (i + 1 < kEffectCount ? ", " : "");
        return out;
    }
    return "error: chaos [next <effects>|now <effect>|rednext <effects>|redat <x> <y>|box|redbox|names|zen [n]|"
           "power <power> [turns] [player]|ballat <x> <y> <vx> <vy>]";
}

}  // namespace chaos
