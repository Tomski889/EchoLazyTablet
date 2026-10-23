// EchoQueue.dll -- EchoLoader plugin: queue for an Echo Arena match from the hand tablet.
//
//   * PLAY tab on the hand tablet (data built by tools/build_queue_tab.py): its START ARENA
//     button asks for a public Echo Arena match, and the page shows the matchmaking state;
//   * LOBBY on that page: leaves the match you're in for the social lobby;
//   * PRIVATE MATCH: creates a private Echo Arena match on the server set in EchoQueue.ini;
//   * F9 (Echo's window focused) does the same as START;
//   * logs every matchmaking request the game makes, and saves the settings of a public
//     Echo Arena request to EchoQueue.ini so ours match the game's exactly.
//
// The tablet runtime (canvas discovery, page switching, native button gating and event
// dispatch) follows Echo Arcade (kotorvr/echo-arcade), which follows heisthecat31/Doom-on-EchoVR.
//
// Addresses are for echovr.exe timestamp 1683152886 and are verified at startup.
//   0x16afc0  FindMatch(netgame, FindParams*)  the FindMatch node's action; copies the
//             params into the matchmaker and calls 0x168500 ("Requesting search for a %s
//             session of gametype %s, level %s, group %s")
//   0x1a7dc0  JoinFoundMatch(netgame): "Scheduling leave for match", what R15NetJoinFoundMatchNode
//             does. The menu runs it 7 s after "lobby join queued" (the LOCKOUT countdown);
//             a search the menu didn't start never gets it, so we call it for ours.
//   0x15b920  CancelMatch(netgame): what R15NetCancelMatchNode calls (behind the same
//             net-game-ready check as FindMatch); stops a search or a found-but-not-joined match
//   0x1a89f0  ScheduleReturnToLobby(netgame): "Scheduling return to lobby", JoinFoundMatch's
//             twin; what the game's own post-match and party-follow code calls
//   0x20a0478 the game object (set in its init); +0x8518 its net game (getter 0x113a90)
//   netgame+0  NetGame state (names at 0x16d370): 5 lobby, 7 loading level, 9 in game
//   netgame+0x2b20+0x1f8  > 0 when the net game takes requests (the node checks it too)
//   Match end: the game's own HTTP API (127.0.0.1:6721/session, "API access" in Echo's
//             settings) reports game_status "post_match" (state machine symbols
//             post_match_celebration_* / waiting_to_return_to_lobby, mapped at 0x16d510).
//   netgame+0x40  the client lobby; +0x448 its state (setter 0x1b7bb0, "NetClientLobby
//             switching state"): 3..6 starting/creating/finding/joining a match, 7 lobby join
//             queued, 8 joining it. A new request in those states restarts the search.
//   Tablet: 0x71fc90 canvas loaded, 0x7287b0 canvas unload, 0x71c820 element show,
//   0x727f10 element text, 0x726f00 canvas alpha, 0x92f3f0 button update (game thread),
//   0x92b9e0 / 0x92bd10 button disable / enable, 0x510060 event dispatch.
#include "../vendor/minhook/include/MinHook.h"
#include "generated/queue_tab.h"
#include <windows.h>
#include <winhttp.h>
// D3D12 methods returning structs (GetCPUDescriptorHandleForHeapStart) use a hidden return
// pointer in Microsoft's ABI; this makes mingw's d3d12.h declare them that way.
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include <d3d12.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using U = uint64_t;
using P = unsigned char*;
template <class T> static T& at(void* p, size_t n) { return *reinterpret_cast<T*>(static_cast<P>(p) + n); }
template <class T> static T fn(void* p, size_t n) { return reinterpret_cast<T>(static_cast<P>(p) + n); }

namespace {

constexpr unsigned EXE_TIMESTAMP = 1683152886, EXE_IMAGE_SIZE = 35852288;
constexpr unsigned RVA_FIND_MATCH = 0x16afc0, RVA_NETGAME_GETTER = 0x113a90;
constexpr unsigned RVA_JOIN_FOUND = 0x1a7dc0, RVA_CANCEL = 0x15b920, RVA_RETURN_TO_LOBBY = 0x1a89f0;
constexpr unsigned RVA_CREATE_MATCH = 0x15f240;
// Fade: Echo's post-process (screentint tint/gain at +0x16c/+0x20c of the renderer's
// settings, *(exe+0x20a0488)) does nothing in VR: gain written to 0.05 in a running game left
// the picture unchanged (v2.3/v2.4 tried it). Echo's own loading fade is drawn inside its one
// eye layer (it submits a single type-2 layer throughout a load, v2.6 log). So the fade is a
// layer of ours: a head-locked black quad added to what Echo hands the Oculus runtime
// (ovr_EndFrame in LibOVRRT64_1.dll), its alpha raised 0..1. If the quad can't be made, the
// frames go out with no layers instead (a cut to black, which worked in v2.6).
constexpr U FADE_DELAY_MS = 1000;     // after the button, before the fade ([fade] delay_ms)
constexpr U FADE_OUT_MS = 1000;       // to black, like Echo's loading fade
constexpr U FADE_IN_MS = 1000;        // back from black
constexpr U BLACK_HOLD_MS = 300;      // black this long before Echo is asked to move
constexpr U FADE_GIVE_UP_MS = 4000;   // nothing joining this long after: picture back
constexpr U FADE_MAX_MS = 20000;      // black at most this long, whatever happens
// CreateOnServer(lobby, ipv4, port), both in network byte order: "Requesting creation of %s
// session ... on server %s:%hu"; what the -serveraddr launch option uses. It creates from the
// request in the lobby (lobby+0x698: the FindParams fields, slot count at +0x6d8), which
// CreateMatch (0x15f070) copies there first. netgame+0x647c8 is the matchmaking object whose
// vtable+0xc0 says whether requests may start (FindMatch and CreateMatch check it too).
constexpr unsigned RVA_CREATE_ON_SERVER = 0x15e5a0, NETGAME_MATCHMAKER = 0x647c8;
constexpr unsigned LOBBY_REQUEST = 0x698, LOBBY_SLOT_COUNT = 0x6d8;
// Client lobby states past matchmaking (names at 0x1b6d30): 9 joined, 10 starting, 11 in
// game; 12 and up completed, terminated, or failed (creation failed, join errors...).
constexpr int LOBBY_JOINED = 9, LOBBY_IN_GAME = 11;
constexpr int LOBBY_JOIN_QUEUED = 7;
constexpr U JOIN_DELAY_MS = 7000;   // the menu's LOCKOUT countdown
constexpr unsigned RVA_GAME = 0x20a0478, GAME_NETGAME = 0x8518, NETGAME_READY = 0x2b20 + 0x1f8;
constexpr unsigned NETGAME_STATE = 0, NETGAME_LOBBY = 0x40, LOBBY_STATE = 0x448;
constexpr int NETGAME_IN_GAME = 9, NETGAME_LOADING = 7, NETGAME_LOBBY_SEARCH = 5;
constexpr INTERNET_PORT API_PORT = 6721;
constexpr unsigned RVA_LOADED = 0x71fc90, RVA_UNLOAD = 0x7287b0, RVA_SHOW = 0x71c820, RVA_TEXT = 0x727f10,
                   RVA_ALPHA = 0x726f00, RVA_BUTTON_UPDATE = 0x92f3f0, RVA_BUTTON_DISABLE = 0x92b9e0,
                   RVA_BUTTON_ENABLE = 0x92bd10, RVA_DISPATCH = 0x510060;
constexpr U ECHO_ARENA = 0xcb60a4de7e1caf73, NONE = ~0ull;
constexpr U ECHO_ARENA_PRIVATE = 0x09990965f4db8c03, MPL_ARENA_A = 0x576ed3f8428ebc4b;  // echo_arena_private, its arena
// Sounds: Wwise is linked into echovr.exe, which exports AK::SoundEngine::PostEvent(eventID,
// gameObject, ...) at 0x138f240. Stock tablet buttons get their sounds from the tablet's
// scripts, ours have none, so EchoQueue posts the same Wwise events on the tablet's sound
// object. Event IDs were read off the game's own presses (tab click, mute, unmute); the
// tablet's object is taken from the game's own use of those events (0x3eb until then).
constexpr unsigned RVA_POST_EVENT = 0x138f240;
constexpr uint32_t SOUND_CLICK = 0xc1b65330, SOUND_TOGGLE_ON = 0x6ffccf55, SOUND_TOGGLE_OFF = 0xa7fa6f01;
constexpr U TABLET_SOUND_OBJECT = 0x3eb;
// The announcer's matchmaking lines, read off a terminal search and cancel: "Finding Match"
// (which the game plays for every search) and "Matchmaking Cancelled" (which only the
// terminal's script plays), both on the announcer's object.
constexpr uint32_t VOICE_FINDING_MATCH = 0x09f71117, VOICE_MATCHMAKING_CANCELLED = 0x899e8edf;
constexpr U ANNOUNCER_SOUND_OBJECT = 0x166;

// Tablet symbols (from Echo Arcade): press/release events, the tablet actor, the stock tabs and
// the stock page-content buttons that must not react while the PLAY page covers them.
constexpr U PRESS = 0xfdb213d9dc5e4826, RELEASE = 0xd7ded077f321f69b;
constexpr U TABLET_ACTOR = 0x6c1f6ff04e070923;
constexpr U STOCK_TAB_FIRST = 0x275876572b742791, STOCK_TAB_LAST = 0x275876572b742794;
constexpr U CONTENT[] = {0x5071f3ac46630320, 0x645c2234c519a275, 0x645c2234c519a276, 0x645c2234c519a277,
    0x6567539d25efbdc0, 0xfc4ee470b38fb49d, 0xfc4ee470b38fb49e, 0xfc4ee470b38fb49f,
    0x2755735736742790, 0x2755735736742791, 0x2755735736742792, 0x2755735736742793,
    0x2755735736742794, 0x2755735736742795, 0x2755735736742796, 0x2755735736742797,
    0x556819fa696d48c4};

struct Signature { unsigned rva; unsigned char bytes[16]; };
constexpr Signature SIGNATURES[] = {
    {RVA_FIND_MATCH, {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x40,0x48,0x8b,0xf9,0x48,0x8b,0xda}},
    {RVA_NETGAME_GETTER, {0x48,0x8b,0x81,0x18,0x85,0x00,0x00,0xc3,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc}},
    {RVA_JOIN_FOUND, {0x40,0x53,0x48,0x83,0xec,0x30,0x33,0xd2,0x4c,0x8d,0x05,0x11,0x89,0x53,0x01,0x48}},
    {RVA_RETURN_TO_LOBBY, {0x40,0x53,0x48,0x83,0xec,0x30,0x33,0xd2,0x4c,0x8d,0x05,0x99,0x7c,0x53,0x01,0x48}},
    {RVA_CREATE_MATCH, {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x8b,0xfa}},
    {RVA_CREATE_ON_SERVER, {0x48,0x89,0x5c,0x24,0x18,0x89,0x54,0x24,0x10,0x55,0x56,0x57,0x41,0x54,0x41,0x55}},
    {RVA_CANCEL, {0x48,0x8b,0x49,0x40,0xba,0x0b,0x00,0x00,0x00,0x8b,0x81,0x48,0x04,0x00,0x00,0x83}},
    {RVA_POST_EVENT, {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x4c}},
    {RVA_LOADED, {0x48,0x89,0x5c,0x24,0x08,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57}},
    {RVA_UNLOAD, {0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xec}},
    {RVA_SHOW, {0x89,0x54,0x24,0x10,0x57,0x41,0x57,0x48,0x83,0xec,0x38,0x48,0x8b,0x81,0xc0,0x00}},
    {RVA_TEXT, {0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x89,0x54,0x24,0x10,0x57,0x48}},
    {RVA_ALPHA, {0xf3,0x0f,0x10,0x81,0x58,0x03,0x00,0x00,0xb8,0x01,0x00,0x00,0x00,0xf3,0x0f,0x5c}},
    {RVA_BUTTON_UPDATE, {0x40,0x55,0x57,0x41,0x54,0x48,0x8d,0xac,0x24,0x40,0xfe,0xff,0xff,0x48,0x81,0xec}},
    {RVA_BUTTON_DISABLE, {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48}},
    {RVA_BUTTON_ENABLE, {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57}},
    {RVA_DISPATCH, {0x48,0x83,0xec,0x58,0x48,0x8b,0x41,0x58,0x48,0x89,0x54,0x24,0x20,0x48,0x8d,0x54}},
};

// The FindMatch node's settings block (0x48 bytes), as FindMatch reads it.
#pragma pack(push, 1)
struct FindParams {
    uint8_t access;       // 0 public, 1 private, 2 unassigned
    uint16_t field1;
    uint8_t field3;
    uint32_t field4;      // the node starts it at 3
    U gametype;           // symbol, e.g. echo_arena
    U level;              // symbol, -1 = any
    U group;              // symbol, -1 = none
    uint16_t slots[16];
    uint32_t count;
    uint32_t pad;
};
#pragma pack(pop)
static_assert(sizeof(FindParams) == 0x48, "FindParams layout");

using FindMatch = void (*)(void*, FindParams*);
using JoinFound = void (*)(void*);
using ReturnToLobby = void (*)(void*);
using CreateMatch = void (*)(void*, FindParams*);
using CreateOnServer = void (*)(void*, uint32_t, uint16_t);
using MayRequest = int (*)(void*);
// ovr_SubmitFrame2(session, frameIndex, viewScaleDesc, layerPtrList, layerCount)
using SubmitFrame2 = int (*)(void*, long long, const void*, void* const*, unsigned);
using CancelMatch = void (*)(void*);
using Show = U (*)(void*, unsigned, int);
using Text = U (*)(void*, unsigned, const char*);
using Alpha = U (*)(void*, float);
using Mask = void (*)(void*, U, unsigned short);
using Dispatch = void (*)(void*, U, U, U, int);
using Loaded = U (*)(void*, U, U, U);
using Unload = void (*)(void*, U, U, U);
using ButtonUpdate = void (*)(void*);
using PostEvent = uint32_t (*)(uint32_t, U, uint32_t, void*, void*, uint32_t, void*, uint32_t);

P exe = nullptr;
FindMatch findOriginal = nullptr;
PostEvent postEventOriginal = nullptr;
Show showFn; Text textFn; Alpha alphaFn; Mask enableFn, disableFn;

// Redraw counters for the perf line in the log (each of these makes the tablet redraw).
struct Perf { std::atomic<U> show{0}, text{0}, alpha{0}, mask{0}, ticks{0}, ns{0}; } perf;
U show(void* c, unsigned e, int v) { perf.show++; return showFn(c, e, v); }
U text(void* c, unsigned e, const char* s) { perf.text++; return textFn(c, e, s); }
U alpha(void* c, float a) { perf.alpha++; return alphaFn(c, a); }
void enableButton(void* cs, U h, unsigned short r) { perf.mask++; enableFn(cs, h, r); }
void disableButton(void* cs, U h, unsigned short r) { perf.mask++; disableFn(cs, h, r); }

std::atomic<U> tabletObject{TABLET_SOUND_OBJECT}, announcerObject{ANNOUNCER_SOUND_OBJECT};
Dispatch dispatchOriginal; Loaded loadedOriginal; Unload unloadOriginal; ButtonUpdate buttonOriginal;
std::wstring dir;
std::mutex logMutex;
std::atomic<bool> fault{false};       // matchmaking off
std::atomic<bool> tabletFault{false}; // tablet off (the stock tablet is left alone)

FindParams captured{};
bool haveCaptured = false;
int hotkey = VK_F9;
std::atomic<DWORD> gameThread{0};   // the thread the game asked for a match on
HWND window = nullptr;
WNDPROC windowOriginal = nullptr;

// In a match (not the social lobby), as Echo's API reports it (matchWatch): what the LOBBY
// button leaves.
std::atomic<bool> inMatch{false};

// What the stats box shows; tapping it steps to the next ([stats] show in EchoQueue.ini).
enum { STAT_TODAY, STAT_OVERALL, STAT_PLAYERS, STAT_MODES };
constexpr const char* STAT_CAPTIONS[STAT_MODES] = {"HOURS TODAY", "MATCHES OVERALL", "PLAYERS ONLINE"};
std::atomic<int> statMode{STAT_TODAY};
std::atomic<U> fadeDelayMs{FADE_DELAY_MS};  // button to black ([fade] delay_ms)

// Our searches must also be joined (the menu's JoinFoundMatch step); see joinTick.
std::atomic<bool> ourSearch{false};
std::atomic<bool> ourPrivate{false};  // ...and it's a private match we're creating
std::atomic<U> searchSince{0};      // when the current search started (for the page's timer)
std::atomic<U> joinQueuedSince{0};
std::atomic<U> secondsToday{0};      // time played today (see updateToday), for the page
std::atomic<U> lastWaitMs{0};
bool joinSent = false;

void logf(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    std::lock_guard<std::mutex> lock(logMutex);
    std::wstring path = dir + L"\\EchoQueue.log";
    if (FILE* f = _wfopen(path.c_str(), L"a")) { fprintf(f, "%llu %s\n", GetTickCount64(), line); fclose(f); }
}

bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION m{};
    if (!p || !VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT) return false;
    if (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return static_cast<const P>(const_cast<void*>(p)) + n <= static_cast<P>(m.BaseAddress) + m.RegionSize;
}

std::wstring iniPath() { return dir + L"\\EchoQueue.ini"; }

U readHex(const wchar_t* section, const wchar_t* key, U fallback) {
    wchar_t buffer[64];
    GetPrivateProfileStringW(section, key, L"", buffer, 64, iniPath().c_str());
    return buffer[0] ? wcstoull(buffer, nullptr, 16) : fallback;
}

void writeHex(const wchar_t* key, U value) {
    wchar_t buffer[32];
    swprintf(buffer, 32, L"%llx", value);
    WritePrivateProfileStringW(L"captured", key, buffer, iniPath().c_str());
}

int parseKey(const wchar_t* name) {
    if ((name[0] == L'F' || name[0] == L'f') && name[1]) {
        int n = _wtoi(name + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    return VK_F9;
}

// PRIVATE MATCH goes to one game server: [private] server / ports in EchoQueue.ini, by default
// Maidenhead, England (80.64.218.95, which runs five match slots on ports 6792..6796). The
// slot that last worked is tried first ([private] last_port).
std::string privateServer;
uint32_t privateIp = 0;                // network byte order, 0 = let the server choose
std::vector<uint16_t> privatePorts;

void loadPrivateServer() {
    wchar_t buffer[128];
    GetPrivateProfileStringW(L"private", L"server", L"80.64.218.95", buffer, 128, iniPath().c_str());
    unsigned a, b, c, d;
    privateIp = swscanf(buffer, L"%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a < 256 && b < 256 && c < 256 && d < 256
                    ? a | b << 8 | c << 16 | d << 24 : 0;
    char narrow[128];
    snprintf(narrow, sizeof narrow, "%ls", buffer);
    privateServer = narrow;
    GetPrivateProfileStringW(L"private", L"ports", L"6792,6793,6794,6795,6796", buffer, 128, iniPath().c_str());
    privatePorts.clear();
    for (wchar_t* p = buffer; *p;) {
        unsigned port = wcstoul(p, &p, 10);
        if (port > 0 && port < 65536) privatePorts.push_back(uint16_t(port));
        while (*p && (*p < L'0' || *p > L'9')) p++;
    }
    unsigned last = GetPrivateProfileIntW(L"private", L"last_port", 0, iniPath().c_str());
    auto it = std::find(privatePorts.begin(), privatePorts.end(), uint16_t(last));
    if (it != privatePorts.end()) std::rotate(privatePorts.begin(), it, it + 1);
    if (!privateIp || privatePorts.empty()) { privateIp = 0; logf("settings: private matches go wherever the server puts them"); }
    else logf("settings: private matches on %s (%zu port(s), %u first)", privateServer.c_str(), privatePorts.size(), unsigned(privatePorts[0]));
}

void loadSettings() {
    wchar_t key[16];
    GetPrivateProfileStringW(L"queue", L"hotkey", L"F9", key, 16, iniPath().c_str());
    hotkey = parseKey(key);
    loadPrivateServer();
    statMode = std::clamp(int(GetPrivateProfileIntW(L"stats", L"show", STAT_TODAY, iniPath().c_str())), 0, STAT_MODES - 1);
    fadeDelayMs = std::min<U>(GetPrivateProfileIntW(L"fade", L"delay_ms", FADE_DELAY_MS, iniPath().c_str()), 10000);
    if (readHex(L"captured", L"gametype", 0) == ECHO_ARENA) {
        captured.access = uint8_t(readHex(L"captured", L"access", 0));
        captured.field1 = uint16_t(readHex(L"captured", L"field1", 0));
        captured.field3 = uint8_t(readHex(L"captured", L"field3", 0));
        captured.field4 = uint32_t(readHex(L"captured", L"field4", 3));
        captured.gametype = ECHO_ARENA;
        captured.level = readHex(L"captured", L"level", NONE);
        captured.group = readHex(L"captured", L"group", NONE);
        haveCaptured = true;
        logf("settings: hotkey F%d, using the Echo Arena request saved in EchoQueue.ini", hotkey - VK_F1 + 1);
    } else {
        logf("settings: hotkey F%d, no saved Echo Arena request yet (built-in defaults until you queue once the normal way)", hotkey - VK_F1 + 1);
    }
}

FindParams arenaRequest() {
    if (haveCaptured) { FindParams p = captured; memset(p.slots, 0, sizeof p.slots); p.count = 0; return p; }
    FindParams p{};  // what the FindMatch node starts with, plus public Echo Arena
    p.access = 0; p.field4 = 3; p.gametype = ECHO_ARENA; p.level = NONE; p.group = NONE;
    return p;
}

const char* accessName(uint8_t a) { return a == 0 ? "public" : a == 1 ? "private" : a == 2 ? "unassigned" : "invalid"; }

void describe(const char* who, void* netgame, const FindParams* p) {
    logf("%s: netgame %p, %s, gametype %016llx%s, level %016llx, group %016llx, field1 %u field3 %u field4 %u, %u slot(s)",
         who, netgame, accessName(p->access), p->gametype, p->gametype == ECHO_ARENA ? " (echo_arena)" : "",
         p->level, p->group, p->field1, p->field3, p->field4, p->count);
}

// ---- matchmaking ----
void findDetour(void* netgame, FindParams* p) {
    if (!fault && readable(p, sizeof *p)) {
        DWORD thread = GetCurrentThreadId();
        if (gameThread.exchange(thread) != thread)
            logf("the game asks for matches on thread %lu; Echo's window runs on thread %lu", thread,
                 window ? GetWindowThreadProcessId(window, nullptr) : 0);
        describe("game asked for a match", netgame, p);
        ourSearch = false;  // the menu's own search: the menu joins it
        if (p->gametype == ECHO_ARENA && p->access == 0) {
            captured = *p;
            haveCaptured = true;
            writeHex(L"access", p->access); writeHex(L"field1", p->field1); writeHex(L"field3", p->field3);
            writeHex(L"field4", p->field4); writeHex(L"gametype", p->gametype); writeHex(L"level", p->level);
            writeHex(L"group", p->group);
            logf("saved this public Echo Arena request to EchoQueue.ini");
        }
    }
    findOriginal(netgame, p);
}

// Every sound the game plays passes here (any thread): when it plays one of the tablet's
// button sounds, that call names the tablet's sound object.
// The announcer's "Joining Match" (what the menu's script plays when its search finds a
// match; ours get none). Its event ID isn't known in advance: the first time the menu finds a
// match, the announcer event it plays then is learnt and saved ([sounds] joining_match).
std::atomic<uint32_t> voiceJoiningMatch{0};
std::atomic<uint32_t> lastAnnouncerEvent{0};  // the latest other announcer event, and when
std::atomic<U> lastAnnouncerEventAt{0};
std::atomic<U> learnJoiningUntil{0};          // a menu search just found a match: learn until then

void learnJoiningMatch(uint32_t id) {
    voiceJoiningMatch = id;
    learnJoiningUntil = 0;
    wchar_t hex[16];
    swprintf(hex, 16, L"%x", id);
    WritePrivateProfileStringW(L"sounds", L"joining_match", hex, iniPath().c_str());
    logf("sound: learnt the announcer's \"Joining Match\" (%08x)", id);
}

uint32_t postEventDetour(uint32_t id, U object, uint32_t flags, void* callback, void* cookie, uint32_t externals,
                         void* external, uint32_t playing) {
    if ((id == SOUND_CLICK || id == SOUND_TOGGLE_ON || id == SOUND_TOGGLE_OFF) && tabletObject.exchange(object) != object)
        logf("sound: the tablet's sound object is %llx", object);
    if ((id == VOICE_FINDING_MATCH || id == VOICE_MATCHMAKING_CANCELLED) && announcerObject.exchange(object) != object)
        logf("sound: the announcer's sound object is %llx", object);
    else if (object == announcerObject && id != VOICE_FINDING_MATCH && id != VOICE_MATCHMAKING_CANCELLED && !voiceJoiningMatch) {
        if (GetTickCount64() < learnJoiningUntil) learnJoiningMatch(id);
        else { lastAnnouncerEvent = id; lastAnnouncerEventAt = GetTickCount64(); }
    }
    return postEventOriginal(id, object, flags, callback, cookie, externals, external, playing);
}

// One of the tablet's own button sounds, for one of our buttons.
void playSound(uint32_t id) {
    postEventOriginal(id, tabletObject, 0, nullptr, nullptr, 0, nullptr, 0);
}

// readable() asks Windows (VirtualQuery), which takes milliseconds in Echo's address space:
// far too slow for every frame. The game, net game and lobby objects live for the whole
// session, so each pointer is checked once when it first appears (per thread) and then read.
bool checked(void* p, size_t offset, size_t n, void*& last, bool& ok) {
    if (p != last) { last = p; ok = readable(static_cast<P>(p) + offset, n); }
    return ok;
}

void* netGame() {
    thread_local void *lastGame = nullptr, *lastNet = nullptr;
    thread_local bool gameOk = false, netOk = false;
    void* game = *reinterpret_cast<void**>(exe + RVA_GAME);
    if (!game || !checked(game, GAME_NETGAME, 8, lastGame, gameOk)) return nullptr;
    void* netgame = at<void*>(game, GAME_NETGAME);
    return netgame && checked(netgame, 0, NETGAME_READY + 4, lastNet, netOk) ? netgame : nullptr;
}

// Lobby state, or -1 when there is no net game / client lobby yet.
int lobbyState() {
    thread_local void* lastLobby = nullptr;
    thread_local bool lobbyOk = false;
    void* netgame = netGame();
    if (!netgame) return -1;
    void* lobby = at<void*>(netgame, NETGAME_LOBBY);
    return lobby && checked(lobby, LOBBY_STATE, 4, lastLobby, lobbyOk) ? at<int>(lobby, LOBBY_STATE) : -1;
}

bool matchmaking(int state) { return state >= 3 && state <= 8; }

enum class Queued { Started, AlreadySearching, NotReady, WrongThread, None };

// A new private Echo Arena match, as the CreateMatch node asks for one: the server puts it
// on a game server of its choosing (it has the client's pings from every search).
FindParams privateRequest() {
    FindParams p{};
    p.access = 1; p.field4 = 3; p.gametype = ECHO_ARENA_PRIVATE; p.level = MPL_ARENA_A; p.group = NONE;
    return p;
}

// The private match slot being tried (index into privatePorts), -1 none, -2 waiting for
// Echo's window thread to try the next one. joinTick watches how it goes.
std::atomic<int> privateTry{-1};
std::atomic<bool> privateSeenMatchmaking{false};

// Echo's window thread only: create the private match on privatePorts[index] of the
// configured server, as CreateMatch would but on that server.
bool createOnServer(void* netgame, int index) {
    void* matchmaker = at<void*>(netgame, NETGAME_MATCHMAKER);
    if (matchmaker && !reinterpret_cast<MayRequest>((*reinterpret_cast<void***>(matchmaker))[0xc0 / 8])(matchmaker)) {
        logf("private: the game isn't taking match requests right now");
        return false;
    }
    void* lobby = at<void*>(netgame, NETGAME_LOBBY);
    FindParams p = privateRequest();
    at<uint8_t>(lobby, LOBBY_REQUEST) = p.access;
    at<uint16_t>(lobby, LOBBY_REQUEST + 1) = p.field1;
    at<uint8_t>(lobby, LOBBY_REQUEST + 3) = p.field3;
    at<uint32_t>(lobby, LOBBY_REQUEST + 4) = p.field4;
    at<U>(lobby, LOBBY_REQUEST + 8) = p.gametype;
    at<U>(lobby, LOBBY_REQUEST + 0x10) = p.level;
    at<U>(lobby, LOBBY_REQUEST + 0x18) = p.group;
    at<uint32_t>(lobby, LOBBY_SLOT_COUNT) = 0;
    uint16_t port = privatePorts[index];
    logf("private: creating a private match on %s:%u (slot %d of %zu)", privateServer.c_str(), unsigned(port), index + 1, privatePorts.size());
    privateSeenMatchmaking = false;
    privateTry = index;
    fn<CreateOnServer>(exe, RVA_CREATE_ON_SERVER)(lobby, privateIp, uint16_t(port << 8 | port >> 8));
    return true;
}

// Only on Echo's window thread (the hotkey, or WM_QUEUE / WM_PRIVATE below): the game makes
// its own requests there, and the net game only takes them there. Tablet presses and the
// tablet update arrive on worker threads, so they post a message instead of calling this.
// `createPrivate`: create a private match instead of searching for a public one.
Queued queueNow(bool createPrivate = false) {
    DWORD thread = GetCurrentThreadId();
    if (gameThread != 0 && gameThread != thread) { logf("queue: thread %lu isn't the game's thread %lu; not queueing (send this log)", thread, DWORD(gameThread)); return Queued::WrongThread; }
    void* netgame = netGame();
    if (!netgame) { logf("queue: the game isn't ready (no net game yet)"); return Queued::NotReady; }
    if (at<uint32_t>(netgame, NETGAME_READY) == 0) { logf("queue: the net game isn't taking requests right now"); return Queued::NotReady; }
    int state = lobbyState();
    if (state < 0) { logf("queue: no client lobby yet"); return Queued::NotReady; }
    if (matchmaking(state)) { logf("queue: already matchmaking (lobby state %d); not starting over", state); return Queued::AlreadySearching; }
    logf("queue: lobby state %d, thread %lu", state, thread);
    if (createPrivate) {
        privateTry = -1;
        if (!privateIp) {
            FindParams p = privateRequest();
            describe("queue: creating a private match", netgame, &p);
            fn<CreateMatch>(exe, RVA_CREATE_MATCH)(netgame, &p);
        } else if (!createOnServer(netgame, 0)) {
            return Queued::NotReady;
        }
    } else {
        FindParams p = arenaRequest();
        describe("queue: asking for a match", netgame, &p);
        findOriginal(netgame, &p);
    }
    ourSearch = true;
    ourPrivate = createPrivate;
    return Queued::Started;
}

constexpr UINT WM_QUEUE = WM_APP + 0x51, WM_JOIN = WM_APP + 0x52, WM_CANCEL = WM_APP + 0x53, WM_RETURN = WM_APP + 0x54,
               WM_PRIVATE = WM_APP + 0x55, WM_PRIVATE_NEXT = WM_APP + 0x56;  // wparam: the slot to try

// Echo's window thread only, like queueNow.
// `announce`: play the announcer's "Matchmaking Cancelled" for a search EchoQueue started (a
// terminal's search gets it from the terminal's own script).
bool cancelNow(const char* why, bool announce = false) {
    void* netgame = netGame();
    int state = lobbyState();
    if (!netgame || !matchmaking(state)) { logf("cancel (%s): not matchmaking (lobby state %d)", why, state); return false; }
    if (at<uint32_t>(netgame, NETGAME_READY) == 0) { logf("cancel (%s): the net game isn't taking requests right now", why); return false; }
    bool ours = ourSearch;
    logf("cancel (%s): cancelling (lobby state %d, %s search)", why, state, ours ? "our" : "the menu's");
    fn<CancelMatch>(exe, RVA_CANCEL)(netgame);
    ourSearch = ourPrivate = false;
    privateTry = -1;
    if (announce && ours) postEventOriginal(VOICE_MATCHMAKING_CANCELLED, announcerObject, 0, nullptr, nullptr, 0, nullptr, 0);
    return true;
}

std::atomic<int> lastResult{int(Queued::None)};  // result of the last WM_QUEUE, for the page
std::atomic<U> lastResultAt{0};

// From any thread: ask Echo's window thread to queue, or to create a private match
// (handled in windowDetour).
void requestQueue(const char* why, bool createPrivate = false) {
    if (!window || !windowOriginal) { logf("queue (%s): Echo's window isn't hooked; can't queue", why); lastResult = int(Queued::NotReady); lastResultAt = GetTickCount64(); return; }
    logf("queue (%s): asking Echo's window thread", why);
    PostMessageW(window, createPrivate ? WM_PRIVATE : WM_QUEUE, 0, 0);
}

// ---- screen fade ----
// startFade(message): after fadeDelayMs the headset fades to black over FADE_OUT_MS
// (submitFrame), and BLACK_HOLD_MS later `message` goes to Echo's window thread (join,
// create's join, return to lobby). It fades back in over FADE_IN_MS once the net game state
// changes (the load has begun; Echo shows its own loading screen), or the join is over /
// nothing is joining after FADE_GIVE_UP_MS. The state is checked every frame there too: the
// tablet (fadeTick) pauses during loads.
SubmitFrame2 submitOriginal = nullptr;
std::atomic<U> fadeStartedAt{0};     // 0 = no fade
std::atomic<U> fadeEndedAt{0};       // fading back in since then
std::atomic<UINT> fadeMessage{0};    // still to post once black; 0 once posted
std::atomic<U> fadeActedAt{0};
std::atomic<int> fadeFromState{-1};  // the net game state when the fade started
std::atomic<int> blackFrames{0};     // frames sent with the fade this time (for the log)

SubmitFrame2 endFrameOriginal = nullptr;

void stopFade(const char* why);

// The fade quad: a 16x16 render-target swap chain made on Echo's own D3D11 device (found
// through Echo's eye texture), cleared to (0,0,0,alpha) each frame and committed.
// LibOVR layouts (SDK 1.25+): ovrLayerHeader {int Type; unsigned Flags; char Reserved[128]}
// (older SDKs have no Reserved: told apart from Echo's own layer); ovrLayerQuad after the
// header: ovrTextureSwapChain ColorTexture; ovrRecti Viewport; ovrPosef QuadPoseCenter
// {quat xyzw, pos xyz}; ovrVector2f QuadSize. ovrTextureSwapChainDesc {Type, Format,
// ArraySize, Width, Height, MipLevels, SampleCount, ovrBool StaticImage, MiscFlags, BindFlags}.
// Echo renders with D3D12 (it loads d3d12.dll; v2.8 asked Echo's eye texture for a D3D11
// texture and got -1005). So the quad's textures are STEPS static-image swap chains, black at
// alpha 1/STEPS .. 1, cleared once on our own command queue on Echo's device the first time
// a fade runs; each frame the layer just points at the chain for the current alpha (no GPU
// work during play). The chains are made with Echo's own command queue (caught in
// createChainDetour when Echo makes its eye chains): with our own the runtime said -1005
// (v2.9). Static images first; if refused, ordinary chains committed once.
namespace quad {
using GetBuffer = int (*)(void*, void*, int, IID, void**);
using Create = int (*)(void*, IUnknown*, const void*, void**);
using Commit = int (*)(void*, void*);
using ChainInt = int (*)(void*, void*, int*);
// 128 steps: with 16 a step lasted ~60 ms and the fade looked choppy (v3.0); 64 was better,
// 128 asked for. Making them takes ~0.1 s, so it's done during the delay before the fade.
constexpr int LAYER_QUAD = 3, FLAG_HEAD_LOCKED = 4, FORMAT_RGBA8 = 4, BIND_RENDER_TARGET = 1, STEPS = 128;
Create createOriginal = nullptr;
std::atomic<ID3D12CommandQueue*> echoQueue{nullptr};
bool failed = false, ready = false;
size_t header = 0;
void* chains[STEPS + 1] = {};  // [k]: black at alpha k/STEPS
alignas(16) unsigned char layer[256] = {};

bool fail(const char* why, long result = 0) {
    failed = true;
    logf("fade: no fade layer (%s, %ld): cutting to black instead", why, result);
    return false;
}

template <class T> void release(T*& p) { if (p) p->Release(), p = nullptr; }

// Echo's render thread, first fade: everything made once and kept.
bool make(void* session, void* const* layers, unsigned count) {
    if (ready) return true;
    if (failed) return false;
    HMODULE ovr = GetModuleHandleW(L"LibOVRRT64_1.dll");
    auto getBuffer = reinterpret_cast<GetBuffer>(GetProcAddress(ovr, "ovr_GetTextureSwapChainBufferDX"));
    Create create = createOriginal ? createOriginal : reinterpret_cast<Create>(GetProcAddress(ovr, "ovr_CreateTextureSwapChainDX"));
    auto commit = reinterpret_cast<Commit>(GetProcAddress(ovr, "ovr_CommitTextureSwapChain"));
    auto currentIndex = reinterpret_cast<ChainInt>(GetProcAddress(ovr, "ovr_GetTextureSwapChainCurrentIndex"));
    if (!getBuffer || !create || !commit || !currentIndex) return fail("LibOVR functions missing");
    P echo = nullptr;
    for (unsigned i = 0; i < count && !echo; i++) echo = static_cast<P>(layers[i]);
    if (!echo || !readable(echo, 160)) return fail("no layer of Echo's");
    // The eye texture follows the header: Reserved[128] (all zero) then the texture at 136
    // (v2.8 log: type 2, zeros, the same chain for both eyes at 136 and 144), else at 8.
    bool reserved = true;
    for (int i = 8; i < 136; i++) reserved = reserved && !echo[i];
    header = reserved && at<void*>(echo, 136) ? 136 : 8;
    ID3D12Resource* texture = nullptr;
    int r = getBuffer(session, at<void*>(echo, header), 0, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&texture));
    if (r < 0 || !texture) return fail("Echo's eye texture isn't D3D12", r);
    ID3D12Device* device = nullptr;
    texture->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
    release(texture);
    if (!device) return fail("no D3D12 device");

    ID3D12CommandQueue* queue = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12Resource* textures[STEPS + 1] = {};
    const char* problem = nullptr;
    long code = 0;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = STEPS;
    if ((code = device->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue))) < 0) problem = "command queue";
    else if ((code = device->CreateDescriptorHeap(&heapDesc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&heap))) < 0) problem = "descriptor heap";
    else if ((code = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&allocator))) < 0) problem = "command allocator";
    else if ((code = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list))) < 0) problem = "command list";
    else if ((code = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence))) < 0) problem = "fence";
    if (!problem) {
        D3D12_CPU_DESCRIPTOR_HANDLE first = heap->GetCPUDescriptorHandleForHeapStart();
        UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        ID3D12CommandQueue* owner = echoQueue ? echoQueue.load() : queue;
        logf("fade: making the fade layer with %s command queue", echoQueue ? "Echo's" : "our own (Echo's wasn't caught)");
        int staticImage = 1;
        for (int k = 1; k <= STEPS && !problem; k++) {
            int desc[10] = {0, FORMAT_RGBA8, 1, 16, 16, 1, 1, staticImage, 0, BIND_RENDER_TARGET};
            code = create(session, owner, desc, &chains[k]);
            if (code < 0 && k == 1) {  // not as a static image: an ordinary chain, committed once
                logf("fade: static image refused (%ld); trying an ordinary swap chain", code);
                staticImage = desc[7] = 0;
                chains[k] = nullptr;
                code = create(session, owner, desc, &chains[k]);
            }
            if (code < 0 || !chains[k]) { chains[k] = nullptr; problem = "swap chain"; break; }
            int index = 0;
            if (!staticImage) currentIndex(session, chains[k], &index);
            if ((code = getBuffer(session, chains[k], index, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&textures[k]))) < 0 || !textures[k]) { problem = "swap chain texture"; break; }
            D3D12_CPU_DESCRIPTOR_HANDLE target = first;
            target.ptr += SIZE_T(k - 1) * step;
            device->CreateRenderTargetView(textures[k], &view, target);
            const float colour[4] = {0, 0, 0, float(k) / STEPS};
            list->ClearRenderTargetView(target, colour, 0, nullptr);
        }
    }
    if (!problem) {
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, 1);
        if (fence->GetCompletedValue() < 1) {
            HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            fence->SetEventOnCompletion(1, done);
            WaitForSingleObject(done, 2000);
            CloseHandle(done);
        }
        if (fence->GetCompletedValue() < 1) problem = "clearing took too long";
        for (int k = 1; k <= STEPS && !problem; k++)
            if ((code = commit(session, chains[k])) < 0) problem = "commit";
    }
    for (auto& t : textures) release(t);
    release(fence), release(list), release(allocator), release(heap), release(device);
    if (problem) return fail(problem, code);  // queue kept: chains made on it may still exist

    at<int>(layer, 0) = LAYER_QUAD;
    at<unsigned>(layer, 4) = FLAG_HEAD_LOCKED;
    int* viewport = &at<int>(layer, header + 8);
    viewport[0] = viewport[1] = 0, viewport[2] = viewport[3] = 16;
    float* pose = &at<float>(layer, header + 24);  // quat (0,0,0,1), 0.3 m ahead
    pose[0] = pose[1] = pose[2] = 0, pose[3] = 1, pose[4] = pose[5] = 0, pose[6] = -0.3f;
    float* size = &at<float>(layer, header + 52);  // 4 m square at 0.3 m: past the edge of view
    size[0] = size[1] = 4;
    ready = true;
    logf("fade: fade layer made (D3D12, header %zu bytes, %d steps)", header, STEPS);
    return true;
}

// ovr_CreateTextureSwapChainDX, Echo's render thread: Echo's command queue (d3dPtr) noted.
int createChainDetour(void* session, IUnknown* d3d, const void* desc, void** chain) {
    if (d3d && !echoQueue) {
        ID3D12CommandQueue* queue = nullptr;
        if (d3d->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue)) >= 0 && queue) {
            echoQueue = queue;  // reference kept
            logf("fade: caught Echo's command queue");
        }
    }
    return createOriginal(session, d3d, desc, chain);
}

// ovr_Destroy: our chains go first (v3.1: "ovrError_LeakingResources, client disconnecting
// with active texture sets" at shutdown). Echo's render thread; no fade can run after this.
using Destroy = void (*)(void*);
Destroy destroyOriginal = nullptr;
void destroyDetour(void* session) {
    if (ready) {
        using DestroyChain = void (*)(void*, void*);
        auto destroyChain = reinterpret_cast<DestroyChain>(GetProcAddress(GetModuleHandleW(L"LibOVRRT64_1.dll"), "ovr_DestroyTextureSwapChain"));
        for (auto& chain : chains)
            if (chain && destroyChain) destroyChain(session, chain), chain = nullptr;
        ready = false;
        failed = true;  // the session is gone: no more fade layer this run
        logf("fade: fade layer released");
    }
    destroyOriginal(session);
}

// Points the layer at the texture for `alpha` (0..1].
void use(float alpha) {
    int k = std::clamp(int(alpha * STEPS + 0.999f), 1, STEPS);
    at<void*>(layer, header) = chains[k];
}
}  // namespace quad

// How black the picture is now: 0..1, eased in and out (smoothstep).
float fadeAlpha(U now) {
    float x;
    if (U started = fadeStartedAt) {
        U t = now - started;
        if (t > fadeDelayMs + FADE_MAX_MS) { stopFade("black for too long"); return 1.f; }  // never stuck
        if (t < fadeDelayMs) return 0.f;
        x = std::min(1.f, float(t - fadeDelayMs) / float(FADE_OUT_MS));
    } else {
        U ended = fadeEndedAt;
        if (!ended || now - ended >= FADE_IN_MS) return 0.f;
        x = 1.f - float(now - ended) / float(FADE_IN_MS);
    }
    return x * x * (3.f - 2.f * x);
}

// What Echo hands the headset (layer types and flags; ovrLayerHeader starts {int Type;
// unsigned Flags}), logged whenever it changes, to see how Echo's own loading fade is done.
void logLayers(void* const* layers, unsigned count) {
    static char last[256] = "";
    static int logged = 0;
    char now[256];
    int n = snprintf(now, sizeof now, "%u:", count);
    for (unsigned i = 0; i < count && i < 8 && n < 200; i++) {
        const unsigned* header = static_cast<const unsigned*>(layers[i]);
        n += header ? snprintf(now + n, sizeof now - n, " t%u/f%x", header[0], header[1]) : snprintf(now + n, sizeof now - n, " -");
    }
    if (strcmp(now, last) && logged < 200) {
        logged++;
        strcpy(last, now);
        void* netgame = netGame();
        logf("headset: layers %s (net game state %d)", now, netgame ? at<int>(netgame, NETGAME_STATE) : -1);
    }
}

// Echo's render thread, every frame it shows in the headset (ovr_EndFrame and
// ovr_SubmitFrame2 take the same arguments).
int submitFrame(SubmitFrame2 original, void* session, long long frame, const void* scale, void* const* layers, unsigned count) {
    if (layers) logLayers(layers, count);
    U now = GetTickCount64();
    // Loading the level (Echo's own loading screen from here): fade back in. Any change isn't
    // enough: a join or return changes the state at once, long before the load (v2.7).
    if (fadeStartedAt) {
        static int lastState = -2;
        void* netgame = netGame();
        int state = netgame ? at<int>(netgame, NETGAME_STATE) : -1;
        if (state != lastState) {
            logf("fade: net game state %d -> %d (lobby state %d)", lastState, state, lobbyState());
            lastState = state;
        }
        // Returning to the lobby goes 9 -> 5 (Echo looks for a lobby server, its own black
        // screen) for ~3.6 s before loading (v3.0 log): fade back in at 5 rather than wait.
        if (fadeActedAt && state == NETGAME_LOADING) stopFade("loading");
        else if (fadeActedAt && state == NETGAME_LOBBY_SEARCH) stopFade("leaving for the lobby");
    }
    float alpha = fadeAlpha(now);
    // During the delay (nothing showing yet): make the fade layer now, not on the first faded frame.
    if (alpha <= 0.f && fadeStartedAt && count && layers) quad::make(session, layers, count);
    if (alpha <= 0.f || !count || !layers || count >= 16) return original(session, frame, scale, layers, count);
    blackFrames++;
    void* list[16] = {};
    if (quad::make(session, layers, count)) {
        quad::use(alpha);
        std::copy(layers, layers + count, list);
        list[count] = quad::layer;  // on top of Echo's
        int r = original(session, frame, scale, list, count + 1);
        if (r < 0) quad::fail("the runtime refused the fade layer", r);  // later frames: the cut
        return r;
    }
    if (alpha < 1.f) return original(session, frame, scale, layers, count);
    return original(session, frame, scale, list, count);  // null layers are skipped: black
}
int submitDetour(void* session, long long frame, const void* scale, void* const* layers, unsigned count) {
    return submitFrame(submitOriginal, session, frame, scale, layers, count);
}
int endFrameDetour(void* session, long long frame, const void* scale, void* const* layers, unsigned count) {
    return submitFrame(endFrameOriginal, session, frame, scale, layers, count);
}

bool hookExport(HMODULE module, const char* name, void* detour, void** original) {
    void* target = reinterpret_cast<void*>(GetProcAddress(module, name));
    MH_STATUS s = target ? MH_CreateHook(target, detour, original) : MH_ERROR_FUNCTION_NOT_FOUND;
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) { *original = nullptr; logf("fade: can't hook %s (%s)", name, MH_StatusToString(s)); return false; }
    logf("fade hook installed (%s)", name);
    return true;
}

// Echo's LibOVR shim looks every ovr_* function up (GetProcAddress) at ovr_Initialize, which
// runs before this plugin starts (v3.3/v3.4 hooked GetProcAddress and saw no lookups), and
// keeps the addresses in a table in echovr.exe's writable data. Swapping the four we need
// there is a pointer write each: no code patched, no threads frozen. v3.0/v3.1 patched LibOVR
// itself with MinHook at startup instead; freezing every thread while Echo's audio started
// played a ring and static at every launch. Done at startup, before Echo makes its swap
// chains (v3.0 caught the queue ~0.7 s after the plugin started).
int retargetShim() {
    HMODULE ovr = GetModuleHandleW(L"LibOVRRT64_1.dll");
    if (!ovr) return 0;
    struct Takeover { const char* name; void* detour; void** original; };
    const Takeover takeovers[] = {
        {"ovr_EndFrame", reinterpret_cast<void*>(endFrameDetour), reinterpret_cast<void**>(&endFrameOriginal)},
        {"ovr_SubmitFrame2", reinterpret_cast<void*>(submitDetour), reinterpret_cast<void**>(&submitOriginal)},
        {"ovr_CreateTextureSwapChainDX", reinterpret_cast<void*>(quad::createChainDetour), reinterpret_cast<void**>(&quad::createOriginal)},
        {"ovr_Destroy", reinterpret_cast<void*>(quad::destroyDetour), reinterpret_cast<void**>(&quad::destroyOriginal)},
    };
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(exe + dos->e_lfanew);
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    int swapped = 0;
    for (const Takeover& t : takeovers) {
        if (*t.original) { swapped++; continue; }  // done on an earlier pass
        void* target = reinterpret_cast<void*>(GetProcAddress(ovr, t.name));
        if (!target) continue;
        int found = 0;
        for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; s++) {
            if (!(section[s].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
            auto slot = reinterpret_cast<void**>(exe + section[s].VirtualAddress);
            size_t slots = section[s].Misc.VirtualSize / sizeof(void*);
            for (size_t i = 0; i < slots; i++)
                if (slot[i] == target) {
                    *t.original = target;
                    InterlockedExchangePointer(&slot[i], t.detour);
                    found++;
                }
        }
        if (found) logf("fade: %s: %d pointer(s) in Echo's table now ours", t.name, found);
        swapped += found > 0;
    }
    return swapped;
}

// Echo may load LibOVR before or after this plugin starts (v3.5: before; v3.6: 35 s of
// "after" and the swap came too late for Echo's queue). This thread checks every millisecond
// from startup until Echo's table holds the LibOVR addresses (filled at ovr_Initialize; Echo
// makes its swap chains ~1 s later) and swaps them at once.
std::atomic<bool> shimWatchDone{false};
DWORD WINAPI shimWatch(void*) {
    U start = GetTickCount64(), loadedAt = 0;
    while (GetTickCount64() - start < 300000) {
        if (GetModuleHandleW(L"LibOVRRT64_1.dll")) {
            if (!loadedAt) loadedAt = GetTickCount64();
            if (retargetShim() == 4) break;
            if (GetTickCount64() - loadedAt > 30000) break;  // some never appeared: keep what we have
        }
        Sleep(1);
    }
    logf("fade: Echo's LibOVR table %s", endFrameOriginal || submitOriginal ? "taken over" : "not found");
    shimWatchDone = true;
    return 0;
}

bool hookSubmit() {
    static std::atomic<bool> tried{false};
    if (submitOriginal || endFrameOriginal) return true;
    HMODULE ovr = GetModuleHandleW(L"LibOVRRT64_1.dll");
    if (!ovr || !shimWatchDone || tried.exchange(true)) return submitOriginal || endFrameOriginal;
    // Fallback (no table found): patched directly, as v2.6 did. Echo's command queue is missed
    // this way, so the fade becomes the cut to black.
    logf("fade: Echo's LibOVR table not found; hooking LibOVR directly");
    hookExport(ovr, "ovr_Destroy", reinterpret_cast<void*>(quad::destroyDetour), reinterpret_cast<void**>(&quad::destroyOriginal));
    bool end = hookExport(ovr, "ovr_EndFrame", reinterpret_cast<void*>(endFrameDetour), reinterpret_cast<void**>(&endFrameOriginal));
    bool submit = hookExport(ovr, "ovr_SubmitFrame2", reinterpret_cast<void*>(submitDetour), reinterpret_cast<void**>(&submitOriginal));
    if (!end && !submit) logf("fade: no black screen");
    return end || submit;
}

void startFade(UINT message, const char* why) {
    if (!window) return;
    if (!hookSubmit()) { PostMessageW(window, message, 0, 0); return; }  // no headset hook: straight there
    void* netgame = netGame();
    fadeFromState = netgame ? at<int>(netgame, NETGAME_STATE) : -1;
    fadeMessage = message;
    fadeActedAt = 0;
    fadeEndedAt = 0;
    blackFrames = 0;
    fadeStartedAt = GetTickCount64();
    logf("fade: fading in %llu ms, then %s", U(fadeDelayMs), why);
}

void stopFade(const char* why) {
    if (fadeStartedAt.exchange(0)) {
        fadeEndedAt = GetTickCount64();
        logf("fade: %s; fading back in (%d faded frames)", why, int(blackFrames));
    }
    fadeMessage = 0;
}

// Tablet update, ten times a second.
void fadeTick() {
    hookSubmit();
    U started = fadeStartedAt;
    if (!started) return;
    U now = GetTickCount64();
    if (UINT message = fadeMessage) {
        if (now - started < fadeDelayMs + FADE_OUT_MS + BLACK_HOLD_MS) return;
        logf("fade: black");
        fadeMessage = 0;
        fadeActedAt = now;
        PostMessageW(window, message, 0, 0);
        return;
    }
    U acted = fadeActedAt;
    if (!acted) return;
    void* netgame = netGame();
    int state = netgame ? at<int>(netgame, NETGAME_STATE) : -1;
    int lobby = lobbyState();
    bool joining = lobby >= 6 && lobby < LOBBY_IN_GAME;  // finding .. starting
    if (state == NETGAME_LOADING) stopFade("loading");
    else if (state == NETGAME_LOBBY_SEARCH) stopFade("leaving for the lobby");
    else if (now - acted > FADE_MAX_MS) stopFade("black for too long");
    else if (now - acted > FADE_GIVE_UP_MS && !joining) stopFade(lobby == LOBBY_IN_GAME ? "joined" : "nothing happened");
}

// Tablet update, every frame: once a search we started has found a match ("lobby join
// queued"), join it after the same countdown the menu uses.
void joinTick() {
    static U polledAt = 0;
    U now = GetTickCount64();
    if (now - polledAt < 200) return;
    polledAt = now;
    int state = lobbyState();
    // A private match on the chosen server: in the match means that slot worked (remember it);
    // dropping out of matchmaking any other way means it didn't (that slot is busy), so the
    // next one is tried, and after the last the server may put it anywhere.
    int slot = privateTry;
    if (slot >= 0) {
        if (matchmaking(state)) privateSeenMatchmaking = true;
        else if (!privateSeenMatchmaking) {}  // still in the social lobby (also "in game"): not started yet
        else if (state >= LOBBY_JOINED && state <= LOBBY_IN_GAME) {
            logf("private: in the match on %s:%u", privateServer.c_str(), unsigned(privatePorts[slot]));
            WritePrivateProfileStringW(L"private", L"last_port", std::to_wstring(privatePorts[slot]).c_str(), iniPath().c_str());
            privateTry = -1;
        } else if (window) {
            logf("private: %s:%u didn't take it (lobby state %d)", privateServer.c_str(), unsigned(privatePorts[slot]), state);
            privateTry = -2;
            PostMessageW(window, WM_PRIVATE_NEXT, WPARAM(slot + 1), 0);
            return;
        }
    }
    if (!matchmaking(state)) searchSince = 0;
    else if (!searchSince) searchSince = now;
    if (state != LOBBY_JOIN_QUEUED) {
        if (!matchmaking(state)) ourSearch = ourPrivate = false;
        joinQueuedSince = 0;
        joinSent = false;
        return;
    }
    if (!joinQueuedSince) {  // a match was just found (ours or the menu's)
        joinQueuedSince = now;
        if (searchSince) lastWaitMs = now - searchSince;
        logf("match found after %llu s", U(lastWaitMs) / 1000);
        if (ourSearch) {  // the menu's script plays "Joining Match" for its own searches only
            if (uint32_t voice = voiceJoiningMatch) postEventOriginal(voice, announcerObject, 0, nullptr, nullptr, 0, nullptr, 0);
        } else if (!voiceJoiningMatch) {  // the menu's search: its "Joining Match" just played, or is about to
            if (lastAnnouncerEvent && now - lastAnnouncerEventAt < 1500) learnJoiningMatch(lastAnnouncerEvent);
            else learnJoiningUntil = now + 2500;
        }
    }
    // A private match we created is ours to walk into: no countdown.
    if (!ourSearch || joinSent || (!ourPrivate && now - joinQueuedSince < JOIN_DELAY_MS) || !window) return;
    joinSent = true;
    logf("join: match found for our search; asking Echo's window thread to join it");
    startFade(WM_JOIN, "joining the match");
}

// ---- Echo's API ----
// GET /session from Echo's own HTTP API into `body`: the HTTP status (200 in a match, an
// error in the lobby), or 0 when nothing answers.
DWORD apiSession(std::string& body) {
    static HINTERNET session = nullptr, connection = nullptr;
    if (!session) {
        session = WinHttpOpen(L"EchoQueue", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return 0;
        WinHttpSetTimeouts(session, 300, 300, 300, 500);
        connection = WinHttpConnect(session, L"127.0.0.1", API_PORT, 0);
    }
    if (!connection) return 0;
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", L"/session", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) return 0;
    DWORD status = 0, size = sizeof status;
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr) ||
        !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        status = 0;
    body.clear();
    char buffer[4096];
    DWORD read = 0;
    while (status == 200 && WinHttpReadData(request, buffer, sizeof buffer, &read) && read) body.append(buffer, read);
    WinHttpCloseHandle(request);
    return status;
}

// The string value of "key" in a flat JSON body, or "" (enough for game_status / match_type).
std::string jsonString(const std::string& body, const char* key) {
    size_t i = body.find("\"" + std::string(key) + "\"");
    if (i == std::string::npos) return "";
    i = body.find_first_not_of(" \t\r\n:", i + strlen(key) + 2);
    if (i == std::string::npos || body[i] != '"') return "";
    size_t end = body.find('"', i + 1);
    return end == std::string::npos ? "" : body.substr(i + 1, end - i - 1);
}

// ---- hours today ----
// Time Echo has been running today, added up across sessions in EchoQueue.ini ([stats] day /
// seconds_today, saved once a minute) and back to 0 on a new (local) day.
std::mutex statsMutex;
std::wstring statsDay;  // the day secondsToday counts, YYYY-MM-DD

std::wstring localDay() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[16];
    swprintf(b, 16, L"%04u-%02u-%02u", t.wYear, t.wMonth, t.wDay);
    return b;
}

void saveToday() {
    WritePrivateProfileStringW(L"stats", L"day", statsDay.c_str(), iniPath().c_str());
    WritePrivateProfileStringW(L"stats", L"seconds_today", std::to_wstring(U(secondsToday)).c_str(), iniPath().c_str());
}

// Starts again at 0 once midnight has passed; adds `seconds` played.
void updateToday(U seconds) {
    std::lock_guard<std::mutex> lock(statsMutex);
    std::wstring day = localDay();
    bool changed = seconds > 0;
    if (day != statsDay) { statsDay = day; secondsToday = 0; changed = true; }
    secondsToday += seconds;
    if (changed) saveToday();
}

void loadToday() {
    wchar_t day[16];
    GetPrivateProfileStringW(L"stats", L"day", L"", day, 16, iniPath().c_str());
    statsDay = day;
    secondsToday = GetPrivateProfileIntW(L"stats", L"seconds_today", 0, iniPath().c_str());
    updateToday(0);
    logf("stats: %llu min played today", U(secondsToday) / 60);
}

// Minutes under an hour ("45 MINS"), then hours and minutes ("1H 22M").
std::string playedText(U seconds) {
    char b[16];
    U m = seconds / 60;
    if (m < 60) snprintf(b, sizeof b, "%llu MIN%s", m, m == 1 ? "" : "S");
    else snprintf(b, sizeof b, "%lluH %lluM", m / 60, m % 60);
    return b;
}

// ---- matches overall ----
// The game's own count: GamesPlayed (arena + combat) in the profile the game saves,
// %LOCALAPPDATA%\RAD\EchoVR\users\<platform>\<id>\serverprofile.json (the newest one when
// several accounts have played on this PC).
std::atomic<int> matchesOverall{-1};  // -1 = not read yet

std::wstring profilePath() {
    wchar_t local[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return L"";
    std::wstring users = std::wstring(local) + L"\\RAD\\EchoVR\\users\\", best;
    FILETIME newest{};
    WIN32_FIND_DATAW platform, account;
    HANDLE p = FindFirstFileW((users + L"*").c_str(), &platform);
    if (p == INVALID_HANDLE_VALUE) return L"";
    do {
        if (!(platform.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || platform.cFileName[0] == L'.') continue;
        std::wstring dirPath = users + platform.cFileName + L"\\";
        HANDLE a = FindFirstFileW((dirPath + L"*").c_str(), &account);
        if (a == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(account.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || account.cFileName[0] == L'.') continue;
            std::wstring file = dirPath + account.cFileName + L"\\serverprofile.json";
            WIN32_FILE_ATTRIBUTE_DATA info;
            if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &info) && CompareFileTime(&info.ftLastWriteTime, &newest) > 0) {
                newest = info.ftLastWriteTime;
                best = file;
            }
        } while (FindNextFileW(a, &account));
        FindClose(a);
    } while (FindNextFileW(p, &platform));
    FindClose(p);
    return best;
}

// The extent of the object "key":{...} at or after `from`, string-aware brace matching.
bool jsonObject(const std::string& s, size_t from, const char* key, size_t& begin, size_t& end) {
    size_t i = s.find("\"" + std::string(key) + "\"", from);
    if (i == std::string::npos) return false;
    i = s.find_first_not_of(" \t\r\n:", i + strlen(key) + 2);
    if (i == std::string::npos || s[i] != '{') return false;
    int depth = 0;
    bool quoted = false;
    for (size_t j = i; j < s.size(); j++) {
        char ch = s[j];
        if (quoted) { if (ch == '\\') j++; else if (ch == '"') quoted = false; continue; }
        if (ch == '"') quoted = true;
        else if (ch == '{') depth++;
        else if (ch == '}' && --depth == 0) { begin = i; end = j + 1; return true; }
    }
    return false;
}

// stats.<mode>.GamesPlayed.val in the profile, 0 when the mode has none.
int gamesPlayed(const std::string& profile, const char* mode) {
    size_t sb, se, mb, me, gb, ge;
    if (!jsonObject(profile, 0, "stats", sb, se) || !jsonObject(profile, sb + 1, mode, mb, me) || mb >= se ||
        !jsonObject(profile, mb + 1, "GamesPlayed", gb, ge) || gb >= me) return 0;
    size_t v = profile.find("\"val\"", gb);
    if (v == std::string::npos || v >= ge) return 0;
    v = profile.find_first_not_of(" \t\r\n:", v + 5);
    return v == std::string::npos ? 0 : atoi(profile.c_str() + v);
}

// matchWatch, every couple of seconds: re-read the profile when the game has saved it.
void readMatchesOverall() {
    static U checkedAt = 0, pathAt = 0;
    static std::wstring path;
    static FILETIME seen{};
    U now = GetTickCount64();
    if (checkedAt && now - checkedAt < 2000) return;
    checkedAt = now;
    if (path.empty() || now - pathAt > 60000) { path = profilePath(); pathAt = now; }
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) ||
        CompareFileTime(&info.ftLastWriteTime, &seen) == 0) return;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string profile;
    char buffer[8192];
    DWORD read = 0;
    while (ReadFile(f, buffer, sizeof buffer, &read, nullptr) && read) profile.append(buffer, read);
    CloseHandle(f);
    int arena = gamesPlayed(profile, "arena"), combat = gamesPlayed(profile, "combat");
    if (!arena && !combat && profile.find("GamesPlayed") != std::string::npos) return;  // caught mid-save: try again later
    seen = info.ftLastWriteTime;
    if (matchesOverall.exchange(arena + combat) != arena + combat)
        logf("stats: %d matches overall (arena %d, combat %d)", arena + combat, arena, combat);
}

// ---- players online ----
// EchoVRCE's public status page (what its own matchmaker status example reads):
// https://g.echovrce.com/status/matches, whose top-level "player_count" is everyone connected.
// Fetched on its own thread, only while the PLAY page shows PLAYERS ONLINE, once a minute.
std::atomic<int> playersOnline{-1};      // -1 = not fetched yet
std::atomic<bool> playersWanted{false};  // the page is showing PLAYERS ONLINE
std::atomic<U> playersWantedAt{0};

int fetchPlayersOnline() {
    static HINTERNET session = nullptr, connection = nullptr;
    if (!session) {
        session = WinHttpOpen(L"EchoQueue", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return -1;
        WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);
        connection = WinHttpConnect(session, L"g.echovrce.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    }
    if (!connection) return -1;
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", L"/status/matches", nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) return -1;
    DWORD status = 0, size = sizeof status;
    std::string body;
    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr) &&
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) &&
        status == 200) {
        char buffer[8192];
        DWORD read = 0;
        // player_count comes before the (long) list of matches: the first few KB are enough.
        while (body.size() < 4096 && WinHttpReadData(request, buffer, sizeof buffer, &read) && read) body.append(buffer, read);
    }
    WinHttpCloseHandle(request);
    size_t i = body.find("\"player_count\"");
    if (i == std::string::npos) { logf("players online: no player_count from g.echovrce.com (HTTP %lu)", status); return -1; }
    i = body.find_first_not_of(" \t\r\n:", i + 14);
    return i == std::string::npos ? -1 : atoi(body.c_str() + i);
}

DWORD WINAPI playersWatch(LPVOID) {
    U fetchedAt = 0;
    for (;;) {
        Sleep(500);
        U now = GetTickCount64();
        if (!playersWanted || now - playersWantedAt > 3000) continue;  // the page stopped asking
        if (fetchedAt && now - fetchedAt < 60000) continue;
        fetchedAt = now;
        int n = fetchPlayersOnline();
        if (n >= 0 && playersOnline.exchange(n) != n) logf("players online: %d", n);
    }
}

// Own thread, ten times a second: adds the time played to today's every 10 s, and while the
// net game is "in game" (a match or the social lobby, which the API doesn't report) asks the
// API until it reports a match, which sets inMatch for the LOBBY button.
DWORD WINAPI matchWatch(LPVOID) {
    bool counted = false, apiDown = false, apiSeen = false;
    U creditedAt = GetTickCount64();
    std::string body;
    for (;;) {
        Sleep(100);
        U now = GetTickCount64();
        if (now - creditedAt >= 10000) {
            U seconds = (now - creditedAt) / 1000;
            creditedAt += seconds * 1000;
            updateToday(seconds);
        }
        readMatchesOverall();
        void* netgame = fault ? nullptr : netGame();
        if (!netgame || at<int>(netgame, NETGAME_STATE) != NETGAME_IN_GAME) { counted = inMatch = false; continue; }
        if (counted) continue;
        DWORD http = apiSession(body);
        if (!http) {  // also normal in the social lobby, so only worth a word if it never answered
            if (!apiDown && !apiSeen) logf("Echo's API (127.0.0.1:%u/session) isn't answering; is API access on in Echo's settings?", unsigned(API_PORT));
            apiDown = true;
            Sleep(900);
            continue;
        }
        apiDown = false;
        apiSeen = true;
        if (http != 200) { Sleep(400); continue; }  // the social lobby: no match to report
        std::string type = jsonString(body, "match_type");
        if (type.empty()) continue;
        counted = inMatch = true;
        logf("in a %s match", type.c_str());
    }
}

// ---- tablet ----
// Canvas kinds: 0 root, 1 nav, 2 PLAY page, 3-5 the stock pages, 6 the stock title. The stock
// ones are drawn over ours, so while PLAY is up they are faded out with the canvas opacity:
// the game's scripts show/hide them (re-hiding meant a redraw every frame) but leave opacity alone.
// Which canvas is which, and where our elements are in the shared ones, depends on what other
// tablet mods added: the installer measures this PC's tablet and writes it to EchoQueue.layout
// (each canvas: element count, plus an index whose name tells it apart from every other canvas
// with that many elements).
constexpr unsigned KINDS = 7, FIRST_STOCK = 3;
constexpr unsigned CANVAS_OPACITY = 0x358;
// Kind 2 is EchoQueue's overlay canvas (page, title strip, tab icon), shown by the root's spare
// stock element 6 so that no canvas other mods count changes its element count.
struct Layout {
    unsigned counts[KINDS] = {}, markerIndex[KINDS] = {};
    U markers[KINDS] = {};
    unsigned rootSlot = 0;                                      // the root element showing the overlay
    std::vector<unsigned> navStockSelected, navOtherSelected;  // other tabs' highlights, hidden under PLAY
} layout;

std::vector<unsigned> readList(const wchar_t* key, const std::wstring& path) {
    wchar_t buffer[256];
    GetPrivateProfileStringW(L"tablet", key, L"", buffer, 256, path.c_str());
    std::vector<unsigned> out;
    for (wchar_t* p = buffer; *p;) {
        if (*p >= L'0' && *p <= L'9') out.push_back(wcstoul(p, &p, 10));
        else p++;
    }
    return out;
}

// -> false (and the reason logged) when the layout is missing or not for this build.
bool loadLayout() {
    std::wstring path = dir + L"\\EchoQueue.layout";
    if (GetPrivateProfileIntW(L"tablet", L"version", 0, path.c_str()) != QUEUE_LAYOUT_VERSION) {
        logf("tablet: EchoQueue.layout missing, unreadable or from another EchoQueue version -- run the EchoQueue installer's Repair");
        return false;
    }
    const wchar_t* names[KINDS] = {L"root", L"nav", L"overlay", L"lobby", L"rows", L"settings", L"title"};
    for (unsigned k = 0; k < KINDS; k++) {
        wchar_t buffer[64];
        unsigned long long marker = 0;
        GetPrivateProfileStringW(L"tablet", names[k], L"", buffer, 64, path.c_str());
        if (swscanf(buffer, L"%u,%u,%llx", &layout.counts[k], &layout.markerIndex[k], &marker) != 3 ||
            layout.markerIndex[k] >= layout.counts[k]) {
            logf("tablet: EchoQueue.layout has no valid '%ls' entry -- run the installer's Repair", names[k]);
            return false;
        }
        layout.markers[k] = marker;
    }
    auto get = [&](const wchar_t* key) { return unsigned(GetPrivateProfileIntW(L"tablet", key, 0, path.c_str())); };
    layout.rootSlot = get(L"root_slot");
    layout.navStockSelected = readList(L"nav_stock_selected", path);
    layout.navOtherSelected = readList(L"nav_other_selected", path);
    if (layout.counts[2] != QUEUE_OVERLAY_ELEMENTS || get(L"overlay_elements") != QUEUE_OVERLAY_ELEMENTS) {
        logf("tablet: EchoQueue.layout is for a different EchoQueue version -- run the installer's Repair");
        return false;
    }
    unsigned navMax = 0;
    for (unsigned i : layout.navStockSelected) navMax = std::max(navMax, i);
    for (unsigned i : layout.navOtherSelected) navMax = std::max(navMax, i);
    if (layout.rootSlot >= layout.counts[0] || navMax >= layout.counts[1]) {
        logf("tablet: EchoQueue.layout points outside the tablet -- run the installer's Repair");
        return false;
    }
    logf("tablet: layout for %u root / %u tab bar elements, %zu other tab(s)' highlights",
         layout.counts[0], layout.counts[1], layout.navOtherSelected.size());
    return true;
}

struct View { void* p = nullptr; bool saved = false; float opacity = 1; };
enum class Page { Stock, Queue };
struct Context {
    Page page = Page::Stock;
    int navState = -1;          // 0 stock tab highlighted, 1 PLAY
    U navCheckedAt = 0;
    std::array<View, KINDS> views{};
    std::set<unsigned> masked;
    U gateKey = ~0ull;          // button masks last applied for (page, searching, ready, buttons)
    std::string status, timer, timerCaption, statCaption, matches;  // what the page's dynamic text shows
    int searchShown = -1;       // the searching layout (teal card, timer, CANCEL SEARCH) shown
    int applied = -1;           // page visibility last applied (1 PLAY, 0 stock)
    U polledAt = 0;
    int state = -1;             // lobby state, polled a few times a second
    std::string flash;          // a press result shown for a moment
    U flashUntil = 0;
};
std::recursive_mutex tabletMutex;
std::map<void*, Context> contexts;                          // keyed by gamespace
std::map<void*, std::pair<void*, unsigned>> canvases;       // canvas -> (gamespace, kind)
std::set<void*> pending;
std::set<std::array<U, 3>> held;                            // (gamespace, actor, component)

bool isContent(U n) { return std::find(std::begin(CONTENT), std::end(CONTENT), n) != std::end(CONTENT); }
bool isStockTab(U n) { return n >= STOCK_TAB_FIRST && n <= STOCK_TAB_LAST; }
bool isOurs(U n) {
    return n == QUEUE_TAB || n == QUEUE_START || n == QUEUE_CANCEL || n == QUEUE_LOBBY || n == QUEUE_PRIVATE || n == QUEUE_STATS;
}

void failTablet(const char* why) { tabletFault = true; logf("TABLET FAULT: %s -- PLAY tab off for this session", why); }

void remember(void* p, unsigned depth = 0) {
    if (!p || depth > 8 || canvases.count(p) || !readable(static_cast<P>(p) + 0x370, 16)) return;
    U n = at<U>(p, 0xc0);
    auto elements = at<P>(p, 0x90);
    for (unsigned kind = 0; kind < KINDS; kind++) {
        if (n != layout.counts[kind] || !readable(elements, (layout.markerIndex[kind] + 1) * 224) ||
            at<U>(elements, layout.markerIndex[kind] * 224) != layout.markers[kind]) continue;
        auto cs = at<void*>(p, 0x370);
        if (!cs) { pending.insert(p); return; }
        pending.erase(p);
        auto gs = at<void*>(cs, 0x80);
        contexts[gs].views[kind] = View{p};
        canvases[p] = {gs, kind};
        logf("tablet: canvas kind=%u gs=%p ptr=%p", kind, gs, p);
        remember(at<void*>(p, 0x378), depth + 1);
        return;
    }
}

std::string elapsed(U ms) {
    char b[16];
    snprintf(b, sizeof b, "%llu:%02llu", ms / 60000, ms / 1000 % 60);
    return b;
}

std::string statusFor(int state) {
    U now = GetTickCount64();
    if (state >= 3 && state <= 6) return ourPrivate ? "CREATING A PRIVATE MATCH" : "SEARCHING FOR A MATCH";
    if (state == LOBBY_JOIN_QUEUED) {
        U since = joinQueuedSince;
        if (ourSearch && !ourPrivate && since && now - since < JOIN_DELAY_MS)  // a private match has no countdown
            return "MATCH FOUND - JOINING IN " + std::to_string((JOIN_DELAY_MS - (now - since) + 999) / 1000) + "...";
        return "MATCH FOUND - JOINING...";
    }
    if (state == 8) return "JOINING THE MATCH...";
    return "TAP FIND MATCH TO QUEUE";
}

void setText(void* canvas, unsigned element, std::string& shownText, const std::string& value) {
    if (value != shownText) { text(canvas, element, value.c_str()); shownText = value; }
}

bool shown(void* canvas, unsigned element) { return at<unsigned>(at<P>(canvas, 0x90), element * 224 + 0x10) == 0; }

// Show/hide only when an element isn't already that way: each call makes the tablet redraw,
// while reading the flag is free.
void ensure(void* canvas, unsigned element, bool visible) {
    if (shown(canvas, element) != visible) show(canvas, element, visible);
}

void render(Context& c) {
    bool active = c.page == Page::Queue && !tabletFault;
    U now = GetTickCount64();
    bool changed = c.applied != int(active);
    c.applied = int(active);
    if (now - c.polledAt > 200) { c.state = lobbyState(); c.polledAt = now; }
    for (unsigned i = FIRST_STOCK; i < KINDS; i++) {  // stock pages and title: faded out under PLAY
        auto& v = c.views[i];
        if (!v.p) continue;
        float& opacity = at<float>(v.p, CANVAS_OPACITY);
        if (active) {
            if (!v.saved) { v.opacity = opacity; v.saved = true; }
            if (opacity != 0) alpha(v.p, 0);
        } else if (v.saved) {
            alpha(v.p, v.opacity);
            v.saved = false;
        }
    }
    // The overlay: our page, the cover over the stock title and our title tab (shown with PLAY),
    // hung on the root's spare element, which must stay shown for our tab icon to be there.
    void* overlay = c.views[2].p;
    if (c.views[0].p) ensure(c.views[0].p, layout.rootSlot, true);
    if (overlay && changed)
        for (unsigned i : {QUEUE_PAGE_PAGE, QUEUE_PAGE_COVER, QUEUE_PAGE_HEADER, QUEUE_PAGE_TITLE, QUEUE_PAGE_SUBTITLE})
            ensure(overlay, i, active);
    // Tab highlight: ours while our page is up, otherwise the stock ones (which the game's
    // own scripts keep pointing at the right stock tab). While ours is up, the game may
    // re-show its highlight; re-checked a few times a second, not every frame.
    int nav = active ? 1 : 0;
    if (overlay && c.views[1].p && (c.navState != nav || (active && now - c.navCheckedAt > 250))) {
        c.navCheckedAt = now;
        ensure(overlay, QUEUE_PAGE_TAB_SELECTED, nav == 1);
        ensure(overlay, QUEUE_PAGE_TAB_ICON, nav == 0);
        if (active) {
            for (auto i : layout.navStockSelected) ensure(c.views[1].p, i, false);
            for (auto i : layout.navOtherSelected) ensure(c.views[1].p, i, false);  // their own plugin shows them again
        } else for (auto i : layout.navStockSelected) show(c.views[1].p, i, true);  // once; the game's scripts pick the right one
        c.navState = nav;
    }
    void* page = c.views[2].p;
    if (active && page) {
        int state = c.state;
        bool searching = matchmaking(state);
        Queued r = Queued(lastResult.load());
        if (r != Queued::None && now - lastResultAt < 3000 && r != Queued::Started) {
            c.flash = r == Queued::AlreadySearching ? "ALREADY SEARCHING"
                    : r == Queued::NotReady ? "NOT READY YET - TRY AGAIN IN A MOMENT" : "COULDN'T QUEUE - SEE ECHOQUEUE.LOG";
            c.flashUntil = lastResultAt + 3000;
        }
        std::string message = now < c.flashUntil && !searching ? c.flash : statusFor(state);
        setText(page, QUEUE_PAGE_STATUS, c.status, message);
        // Searching layout: teal card, queue timer, CANCEL SEARCH in place of FIND MATCH.
        if (c.searchShown != int(searching)) {
            show(page, QUEUE_PAGE_CARD, !searching);
            show(page, QUEUE_PAGE_CARD_ACTIVE, searching);
            show(page, QUEUE_PAGE_PRIVATE, !searching);
            show(page, QUEUE_PAGE_PRIVATE_TEXT, !searching);
            show(page, QUEUE_PAGE_PRIVATE_BUSY, searching);
            show(page, QUEUE_PAGE_LOBBY, !searching);
            show(page, QUEUE_PAGE_LOBBY_TEXT, !searching);
            show(page, QUEUE_PAGE_LOBBY_BUSY, searching);
            show(page, QUEUE_PAGE_FIND, !searching);
            show(page, QUEUE_PAGE_FIND_TEXT, !searching);
            show(page, QUEUE_PAGE_CANCEL, searching);
            show(page, QUEUE_PAGE_CANCEL_TEXT, searching);
            c.searchShown = int(searching);
        }
        if (searching) {
            U since = searchSince;
            setText(page, QUEUE_PAGE_TIMER, c.timer, elapsed(since ? now - since : 0));
            setText(page, QUEUE_PAGE_TIMER_CAPTION, c.timerCaption,
                    state >= LOBBY_JOIN_QUEUED ? (ourPrivate ? "JOINING" : "MATCH FOUND") : ourPrivate ? "CREATING" : "IN QUEUE");
        } else {  // the same spot shows the state while idle
            setText(page, QUEUE_PAGE_TIMER, c.timer, "READY");
            setText(page, QUEUE_PAGE_TIMER_CAPTION, c.timerCaption, "STATUS");
        }
        // The stats box: whichever of its three numbers was last picked (tap to step through).
        int mode = statMode, overall = matchesOverall, players = playersOnline;
        playersWanted = mode == STAT_PLAYERS;
        if (mode == STAT_PLAYERS) playersWantedAt = now;
        setText(page, QUEUE_PAGE_STATS_CAPTION, c.statCaption, STAT_CAPTIONS[mode]);
        setText(page, QUEUE_PAGE_STATS_TEXT, c.matches,
                mode == STAT_TODAY ? playedText(secondsToday)
                : mode == STAT_OVERALL ? (overall < 0 ? "--" : std::to_string(overall))
                : players < 0 ? "..." : std::to_string(players));
    }
}

struct Button { U name; unsigned handle; unsigned short reasons; };
// Button masks only change with the page, the search state, the tablet being ready and the
// set of live buttons, so the (long) walk over the button pool only runs when one changes.
void gate(Context& c, void* cs) {
    auto resource = at<void*>(cs, 0xd0);
    if (!resource) return;
    auto rows = at<P>(resource, 0);
    U count = at<U>(resource, 0x30);
    unsigned n = at<unsigned short>(cs, 0x10a);
    bool ready = c.views[0].p && c.views[1].p && c.views[2].p;
    U key = U(c.page == Page::Queue) | U(matchmaking(c.state)) << 1 | U(ready) << 2 | U(tabletFault.load()) << 3 | U(n) << 8 | count << 24;
    if (key == c.gateKey) return;
    c.gateKey = key;
    if (count > 8192 || n > count) { failTablet("unexpected native button pool layout"); return; }
    std::vector<Button> buttons;
    auto instances = at<P>(cs, 0x110), inverse = at<P>(cs, 0xf0), handles = at<P>(cs, 0xe0), masks = at<P>(cs, 0xf8);
    for (unsigned i = 0; i < n; i++) {
        unsigned row = at<unsigned short>(instances, i * 400);
        if (row >= count) { failTablet("invalid button row"); return; }
        U name = at<U>(rows, row * 296);
        if (!isOurs(name) && !isContent(name)) continue;
        if (isOurs(name) && at<U>(rows, row * 296 + 8) != TABLET_ACTOR) continue;
        unsigned slot = at<unsigned short>(inverse, i * 2);
        if (slot >= count) { failTablet("invalid button handle slot"); return; }
        unsigned h = slot | (unsigned(at<unsigned short>(handles, slot * 4 + 2)) << 16);
        buttons.push_back({name, h, at<unsigned short>(masks, i * 2)});
    }
    std::set<unsigned> live;
    for (auto& b : buttons) live.insert(b.handle);
    for (auto it = c.masked.begin(); it != c.masked.end();) it = live.count(*it) ? std::next(it) : c.masked.erase(it);
    for (auto& b : buttons) {
        bool hide;
        if (tabletFault) hide = isOurs(b.name);                 // stock buttons back to normal
        else if (b.name == QUEUE_TAB) hide = !ready;
        else if (b.name == QUEUE_START) hide = c.page != Page::Queue || matchmaking(c.state);
        else if (b.name == QUEUE_CANCEL) hide = c.page != Page::Queue || !matchmaking(c.state);
        else if (b.name == QUEUE_LOBBY) hide = c.page != Page::Queue || matchmaking(c.state);
        else if (b.name == QUEUE_PRIVATE) hide = c.page != Page::Queue || matchmaking(c.state);
        else if (b.name == QUEUE_STATS) hide = c.page != Page::Queue;
        else hide = c.page != Page::Stock;                       // stock page content
        if (hide && !c.masked.count(b.handle)) {
            if (b.reasons & 0x8000) { failTablet("button disable bit 0x8000 already owned by another mod"); return; }
            disableButton(cs, b.handle, 0x8000);
            c.masked.insert(b.handle);
        } else if (!hide && c.masked.count(b.handle)) {
            enableButton(cs, b.handle, 0x8000);
            c.masked.erase(b.handle);
        }
    }
}

U qpcNs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return U(t.QuadPart * 1000000000.0 / f.QuadPart);
}

std::atomic<U> nsHousekeeping{0}, nsGate{0}, nsRender{0};

// Runs for every button system in the game, twice a frame each: everything but the tablet's
// own system returns at once, and the housekeeping runs a few times a second.
void buttonTick(void* cs) {
    U now = GetTickCount64();
    static U housekeptAt = 0, retriedAt = 0;
    if (now - housekeptAt >= 100) {
        U t0 = qpcNs();
        housekeptAt = now;
        if (!pending.empty() && now - retriedAt >= 1000) {  // canvases still waiting for their system (slow checks)
            retriedAt = now;
            std::vector<void*> retry(pending.begin(), pending.end());
            for (auto p : retry) remember(p);
        }
        if (!fault) joinTick();
        fadeTick();
        nsHousekeeping += qpcNs() - t0;
    }
    auto it = contexts.find(at<void*>(cs, 0x80));
    if (it == contexts.end() || !it->second.views[0].p) return;
    U t1 = qpcNs();
    gate(it->second, cs);
    U t2 = qpcNs();
    render(it->second);
    nsGate += t2 - t1;
    nsRender += qpcNs() - t2;
}

// Every 5 s while anything happened: how often the hook ran, EchoQueue's own time in it,
// and how many redraw-causing calls it made.
void perfReport() {
    static U last = 0;
    U now = GetTickCount64();
    if (!last) last = now;
    if (now - last < 5000) return;
    U secs = (now - last) / 1000;
    last = now;
    U s = perf.show.exchange(0), t = perf.text.exchange(0), a = perf.alpha.exchange(0), m = perf.mask.exchange(0);
    U ticks = perf.ticks.exchange(0), ns = perf.ns.exchange(0);
    bool page = false;
    for (auto& [gs, c] : contexts) page |= c.page == Page::Queue;
    U hk = nsHousekeeping.exchange(0), g = nsGate.exchange(0), r = nsRender.exchange(0);
    if (page || s || t || a || m)
        logf("perf: %llu updates/s, EchoQueue %.3f ms/s (housekeeping %.3f, buttons %.3f, page %.3f), per second: show %.1f text %.1f alpha %.1f masks %.1f (PLAY page %s)",
             ticks / secs, ns / 1e6 / secs, hk / 1e6 / secs, g / 1e6 / secs, r / 1e6 / secs,
             double(s) / secs, double(t) / secs, double(a) / secs, double(m) / secs, page ? "open" : "closed");
}

void button(void* cs) {
    if (!tabletFault) {
        std::lock_guard<std::recursive_mutex> lock(tabletMutex);
        U t0 = qpcNs();
        buttonTick(cs);
        perf.ns += qpcNs() - t0;
        perf.ticks++;
        perfReport();
    }
    buttonOriginal(cs);
    if (!tabletFault) {
        std::lock_guard<std::recursive_mutex> lock(tabletMutex);
        U t0 = qpcNs();
        buttonTick(cs);
        perf.ns += qpcNs() - t0;
    }
}

U loaded(void* p, U a, U b, U c) {
    U r = loadedOriginal(p, a, b, c);
    if (r == 0 && !tabletFault) { std::lock_guard<std::recursive_mutex> lock(tabletMutex); remember(p); }
    return r;
}

void unload(void* p, U a, U b, U c) {
    {
        std::lock_guard<std::recursive_mutex> lock(tabletMutex);
        pending.erase(p);
        auto it = canvases.find(p);
        if (it != canvases.end()) {
            auto& ctx = contexts[it->second.first];
            unsigned kind = it->second.second;
            if (ctx.views[kind].p == p) ctx.views[kind] = View{};
            ctx.applied = -1;  // re-apply onto whatever loads next
            ctx.gateKey = ~0ull;
            if (kind == 0) ctx.page = Page::Stock;
            if (kind == 1) ctx.navState = -1;
            if (kind == 2) {
                ctx.status.clear(), ctx.timer.clear(), ctx.timerCaption.clear(), ctx.statCaption.clear(), ctx.matches.clear();
                ctx.searchShown = -1;
            }
            canvases.erase(it);
        }
    }
    unloadOriginal(p, a, b, c);
}

void onEvent(void* gs, U event, U actor, U component) {
    if (!isOurs(component) && !isStockTab(component)) return;
    auto it = contexts.find(gs);
    if (it == contexts.end() || !it->second.views[0].p) return;
    auto& c = it->second;
    std::array<U, 3> key = {reinterpret_cast<U>(gs), actor, component};
    if (event == RELEASE) { held.erase(key); return; }
    if (!held.insert(key).second) return;  // still held: one press, one action
    if (isStockTab(component)) {
        if (c.page == Page::Queue) logf("tablet: left the PLAY page");
        c.page = Page::Stock;
    } else if (component == QUEUE_TAB) {
        if (c.page != Page::Queue) { logf("tablet: PLAY page selected"); playSound(SOUND_CLICK); }
        c.page = Page::Queue;
    } else if (component == QUEUE_START && c.page == Page::Queue) {
        logf("tablet: FIND MATCH pressed (thread %lu)", GetCurrentThreadId());
        playSound(SOUND_CLICK);
        c.flash = "STARTING THE SEARCH...";
        c.flashUntil = GetTickCount64() + 3000;
        lastResult = int(Queued::None);
        if (!fault) requestQueue("FIND MATCH");
    } else if (component == QUEUE_CANCEL && c.page == Page::Queue) {
        logf("tablet: CANCEL SEARCH pressed");
        playSound(SOUND_CLICK);
        if (!fault && window) PostMessageW(window, WM_CANCEL, 0, 0);
    } else if (component == QUEUE_STATS && c.page == Page::Queue) {
        int mode = (statMode + 1) % STAT_MODES;
        statMode = mode;
        playSound(SOUND_CLICK);
        WritePrivateProfileStringW(L"stats", L"show", std::to_wstring(mode).c_str(), iniPath().c_str());
        logf("tablet: stats box shows %s", STAT_CAPTIONS[mode]);
    } else if (component == QUEUE_LOBBY && c.page == Page::Queue) {
        logf("tablet: LOBBY pressed (%s)", inMatch ? "in a match" : "not in a match");
        playSound(SOUND_CLICK);
        c.flash = inMatch ? "HEADING TO THE LOBBY..." : "YOU'RE ALREADY IN THE LOBBY";
        c.flashUntil = GetTickCount64() + 3000;
        if (inMatch && !fault) startFade(WM_RETURN, "the lobby");
    } else if (component == QUEUE_PRIVATE && c.page == Page::Queue) {
        logf("tablet: PRIVATE MATCH pressed");
        playSound(SOUND_CLICK);
        c.flash = "CREATING A PRIVATE MATCH...";
        c.flashUntil = GetTickCount64() + 3000;
        lastResult = int(Queued::None);
        if (!fault) requestQueue("PRIVATE MATCH", true);
    }
}

void dispatch(void* gs, U event, U actor, U component, int arg) {
    if (!tabletFault && (event == PRESS || event == RELEASE)) {
        std::lock_guard<std::recursive_mutex> lock(tabletMutex);
        onEvent(gs, event, actor, component);
    }
    dispatchOriginal(gs, event, actor, component, arg);
}

// ---- hotkey ----
LRESULT CALLBACK windowDetour(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_QUEUE || message == WM_PRIVATE) {
        Queued r = fault ? Queued::NotReady : queueNow(message == WM_PRIVATE);
        lastResult = int(r);
        lastResultAt = GetTickCount64();
        return 0;
    }
    if (message == WM_PRIVATE_NEXT) {
        size_t slot = size_t(wparam);
        void* netgame = netGame();
        int state = lobbyState();
        if (fault || privateTry != -2 || !netgame || matchmaking(state)) return 0;  // cancelled, or something else started
        if (slot < privatePorts.size()) {
            if (createOnServer(netgame, int(slot))) { ourSearch = ourPrivate = true; return 0; }
        } else {
            logf("private: every slot on %s is busy; letting the server choose", privateServer.c_str());
            FindParams p = privateRequest();
            fn<CreateMatch>(exe, RVA_CREATE_MATCH)(netgame, &p);
            ourSearch = ourPrivate = true;
        }
        privateTry = -1;
        return 0;
    }
    if (message == WM_CANCEL) {
        stopFade("search cancelled");
        if (!fault) cancelNow("CANCEL SEARCH", true);
        return 0;
    }
    // Closing Echo mid-search used to leave the server thinking we were still queued
    // (friends saw MATCHMAKING): cancel first.
    if ((message == WM_CLOSE || message == WM_DESTROY) && !fault && matchmaking(lobbyState()))
        cancelNow("Echo is closing");
    if (message == WM_JOIN) {
        void* netgame = netGame();
        int state = lobbyState();
        if (fault || !netgame || state != LOBBY_JOIN_QUEUED) logf("join: not joining (lobby state %d)", state);
        else { logf("join: joining the found match"); fn<JoinFound>(exe, RVA_JOIN_FOUND)(netgame); }
        return 0;
    }
    if (message == WM_RETURN) {
        void* netgame = netGame();
        if (fault || !netgame || at<int>(netgame, NETGAME_STATE) != NETGAME_IN_GAME || !inMatch) logf("lobby: not in a match; not leaving");
        else { logf("lobby: returning to the lobby"); fn<ReturnToLobby>(exe, RVA_RETURN_TO_LOBBY)(netgame); }
        return 0;
    }
    if (!fault && message == WM_KEYDOWN && int(wparam) == hotkey && !(lparam & (1 << 30))) {  // not auto-repeat
        logf("hotkey pressed");
        queueNow();
    }
    return CallWindowProcW(windowOriginal, hwnd, message, wparam, lparam);
}

BOOL CALLBACK findWindow(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    window = hwnd;
    return FALSE;
}

void attachWindow() {
    for (int i = 0; i < 1200 && !window; i++) {  // up to 10 minutes for Echo to open its window
        EnumWindows(findWindow, 0);
        if (!window) Sleep(500);
    }
    if (!window) { logf("Echo's window never appeared; the hotkey is off"); return; }
    windowOriginal = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(windowDetour)));
    wchar_t title[128] = L"";
    GetWindowTextW(window, title, 128);
    if (!windowOriginal) logf("couldn't listen to Echo's window (error %lu); the hotkey is off", GetLastError());
    else logf("listening for F%d on window \"%ls\" (thread %lu)", hotkey - VK_F1 + 1, title, GetWindowThreadProcessId(window, nullptr));
}

// ---- setup ----
bool hook(unsigned rva, void* detour, void** original) {
    MH_STATUS s = MH_CreateHook(exe + rva, detour, original);
    if (s == MH_OK) s = MH_EnableHook(exe + rva);
    if (s != MH_OK) { logf("hook at %x failed: %s", rva, MH_StatusToString(s)); return false; }
    return true;
}

bool verifyExecutable() {
    exe = reinterpret_cast<P>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(exe + at<unsigned>(exe, 0x3c));
    if (nt->OptionalHeader.SizeOfImage != EXE_IMAGE_SIZE || nt->FileHeader.TimeDateStamp != EXE_TIMESTAMP) {
        logf("unsupported echovr.exe (timestamp %lu, size %lu)", nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
        return false;
    }
    // A hook another mod already placed starts with a jump; MinHook chains onto it, so only
    // check the bytes when they are still the game's own.
    for (auto& s : SIGNATURES)
        if (exe[s.rva] != 0xe9 && memcmp(exe + s.rva, s.bytes, 16) != 0) {
            logf("code signature mismatch at %x (another mod patched it?)", s.rva);
            return false;
        }
    return true;
}

void installTablet() {
    if (!loadLayout()) { failTablet("no tablet layout"); return; }
    showFn = fn<Show>(exe, RVA_SHOW);
    textFn = fn<Text>(exe, RVA_TEXT);
    alphaFn = fn<Alpha>(exe, RVA_ALPHA);
    enableFn = fn<Mask>(exe, RVA_BUTTON_ENABLE);
    disableFn = fn<Mask>(exe, RVA_BUTTON_DISABLE);
    bool ok = hook(RVA_LOADED, reinterpret_cast<void*>(loaded), reinterpret_cast<void**>(&loadedOriginal)) &&
              hook(RVA_UNLOAD, reinterpret_cast<void*>(unload), reinterpret_cast<void**>(&unloadOriginal)) &&
              hook(RVA_DISPATCH, reinterpret_cast<void*>(dispatch), reinterpret_cast<void**>(&dispatchOriginal)) &&
              hook(RVA_BUTTON_UPDATE, reinterpret_cast<void*>(button), reinterpret_cast<void**>(&buttonOriginal));
    if (!ok) { failTablet("hooks failed"); return; }
    logf("tablet hooks installed (PLAY tab)");
}

void initialize() {
    logf("EchoQueue v3.9 starting (pid %lu)", GetCurrentProcessId());
    if (!verifyExecutable()) { fault = tabletFault = true; logf("EchoQueue disabled"); return; }
    loadSettings();
    loadToday();
    voiceJoiningMatch = uint32_t(readHex(L"sounds", L"joining_match", 0));
    if (!voiceJoiningMatch) logf("settings: \"Joining Match\" not learnt yet (find one match from the menu or a terminal)");
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) { logf("MinHook init failed"); fault = tabletFault = true; return; }
    if (!hook(RVA_FIND_MATCH, reinterpret_cast<void*>(findDetour), reinterpret_cast<void**>(&findOriginal)) ||
        !hook(RVA_POST_EVENT, reinterpret_cast<void*>(postEventDetour), reinterpret_cast<void**>(&postEventOriginal))) { fault = tabletFault = true; return; }
    logf("matchmaking hooks installed");
    if (HANDLE t = CreateThread(nullptr, 0, shimWatch, nullptr, 0, nullptr)) CloseHandle(t);
    else shimWatchDone = true;
    installTablet();
    attachWindow();
    if (HANDLE t = CreateThread(nullptr, 0, matchWatch, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, playersWatch, nullptr, 0, nullptr)) CloseHandle(t);
}

void start(HMODULE self) {
    static std::once_flag once;
    std::call_once(once, [self] {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(self, path, MAX_PATH);
        dir = path;
        dir.resize(dir.find_last_of(L"\\/"));
        HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD { initialize(); return 0; }, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    });
}

}  // namespace

extern "C" __declspec(dllexport) void PluginInit(const char*) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&PluginInit), &self);
    start(self);
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        HMODULE pinned = nullptr;  // never unload while hooks point into us
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
        start(self);
    }
    return TRUE;
}
