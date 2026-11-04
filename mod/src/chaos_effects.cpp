#include "chaos_effects.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "game.h"
#include "hook.h"
#include "log.h"
#include "ui.h"

namespace chaos {

namespace {

// ---- The board's objects (see re/NOTES.md, section 9) ----
constexpr int kBoardOffGun = 0xD4;
constexpr int kBoardOffSlowMo = 0xDA;        // bool: slow motion on (the time scale is at +0xEC)
constexpr int kBoardOffSlowHold = 0xE4;      // ticks it stays at its slowest
constexpr int kBoardOffZoomHold = 0x120;     // ticks the camera stays zoomed in
constexpr int kBoardOffZoomRequest = 0x6E5;  // bool: zoom in on the ball
constexpr int kBoardOffPhysics = 0x708;      // PhysMgr*
constexpr int kBoardOffLogic = 0x720;
constexpr int kBoardOffObjects = 0x764;  // std::list head: everything on the board (walls, bucket, pegs, balls)
constexpr int kBoardOffBalls = 0x770;    // std::list head: the balls in play
constexpr int kBoardOffChallenge = 0x6F4, kChallengeOffCovered = 0xDD;  // a challenge whose bucket keeps its lid
constexpr float kFieldW = 650, kFieldH = 560;  // the playfield, drawn at (75, 40)

constexpr int kLogicOffState = 0x04, kLogicOffFever = 0x102;  // Extreme Fever has begun (the last orange is hit)
constexpr int kLogicOffStateSteps = 0x08;  // logic steps since the state began
constexpr int kLogicOffAim = 0xEC;
constexpr int kLogicOffReplaying = 0x101;  // an Instant Replay is playing
constexpr int kLogicOffBoard = 0x184, kLogicOffPlayer = 0x1AC;  // the current player
constexpr int kLogicOffSpookies = 0x290;  // int per player: how many more times a Spooky Ball comes back
constexpr int kLogicOffPowerTurns = 0x260;  // int per power and player (+ 8 * power + 4 * player): turns it lasts
constexpr int kLogicOffTrackGoal = 0x1B8;   // int: the width the bucket's track is easing to (0: it's there)
constexpr int kStateBallInPlay = 2, kStateTurnPassing = 6;

// Everything on the board is a PhysObj. The shot ball and round pegs are Balls, bricks are Bricks.
constexpr uintptr_t kBallVtable = 0x688974, kBrickVtable = 0x688D7C;
constexpr int kObjOffRefs = 0x04;
constexpr int kObjOffKind = 0x10;  // 1 Hole, 2 ball, 3 round peg, 4 line, 5 poly or brick
constexpr int kKindHole = 1, kKindBall = 2, kKindRoundPeg = 3;
constexpr int kObjOffLeft = 0x14, kObjOffTop = 0x18, kObjOffRight = 0x1C, kObjOffBottom = 0x20;  // bounding box
constexpr int kObjOffCollides = 0x24;
constexpr int kObjOffAlive = 0x25;    // on the board (hit pegs go 4 ticks after they pop)
constexpr int kObjOffMover = 0x48;    // Mover*: objects that move along a path (the bucket)
constexpr int kObjOffId = 0x5C;       // std::string from the level file ("wall", "freeball", ...)
constexpr int kObjOffPegInfo = 0xD0;  // PegInfo*: set on pegs only
constexpr int kObjSlotSetPos = 17, kObjSlotSetVelocity = 19;                           // (float, float)
constexpr int kObjSlotDrawShadow = 24, kObjSlotDraw = 25, kObjSlotDrawOverlay = 26;  // (Graphics*)
constexpr int kObjSlotSetActive = 33;  // (bool): on the board or off it, fading

// Polys: a shape (the points about its position, unturned), turned about the position (a flipper about its pivot).
constexpr uintptr_t kPolyVtable = 0x688B3C;
constexpr int kPolyOffPoints = 0xE8;  // std::vector of (x, y): first, last
constexpr int kPolyOffX = 0x114, kPolyOffY = 0x118;
constexpr int kPolyOffAngle = 0x11C, kPolyOffSpin = 0x120, kPolyOffDrawAngle = 0x128;  // radians (a step)
constexpr int kPolyOffPictureY = 0xBC;                       // its picture's centre from the position
constexpr int kPolyOffTurned = 0x145, kPolyOffMoved = 0x146;  // its edges are to be worked out again
constexpr int kPolyOffShown = 0x28, kPolyOffDrawnOnly = 0xB0;  // a bucket part drawn (or one that only collides)
constexpr uintptr_t kReverseShape = 0x4A2C40;  // stdcall(poly): its points the other way round, its edges anew

// Balls
constexpr int kBallOffX = 0xEC, kBallOffY = 0xF0, kBallOffVX = 0xFC, kBallOffVY = 0x100;
constexpr int kBallOffLastX = 0xF4, kBallOffLastY = 0xF8;  // where it was before this step's move
constexpr int kBallOffRadius = 0x13C;
constexpr int kBallOffKnockedLid = 0x145;   // a fireball knocked the bucket's lid off ("Hot Stuff!" if it falls in)
constexpr int kBallOffSpooky = 0x164;       // Renfield's Spooky Ball: comes back when it falls out
constexpr int kBallOffFireball = 0x165;     // passes through pegs
constexpr int kBallOffCharged = 0x166;      // carries Chain Lightning (its first peg hit arcs to the bucket's rod)
constexpr int kBallOffHitsBalls = 0x16D;    // bounces off other balls
constexpr int kBallOffCompanion = 0x19C;    // (Multiball copies these two to its new ball)
constexpr int kBallOffGhost = 0x1A0;
constexpr int kBallSize = 0x1AC;
constexpr float kBallRadius = 6.0f;

// The bucket's movers: x = base + radius * cos(2 pi f), f from the tick plus an offset. A flipper's (type 9 or -9,
// the sign the way it turns) keeps it at the base, turned by sign * (its swing) + the turn.
constexpr int kMoverOffType = 0x10;
constexpr int kMoverOffTicks = 0x14, kMoverOffRadius = 0x18;  // int, int
constexpr int kMoverOffTurn = 0x44, kMoverOffAngle = 0x64;   // float: radians (the angle now)
constexpr int kMoverOffX = 0x54, kMoverOffY = 0x58;          // float: where it is now
constexpr int kMoverOffBaseX = 0x5C, kMoverOffBaseY = 0x60;  // float
constexpr int kHoleOffX = 0xF4, kHoleOffY = 0xF8;  // float: the catch area's centre
constexpr int kHoleOffW = 0x104, kHoleOffH = 0x108;  // int: its size

// The cannon (a Poly): pivot, angle (radians; direction (cos a, -sin a), 3 pi / 2 is straight down), loaded ball.
constexpr int kGunOffX = 0x114, kGunOffY = 0x118, kGunOffBall = 0x1C0;
constexpr float kGunY = 30, kFlippedGunY = 535;
constexpr float kPi = 3.14159265f, kTwoPi = 6.28318531f;
constexpr float kAimDown = 4.71238898f, kAimUp = 1.57079633f;
constexpr float kFlippedAimLow = 3.26376588f, kFlippedAimHigh = 6.16101226f;  // 187 and 353 degrees: no aiming down
// Upside Down mirrors the bucket's parts about y = kBucketMirror / 2: from the bottom edge to a little below the top
// one (the score bar covers more of the board than the bottom of the frame does).
constexpr float kBucketMirror = 586;

// PegInfo
constexpr int kPegOffType = 0x10, kPegOffLit = 0x14;
enum PegType { kBlue = 1, kOrange = 2, kPurple = 3, kGreen = 4 };

// LogicMgr: the pegs, the unhit ones by color.
constexpr int kLogicOffPegList = 0x3F8;  // std::list head: every peg in the level's order, hit and gone ones too
constexpr int kLogicOffOranges = 0x40C;  // std::set<PhysObj*> of the unhit orange pegs (ordered by address)
constexpr int kLogicOffOthers = 0x418;   // ... and of the unhit pegs of the other colors

// Functions and patch sites in the game.
constexpr uintptr_t kSetPegType = 0x4A04D0;    // ECX peg, EDX type (a brick redraws itself)
constexpr uintptr_t kPegFlash = 0x49E080;      // EAX PegInfo, ECX kind (2: it became special)
constexpr uintptr_t kSetErase = 0x47EC20;      // ESI set, ECX &peg -> EAX how many were erased
constexpr uintptr_t kSetInsert = 0x43DFA0;     // EBX 12-byte result, stdcall(set, &peg)
constexpr uintptr_t kChoosePurple = 0x469600;  // stdcall(LogicMgr): the turn's purple peg (5-byte prologue)
constexpr uintptr_t kPegHit = 0x4701B0;        // thiscall(LogicMgr; ball, peg, indirect) (6-byte prologue)
constexpr uintptr_t kSpaceBlast = 0x46C610;    // EAX peg, stdcall(LogicMgr, ball): Splork's blast, 100 pixels
constexpr uintptr_t kBallCtor = 0x491AC0;      // ESI this, BL 0 (1 makes a round peg)
constexpr uintptr_t kSetRadius = 0x492530;     // EAX ball, stdcall(float)
constexpr uintptr_t kTeleport = 0x492350;      // ESI ball, stdcall(float x, float y): keeps the velocity
constexpr uintptr_t kSortAxes = 0x454390;      // stdcall(PhysMgr, axis list): before adding an object
constexpr uintptr_t kAddObject = 0x40A6F0;     // thiscall(Board; obj)
constexpr uintptr_t kCopyBallKind = 0x4926D0;  // stdcall(ball, 1): what Multiball does for special balls
constexpr uintptr_t kCatch = 0x4713C0;         // ECX LogicMgr, ESI hole, stdcall(ball): a ball fell in (and goes)
constexpr uintptr_t kNearPeg = 0x493700;       // ECX ball, EAX peg, stdcall(1.0f, margin) -> AL: touches it soon
constexpr uintptr_t kStartSlowMo = 0x471B80;   // EBX LogicMgr: 10% speed, zoom in, the slow-motion sound
constexpr uintptr_t kSetSlowMo = 0x408710;     // EDI Board, AL on, ECX speed (1000 = normal)
constexpr uintptr_t kSetGunAngle = 0x498C50;   // EAX gun, stdcall(float, bool immediate)
constexpr uintptr_t kSwitchPower = 0x46AA40;   // stdcall(LogicMgr, power, bool on): a master's power's objects
constexpr uintptr_t kChargeBall = 0x498E10;    // ESI gun: the loaded ball carries Chain Lightning (a turn begins)
constexpr uintptr_t kFireBall = 0x498E50;      // EAX gun: the loaded ball is a fireball (a turn begins)
constexpr uintptr_t kSwitchPyramid = 0x46A850;  // stdcall(LogicMgr, bool on): the pyramid on the bucket or not
enum Power { kPowerFlippers = 2, kPowerPyramid = 4, kPowerFireball = 12, kPowerLightning = 14 };
constexpr uintptr_t kGravityOp = 0x493B85;     // Ball::Update: fadd (05) / fsub (25) dword ptr [gravity]
constexpr uintptr_t kAiRodOp = 0x4394F2;       // the computer's rod: the bucket's y fsub (25) / fadd (05) 44
constexpr uintptr_t kAiFlipperLow = 0x473A60, kAiFlipperHigh = 0x473A6A;  // the computer flips for y 400..550 - 40
// (the gravity itself, a float 0.05, is at 0x6C77CC)
constexpr uintptr_t kCollisionCallback = 0x4547E8;  // immediate: what the physics calls after each bounce
constexpr uintptr_t kRecordCollision = 0x4546D0;    // cdecl(ball, obj): the game's callback
constexpr uintptr_t kSpeedCapSq = 0x493C8E, kSpeedCap = 0x493CB2;  // operands: double 225, double 15
constexpr uintptr_t kBallStep = 0x40DF81;      // each ball, after it moved (ESI ball, EDI board): 6-byte fld
constexpr uintptr_t kHitProcessing = 0x40E104;  // call: the step's hits and catches
constexpr uintptr_t kShotFired = 0x4770CE;     // call: a shot's bookkeeping, the ball at [ebp-0x14]
constexpr uintptr_t kZenShotFired = 0x476B15;  // ... a Zen Ball's (Master Hu's power aimed it), the ball at [esp+0x18]
constexpr uintptr_t kAimAtMouse = 0x4689D0;    // EAX LogicMgr, stdcall(float x, float y) -> ST0 (6-byte prologue)
constexpr uintptr_t kClampAim = 0x468D00;      // ESI LogicMgr, stdcall(float): stores LogicMgr+0xEC (7 bytes)
constexpr uintptr_t kTurnGun = 0x476DE9;       // call: the cannon follows the aim each tick
constexpr uintptr_t kDrawGuide = 0x499D00;     // ECX gun, EAX Graphics, stdcall(bool) (6-byte prologue)
constexpr uintptr_t kDrawUnderBalls = 0x40C973;  // call: EAX Board+0x714, EBX Board, stdcall(&g2, -1)
constexpr uintptr_t kDrawOverBoard = 0x40CBA2;   // call: EBX Board, stdcall(g, &g1): under the HUD
constexpr uintptr_t kSpookyCheck = 0x4749A3;   // call: each logic step, stdcall(LogicMgr): out of the bottom?
constexpr uintptr_t kSpookyReturn = 0x472CB0;  // stdcall(LogicMgr, ball): back at the top, aimed to hit a peg
constexpr uintptr_t kSpookyY[] = {0x472EDD, 0x473081};  // operands: the float 10 (the height it comes back at)
constexpr uintptr_t kPolyFill = 0x51B130;      // ESI g, ECX count, stdcall(const Point*, bool convex)
constexpr uintptr_t kSetDrawMode = 0x51ADC0;   // EAX g, ECX mode (0 normal, 1 additive)

// Peg pictures. Round pegs draw a cell (type - 1, + 4 lit) of one image of stacked cells; bricks draw a picture of
// their own per cell, made when first drawn (with 3D acceleration: without it, from copies kept elsewhere).
constexpr uintptr_t kImagePegs = 0x6CAC04, kImagePegsColorblind = 0x6CAC20;  // IMAGE_BALLPEG, IMAGE_BALLPEGCB
constexpr uintptr_t kColorblind = 0x6CBDA7;                                   // bool: the colorblind pictures
constexpr uintptr_t kImagePegShadow = 0x6CA858;  // IMAGE_PEGSHADOW: drawn black, alpha 0x90, 3 left and 3 down
constexpr uintptr_t kMovingPegCells = 0x6CBD80;  // Image*[8]: a moving round peg's cells without 3D acceleration
constexpr uintptr_t kImagePegGlow = 0x6CA9AC;    // IMAGE_PEGGLOW: a lit round peg's glow, added in its color
constexpr int kBrickOffImages = 0x4F4;           // Image*[9]: the cells, then (8) the brick's own glow
constexpr int kBrickGlowCell = 8;
constexpr int kBrickOffPictureX = 0xB8, kBrickOffPictureY = 0xBC;  // float: its picture's centre from its position
constexpr uintptr_t kMakeBrickImage = 0x494A00;  // stdcall(brick, cell): makes the cell's picture if it's missing
constexpr uintptr_t kDrawBrickCell = 0x497650;   // EAX cell, stdcall(brick, g): a brick drawn in a cell's picture
constexpr uintptr_t kDrawImageF = 0x51BE40;      // EAX g, stdcall(image, float x, float y, const Rect* src)
constexpr uintptr_t kDrawImage = 0x51B8A0;       // the same at ints, and scaled by the Graphics' scale
// MemoryImage: 0x6C bytes, constructor ECX this; virtual Create(w, h), GetBits() (ARGB), BitsChanged().
constexpr uintptr_t kMemoryImageCtor = 0x525250;
constexpr int kMemoryImageSize = 0x6C;
constexpr int kImageSlotBitsChanged = 25, kImageSlotCreate = 30, kImageSlotGetBits = 31;
constexpr int kImageOffCols = 0x30;

// Sounds
constexpr uintptr_t kSoundPop = 0x6CA5C0;      // SOUND_FAIRY_POP
constexpr uintptr_t kSoundWoosh = 0x6CAA20;    // SOUND_TEXT_WOOSH
constexpr uintptr_t kSoundBuzz = 0x6CA6A0;     // SOUND_LOGO_BUZZ_ON (the PopCap logo's neon)
constexpr uintptr_t kSoundPowerUp = 0x6CA8B8;  // SOUND_POWERUP
constexpr uintptr_t kSoundFireballBounce = 0x6CAAD8;  // SOUND_FIREBALLBOUNCE

// A text floating up over the board, as the game names a power ("Spooky Ball!") when its peg is hit.
constexpr uintptr_t kShowText = 0x468DD0;  // EAX LogicMgr, EDI font (-1: the usual), stdcall(const string*, x, y)
constexpr int kTextOffColor = 0x60, kTextOffLife = 0x68, kTextOffRise = 0x8C;  // RGB, ticks, pixels a tick
constexpr int kTextOff6C = 0x6C, kTextOff78 = 0x78;  // (what a power's text sets them to)

template <typename T>
T& F(void* p, int off)
{
    return game::At<T>(p, off);
}

template <typename Fn>
Fn VCall(void* obj, int slot)
{
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(obj))[slot]);
}

void* Logic(void* board) { return board ? F<void*>(board, kBoardOffLogic) : nullptr; }
void* PegInfo(void* obj) { return F<void*>(obj, kObjOffPegInfo); }
int Type(void* peg) { return F<int>(PegInfo(peg), kPegOffType); }
bool Lit(void* peg) { return F<uint8_t>(PegInfo(peg), kPegOffLit) != 0; }
bool Unhit(void* peg) { return F<uint8_t>(peg, kObjOffAlive) && !Lit(peg); }
std::string Id(void* obj) { return game::ReadString(static_cast<char*>(obj) + kObjOffId); }
bool Fever(void* board) { return F<uint8_t>(Logic(board), kLogicOffFever) != 0; }

struct Node {
    Node* next;
    Node* prev;
    void* obj;
};

std::vector<void*> List(void* owner, int head_off)
{
    std::vector<void*> out;
    Node* head = F<Node*>(owner, head_off);
    for (Node* n = head ? head->next : nullptr; n && n != head; n = n->next)
        if (n->obj)
            out.push_back(n->obj);
    return out;
}

std::vector<void*> Balls(void* board) { return List(board, kBoardOffBalls); }

// Every peg in the level's order (the same on both computers).
std::vector<void*> Pegs(void* lm)
{
    std::vector<void*> out;
    for (void* obj : List(lm, kLogicOffPegList))
        if (PegInfo(obj))
            out.push_back(obj);
    return out;
}

std::vector<void*> UnhitPegs(void* lm, int type = 0)
{
    std::vector<void*> out;
    for (void* p : Pegs(lm))
        if (Unhit(p) && (!type || Type(p) == type))
            out.push_back(p);
    return out;
}

int IndexOf(const std::vector<void*>& v, void* p)
{
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == p)
            return static_cast<int>(i);
    return -1;
}

uint32_t Bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// ---- Calling the game ----
GAME_CALL void SetPegType(void* peg, int kind)
{
    __asm {
        mov ecx, peg
        mov edx, kind
        mov eax, kSetPegType
        call eax
    }
}

GAME_CALL void Flash(void* peg)
{
    void* info = PegInfo(peg);
    __asm {
        mov eax, info
        mov ecx, 2
        mov edx, kPegFlash
        call edx
    }
}

GAME_CALL int SetErase(void* set, void* peg)
{
    void* key = peg;
    int n;
    __asm {
        mov esi, set
        lea ecx, key
        mov eax, kSetErase
        call eax
        mov n, eax
    }
    return n;
}

GAME_CALL void SetInsert(void* set, void* peg)
{
    void* key = peg;
    uint8_t result[12];
    __asm {
        lea eax, key
        push eax
        push set
        lea ebx, result
        mov eax, kSetInsert
        call eax
    }
}

// A new color, kept in the right one of the LogicMgr's sets (the oranges' count drives the game).
void Recolor(void* lm, void* peg, int type)
{
    int old = Type(peg);
    if (old == type)
        return;
    char* oranges = static_cast<char*>(lm) + kLogicOffOranges;
    char* others = static_cast<char*>(lm) + kLogicOffOthers;
    if ((old == kOrange) != (type == kOrange)) {
        SetErase(old == kOrange ? oranges : others, peg);
        SetPegType(peg, type);
        SetInsert(type == kOrange ? oranges : others, peg);
    } else {
        SetPegType(peg, type);
    }
}

GAME_CALL void SetRadius(void* ball, float r)
{
    __asm {
        push r
        mov eax, ball
        mov ecx, kSetRadius
        call ecx
    }
}

GAME_CALL void Teleport(void* ball, float x, float y)
{
    __asm {
        push y
        push x
        mov esi, ball
        mov eax, kTeleport
        call eax
    }
}

void SetPos(void* obj, float x, float y) { VCall<void(__thiscall*)(void*, float, float)>(obj, kObjSlotSetPos)(obj, x, y); }

void SetVelocity(void* obj, float vx, float vy)
{
    VCall<void(__thiscall*)(void*, float, float)>(obj, kObjSlotSetVelocity)(obj, vx, vy);
}

void Release(void* obj)
{
    if (InterlockedDecrement(&F<LONG>(obj, kObjOffRefs)) <= 0)
        VCall<void(__thiscall*)(void*, int)>(obj, 0)(obj, 1);
}

// A new ball on the board, as Multiball makes one.
GAME_CALL void AddBall(void* board, void* ball)
{
    void* pm = F<void*>(board, kBoardOffPhysics);
    void* axis1 = static_cast<char*>(pm) + 4;
    void* axis2 = static_cast<char*>(pm) + 0x14;
    __asm {
        push axis1
        push pm
        mov eax, kSortAxes
        call eax
        push axis2
        push pm
        mov eax, kSortAxes
        call eax
    }
    reinterpret_cast<void(__thiscall*)(void*, void*)>(kAddObject)(board, ball);
}

GAME_CALL void* NewBall()
{
    void* b = game::GameNew(kBallSize);
    __asm {
        mov esi, b
        xor ebx, ebx
        mov eax, kBallCtor
        call eax
    }
    InterlockedIncrement(&F<LONG>(b, kObjOffRefs));
    return b;
}

GAME_CALL void SetGunAngle(void* gun, float a, bool immediate)
{
    int imm = immediate ? 1 : 0;
    __asm {
        push imm
        push a
        mov eax, gun
        mov ecx, kSetGunAngle
        call ecx
    }
}

GAME_CALL void Catch(void* lm, void* hole, void* ball)
{
    __asm {
        push ball
        mov esi, hole
        mov ecx, lm
        mov eax, kCatch
        call eax
    }
}

GAME_CALL void CopyBallKind(void* ball)
{
    __asm {
        push 1
        push ball
        mov eax, kCopyBallKind
        call eax
    }
}

// Whether the ball touches the peg soon: within a share of its next move, or a margin.
GAME_CALL bool NearPeg(void* ball, void* peg, float share, float margin)
{
    uint8_t near_peg;
    __asm {
        push margin
        push share
        mov eax, peg
        mov ecx, ball
        mov edx, kNearPeg
        call edx
        mov near_peg, al
    }
    return near_peg != 0;
}

GAME_CALL void StartSlowMo(void* lm)
{
    __asm {
        mov ebx, lm
        mov eax, kStartSlowMo
        call eax
    }
}

GAME_CALL void SetSlowMo(void* board, int speed)
{
    __asm {
        mov edi, board
        mov al, 1
        mov ecx, speed
        mov edx, kSetSlowMo
        call edx
    }
}

GAME_CALL void SwitchPower(void* lm, int power, bool on)
{
    int on_arg = on;
    __asm {
        push on_arg
        push power
        push lm
        mov eax, kSwitchPower
        call eax
    }
}

// What a turn's beginning does for these powers, at once (testing).
GAME_CALL void PowerTurnBegins(void* lm, int power, bool on)
{
    void* gun = F<void*>(F<void*>(lm, kLogicOffBoard), kBoardOffGun);
    int on_arg = on;
    if (power == kPowerPyramid) {
        __asm {
            push on_arg
            push lm
            mov eax, kSwitchPyramid
            call eax
        }
    } else if (power == kPowerLightning && on && gun) {
        __asm {
            mov esi, gun
            mov eax, kChargeBall
            call eax
        }
    } else if (power == kPowerFireball && on && gun) {
        __asm {
            mov eax, gun
            mov ecx, kFireBall
            call ecx
        }
    }
}

// ---- The effects in play: a round's and a red peg's ----
uint32_t g_active;               // bits by Effect
uint32_t g_ticks[kEffectCount];  // board ticks since each began
void* g_board;                   // the board they're on

bool On(Effect e) { return (g_active >> e & 1) != 0; }
uint32_t Ticks(Effect e) { return g_ticks[e]; }

// Red Peg Mode's red peg, on the duel's board (chaos.cpp decides when there is one).
void* g_red;
int g_red_index = -1;      // in the level's order (the same on both computers)
uint32_t g_red_clock;      // board ticks since it was chosen
uint32_t g_red_hit_at;     // ... when it was lit (0: not yet)
float g_red_x, g_red_y;    // where it was then

// Diagnostics (testing): how often the hooks ran.
uint32_t g_n_under, g_n_over, g_n_steps, g_n_portal, g_n_wrap, g_n_kicks, g_n_twins, g_n_catches, g_n_slow, g_n_jumps;
uint32_t g_n_blind;     // guide drawings left out
uint32_t g_n_glitches;  // fake pegs the ball went through
uint32_t g_n_gray, g_n_red;  // pegs drawn gray, drawn red

bool Ours(void* lm) { return g_board && lm == Logic(g_board); }

// Whether a peg is one of the LogicMgr's (walking its list: no allocation, which could differ between the two
// computers inside the simulation).
bool HasPeg(void* lm, void* peg)
{
    Node* head = F<Node*>(lm, kLogicOffPegList);
    for (Node* n = head ? head->next : nullptr; n && n != head; n = n->next)
        if (n->obj == peg)
            return true;
    return false;
}

// The unhit blue pegs but the red one (Purple Party's and the red peg's choices).
std::vector<void*> BluePegs(void* lm)
{
    std::vector<void*> blue = UnhitPegs(lm, kBlue);
    blue.erase(std::remove(blue.begin(), blue.end(), g_red), blue.end());
    return blue;
}

// ---- Purple Party: two more purple pegs each turn ----
constexpr int kExtraPurples = 2;
std::vector<void*> g_extra_purples;

void ClearExtraPurples()
{
    for (void* p : g_extra_purples)
        if (Unhit(p) && Type(p) == kPurple)
            SetPegType(p, kBlue);
    g_extra_purples.clear();
}

void AddExtraPurples(void* lm)
{
    std::vector<void*> blue = BluePegs(lm);
    for (int i = 0; i < kExtraPurples && !blue.empty(); ++i) {
        size_t k = Random(static_cast<uint32_t>(blue.size()));
        void* p = blue[k];
        blue.erase(blue.begin() + k);
        SetPegType(p, kPurple);
        Flash(p);
        g_extra_purples.push_back(p);
    }
}

using ChoosePurpleFn = void(__stdcall*)(void* lm);
ChoosePurpleFn g_orig_choose_purple;

void __stdcall HookChoosePurple(void* lm)
{
    bool party = On(kPurpleParty) && Ours(lm);
    if (party)
        ClearExtraPurples();  // else the game takes one of them as its purple instead of choosing
    // The red peg stays red: while the game chooses (from the blue pegs, or another purple one) it isn't blue.
    int* red_type = nullptr;
    if (g_red && HasPeg(lm, g_red) && Unhit(g_red) && Type(g_red) == kBlue) {
        red_type = &F<int>(PegInfo(g_red), kPegOffType);
        *red_type = kGreen;
    }
    g_orig_choose_purple(lm);
    if (red_type)
        *red_type = kBlue;
    if (party)
        AddExtraPurples(lm);
}

// ---- Peg Shuffle: the unhit pegs' colors change places (the purple one and the red one stay) ----
void ShufflePegs(void* lm)
{
    std::vector<void*> pegs;
    std::vector<int> types;
    for (void* p : UnhitPegs(lm)) {
        if (Type(p) == kPurple || p == g_red)
            continue;
        pegs.push_back(p);
        types.push_back(Type(p));
    }
    for (size_t i = types.size(); i > 1; --i) {
        size_t j = Random(static_cast<uint32_t>(i));
        int t = types[i - 1];
        types[i - 1] = types[j];
        types[j] = t;
    }
    int changed = 0;
    for (size_t i = 0; i < pegs.size(); ++i) {
        if (Type(pegs[i]) == types[i])
            continue;
        Recolor(lm, pegs[i], types[i]);
        Flash(pegs[i]);
        ++changed;
    }
    Log("chaos: shuffled %d pegs, %d changed color", static_cast<int>(pegs.size()), changed);
}

bool ShufflePossible(void* lm)
{
    int first = 0;
    for (void* p : UnhitPegs(lm)) {
        int t = Type(p);
        if (t == kPurple || p == g_red)
            continue;
        if (!first)
            first = t;
        else if (t != first)
            return true;
    }
    return false;
}

// ---- Bomb Peg: one peg blows up whatever is around it when it's hit ----
void* g_bomb;
int g_bomb_index = -1;
bool g_bomb_blown;

GAME_CALL void SpaceBlast(void* lm, void* ball, void* peg)
{
    __asm {
        mov eax, peg
        push ball
        push lm
        mov ecx, kSpaceBlast
        call ecx
    }
}

using PegHitFn = int(__thiscall*)(void* lm, void* ball, void* peg, int indirect);
PegHitFn g_orig_peg_hit;

int __fastcall HookPegHit(void* lm, void*, void* ball, void* peg, int indirect)
{
    bool bomb = On(kBombPeg) && peg == g_bomb && !g_bomb_blown && Ours(lm) && !Lit(peg);
    int r = g_orig_peg_hit(lm, ball, peg, indirect);
    if (bomb && Lit(peg)) {
        g_bomb_blown = true;
        Log("chaos: the bomb peg went off");
        SpaceBlast(lm, ball, peg);
    }
    return r;
}

// ---- Big Balls: every ball 50% bigger (a fireball is bigger to start with) ----
struct Enlarged {
    void* ball;
    float from, to;
};
std::vector<Enlarged> g_enlarged;

std::vector<void*> BallsAndLoaded(void* board)
{
    std::vector<void*> balls = Balls(board);
    void* gun = F<void*>(board, kBoardOffGun);
    if (gun && F<void*>(gun, kGunOffBall))
        balls.push_back(F<void*>(gun, kGunOffBall));
    return balls;
}

void EnlargeBalls(void* board)
{
    std::vector<void*> balls = BallsAndLoaded(board);
    std::vector<Enlarged> kept;  // (a ball that went, or that the game resized since, is new to us)
    for (const Enlarged& e : g_enlarged)
        if (IndexOf(balls, e.ball) >= 0 && F<float>(e.ball, kBallOffRadius) == e.to)
            kept.push_back(e);
    g_enlarged.swap(kept);
    for (void* b : balls) {
        bool known = false;
        for (const Enlarged& e : g_enlarged)
            known = known || e.ball == b;
        if (known)
            continue;
        float r = F<float>(b, kBallOffRadius);
        g_enlarged.push_back({b, r, r * 1.5f});
        SetRadius(b, r * 1.5f);
    }
}

void ShrinkBalls(void* board)
{
    std::vector<void*> balls = BallsAndLoaded(board);
    for (const Enlarged& e : g_enlarged)
        if (IndexOf(balls, e.ball) >= 0 && F<float>(e.ball, kBallOffRadius) == e.to)
            SetRadius(e.ball, e.from);
    g_enlarged.clear();
}

// ---- Pinball Madness: pegs kick the ball, and it may go faster than the game allows ----
double g_speed_cap_sq = 225, g_speed_cap = 15;  // the game's own, patched to be read from here
constexpr float kKick = 1.3f, kMinKickSpeedSq = 5.0f * 5.0f;

void __cdecl CollisionCallback(void* a, void* b)
{
    void* ball = F<int>(a, kObjOffKind) == kKindBall ? a : b;
    void* obj = ball == a ? b : a;
    if (On(kPinballMadness) && g_board && F<int>(ball, kObjOffKind) == kKindBall && obj && PegInfo(obj) &&
        !F<uint8_t>(ball, kBallOffFireball)) {
        float vx = F<float>(ball, kBallOffVX) * kKick, vy = F<float>(ball, kBallOffVY) * kKick;
        float sq = vx * vx + vy * vy;
        if (sq < kMinKickSpeedSq && sq > 0.0001f) {
            float k = sqrtf(kMinKickSpeedSq / sq);  // (correctly rounded: the same on every computer)
            vx *= k;
            vy *= k;
        }
        F<float>(ball, kBallOffVX) = vx;
        F<float>(ball, kBallOffVY) = vy;
        ++g_n_kicks;
    }
    reinterpret_cast<void(__cdecl*)(void*, void*)>(kRecordCollision)(a, b);
}

// ---- Double Shot: a second ball beside the first ----
constexpr float kTwinCos = 0.99619470f, kTwinSin = 0.08715574f;  // 5 degrees each way

void Rotate(float& x, float& y, float s)
{
    float nx = x * kTwinCos - y * s, ny = x * s + y * kTwinCos;
    x = nx;
    y = ny;
}

bool g_spooky_next;  // testing (chaos spooky): the next shot is a Spooky Ball, as if Renfield's power were on

void __cdecl OnShotFired(void* lm, void* ball)
{
    if (g_spooky_next && ball) {
        g_spooky_next = false;
        F<uint8_t>(ball, kBallOffSpooky) = 1;
        F<int>(lm, kLogicOffSpookies + 4 * F<int>(lm, kLogicOffPlayer)) = 1;
    }
    if (!On(kDoubleShot) || !Ours(lm) || !ball)
        return;
    float vx = F<float>(ball, kBallOffVX), vy = F<float>(ball, kBallOffVY);
    float vx2 = vx, vy2 = vy;
    Rotate(vx, vy, -kTwinSin);
    Rotate(vx2, vy2, kTwinSin);
    SetVelocity(ball, vx, vy);
    void* twin = NewBall();
    SetPos(twin, F<float>(ball, kBallOffX), F<float>(ball, kBallOffY));
    SetVelocity(twin, vx2, vy2);
    if (F<float>(ball, kBallOffRadius) != kBallRadius)
        SetRadius(twin, F<float>(ball, kBallOffRadius));
    F<uint8_t>(ball, kBallOffHitsBalls) = 0;  // they start in the same place
    F<uint8_t>(twin, kBallOffHitsBalls) = 0;
    F<uint8_t>(twin, kBallOffFireball) = F<uint8_t>(ball, kBallOffFireball);
    AddBall(g_board, twin);
    if (F<int>(ball, kBallOffCompanion) || F<uint8_t>(ball, kBallOffGhost))
        CopyBallKind(twin);
    Release(twin);  // the board holds it now
    ++g_n_twins;
}

uintptr_t g_orig_shot_bookkeeping;

__declspec(naked) void ShotFiredHook()  // replaces a call; EBX = LogicMgr, the new ball at [ebp-0x14]
{
    __asm {
        pushad
        push dword ptr [ebp - 0x14]
        push ebx
        call OnShotFired
        add esp, 8
        popad
        jmp dword ptr [g_orig_shot_bookkeeping]
    }
}

__declspec(naked) void ZenShotFiredHook()  // the same, the ball at the caller's [esp+0x18]
{
    __asm {
        pushad
        push dword ptr [esp + 0x20 + 4 + 0x18]  // (past what pushad saved and the return address)
        push ebx
        call OnShotFired
        add esp, 8
        popad
        jmp dword ptr [g_orig_shot_bookkeeping]
    }
}

// ---- Wrap Around: no side walls ----
void* g_walls[3];  // left, right, ceiling

void FindWalls(void* board)
{
    g_walls[0] = g_walls[1] = g_walls[2] = nullptr;
    for (void* obj : List(board, kBoardOffObjects)) {
        if (Id(obj) != "wall")
            continue;
        float cx = (F<float>(obj, kObjOffLeft) + F<float>(obj, kObjOffRight)) / 2;
        g_walls[cx < 0 ? 0 : cx > kFieldW ? 1 : 2] = obj;
    }
}

void SetCollides(void* obj, bool on)
{
    if (obj)
        F<uint8_t>(obj, kObjOffCollides) = on;
}

// ---- Wormholes and Portal Storm ----
struct Portal {
    float x, y;
};
constexpr float kPortalR = 17;
Portal g_portals[6];
int g_portal_count;
struct Cooldown {
    void* ball;
    int portal;  // the one it came out of: it doesn't go back in until it has left
};
Cooldown g_cooldown[8];

float RectDistSq(void* obj, float x, float y)
{
    float l = F<float>(obj, kObjOffLeft), t = F<float>(obj, kObjOffTop);
    float r = F<float>(obj, kObjOffRight), b = F<float>(obj, kObjOffBottom);
    float dx = x < l ? l - x : x > r ? x - r : 0;
    float dy = y < t ? t - y : y > b ? y - b : 0;
    return dx * dx + dy * dy;
}

void PlacePortals(void* board, int pairs)
{
    std::vector<void*> pegs = UnhitPegs(Logic(board));
    g_portal_count = 0;
    float gap = pairs > 1 ? 105.0f : 160.0f;
    for (int i = 0; i < 2 * pairs; ++i) {
        Portal best = {kFieldW / 2, kFieldH / 2};
        float best_clear = -1;
        for (int attempt = 0; attempt < 300; ++attempt) {
            Portal p = {50.0f + Random(551), 135.0f + Random(321)};
            bool apart = true;
            for (int j = 0; j < g_portal_count; ++j) {
                float dx = p.x - g_portals[j].x, dy = p.y - g_portals[j].y;
                apart = apart && dx * dx + dy * dy >= gap * gap;
            }
            if (!apart)
                continue;
            float clear = 1e9f;  // squared distance to the nearest peg
            for (void* peg : pegs) {
                float d = RectDistSq(peg, p.x, p.y);
                if (d < clear)
                    clear = d;
            }
            if (clear > best_clear) {
                best_clear = clear;
                best = p;
            }
            if (clear >= (kPortalR + 8) * (kPortalR + 8))
                break;
        }
        g_portals[g_portal_count++] = best;
    }
    for (Cooldown& c : g_cooldown)
        c = {nullptr, -1};
    Log("chaos: %d portals", g_portal_count);
}

Cooldown* CooldownOf(void* ball)
{
    Cooldown* free_slot = nullptr;
    for (Cooldown& c : g_cooldown) {
        if (c.ball == ball)
            return &c;
        if (!c.ball && !free_slot)
            free_slot = &c;
    }
    if (free_slot)
        *free_slot = {ball, -1};
    return free_slot;
}

void ThroughPortals(void* ball)
{
    Cooldown* cd = CooldownOf(ball);
    float x = F<float>(ball, kBallOffX), y = F<float>(ball, kBallOffY);
    for (int i = 0; i < g_portal_count; ++i) {
        float dx = x - g_portals[i].x, dy = y - g_portals[i].y;
        float d = dx * dx + dy * dy;
        if (cd && cd->portal == i) {
            if (d > (kPortalR + 6) * (kPortalR + 6))
                cd->portal = -1;  // out of it: it works again
            continue;
        }
        if (d >= kPortalR * kPortalR)
            continue;
        int to = i ^ 1;
        Teleport(ball, g_portals[to].x + dx, g_portals[to].y + dy);
        ++g_n_portal;
        if (cd)
            cd->portal = to;
        ui::PlaySound(kSoundPop);
        return;
    }
}

// ---- The bucket: Double Bucket, Teleporting Bucket, and Upside Down's ----
std::vector<void*> g_bucket;  // its parts: the catcher and its lips, the pyramid version, the lid
void* g_hole;                 // the catcher
void* g_lid;                  // the lid (a Brick): on for a Fireball's turn (a fireball knocks it off), a Spooky Ball's
bool g_flipped;               // Upside Down (below): the bucket along the top

void FindBucket(void* board)
{
    g_bucket.clear();
    g_hole = g_lid = nullptr;
    for (void* obj : List(board, kBoardOffObjects)) {
        std::string id = Id(obj);
        if (id != "freeball" && id != "bumperhole" && id != "freeballcover")
            continue;
        if (!F<void*>(obj, kObjOffMover))
            continue;
        g_bucket.push_back(obj);
        if (!g_hole && id == "freeball" && F<int>(obj, kObjOffKind) == kKindHole)
            g_hole = obj;
        if (id == "freeballcover")
            g_lid = obj;
    }
}

void* Mover(void* part) { return F<void*>(part, kObjOffMover); }
bool LidOn() { return g_lid && F<uint8_t>(g_lid, kObjOffCollides); }

// Double Bucket: the bucket keeps to the left half of its track, a copy of it (drawn by us, catching with the
// game's own catch) to the right half, side by side. The game eases the track's width towards a goal it sets each
// turn (narrower with Flippers or the Pyramid): halved too while the copy is there.
constexpr float kTwinOffset = 260;
std::vector<void*> g_pending_catches;  // balls in the copy, caught after the step's ball loop

int TrackGoal(void* lm)  // the width the game wants for the player in turn
{
    int p = F<int>(lm, kLogicOffPlayer);
    if (F<int>(lm, kLogicOffPowerTurns + 8 * kPowerFlippers + 4 * p))
        return 130;
    return F<int>(lm, kLogicOffPowerTurns + 8 * kPowerPyramid + 4 * p) ? 200 : 260;
}

void SplitBucket(bool on)
{
    if (Fever(g_board))  // (Extreme Fever has taken the bucket away along a track of its own)
        return;
    for (void* part : g_bucket) {
        void* m = Mover(part);
        F<float>(m, kMoverOffBaseX) += on ? -kTwinOffset / 2 : kTwinOffset / 2;
        F<int>(m, kMoverOffRadius) = on ? F<int>(m, kMoverOffRadius) / 2 : F<int>(m, kMoverOffRadius) * 2;
    }
    void* lm = Logic(g_board);
    int& goal = F<int>(lm, kLogicOffTrackGoal);
    if (goal)
        goal = on ? TrackGoal(lm) / 2 : TrackGoal(lm);
}

// The game eases the width each logic step it has a goal, before the effects' step: the goal it set (it only sets
// whole widths) is halved as it gets there.
constexpr uintptr_t kEaseTrack = 0x47727D;  // call: stdcall(LogicMgr), when LogicMgr+0x1B8 is set
uintptr_t g_orig_ease_track;

void __stdcall EaseTrackHook(void* lm)
{
    int& goal = F<int>(lm, kLogicOffTrackGoal);
    if (On(kDoubleBucket) && Ours(lm) && !Fever(g_board) && goal != TrackGoal(lm) / 2)
        goal = TrackGoal(lm) / 2;
    reinterpret_cast<void(__stdcall*)(void*)>(g_orig_ease_track)(lm);
}

bool InTwinBucket(void* ball)
{
    if (!g_hole || !F<uint8_t>(g_hole, kObjOffAlive) || Fever(g_board) || LidOn())
        return false;
    float hx = F<float>(g_hole, kHoleOffX) + kTwinOffset, hy = F<float>(g_hole, kHoleOffY);
    float hw = F<int>(g_hole, kHoleOffW) / 2.0f, hh = F<int>(g_hole, kHoleOffH) / 2.0f;
    float x = F<float>(ball, kBallOffX), y = F<float>(ball, kBallOffY);
    return x >= hx - hw && x <= hx + hw && y >= hy - hh && y <= hy + hh;
}

// A catch shows its texts (a bonus, "Bucket Bonus!") 60 pixels over the bucket: upside down that would be off the
// board, so they go 60 under it, the bucket's place moved down for the catch (as the copy's is moved along).
constexpr float kCatchTextDrop = 120;
constexpr uintptr_t kCatchCall = 0x471906;  // call: the step's catches, ECX LogicMgr, ESI hole, stdcall(ball)
uint32_t g_n_bucket_catches;  // testing: catches in the bucket upside down

void CatchAt(void* lm, void* hole, void* ball, float dx)
{
    float& x = F<float>(hole, kHoleOffX);
    float& y = F<float>(hole, kHoleOffY);
    float dy = g_flipped ? kCatchTextDrop : 0;
    x += dx;
    y += dy;
    Catch(lm, hole, ball);
    x -= dx;
    y -= dy;
}

uintptr_t g_orig_catch;
void __cdecl CatchUpsideDown(void* lm, void* hole, void* ball)
{
    if (hole == g_hole && Ours(lm)) {
        ++g_n_bucket_catches;
        CatchAt(lm, hole, ball, 0);
    } else {
        Catch(lm, hole, ball);
    }
}

__declspec(naked) void CatchHook()  // replaces a call: ECX LogicMgr, ESI hole, stdcall(ball)
{
    __asm {
        cmp g_flipped, 0
        je original
        push dword ptr [esp + 4]
        push esi
        push ecx
        call CatchUpsideDown
        add esp, 12
        ret 4
    original:
        jmp dword ptr [g_orig_catch]
    }
}

void CatchPending()
{
    std::vector<void*> balls;
    balls.swap(g_pending_catches);
    for (void* ball : balls) {
        if (On(kDoubleBucket) && g_hole && IndexOf(Balls(g_board), ball) >= 0) {
            CatchAt(Logic(g_board), g_hole, ball, kTwinOffset);  // its effects where the ball fell in
            ++g_n_catches;
            Log("chaos: a ball fell into the second bucket");
        }
        Release(ball);
    }
}

// The copy collides as the bucket does: the bucket's parts that collide (its lips, and while they're on the
// pyramid's sides and the lid) have their edges 260 pixels along too, and a ball moving into one bounces off it the
// way the game bounces balls off Polys (the part's bounce, a slow ball sliding along). A fireball meeting the lid
// knocks it off as on the bucket (one lid: both lose it). Parts whose place there the bucket's own parts already
// take are left out (the pyramid's sides cross there: both would make a dip a ball could rest in).
constexpr int kPolyOffBounce = 0x34, kPolyOffSlide = 0x38, kPolyOffMaxBounce = 0x3C;  // (0: 0.82, 1.4, none)
uint32_t g_n_twin_bounces, g_n_lid_knocks;  // testing

bool Collider(void* part)
{
    return part != g_hole && F<int>(part, kObjOffKind) == 5 && F<uint8_t>(part, kObjOffCollides);
}

bool BounceOffEdges(void* ball, void* part, float dx)  // the part dx along: whether the ball bounced off it
{
    float r = F<float>(ball, kBallOffRadius);
    float bx = F<float>(ball, kBallOffX), by = F<float>(ball, kBallOffY);
    if (bx + r < F<float>(part, kObjOffLeft) + dx || bx - r > F<float>(part, kObjOffRight) + dx ||
        by + r < F<float>(part, kObjOffTop) || by - r > F<float>(part, kObjOffBottom))
        return false;
    const float* p = F<float*>(part, kPolyOffPoints);
    const float* end = F<float*>(part, kPolyOffPoints + 4);
    float px = F<float>(part, kPolyOffX) + dx, py = F<float>(part, kPolyOffY);
    float best = r, cx = 0, cy = 0, nx = 0, ny = 0;
    for (; p && p + 3 < end; p += 2) {  // the nearest edge it overlaps
        float x0 = px + p[0], y0 = py + p[1], ex = p[2] - p[0], ey = p[3] - p[1], len2 = ex * ex + ey * ey;
        if (len2 <= 0)
            continue;
        float t = ((bx - x0) * ex + (by - y0) * ey) / len2;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        float qx = x0 + t * ex, qy = y0 + t * ey, d = sqrtf((bx - qx) * (bx - qx) + (by - qy) * (by - qy));
        if (d >= best)
            continue;
        float len = sqrtf(len2), ox = -ey / len, oy = ex / len;  // its outward normal (the game's turn of points)
        best = d;
        cx = qx;
        cy = qy;
        if (d > 0.001f && (bx - qx) * ox + (by - qy) * oy > 0) {
            nx = (bx - qx) / d;
            ny = (by - qy) / d;
        } else {  // (its centre past the edge: out the way the edge faces)
            nx = ox;
            ny = oy;
        }
    }
    if (best >= r)
        return false;
    float vx = F<float>(ball, kBallOffVX), vy = F<float>(ball, kBallOffVY), vn = vx * nx + vy * ny;
    if (vn >= 0)
        return false;  // (on its way out already)
    float bounce = F<float>(part, kPolyOffBounce), slide = F<float>(part, kPolyOffSlide);
    if (-vn * (fabsf(nx) > 0.99f ? 5 : 1) <= (slide > 0 ? slide : 1.4f)) {  // too slow: it slides along
        vx -= vn * nx;
        vy -= vn * ny;
    } else {
        float e = bounce > 0 ? bounce : 0.82f;
        vx = e * (vx - 2 * vn * nx);
        vy = e * (vy - 2 * vn * ny);
    }
    float cap = F<float>(part, kPolyOffMaxBounce), speed = sqrtf(vx * vx + vy * vy);
    if (cap > 0 && speed > cap) {
        vx *= cap / speed;
        vy *= cap / speed;
    }
    SetPos(ball, cx + nx * (r + 0.05f), cy + ny * (r + 0.05f));
    SetVelocity(ball, vx, vy);
    return true;
}

bool TakenByBucket(void* part)  // the copy's part where the bucket's own colliding parts are
{
    for (void* own : g_bucket)
        if (Collider(own) && F<float>(part, kObjOffLeft) + kTwinOffset < F<float>(own, kObjOffRight) &&
            F<float>(part, kObjOffRight) + kTwinOffset > F<float>(own, kObjOffLeft))
            return true;
    return false;
}

void TwinColliders(void* ball)
{
    if (!g_hole || Fever(g_board))
        return;
    for (void* part : g_bucket) {
        if (!Collider(part) || TakenByBucket(part) || !BounceOffEdges(ball, part, kTwinOffset))
            continue;
        ++g_n_twin_bounces;
        if (part == g_lid && F<uint8_t>(ball, kBallOffFireball)) {
            F<uint8_t>(ball, kBallOffKnockedLid) = 1;
            void* challenge = F<void*>(g_board, kBoardOffChallenge);
            if (!challenge || !F<uint8_t>(challenge, kChallengeOffCovered))
                VCall<void(__thiscall*)(void*, int)>(g_lid, kObjSlotSetActive)(g_lid, 0);
            ui::PlaySound(kSoundFireballBounce);
            ++g_n_lid_knocks;
            Log("chaos: a fireball knocked the lid off at the second bucket");
        }
        return;  // (one bounce a step)
    }
}

uintptr_t g_orig_hit_processing;
void __cdecl OnPhysicsStep(void* lm);  // (below: what the effects do each step)

__declspec(naked) void HitProcessingHook()  // replaces a call: stdcall(LogicMgr)
{
    __asm {
        pushad
        push dword ptr [esp + 36]
        call OnPhysicsStep
        add esp, 4
        popad
        jmp dword ptr [g_orig_hit_processing]
    }
}

// Teleporting Bucket: it jumps along its track every few seconds.
constexpr uint32_t kTeleportEvery = 260;
uint32_t g_next_teleport;
uint32_t g_teleported_at = 0xFFFFFFFF;
float g_teleport_from, g_teleport_to;

void TeleportBucket(void* board)
{
    if (!g_hole || Fever(board) || Ticks(kTeleportingBucket) < g_next_teleport)
        return;
    // Not while a ball is as low as the bucket (as high, upside down): a part landing on it, where the bucket is or
    // where it goes, would fling it (the pyramid's sides reach 200 pixels either way).
    for (void* ball : Balls(board)) {
        float dy = F<float>(ball, kBallOffY) - F<float>(g_hole, kHoleOffY);
        if (dy * dy < 75.0f * 75.0f)
            return;
    }
    uint32_t jump = 120 + Random(361);  // ticks along its 600-tick track
    for (void* part : g_bucket)
        F<int>(Mover(part), kMoverOffTicks) += static_cast<int>(jump);
    g_teleport_from = F<float>(g_hole, kHoleOffX);
    g_teleported_at = Ticks(kTeleportingBucket);
    g_next_teleport = Ticks(kTeleportingBucket) + kTeleportEvery;
    ++g_n_jumps;
    ui::PlaySound(kSoundPop);
    Log("chaos: the bucket jumped %u ticks along its track (from x %.0f)", jump, g_teleport_from);
}

// ---- Upside Down: gravity up, the cannon at the bottom, the bucket at the top ----
float g_spooky_y = 10;  // where a Spooky Ball comes back (the game's 10, read from here)

// The part in the game's code (the board's objects are turned over by Flip, and kept so in its copies). The
// computer opponent's guesses go with it: where Chain Lightning's rod is, when to flip Claude's flippers.
void SetFlipped(bool on)
{
    if (g_flipped == on)
        return;
    g_flipped = on;
    uint8_t op = on ? 0x25 : 0x05, rod_op = on ? 0x05 : 0x25;
    PatchBytes(kGravityOp, &op, 1);
    PatchBytes(kAiRodOp, &rod_op, 1);
    uint32_t low = on ? 116 : 400, high = on ? 266 : 550;  // (mirrored: 76 < y < 226 for 360 < y < 510)
    PatchBytes(kAiFlipperLow, &low, 4);
    PatchBytes(kAiFlipperHigh, &high, 4);
    g_spooky_y = on ? kFieldH - 10 : 10;
}

// A shape turned over about its own position: its points' y the other way, then their order reversed too, so that
// its edges keep facing out (the game's collisions take one side of each).
GAME_CALL void ReverseShape(void* poly)
{
    __asm {
        push poly
        mov eax, kReverseShape
        call eax
    }
}

void MirrorShape(void* poly)
{
    float* p = F<float*>(poly, kPolyOffPoints);
    float* end = F<float*>(poly, kPolyOffPoints + 4);
    if (!p || end - p < 4)
        return;
    for (; p + 1 < end; p += 2)
        p[1] = -p[1];
    ReverseShape(poly);
    F<uint8_t>(poly, kPolyOffTurned) = 1;
    F<uint8_t>(poly, kPolyOffMoved) = 1;
}

// Claude's flippers (Polys turning about a pivot) at the top: the pivot's height, the shape, the way it turns and
// the turn so far all mirrored, so the swing goes down and the push off it is mirrored too (and the drawing,
// FlipperDraw). The picture's offset as well, for drawing without 3D acceleration.
bool IsFlipper(void* obj) { return Mover(obj) && Id(obj) == "flipper"; }

void MirrorFlipper(void* flipper)
{
    void* m = Mover(flipper);
    F<int>(m, kMoverOffType) = -F<int>(m, kMoverOffType);
    F<float>(m, kMoverOffTurn) = -F<float>(m, kMoverOffTurn);
    F<float>(m, kMoverOffAngle) = -F<float>(m, kMoverOffAngle);
    F<float>(m, kMoverOffBaseY) = kBucketMirror - F<float>(m, kMoverOffBaseY);
    F<float>(m, kMoverOffY) = kBucketMirror - F<float>(m, kMoverOffY);
    F<float>(flipper, kPolyOffY) = kBucketMirror - F<float>(flipper, kPolyOffY);
    F<float>(flipper, kPolyOffAngle) = -F<float>(flipper, kPolyOffAngle);
    F<float>(flipper, kPolyOffSpin) = -F<float>(flipper, kPolyOffSpin);
    F<float>(flipper, kPolyOffDrawAngle) = -F<float>(flipper, kPolyOffDrawAngle);
    F<float>(flipper, kPolyOffPictureY) = -F<float>(flipper, kPolyOffPictureY);
    MirrorShape(flipper);
}

// The bucket's parts that only collide (drawn ones are turned over as they're drawn): its lips, the pyramid's sides.
// (The lid keeps its shape: a Brick makes its picture from it.)
bool BucketCollider(void* part)
{
    if (F<uint32_t>(part, 0) != kPolyVtable)
        return false;
    std::string id = Id(part);
    return id == "freeball" ? !F<uint8_t>(part, kPolyOffShown)
                            : id == "bumperhole" && !F<uint8_t>(part, kPolyOffDrawnOnly);
}

void Flip(void* board, bool on)
{
    if (g_flipped == on)
        return;
    SetFlipped(on);
    void* gun = F<void*>(board, kBoardOffGun);
    if (gun) {
        SetPos(gun, F<float>(gun, kGunOffX), on ? kFlippedGunY : kGunY);
        F<float>(Logic(board), kLogicOffAim) = on ? kAimUp : kAimDown;
        SetGunAngle(gun, on ? kAimUp : kAimDown, true);
    }
    SetCollides(g_walls[2], !on);  // out through the top
    // The bucket's parts mirrored to the top (its lips where its opening now is), drawn turned over (BucketDraw).
    for (void* part : g_bucket) {
        F<float>(Mover(part), kMoverOffBaseY) = kBucketMirror - F<float>(Mover(part), kMoverOffBaseY);
        if (BucketCollider(part))
            MirrorShape(part);
    }
    for (void* obj : List(board, kBoardOffObjects))
        if (IsFlipper(obj))
            MirrorFlipper(obj);
    Log("chaos: upside %s", on ? "down" : "up again");
}

// Renfield's Spooky Ball: the game brings one that falls out of the bottom back at the top (its check, each logic
// step, after the physics). Upside down it falls out of the top: it comes back at the bottom (g_spooky_y), on its way
// up again.
uintptr_t g_orig_spooky_check;
uint32_t g_n_spooky;     // testing: how many times one came back ...
float g_spooky_at[4];  // ... and where, how fast (x, y, vx, vy) the last time
int g_spooky_was, g_spooky_left;  // ... and how many more it had before and after (Renfield's power adds more)

void __stdcall SpookyCheckHook(void* lm)
{
    int& left = F<int>(lm, kLogicOffSpookies + 4 * F<int>(lm, kLogicOffPlayer));
    int before = left;
    void* back = nullptr;
    if (!g_flipped) {
        reinterpret_cast<void(__stdcall*)(void*)>(g_orig_spooky_check)(lm);
    } else {
        for (void* ball : Balls(F<void*>(lm, kLogicOffBoard)))
            if (F<uint8_t>(ball, kBallOffSpooky) && F<float>(ball, kBallOffY) + F<float>(ball, kBallOffRadius) < 0) {
                reinterpret_cast<void(__stdcall*)(void*, void*)>(kSpookyReturn)(lm, ball);
                back = ball;
            }
    }
    if (left == before)
        return;
    for (void* ball : Balls(F<void*>(lm, kLogicOffBoard)))
        if (!back && F<float>(ball, kBallOffY) == g_spooky_y)
            back = ball;
    ++g_n_spooky;
    g_spooky_was = before;
    g_spooky_left = left;
    if (back)
        for (int i = 0; i < 4; ++i)
            g_spooky_at[i] = F<float>(back, i < 2 ? kBallOffX + 4 * i : kBallOffVX + 4 * (i - 2));
    Log("chaos: a Spooky Ball came back at y %.0f (%d more)", g_spooky_y, left);
}

float CallAimAtMouse(void* lm, float x, float y);

float __cdecl FlippedAim(void* lm, float x, float y)
{
    // The game's own aim (which allows for gravity) for the mirror image: a cannon shooting down.
    void* gun = F<void*>(F<void*>(lm, 0x184), kBoardOffGun);
    float a = -CallAimAtMouse(lm, x, 2 * F<float>(gun, kGunOffY) - y);
    if (!(a == a))
        return kAimUp;
    while (a < 0)
        a += kTwoPi;
    while (a >= kTwoPi)
        a -= kTwoPi;
    return a;
}

void __cdecl FlippedClamp(void* lm, float a)
{
    if (!(a == a))
        a = kAimUp;
    while (a < 0)
        a += kTwoPi;
    while (a >= kTwoPi)
        a -= kTwoPi;
    if (a > kFlippedAimLow && a < kFlippedAimHigh)
        a = a - kFlippedAimLow < kFlippedAimHigh - a ? kFlippedAimLow : kFlippedAimHigh;
    F<float>(lm, kLogicOffAim) = a;
}

void* g_aim_tramp;
void* g_clamp_tramp;
uintptr_t g_orig_turn_gun;

GAME_CALL float CallAimAtMouse(void* lm, float x, float y)
{
    float r;
    __asm {
        push y
        push x
        mov eax, lm
        call dword ptr [g_aim_tramp]
        fstp r
    }
    return r;
}

__declspec(naked) void AimHook()  // EAX LogicMgr, stdcall(float x, float y) -> ST0
{
    __asm {
        cmp g_flipped, 0
        jne flipped
        jmp dword ptr [g_aim_tramp]
    flipped:
        push dword ptr [esp + 8]
        push dword ptr [esp + 8]
        push eax
        call FlippedAim
        add esp, 12
        ret 8
    }
}

__declspec(naked) void ClampHook()  // ESI LogicMgr, stdcall(float)
{
    __asm {
        cmp g_flipped, 0
        jne flipped
        jmp dword ptr [g_clamp_tramp]
    flipped:
        push dword ptr [esp + 4]
        push esi
        call FlippedClamp
        add esp, 8
        ret 4
    }
}

// The cannon turns towards the aim a little each tick, through angles measured from straight up, so pointing up it
// would swing the long way round: it follows at once instead.
__declspec(naked) void TurnGunHook()  // replaces a call: EAX gun, stdcall(float angle, bool immediate)
{
    __asm {
        cmp g_flipped, 0
        je original
        mov dword ptr [esp + 8], 1
    original:
        jmp dword ptr [g_orig_turn_gun]
    }
}

// ---- Bullet Time: slow motion near each orange peg ----
std::vector<void*> g_slowed;  // oranges that have slowed this shot down already
constexpr float kNearOrange = 8;

void BulletTime(void* ball, void* board)
{
    void* lm = Logic(board);
    if (F<int>(lm, kLogicOffState) != kStateBallInPlay || Fever(board))
        return;
    for (void* peg : UnhitPegs(lm, kOrange)) {
        if (IndexOf(g_slowed, peg) >= 0)
            continue;
        if (!NearPeg(ball, peg, 1.0f, kNearOrange))
            continue;
        g_slowed.push_back(peg);
        ++g_n_slow;
        if (!F<uint8_t>(board, kBoardOffSlowMo)) {
            StartSlowMo(lm);
        } else {  // already slow: slowest again, without the sound
            SetSlowMo(board, 100);
            F<int>(board, kBoardOffSlowHold) = 100;
            F<uint8_t>(board, kBoardOffZoomRequest) = 1;
        }
        if (F<int>(board, kBoardOffZoomHold) < 40)
            F<int>(board, kBoardOffZoomHold) = 40;
        return;
    }
}

// ---- Mirage: pegs that aren't there. The ball goes through them, and they glitch away ----
// First in the places of the level's pegs that are gone (hit in earlier shots: the game keeps them, moving along their
// paths), looking as those pegs did; then, if there are too few, round ones of their own beside real pegs.
constexpr int kMaxFakes = 14;
constexpr float kFakeR = 10;           // a round peg's radius
constexpr uint32_t kGlitchTicks = 70;  // how long one glitches before it's gone
struct Fake {
    float x, y;  // playfield pixels: its centre
    bool orange;
    uint32_t glitched;  // Mirage's tick when the ball went through it (0: not yet)
    void* peg;          // the gone peg whose place it's in (nullptr: a round one of its own)
};
Fake g_fakes[kMaxFakes];
int g_fake_count;

// Directions round a peg (no sines inside the simulation: the two computers' math libraries might differ).
const float kDirs[12][2] = {{1, 0},      {0.866f, 0.5f},   {0.5f, 0.866f},   {0, 1},      {-0.5f, 0.866f},
                            {-0.866f, 0.5f}, {-1, 0},      {-0.866f, -0.5f}, {-0.5f, -0.866f}, {0, -1},
                            {0.5f, -0.866f}, {0.866f, -0.5f}};

float CenterX(void* obj) { return (F<float>(obj, kObjOffLeft) + F<float>(obj, kObjOffRight)) / 2; }
float CenterY(void* obj) { return (F<float>(obj, kObjOffTop) + F<float>(obj, kObjOffBottom)) / 2; }

bool Drawable(void* peg)  // a round peg or a brick (the kinds the mod draws)
{
    uintptr_t vtable = *reinterpret_cast<uintptr_t*>(peg);
    return vtable == kBallVtable || vtable == kBrickVtable;
}

// Gone pegs' places first, in a random order, clear of portals. Then beside real pegs (so they pass for some), clear
// of them, of each other and of portals, away from the cannon and the bucket. About two in five look orange.
void PlaceFakes(void* board)
{
    void* lm = Logic(board);
    std::vector<void*> pegs = UnhitPegs(lm);
    g_fake_count = 0;
    if (pegs.empty())
        return;
    int want = static_cast<int>(pegs.size()) / 6;
    want = want < 6 ? 6 : want > kMaxFakes ? kMaxFakes : want;
    int oranges = (want * 2 + 2) / 5;
    const float clear_peg = kFakeR + 7, apart = 2 * kFakeR + 8, clear_portal = kPortalR + kFakeR + 6;
    auto clear_of_portals = [&](float x, float y) {
        for (int i = 0; i < g_portal_count; ++i) {
            float dx = x - g_portals[i].x, dy = y - g_portals[i].y;
            if (dx * dx + dy * dy < clear_portal * clear_portal)
                return false;
        }
        return true;
    };
    std::vector<void*> gone;
    for (void* p : Pegs(lm))
        if (!F<uint8_t>(p, kObjOffAlive) && Drawable(p) && clear_of_portals(CenterX(p), CenterY(p)))
            gone.push_back(p);
    while (g_fake_count < want && !gone.empty()) {
        uint32_t i = Random(static_cast<uint32_t>(gone.size()));
        g_fakes[g_fake_count++] = {CenterX(gone[i]), CenterY(gone[i]), false, 0, gone[i]};
        gone.erase(gone.begin() + i);
    }
    int in_places = g_fake_count;
    for (int attempt = 0; attempt < want * 40 && g_fake_count < want; ++attempt) {
        void* beside = pegs[Random(static_cast<uint32_t>(pegs.size()))];
        const float* dir = kDirs[Random(12)];
        float d = 24.0f + Random(19);
        float x = CenterX(beside) + dir[0] * d, y = CenterY(beside) + dir[1] * d;
        if (x < 25 || x > kFieldW - 25 || y < 80 || y > kFieldH - 70)
            continue;
        bool clear = true;
        for (size_t i = 0; clear && i < pegs.size(); ++i)
            clear = RectDistSq(pegs[i], x, y) >= clear_peg * clear_peg;
        for (int i = 0; clear && i < g_fake_count; ++i) {
            float dx = x - g_fakes[i].x, dy = y - g_fakes[i].y;
            clear = g_fakes[i].peg ? RectDistSq(g_fakes[i].peg, x, y) >= clear_peg * clear_peg
                                   : dx * dx + dy * dy >= apart * apart;
        }
        if (!clear || !clear_of_portals(x, y))
            continue;
        g_fakes[g_fake_count++] = {x, y, false, 0, nullptr};
    }
    for (int n = 0; n < oranges && n < g_fake_count;) {
        Fake& f = g_fakes[Random(static_cast<uint32_t>(g_fake_count))];
        if (!f.orange) {
            f.orange = true;
            ++n;
        }
    }
    Log("chaos: %d fake pegs (%d in gone pegs' places)", g_fake_count, in_places);
}

// Fakes in gone pegs' places go where those pegs go (along their paths), each physics step.
void FollowFakes()
{
    for (int i = 0; i < g_fake_count; ++i)
        if (void* p = g_fakes[i].peg) {
            g_fakes[i].x = CenterX(p);
            g_fakes[i].y = CenterY(p);
        }
}

bool IsBrick(void* obj) { return F<int>(obj, kObjOffKind) != kKindRoundPeg; }

// Whether a ball of radius r moving from (x, y) by (dx, dy) touches a peg's own shape within a share of that move:
// the game's own test of a ball against a peg, on a stand-in ball.
bool Touches(void* peg, float x, float y, float dx, float dy, float r, float share)
{
    alignas(16) uint8_t stand_in[kBallSize] = {};
    void* ball = stand_in;
    F<float>(ball, kBallOffX) = x;
    F<float>(ball, kBallOffY) = y;
    F<float>(ball, kBallOffVX) = dx;
    F<float>(ball, kBallOffVY) = dy;
    F<float>(ball, kBallOffRadius) = r;
    return NearPeg(ball, peg, share, 0);
}

// The share (0..1) of a ball's move from (x, y) by (dx, dy) at which it first touches a fake, or -1.
float FakeContact(const Fake& f, float x, float y, float dx, float dy, float r)
{
    if (f.peg && IsBrick(f.peg)) {
        if (!Touches(f.peg, x, y, dx, dy, r, 1))
            return -1;
        float lo = 0, hi = 1;  // (bisected)
        for (int i = 0; i < 7; ++i) {
            float mid = (lo + hi) / 2;
            (Touches(f.peg, x, y, dx, dy, r, mid) ? hi : lo) = mid;
        }
        return hi;
    }
    float cx = f.peg ? CenterX(f.peg) : f.x, cy = f.peg ? CenterY(f.peg) : f.y;
    float reach = r + (f.peg ? F<float>(f.peg, kBallOffRadius) : kFakeR);
    float ox = x - cx, oy = y - cy;
    float a = dx * dx + dy * dy, b = ox * dx + oy * dy, c = ox * ox + oy * oy - reach * reach;
    if (c <= 0)
        return 0;  // (already in it)
    float disc = b * b - a * c;
    if (a <= 0 || b >= 0 || disc < 0)
        return -1;
    float share = (-b - sqrtf(disc)) / a;
    return share <= 1 ? share : -1;
}

// The ball's path this step (from where it was) against each fake.
void ThroughFakes(void* ball)
{
    float x = F<float>(ball, kBallOffX), y = F<float>(ball, kBallOffY);
    float px = F<float>(ball, kBallOffLastX), py = F<float>(ball, kBallOffLastY);
    float dx = x - px, dy = y - py;
    if (dx * dx + dy * dy > 30.0f * 30.0f) {  // it jumped (a portal, round to the other side): only where it is
        px = x;
        py = y;
        dx = dy = 0;
    }
    for (int i = 0; i < g_fake_count; ++i) {
        Fake& f = g_fakes[i];
        if (f.glitched || FakeContact(f, px, py, dx, dy, F<float>(ball, kBallOffRadius)) < 0)
            continue;
        f.glitched = Ticks(kMirage) + 1;
        ++g_n_glitches;
        ui::PlaySound(kSoundBuzz);
        Log("chaos: the ball went through a fake peg at (%.0f, %.0f)", f.x + 75, f.y + 40);
    }
}

// Mirage's way in, so its fakes never pop up in sight: late in the turn passing (the next turn comes 150 steps
// into it at the earliest) the board fades to black, the fakes appear in the dark as the turn begins, and the
// board fades back in. Timed by the board's steps, so it's the same on both computers.
enum Blackout { kNoBlackout, kBlackoutSoon, kFadingOut, kBlack, kFadingIn };
int g_blackout;
uint32_t g_blackout_steps;  // in this phase
void* g_blackout_board;
constexpr int kBlackoutFrom = 110;  // the turn passing's step to start fading at
constexpr uint32_t kFadeOutSteps = 30, kBlackSteps = 20, kFadeInSteps = 50, kBlackMax = 600;
uint32_t g_n_blackouts;  // testing: how many went fully black

void StepBlackout(void* board)
{
    void* lm = Logic(board);
    if (!g_blackout || board != g_blackout_board || F<uint8_t>(lm, kLogicOffReplaying))
        return;
    ++g_blackout_steps;
    switch (g_blackout) {
    case kBlackoutSoon:
        if (F<int>(lm, kLogicOffState) != kStateTurnPassing || F<int>(lm, kLogicOffStateSteps) >= kBlackoutFrom) {
            g_blackout = kFadingOut;
            g_blackout_steps = 0;
        }
        break;
    case kFadingOut:
        if (g_blackout_steps >= kFadeOutSteps) {
            g_blackout = kBlack;
            g_blackout_steps = 0;
            ++g_n_blackouts;
        }
        break;
    case kBlack:  // until the fakes are in (the turn began) and a moment longer
        if ((On(kMirage) && g_blackout_steps >= kBlackSteps) || g_blackout_steps >= kBlackMax) {
            g_blackout = kFadingIn;
            g_blackout_steps = 0;
        }
        break;
    case kFadingIn:
        if (g_blackout_steps >= kFadeInSteps) {
            g_blackout = kNoBlackout;
            g_blackout_board = nullptr;
        }
        break;
    default:
        break;
    }
}

int BlackoutAlpha()
{
    switch (g_blackout) {
    case kFadingOut:
        return static_cast<int>(g_blackout_steps * 255 / kFadeOutSteps);
    case kBlack:
        return 255;
    case kFadingIn:
        return 255 - static_cast<int>(g_blackout_steps * 255 / kFadeInSteps);
    default:
        return 0;
    }
}

bool FakesHidden() { return g_blackout == kBlackoutSoon || g_blackout == kFadingOut; }

bool g_drop_in;  // testing: the next ball steps into the second bucket

// ---- Each ball, each physics step, after it moved ----
bool g_place;          // testing: the next ball to step is put here, at this speed (playfield)
float g_place_at[4];

void __cdecl OnBallStep(void* ball, void* board)
{
    ++g_n_steps;
    if (g_place) {
        g_place = false;
        Teleport(ball, g_place_at[0], g_place_at[1]);
        SetVelocity(ball, g_place_at[2], g_place_at[3]);
    }
    if (!g_active || board != g_board)
        return;
    if (On(kWrapAround)) {
        float x = F<float>(ball, kBallOffX);
        if (x < 0 || x > kFieldW) {
            Teleport(ball, x < 0 ? x + kFieldW : x - kFieldW, F<float>(ball, kBallOffY));
            ++g_n_wrap;
        }
    }
    if (On(kWormholes) || On(kPortalStorm))
        ThroughPortals(ball);
    if (On(kDoubleBucket)) {
        if (g_drop_in && g_hole) {  // testing
            g_drop_in = false;
            Teleport(ball, F<float>(g_hole, kHoleOffX) + kTwinOffset, F<float>(g_hole, kHoleOffY));
        }
        TwinColliders(ball);
        if (InTwinBucket(ball) && IndexOf(g_pending_catches, ball) < 0) {
            InterlockedIncrement(&F<LONG>(ball, kObjOffRefs));
            g_pending_catches.push_back(ball);
        }
    }
    if (On(kBulletTime))
        BulletTime(ball, board);
    if (On(kMirage))
        ThroughFakes(ball);
}

void* g_ball_step_tramp;

__declspec(naked) void BallStepHook()  // ESI ball, EDI board
{
    __asm {
        pushad
        pushfd
        push edi
        push esi
        call OnBallStep
        add esp, 8
        popfd
        popad
        jmp dword ptr [g_ball_step_tramp]
    }
}

// ---- Blind Shot (no aiming guide), the guide upside down, and the guide meeting Mirage's fakes ----
// The guide is a ball flown ahead each tick (in the simulation), which leaves in the cannon where it went and where it
// first hit something; the drawing reads those.
constexpr int kGunOffBoard = 0x148;
constexpr int kGunOffFromX = 0x158, kGunOffFromY = 0x15C;  // where it starts
constexpr int kGunOffHitStep = 0x198;                      // the step of its first hit (-1: none)
constexpr int kGunOffHitX = 0x1A4, kGunOffHitY = 0x1A8;    // where the ball is then
constexpr int kGunOffBounce = 0x1AC;                       // atan2(vy, vx) after it: the Super Guide's arrow
constexpr int kGunOffPath = 0x1C8;                         // std::vector of (x, y): where it is after each step
constexpr uintptr_t kDefaultBallRadius = 0x6C6E0C;         // float (no ball loaded)

bool g_hide_guide;
void* g_guide_tramp;
struct GuideSaved {
    void* gun;  // nullptr: nothing changed
    int step;
    float x, y, bounce;
} g_guide_saved;
uint32_t g_n_guide_fakes;  // testing: guide drawings that stopped at a fake, and where the last one did
int g_guide_fake_step = -1;
float g_guide_fake_x, g_guide_fake_y;

bool NearSegment(const Fake& f, float x0, float y0, float x1, float y1, float r)
{
    float l = std::min(x0, x1) - r, rt = std::max(x0, x1) + r, t = std::min(y0, y1) - r, b = std::max(y0, y1) + r;
    if (f.peg)
        return F<float>(f.peg, kObjOffRight) >= l && F<float>(f.peg, kObjOffLeft) <= rt &&
               F<float>(f.peg, kObjOffBottom) >= t && F<float>(f.peg, kObjOffTop) <= b;
    return f.x + kFakeR >= l && f.x - kFakeR <= rt && f.y + kFakeR >= t && f.y - kFakeR <= b;
}

// That the guide went through a fake would give it away: where the flight first touches one in sight (before what
// it really hits), the guide stops there as if at a peg, with the arrow of the Super Guide pointing off it. Only for
// the drawing: the cannon's fields are put back after it.
void __cdecl GuideToFake(void* gun)
{
    g_guide_saved.gun = nullptr;
    if (!gun || !On(kMirage) || !g_fake_count || On(kInvisiblePegs) || FakesHidden() ||
        F<void*>(gun, kGunOffBoard) != g_board)
        return;
    const float* path = F<const float*>(gun, kGunOffPath);
    const float* path_end = F<const float*>(gun, kGunOffPath + 4);
    if (!path || path_end <= path)
        return;
    int steps = static_cast<int>(path_end - path) / 2, hit = F<int>(gun, kGunOffHitStep);
    int limit = hit >= 0 && hit < steps ? hit : steps;
    void* loaded = F<void*>(gun, kGunOffBall);
    float r = loaded ? F<float>(loaded, kBallOffRadius) : *reinterpret_cast<const float*>(kDefaultBallRadius);
    float x = F<float>(gun, kGunOffFromX), y = F<float>(gun, kGunOffFromY);
    for (int step = 0; step < limit; ++step) {
        float nx = path[2 * step], ny = path[2 * step + 1], dx = nx - x, dy = ny - y;
        float first = 2;
        const Fake* met = nullptr;
        for (int i = 0; i < g_fake_count; ++i) {
            const Fake& f = g_fakes[i];
            if (f.glitched || !NearSegment(f, x, y, nx, ny, r + 2))
                continue;
            float share = FakeContact(f, x, y, dx, dy, r);
            if (share >= 0 && share < first) {
                first = share;
                met = &f;
            }
        }
        if (!met) {
            x = nx;
            y = ny;
            continue;
        }
        float hx = x + dx * first, hy = y + dy * first;
        float cx = met->peg ? CenterX(met->peg) : met->x, cy = met->peg ? CenterY(met->peg) : met->y;
        float ox = hx - cx, oy = hy - cy, len = sqrtf(ox * ox + oy * oy), bx = dx, by = dy;
        if (len > 0) {  // off it: the step mirrored in its surface (as seen from its centre)
            ox /= len;
            oy /= len;
            float d = dx * ox + dy * oy;
            if (d < 0) {
                bx -= 2 * d * ox;
                by -= 2 * d * oy;
            }
        }
        g_guide_saved = {gun, hit, F<float>(gun, kGunOffHitX), F<float>(gun, kGunOffHitY),
                         F<float>(gun, kGunOffBounce)};
        F<int>(gun, kGunOffHitStep) = step;
        F<float>(gun, kGunOffHitX) = hx;
        F<float>(gun, kGunOffHitY) = hy;
        F<float>(gun, kGunOffBounce) = atan2f(by, bx);
        ++g_n_guide_fakes;
        g_guide_fake_step = step;
        g_guide_fake_x = hx;
        g_guide_fake_y = hy;
        return;
    }
}

void __cdecl GuideRestore()
{
    if (void* gun = g_guide_saved.gun) {
        F<int>(gun, kGunOffHitStep) = g_guide_saved.step;
        F<float>(gun, kGunOffHitX) = g_guide_saved.x;
        F<float>(gun, kGunOffHitY) = g_guide_saved.y;
        F<float>(gun, kGunOffBounce) = g_guide_saved.bounce;
        g_guide_saved.gun = nullptr;
    }
}

__declspec(naked) void GuideHook()  // ECX gun, EAX Graphics, stdcall(bool)
{
    __asm {
        cmp g_hide_guide, 0
        jne hide
        push eax
        push ecx
        push ecx
        call GuideToFake
        add esp, 4
        pop ecx
        pop eax
        cmp g_flipped, 0
        jne flipped
        push dword ptr [esp + 4]
        call dword ptr [g_guide_tramp]
        jmp done
    flipped:  // its dots fall with the gravity too
        xor dword ptr ds:[0x6C77CC], 0x80000000
        push dword ptr [esp + 4]
        call dword ptr [g_guide_tramp]
        xor dword ptr ds:[0x6C77CC], 0x80000000
    done:
        call GuideRestore
        ret 4
    hide:
        inc g_n_blind
        ret 4
    }
}

// ---- Drawing the board's objects: the bucket turned over (Upside Down) ----
using ObjDrawFn = void(__thiscall*)(void* obj, void* g);
ObjDrawFn g_ball_draw[3], g_brick_draw[3];  // shadow, body, overlay
ObjDrawFn g_hole_draw, g_poly_draw;

constexpr int kGraphicsOffTransX = 0x08, kGraphicsOffTransY = 0x0C, kGraphicsOffColor = 0x30;
constexpr int kGraphicsOffScale = 0x10;  // float x, y, then the point it scales about (with the translation in)
constexpr int kGraphicsOffClip = 0x20;   // Rect, with the translation in
constexpr int kGraphicsOffColorize = 0x48, kGraphicsOffIs3D = 0x4C;
constexpr uintptr_t kHoleVtable = 0x688C5C;
constexpr uintptr_t kMatrixIdentity = 0x5605D0;   // EAX matrix (3x3 floats)
constexpr uintptr_t kMatrixTranslate = 0x5605F0;  // ESI matrix, stdcall(float x, float y)
constexpr uintptr_t kMatrixScale = 0x5606B0;      // ESI matrix, stdcall(float x, float y)
constexpr uintptr_t kPushTransform = 0x4FC180;    // EAX D3DInterface, stdcall(const matrix*): onto what's there
constexpr uintptr_t kPopTransform = 0x4FC220;     // EAX D3DInterface
constexpr int kAppOffDDInterface = 0x36C, kDDOffD3D = 0x30;

// Upside Down: drawing mirrored about a point of the playfield (scale -1, -1: turned round, which for the bucket is
// upside down as it's the same on both sides; 1, -1: upside down), through the Direct3D transform the game zooms
// with, as it does: only with 3D acceleration on (without it things stay the right way up).
void* D3D(void* g)
{
    void* dd = F<void*>(game::App(), kAppOffDDInterface);
    return dd && F<uint8_t>(g, kGraphicsOffIs3D) ? F<void*>(dd, kDDOffD3D) : nullptr;
}

GAME_CALL void DrawMirrored(ObjDrawFn draw, void* obj, void* g, float x, float y, float scale_x, float scale_y)
{
    void* d3d = D3D(g);
    if (!d3d) {
        draw(obj, g);
        return;
    }
    float centre_x = x + F<float>(g, kGraphicsOffTransX), centre_y = y + F<float>(g, kGraphicsOffTransY);
    float back_x = -centre_x, back_y = -centre_y;  // (no names of registers in here: cx is one)
    float matrix[9];
    float* m = matrix;
    __asm {
        mov eax, m
        mov ecx, kMatrixIdentity
        call ecx
        mov esi, m
        push back_y
        push back_x
        mov ecx, kMatrixTranslate
        call ecx
        push scale_y
        push scale_x
        mov ecx, kMatrixScale
        call ecx
        push centre_y
        push centre_x
        mov ecx, kMatrixTranslate
        call ecx
        push m
        mov eax, d3d
        mov ecx, kPushTransform
        call ecx
    }
    draw(obj, g);
    __asm {
        mov eax, d3d
        mov ecx, kPopTransform
        call ecx
    }
}

void DrawTurnedOver(ObjDrawFn draw, void* part, void* g)  // a part of the bucket, about its own position
{
    DrawMirrored(draw, part, g, F<float>(Mover(part), kMoverOffX), F<float>(Mover(part), kMoverOffY), -1, -1);
}

void DrawBucketPart(ObjDrawFn draw, void* part, void* g)
{
    if (g_flipped && IndexOf(g_bucket, part) >= 0)
        DrawTurnedOver(draw, part, g);
    else
        draw(part, g);
}

// A flipper turned over (MirrorFlipper) draws as the flipper the right way up would, at its turn the other way, and
// that upside down about its pivot. (Without 3D acceleration: at its own turn, the picture not mirrored.)
void FlipperDraw(void* flipper, void* g)
{
    if (!D3D(g)) {
        g_poly_draw(flipper, g);
        return;
    }
    float& angle = F<float>(flipper, kPolyOffDrawAngle);
    float& picture_y = F<float>(flipper, kPolyOffPictureY);
    angle = -angle;
    picture_y = -picture_y;
    DrawMirrored(g_poly_draw, flipper, g, F<float>(flipper, kPolyOffX), F<float>(flipper, kPolyOffY), 1, -1);
    angle = -angle;
    picture_y = -picture_y;
}

void __fastcall HoleDraw(void* obj, void*, void* g) { DrawBucketPart(g_hole_draw, obj, g); }

void __fastcall PolyDraw(void* obj, void*, void* g)
{
    if (g_flipped && IsFlipper(obj))
        FlipperDraw(obj, g);
    else
        DrawBucketPart(g_poly_draw, obj, g);
}

// ---- Marina's Chain Lightning: the rod on the bucket ----
// While a shot carries it a rod rises out of the bucket (to 512 from 586: 44 over the bucket's middle), and the
// shot's first peg hit sends an arc through the pegs between to the rod. Upside down the rod moves as it would and
// its height is mirrored wherever it's used: the arc's way (at the hit), its sparks, and the drawing (the rod
// turned over, the arc, and the pegs it would take, shown while aiming). With Double Bucket the arc goes to the
// nearer bucket, the rod moving there for it (it falls back there, and rises from the first one again); while it's
// up on the first one, the copy shows one too.
constexpr int kLogicOffRod = 0x74;
constexpr int kRodOffState = 0x00, kRodOffBaseX = 0x04, kRodOffX = 0x0C;  // ints (x: where it's drawn)
constexpr int kRodOffY = 0x10;                                           // float
constexpr uintptr_t kZap = 0x470E48;       // call: PegHit's arc, stdcall(LogicMgr, ball, peg)
constexpr uintptr_t kRodSpark = 0x438D58;  // call: a spark at the struck rod, stdcall(kind, x, y, ...)
constexpr uintptr_t kZapPreview = 0x4776C7;  // call: the pegs it would take (drawn), ECX LogicMgr, EAX peg, stdcall(2)
constexpr uintptr_t kDrawRod = 0x40CA7F;   // call: thiscall(LogicMgr), stdcall(Graphics*)

void* Rod(void* lm) { return lm ? F<void*>(lm, kLogicOffRod) : nullptr; }
bool RodOnBucket(int state) { return state == 1 || state == 2 || state == 7 || state == 8; }  // up, following it
// Testing: arcs, and the last one's end (playfield), whether it went to the copy, its peg, where the rod was then.
uint32_t g_n_zaps;
float g_zap_x, g_zap_y, g_zap_peg_x, g_zap_peg_y, g_zap_rod_x;
bool g_zap_twin;

// Whether the arc from a peg (or the pegs it would take, from the guide's first peg) goes to the copy.
bool ZapToTwin(void* lm, void* peg, void* rod)
{
    if (!On(kDoubleBucket) || !g_hole || Fever(g_board) || !peg || !Ours(lm))
        return false;
    float x = CenterX(peg), rod_x = static_cast<float>(F<int>(rod, kRodOffX));
    return fabsf(x - rod_x - kTwinOffset) < fabsf(x - rod_x);
}

uintptr_t g_orig_zap;

void __stdcall ZapHook(void* lm, void* ball, void* peg)
{
    using ZapFn = void(__stdcall*)(void*, void*, void*);
    void* rod = Rod(lm);
    if (!rod) {
        reinterpret_cast<ZapFn>(g_orig_zap)(lm, ball, peg);
        return;
    }
    bool ours = Ours(lm), twin = ZapToTwin(lm, peg, rod);
    g_zap_peg_x = CenterX(peg);
    g_zap_peg_y = CenterY(peg);
    g_zap_rod_x = static_cast<float>(F<int>(rod, kRodOffX));
    if (twin) {
        F<int>(rod, kRodOffBaseX) += static_cast<int>(kTwinOffset);
        F<int>(rod, kRodOffX) += static_cast<int>(kTwinOffset);
    }
    float y = F<float>(rod, kRodOffY);
    if (ours && g_flipped)
        F<float>(rod, kRodOffY) = kBucketMirror - y;
    g_zap_x = static_cast<float>(F<int>(rod, kRodOffX));
    g_zap_y = F<float>(rod, kRodOffY);
    reinterpret_cast<ZapFn>(g_orig_zap)(lm, ball, peg);
    F<float>(rod, kRodOffY) = y;
    if (twin)
        F<int>(rod, kRodOffX) = F<int>(rod, kRodOffBaseX);  // (the arc put it back over the first bucket)
    g_zap_twin = twin;
    ++g_n_zaps;
    Log("chaos: Chain Lightning arced to the rod at %.0f,%.0f%s", g_zap_x, g_zap_y,
        twin ? " (the second bucket)" : "");
}

uintptr_t g_orig_rod_spark;
void __cdecl MirrorY(float* y) { *y = kBucketMirror - *y; }

__declspec(naked) void RodSparkHook()  // replaces a call: stdcall(kind, x, y, ...)
{
    __asm {
        cmp g_flipped, 0
        je original
        pushad
        lea eax, [esp + 32 + 12]
        push eax
        call MirrorY
        add esp, 4
        popad
    original:
        jmp dword ptr [g_orig_rod_spark]
    }
}

// The pegs it would take, shown while aiming: to the copy, if it's nearer.
uintptr_t g_orig_zap_preview;
int g_preview_shift;

void __cdecl PreviewBegins(void* lm, void* peg)
{
    void* rod = Rod(lm);
    g_preview_shift = rod && RodOnBucket(F<int>(rod, kRodOffState)) && ZapToTwin(lm, peg, rod)
                          ? static_cast<int>(kTwinOffset) : 0;
    if (g_preview_shift)
        F<int>(rod, kRodOffX) += g_preview_shift;
}

void __cdecl PreviewEnds(void* lm)
{
    if (g_preview_shift)
        F<int>(Rod(lm), kRodOffX) -= g_preview_shift;
    g_preview_shift = 0;
}

__declspec(naked) void ZapPreviewHook()  // replaces a call: ECX LogicMgr, EAX peg, stdcall(out, out)
{
    __asm {
        pushad
        push eax
        push ecx
        call PreviewBegins
        add esp, 8
        popad
        push ecx                  // (the LogicMgr, for PreviewEnds)
        push dword ptr [esp + 12]  // the two arguments again
        push dword ptr [esp + 12]
        call dword ptr [g_orig_zap_preview]
        pop ecx
        pushad
        push ecx
        call PreviewEnds
        add esp, 4
        popad
        ret 8
    }
}

// The board drawn upside down: the rod's height mirrored for all of it (the rod, the arc, the preview), and back.
void* g_rod_mirrored;
float g_rod_y;

void RodMirrorBegins(void* board)
{
    void* rod = Rod(Logic(board));
    if (!g_flipped || board != g_board || !rod)
        return;
    g_rod_mirrored = rod;
    g_rod_y = F<float>(rod, kRodOffY);
    F<float>(rod, kRodOffY) = kBucketMirror - g_rod_y;
}

void RodMirrorEnds()
{
    if (g_rod_mirrored)
        F<float>(g_rod_mirrored, kRodOffY) = g_rod_y;
    g_rod_mirrored = nullptr;
}

uintptr_t g_orig_draw_rod;

void DrawRodAt(void* lm, void* rod, void* g, float dx)
{
    auto draw = reinterpret_cast<ObjDrawFn>(g_orig_draw_rod);
    float& tx = F<float>(g, kGraphicsOffTransX);
    tx += dx;
    if (g_flipped)  // (its height is mirrored already: turned round about where it is)
        DrawMirrored(draw, lm, g, static_cast<float>(F<int>(rod, kRodOffX)), F<float>(rod, kRodOffY), -1, -1);
    else
        draw(lm, g);
    tx -= dx;
}

void __fastcall DrawRodHook(void* lm, void*, void* g)  // replaces a call: thiscall(LogicMgr), stdcall(g)
{
    void* rod = Rod(lm);
    if (!rod || !Ours(lm) || (!g_flipped && !On(kDoubleBucket))) {
        reinterpret_cast<ObjDrawFn>(g_orig_draw_rod)(lm, g);
        return;
    }
    int state = F<int>(rod, kRodOffState);
    if (g_flipped && state == 0)
        return;  // (at rest it's in the bucket, below the board; mirrored that would be over its top edge)
    DrawRodAt(lm, rod, g, 0);
    if (On(kDoubleBucket) && g_hole && !Fever(g_board) && RodOnBucket(state))
        DrawRodAt(lm, rod, g, kTwinOffset);
}

int Pulse(int period, uint32_t ticks)  // 0..255..0 with the board's ticks
{
    int t = static_cast<int>(ticks % static_cast<uint32_t>(period)) * 512 / period;
    return t < 256 ? t : 511 - t;
}

// ---- Pictures recolored: Silhouette's gray pegs, the red peg ----
// Copies of the game's own pictures, made while drawing (outside the simulation) and swapped in while a peg
// draws. Gray ones are made from the blue pictures, so that every gray peg looks the same whatever its color; red
// ones from the purple pictures turned crimson (the colorblind pictures mark purple with a cross: from their blue).
enum Tint { kTintGray, kTintRed };

// The cell (type - 1, + 4 lit) a tinted cell is made from.
int SourceCell(int tint, bool lit, bool colorblind)
{
    if (tint == kTintGray)
        return 0;  // (lit too: a peg made purple flashes lit)
    return (colorblind ? 0 : 2) + (lit ? 4 : 0);
}
struct TintedImage {
    void* from;
    int tint;
    int w, h;
    void* image;  // nullptr: it couldn't be made
};
std::vector<TintedImage> g_tinted;
bool g_tinted_stale;     // a new red peg or Silhouette: bricks' pictures may be new ones at old addresses
void* g_tinted_board;    // the board the bricks' pictures belong to

int Clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

uint32_t TintPixel(uint32_t argb, int tint)
{
    int r = argb >> 16 & 0xFF, gr = argb >> 8 & 0xFF, b = argb & 0xFF;
    int nr, ng, nb;
    if (tint == kTintGray) {  // stone gray, a little cool
        int l = (r * 2 + gr * 3 + b * 5) / 10 * 5 / 4 + 10;
        nr = l * 24 / 25;
        ng = l * 49 / 50;
        nb = l * 26 / 25;
    } else {  // the hue turned to crimson (357 degrees), its saturation and brightness kept
        int hi = std::max({r, gr, b}), lo = std::min({r, gr, b});
        if ((hi - lo) * 100 <= hi * 8)
            return argb;  // (near gray: the shine)
        nr = hi;
        ng = lo;
        nb = lo + (hi - lo) / 20;
    }
    return (argb & 0xFF000000) | Clamp255(nr) << 16 | Clamp255(ng) << 8 | Clamp255(nb);
}

GAME_CALL void* MakeTinted(void* from, int tint)
{
    int w = F<int>(from, game::kImageOffWidth), h = F<int>(from, game::kImageOffHeight);
    int rows = F<int>(from, game::kImageOffRows), cols = F<int>(from, kImageOffCols);
    uint32_t* src = w > 0 && h > 0 ? VCall<uint32_t*(__thiscall*)(void*)>(from, kImageSlotGetBits)(from) : nullptr;
    if (!src)
        return nullptr;
    void* image = game::GameNew(kMemoryImageSize);
    __asm {
        mov ecx, image
        mov eax, kMemoryImageCtor
        call eax
    }
    VCall<void(__thiscall*)(void*, int, int)>(image, kImageSlotCreate)(image, w, h);
    uint32_t* dst = VCall<uint32_t*(__thiscall*)(void*)>(image, kImageSlotGetBits)(image);
    // The round pegs' cells (four colors, then lit): each made from its source cell. (A picture of one cell is
    // its own source.)
    bool colorblind = from == ui::Resource<void>(kImagePegsColorblind);
    int cell_h = rows == 8 ? h / 8 : h;
    for (int y = 0; y < h; ++y) {
        int sy = y;
        if (rows == 8)
            sy = SourceCell(tint, y >= 4 * cell_h, colorblind) * cell_h + y % cell_h;
        for (int x = 0; x < w; ++x)
            dst[y * w + x] = TintPixel(src[sy * w + x], tint);
    }
    F<int>(image, game::kImageOffRows) = rows;
    F<int>(image, kImageOffCols) = cols;
    VCall<void(__thiscall*)(void*)>(image, kImageSlotBitsChanged)(image);
    return image;
}

void FreeTinted()
{
    for (const TintedImage& t : g_tinted)
        if (t.image)
            VCall<void(__thiscall*)(void*, int)>(t.image, 0)(t.image, 1);
    g_tinted.clear();
}

void* Tinted(void* from, int tint)
{
    void* board = game::At<void*>(game::App(), game::kAppOffBoard);
    if (g_tinted_stale || board != g_tinted_board) {
        FreeTinted();
        g_tinted_stale = false;
        g_tinted_board = board;
    }
    if (!from)
        return nullptr;
    int w = F<int>(from, game::kImageOffWidth), h = F<int>(from, game::kImageOffHeight);
    for (const TintedImage& t : g_tinted)
        if (t.from == from && t.tint == tint && t.w == w && t.h == h)
            return t.image;
    g_tinted.push_back({from, tint, w, h, MakeTinted(from, tint)});
    return g_tinted.back().image;
}

// A brick's picture of a cell, made now if it hasn't been drawn in that color yet.
GAME_CALL void* BrickImage(void* brick, int cell)
{
    void** images = &F<void*>(brick, kBrickOffImages);
    if (!images[cell]) {
        __asm {
            push cell
            push brick
            mov eax, kMakeBrickImage
            call eax
        }
    }
    return images[cell];
}

// A peg drawn in a tint: round pegs from a tinted copy of the cells (the colorblind ones too, and a moving peg's
// own cells without 3D acceleration), bricks from tinted copies of their own pictures in place of their color's
// (unlit and lit). Bricks keep their colors without 3D acceleration.
void DrawTinted(ObjDrawFn draw, void* peg, void* g, int tint)
{
    int cell = Type(peg) - 1;
    if (cell < 0 || cell > 3) {
        draw(peg, g);
        return;
    }
    bool colorblind = *reinterpret_cast<uint8_t*>(kColorblind) != 0;
    int from[2] = {SourceCell(tint, false, colorblind), SourceCell(tint, true, colorblind)};
    bool is_3d = F<uint8_t>(g, kGraphicsOffIs3D) != 0;
    if (F<int>(peg, kObjOffKind) == kKindRoundPeg) {
        void** sheets[2] = {reinterpret_cast<void**>(kImagePegs), reinterpret_cast<void**>(kImagePegsColorblind)};
        void** moving = reinterpret_cast<void**>(kMovingPegCells);
        void* saved[4] = {*sheets[0], *sheets[1], moving[cell], moving[cell + 4]};
        void* moving_tinted[2] = {nullptr, nullptr};
        if (!is_3d && F<void*>(peg, kObjOffMover))
            for (int i = 0; i < 2; ++i)
                moving_tinted[i] = Tinted(moving[from[i]], tint);
        for (int i = 0; i < 2; ++i) {
            if (void* t = Tinted(saved[i], tint))
                *sheets[i] = t;
            if (moving_tinted[i])
                moving[cell + 4 * i] = moving_tinted[i];
        }
        draw(peg, g);
        *sheets[0] = saved[0];
        *sheets[1] = saved[1];
        moving[cell] = saved[2];
        moving[cell + 4] = saved[3];
        return;
    }
    if (!is_3d) {
        draw(peg, g);
        return;
    }
    void** images = &F<void*>(peg, kBrickOffImages);
    void* tinted[2] = {Tinted(BrickImage(peg, from[0]), tint), Tinted(BrickImage(peg, from[1]), tint)};
    void* saved[2] = {images[cell], images[cell + 4]};
    for (int i = 0; i < 2; ++i)
        if (tinted[i])
            images[cell + 4 * i] = tinted[i];
    draw(peg, g);
    images[cell] = saved[0];
    images[cell + 4] = saved[1];
}

// ---- Drawing the pegs: hidden (Invisible Pegs), gray (Silhouette), marked (the bomb, the red peg) ----
void SetDrawMode(void* g, int mode);
void Disc(void* g, float cx, float cy, float r, const ui::Color& c);
void Ring(void* g, float cx, float cy, float r0, float r1, const ui::Color& c);

bool Hidden(void* obj) { return On(kInvisiblePegs) && PegInfo(obj) && !Lit(obj); }

void DrawImageF(void* g, void* image, float x, float y, const ui::Rect& src);
int Width(void* image) { return F<int>(image, game::kImageOffWidth); }
int Height(void* image) { return F<int>(image, game::kImageOffHeight); }

GAME_CALL void DrawImage(void* g, void* image, int x, int y, const ui::Rect& src)
{
    const ui::Rect* r = &src;
    __asm {
        push r
        push y
        push x
        push image
        mov eax, g
        mov ecx, kDrawImage
        call ecx
    }
}

GAME_CALL void DrawBrickCell(void* brick, void* g, int cell)
{
    __asm {
        push g
        push brick
        mov eax, cell
        mov ecx, kDrawBrickCell
        call ecx
    }
}

// A glow added round a peg in a color, as the game draws a lit peg's: round pegs the soft disc of IMAGE_PEGGLOW,
// bricks their own glow picture (its offset grown with it, as the game does).
void DrawPegGlow(void* g, void* peg, const ui::Color& c)
{
    ui::Color saved = F<ui::Color>(g, kGraphicsOffColor);
    uint8_t colorize = F<uint8_t>(g, kGraphicsOffColorize);
    SetDrawMode(g, 1);
    F<uint8_t>(g, kGraphicsOffColorize) = 1;
    ui::SetColor(g, c);
    if (F<int>(peg, kObjOffKind) == kKindRoundPeg) {
        if (void* glow = ui::Resource<void>(kImagePegGlow))
            DrawImageF(g, glow, CenterX(peg) - Width(glow) / 2.0f, CenterY(peg) - Height(glow) / 2.0f,
                       {0, 0, Width(glow), Height(glow)});
    } else {
        void* glow = BrickImage(peg, kBrickGlowCell);
        void* picture = BrickImage(peg, 0);
        if (glow && picture && Width(picture) > 3) {
            float grow = (Width(glow) - 3.0f) / (Width(picture) - 3.0f);
            float& px = F<float>(peg, kBrickOffPictureX);
            float& py = F<float>(peg, kBrickOffPictureY);
            float saved_x = px, saved_y = py;
            px *= grow;
            py *= grow;
            DrawBrickCell(peg, g, kBrickGlowCell);
            px = saved_x;
            py = saved_y;
        }
    }
    F<uint8_t>(g, kGraphicsOffColorize) = colorize;
    ui::SetColor(g, saved);
    SetDrawMode(g, 0);
}

// The peg glow scaled about its centre (the Graphics' scale, which the drawing at whole pixels follows).
void DrawGlowScaled(void* g, float cx, float cy, float scale, const ui::Color& c)
{
    void* glow = ui::Resource<void>(kImagePegGlow);
    if (!glow || c.a <= 0)
        return;
    int w = Width(glow), h = Height(glow);
    float* s = &F<float>(g, kGraphicsOffScale);
    float saved_scale[4] = {s[0], s[1], s[2], s[3]};
    s[0] = s[1] = scale;
    s[2] = cx + F<float>(g, kGraphicsOffTransX);
    s[3] = cy + F<float>(g, kGraphicsOffTransY);
    ui::Color saved = F<ui::Color>(g, kGraphicsOffColor);
    uint8_t colorize = F<uint8_t>(g, kGraphicsOffColorize);
    SetDrawMode(g, 1);
    F<uint8_t>(g, kGraphicsOffColorize) = 1;
    ui::SetColor(g, c);
    DrawImage(g, glow, static_cast<int>(floorf(cx - w / 2.0f + 0.5f)), static_cast<int>(floorf(cy - h / 2.0f + 0.5f)),
              {0, 0, w, h});
    F<uint8_t>(g, kGraphicsOffColorize) = colorize;
    ui::SetColor(g, saved);
    SetDrawMode(g, 0);
    memcpy(s, saved_scale, sizeof saved_scale);
}

constexpr uint32_t kRedIntroTicks = 70;  // its glow closes in on the red peg as the duel begins
constexpr uint32_t kRedBurstTicks = 45;  // and bursts out when it's hit

void DrawRedGlow(void* g, void* peg)  // under every peg (with the shadows, where the game draws lit pegs' glows)
{
    int p = Lit(peg) ? 255 : Pulse(90, g_red_clock);
    int a = F<int>(peg, kObjOffKind) == kKindRoundPeg ? 80 + p * 90 / 255 : 110 + p * 120 / 255;
    DrawPegGlow(g, peg, {255, 30, 30, a});
}

void DrawObj(ObjDrawFn draw, int part, void* obj, void* g)
{
    if (Hidden(obj))
        return;
    if (!PegInfo(obj)) {
        draw(obj, g);
        return;
    }
    if (obj == g_red) {
        if (part == 1) {
            ++g_n_red;
            DrawTinted(draw, obj, g, kTintRed);
            return;
        }
        draw(obj, g);
        if (part == 0 && F<uint8_t>(obj, kObjOffAlive))
            DrawRedGlow(g, obj);
        return;
    }
    if (obj == g_bomb && On(kBombPeg) && !g_bomb_blown && Unhit(obj)) {
        if (part != 1) {
            draw(obj, g);
            return;
        }
        int p = Pulse(70, Ticks(kBombPeg));  // a bomb: dark, glowing red on and off
        ui::Color saved = F<ui::Color>(g, kGraphicsOffColor);
        uint8_t colorize = F<uint8_t>(g, kGraphicsOffColorize);
        F<uint8_t>(g, kGraphicsOffColorize) = 1;
        ui::SetColor(g, {70 + p * 185 / 255, 25 + p * 40 / 255, 20, 255});
        draw(obj, g);
        F<uint8_t>(g, kGraphicsOffColorize) = colorize;
        ui::SetColor(g, saved);
        return;
    }
    if (On(kSilhouette) && !Lit(obj)) {
        if (part == 2)
            return;  // (the flash of a peg made purple, in its color)
        if (part == 1) {
            ++g_n_gray;
            DrawTinted(draw, obj, g, kTintGray);
            return;
        }
    }
    draw(obj, g);
}

template <int Part>
void __fastcall BallDraw(void* obj, void*, void* g)
{
    DrawObj(g_ball_draw[Part], Part, obj, g);
}

template <int Part>
void __fastcall BrickDraw(void* obj, void*, void* g)
{
    if (Part == 1 && g_flipped && IndexOf(g_bucket, obj) >= 0)
        DrawTurnedOver(g_brick_draw[Part], obj, g);  // the bucket's lid
    else
        DrawObj(g_brick_draw[Part], Part, obj, g);
}

// ---- Shapes ----
struct Point {
    int x, y;
};

GAME_CALL void PolyFill(void* g, const Point* pts, int n)
{
    int convex = 1;
    __asm {
        push convex
        push pts
        mov esi, g
        mov ecx, n
        mov eax, kPolyFill
        call eax
    }
}

GAME_CALL void SetDrawMode(void* g, int mode)
{
    __asm {
        mov eax, g
        mov ecx, mode
        mov edx, kSetDrawMode
        call edx
    }
}

constexpr int kSegments = 24;

void Disc(void* g, float cx, float cy, float r, const ui::Color& c)
{
    Point pts[kSegments];
    for (int i = 0; i < kSegments; ++i) {
        float a = kTwoPi * i / kSegments;
        pts[i] = {static_cast<int>(lroundf(cx + r * cosf(a))), static_cast<int>(lroundf(cy + r * sinf(a)))};
    }
    ui::SetColor(g, c);
    PolyFill(g, pts, kSegments);
}

void Ring(void* g, float cx, float cy, float r0, float r1, const ui::Color& c)
{
    ui::SetColor(g, c);
    for (int i = 0; i < kSegments; ++i) {
        float a0 = kTwoPi * i / kSegments, a1 = kTwoPi * (i + 1) / kSegments;
        Point q[4] = {
            {static_cast<int>(lroundf(cx + r0 * cosf(a0))), static_cast<int>(lroundf(cy + r0 * sinf(a0)))},
            {static_cast<int>(lroundf(cx + r1 * cosf(a0))), static_cast<int>(lroundf(cy + r1 * sinf(a0)))},
            {static_cast<int>(lroundf(cx + r1 * cosf(a1))), static_cast<int>(lroundf(cy + r1 * sinf(a1)))},
            {static_cast<int>(lroundf(cx + r0 * cosf(a1))), static_cast<int>(lroundf(cy + r0 * sinf(a1)))},
        };
        PolyFill(g, q, 4);
    }
}

const ui::Color kPairColors[3] = {{50, 200, 255, 255}, {255, 80, 220, 255}, {140, 255, 70, 255}};

ui::Color Alpha(ui::Color c, int a)
{
    c.a = a;
    return c;
}

void DrawPortal(void* g, float cx, float cy, const ui::Color& c, float spin)
{
    SetDrawMode(g, 1);
    Disc(g, cx, cy, kPortalR + 9, Alpha(c, 45));
    Disc(g, cx, cy, kPortalR + 4, Alpha(c, 60));
    SetDrawMode(g, 0);
    Disc(g, cx, cy, kPortalR, {12, 4, 30, 235});
    SetDrawMode(g, 1);
    for (int arm = 0; arm < 3; ++arm) {  // a swirl
        for (int s = 0; s < 7; ++s) {
            float t0 = s / 7.0f, t1 = (s + 1) / 7.0f;
            float a0 = spin + arm * kTwoPi / 3 + t0 * 2.4f, a1 = spin + arm * kTwoPi / 3 + t1 * 2.4f;
            float r0 = 2 + t0 * (kPortalR - 3), r1 = 2 + t1 * (kPortalR - 3), w = 1.5f + t0 * 1.5f;
            Point q[4] = {
                {static_cast<int>(cx + r0 * cosf(a0)), static_cast<int>(cy + r0 * sinf(a0))},
                {static_cast<int>(cx + (r0 + w) * cosf(a0 + 0.35f)), static_cast<int>(cy + (r0 + w) * sinf(a0 + 0.35f))},
                {static_cast<int>(cx + (r1 + w) * cosf(a1 + 0.35f)), static_cast<int>(cy + (r1 + w) * sinf(a1 + 0.35f))},
                {static_cast<int>(cx + r1 * cosf(a1)), static_cast<int>(cy + r1 * sinf(a1))},
            };
            ui::SetColor(g, Alpha(c, 70 + s * 20));
            PolyFill(g, q, 4);
        }
    }
    SetDrawMode(g, 0);
    Ring(g, cx, cy, kPortalR - 2, kPortalR + 1, Alpha(c, 230));
}

// ---- Lights Out: darkness but for two sweeping spotlights ----
constexpr float kConeHalf = 0.10f, kPenumbra = 0.035f;
constexpr int kStrip = 4;
constexpr uint32_t kSweepTicks = 640;

// Where a spotlight at the bottom left corner, pointing `dir` radians above horizontal, lights a strip `dy` above
// it with half-angle `half`.
void ConeSpan(float dir, float half, float dy, float& x0, float& x1)
{
    float lo = dir - half, hi = dir + half;
    x0 = hi >= kPi / 2 ? (hi >= kPi ? -1e6f : dy / tanf(hi)) : dy / tanf(hi);
    x1 = lo <= 0 ? 1e6f : dy / tanf(lo);
}

void DrawDarkness(void* g, float dir)
{
    const int dark[4] = {0, 70, 150, 238};  // in the light, its edge, its outer edge, darkness
    for (int y = 0; y < kFieldH; y += kStrip) {
        float dy = kFieldH - (y + kStrip / 2.0f);
        // Each light's three nested spans (light, edge, outer edge), the right one mirrored.
        float spans[2][3][2];
        for (int level = 0; level < 3; ++level) {
            float x0, x1;
            ConeSpan(dir, kConeHalf + level * kPenumbra, dy, x0, x1);
            spans[0][level][0] = x0;
            spans[0][level][1] = x1;
            spans[1][level][0] = kFieldW - x1;
            spans[1][level][1] = kFieldW - x0;
        }
        float cuts[14];
        int n = 0;
        cuts[n++] = 0;
        cuts[n++] = kFieldW;
        for (auto& light : spans)
            for (auto& s : light)
                for (float v : s)
                    if (v > 0 && v < kFieldW)
                        cuts[n++] = v;
        for (int i = 1; i < n; ++i)  // sort (a handful)
            for (int j = i; j > 0 && cuts[j] < cuts[j - 1]; --j) {
                float t = cuts[j];
                cuts[j] = cuts[j - 1];
                cuts[j - 1] = t;
            }
        int run_start = 0, run_level = -1;
        for (int i = 0; i + 1 < n; ++i) {
            float mid = (cuts[i] + cuts[i + 1]) / 2;
            int level = 3;
            for (auto& light : spans)
                for (int l = 0; l < 3; ++l)
                    if (mid >= light[l][0] && mid <= light[l][1] && l < level)
                        level = l;
            int x = static_cast<int>(cuts[i]);
            if (level != run_level) {
                if (run_level > 0) {
                    ui::SetColor(g, {0, 0, 0, dark[run_level]});
                    ui::FillRect(g, run_start, y, x - run_start, kStrip);
                }
                run_start = x;
                run_level = level;
            }
        }
        if (run_level > 0) {
            ui::SetColor(g, {0, 0, 0, dark[run_level]});
            ui::FillRect(g, run_start, y, static_cast<int>(kFieldW) - run_start, kStrip);
        }
    }
    SetDrawMode(g, 1);  // the lamps
    Disc(g, 0, kFieldH, 16, {255, 240, 190, 120});
    Disc(g, kFieldW, kFieldH, 16, {255, 240, 190, 120});
    SetDrawMode(g, 0);
}

void DrawLightsOut(void* board, void* g)
{
    // From flat to straight up and back, slowing down at each end like the cannon's swing.
    float t = static_cast<float>(Ticks(kLightsOut) % kSweepTicks) / kSweepTicks;
    float dir = (kPi / 2) * (1 - cosf(kTwoPi * t)) / 2;
    DrawDarkness(g, dir);
    // The balls and the pegs they lit glow in the dark.
    for (void* peg : Pegs(Logic(board)))
        if (F<uint8_t>(peg, kObjOffAlive) && Lit(peg))
            VCall<ObjDrawFn>(peg, kObjSlotDraw)(peg, g);
    for (void* ball : Balls(board))
        VCall<ObjDrawFn>(ball, kObjSlotDraw)(ball, g);
}

// ---- Drawing over the board ----
void DrawTwinBucket(void* board, void* g)
{
    if (!g_hole || Fever(board))
        return;
    float& tx = F<float>(g, kGraphicsOffTransX);
    tx += kTwinOffset;
    for (void* part : g_bucket)
        if (F<uint8_t>(part, kObjOffAlive))
            VCall<ObjDrawFn>(part, kObjSlotDraw)(part, g);
    tx -= kTwinOffset;
}

void DrawTeleport(void* g)
{
    if (g_teleported_at == 0xFFFFFFFF || !g_hole)
        return;
    uint32_t age = Ticks(kTeleportingBucket) - g_teleported_at;
    if (age >= 40)
        return;
    float y = F<float>(g_hole, kHoleOffY);
    int a = 255 - static_cast<int>(age) * 255 / 40;
    float grow = static_cast<float>(age);
    SetDrawMode(g, 1);
    Ring(g, g_teleport_from, y, 10 + grow * 1.6f, 16 + grow * 1.8f, {120, 220, 255, a});
    Ring(g, F<float>(g_hole, kHoleOffX), y, 70 - grow * 1.4f > 4 ? 70 - grow * 1.4f : 4,
         78 - grow * 1.4f > 10 ? 78 - grow * 1.4f : 10, {255, 255, 160, a});
    SetDrawMode(g, 0);
}

void DrawBombMarker(void* g)
{
    if (!g_bomb || g_bomb_blown || !Unhit(g_bomb))
        return;
    float cx = (F<float>(g_bomb, kObjOffLeft) + F<float>(g_bomb, kObjOffRight)) / 2;
    float cy = (F<float>(g_bomb, kObjOffTop) + F<float>(g_bomb, kObjOffBottom)) / 2;
    float r = (F<float>(g_bomb, kObjOffRight) - F<float>(g_bomb, kObjOffLeft)) / 2;
    int p = Pulse(70, Ticks(kBombPeg));
    SetDrawMode(g, 1);
    Disc(g, cx, cy, r + 14 + p / 32.0f, {255, 60, 0, 40 + p / 5});  // a warning glow
    Ring(g, cx, cy, r + 3 + p / 64.0f, r + 8 + p / 32.0f, {255, 80 + p / 3, 20, 140 + p / 3});
    Ring(g, cx, cy, r + 13 + p / 20.0f, r + 15 + p / 20.0f, {255, 200, 60, 200 - p / 2});
    Disc(g, cx + r * 0.75f, cy - r * 0.75f, 2.5f + p / 100.0f, {255, 240, 150, 255});  // the fuse's spark
    SetDrawMode(g, 0);
}

void DrawWrapEdges(void* g)
{
    // Glowing edges (the frame covers the first few pixels), with arrows across each.
    int a = Pulse(90, Ticks(kWrapAround));
    SetDrawMode(g, 1);
    for (int i = 0; i < 4; ++i) {
        ui::SetColor(g, {60, 200, 255, (90 + a / 3) * (4 - i) / 5});
        ui::FillRect(g, 6 + i * 3, 0, 3, static_cast<int>(kFieldH));
        ui::FillRect(g, static_cast<int>(kFieldW) - 9 - i * 3, 0, 3, static_cast<int>(kFieldH));
    }
    ui::Color c = {150, 235, 255, 120 + a / 3};
    for (int y = 60; y < kFieldH - 40; y += 110) {
        Point left[3] = {{8, y}, {22, y - 10}, {22, y + 10}};       // out on the left ...
        Point right[3] = {{static_cast<int>(kFieldW) - 8, y}, {static_cast<int>(kFieldW) - 22, y - 10},
                          {static_cast<int>(kFieldW) - 22, y + 10}};  // ... and on the right
        ui::SetColor(g, c);
        PolyFill(g, left, 3);
        PolyFill(g, right, 3);
    }
    SetDrawMode(g, 0);
}

GAME_CALL void DrawImageF(void* g, void* image, float x, float y, const ui::Rect& src)
{
    const ui::Rect* r = &src;
    __asm {
        push r
        push y
        push x
        push image
        mov eax, g
        mov ecx, kDrawImageF
        call ecx
    }
}

uint32_t Mix(uint32_t a, uint32_t b)  // a hash (the glitch's jitter: the same picture on both computers)
{
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

// A gone peg drawn as it was (unlit, in the fake's color) by the game's own drawing of its kind: parts 0 (its shadow)
// to 1 (itself).
void DrawGonePeg(void* peg, void* g, bool orange, int first_part, int last_part)
{
    int& type = F<int>(PegInfo(peg), kPegOffType);
    uint8_t& lit = F<uint8_t>(PegInfo(peg), kPegOffLit);
    int saved_type = type;
    uint8_t saved_lit = lit;
    type = orange ? kOrange : kBlue;
    lit = 0;
    ObjDrawFn* draw = *reinterpret_cast<uintptr_t*>(peg) == kBallVtable ? g_ball_draw : g_brick_draw;
    for (int part = first_part; part <= last_part; ++part)
        draw[part](peg, g);
    type = saved_type;
    lit = saved_lit;
}

// A fake in a gone brick's place torn into bands (its drawing clipped to each) as a round one is into slices.
void DrawBrickBands(void* g, const Fake& f, uint32_t frame, float t, int a)
{
    ui::Rect& clip = F<ui::Rect>(g, kGraphicsOffClip);
    float& tx = F<float>(g, kGraphicsOffTransX);
    ui::Rect saved_clip = clip;
    float saved_tx = tx, ty = F<float>(g, kGraphicsOffTransY);
    int top = static_cast<int>(F<float>(f.peg, kObjOffTop) + ty) - 2;
    int bottom = static_cast<int>(F<float>(f.peg, kObjOffBottom) + ty) + 3;
    const int bands = 4;
    int band_h = (bottom - top + bands - 1) / bands;
    for (int s = 0; s < bands; ++s) {
        uint32_t h = Mix(frame, 10 + s);
        float dx = (static_cast<int>(h % 9) - 4) * (1.0f + 4.0f * t), split = 2.0f + 4.0f * t;
        int y0 = std::max(top + s * band_h, saved_clip.y);
        int y1 = std::min(top + (s + 1) * band_h, saved_clip.y + saved_clip.h);
        if (y1 <= y0)
            continue;
        clip = {saved_clip.x, y0, saved_clip.w, y1 - y0};
        const struct {
            float dx;
            ui::Color c;
            int mode;
        } passes[3] = {{dx, {255, 255, 255, a * 3 / 4}, 0},
                       {dx - split, {255, 30, 90, a}, 1},
                       {dx + split, {30, 230, 255, a}, 1}};
        for (const auto& p : passes) {
            tx = saved_tx + p.dx;
            SetDrawMode(g, p.mode);
            F<uint8_t>(g, kGraphicsOffColorize) = 1;
            ui::SetColor(g, p.c);
            DrawGonePeg(f.peg, g, f.orange, 1, 1);
        }
    }
    tx = saved_tx;
    clip = saved_clip;
}

// A cyber glitch: the peg torn into slices that jump sideways, split into red and cyan and flickering, with bars of
// static round it that fly apart as it goes.
void DrawGlitch(void* g, void* sheet, const ui::Rect& cell, const Fake& f, int index, uint32_t age)
{
    float t = static_cast<float>(age) / kGlitchTicks;
    uint32_t frame = age / 2 * 31 + static_cast<uint32_t>(index);
    int a = static_cast<int>(255 * (1 - t * t));
    SetDrawMode(g, 1);
    if (age < 8)  // the moment it's found out: a flash
        Disc(g, f.x, f.y, kFakeR + 4 + age, {120, 255, 255, 160 - static_cast<int>(age) * 20});
    if ((age < 6 || (Mix(frame, 1) & 7) != 0) && f.peg && IsBrick(f.peg)) {
        DrawBrickBands(g, f, frame, t, a);
        F<uint8_t>(g, kGraphicsOffColorize) = 0;
        SetDrawMode(g, 1);
    } else if (age < 6 || (Mix(frame, 1) & 7) != 0) {
        const int slices = 5;
        int slice_h = cell.h / slices;
        F<uint8_t>(g, kGraphicsOffColorize) = 1;
        for (int s = 0; s < slices; ++s) {
            uint32_t h = Mix(frame, 10 + s);
            float dx = (static_cast<int>(h % 9) - 4) * (1.0f + 4.0f * t), split = 2.0f + 4.0f * t;
            ui::Rect src = {cell.x, cell.y + s * slice_h, cell.w, s == slices - 1 ? cell.h - s * slice_h : slice_h};
            float x = f.x - 11 + dx, y = f.y - 11 + s * slice_h;
            SetDrawMode(g, 0);
            ui::SetColor(g, {255, 255, 255, a * 3 / 4});
            DrawImageF(g, sheet, x, y, src);
            SetDrawMode(g, 1);
            ui::SetColor(g, {255, 30, 90, a});
            DrawImageF(g, sheet, x - split, y, src);
            ui::SetColor(g, {30, 230, 255, a});
            DrawImageF(g, sheet, x + split, y, src);
        }
        F<uint8_t>(g, kGraphicsOffColorize) = 0;
    }
    static const ui::Color kStatic[3] = {{0, 255, 230, 0}, {255, 40, 220, 0}, {235, 255, 255, 0}};
    int bars = 4 + static_cast<int>(t * 8);
    float spread = 12 + 26 * t;
    for (int b = 0; b < bars; ++b) {
        uint32_t h = Mix(frame, 100 + b);
        int w = 3 + static_cast<int>(h % 12), bar_h = 1 + static_cast<int>(h >> 4 & 3);
        int x = static_cast<int>(f.x - spread) + static_cast<int>((h >> 8) % static_cast<uint32_t>(2 * spread)) - w / 2;
        int y = static_cast<int>(f.y - spread * 0.8f) + static_cast<int>((h >> 16) % static_cast<uint32_t>(1.6f * spread));
        ui::Color c = kStatic[(h >> 24) % 3];
        c.a = a * 3 / 4;
        ui::SetColor(g, c);
        ui::FillRect(g, x, y, w, bar_h);
    }
    SetDrawMode(g, 0);
}

// Mirage's fake pegs, drawn as the game draws a peg (its shadow, then its cell; one in a gone peg's place, that peg
// by its own drawing); they hide with Invisible Pegs.
void DrawFakes(void* g)
{
    void* sheet = *reinterpret_cast<void**>(*reinterpret_cast<uint8_t*>(kColorblind) ? kImagePegsColorblind
                                                                                       : kImagePegs);
    void* shadow = ui::Resource<void>(kImagePegShadow);
    if (!sheet)
        return;
    int cell_w = F<int>(sheet, game::kImageOffWidth) / F<int>(sheet, kImageOffCols);
    int cell_h = F<int>(sheet, game::kImageOffHeight) / F<int>(sheet, game::kImageOffRows);
    ui::Color saved = F<ui::Color>(g, kGraphicsOffColor);
    uint8_t colorize = F<uint8_t>(g, kGraphicsOffColorize);
    for (int i = 0; i < g_fake_count; ++i) {
        const Fake& f = g_fakes[i];
        ui::Rect cell = {0, (f.orange ? 1 : 0) * cell_h, cell_w, cell_h};
        if (f.glitched) {
            uint32_t age = Ticks(kMirage) - (f.glitched - 1);
            if (age < kGlitchTicks)
                DrawGlitch(g, sheet, cell, f, i, age);
            continue;
        }
        if (On(kInvisiblePegs) || FakesHidden())
            continue;
        if (f.peg) {
            F<uint8_t>(g, kGraphicsOffColorize) = 0;
            ui::SetColor(g, {255, 255, 255, 255});
            DrawGonePeg(f.peg, g, f.orange, 0, 1);
            continue;
        }
        if (shadow) {
            F<uint8_t>(g, kGraphicsOffColorize) = 1;
            ui::SetColor(g, {0, 0, 0, 0x90});
            DrawImageF(g, shadow, f.x - 14, f.y - 8,
                       {0, 0, F<int>(shadow, game::kImageOffWidth), F<int>(shadow, game::kImageOffHeight)});
        }
        F<uint8_t>(g, kGraphicsOffColorize) = 0;
        ui::SetColor(g, {255, 255, 255, 255});
        DrawImageF(g, sheet, f.x - 11, f.y - 11, cell);
    }
    F<uint8_t>(g, kGraphicsOffColorize) = colorize;
    ui::SetColor(g, saved);
}

// The red peg: a wide glow closing in on it as the duel begins, a burst of glow when it's hit.
void* g_red_board;

void DrawRedMarks(void* g)
{
    if (g_red && !g_red_hit_at && g_red_clock < kRedIntroTicks && Unhit(g_red)) {
        float t = static_cast<float>(g_red_clock) / kRedIntroTicks;
        DrawGlowScaled(g, CenterX(g_red), CenterY(g_red), 1 + 3.5f * (1 - t) * (1 - t),
                       {255, 40, 40, static_cast<int>(230 * sinf(kPi * t))});
    }
    if (g_red_hit_at && g_red_clock - g_red_hit_at < kRedBurstTicks) {
        float u = static_cast<float>(g_red_clock - g_red_hit_at) / kRedBurstTicks, fade = (1 - u) * (1 - u);
        DrawGlowScaled(g, g_red_x, g_red_y, 1.2f + 3 * u, {255, 40, 40, static_cast<int>(255 * fade)});
        DrawGlowScaled(g, g_red_x, g_red_y, 0.8f + 1.2f * u, {255, 190, 170, static_cast<int>(200 * fade * (1 - u))});
    }
}

void __cdecl DrawUnder(void* board, void* g)  // after the pegs, under the balls (g: the playfield's, unclipped)
{
    ++g_n_under;
    if (board == g_red_board && g_red)
        DrawRedMarks(g);
    if (board != g_board)
        return;
    if (On(kWormholes) || On(kPortalStorm)) {
        float spin = Ticks(On(kWormholes) ? kWormholes : kPortalStorm) * 0.06f;
        for (int i = 0; i < g_portal_count; ++i)
            DrawPortal(g, g_portals[i].x, g_portals[i].y, kPairColors[i / 2], i & 1 ? -spin : spin);
    }
    if (On(kDoubleBucket))
        DrawTwinBucket(board, g);
    if (On(kTeleportingBucket))
        DrawTeleport(g);
    if (On(kBombPeg))
        DrawBombMarker(g);
    if (On(kMirage))
        DrawFakes(g);
}

void __cdecl DrawOver(void* board, void* g)  // over the balls, under the cannon and the frame (g: clipped)
{
    ++g_n_over;
    if (board == g_board) {
        if (On(kLightsOut))
            DrawLightsOut(board, g);
        if (On(kWrapAround))
            DrawWrapEdges(g);  // (over the dark: the way out stays to be seen)
    }
    int black = board == g_blackout_board && !F<uint8_t>(Logic(board), kLogicOffReplaying) ? BlackoutAlpha() : 0;
    if (black > 0) {
        ui::SetColor(g, {0, 0, 0, black});
        ui::FillRect(g, 0, 0, static_cast<int>(kFieldW), static_cast<int>(kFieldH));
    }
}

uintptr_t g_orig_under_balls, g_orig_over_board;

__declspec(naked) void UnderBallsHook()  // replaces a call: EAX Board+0x714, EBX Board, stdcall(&g2, -1)
{
    __asm {
        push dword ptr [esp + 8]
        push dword ptr [esp + 8]
        call dword ptr [g_orig_under_balls]
        pushad
        push dword ptr [esp + 36]
        push ebx
        call DrawUnder
        add esp, 8
        popad
        ret 8
    }
}

__declspec(naked) void OverBoardHook()  // replaces a call: EBX Board, stdcall(g, &g1)
{
    __asm {
        pushad
        push dword ptr [esp + 40]
        push ebx
        call DrawOver
        add esp, 8
        popad
        jmp dword ptr [g_orig_over_board]
    }
}

// An effect's own state back to nothing (its board may be gone).
void Forget(Effect e)
{
    switch (e) {
    case kBigBalls:
        g_enlarged.clear();
        break;
    case kPinballMadness:
        g_speed_cap_sq = 225;
        g_speed_cap = 15;
        break;
    case kDoubleBucket:
        for (void* ball : g_pending_catches)
            Release(ball);
        g_pending_catches.clear();
        g_drop_in = false;
        break;
    case kTeleportingBucket:
        g_next_teleport = 0;
        g_teleported_at = 0xFFFFFFFF;
        break;
    case kPurpleParty:
        g_extra_purples.clear();
        break;
    case kBombPeg:
        g_bomb = nullptr;
        g_bomb_index = -1;
        g_bomb_blown = false;
        break;
    case kBlindShot:
        g_hide_guide = false;
        break;
    case kWormholes:
    case kPortalStorm:
        g_portal_count = 0;
        break;
    case kUpsideDown:
        SetFlipped(false);  // (the board went away upside down: put the game's code back)
        break;
    case kBulletTime:
        g_slowed.clear();
        break;
    case kMirage:
        g_fake_count = 0;
        break;
    default:
        break;
    }
    g_active &= ~(1u << e);
    if (!g_active)
        g_board = nullptr;
}

void SetRed(void* board, void* peg)
{
    g_red = peg;
    g_red_board = peg ? board : nullptr;
    g_red_index = peg ? IndexOf(Pegs(Logic(board)), peg) : -1;
    g_red_clock = 0;
    g_red_hit_at = 0;
    g_tinted_stale = true;
}

// "Sabotage!" floating up over it with the power-up sound, as the game names a power when its peg is hit.
uint32_t g_n_red_texts;  // testing

GAME_CALL void RedPegText(void* board, float x, float y)
{
    void* lm = Logic(board);
    game::GameString text("Sabotage!");
    game::GameString* pt = &text;
    float ty = y - 40 < 15 ? 15 : y - 40;
    void* ft;
    __asm {
        push ty
        push x
        push pt
        mov eax, lm
        mov edi, -1
        mov ecx, kShowText
        call ecx
        mov ft, eax
    }
    if (ft) {
        F<float>(ft, kTextOffRise) = -0.5f;
        F<uint32_t>(ft, kTextOffColor) = 0xFF5050;
        F<uint32_t>(ft, kTextOff6C) = 0xFFFFFFDD;
        F<int>(ft, kTextOff78) = 10;
        F<int>(ft, kTextOffLife) = 150;
    }
    ui::PlaySound(kSoundPowerUp);
    ++g_n_red_texts;
}

// ---- Each physics step (every board, every state; the game's look-aheads have physics of their own) ----
void StepEffects(void* board)
{
    if (g_red && board == g_red_board) {
        ++g_red_clock;
        if (!g_red_hit_at && (!F<uint8_t>(g_red, kObjOffAlive) || Lit(g_red))) {
            g_red_hit_at = g_red_clock;
            g_red_x = CenterX(g_red);
            g_red_y = CenterY(g_red);
            RedPegText(board, g_red_x, g_red_y);
        }
    }
    if (!g_active || board != g_board)
        return;
    for (int i = 0; i < kEffectCount; ++i) {
        auto e = static_cast<Effect>(i);
        if (!On(e))
            continue;
        ++g_ticks[e];
        switch (e) {
        case kBigBalls:
            EnlargeBalls(board);  // new balls start small
            break;
        case kTeleportingBucket:
            TeleportBucket(board);
            break;
        case kUpsideDown:
            if (Fever(board))
                Flip(board, false);  // Extreme Fever's buckets are at the bottom
            break;
        case kBulletTime:
            if (F<int>(Logic(board), kLogicOffState) != kStateBallInPlay)
                g_slowed.clear();
            break;
        case kWormholes:
        case kPortalStorm:
            if (F<int>(Logic(board), kLogicOffState) != kStateBallInPlay)
                for (Cooldown& c : g_cooldown)
                    c = {nullptr, -1};
            break;
        case kMirage:
            FollowFakes();
            break;
        default:
            break;
        }
    }
}

// Testing: where the balls are after each physics step of a shot (walking the list: no allocation).
uint32_t g_trace = 2166136261u, g_trace_steps;

void TraceStep(void* board)
{
    void* lm = Logic(board);
    if (!lm || F<int>(lm, kLogicOffState) != kStateBallInPlay)
        return;
    Node* head = F<Node*>(board, kBoardOffBalls);
    for (Node* n = head ? head->next : nullptr; n && n != head; n = n->next) {
        if (!n->obj)
            continue;
        g_trace = (g_trace ^ Bits(F<float>(n->obj, kBallOffX))) * 16777619u;
        g_trace = (g_trace ^ Bits(F<float>(n->obj, kBallOffY))) * 16777619u;
    }
    ++g_trace_steps;
}

void __cdecl OnPhysicsStep(void* lm)
{
    CatchPending();
    void* board = lm ? F<void*>(lm, kLogicOffBoard) : nullptr;
    if (!board)
        return;
    StepEffects(board);
    StepBlackout(board);
    TraceStep(board);
}

// ---- Shots kept for Instant Replay: pegs by their place in the level's order, and where they were ----
ShotPeg PegRef(void* lm, void* peg)
{
    if (!peg)
        return {-1, 0, 0};
    return {IndexOf(Pegs(lm), peg), CenterX(peg), CenterY(peg)};
}

void* PegAt(void* lm, const ShotPeg& r)  // (a copy of the board lists its pegs in the same order, it seems)
{
    if (r.index < 0)
        return nullptr;
    std::vector<void*> pegs = Pegs(lm);
    if (r.index < static_cast<int>(pegs.size())) {
        void* p = pegs[r.index];
        float dx = CenterX(p) - r.x, dy = CenterY(p) - r.y;
        if (dx * dx + dy * dy < 1)
            return p;
    }
    void* best = nullptr;
    float best_d = 25;  // within 5 pixels
    for (void* p : pegs) {
        float dx = CenterX(p) - r.x, dy = CenterY(p) - r.y, d = dx * dx + dy * dy;
        if (d < best_d) {
            best_d = d;
            best = p;
        }
    }
    return best;
}

}  // namespace

void InstallEffects()
{
    g_orig_choose_purple = reinterpret_cast<ChoosePurpleFn>(
        HookInline(kChoosePurple, 5, reinterpret_cast<void*>(HookChoosePurple)));
    g_orig_peg_hit = reinterpret_cast<PegHitFn>(HookInline(kPegHit, 6, reinterpret_cast<void*>(HookPegHit)));
    void* ball_hooks[3] = {reinterpret_cast<void*>(BallDraw<0>), reinterpret_cast<void*>(BallDraw<1>),
                           reinterpret_cast<void*>(BallDraw<2>)};
    void* brick_hooks[3] = {reinterpret_cast<void*>(BrickDraw<0>), reinterpret_cast<void*>(BrickDraw<1>),
                            reinterpret_cast<void*>(BrickDraw<2>)};
    for (int i = 0; i < 3; ++i) {
        g_ball_draw[i] = reinterpret_cast<ObjDrawFn>(HookVtable(kBallVtable, kObjSlotDrawShadow + i, ball_hooks[i]));
        g_brick_draw[i] =
            reinterpret_cast<ObjDrawFn>(HookVtable(kBrickVtable, kObjSlotDrawShadow + i, brick_hooks[i]));
    }
    g_hole_draw = reinterpret_cast<ObjDrawFn>(HookVtable(kHoleVtable, kObjSlotDraw, reinterpret_cast<void*>(HoleDraw)));
    g_poly_draw = reinterpret_cast<ObjDrawFn>(HookVtable(kPolyVtable, kObjSlotDraw, reinterpret_cast<void*>(PolyDraw)));
    uint32_t callback = reinterpret_cast<uint32_t>(CollisionCallback);
    PatchBytes(kCollisionCallback, &callback, 4);
    double* cap_sq = &g_speed_cap_sq;
    double* cap = &g_speed_cap;
    PatchBytes(kSpeedCapSq, &cap_sq, 4);
    PatchBytes(kSpeedCap, &cap, 4);
    g_orig_shot_bookkeeping = HookCall(kShotFired, reinterpret_cast<void*>(ShotFiredHook));
    HookCall(kZenShotFired, reinterpret_cast<void*>(ZenShotFiredHook));  // (the same function)
    g_ball_step_tramp = HookInline(kBallStep, 6, reinterpret_cast<void*>(BallStepHook));
    g_orig_hit_processing = HookCall(kHitProcessing, reinterpret_cast<void*>(HitProcessingHook));
    g_aim_tramp = HookInline(kAimAtMouse, 6, reinterpret_cast<void*>(AimHook));
    g_clamp_tramp = HookInline(kClampAim, 7, reinterpret_cast<void*>(ClampHook));
    g_orig_turn_gun = HookCall(kTurnGun, reinterpret_cast<void*>(TurnGunHook));
    g_guide_tramp = HookInline(kDrawGuide, 6, reinterpret_cast<void*>(GuideHook));
    g_orig_under_balls = HookCall(kDrawUnderBalls, reinterpret_cast<void*>(UnderBallsHook));
    g_orig_over_board = HookCall(kDrawOverBoard, reinterpret_cast<void*>(OverBoardHook));
    float* spooky_y = &g_spooky_y;
    for (uintptr_t at : kSpookyY)
        PatchBytes(at, &spooky_y, 4);
    g_orig_spooky_check = HookCall(kSpookyCheck, reinterpret_cast<void*>(SpookyCheckHook));
    g_orig_zap = HookCall(kZap, reinterpret_cast<void*>(ZapHook));
    g_orig_rod_spark = HookCall(kRodSpark, reinterpret_cast<void*>(RodSparkHook));
    g_orig_zap_preview = HookCall(kZapPreview, reinterpret_cast<void*>(ZapPreviewHook));
    g_orig_draw_rod = HookCall(kDrawRod, reinterpret_cast<void*>(DrawRodHook));
    g_orig_catch = HookCall(kCatchCall, reinterpret_cast<void*>(CatchHook));
    g_orig_ease_track = HookCall(kEaseTrack, reinterpret_cast<void*>(EaseTrackHook));
}

void BoardDrawBegins(void* board)
{
    RodMirrorBegins(board);
}

void BoardDrawEnds()
{
    RodMirrorEnds();
}

void UpdateEffects()
{
    void* board = game::App() ? game::At<void*>(game::App(), game::kAppOffBoard) : nullptr;
    if (!g_tinted.empty() && board != g_tinted_board) {
        FreeTinted();
        g_tinted_board = board;
    }
}

bool EffectPossible(void* board, Effect e)
{
    void* lm = Logic(board);
    switch (e) {
    case kPurpleParty:
        return BluePegs(lm).size() >= 1 + kExtraPurples;
    case kPegShuffle:
        return ShufflePossible(lm);
    case kBombPeg:
    case kMirage:
        return !UnhitPegs(lm).empty();
    case kDoubleBucket:
    case kTeleportingBucket:
    case kUpsideDown:
        FindBucket(board);
        return g_hole != nullptr && !Fever(board);
    case kWrapAround:
        FindWalls(board);
        return g_walls[0] && g_walls[1];
    default:
        return true;
    }
}

void BeginEffect(void* board, Effect e)
{
    if (On(e))
        return;
    g_board = board;
    g_active |= 1u << e;
    g_ticks[e] = 0;
    void* lm = Logic(board);
    switch (e) {
    case kBigBalls:
        EnlargeBalls(board);
        break;
    case kPinballMadness:
        g_speed_cap_sq = 22.0 * 22.0;
        g_speed_cap = 22.0;
        break;
    case kDoubleBucket:
        FindBucket(board);
        SplitBucket(true);
        break;
    case kTeleportingBucket:
        FindBucket(board);
        g_next_teleport = 150;
        g_teleported_at = 0xFFFFFFFF;
        break;
    case kPurpleParty:
        AddExtraPurples(lm);
        break;
    case kPegShuffle:
        ShufflePegs(lm);
        break;
    case kBlindShot:
        g_hide_guide = true;
        break;
    case kWormholes:
        PlacePortals(board, 1);
        break;
    case kPortalStorm:
        PlacePortals(board, 3);
        break;
    case kWrapAround:
        FindWalls(board);
        SetCollides(g_walls[0], false);
        SetCollides(g_walls[1], false);
        break;
    case kBombPeg: {
        std::vector<void*> pegs, round;
        for (void* p : UnhitPegs(lm))
            if (p != g_red)
                (F<int>(p, kObjOffKind) == kKindRoundPeg ? round : pegs).push_back(p);
        if (!round.empty())
            pegs.swap(round);  // a round peg looks more like a bomb than a brick does
        g_bomb = pegs.empty() ? nullptr : pegs[Random(static_cast<uint32_t>(pegs.size()))];
        g_bomb_index = g_bomb ? IndexOf(Pegs(lm), g_bomb) : -1;
        g_bomb_blown = false;
        if (g_bomb)
            Flash(g_bomb);
        break;
    }
    case kUpsideDown:
        FindBucket(board);
        FindWalls(board);
        Flip(board, true);
        break;
    case kBulletTime:
        g_slowed.clear();
        break;
    case kMirage:
        PlaceFakes(board);
        break;
    case kSilhouette:
        g_tinted_stale = true;
        break;
    default:
        break;
    }
}

void EndEffect(void* board, Effect e)
{
    if (!On(e))
        return;
    switch (e) {
    case kBigBalls:
        ShrinkBalls(board);
        break;
    case kPinballMadness:
        g_speed_cap_sq = 225;
        g_speed_cap = 15;
        break;
    case kDoubleBucket:
        SplitBucket(false);
        break;
    case kPurpleParty:
        ClearExtraPurples();
        break;
    case kWrapAround:
        SetCollides(g_walls[0], true);
        SetCollides(g_walls[1], true);
        break;
    case kUpsideDown:
        Flip(board, false);
        break;
    default:
        break;
    }
    Forget(e);
}

void DropEffects(void* keep_board)
{
    if (g_blackout && g_blackout_board != keep_board)
        CancelBlackout();
    if (g_board && g_board == keep_board)
        return;
    for (int i = 0; i < kEffectCount; ++i)
        if (On(static_cast<Effect>(i)))
            Forget(static_cast<Effect>(i));
    g_active = 0;
    g_board = nullptr;
    g_bucket.clear();  // (dealing looks for them, whatever it deals)
    g_hole = nullptr;
    for (void*& w : g_walls)
        w = nullptr;
}

void StartBlackout(void* board)
{
    g_blackout = kBlackoutSoon;
    g_blackout_steps = 0;
    g_blackout_board = board;
}

void CancelBlackout()
{
    g_blackout = kNoBlackout;
    g_blackout_steps = 0;
    g_blackout_board = nullptr;
}

void CaptureShot(void* board, ShotState& s)
{
    memset(&s, 0, sizeof s);
    s.size = sizeof s;
    void* lm = Logic(board);
    if (!lm)
        return;
    s.active = g_board == board ? g_active : 0;
    memcpy(s.ticks, g_ticks, sizeof g_ticks);
    s.rng = RandomState();
    s.shooter = F<int>(lm, kLogicOffPlayer) & 1;
    s.red = g_red && g_red_board == board ? PegRef(lm, g_red) : ShotPeg{-1, 0, 0};
    s.red_clock = g_red_clock;
    s.red_hit_at = g_red_hit_at;
    s.red_x = g_red_x;
    s.red_y = g_red_y;
    s.bomb = s.active ? PegRef(lm, g_bomb) : ShotPeg{-1, 0, 0};
    s.bomb_blown = g_bomb_blown;
    for (void* p : g_extra_purples)
        if (s.purple_count < kShotMaxPurples && s.active)
            s.purples[s.purple_count++] = PegRef(lm, p);
    s.next_teleport = g_next_teleport;
    s.teleported_at = g_teleported_at;
    s.teleport_from = g_teleport_from;
    s.portal_count = s.active ? g_portal_count : 0;
    for (int i = 0; i < s.portal_count; ++i) {
        s.portals[i][0] = g_portals[i].x;
        s.portals[i][1] = g_portals[i].y;
    }
    s.fake_count = s.active ? g_fake_count : 0;
    for (int i = 0; i < kShotMaxFakes; ++i)
        s.fake_pegs[i] = i < s.fake_count ? PegRef(lm, g_fakes[i].peg) : ShotPeg{-1, 0, 0};
    for (int i = 0; i < s.fake_count; ++i)
        s.fakes[i] = {g_fakes[i].x, g_fakes[i].y, g_fakes[i].orange ? 1u : 0u, g_fakes[i].glitched};
    s.flipped = g_flipped;
}

// The board was just loaded from a copy (its objects are new, and as they were in the copy: a split or turned-over
// bucket, walls without collisions): the effects' own state, the game's code they patch, and their objects again.
void ApplyShot(void* board, const ShotState& s)
{
    void* lm = Logic(board);
    if (!lm)
        return;
    for (void* ball : g_pending_catches)
        Release(ball);
    g_pending_catches.clear();
    g_active = s.active;
    g_board = s.active ? board : nullptr;
    memcpy(g_ticks, s.ticks, sizeof g_ticks);
    SetRandomState(s.rng);
    g_red = PegAt(lm, s.red);
    g_red_board = g_red ? board : nullptr;
    g_red_index = g_red ? s.red.index : -1;
    g_red_clock = s.red_clock;
    g_red_hit_at = s.red_hit_at;
    g_red_x = s.red_x;
    g_red_y = s.red_y;
    g_bomb = On(kBombPeg) ? PegAt(lm, s.bomb) : nullptr;
    g_bomb_index = g_bomb ? s.bomb.index : -1;
    g_bomb_blown = s.bomb_blown != 0;
    g_extra_purples.clear();
    for (int i = 0; i < s.purple_count && i < kShotMaxPurples; ++i)
        if (void* p = PegAt(lm, s.purples[i]))
            g_extra_purples.push_back(p);
    g_next_teleport = s.next_teleport;
    g_teleported_at = s.teleported_at;
    g_teleport_from = s.teleport_from;
    g_portal_count = s.portal_count < 0 ? 0 : s.portal_count > kShotMaxPortals ? kShotMaxPortals : s.portal_count;
    for (int i = 0; i < g_portal_count; ++i)
        g_portals[i] = {s.portals[i][0], s.portals[i][1]};
    for (Cooldown& c : g_cooldown)
        c = {nullptr, -1};
    g_fake_count = s.fake_count < 0 ? 0 : s.fake_count > kMaxFakes ? kMaxFakes : s.fake_count;
    for (int i = 0; i < g_fake_count; ++i) {
        void* peg = PegAt(lm, s.fake_pegs[i]);
        g_fakes[i] = {s.fakes[i].x, s.fakes[i].y, s.fakes[i].orange != 0, s.fakes[i].glitched,
                      peg && !F<uint8_t>(peg, kObjOffAlive) && Drawable(peg) ? peg : nullptr};
    }
    g_hide_guide = On(kBlindShot);
    g_speed_cap_sq = On(kPinballMadness) ? 22.0 * 22.0 : 225;
    g_speed_cap = On(kPinballMadness) ? 22.0 : 15;
    SetFlipped(s.flipped != 0);
    FindBucket(board);
    FindWalls(board);
    g_enlarged.clear();
    if (On(kBigBalls))
        for (void* b : BallsAndLoaded(board)) {
            float r = F<float>(b, kBallOffRadius);
            g_enlarged.push_back({b, r / 1.5f, r});
        }
    g_slowed.clear();
    g_drop_in = false;
    g_tinted_stale = true;  // (the bricks' pictures are new too)
}

uint32_t Trace() { return g_trace; }
uint32_t TraceSteps() { return g_trace_steps; }

void ResetTrace()
{
    g_trace = 2166136261u;
    g_trace_steps = 0;
}

void* ChooseRedPeg(void* board)  // as the game picks its green pegs: any blue one, round or a brick, still or moving
{
    std::vector<void*> blue = BluePegs(Logic(board));
    blue.erase(std::remove_if(blue.begin(), blue.end(), [](void* p) { return !Drawable(p); }), blue.end());
    SetRed(board, blue.empty() ? nullptr : blue[Random(static_cast<uint32_t>(blue.size()))]);
    return g_red;
}

void ForgetRedPeg(void* keep_board)
{
    if (g_red && g_red_board == keep_board)
        return;
    g_red = nullptr;
    g_red_board = nullptr;
    g_red_index = -1;
    g_red_clock = 0;
    g_red_hit_at = 0;
}

bool RedPegHit() { return g_red && g_red_hit_at != 0; }

bool DebugRedAt(void* board, float x, float y, int kind)
{
    void* best_peg = nullptr;
    float best = 1e9f;
    for (void* p : UnhitPegs(Logic(board), kBlue)) {
        bool moving = F<void*>(p, kObjOffMover) != nullptr;
        bool good = kind == 1 ? !IsBrick(p) && !moving : kind == 2 ? IsBrick(p) : kind == 3 ? moving : true;
        float d = RectDistSq(p, x - 75, y - 40);
        if (good && Drawable(p) && d < best) {
            best = d;
            best_peg = p;
        }
    }
    if (best_peg)  // (none of that kind: the red peg stays)
        SetRed(board, best_peg);
    return best_peg != nullptr;
}

bool DebugBombAt(float x, float y)  // testing: the unhit peg nearest a screen point becomes the bomb
{
    if (!On(kBombPeg))
        return false;
    void* lm = Logic(g_board);
    float best = 1e9f;
    for (void* p : UnhitPegs(lm)) {
        if (p == g_red)
            continue;
        float d = RectDistSq(p, x - 75, y - 40);
        if (d < best) {
            best = d;
            g_bomb = p;
        }
    }
    g_bomb_index = IndexOf(Pegs(lm), g_bomb);
    return true;
}

void DebugPlaceBall(float x, float y, float vx, float vy)
{
    g_place_at[0] = x - 75;
    g_place_at[1] = y - 40;
    g_place_at[2] = vx;
    g_place_at[3] = vy;
    g_place = true;
}

bool DebugDropIn()
{
    if (!On(kDoubleBucket))
        return false;
    g_drop_in = true;
    return true;
}

void DebugSpooky()
{
    g_spooky_next = true;
}

bool DebugPower(void* board, int power, int turns, int player)
{
    void* lm = Logic(board);
    if (!lm || power < 1 || power > 14 || player > 1)
        return false;
    int in_turn = F<int>(lm, kLogicOffPlayer);
    F<int>(lm, kLogicOffPowerTurns + 8 * power + 4 * (player < 0 ? in_turn : player)) = turns;
    if (player < 0 || player == in_turn) {
        SwitchPower(lm, power, turns != 0);  // (as each turn begins)
        PowerTurnBegins(lm, power, turns != 0);
    }
    return true;
}

std::string DebugObjects(const std::string& id)
{
    void* board = game::App() ? F<void*>(game::App(), game::kAppOffBoard) : nullptr;
    if (!board)
        return "none";
    std::string out;
    char buf[400];
    for (void* obj : List(board, kBoardOffObjects)) {
        std::string oid = Id(obj);
        if (id.empty() ? PegInfo(obj) != nullptr : oid != id)
            continue;
        void* m = Mover(obj);
        int n = snprintf(buf, sizeof buf, "%p vt=%X kind=%d id=%s alive=%d collides=%d box=%.1f,%.1f,%.1f,%.1f",
                         obj, F<uint32_t>(obj, 0), F<int>(obj, kObjOffKind), oid.c_str(),
                         F<uint8_t>(obj, kObjOffAlive), F<uint8_t>(obj, kObjOffCollides), F<float>(obj, kObjOffLeft),
                         F<float>(obj, kObjOffTop), F<float>(obj, kObjOffRight), F<float>(obj, kObjOffBottom));
        if (m)
            snprintf(buf + n, sizeof buf - n,
                     " mover=%p type=%d ticks=%d radius=%d period=%d at=%.1f,%.1f base=%.1f,%.1f",
                     m, F<int>(m, kMoverOffType), F<int>(m, kMoverOffTicks), F<int>(m, kMoverOffRadius), F<int>(m, 0x1C),
                     F<float>(m, kMoverOffX), F<float>(m, kMoverOffY), F<float>(m, kMoverOffBaseX),
                     F<float>(m, kMoverOffBaseY));
        out += (out.empty() ? "" : " | ") + std::string(buf);
    }
    return out.empty() ? "none" : out;
}

std::string EffectDebug()
{
    char buf[1600];
    void* board = g_board;
    void* gun = board ? F<void*>(board, kBoardOffGun) : nullptr;
    void* ball = gun ? F<void*>(gun, kGunOffBall) : nullptr;
    std::vector<void*> in_play = board ? Balls(board) : std::vector<void*>();
    std::vector<void*> oranges = board ? UnhitPegs(Logic(board), kOrange) : std::vector<void*>();
    void* orange = oranges.empty() ? nullptr : oranges[0];
    snprintf(buf, sizeof buf,
             "under=%u over=%u steps=%u portals=%d teleports=%u wraps=%u kicks=%u twins=%u catches=%u slowed=%u jumps=%u "
             "bomb=%d blown=%d purples=%d flipped=%d gun=%.0f,%.0f radius=%.1f hole=%.0f,%.0f parts=%d cap=%.0f "
             "walls=%d,%d,%d bombat=%.0f,%.0f blind=%u inplay=%d ball=%.0f,%.0f slowmo=%d orange=%.0f,%.0f "
             "spooky=%u spookyat=%.0f,%.0f spookyv=%.2f,%.2f spookywas=%d spookyleft=%d active=%X",
             g_n_under, g_n_over, g_n_steps, g_portal_count, g_n_portal, g_n_wrap, g_n_kicks, g_n_twins, g_n_catches,
             g_n_slow, g_n_jumps, g_bomb_index, g_bomb_blown ? 1 : 0, static_cast<int>(g_extra_purples.size()), g_flipped ? 1 : 0,
             gun ? F<float>(gun, kGunOffX) : -1.0f, gun ? F<float>(gun, kGunOffY) : -1.0f,
             ball ? F<float>(ball, kBallOffRadius) : -1.0f, g_hole ? F<float>(g_hole, kHoleOffX) : -1.0f,
             g_hole ? F<float>(g_hole, kHoleOffY) : -1.0f, static_cast<int>(g_bucket.size()), g_speed_cap,
             g_walls[0] ? F<uint8_t>(g_walls[0], kObjOffCollides) : -1,
             g_walls[1] ? F<uint8_t>(g_walls[1], kObjOffCollides) : -1,
             g_walls[2] ? F<uint8_t>(g_walls[2], kObjOffCollides) : -1,
             g_bomb ? (F<float>(g_bomb, kObjOffLeft) + F<float>(g_bomb, kObjOffRight)) / 2 + 75 : -1.0f,
             g_bomb ? (F<float>(g_bomb, kObjOffTop) + F<float>(g_bomb, kObjOffBottom)) / 2 + 40 : -1.0f, g_n_blind,
             static_cast<int>(in_play.size()), in_play.empty() ? -1.0f : F<float>(in_play[0], kBallOffX) + 75,
             in_play.empty() ? -1.0f : F<float>(in_play[0], kBallOffY) + 40,
             board ? F<uint8_t>(board, kBoardOffSlowMo) : -1,
             orange ? (F<float>(orange, kObjOffLeft) + F<float>(orange, kObjOffRight)) / 2 + 75 : -1.0f,
             orange ? (F<float>(orange, kObjOffTop) + F<float>(orange, kObjOffBottom)) / 2 + 40 : -1.0f, g_n_spooky,
             g_spooky_at[0] + 75, g_spooky_at[1] + 40, g_spooky_at[2], g_spooky_at[3], g_spooky_was, g_spooky_left,
             g_active);
    std::string out = buf;
    // Red Peg Mode: the red peg (where, its type, lit, brick, moving, how many times drawn red, "Sabotage!" shown),
    // Mirage's fakes (how many in gone pegs' places and bricks, and the easiest still there to shoot through: the
    // highest still round one), the guide stopped at a fake, Silhouette's gray drawings, pictures recolored.
    std::string fakes, brick_at = "-1,-1";
    int left = 0, top = -1, in_places = 0, bricks = 0, moving = 0;
    auto rank = [](const Fake& f) {
        return f.peg && (IsBrick(f.peg) || F<void*>(f.peg, kObjOffMover)) ? (IsBrick(f.peg) ? 0 : 1) : 2;
    };
    for (int i = 0; i < g_fake_count; ++i) {
        const Fake& f = g_fakes[i];
        in_places += f.peg ? 1 : 0;
        bricks += f.peg && IsBrick(f.peg) ? 1 : 0;
        moving += f.peg && F<void*>(f.peg, kObjOffMover) ? 1 : 0;
        if (f.glitched)
            continue;
        ++left;
        if (f.peg && IsBrick(f.peg) && (brick_at == "-1,-1" || !F<void*>(f.peg, kObjOffMover))) {  // (still first)
            char at[40];
            snprintf(at, sizeof at, "%.0f,%.0f", f.x + 75, f.y + 40);
            brick_at = at;
        }
        if (top < 0 || rank(f) > rank(g_fakes[top]) || (rank(f) == rank(g_fakes[top]) && f.y < g_fakes[top].y))
            top = i;
    }
    if (top >= 0) {
        char at[40];
        snprintf(at, sizeof at, "%.0f,%.0f", g_fakes[top].x + 75, g_fakes[top].y + 40);
        fakes = at;
    }
    std::string list;  // every one still there, still round ones first: x,y;x,y...
    for (int pass = 2; pass >= 0; --pass)
        for (int i = 0; i < g_fake_count; ++i)
            if (!g_fakes[i].glitched && rank(g_fakes[i]) == pass) {
                char at[40];
                snprintf(at, sizeof at, "%s%.0f,%.0f", list.empty() ? "" : ";", g_fakes[i].x + 75, g_fakes[i].y + 40);
                list += at;
            }
    bool red_there = g_red && F<uint8_t>(g_red, kObjOffAlive);
    void* shown = game::At<void*>(game::App(), game::kAppOffBoard);  // (the level's unhit blue bricks, moving ones)
    int blue_bricks = 0, blue_moving = 0;
    if (shown && Logic(shown))
        for (void* p : UnhitPegs(Logic(shown), kBlue)) {
            blue_bricks += Drawable(p) && IsBrick(p) ? 1 : 0;
            blue_moving += Drawable(p) && F<void*>(p, kObjOffMover) ? 1 : 0;
        }
    snprintf(buf, sizeof buf,
             " redpeg=%d redat=%.0f,%.0f redtype=%d redlit=%d redbrick=%d redmoving=%d redhitat=%u reddrawn=%u "
             "redtexts=%u fakes=%d fakesleft=%d fakesinplaces=%d fakebricks=%d fakemoving=%d fakeat=%s fakebrickat=%s "
             "fakelist=%s fakeshidden=%d glitches=%u guidefakes=%u guidefake=%d,%.0f,%.0f gray=%u tinted=%d "
             "blackout=%d black=%d blackouts=%u bluebricks=%d bluemoving=%d redpicture=%g,%g",
             g_red_index, red_there ? CenterX(g_red) + 75 : -1.0f, red_there ? CenterY(g_red) + 40 : -1.0f,
             red_there ? Type(g_red) : -1, red_there && Lit(g_red) ? 1 : 0, g_red && IsBrick(g_red) ? 1 : 0,
             g_red && F<void*>(g_red, kObjOffMover) ? 1 : 0, g_red_hit_at, g_n_red, g_n_red_texts, g_fake_count, left,
             in_places, bricks, moving, fakes.empty() ? "-1,-1" : fakes.c_str(), brick_at.c_str(),
             list.empty() ? "-" : list.c_str(), FakesHidden() ? 1 : 0, g_n_glitches, g_n_guide_fakes,
             g_guide_fake_step, g_guide_fake_x + 75,
             g_guide_fake_y + 40, g_n_gray, static_cast<int>(g_tinted.size()), g_blackout, BlackoutAlpha(),
             g_n_blackouts, blue_bricks, blue_moving,  // (a red brick's picture: where it is in the brick)
             g_red && IsBrick(g_red) ? F<float>(g_red, kBrickOffPictureX) : 0.0f,
             g_red && IsBrick(g_red) ? F<float>(g_red, kBrickOffPictureY) : 0.0f);
    out += buf;
    // Masters' powers at the bucket: Claude's flippers (pivots, how far up and down they reach, alive, the way they
    // turn), the bucket's track width and the goal it's easing to, Chain Lightning's rod (state, where) and its arcs
    // (where the last ended, to the copy or not, from which peg, the rod then), the lid, bounces off the copy's parts,
    // fireballs knocking the lid off there, catches upside down, the loaded ball (fireball, charged).
    void* lm = shown ? Logic(shown) : nullptr;
    float pivots[4] = {-1, -1, -1, -1}, reach[2] = {1e9f, -1e9f};
    int alive = 0, turns[2] = {0, 0}, n = 0;
    if (shown)
        for (void* obj : List(shown, kBoardOffObjects))
            if (IsFlipper(obj) && n < 2) {
                pivots[2 * n] = F<float>(obj, kPolyOffX) + 75;
                pivots[2 * n + 1] = F<float>(obj, kPolyOffY) + 40;
                reach[0] = std::min(reach[0], F<float>(obj, kObjOffTop) + 40);
                reach[1] = std::max(reach[1], F<float>(obj, kObjOffBottom) + 40);
                alive += F<uint8_t>(obj, kObjOffAlive);
                turns[n++] = F<int>(Mover(obj), kMoverOffType);
            }
    void* rod = Rod(lm);
    void* shown_gun = shown ? F<void*>(shown, kBoardOffGun) : nullptr;
    void* loaded = shown_gun ? F<void*>(shown_gun, kGunOffBall) : nullptr;
    snprintf(buf, sizeof buf,
             " flippers=%.0f,%.0f,%.0f,%.0f flipreach=%.0f,%.0f flipalive=%d flipturn=%d,%d goal=%d track=%d "
             "rod=%d,%d,%.0f zaps=%u zapat=%.0f,%.0f zaptwin=%d zappeg=%.0f,%.0f zaprod=%.0f lid=%d twinbounces=%u "
             "lidknocks=%u upcatches=%u fireball=%d charged=%d",
             pivots[0], pivots[1], pivots[2], pivots[3], reach[0], reach[1], alive, turns[0], turns[1],
             lm ? F<int>(lm, kLogicOffTrackGoal) : -1, g_hole ? F<int>(Mover(g_hole), kMoverOffRadius) : -1,
             rod ? F<int>(rod, kRodOffState) : -1, rod ? F<int>(rod, kRodOffX) + 75 : -1,
             rod ? F<float>(rod, kRodOffY) + 40 : -1.0f, g_n_zaps, g_zap_x + 75, g_zap_y + 40, g_zap_twin ? 1 : 0,
             g_zap_peg_x + 75, g_zap_peg_y + 40, g_zap_rod_x + 75, LidOn() ? 1 : 0, g_n_twin_bounces,
             g_n_lid_knocks, g_n_bucket_catches, loaded ? F<uint8_t>(loaded, kBallOffFireball) : -1,
             loaded ? F<uint8_t>(loaded, kBallOffCharged) : -1);
    return out + buf;
}

uint32_t EffectHash()
{
    uint32_t h = g_blackout ? static_cast<uint32_t>(g_blackout) * 1009 + g_blackout_steps : 0;
    if (g_red)  // (nothing left over from an earlier duel may count: the other computer never had it)
        h = h * 131 + static_cast<uint32_t>(g_red_index) * 17 + g_red_clock * 3 + g_red_hit_at;
    if (!g_active)
        return h;
    h = h * 131 + g_active * 31 + static_cast<uint32_t>(g_bomb_index) * 7 + g_bomb_blown;
    for (int i = 0; i < kEffectCount; ++i)
        if (On(static_cast<Effect>(i)))
            h = h * 7 + g_ticks[i];
    h = h * 131 + static_cast<uint32_t>(g_extra_purples.size());
    for (int i = 0; i < g_portal_count; ++i)
        h = h * 131 + Bits(g_portals[i].x) * 3 + Bits(g_portals[i].y);
    h = h * 131 + g_next_teleport + (g_flipped ? 1 : 0) + static_cast<uint32_t>(g_slowed.size()) * 5;
    h = h * 131 + static_cast<uint32_t>(g_enlarged.size());
    for (int i = 0; i < g_fake_count; ++i)
        h = h * 131 + Bits(g_fakes[i].x) * 3 + Bits(g_fakes[i].y) + g_fakes[i].glitched * 5 + g_fakes[i].orange +
            (g_fakes[i].peg ? 7 : 0);
    return h;
}

}  // namespace chaos
