// EchoLazyTablet.dll -- EchoLoader plugin: queue for an Echo Arena match from the hand tablet.
//
//   * PLAY tab on the hand tablet (data built by tools/build_queue_tab.py): its START ARENA
//     button asks for a public Echo Arena match, and the page shows the matchmaking state;
//   * LOBBY on that page: leaves the match you're in for the social lobby;
//   * PRIVATE MATCH: creates a private Echo Arena match on the player's lowest-ping server;
//   * F9 (Echo's window focused) does the same as START;
//   * logs every matchmaking request the game makes, and saves the settings of a public
//     Echo Arena request to EchoLazyTablet.ini so ours match the game's exactly.
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
#include <winsock2.h>
#include <ws2tcpip.h>
#include "../vendor/minhook/include/MinHook.h"
#include "generated/queue_tab.h"
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
// D3D12 methods returning structs (GetCPUDescriptorHandleForHeapStart) use a hidden return
// pointer in Microsoft's ABI; this makes mingw's d3d12.h declare them that way.
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include <d3d12.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <clocale>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
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
constexpr U ECHO_ARENA = 0xcb60a4de7e1caf73, ECHO_COMBAT = 0x3d5c3976578a321a, NONE = ~0ull;  // echo_arena, echo_combat
constexpr U ECHO_ARENA_PRIVATE = 0x09990965f4db8c03, MPL_ARENA_A = 0x576ed3f8428ebc4b;  // echo_arena_private, its arena
// Sounds: Wwise is linked into echovr.exe, which exports AK::SoundEngine::PostEvent(eventID,
// gameObject, ...) at 0x138f240. Stock tablet buttons get their sounds from the tablet's
// scripts, ours have none, so EchoLazyTablet posts the same Wwise events on the tablet's sound
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
// Echo Arcade's ARCADE and SETTINGS tabs (kotorvr/echo-arcade, native/generated/arcade_tab.h):
// pressing one opens its page over ours. Other mods' tabs come from EchoLazyTablet.layout's other_tabs.
constexpr U ECHO_ARCADE_TABS[] = {0xc5a79e9d89b4bc0f, 0x51a014bc63ecbedb};
// The disable bit EchoLazyTablet sets on buttons. Echo Arcade (and Doom-on-EchoVR) own 0x8000, so
// sharing it made whichever mod came second find it "taken" and switch itself off.
constexpr unsigned short MASK_BIT = 0x4000;
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
// Also EchoLazyTablet's own time in the per-frame and per-sound hooks.
struct Perf { std::atomic<U> show{0}, text{0}, alpha{0}, mask{0}, ticks{0}, ns{0}, hookNs{0}, frames{0}, soundNs{0}, sounds{0}; } perf;
U qpcNs();
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

// Whether the match we're in is a public one, where queueing again would walk out on your
// team: no FIND MATCH or PRIVATE MATCH there (private matches are fine). Echo's API says so
// when it answers (matchPublic, matchWatch); without it, what the last search that turned into
// a join asked for (loadedKind): a public Echo Arena search counts as public. A load no search
// led to (back to the social lobby) is unknown, which doesn't block.
enum { KIND_UNKNOWN, KIND_PUBLIC, KIND_OTHER };
std::atomic<int> matchPublic{-1};     // API: 1 public match, 0 private match or lobby, -1 no answer
std::atomic<int> searchKind{KIND_UNKNOWN}, loadedKind{KIND_UNKNOWN};
std::atomic<U> matchmakingSeenAt{0};  // when the lobby was last seen searching / joining

// EchoLazyTablet turned off for this Echo account by its author (the access service, accessWatch):
// no searches, private matches or LOBBY.
std::atomic<bool> accessBlocked{false};

// What the stats box shows; tapping it steps to the next ([stats] show in EchoLazyTablet.ini).
enum { STAT_TODAY, STAT_OVERALL, STAT_PLAYERS, STAT_MODES };
constexpr const char* STAT_CAPTIONS[STAT_MODES] = {"HOURS TODAY", "MATCHES OVERALL", "PLAYERS ONLINE"};
std::atomic<int> statMode{STAT_TODAY};
// The clock in the title strip: 24-hour, or 12-hour with AM / PM ([clock] format=12); its switch flips it.
std::atomic<bool> clock12{false};
constexpr unsigned SOUND_PADS = sizeof QUEUE_SOUND_BUTTONS / sizeof QUEUE_SOUND_BUTTONS[0];
// The SFX tab's keybinds ([sounds] key1..key8; "" not set) and what their buttons say. Under tabletMutex.
std::array<std::string, SOUND_PADS> soundKeys, soundLabels;
std::array<std::string, SOUND_PADS> soundNames;   // [sounds] name1..name8, or SOUND 1..8
void setSoundKey(unsigned pad, const std::string& spec);
std::atomic<U> fadeDelayMs{FADE_DELAY_MS};  // button to black ([fade] delay_ms)

// Our searches must also be joined (the menu's JoinFoundMatch step); see joinTick.
std::atomic<bool> ourSearch{false};
std::atomic<bool> ourPrivate{false};  // ...and it's a private match we're creating
std::atomic<bool> ourCombat{false};   // ...and it's an Echo Combat search
std::atomic<U> searchSince{0};      // when the current search started (for the page's timer)
std::atomic<U> joinQueuedSince{0};
std::atomic<U> secondsToday{0};      // time played today (see updateToday), for the page
std::atomic<U> lastWaitMs{0};
bool joinSent = false;

// Log lines are written by a thread of their own: opening the log (which antivirus scans on
// every close) on Echo's game or render thread cost frames.
std::condition_variable logWake;
std::deque<std::string> logQueue;

void logWriter() {
    std::wstring path = dir + L"\\EchoLazyTablet.log";
    for (;;) {
        std::deque<std::string> lines;
        {
            std::unique_lock<std::mutex> lock(logMutex);
            logWake.wait(lock, [] { return !logQueue.empty(); });
            lines.swap(logQueue);
        }
        if (FILE* f = _wfopen(path.c_str(), L"a")) {
            for (auto& l : lines) fputs(l.c_str(), f);
            fclose(f);
        }
        Sleep(250);  // gather a burst into one write
    }
}

void logf(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    static std::once_flag started;
    std::call_once(started, [] { std::thread(logWriter).detach(); });
    char stamped[1100];
    snprintf(stamped, sizeof stamped, "%llu %s\n", GetTickCount64(), line);
    {
        std::lock_guard<std::mutex> lock(logMutex);
        if (logQueue.size() < 2000) logQueue.emplace_back(stamped);
    }
    logWake.notify_one();
}

bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION m{};
    if (!p || !VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT) return false;
    if (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return static_cast<const P>(const_cast<void*>(p)) + n <= static_cast<P>(m.BaseAddress) + m.RegionSize;
}

std::wstring iniPath() { return dir + L"\\EchoLazyTablet.ini"; }

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

// With [private] use_server=1, PRIVATE MATCH goes to one game server: [private] server / ports
// in EchoLazyTablet.ini (e.g. Maidenhead, England: 80.64.218.95, five match slots on ports
// 6792..6796). The slot that last worked is tried first ([private] last_port).
std::string privateServer;
uint32_t privateIp = 0;                // network byte order, 0 = let the server choose
std::vector<uint16_t> privatePorts;

// Private matches go where EchoVRCE's matchmaker puts them, which is the game server with the
// lowest ping for the player creating it (from the pings Echo measures to every server while
// matchmaking). [private] use_server=1 pins them to [private] server instead (older EchoLazyTablet
// pinned Maidenhead for everyone, which was far away for players outside the UK).
void loadPrivateServer() {
    wchar_t buffer[128];
    GetPrivateProfileStringW(L"private", L"server", L"", buffer, 128, iniPath().c_str());
    if (!GetPrivateProfileIntW(L"private", L"use_server", 0, iniPath().c_str())) buffer[0] = 0;
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
    if (!privateIp || privatePorts.empty()) { privateIp = 0; logf("settings: private matches go to the game server with your lowest ping (the matchmaker picks it)"); }
    else logf("settings: private matches on %s (%zu port(s), %u first)", privateServer.c_str(), privatePorts.size(), unsigned(privatePorts[0]));
}

std::string iniKeys(const wchar_t* section, const wchar_t* name) {
    wchar_t buffer[64];
    GetPrivateProfileStringW(section, name, L"", buffer, 64, iniPath().c_str());
    std::string spec;
    for (const wchar_t* c = buffer; *c; c++) spec += char(*c);
    return spec;
}

std::wstring soundIniKey(unsigned pad) { return L"key" + std::to_wstring(pad + 1); }

// What an SFX button is called: [sounds] nameN (e.g. the sound's file name), in capitals, without a
// file extension and cut to fit the button; SOUND N when there's none.
std::string soundName(unsigned pad) {
    wchar_t buffer[64];
    GetPrivateProfileStringW(L"sounds", (L"name" + std::to_wstring(pad + 1)).c_str(), L"", buffer, 64, iniPath().c_str());
    std::string name;
    for (const wchar_t* c = buffer; *c; c++) name += *c < 128 ? char(toupper(int(*c))) : '?';
    size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0 && name.size() - dot >= 3 && name.size() - dot <= 5) name.resize(dot);
    name.erase(name.find_last_not_of(" \t") + 1);
    if (name.size() > 16) name = name.substr(0, 15) + ".";
    return name.empty() ? "SOUND " + std::to_string(pad + 1) : name;
}

void loadSettings() {
    for (unsigned i = 0; i < SOUND_PADS; i++) {
        setSoundKey(i, iniKeys(L"sounds", soundIniKey(i).c_str()));
        soundNames[i] = soundName(i);
    }
    wchar_t key[16];
    GetPrivateProfileStringW(L"queue", L"hotkey", L"F9", key, 16, iniPath().c_str());
    hotkey = parseKey(key);
    loadPrivateServer();
    statMode = std::clamp(int(GetPrivateProfileIntW(L"stats", L"show", STAT_TODAY, iniPath().c_str())), 0, STAT_MODES - 1);
    clock12 = GetPrivateProfileIntW(L"clock", L"format", 24, iniPath().c_str()) == 12;
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
        logf("settings: hotkey F%d, using the Echo Arena request saved in EchoLazyTablet.ini", hotkey - VK_F1 + 1);
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
        searchKind = (p->gametype == ECHO_ARENA || p->gametype == ECHO_COMBAT) && p->access == 0 ? KIND_PUBLIC : KIND_OTHER;
        if (p->gametype == ECHO_ARENA && p->access == 0) {
            captured = *p;
            haveCaptured = true;
            writeHex(L"access", p->access); writeHex(L"field1", p->field1); writeHex(L"field3", p->field3);
            writeHex(L"field4", p->field4); writeHex(L"gametype", p->gametype); writeHex(L"level", p->level);
            writeHex(L"group", p->group);
            logf("saved this public Echo Arena request to EchoLazyTablet.ini");
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
    U t0 = qpcNs();
    if ((id == SOUND_CLICK || id == SOUND_TOGGLE_ON || id == SOUND_TOGGLE_OFF) && tabletObject.exchange(object) != object)
        logf("sound: the tablet's sound object is %llx", object);
    if ((id == VOICE_FINDING_MATCH || id == VOICE_MATCHMAKING_CANCELLED) && announcerObject.exchange(object) != object)
        logf("sound: the announcer's sound object is %llx", object);
    else if (object == announcerObject && id != VOICE_FINDING_MATCH && id != VOICE_MATCHMAKING_CANCELLED && !voiceJoiningMatch) {
        if (GetTickCount64() < learnJoiningUntil) learnJoiningMatch(id);
        else { lastAnnouncerEvent = id; lastAnnouncerEventAt = GetTickCount64(); }
    }
    perf.soundNs += qpcNs() - t0;  // ours only, not Wwise's
    perf.sounds++;
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

// In a public match, by the API or else by the search that led here (see matchPublic).
bool inPublicMatch() {
    void* netgame = netGame();
    if (!netgame || at<int>(netgame, NETGAME_STATE) != NETGAME_IN_GAME) return false;
    int api = matchPublic;
    return api == 1 || (api < 0 && loadedKind == KIND_PUBLIC);
}

enum class Queued { Started, AlreadySearching, NotReady, WrongThread, PublicMatch, Blocked, None };

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
// `combat`: a public Echo Combat match instead of Echo Arena.
Queued queueNow(bool createPrivate = false, bool combat = false) {
    DWORD thread = GetCurrentThreadId();
    if (accessBlocked) { logf("queue: EchoLazyTablet is turned off for this account"); return Queued::Blocked; }
    if (gameThread != 0 && gameThread != thread) { logf("queue: thread %lu isn't the game's thread %lu; not queueing (send this log)", thread, DWORD(gameThread)); return Queued::WrongThread; }
    void* netgame = netGame();
    if (!netgame) { logf("queue: the game isn't ready (no net game yet)"); return Queued::NotReady; }
    if (at<uint32_t>(netgame, NETGAME_READY) == 0) { logf("queue: the net game isn't taking requests right now"); return Queued::NotReady; }
    int state = lobbyState();
    if (state < 0) { logf("queue: no client lobby yet"); return Queued::NotReady; }
    if (matchmaking(state)) { logf("queue: already matchmaking (lobby state %d); not starting over", state); return Queued::AlreadySearching; }
    if (inPublicMatch()) { logf("queue: in a public match; not leaving the team for another one"); return Queued::PublicMatch; }
    logf("queue: lobby state %d, thread %lu", state, thread);
    searchKind = createPrivate ? KIND_OTHER : KIND_PUBLIC;
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
        if (combat) p.gametype = ECHO_COMBAT;  // what the menu asks for Combat: the same, but echo_combat
        describe(combat ? "queue: asking for an Echo Combat match" : "queue: asking for a match", netgame, &p);
        findOriginal(netgame, &p);
    }
    ourSearch = true;
    ourPrivate = createPrivate;
    ourCombat = !createPrivate && combat;
    return Queued::Started;
}

constexpr UINT WM_QUEUE = WM_APP + 0x51, WM_JOIN = WM_APP + 0x52, WM_CANCEL = WM_APP + 0x53, WM_RETURN = WM_APP + 0x54,
               WM_PRIVATE = WM_APP + 0x55, WM_PRIVATE_NEXT = WM_APP + 0x56;  // wparam: the slot to try

// Echo's window thread only, like queueNow.
// `announce`: play the announcer's "Matchmaking Cancelled" for a search EchoLazyTablet started (a
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
void requestQueue(const char* why, bool createPrivate = false, bool combat = false) {
    if (!window || !windowOriginal) { logf("queue (%s): Echo's window isn't hooked; can't queue", why); lastResult = int(Queued::NotReady); lastResultAt = GetTickCount64(); return; }
    logf("queue (%s): asking Echo's window thread", why);
    PostMessageW(window, createPrivate ? WM_PRIVATE : WM_QUEUE, combat, 0);
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
bool readFile(const std::wstring& path, std::string& out);

// Live texture switching (texlod) is opt-in, [graphics] live_textures=1: it kept missing views Echo
// made or copied its own way (24 Oct). By default the GRAPHICS buttons switch the game data's
// manifest, which takes a restart and is exact (and loads only the small textures).
bool liveTextures() { return GetPrivateProfileIntW(L"graphics", L"live_textures", 0, iniPath().c_str()) != 0; }

// ---- live texture detail (GRAPHICS LOW / HIGH without a restart) ----
// Every texture Echo draws with is reached through a shader resource view (SRV). Echo makes
// them with the default view (all mips) in its CPU-side descriptor slots and copies them into
// the shader-visible heap for every draw (~30k copies/s; v3.14 probe, 24 Oct). Remade with a
// ResourceMinLODClamp, the same texture is sampled from its small mips only, from the next frame,
// with nothing uploaded or rebuilt: that's LOW. HIGH remakes them as Echo made them. Slots
// are remembered with a reference on their texture, so a slot is never remade for a freed
// texture; the reference goes when Echo writes something else there (texture streaming
// rewrites slots all the time) or at shutdown. The device's methods are taken over by swapping
// their pointers in its vtable (a pointer write, no threads frozen) once Echo's command queue is
// caught. Only views of 2D textures with mips (plain, array or cube: skies) that start at the top
// mip are touched: UI and render targets have no mips.
// Player textures (suits, tints: team colours) stay clearer than the world: the potato tool
// leaves fingerprints of their mips in EchoLazyTablet.players.bin (FNV-1a of a mip's first row
// of blocks, with its size), and each texture upload Echo records (CopyTextureRegion from its
// upload buffer) is checked against them. Echo also copies views into its shader-visible heap
// and keeps drawing from those copies while an object stays in view (after a switch, what was
// in view kept its old detail until a turn round, 24 Oct), so a copy of a remembered slot is
// remembered too, and remade with it. The background effects
// outside the arena and lobby (the blue band, the red streaks: particle textures nothing else
// uses) are blanked in LOW the same way, from EchoLazyTablet.hidden.bin: a null view samples as
// transparent black, which draws nothing for a glowing particle.
namespace texlod {
using CreateSrv = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
constexpr unsigned VT_CREATE_SRV = 18;
CreateSrv createSrvOriginal = nullptr;
std::atomic<bool> installed{false}, deviceHooked{false}, low{false}, released{false};
std::atomic<unsigned> targetSize{4};      // LOW: largest mip side sampled ([graphics] live_texture_size)
std::atomic<unsigned> playerSize{128};    // the same for player textures ([graphics] live_player_size)
std::mutex mutex;
ID3D12Device* device = nullptr;
struct Slot {
    ID3D12Resource* resource;
    unsigned mips, layers;
    U width;
    unsigned height;
    DXGI_FORMAT format;
    bool player, hasView, hidden;
    D3D12_SHADER_RESOURCE_VIEW_DESC view;   // Echo's own view, when it gave one
};
std::unordered_map<SIZE_T, Slot> slots;
std::set<ID3D12Resource*> players;        // textures an upload showed to be a player's
std::set<ID3D12Resource*> hiddenTextures; // ... or a background effect's
std::vector<U> fingerprints, hiddenFingerprints;   // sorted
U made = 0, clampedMade = 0, playerUploads = 0, hiddenUploads = 0;
// Why views weren't taken (diagnostics, per 30 s): no resource, Echo's own view LOW can't clamp,
// not a 2D texture, one mip, drawn into (render target / depth / UAV), multisampled; and
// remembered slots another copy overwrote.
U skipNull = 0, skipView = 0, skipDim = 0, skipOneMip = 0, skipDrawn = 0, skipMsaa = 0, overwrittenByCopy = 0;
int skipShown = 0;

// The view for LOW: the resource's own format and mips, sampling from mip `skip` down.
// Echo's view (or the default one spelt out), sampling from mip `skip` down.
bool clampedView(const Slot& s, D3D12_SHADER_RESOURCE_VIEW_DESC& v) {
    unsigned skip = 0, target = s.player ? playerSize : targetSize;
    while (skip + 1 < s.mips && std::max<U>(s.width >> skip, s.height >> skip) > target) skip++;
    if (!skip) return false;
    if (s.hasView) {
        v = s.view;
    } else {
        v = {};
        v.Format = s.format;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (s.layers > 1) {
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            v.Texture2DArray.MipLevels = UINT(-1);
            v.Texture2DArray.ArraySize = s.layers;
        } else {
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipLevels = UINT(-1);
        }
    }
    switch (v.ViewDimension) {
    case D3D12_SRV_DIMENSION_TEXTURE2D: v.Texture2D.ResourceMinLODClamp = float(skip); break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: v.Texture2DArray.ResourceMinLODClamp = float(skip); break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE: v.TextureCube.ResourceMinLODClamp = float(skip); break;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: v.TextureCubeArray.ResourceMinLODClamp = float(skip); break;
    default: return false;
    }
    return true;
}

void write(SIZE_T slot, const Slot& s) {
    D3D12_SHADER_RESOURCE_VIEW_DESC v;
    if (low && s.hidden) {   // a null view of the same shape: samples as 0
        v = {};
        v.Format = s.hasView ? s.view.Format : s.format;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        createSrvOriginal(device, nullptr, &v, D3D12_CPU_DESCRIPTOR_HANDLE{slot});
        return;
    }
    bool clamp = low && clampedView(s, v);
    createSrvOriginal(device, s.resource, clamp ? &v : s.hasView ? &s.view : nullptr, D3D12_CPU_DESCRIPTOR_HANDLE{slot});
    if (clamp) clampedMade++;
}

using CopySimple = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using CopyMany = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
                                          const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using CreateHeap = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
constexpr unsigned VT_CREATE_HEAP = 14, VT_COPY_MANY = 23, VT_COPY_SIMPLE = 24;
CopySimple copySimpleOriginal = nullptr;
CopyMany copyManyOriginal = nullptr;
CreateHeap createHeapOriginal = nullptr;
std::vector<std::pair<SIZE_T, SIZE_T>> cpuHeaps;   // [start, end) of Echo's CPU-only SRV heaps
std::vector<std::pair<SIZE_T, SIZE_T>> allHeaps;   // and of all its SRV heaps seen made: kept alive (one
                                                   // reference each). Slots in heaps made before the hook are
                                                   // written too: Echo keeps those heaps for the whole run.
UINT increment = 0;
U aliased = 0;


// One descriptor copied from `src` to `dst` (lock held): `dst` now shows what `src` shows.
void copied(SIZE_T dst, SIZE_T src) {
    auto old = slots.find(dst);
    if (old != slots.end()) {
        if (old->second.resource) old->second.resource->Release();
        slots.erase(old);
        if (!slots.count(src)) overwrittenByCopy++;
    }
    auto from = slots.find(src);
    if (from == slots.end() || released) return;
    from->second.resource->AddRef();
    slots[dst] = from->second;
    aliased++;
}

void STDMETHODCALLTYPE copySimple(ID3D12Device* self, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src,
                                  D3D12_DESCRIPTOR_HEAP_TYPE type) {
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !increment) return copySimpleOriginal(self, n, dst, src, type);
    std::lock_guard<std::mutex> lock(mutex);
    copySimpleOriginal(self, n, dst, src, type);
    for (UINT i = 0; i < n; i++) copied(dst.ptr + SIZE_T(i) * increment, src.ptr + SIZE_T(i) * increment);
}

void STDMETHODCALLTYPE copyMany(ID3D12Device* self, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* ds, const UINT* dsz, UINT ns,
                                const D3D12_CPU_DESCRIPTOR_HANDLE* ss, const UINT* ssz, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    bool mine = type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && increment && nd && ns;
    if (!mine) return copyManyOriginal(self, nd, ds, dsz, ns, ss, ssz, type);
    std::lock_guard<std::mutex> lock(mutex);
    copyManyOriginal(self, nd, ds, dsz, ns, ss, ssz, type);
    // Walk both range lists in step, a descriptor at a time (sizes default to 1).
    UINT di = 0, si = 0, dk = 0, sk = 0;
    while (di < nd && si < ns) {
        copied(ds[di].ptr + SIZE_T(dk) * increment, ss[si].ptr + SIZE_T(sk) * increment);
        if (++dk >= (dsz ? dsz[di] : 1)) dk = 0, di++;
        if (++sk >= (ssz ? ssz[si] : 1)) sk = 0, si++;
    }
}

HRESULT STDMETHODCALLTYPE createHeap(ID3D12Device* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID iid, void** heap) {
    HRESULT r = createHeapOriginal(self, desc, iid, heap);
    if (r >= 0 && heap && *heap && desc && desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && iid == __uuidof(ID3D12DescriptorHeap)) {
        auto h = static_cast<ID3D12DescriptorHeap*>(*heap);
        h->AddRef();
        SIZE_T start = h->GetCPUDescriptorHandleForHeapStart().ptr;
        std::lock_guard<std::mutex> lock(mutex);
        if (!increment) increment = self->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        SIZE_T end = start + SIZE_T(desc->NumDescriptors) * increment;
        allHeaps.emplace_back(start, end);
        if (!(desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) cpuHeaps.emplace_back(start, end);
    }
    return r;
}

template <class T> bool swapMethod(void** vt, unsigned index, void* detour, T& original) {
    DWORD old;
    if (!VirtualProtect(&vt[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    original = reinterpret_cast<T>(vt[index]);
    vt[index] = detour;
    VirtualProtect(&vt[index], sizeof(void*), old, &old);
    return true;
}

// Views LOW can clamp: Echo's default view, or its own 2D / array / cube view of all the mips.
bool clampable(const D3D12_SHADER_RESOURCE_VIEW_DESC* d) {
    if (!d) return true;
    switch (d->ViewDimension) {
    case D3D12_SRV_DIMENSION_TEXTURE2D: return !d->Texture2D.MostDetailedMip && d->Texture2D.ResourceMinLODClamp == 0;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: return !d->Texture2DArray.MostDetailedMip && d->Texture2DArray.ResourceMinLODClamp == 0;
    case D3D12_SRV_DIMENSION_TEXTURECUBE: return !d->TextureCube.MostDetailedMip && d->TextureCube.ResourceMinLODClamp == 0;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: return !d->TextureCubeArray.MostDetailedMip && d->TextureCubeArray.ResourceMinLODClamp == 0;
    default: return false;
    }
}

void STDMETHODCALLTYPE createSrv(ID3D12Device* self, ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                 D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    std::lock_guard<std::mutex> lock(mutex);    // also orders Echo's writes against a switch
    auto it = slots.find(dest.ptr);
    if (it != slots.end()) {
        if (it->second.resource) it->second.resource->Release();
        slots.erase(it);
    }
    if (!resource) skipNull++;
    if (resource && !released) {
        D3D12_RESOURCE_DESC r = resource->GetDesc();
        constexpr auto DRAWN = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const char* why = r.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ? (skipDim++, nullptr)
                        : r.MipLevels <= 1 ? (skipOneMip++, nullptr)
                        : r.SampleDesc.Count != 1 ? (skipMsaa++, nullptr)
                        : (r.Flags & DRAWN) ? (skipDrawn++, "drawn into")
                        : !clampable(desc) ? (skipView++, "Echo's own view") : "";
        if (why && *why && skipShown < 40) {
            skipShown++;
            logf("texlod: skipped (%s): %llux%u mips %u fmt %u flags %x layers %u; view dim %d mip %u..%d clamp %.2f", why, U(r.Width), r.Height,
                 r.MipLevels, unsigned(r.Format), unsigned(r.Flags), r.DepthOrArraySize, desc ? int(desc->ViewDimension) : -1,
                 desc ? desc->Texture2D.MostDetailedMip : 0, desc ? int(desc->Texture2D.MipLevels) : -1, desc ? desc->Texture2D.ResourceMinLODClamp : 0.f);
        }
        if (why && !*why) {  // images, not what Echo draws into
            resource->AddRef();
            Slot s{resource, r.MipLevels, r.DepthOrArraySize, r.Width, r.Height, r.Format, players.count(resource) > 0, desc != nullptr,
                   hiddenTextures.count(resource) > 0, {}};
            if (desc) s.view = *desc;
            slots[dest.ptr] = s;
            made++;
            device = self;
            write(dest.ptr, s);
            return;
        }
    }
    createSrvOriginal(self, resource, desc, dest);
}

bool isBc(DXGI_FORMAT f, unsigned& block) {
    if ((f >= 70 && f <= 72) || (f >= 79 && f <= 81)) { block = 8; return true; }    // BC1, BC4
    if ((f >= 73 && f <= 78) || (f >= 82 && f <= 84) || (f >= 94 && f <= 99)) { block = 16; return true; }
    return false;
}

using CopyTexture = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
                                             const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
constexpr unsigned VT_COPY_TEXTURE = 16;
std::vector<std::pair<void**, CopyTexture>> copyOriginals;   // per command-list vtable

// Echo's loader threads: a texture upload. Its source (an upload buffer, CPU-visible) is read
// while the copy is recorded; Echo has filled it by then.
void STDMETHODCALLTYPE copyTexture(ID3D12GraphicsCommandList* list, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
                                   const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    unsigned block = 0;
    if (dst && src && dst->pResource && src->pResource && (!fingerprints.empty() || !hiddenFingerprints.empty()) &&
        dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT && !box) {
        auto& f = src->PlacedFootprint.Footprint;
        if (f.Width >= 32 && f.Height >= 32 && isBc(f.Format, block)) {
            void* data = nullptr;
            D3D12_RANGE none{0, 0};
            if (src->pResource->Map(0, nullptr, &data) >= 0 && data) {
                const unsigned char* p = static_cast<const unsigned char*>(data) + src->PlacedFootprint.Offset;
                unsigned n = std::min(64u, (f.Width + 3) / 4 * block);
                U v = 0xcbf29ce484222325ull;
                bool seen[256] = {};
                unsigned distinct = 0;
                for (unsigned i = 0; i < n; i++) {
                    v = (v ^ p[i]) * 0x100000001b3ull;
                    if (!seen[p[i]]) seen[p[i]] = true, distinct++;
                }
                v ^= U(f.Width) << 40 ^ U(f.Height) << 20;
                src->pResource->Unmap(0, &none);
                if (distinct > 4 && std::binary_search(fingerprints.begin(), fingerprints.end(), v)) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (players.insert(dst->pResource).second) {
                        playerUploads++;
                        for (auto& [slot, s] : slots)
                            if (s.resource == dst->pResource && !s.player) { s.player = true; write(slot, s); }
                    }
                } else if (distinct > 4 && std::binary_search(hiddenFingerprints.begin(), hiddenFingerprints.end(), v)) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (hiddenTextures.insert(dst->pResource).second) {
                        hiddenUploads++;
                        for (auto& [slot, s] : slots)
                            if (s.resource == dst->pResource && !s.hidden) { s.hidden = true; write(slot, s); }
                    }
                }
            }
        }
    }
    void** vt = *reinterpret_cast<void***>(list);
    for (auto& [table, original] : copyOriginals)
        if (table == vt) return original(list, dst, x, y, z, src, box);
    copyOriginals.front().second(list, dst, x, y, z, src, box);   // (not reached: every table patched is listed)
}

// Every command list of a kind shares one method table: a list of our own shows where it is.
void hookCopies(ID3D12Device* d) {
    for (auto type : {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COPY}) {
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        if (d->CreateCommandAllocator(type, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&allocator)) >= 0 &&
            d->CreateCommandList(0, type, allocator, nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list)) >= 0) {
            void** vt = *reinterpret_cast<void***>(list);
            bool known = false;
            for (auto& [table, original] : copyOriginals) known |= table == vt;
            DWORD old;
            if (!known && VirtualProtect(&vt[VT_COPY_TEXTURE], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
                copyOriginals.emplace_back(vt, reinterpret_cast<CopyTexture>(vt[VT_COPY_TEXTURE]));
                vt[VT_COPY_TEXTURE] = reinterpret_cast<void*>(copyTexture);
                VirtualProtect(&vt[VT_COPY_TEXTURE], sizeof(void*), old, &old);
            }
            list->Close();
        }
        if (list) list->Release();
        if (allocator) allocator->Release();
    }
}

void loadFingerprints(const wchar_t* file, std::vector<U>& out) {
    std::string data;
    if (!readFile(dir + L"\\" + file, data)) { logf("texlod: no %ls", file); return; }
    out.resize(data.size() / 8);
    memcpy(out.data(), data.data(), out.size() * 8);
    std::sort(out.begin(), out.end());
}

// Any thread. Remakes every remembered view for the new detail; from the next frame on.
void setLow(bool on) {
    std::lock_guard<std::mutex> lock(mutex);
    low = on;
    if (!deviceHooked || !device) return;
    for (auto& [slot, s] : slots) write(slot, s);
    logf("texlod: %s: %zu texture views remade", on ? "LOW" : "HIGH", slots.size());
}

// ovr_Destroy (shutdown): the references go, so Echo's textures can be freed.
void release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    for (auto& [slot, s] : slots) if (s.resource) s.resource->Release();
    slots.clear();
}

// The device's methods: one vtable for every D3D12 device of the runtime.
bool hookDevice(ID3D12Device* d) {
    if (deviceHooked) return true;
    void** vt = *reinterpret_cast<void***>(d);
    if (!swapMethod(vt, VT_CREATE_SRV, reinterpret_cast<void*>(createSrv), createSrvOriginal)) {
        logf("texlod: couldn't take over the device's CreateShaderResourceView");
        return false;
    }
    if (!swapMethod(vt, VT_CREATE_HEAP, reinterpret_cast<void*>(createHeap), createHeapOriginal) ||
        !swapMethod(vt, VT_COPY_SIMPLE, reinterpret_cast<void*>(copySimple), copySimpleOriginal) ||
        !swapMethod(vt, VT_COPY_MANY, reinterpret_cast<void*>(copyMany), copyManyOriginal))
        logf("texlod: couldn't watch Echo's descriptor copies");
    deviceHooked = true;
    return true;
}

void loadAll() {
    targetSize = std::clamp<unsigned>(GetPrivateProfileIntW(L"graphics", L"live_texture_size", 4, iniPath().c_str()), 1, 4096);
    playerSize = std::clamp<unsigned>(GetPrivateProfileIntW(L"graphics", L"live_player_size", 128, iniPath().c_str()), 1, 4096);
    loadFingerprints(L"EchoLazyTablet.players.bin", fingerprints);
    loadFingerprints(L"EchoLazyTablet.hidden.bin", hiddenFingerprints);
}

// From createChainDetour, once Echo's command queue is known: its uploads (player textures).
void install(ID3D12CommandQueue* queue) {
    if (installed || !liveTextures()) return;
    ID3D12Device* d = nullptr;
    if (queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&d)) < 0 || !d) { logf("texlod: no device"); return; }
    if (hookDevice(d)) {
        if (!fingerprints.empty() || !hiddenFingerprints.empty()) hookCopies(d);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!increment) increment = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        installed = true;
        std::lock_guard<std::mutex> lock(mutex);
        logf("texlod: watching Echo's texture views (LOW: world up to %u px, players %u px; %zu player fingerprints, %zu upload table(s); "
             "%zu SRV heaps seen, %zu views remembered so far)", unsigned(targetSize), unsigned(playerSize), fingerprints.size(),
             copyOriginals.size(), allHeaps.size(), slots.size());
    }
    d->Release();   // Echo holds its own
}

// Every 30 s with the perf line.
void report() {
    if (!deviceHooked) return;
    std::lock_guard<std::mutex> lock(mutex);
    if (made || clampedMade || playerUploads || hiddenUploads || aliased)
        logf("texlod: %llu views made (%llu at LOW detail), %llu copied, %llu player / %llu background textures found in 30 s; "
             "%zu remembered, %zu players, %zu background, %zu CPU heaps",
             made, clampedMade, aliased, playerUploads, hiddenUploads, slots.size(), players.size(), hiddenTextures.size(), cpuHeaps.size());
    if (skipNull || skipView || skipDim || skipOneMip || skipDrawn || skipMsaa || overwrittenByCopy)
        logf("texlod: not taken in 30 s: %llu without a texture, %llu own views, %llu not 2D, %llu one mip, %llu drawn into, %llu multisampled; "
             "%llu remembered slots overwritten by a copy", skipNull, skipView, skipDim, skipOneMip, skipDrawn, skipMsaa, overwrittenByCopy);
    made = clampedMade = playerUploads = hiddenUploads = aliased = 0;
    skipNull = skipView = skipDim = skipOneMip = skipDrawn = skipMsaa = overwrittenByCopy = 0;
}
}  // namespace texlod

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
            texlod::install(queue);
        }
    }
    return createOriginal(session, d3d, desc, chain);
}

// ovr_Destroy: our chains go first (v3.1: "ovrError_LeakingResources, client disconnecting
// with active texture sets" at shutdown). Echo's render thread; no fade can run after this.
using Destroy = void (*)(void*);
Destroy destroyOriginal = nullptr;
void destroyDetour(void* session) {
    texlod::release();
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
    U t0 = qpcNs();
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
    perf.hookNs += qpcNs() - t0;  // ours, before the runtime's submit
    perf.frames++;
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

namespace remoteclip {
bool hasTarget();
void shareMatch(const std::string& id);
}

// ---- Echo's API ----
// GET /session from Echo's own HTTP API into `body`: the HTTP status (200 in a match, an
// error in the lobby), or 0 when nothing answers.
DWORD apiSession(std::string& body) {
    static HINTERNET session = nullptr, connection = nullptr;
    if (!session) {
        session = WinHttpOpen(L"EchoLazyTablet", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
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

// The true/false value of "key" in a flat JSON body: 1, 0, or -1 when it isn't there.
int jsonBool(const std::string& body, const char* key) {
    size_t i = body.find("\"" + std::string(key) + "\"");
    if (i == std::string::npos) return -1;
    i = body.find_first_not_of(" \t\r\n:", i + strlen(key) + 2);
    if (i == std::string::npos) return -1;
    return body.compare(i, 4, "true") == 0 ? 1 : body.compare(i, 5, "false") == 0 ? 0 : -1;
}

// The string value of "key" in a flat JSON body, or "" (enough for game_status / match_type).
// The number value of "key" in a JSON body (its first one), or -1 when it isn't there.
double jsonNumber(const std::string& body, const char* key) {
    size_t i = body.find("\"" + std::string(key) + "\"");
    if (i == std::string::npos) return -1;
    i = body.find_first_not_of(" \t\r\n:", i + strlen(key) + 2);
    if (i == std::string::npos) return -1;
    char* end = nullptr;
    double v = strtod(body.c_str() + i, &end);
    return end == body.c_str() + i ? -1 : v;
}

// LAST THROW: the speed of the player's last throw (m/s, the API's last_throw total_speed), -1 none yet
// this run. Echo's API only answers in a match, and each ask costs it frame time, so it's only asked
// about once a second while the PLAY page is up there; the last one seen stays on the page.
std::atomic<double> lastThrow{-1};
std::atomic<U> throwWantedAt{0};

std::string jsonString(const std::string& body, const char* key) {
    size_t i = body.find("\"" + std::string(key) + "\"");
    if (i == std::string::npos) return "";
    i = body.find_first_not_of(" \t\r\n:", i + strlen(key) + 2);
    if (i == std::string::npos || body[i] != '"') return "";
    size_t end = body.find('"', i + 1);
    return end == std::string::npos ? "" : body.substr(i + 1, end - i - 1);
}

// ---- HOLO: the hologram chassis mod's arm colours ----
// The hologram chassis mod (echo-modding-tool holo_whole.rs) gives the first-person Series 11 the
// loading screen's hologram arms; their colour is three rgb values in each of its materials' value
// blocks (+0x34, +0xd4, +0x2e4 of a 0x47c-byte block), which the game reads every frame from its
// loaded copies. The mod ships them as the hologram's own blue with every float's lowest bit flipped:
// no other material has those bits, so the loaded copies can be found in memory without shipping
// any game data. A thread finds them (after loads), then keeps them at the colour picked on the HOLO
// page for where the player is: blue team, orange team or anywhere else (the lobby, spectating).
// ARMS SOLID / SEE-THROUGH (defined with the manifest code below): -> what to flash.
std::string switchArmsLook(int look);
int armsLookShown();

namespace holo {
constexpr size_t BLOCK = 0x47c;
constexpr size_t COLOURS[3] = {0x34, 0xd4, 0x2e4};
constexpr uint32_t SIGN_D4[3] = {0x3e405442, 0x3f40c0d7, 0x3f800001};
constexpr uint32_t SIGN_2E4[3] = {0x3dd16460, 0x3f26c287, 0x3f800001};
enum Where { BLUE = 0, ORANGE = 1, LOBBY = 2 };

// What each bar (blue team, orange team, lobby) is set to: any colour (red, green, blue 0-255, as the
// tablet shows it), the hologram's own colour (holo) or a cycling rainbow (at the colour's saturation
// and brightness). `hue` keeps the hue while the colour is grey. Guarded by `lock`.
struct Setting {
    int rgb[3] = {48, 191, 255};   // (the hologram's blue, as near as the tablet shows it)
    float hue = 197;
    bool holo = true, rainbow = false;
};
constexpr float SPEEDS[HOLO_SPEEDS] = {10, 25, 50, 100, 200};   // rainbow: degrees per second
std::mutex lock;
Setting settings[HOLO_BAR_COUNT];
int speed = 2;                   // rainbow speed (SPEEDS index); the rainbow is whole arms (bands
                                 // per piece looked blocky, 12 Nov)
std::atomic<int> editing{0};     // the bar the HOLO page edits
std::atomic<int> where{LOBBY};   // the bar in use
std::atomic<int> found{-1};      // copies being kept (-1 not looked yet)
std::atomic<bool> rescan{false};
// ARMS live (12 Nov test): the arm piece materials Echo is drawing with are loaded whole, header and
// all, so each found copy whose header is ours has its draw group (file @0x18, = copy - 0x190) and
// flags (@0x1c) right there. A look tapped on the tablet is written into them at once (the manifest
// is still swapped for the next start, in case the game only reads them when it loads).
std::atomic<int> liveLook{-1};
// per look (SOLID, SEE-THROUGH, SOLID HANDS, CLEAR HANDS) and piece: group | blend, flags (as
// holo_whole stages them: solid = group 1 with depth bit 0x2 on pieces 0, 3, 5; see-through =
// group 0xc; hands looks hide the forearms and upper arms, pieces 0-3, with group 0xe)
void lookHeader(int look, int piece, uint32_t& group, uint32_t& flags) {
    bool clear = look == 1 || look == 3, hide = (look == 2 || look == 3) && piece < 4;
    group = hide ? 0x000e0001 : clear ? 0x000c0001 : 0x00010001;
    flags = !clear && (piece == 0 || piece == 3 || piece == 5) ? 0xba0e : 0xba0c;
}

struct Block {
    uintptr_t base;
    int piece;                   // the arm piece (0 for a mod without pieces: all the same)
    std::vector<uint8_t> snap;   // the block as found (colours aside): it must still be that to be written
    float orig[3][3];            // its colours as found (the signed blue)
};

bool masked(size_t i) {
    for (size_t c : COLOURS)
        if (i >= c && i < c + 12) return true;
    return false;
}

bool readSelf(uintptr_t at, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(at), out, n, &got) && got == n;
}

// Every signed copy in plain read-write memory (the game's loaded data; its GPU upload buffers are
// write-combined and rebuilt from these each frame, so they're skipped).
std::vector<Block> scan() {
    std::vector<Block> out;
    std::vector<uint8_t> buf(4 << 20);
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t at = 0x10000; VirtualQuery(reinterpret_cast<LPCVOID>(at), &mbi, sizeof mbi); at = uintptr_t(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || mbi.Protect != PAGE_READWRITE) continue;
        uintptr_t start = uintptr_t(mbi.BaseAddress), end = start + mbi.RegionSize;
        for (uintptr_t chunk = start; chunk < end; chunk += buf.size() - 16) {
            size_t n = std::min<uintptr_t>(buf.size(), end - chunk);
            if (n < 12 || !readSelf(chunk, buf.data(), n)) break;
            for (size_t i = 0; i + 12 <= n; i += 4) {
                uint32_t w;
                memcpy(&w, &buf[i], 4);
                if (w != SIGN_D4[0] || memcmp(&buf[i], SIGN_D4, 12) != 0) continue;
                uintptr_t base = chunk + i - COLOURS[1];
                Block b{base, 0, std::vector<uint8_t>(BLOCK), {}};
                uint32_t blue;
                if (!readSelf(base, b.snap.data(), BLOCK) || memcmp(&b.snap[COLOURS[2]], SIGN_2E4, 8) != 0) continue;
                memcpy(&blue, &b.snap[COLOURS[2] + 8], 4);
                if ((blue & ~0xeu) != SIGN_2E4[2] || ((blue >> 1) & 7) > 5) continue;
                b.piece = int((blue >> 1) & 7);
                uint32_t z;
                memcpy(&z, &b.snap[COLOURS[0] + 8], 4);
                if (z != 0x3f800001) continue;
                for (int c = 0; c < 3; c++) memcpy(b.orig[c], &b.snap[COLOURS[c]], 12);
                bool known = false;
                for (auto& o : out) known |= o.base == base;
                if (!known) out.push_back(std::move(b));
            }
            if (n < buf.size()) break;
        }
    }
    return out;
}

// Probe (12 Nov, read-only, once a start): where does the game keep each arm piece material's draw
// group (file header u32 @0x18 = group << 16 | blend)? The installed looks give the pieces groups
// 1 (solid), 0xc (see-through) and 0xe (hidden), all loaded at once. Every pointer into a found copy
// is a runtime object using it; the group-like u32s around each are written to
// EchoLazyTablet.probe.log, with a tally by offset, to find a field the tablet could flip live.
void probe(const std::vector<Block>& found) {
    std::vector<uintptr_t> bases;
    for (auto& b : found) bases.push_back(b.base);
    std::sort(bases.begin(), bases.end());
    FILE* out = _wfopen((dir + L"\\EchoLazyTablet.probe.log").c_str(), L"w");
    if (!out) return;
    U begin = GetTickCount64();
    fprintf(out, "%zu copies, arms this start: %d\n", bases.size(), int(armsLookShown()));
    for (auto& b : found) fprintf(out, "copy %llx piece %d\n", (unsigned long long)b.base, b.piece);
    auto isGroup = [](uint32_t v) { return (v >> 16) >= 1 && (v >> 16) <= 0xf && (v & 0xffff) < 8; };
    std::map<std::pair<int, uint32_t>, int> tally;
    std::vector<uint8_t> buf(4 << 20);
    size_t hits = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t at = 0x10000; VirtualQuery(reinterpret_cast<LPCVOID>(at), &mbi, sizeof mbi); at = uintptr_t(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || mbi.Protect != PAGE_READWRITE) continue;
        uintptr_t start = uintptr_t(mbi.BaseAddress), end = start + mbi.RegionSize;
        // chunks overlap by 0x400 so every pointer has 0x200 either side in some chunk
        for (uintptr_t chunk = start; chunk < end; chunk += buf.size() - 0x400) {
            size_t n = std::min<uintptr_t>(buf.size(), end - chunk);
            if (n < 16 || !readSelf(chunk, buf.data(), n)) break;
            size_t from = chunk == start ? 0 : 0x200, to = n < buf.size() ? n : n - 0x200;
            for (size_t i = from; i + 8 <= to; i += 8) {
                uintptr_t v;
                memcpy(&v, &buf[i], 8);
                if (v < 0x10000) continue;
                auto k = std::upper_bound(bases.begin(), bases.end(), v + 0x200);
                if (k == bases.begin()) continue;
                uintptr_t c = *(k - 1);
                if (v + 0x200 < c || v >= c + BLOCK) continue;
                if (chunk + i >= c && chunk + i < c + BLOCK) continue;   // inside a copy itself
                hits++;
                std::string around;
                for (size_t p = i >= 0x200 ? i - 0x200 : 0; p + 4 <= std::min(n, i + 0x200); p += 4) {
                    uint32_t g;
                    memcpy(&g, &buf[p], 4);
                    if (!isGroup(g)) continue;
                    int off = int(p) - int(i);
                    tally[{off, g}]++;
                    char s[32];
                    snprintf(s, sizeof s, " %c%x:%08x", off < 0 ? '-' : '+', unsigned(off < 0 ? -off : off), g);
                    around += s;
                }
                if (hits <= 400)
                    fprintf(out, "ptr %llx -> copy %llx %+d:%s\n", (unsigned long long)(chunk + i), (unsigned long long)c, int(v - c), around.c_str());
            }
            if (n < buf.size()) break;
        }
    }
    fprintf(out, "%zu pointers, %llu ms\ngroup-like values by offset from the pointer (2+):\n", hits, GetTickCount64() - begin);
    for (auto& [key, count] : tally)
        if (count >= 2) fprintf(out, "  %+d %08x: %d\n", key.first, key.second, count);
    fclose(out);
    logf("holo: probe: %zu pointers into %zu copies (EchoLazyTablet.probe.log)", hits, bases.size());
}

void hsv(float h, float s, float v, float out[3]) {   // h in degrees, s and v 0-1
    h = std::fmod(std::fmod(h, 360.f) + 360.f, 360.f) / 60;
    float x = 1 - std::fabs(std::fmod(h, 2.f) - 1);
    const float table[6][3] = {{1, x, 0}, {x, 1, 0}, {0, 1, x}, {0, x, 1}, {x, 0, 1}, {1, 0, x}};
    for (int k = 0; k < 3; k++) out[k] = v * (1 - s + s * table[std::min(int(h), 5)][k]);
}

// A setting's hue (degrees), saturation and brightness (0-1).
void toHsv(const Setting& t, float& h, float& s, float& v) {
    int hi = std::max({t.rgb[0], t.rgb[1], t.rgb[2]}), lo = std::min({t.rgb[0], t.rgb[1], t.rgb[2]});
    v = hi / 255.f;
    s = hi ? float(hi - lo) / hi : 0;
    h = t.hue;
    if (hi == lo) return;
    float d = float(hi - lo), r = float(t.rgb[0]), g = float(t.rgb[1]), b = float(t.rgb[2]);
    h = hi == t.rgb[0] ? 60 * std::fmod((g - b) / d + 6, 6.f) : hi == t.rgb[1] ? 60 * ((b - r) / d + 2) : 60 * ((r - g) / d + 4);
}

void fromHsv(Setting& t, float h, float s, float v) {
    float rgb[3];
    hsv(h, s, v, rgb);
    for (int k = 0; k < 3; k++) t.rgb[k] = int(std::lround(rgb[k] * 255));
    t.hue = std::fmod(std::fmod(h, 360.f) + 360.f, 360.f);
}

// The same in HSL (what the page's colour field, LIGHT and STRENGTH work in): l 0 black .. 1 white.
void hsl(float h, float s, float l, float out[3]) {
    float v = l + s * std::min(l, 1 - l);
    hsv(h, v > 0 ? 2 * (1 - l / v) : 0, v, out);
}

void toHsl(const Setting& t, float& h, float& s, float& l) {
    float sv, v;
    toHsv(t, h, sv, v);
    l = v * (1 - sv / 2);
    s = l > 0 && l < 1 ? (v - l) / std::min(l, 1 - l) : 0;
}

void fromHsl(Setting& t, float h, float s, float l) {
    float rgb[3];
    hsl(h, s, l, rgb);
    for (int k = 0; k < 3; k++) t.rgb[k] = int(std::lround(rgb[k] * 255));
    t.hue = std::fmod(std::fmod(h, 360.f) + 360.f, 360.f);
}

// The rainbow's hue now (degrees), the same for every bar so a change of side doesn't jump.
float rainbowHue() {   // (the performance counter: GetTickCount64 only moves every 16 ms, which stutters)
    return float(std::fmod(double(qpcNs()) / 1e9 * SPEEDS[speed], 360.0));
}

// The three colours a block gets: the hologram's own (as shipped) or the picked colour made linear
// (the material's values are; the tablet's swatches aren't) with a little of the original's
// paleness mixed in (all of it washed colours out, 11 Nov).
void colours(const Block& b, const Setting& set, float hue, float out[3][3]) {
    float rgb[3], h, sat, light;
    toHsl(set, h, sat, light);
    if (set.rainbow) hsl(hue, sat, light, rgb);
    else for (int k = 0; k < 3; k++) rgb[k] = set.rgb[k] / 255.f;
    float val = std::max({rgb[0], rgb[1], rgb[2]});
    for (int c = 0; c < 3; c++) {
        // a quarter of the original's paleness, less for darker colours (so black is black)
        float white = 0.25f * std::min({b.orig[c][0], b.orig[c][1], b.orig[c][2]}) * val;
        for (int k = 0; k < 3; k++) {
            float lin = std::pow(rgb[k], 2.2f);
            out[c][k] = set.holo && !set.rainbow ? b.orig[c][k] : lin + (1 - lin) * white;
        }
    }
}

// Where the player is, from Echo's API (its own connection: apiSession's belongs to matchWatch).
int askWhere() {
    static HINTERNET session = nullptr, connection = nullptr;
    if (!session) {
        session = WinHttpOpen(L"EchoLazyTablet", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return LOBBY;
        WinHttpSetTimeouts(session, 300, 300, 300, 500);
        connection = WinHttpConnect(session, L"127.0.0.1", API_PORT, 0);
    }
    if (!connection) return LOBBY;
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", L"/session", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) return LOBBY;
    DWORD status = 0, size = sizeof status;
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr) ||
        !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        status = 0;
    std::string body;
    char buffer[4096];
    DWORD read = 0;
    while (status == 200 && WinHttpReadData(request, buffer, sizeof buffer, &read) && read) body.append(buffer, read);
    WinHttpCloseHandle(request);
    if (status != 200 || jsonString(body, "match_type").rfind("Social", 0) == 0) return LOBBY;
    std::string me = jsonString(body, "client_name");
    size_t teams = body.find("\"teams\"");
    if (me.empty() || teams == std::string::npos) return LOBBY;
    // Each team object in turn: the one listing the player by name says which side they're on.
    size_t i = body.find('[', teams);
    while (i != std::string::npos && i < body.size()) {
        size_t open = body.find('{', i), close = open;
        if (open == std::string::npos) break;
        int depth = 0;
        bool quoted = false;
        for (; close < body.size(); close++) {
            char ch = body[close];
            if (quoted) { if (ch == '\\') close++; else if (ch == '"') quoted = false; }
            else if (ch == '"') quoted = true;
            else if (ch == '{') depth++;
            else if (ch == '}' && --depth == 0) break;
        }
        std::string team = body.substr(open, close - open + 1);
        size_t at = team.find("\"" + me + "\"");
        if (at != std::string::npos) {
            std::string name;   // the team's own "team" key (its players' objects don't have one)
            for (size_t k = team.find("\"team\""); k != std::string::npos; k = team.find("\"team\"", k + 1))
                if (std::string v = jsonString(team.substr(k), "team"); !v.empty()) { name = v; break; }
            if (name.find("BLUE") != std::string::npos) return BLUE;
            if (name.find("ORANGE") != std::string::npos) return ORANGE;
            return LOBBY;   // spectating
        }
        i = close + 1;
        size_t next = body.find_first_not_of(" \t\r\n,", i);
        if (next == std::string::npos || body[next] != '{') break;
    }
    return LOBBY;
}

const wchar_t* const KEYS[HOLO_BAR_COUNT] = {L"blue_v4", L"orange_v4", L"lobby_v4"};   // (earlier pages used blue / blue_hue / blue_v3 ...)

void save(unsigned bar) {   // under lock
    const Setting& t = settings[bar];
    wchar_t v[96];
    swprintf(v, 96, L"%d,%d,%d,%d,%d,%d", t.rgb[0], t.rgb[1], t.rgb[2], int(std::lround(t.hue)), int(t.holo), int(t.rainbow));
    WritePrivateProfileStringW(L"holo", KEYS[bar], v, iniPath().c_str());
}

void load() {
    std::lock_guard<std::mutex> g(lock);
    settings[ORANGE].holo = false;
    fromHsv(settings[ORANGE], 25, 1, 1);
    for (unsigned b = 0; b < HOLO_BAR_COUNT; b++) {
        wchar_t v[96];
        GetPrivateProfileStringW(L"holo", KEYS[b], L"", v, 96, iniPath().c_str());
        int x[6];
        if (swscanf(v, L"%d,%d,%d,%d,%d,%d", &x[0], &x[1], &x[2], &x[3], &x[4], &x[5]) != 6) continue;
        Setting& t = settings[b];
        for (int k = 0; k < 3; k++) t.rgb[k] = std::max(0, std::min(255, x[k]));
        t.hue = float(std::max(0, std::min(359, x[3]))), t.holo = x[4] != 0, t.rainbow = x[5] != 0;
    }
    speed = std::max(0, std::min(int(HOLO_SPEEDS) - 1, int(GetPrivateProfileIntW(L"holo", L"rainbow_speed", 2, iniPath().c_str()))));
}

// A HOLO page control was tapped, or a - / + held (HOLO_CONTROLS index): -> what to flash ("" nothing).
std::string press(unsigned control) {
    if (control >= HOLO_LOOK_FIRST && control < HOLO_LOOK_FIRST + HOLO_LOOKS) return switchArmsLook(int(control - HOLO_LOOK_FIRST));
    std::lock_guard<std::mutex> g(lock);
    if (control < HOLO_BAR_COUNT) { editing = int(control); return std::string("EDITING ") + HOLO_BAR_LABELS[control]; }
    unsigned bar = unsigned(editing.load());
    Setting& t = settings[bar];
    std::string what;
    float h, sat, light;
    toHsl(t, h, sat, light);
    if (control == HOLO_RAINBOW) { t.rainbow = !t.rainbow; what = t.rainbow ? "RAINBOW ON" : "RAINBOW OFF"; }
    else if (control == HOLO_RESET) { t = Setting{}; what = "HOLOGRAM BLUE"; }
    else if (control >= HOLO_SPEED_FIRST && control < HOLO_SPEED_FIRST + HOLO_SPEEDS) {
        speed = int(control - HOLO_SPEED_FIRST);
        WritePrivateProfileStringW(L"holo", L"rainbow_speed", std::to_wstring(speed).c_str(), iniPath().c_str());
        return "RAINBOW SPEED " + std::to_string(speed + 1);
    } else if (control >= HOLO_FINE_FIRST && control < HOLO_FINE_FIRST + 2 * HOLO_FINE) {
        // the fine-tune buttons: 3 degrees of COLOUR, 2% of BRIGHTNESS (lightness) or 4% of
        // VIVIDNESS (saturation) a step (from a grey, COLOUR brings the colour back at full
        // vividness, or nothing would change)
        unsigned row = (control - HOLO_FINE_FIRST) / 2;
        int step = (control - HOLO_FINE_FIRST) % 2 ? 1 : -1;
        if (row == 0) {
            h += 3 * step;
            if (sat < .05f) sat = 1;
            if (light < .03f || light > .97f) light = .5f;
            t.rainbow = false;
        } else if (row == 1) light = std::max(0.f, std::min(1.f, light + step * .02f));
        else sat = std::max(0.f, std::min(1.f, sat + step * .04f));
        fromHsl(t, h, sat, light);
        t.holo = false;
    } else if (control >= HOLO_FIELD_FIRST && control < HOLO_FIELD_FIRST + HOLO_FIELD_COLS * HOLO_FIELD_ROWS) {
        // the colour at the touch area's centre (the field is drawn from the same sums)
        unsigned i = control - HOLO_FIELD_FIRST, r = i / HOLO_FIELD_COLS, c = i % HOLO_FIELD_COLS;
        fromHsl(t, (c + .5f) / HOLO_FIELD_COLS * 360, 1, HOLO_FIELD_TOP - HOLO_FIELD_SPAN * (r + .5f) / HOLO_FIELD_ROWS);
        t.holo = t.rainbow = false;
    } else if (control >= HOLO_GREY_FIRST && control < HOLO_GREY_FIRST + HOLO_GREYS) {
        int grey = int(std::lround(255 * (1 - float(control - HOLO_GREY_FIRST) / (HOLO_GREYS - 1))));
        for (int& k : t.rgb) k = grey;   // (the hue stays, for COLOUR + / - from here)
        t.holo = t.rainbow = false;
    } else return "";
    save(bar);
    logf("holo: %s = %d,%d,%d hue %.0f%s%s", HOLO_BAR_LABELS[bar], t.rgb[0], t.rgb[1], t.rgb[2], t.hue, t.holo ? " holo" : "", t.rainbow ? " rainbow" : "");
    return what;
}

// For the page, of the bar being edited: which highlights to show (the ring on the field or grey
// bar nearest the colour), the three value texts, the hex code and the colour (0-1, as the tablet
// shows it; while RAINBOW is on, the rainbow's now).
struct View {
    bool lit[HOLO_CONTROL_COUNT];
    std::string hex;
    float colour[3];
    float fine[2 * HOLO_FINE][3], fineText[2 * HOLO_FINE][3];   // the buttons' fills and words
};

void view(View& out) {
    int look = armsLookShown();   // (outside the lock: it reads no settings)
    std::lock_guard<std::mutex> g(lock);
    int bar = editing;
    const Setting& t = settings[bar];
    for (bool& l : out.lit) l = false;
    out.lit[bar] = true;
    out.lit[HOLO_RAINBOW] = t.rainbow;
    out.lit[HOLO_RESET] = t.holo && !t.rainbow;
    out.lit[HOLO_SPEED_FIRST + speed] = true;
    if (look >= 0) out.lit[HOLO_LOOK_FIRST + look] = true;
    float h, sat, light;
    toHsl(t, h, sat, light);
    if (!t.holo && !t.rainbow) {
        if (sat < .12f) {
            float grey = (t.rgb[0] + t.rgb[1] + t.rgb[2]) / (3 * 255.f);
            out.lit[HOLO_GREY_FIRST + int(std::lround((1 - grey) * (HOLO_GREYS - 1)))] = true;
        } else {
            int c = int(h / 360 * HOLO_FIELD_COLS) % int(HOLO_FIELD_COLS);
            int r = std::max(0, std::min(int(HOLO_FIELD_ROWS) - 1, int(std::floor((HOLO_FIELD_TOP - light) / HOLO_FIELD_SPAN * HOLO_FIELD_ROWS))));
            out.lit[HOLO_FIELD_FIRST + r * HOLO_FIELD_COLS + c] = true;
        }
    }
    float shown = t.rainbow ? rainbowHue() : h;
    // each fine-tune button filled with where a few taps of it lead (one step wouldn't show), its
    // words dark on a light fill; from a grey, COLOUR shows the colour coming back
    {
        float base = t.holo ? 0.f : sat, hs = base < .05f ? 1.f : base, hlight = base < .05f && (light < .03f || light > .97f) ? .5f : light;
        float lead[2 * HOLO_FINE][3] = {{shown - 24, hs, hlight}, {shown + 24, hs, hlight},
                                        {shown, sat, std::max(0.f, light - .15f)}, {shown, sat, std::min(1.f, light + .15f)},
                                        {shown, std::max(0.f, sat - .4f), light}, {shown, std::min(1.f, sat + .4f), light}};
        for (unsigned i = 0; i < 2 * HOLO_FINE; i++) {
            if (t.holo && !t.rainbow) for (int k = 0; k < 3; k++) out.fine[i][k] = .16f + .02f * k;   // HOLOGRAM BLUE: plain buttons
            else hsl(lead[i][0], lead[i][1], lead[i][2], out.fine[i]);
            float luma = .3f * out.fine[i][0] + .59f * out.fine[i][1] + .11f * out.fine[i][2];
            for (int k = 0; k < 3; k++) out.fineText[i][k] = luma > .55f ? .08f : 1.f;
        }
    }
    if (t.rainbow) hsl(shown, sat, light, out.colour);
    else for (int k = 0; k < 3; k++) out.colour[k] = t.rgb[k] / 255.f;
    char hex[16];
    snprintf(hex, sizeof hex, t.rainbow ? "RAINBOW" : t.holo ? "HOLO BLUE" : "#%02X%02X%02X", t.rgb[0], t.rgb[1], t.rgb[2]);
    out.hex = hex;
}

// Copies the scout has found, for the writer to take on.
std::mutex foundLock;
std::vector<Block> fresh;

// The scout: where the player is (Echo's API, every 2 s) and the memory search (slow: seconds), on
// a thread of its own so the writer never waits on it.
DWORD WINAPI scout(LPVOID) {
    U scannedAt = 0, askedAt = 0, whereChangedAt = 0;
    int lastWhere = -1;
    for (;;) {
        Sleep(250);
        U t = GetTickCount64();
        if (t - askedAt >= 2000) {
            askedAt = t;
            int w = askWhere();
            if (w != lastWhere) {
                static const char* names[] = {"blue team", "orange team", "the lobby / spectating"};
                logf("holo: on %s", names[w]);
                where = w, lastWhere = w, whereChangedAt = t;
            }
        }
        // Look again after a load (a change of side means a level went in), now and then while
        // nothing's found, and every two minutes anyway (copies a respawn may add).
        bool due = rescan.exchange(false) || (whereChangedAt && t - whereChangedAt > 4000 && scannedAt < whereChangedAt) ||
                   t - scannedAt > (found <= 0 ? 20000ull : 120000ull);
        if (!due) continue;
        U begin = GetTickCount64();
        auto list = scan();
        scannedAt = GetTickCount64();
        logf("holo: scan took %llu ms, %zu copies", scannedAt - begin, list.size());
        static bool probed = false;
        if (!probed && !list.empty() && GetPrivateProfileIntW(L"holo", L"probe", 0, iniPath().c_str())) {   // [holo] probe=1
            probed = true;
            probe(list);
        }
        std::lock_guard<std::mutex> g(foundLock);
        for (auto& b : list) fresh.push_back(std::move(b));
    }
}

// The writer: keeps every copy at the colour wanted, ~120 times a second while a rainbow is on
// (on a high-resolution timer: Sleep's steps are 16 ms), 10 times a second otherwise. Each copy is
// read whole and written back only while everything but its colours is as first found (freed or
// reused memory is dropped, never written); the three colours go back in one write.
DWORD WINAPI watch(LPVOID) {
    if (HANDLE t = CreateThread(nullptr, 0, scout, nullptr, 0, nullptr)) CloseHandle(t);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    std::vector<Block> blocks;
    std::vector<uint8_t> now(BLOCK);
    bool cycling = false;
    constexpr size_t FROM = COLOURS[0], TO = COLOURS[2] + 12;   // the span holding the three colours
    for (;;) {
        LARGE_INTEGER due;
        due.QuadPart = cycling ? -80000LL : -1000000LL;   // 8 ms / 100 ms (100 ns units, relative)
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 200);
        else Sleep(cycling ? 8 : 100);
        {
            std::lock_guard<std::mutex> g(foundLock);
            for (auto& f : fresh) {
                bool known = false;
                for (auto& b : blocks) known |= b.base == f.base;
                if (!known) blocks.push_back(std::move(f));
            }
            fresh.clear();
        }
        Setting set;
        float hue;
        {
            std::lock_guard<std::mutex> g(lock);
            set = settings[where];
            hue = rainbowHue();
        }
        cycling = set.rainbow;
        for (auto it = blocks.begin(); it != blocks.end();) {
            bool same = readSelf(it->base, now.data(), BLOCK);
            for (size_t i = 0; same && i < BLOCK; i++) same = masked(i) || now[i] == it->snap[i];
            if (!same) { it = blocks.erase(it); continue; }
            if (int look = liveLook; look >= 0) {
                uint32_t head[2], g, f;
                bool ours = readSelf(it->base - 0x190, head, 8) && (head[0] & 0xffff) == 1 &&
                            ((head[0] >> 16) == 1 || (head[0] >> 16) == 0xc || (head[0] >> 16) == 0xe) && (head[1] | 2) == 0xba0e;
                lookHeader(look, it->piece, g, f);
                if (ours && (head[0] != g || head[1] != f)) {
                    uint32_t want[2] = {g, f};
                    SIZE_T wrote = 0;
                    WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(it->base - 0x190), want, 8, &wrote);
                    static int logged = 0;   // (if the game kept putting them back, this would repeat)
                    if (logged++ < 40) logf("holo: arms live: piece %d %08x/%08x -> %08x/%08x", it->piece, head[0], head[1], g, f);
                }
            }
            float want[3][3];
            colours(*it, set, hue, want);
            bool differs = false;
            for (int c = 0; c < 3; c++)
                if (memcmp(&now[COLOURS[c]], want[c], 12) != 0) { memcpy(&now[COLOURS[c]], want[c], 12); differs = true; }
            if (differs) {
                SIZE_T wrote = 0;
                WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(it->base + FROM), &now[FROM], TO - FROM, &wrote);
            }
            ++it;
        }
        found = int(blocks.size());
    }
}
}  // namespace holo

// ---- hours today ----
// Time Echo has been running today, added up across sessions in EchoLazyTablet.ini ([stats] day /
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
        session = WinHttpOpen(L"EchoLazyTablet", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
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

// ---- access ----
// Whether EchoLazyTablet may be used on this Echo account, as its author's access service says
// (server/worker.js). On its own thread: shortly after Echo starts and every 15 minutes it
// sends the account's ID and display name from the saved profile (xplatformid, displayname)
// and gets back blocked or not. The answer is kept in EchoLazyTablet.ini ([access] blocked) for
// when the service can't be reached; with no answer ever, EchoLazyTablet stays on.
constexpr const char* ECHOQUEUE_VERSION = "3.27";
constexpr const wchar_t* ACCESS_HOST = L"echoqueue.dokuyegar.workers.dev";  // the deployed Worker (server/worker.js)
constexpr U ACCESS_EVERY_MS = 15 * 60 * 1000;

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') { out += '\\'; out += char(ch); }
        else if (ch < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", ch); out += b; }
        else out += char(ch);
    }
    return out;
}

// 1 blocked, 0 allowed, -1 no answer.
int checkIn(const std::wstring& host, const std::string& id, const std::string& name) {
    HINTERNET session = WinHttpOpen(L"EchoLazyTablet", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return -1;
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"POST", L"/checkin", nullptr, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    std::string payload = "{\"id\":\"" + jsonEscape(id) + "\",\"name\":\"" + jsonEscape(name) + "\",\"version\":\"" + ECHOQUEUE_VERSION + "\"}";
    DWORD status = 0, size = sizeof status;
    std::string body;
    if (request && WinHttpSendRequest(request, L"Content-Type: application/json\r\n", DWORD(-1), payload.data(), DWORD(payload.size()), DWORD(payload.size()), 0) &&
        WinHttpReceiveResponse(request, nullptr) &&
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) &&
        status == 200) {
        char buffer[1024];
        DWORD read = 0;
        while (body.size() < 4096 && WinHttpReadData(request, buffer, sizeof buffer, &read) && read) body.append(buffer, read);
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    int blocked = jsonBool(body, "blocked");
    if (blocked < 0) logf("access: no answer from %ls (HTTP %lu)", host.c_str(), status);
    return blocked;
}

// The account's ID and display name from the newest saved profile.
bool readIdentity(std::string& id, std::string& name) {
    std::wstring path = profilePath();
    HANDLE f = path.empty() ? INVALID_HANDLE_VALUE
                            : CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string profile;
    char buffer[8192];
    DWORD read = 0;
    while (ReadFile(f, buffer, sizeof buffer, &read, nullptr) && read) profile.append(buffer, read);
    CloseHandle(f);
    id = jsonString(profile, "xplatformid");
    name = jsonString(profile, "displayname");
    return !id.empty() && !name.empty();
}

// A newer EchoLazyTablet on GitHub: on its own thread, shortly after Echo starts and every 6 hours,
// the latest release's tag (vX.Y) is compared with this version. The PLAY page says so when idle.
constexpr const wchar_t* RELEASES_HOST = L"api.github.com";
constexpr const wchar_t* RELEASES_PATH = L"/repos/Tomski889/EchoLazyTablet/releases/latest";
std::mutex updateMutex;
std::string updateVersion;   // the newer version ("3.22"), empty when up to date or unknown

// "3.21" (or "v3.21") as major * 1000 + minor; -1 if it isn't one.
int versionNumber(const std::string& v) {
    int major = 0, minor = 0;
    return sscanf(v.c_str() + (!v.empty() && (v[0] == 'v' || v[0] == 'V')), "%d.%d", &major, &minor) == 2 ? major * 1000 + minor : -1;
}

std::string latestRelease() {
    HINTERNET session = WinHttpOpen(L"EchoLazyTablet", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return {};
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);
    HINTERNET connection = WinHttpConnect(session, RELEASES_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", RELEASES_PATH, nullptr, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    DWORD status = 0, size = sizeof status;
    std::string body;
    if (request && WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", DWORD(-1), nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr) &&
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) &&
        status == 200) {
        char buffer[4096];
        DWORD read = 0;
        while (body.size() < 262144 && WinHttpReadData(request, buffer, sizeof buffer, &read) && read) body.append(buffer, read);
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    std::string tag = jsonString(body, "tag_name");
    if (tag.empty()) logf("update: no answer from GitHub (HTTP %lu)", status);
    return tag;
}

DWORD WINAPI updateWatch(LPVOID) {
    Sleep(30000);
    for (;;) {
        std::string tag = latestRelease();
        if (versionNumber(tag) > versionNumber(ECHOQUEUE_VERSION)) {
            std::string v = tag.substr(tag[0] == 'v' || tag[0] == 'V');
            std::lock_guard<std::mutex> lock(updateMutex);
            if (updateVersion != v) logf("update: EchoLazyTablet %s is out (this is %s)", v.c_str(), ECHOQUEUE_VERSION);
            updateVersion = v;
        }
        Sleep(6 * 60 * 60 * 1000);
    }
}

// The latest release's page in the default browser (on the PC's desktop), off the game's thread.
void openLatestRelease() {
    if (HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            ShellExecuteW(nullptr, L"open", L"https://github.com/Tomski889/EchoLazyTablet/releases/latest", nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }, nullptr, 0, nullptr)) CloseHandle(t);
}

std::string updateAvailable() {
    std::lock_guard<std::mutex> lock(updateMutex);
    return updateVersion;
}

DWORD WINAPI accessWatch(LPVOID) {
    wchar_t buffer[256];
    GetPrivateProfileStringW(L"access", L"server", ACCESS_HOST, buffer, 256, iniPath().c_str());
    std::wstring host = buffer;
    accessBlocked = GetPrivateProfileIntW(L"access", L"blocked", 0, iniPath().c_str()) != 0;
    if (accessBlocked) logf("access: EchoLazyTablet is turned off for this account (last answer from the access service)");
    if (host.empty()) return 0;
    Sleep(20000);  // let Echo log in and save the profile first
    for (;;) {
        std::string id, name;
        if (readIdentity(id, name)) {
            int blocked = checkIn(host, id, name);
            if (blocked >= 0 && accessBlocked.exchange(blocked != 0) != (blocked != 0)) {
                WritePrivateProfileStringW(L"access", L"blocked", blocked ? L"1" : L"0", iniPath().c_str());
                logf("access: EchoLazyTablet is %s for this account", blocked ? "turned OFF" : "on again");
            }
        }
        Sleep(DWORD(ACCESS_EVERY_MS));
    }
}

// Own thread, ten times a second: adds the time played to today's every 10 s, and while the
// net game is "in game" (a match or the social lobby, which the API doesn't report) asks the
// API until it reports a match, which sets inMatch for the LOBBY button.
// Echo answers its API on its own threads, so asking costs it frame time: it's only asked for
// API_ASK_MS after each arrival (a match reports itself within a second or two; no answer that
// long means the social lobby, or API access off), not for as long as the lobby lasts.
constexpr U API_ASK_MS = 20000;

DWORD WINAPI matchWatch(LPVOID) {
    bool counted = false, apiDown = false, apiSeen = false;
    U inGameSince = 0;
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
        // What the coming load is: the last search, if one was going on just before it.
        static int lastState = -1;
        int netState = netgame ? at<int>(netgame, NETGAME_STATE) : -1;
        if (matchmaking(lobbyState())) matchmakingSeenAt = now;
        if (netState == NETGAME_LOADING && lastState != NETGAME_LOADING) {
            loadedKind = now - matchmakingSeenAt < 15000 ? searchKind.load() : KIND_UNKNOWN;
            logf("loading: %s", loadedKind == KIND_PUBLIC ? "a public Echo Arena match" : loadedKind == KIND_OTHER ? "a private match or other mode" : "not from a search (the lobby?)");
        }
        lastState = netState;
        if (netState != NETGAME_IN_GAME) { counted = inMatch = false; matchPublic = -1; inGameSince = 0; continue; }
        if (!inGameSince) inGameSince = now;
        static U throwAskedAt = 0;
        if (now - throwWantedAt < 1500 && now - throwAskedAt >= 1000) {
            throwAskedAt = now;
            if (apiSession(body) == 200) {
                double v = jsonNumber(body, "total_speed");
                if (v > 0 && v != lastThrow) { lastThrow = v; logf("last throw: %.1f m/s", v); }
            }
        }
        static U sharedAt = 0;
        if (now - sharedAt >= 5000 && remoteclip::hasTarget()) {
            sharedAt = now;
            if (apiSession(body) == 200 && jsonString(body, "match_type").rfind("Social", 0) != 0) remoteclip::shareMatch(jsonString(body, "sessionid"));
        }
        if (counted || now - inGameSince > API_ASK_MS) continue;
        DWORD http = apiSession(body);
        if (!http) {  // also normal in the social lobby, so only worth a word if it never answered
            if (!apiDown && !apiSeen) logf("Echo's API (127.0.0.1:%u/session) isn't answering; is API access on in Echo's settings?", unsigned(API_PORT));
            apiDown = true;
            Sleep(900);
            continue;
        }
        apiDown = false;
        apiSeen = true;
        if (http != 200) { matchPublic = 0; Sleep(400); continue; }  // the social lobby: no match to report
        std::string type = jsonString(body, "match_type");
        if (type.empty()) continue;
        int priv = jsonBool(body, "private_match");
        if (priv < 0) priv = type.find("Private") != std::string::npos;
        counted = inMatch = true;
        matchPublic = !priv && type.rfind("Social", 0) != 0;
        logf("in a %s match (%s)", type.c_str(), matchPublic ? "public: no queueing until it's over" : "private");
    }
}

// ---- tablet ----
// Canvas kinds: 0 root, 1 nav, 2 PLAY page, 3-5 the stock pages, 6 the stock title. The stock
// ones are drawn over ours, so while PLAY is up they are faded out with the canvas opacity:
// the game's scripts show/hide them (re-hiding meant a redraw every frame) but leave opacity alone.
// Which canvas is which, and where our elements are in the shared ones, depends on what other
// tablet mods added: the installer measures this PC's tablet and writes it to EchoLazyTablet.layout
// (each canvas: element count, plus an index whose name tells it apart from every other canvas
// with that many elements).
constexpr unsigned KINDS = 7, FIRST_STOCK = 3;
constexpr unsigned CANVAS_OPACITY = 0x358;
// Kind 2 is EchoLazyTablet's overlay canvas (page, title strip, tab icon), shown by the root's spare
// stock element 6 so that no canvas other mods count changes its element count.
struct Layout {
    unsigned counts[KINDS] = {}, markerIndex[KINDS] = {};
    U markers[KINDS] = {};
    unsigned rootSlot = 0;                                      // the root element showing the overlay
    std::vector<unsigned> navStockSelected, navOtherSelected;  // other tabs' highlights, hidden under PLAY
    std::vector<U> otherTabs;                                   // other mods' tab buttons (optional)
    bool holo = false;                                          // the HOLO tab is installed (hologram chassis mod found)
} layout;

std::vector<U> readHexList(const wchar_t* key, const std::wstring& path) {
    wchar_t buffer[1024];
    GetPrivateProfileStringW(L"tablet", key, L"", buffer, 1024, path.c_str());
    std::vector<U> out;
    for (wchar_t* p = buffer; *p;) {
        if (iswxdigit(*p)) out.push_back(wcstoull(p, &p, 16));
        else p++;
    }
    return out;
}

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
    std::wstring path = dir + L"\\EchoLazyTablet.layout";
    if (GetPrivateProfileIntW(L"tablet", L"version", 0, path.c_str()) != QUEUE_LAYOUT_VERSION) {
        logf("tablet: EchoLazyTablet.layout missing, unreadable or from another EchoLazyTablet version -- run the EchoLazyTablet installer's Repair");
        return false;
    }
    const wchar_t* names[KINDS] = {L"root", L"nav", L"overlay", L"lobby", L"rows", L"settings", L"title"};
    for (unsigned k = 0; k < KINDS; k++) {
        wchar_t buffer[64];
        unsigned long long marker = 0;
        GetPrivateProfileStringW(L"tablet", names[k], L"", buffer, 64, path.c_str());
        if (swscanf(buffer, L"%u,%u,%llx", &layout.counts[k], &layout.markerIndex[k], &marker) != 3 ||
            layout.markerIndex[k] >= layout.counts[k]) {
            logf("tablet: EchoLazyTablet.layout has no valid '%ls' entry -- run the installer's Repair", names[k]);
            return false;
        }
        layout.markers[k] = marker;
    }
    auto get = [&](const wchar_t* key) { return unsigned(GetPrivateProfileIntW(L"tablet", key, 0, path.c_str())); };
    layout.rootSlot = get(L"root_slot");
    layout.holo = get(L"holo") != 0;
    layout.navStockSelected = readList(L"nav_stock_selected", path);
    layout.navOtherSelected = readList(L"nav_other_selected", path);
    layout.otherTabs = readHexList(L"other_tabs", path);
    layout.otherTabs.insert(layout.otherTabs.end(), std::begin(ECHO_ARCADE_TABS), std::end(ECHO_ARCADE_TABS));
    if (layout.counts[2] != QUEUE_OVERLAY_ELEMENTS || get(L"overlay_elements") != QUEUE_OVERLAY_ELEMENTS) {
        logf("tablet: EchoLazyTablet.layout is for a different EchoLazyTablet version -- run the installer's Repair");
        return false;
    }
    unsigned navMax = 0;
    for (unsigned i : layout.navStockSelected) navMax = std::max(navMax, i);
    for (unsigned i : layout.navOtherSelected) navMax = std::max(navMax, i);
    if (layout.rootSlot >= layout.counts[0] || navMax >= layout.counts[1]) {
        logf("tablet: EchoLazyTablet.layout points outside the tablet -- run the installer's Repair");
        return false;
    }
    logf("tablet: layout for %u root / %u tab bar elements, %zu other tab(s)' highlights",
         layout.counts[0], layout.counts[1], layout.navOtherSelected.size());
    return true;
}

struct View { void* p = nullptr; bool saved = false; float opacity = 1; };
// Other: another mod's tab (Echo Arcade's) was pressed last; its own plugin draws that page.
enum class Page { Stock, Queue, Sounds, Holo, Other };
struct Context {
    Page page = Page::Stock;
    bool sawOther = false;      // another mod's page has been up (it may leave stock pages faded)
    U stockTab = STOCK_TAB_FIRST;  // the stock tab last pressed
    int navState = -1;          // 0 stock tab highlighted, 1 PLAY
    U navCheckedAt = 0;
    std::array<View, KINDS> views{};
    std::set<unsigned> masked;
    U gateKey = ~0ull;          // button masks last applied for (page, searching, ready, buttons)
    std::string status, timer, timerCaption, statCaption, matches, gfxCaption, playClockText, playAmpmText, playClock24Text, throwText;  // what the page's dynamic text shows
    std::string updateText;     // the newer version (empty: NO UPDATE, red dot)
    int updateShown = -1;       // UPDATE's text and dot set since the overlay loaded
    int searchShown = -1;       // the searching layout (teal card, timer, CANCEL SEARCH) shown
    int gfxShown = -2;          // the graphics choice lit (GFX_*)
    bool setup = false;         // the hotkey keyboard is up instead of the PLAY or SFX page
    int setupSlot = -1;         // what it's picking for: -1 CLIP, otherwise that SFX button
    U setupChangedAt = 0;       // when it opened or closed (a finger still there would press what's underneath)
    bool editing = false;       // SFX page: tapping a button picks its key
    bool ctrl = false, shift = false, alt = false;   // its modifiers, and its key ("" none yet)
    std::string key, titleText, subtitleText, clockText, clockFormatText, keyOtherText;
    bool naming = false;        // after an SFX button's key: typing its name on the same keyboard
    std::string name, keyCtrlText, keyShiftText, keyAltText;
    std::string soundStatus, soundEditText;
    std::string holoStatus, holoHex;
    std::vector<float> holoTints;   // the slider and preview colours last written
    U holoHeld = 0, holoHeldAt = 0, holoRepeatAt = 0, holoActor = 0;   // a - / + being held (repeats): its control + 1
    U holoTintedAt = 0;
    void* gs = nullptr;         // the gamespace this context is for
    std::array<std::string, SOUND_PADS> soundText, soundCaption;
    std::string pickMessage;    // SAVE refused: says why for a moment
    U pickFirstUntil = 0;
    int modsShown = -1;
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
// PLAY pressed while another mod's page was up: that mod only closes its page for a stock tab,
// so EchoLazyTablet sends one (press and release, through the hooked dispatch so every mod sees it)
// from the next button update, outside the lock. Our own handler ignores it.
struct Kick { void* gs = nullptr; U actor = 0, tab = 0; int arg = 0; } kick;
thread_local bool sendingKick = false;

bool isContent(U n) { return std::find(std::begin(CONTENT), std::end(CONTENT), n) != std::end(CONTENT); }
bool isStockTab(U n) { return n >= STOCK_TAB_FIRST && n <= STOCK_TAB_LAST; }
bool isOtherTab(U n) { return std::find(layout.otherTabs.begin(), layout.otherTabs.end(), n) != layout.otherTabs.end(); }
const QueueKey* keyFor(U n) {
    for (auto& k : QUEUE_KEYS)
        if (k.name == n) return &k;
    return nullptr;
}
int soundPad(U n) {
    for (unsigned i = 0; i < SOUND_PADS; i++)
        if (QUEUE_SOUND_BUTTONS[i] == n) return int(i);
    return -1;
}
int holoButton(U n) {
    for (unsigned i = 0; i < HOLO_CONTROL_COUNT; i++)
        if (QUEUE_HOLO_BUTTONS[i] == n) return int(i);
    return -1;
}
bool onOurPage(const Context& c) { return c.page == Page::Queue || c.page == Page::Sounds || c.page == Page::Holo; }
bool isOurs(U n) {
    if (n == QUEUE_HOLO_TAB || holoButton(n) >= 0) return true;
    if (keyFor(n) || n == QUEUE_CLIP_KEY || soundPad(n) >= 0 || n == QUEUE_SOUND_TAB || n == QUEUE_SOUND_EDIT) return true;
    return n == QUEUE_TAB || n == QUEUE_START || n == QUEUE_COMBAT || n == QUEUE_UPDATE || n == QUEUE_CANCEL || n == QUEUE_LOBBY || n == QUEUE_PRIVATE || n == QUEUE_STATS ||
           n == QUEUE_GFX_HIGH || n == QUEUE_GFX_LOW || n == QUEUE_CLIP || n == QUEUE_CLOCK || n == QUEUE_PLAY_CLOCK || n == QUEUE_CLOCK_12 || n == QUEUE_CLOCK_24;
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

// ---- graphics HIGH / LOW ----
// EchoPotato (potato.py apply) leaves two manifests next to the game's own, <manifest>.potato-high
// and .potato-low, and the graphics settings for each in EchoLazyTablet.graphics.ini ([high] and
// [low], key=JSON value). Which manifest is live says which graphics are on. Echo reads both
// files at startup and writes its settings file back from memory when it closes, so a switch
// takes a restart, and the live mode's settings are put back in each time the plugin starts.
enum { GFX_UNKNOWN = -1, GFX_HIGH = 0, GFX_LOW = 1 };
std::atomic<int> gfxLoaded{GFX_UNKNOWN}, gfxChosen{GFX_UNKNOWN};
std::atomic<bool> gfxBusy{false};
std::string gfxMessage;     // the last switch's result (under tabletMutex)
U gfxMessageAt = 0;

std::wstring manifestPath() {
    wchar_t exe[MAX_PATH];   // from echovr.exe, not our own folder: on nEVR the plugin lives in %LOCALAPPDATA%\nevr
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring root = exe;
    for (int i = 0; i < 3; i++) root.resize(root.find_last_of(L"\\/"));  // echovr.exe -> win10 -> bin -> game
    return root + L"\\_data\\5932408047\\rad15\\win10\\manifests\\48037dc70b0ecab2";
}

std::wstring settingsPath() {
    wchar_t local[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(local) + L"\\rad\\loneecho\\settings_mp_v2.json" : std::wstring();
}

bool readFile(const std::wstring& path, std::string& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    out.clear();
    char buffer[65536];
    for (size_t n; (n = fread(buffer, 1, sizeof buffer, f)) > 0;) out.append(buffer, n);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool writeFile(const std::wstring& path, const std::string& data);

// ARMS SOLID / SEE-THROUGH / HANDS ONLY. The game reads the hologram arms' draw flags only at
// startup, so the installer (echo-modding-tool install_flat with _alt/ and _alt_hands/) puts every
// version in the game data and keeps each manifest it patched each way, as <manifest>.holo-solid,
// .holo-clear and .holo-hands (the live one and, with the GRAPHICS switch set up, both potato ones). Choosing copies those over them; the
// live manifest takes the potato one it is a copy of, so GRAPHICS LOW / HIGH keep working.
const wchar_t* const LOOK_SUFFIX[HOLO_LOOKS] = {L".holo-solid", L".holo-clear", L".holo-hands", L".holo-clear-hands"};
const char* const LOOK_NAME[HOLO_LOOKS] = {"SOLID", "SEE-THROUGH", "SOLID HANDS", "CLEAR HANDS"};
std::atomic<int> armsLoaded{-2};   // which one this start is using (-1 not set up; -2 not looked yet)
std::atomic<int> armsChosen{-1};   // what was last chosen (for the next start)

int armsLookOf(const std::wstring& m, const std::string& live) {
    for (const wchar_t* file : {L"", L".potato-high", L".potato-low"})
        for (int k = 0; k < int(HOLO_LOOKS); k++) {
            std::string data;
            if (readFile(m + file + LOOK_SUFFIX[k], data) && data == live) return k;
        }
    return -1;
}

int armsLookShown() {
    if (armsLoaded == -2) {   // once, the first time the page is drawn (the game read the manifest long before)
        std::wstring m = manifestPath();
        std::string live;
        armsLoaded = readFile(m, live) ? armsLookOf(m, live) : -1;
        logf("holo: arms this start: %s", armsLoaded >= 0 ? LOOK_NAME[armsLoaded] : "unknown (installed without the looks)");
    }
    return armsChosen >= 0 ? armsChosen.load() : armsLoaded.load();
}

std::string switchArmsLook(int look) {
    const char* name = LOOK_NAME[look];
    std::wstring m = manifestPath();
    std::string live, wanted;
    if (!readFile(m, live) || !readFile(m + LOOK_SUFFIX[look], wanted)) {
        logf("holo: arms: no %ls manifest (installed without that look)", LOOK_SUFFIX[look]);
        return "REINSTALL THE HOLOGRAM ARMS FIRST";
    }
    armsLookShown();
    std::string next = wanted;   // the live manifest's next contents: its own, or its potato one's
    for (const wchar_t* potato : {L".potato-high", L".potato-low"}) {
        std::string now, want;
        if (!readFile(m + potato, now) || !readFile(m + potato + LOOK_SUFFIX[look], want)) continue;
        if (now == live) next = want;
        if (now != want && !writeFile(m + potato, want)) {
            logf("holo: arms: couldn't write %ls (error %lu)", potato, GetLastError());
            return "COULDN'T SWITCH - SEE ECHOLAZYTABLET.LOG";
        }
    }
    if (next != live && !writeFile(m, next)) {
        logf("holo: arms: couldn't write the manifest (error %lu)", GetLastError());
        return "COULDN'T SWITCH - SEE ECHOLAZYTABLET.LOG";
    }
    armsChosen = look;
    holo::liveLook = look;
    WritePrivateProfileStringW(L"holo", L"arms", LOOK_SUFFIX[look] + 6, iniPath().c_str());
    logf("holo: arms: %s chosen for the next start", name);
    return std::string(name) + (look == armsLoaded ? " - ON" : " - ON NOW IF IT CAN, OR AFTER A RESTART");
}

bool writeFile(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".elt-tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok;
    if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DWORD error = GetLastError();
    DeleteFileW(tmp.c_str());
    SetLastError(error);
    return false;
}

int liveGraphics() {
    std::string live, high, low;
    std::wstring m = manifestPath();
    if (!readFile(m, live) || !readFile(m + L".potato-high", high) || !readFile(m + L".potato-low", low)) return GFX_UNKNOWN;
    return live == low ? GFX_LOW : live == high ? GFX_HIGH : GFX_UNKNOWN;
}

// Sets keys inside the "graphics" object of Echo's settings JSON (its "quality" object
// included) to the given JSON values. -> how many changed, or -1 if there's no such object.
int setGraphicsValues(std::string& json, const std::vector<std::pair<std::string, std::string>>& values) {
    size_t key = json.find("\"graphics\"");
    if (key == std::string::npos) return -1;
    size_t open = json.find_first_not_of(" \t\r\n", key + 10);
    if (open == std::string::npos || json[open] != ':') return -1;
    open = json.find_first_not_of(" \t\r\n", open + 1);
    if (open == std::string::npos || json[open] != '{') return -1;
    size_t end = open;
    for (int depth = 0; end < json.size(); end++) {
        if (json[end] == '{') depth++;
        else if (json[end] == '}' && --depth == 0) break;
    }
    if (end >= json.size()) return -1;
    int changed = 0;
    for (auto& [k, v] : values) {
        std::string quoted = "\"" + k + "\"";
        size_t at = json.find(quoted, open);
        if (at == std::string::npos || at > end) continue;
        size_t colon = json.find_first_not_of(" \t\r\n", at + quoted.size());
        if (colon == std::string::npos || json[colon] != ':') continue;
        size_t v0 = json.find_first_not_of(" \t\r\n", colon + 1), v1 = json.find_first_of(",}\r\n", v0);
        if (v0 == std::string::npos || v1 == std::string::npos || v1 > end) continue;
        while (v1 > v0 && (json[v1 - 1] == ' ' || json[v1 - 1] == '\t')) v1--;
        if (json.compare(v0, v1 - v0, v) == 0) continue;
        end = end + v.size() - (v1 - v0);
        json.replace(v0, v1 - v0, v);
        changed++;
    }
    return changed;
}

bool applyGraphicsSettings(int mode) {
    wchar_t section[8192];
    std::wstring ini = dir + L"\\EchoLazyTablet.graphics.ini";
    DWORD n = GetPrivateProfileSectionW(mode == GFX_LOW ? L"low" : L"high", section, 8192, ini.c_str());
    std::vector<std::pair<std::string, std::string>> values;
    for (const wchar_t* line = section; line < section + n && *line; line += wcslen(line) + 1) {
        std::string s;
        for (const wchar_t* c = line; *c; c++) s += char(*c);   // ASCII keys and values
        size_t eq = s.find('=');
        if (eq != std::string::npos) values.emplace_back(s.substr(0, eq), s.substr(eq + 1));
    }
    std::string json;
    std::wstring path = settingsPath();
    if (values.empty() || path.empty() || !readFile(path, json)) { logf("graphics: no settings to apply (%s)", values.empty() ? "EchoLazyTablet.graphics.ini" : "settings_mp_v2.json"); return false; }
    int changed = setGraphicsValues(json, values);
    if (changed < 0) { logf("graphics: settings_mp_v2.json has no graphics section"); return false; }
    if (changed && !writeFile(path, json)) { logf("graphics: couldn't write settings_mp_v2.json (error %lu)", GetLastError()); return false; }
    if (changed) logf("graphics: %d %s setting(s) written", changed, mode == GFX_LOW ? "LOW" : "HIGH");
    return true;
}

// The mode (EchoLazyTablet.ini [graphics] mode=low/high) is what the buttons last chose. LOW
// is the textures' small mips (texlod, live) plus Echo's lowest settings (shadows etc., read at
// startup). The potato manifest (tiny textures in the game data) is only left in place if it's
// live already: with full textures loaded, both directions switch without a restart.
int savedMode() {
    wchar_t mode[16];
    GetPrivateProfileStringW(L"graphics", L"mode", L"", mode, 16, iniPath().c_str());
    if (!wcscmp(mode, L"low")) return GFX_LOW;
    if (!wcscmp(mode, L"high")) return GFX_HIGH;
    return liveGraphics() == GFX_LOW ? GFX_LOW : GFX_HIGH;   // before 3.14's live switch: the manifest said
}

// At startup, before Echo reads its settings and makes its textures.
void graphicsStartup() {
    gfxLoaded = liveGraphics();       // GFX_LOW: the potato textures are what's loaded this run
    int mode = liveTextures() || gfxLoaded == GFX_UNKNOWN ? savedMode() : gfxLoaded.load();
    gfxChosen = mode;
    texlod::low = liveTextures() && mode == GFX_LOW;
    logf("graphics: %s%s%s", mode == GFX_LOW ? "LOW" : "HIGH", gfxLoaded == GFX_LOW ? " (potato textures in the game data)" : "",
         liveTextures() ? ", textures switch live" : "");
    applyGraphicsSettings(mode);
}

// Shadows and effects (the settings file) follow at the next start either way: switching those
// live crashed (see below).
std::string switchGraphics(int mode) {
    const char* name = mode == GFX_LOW ? "LOW" : "HIGH";
    WritePrivateProfileStringW(L"graphics", L"mode", mode == GFX_LOW ? L"low" : L"high", iniPath().c_str());
    applyGraphicsSettings(mode);
    if (texlod::deviceHooked) {   // live textures (opt-in): the full textures stay in the game data
        texlod::setLow(mode == GFX_LOW);
        gfxChosen = mode;
        logf("graphics: %s chosen (live)", name);
        return std::string(name) + " GRAPHICS ON";
    }
    int live = liveGraphics();
    if (live == GFX_UNKNOWN) {
        logf("graphics: can't switch: the potato manifests are missing or the game data changed since");
        return "RUN APPLY POTATO TEXTURES AGAIN FIRST";
    }
    if (live != mode) {
        std::string data;
        std::wstring m = manifestPath();
        if (!readFile(m + (mode == GFX_LOW ? L".potato-low" : L".potato-high"), data) || !writeFile(m, data)) {
            logf("graphics: couldn't switch the manifest to %s (error %lu)", name, GetLastError());
            return "COULDN'T SWITCH - SEE ECHOLAZYTABLET.LOG";
        }
    }
    gfxChosen = mode;
    logf("graphics: %s chosen", name);
    return mode == gfxLoaded ? std::string(name) + " GRAPHICS - ALREADY ON" : std::string(name) + " GRAPHICS - RESTART ECHO";
}

// Live switching. Echo applies its graphics settings (game+0x8dd0, the "graphics" object of
// settings_mp_v2.json) with 0xc31870(settings, game, reloadTextures): at startup (0x109209),
// and again when settings change ("### Applying new graphics settings ###"). The "quality"
// object is at settings+0x28 in the order the JSON reader (0xc2e412..) fills it; settings+0x24
// is the preset ("qualitylevel", 0 = custom). Only the quality values switch live: textures come
// from the game data Echo reads once at startup, so those still take a restart.
constexpr unsigned RVA_APPLY_GRAPHICS = 0xc31870, GAME_GRAPHICS = 0x8dd0, GRAPHICS_PRESET = 0x24, GRAPHICS_QUALITY = 0x28;
constexpr unsigned char APPLY_GRAPHICS_PROLOGUE[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
                                                     0x48, 0x89, 0x7c, 0x24, 0x20, 0x55, 0x41, 0x56, 0x41, 0x57};
using ApplyGraphics = void (*)(void* settings, void* game, int reloadTextures);
struct QualityField { const char* key; unsigned offset; bool isFloat; };
const QualityField QUALITY_FIELDS[] = {{"shadowresolution", 0x00, false}, {"cascaderesolution", 0x04, false},
                                       {"cascadedistance", 0x08, true}, {"shadowmsaa", 0x0c, false}, {"lights", 0x18, false},
                                       {"shadows", 0x1c, false}, {"meshes", 0x24, false}, {"fx", 0x2c, false}};
constexpr size_t QUALITY_COUNT = sizeof QUALITY_FIELDS / sizeof QUALITY_FIELDS[0];
using QualityValues = std::array<double, QUALITY_COUNT>;
std::atomic<int> gfxLiveWanted{GFX_UNKNOWN};    // set by the switch thread, done on the game thread
std::atomic<bool> gfxLiveOff{false};            // the check below failed once: restart-only from then on

bool qualityValues(int mode, QualityValues& out) {
    std::wstring ini = dir + L"\\EchoLazyTablet.graphics.ini";
    for (size_t i = 0; i < QUALITY_COUNT; i++) {
        wchar_t key[32], value[32];
        swprintf(key, 32, L"%hs", QUALITY_FIELDS[i].key);
        GetPrivateProfileStringW(mode == GFX_LOW ? L"low" : L"high", key, L"", value, 32, ini.c_str());
        if (!value[0]) return false;
        static _locale_t c = _create_locale(LC_NUMERIC, "C");   // the ini is JSON numbers, whatever Echo's locale
        out[i] = _wcstod_l(value, nullptr, c);
    }
    return true;
}

bool qualityMatches(void* q, const QualityValues& v) {
    for (size_t i = 0; i < QUALITY_COUNT; i++) {
        auto& f = QUALITY_FIELDS[i];
        double now = f.isFloat ? at<float>(q, f.offset) : double(at<unsigned>(q, f.offset));
        if (std::abs(now - v[i]) > 1e-3) return false;
    }
    return true;
}

// On the game thread. Only writes when Echo's settings in memory hold exactly the values of one
// of the two modes (so the layout above is confirmed on this PC), with the custom preset.
bool applyGraphicsLive(int mode) {
    QualityValues want, other;
    void* game = exe ? *reinterpret_cast<void**>(exe + RVA_GAME) : nullptr;
    void* settings = game ? static_cast<P>(game) + GAME_GRAPHICS : nullptr;
    if (!settings || !readable(settings, GRAPHICS_QUALITY + 0x30)) { logf("graphics: live switch: no game object yet"); return false; }
    if (memcmp(exe + RVA_APPLY_GRAPHICS, APPLY_GRAPHICS_PROLOGUE, sizeof APPLY_GRAPHICS_PROLOGUE) != 0) {
        logf("graphics: live switch off: Echo's apply-settings code isn't where expected");
        gfxLiveOff = true;
        return false;
    }
    if (!qualityValues(mode, want) || !qualityValues(mode == GFX_LOW ? GFX_HIGH : GFX_LOW, other)) return false;
    void* q = static_cast<P>(settings) + GRAPHICS_QUALITY;
    if (at<unsigned>(settings, GRAPHICS_PRESET) != 0 || (!qualityMatches(q, want) && !qualityMatches(q, other))) {
        char dump[256] = "";
        for (size_t i = 0; i < QUALITY_COUNT; i++) {
            auto& f = QUALITY_FIELDS[i];
            snprintf(dump + strlen(dump), sizeof dump - strlen(dump), " %s=%g", f.key,
                     f.isFloat ? at<float>(q, f.offset) : double(at<unsigned>(q, f.offset)));
        }
        logf("graphics: live switch off: Echo's settings in memory aren't as expected (preset %u;%s)",
             at<unsigned>(settings, GRAPHICS_PRESET), dump);
        gfxLiveOff = true;
        return false;
    }
    for (size_t i = 0; i < QUALITY_COUNT; i++) {
        auto& f = QUALITY_FIELDS[i];
        if (f.isFloat) at<float>(q, f.offset) = float(want[i]);
        else at<unsigned>(q, f.offset) = unsigned(want[i]);
    }
    fn<ApplyGraphics>(exe, RVA_APPLY_GRAPHICS)(settings, game, 0);
    logf("graphics: %s applied live (shadows, lights, meshes, effects)", mode == GFX_LOW ? "LOW" : "HIGH");
    return true;
}

DWORD WINAPI graphicsSwitch(LPVOID mode) {
    std::string message = switchGraphics(int(reinterpret_cast<intptr_t>(mode)));
    // Live switching is off unless [graphics] live=1: applying settings mid-game makes Echo
    // rebuild its VR swap chains and pipelines, and a second switch 4 s after the first took the
    // GPU device down (24 Oct, "Timed out waiting for GPU to finish").
    bool liveAllowed = GetPrivateProfileIntW(L"graphics", L"live", 0, iniPath().c_str()) != 0;
    if (liveAllowed && gfxChosen == int(reinterpret_cast<intptr_t>(mode)) && !gfxLiveOff) {
        gfxLiveWanted = int(reinterpret_cast<intptr_t>(mode));   // the result message comes from the game thread
    } else {
        std::lock_guard<std::recursive_mutex> lock(tabletMutex);
        gfxMessage = message;
        gfxMessageAt = GetTickCount64();
        gfxBusy = false;
    }
    return 0;
}

// From the button hook (game thread, our lock held).
void graphicsLiveTick() {
    int mode = gfxLiveWanted.exchange(GFX_UNKNOWN);
    if (mode == GFX_UNKNOWN) return;
    const char* name = mode == GFX_LOW ? "LOW" : "HIGH";
    bool live = applyGraphicsLive(mode);
    bool restart = mode != gfxLoaded;    // the textures loaded at startup are the other ones
    gfxMessage = std::string(name) + (live ? (restart ? " GRAPHICS ON - TEXTURES AFTER A RESTART" : " GRAPHICS ON")
                                           : (restart ? " GRAPHICS - RESTART ECHO TO APPLY" : " GRAPHICS - ALREADY ON"));
    gfxMessageAt = GetTickCount64();
    gfxBusy = false;
}

// ---- CLIP and SFX ----
// Presses a keybind ([clip] keys, e.g. ctrl+t for Medal, or [sounds] key1..key8) as if typed, for
// clip recorders and soundboards listening for a global hotkey. Modifiers ctrl/shift/alt/win, then
// one key: a letter or digit, F1..F24, or a name below.
struct Keys { std::vector<WORD> vks; std::string text; };

bool parseKeys(const std::string& spec, Keys& out) {
    static const std::pair<const char*, WORD> NAMED[] = {
        {"ctrl", VK_CONTROL}, {"control", VK_CONTROL}, {"shift", VK_SHIFT}, {"alt", VK_MENU}, {"win", VK_LWIN},
        {"space", VK_SPACE}, {"enter", VK_RETURN}, {"tab", VK_TAB}, {"home", VK_HOME}, {"end", VK_END}, {"insert", VK_INSERT},
        {"delete", VK_DELETE}, {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT}, {"printscreen", VK_SNAPSHOT}, {"pause", VK_PAUSE},
        {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT}};
    out = {};
    size_t start = 0;
    while (start <= spec.size()) {
        size_t plus = spec.find('+', start);
        std::string part = spec.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        part.erase(0, part.find_first_not_of(" \t"));
        part.erase(part.find_last_not_of(" \t") + 1);
        for (auto& ch : part) ch = char(tolower(static_cast<unsigned char>(ch)));
        WORD vk = 0;
        if (part.size() == 1 && isalnum(static_cast<unsigned char>(part[0]))) vk = WORD(toupper(static_cast<unsigned char>(part[0])));
        else if (part.size() >= 2 && part[0] == 'f' && isdigit(static_cast<unsigned char>(part[1]))) {
            int n = atoi(part.c_str() + 1);
            if (n >= 1 && n <= 24) vk = WORD(VK_F1 + n - 1);
        } else if (part.rfind("numpad", 0) == 0 && part.size() == 7 && isdigit(static_cast<unsigned char>(part[6]))) {
            vk = WORD(VK_NUMPAD0 + (part[6] - '0'));
        } else {
            for (auto& [name, code] : NAMED)
                if (part == name) vk = code;
        }
        if (!vk) return false;
        out.vks.push_back(vk);
        if (!out.text.empty()) out.text += "+";
        for (auto ch : part) out.text += char(toupper(static_cast<unsigned char>(ch)));
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return !out.vks.empty();
}

std::string clipKeysSpec() { return iniKeys(L"clip", L"keys"); }

void setSoundKey(unsigned pad, const std::string& spec) {
    Keys keys;
    bool valid = parseKeys(spec, keys);
    soundKeys[pad] = valid ? spec : "";
    soundLabels[pad] = valid ? keys.text : "TAP TO SET";
    if (!valid && !spec.empty()) logf("sounds: can't read [sounds] key%u=%s", pad + 1, spec.c_str());
}

DWORD WINAPI pressKeysThread(LPVOID arg) {
    std::unique_ptr<std::string> spec(static_cast<std::string*>(arg));
    // One at a time: quick taps would otherwise let go of each other's modifiers.
    static std::mutex pressing;
    std::lock_guard<std::mutex> lock(pressing);
    Keys keys;
    if (!parseKeys(*spec, keys)) { logf("keys: can't read %s", spec->c_str()); return 0; }
    std::vector<INPUT> down, up;
    for (WORD vk : keys.vks) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.wScan = WORD(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        down.push_back(in);
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        up.insert(up.begin(), in);   // released in reverse order
    }
    UINT sent = SendInput(UINT(down.size()), down.data(), sizeof(INPUT));
    Sleep(60);   // held a moment: some recorders poll the keyboard
    sent += SendInput(UINT(up.size()), up.data(), sizeof(INPUT));
    logf("keys: pressed %s (%u of %zu key events sent)", keys.text.c_str(), sent, down.size() * 2);
    return 0;
}

void pressKeys(const std::string& spec) {
    auto* copy = new std::string(spec);
    if (HANDLE t = CreateThread(nullptr, 0, pressKeysThread, copy, 0, nullptr)) CloseHandle(t);
    else delete copy;
}

// ---- CLIP on a second PC ----
// One tap on the tablet saves a clip here and on a PC spectating this one: [clip] send_to on this
// PC (the other PC's IP address, or "all" for every PC on the network) sends a small UDP message,
// and [clip] listen=1 on the other PC presses that PC's own [clip] keys when it arrives. Sent three
// times in case Wi-Fi drops one; each tap has its own number, so it only presses once.
// The same link takes a spectator PC along: in a match, send_to also gets the match's id every 5
// seconds, and an Echo started with -spectatorstream and listen=1 restarts itself into that match.
namespace remoteclip {
constexpr unsigned short PORT = 6733;
constexpr char MESSAGE[] = "EchoLazyTablet CLIP ";
constexpr char MATCH[] = "EchoLazyTablet JOIN ";

bool startNetwork() {
    static bool ok = [] { WSADATA data; return WSAStartup(MAKEWORD(2, 2), &data) == 0; }();
    return ok;
}

std::wstring sendTo() {
    wchar_t buffer[64];
    GetPrivateProfileStringW(L"clip", L"send_to", L"", buffer, 64, iniPath().c_str());
    return buffer;
}

// -> how many of `times` went out, or -1 if `target` isn't an address or there's no network.
int sendMessage(const std::wstring& target, const std::string& message, int times) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(PORT);
    bool all = target == L"all";
    if (all) to.sin_addr.s_addr = INADDR_BROADCAST;
    else if (InetPtonW(AF_INET, target.c_str(), &to.sin_addr) != 1) return -1;
    SOCKET s = startNetwork() ? socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) : INVALID_SOCKET;
    if (s == INVALID_SOCKET) return -1;
    BOOL on = TRUE;
    if (all) setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
    int sent = 0;
    for (int i = 0; i < times; i++) {
        if (i) Sleep(40);
        if (sendto(s, message.data(), int(message.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof to) == int(message.size())) sent++;
    }
    closesocket(s);
    return sent;
}

DWORD WINAPI sendThread(LPVOID) {
    std::wstring target = sendTo();
    int sent = sendMessage(target, MESSAGE + std::to_string(GetTickCount64()), 3);
    if (sent < 0) logf("clip: can't send to [clip] send_to=%ls (not an IP address, or no network)", target.c_str());
    else logf("clip: sent to %ls (%d of 3)", target.c_str(), sent);
    return 0;
}

bool hasTarget() { return !sendTo().empty(); }

void shareMatch(const std::string& id) {
    if (id.empty()) return;
    static std::string shared;
    if (id != shared) { shared = id; logf("clip: telling the other PC about match %s", id.c_str()); }
    sendMessage(sendTo(), MATCH + id, 1);
}

// -> true if there's another PC to send to.
bool send() {
    if (sendTo().empty()) return false;
    if (HANDLE t = CreateThread(nullptr, 0, sendThread, nullptr, 0, nullptr)) CloseHandle(t);
    return true;
}

bool isMatchId(const std::string& id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < 36; i++) {
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? id[i] != '-' : !isxdigit(static_cast<unsigned char>(id[i]))) return false;
    }
    return true;
}

// Only an Echo started as a spectator moves, so a headset session on this PC is never pulled away.
void follow(const std::string& id) {
    constexpr wchar_t FLAG[] = L" -lobbyid ";
    constexpr size_t FLAG_LENGTH = 10, ID_LENGTH = 36;
    std::wstring line = GetCommandLineW();
    if (line.find(L"-spectatorstream") == std::wstring::npos || !isMatchId(id)) return;
    std::wstring wide(id.begin(), id.end());
    size_t at = line.find(FLAG);
    if (at != std::wstring::npos) {
        if (_wcsnicmp(line.c_str() + at + FLAG_LENGTH, wide.c_str(), ID_LENGTH) == 0) return;
        line.erase(at, FLAG_LENGTH + ID_LENGTH);
    }
    line += FLAG + wide;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    logf("clip: restarting to spectate match %s", id.c_str());
    if (!CreateProcessW(exe, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        logf("clip: couldn't restart Echo (error %lu)", GetLastError());
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    TerminateProcess(GetCurrentProcess(), 0);
}

DWORD WINAPI listenThread(LPVOID) {
    SOCKET s = startNetwork() ? socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) : INVALID_SOCKET;
    sockaddr_in here{};
    here.sin_family = AF_INET;
    here.sin_port = htons(PORT);
    here.sin_addr.s_addr = INADDR_ANY;
    bool bound = false;
    for (int i = 0; s != INVALID_SOCKET && i < 6 && !bound; i++) {  // the Echo this one replaced may still be closing
        if (i) Sleep(500);
        bound = bind(s, reinterpret_cast<sockaddr*>(&here), sizeof here) == 0;
    }
    if (!bound) {
        logf("clip: can't listen on port %u (error %d)", PORT, WSAGetLastError());
        return 0;
    }
    logf("clip: listening for the other PC on port %u", PORT);
    std::string last;
    for (;;) {
        char buffer[64];
        int n = recv(s, buffer, sizeof buffer, 0);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEMSGSIZE) continue;  // someone else's, too long for us
        if (n == SOCKET_ERROR) { logf("clip: listening stopped (error %d)", WSAGetLastError()); break; }
        std::string message(buffer, n);
        if (message.rfind(MATCH, 0) == 0) { follow(message.substr(sizeof MATCH - 1)); continue; }
        if (message.rfind(MESSAGE, 0) != 0 || message == last) continue;
        last = message;
        std::string spec = clipKeysSpec();
        Keys keys;
        if (!parseKeys(spec, keys)) { logf("clip: from the other PC, but [clip] keys isn't set here"); continue; }
        logf("clip: from the other PC, pressing %s", keys.text.c_str());
        pressKeys(spec);
    }
    closesocket(s);
    return 0;
}

void start() {
    if (!GetPrivateProfileIntW(L"clip", L"listen", 0, iniPath().c_str())) return;
    if (HANDLE t = CreateThread(nullptr, 0, listenThread, nullptr, 0, nullptr)) CloseHandle(t);
}
}  // namespace remoteclip

std::string elapsed(U ms) {
    char b[16];
    snprintf(b, sizeof b, "%llu:%02llu", ms / 60000, ms / 1000 % 60);
    return b;
}

std::string statusFor(int state) {
    U now = GetTickCount64();
    if (state >= 3 && state <= 6) return ourPrivate ? "CREATING A PRIVATE MATCH" : !ourSearch ? "SEARCHING FOR A MATCH" : ourCombat ? "SEARCHING FOR COMBAT" : "SEARCHING FOR ARENA";
    if (state == LOBBY_JOIN_QUEUED) {
        U since = joinQueuedSince;
        if (ourSearch && !ourPrivate && since && now - since < JOIN_DELAY_MS)  // a private match has no countdown
            return "MATCH FOUND - JOINING IN " + std::to_string((JOIN_DELAY_MS - (now - since) + 999) / 1000) + "...";
        return "MATCH FOUND - JOINING...";
    }
    if (state == 8) return "JOINING THE MATCH...";
    if (accessBlocked) return "ECHOLAZYTABLET IS OFF FOR THIS ACCOUNT";
    if (inPublicMatch()) return "PUBLIC MATCH - QUEUE WHEN IT ENDS";
    std::string update = updateAvailable();
    return "PICK ARENA OR COMBAT";
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
    bool active = onOurPage(c) && !tabletFault;
    bool play = active && c.page == Page::Queue, sounds = active && c.page == Page::Sounds, holoPage = active && c.page == Page::Holo;
    U now = GetTickCount64();
    bool changed = c.applied != int(active);
    c.applied = int(active);
    if (now - c.polledAt > 200) { c.state = lobbyState(); c.polledAt = now; }
    for (unsigned i = FIRST_STOCK; i < KINDS; i++) {  // stock pages and title: faded out under PLAY
        auto& v = c.views[i];
        if (!v.p) continue;
        float& opacity = at<float>(v.p, CANVAS_OPACITY);
        if (active) {
            // 0 here is another mod's fade (Echo Arcade's), not the page's own opacity.
            if (!v.saved) { v.opacity = opacity != 0 ? opacity : 1; v.saved = true; }
            if (opacity != 0) alpha(v.p, 0);
        } else if (v.saved) {
            alpha(v.p, v.opacity);
            v.saved = false;
        } else if (c.sawOther && c.page == Page::Stock && opacity == 0) {
            // A stock tab is up but a page is still faded: another mod restored the 0 it saw
            // under PLAY. The game's scripts never fade these pages themselves.
            alpha(v.p, 1);
        }
    }
    // The overlay: our page, the cover over the stock title and our title tab (shown with PLAY),
    // hung on the root's spare element, which must stay shown for our tab icon to be there.
    void* overlay = c.views[2].p;
    if (c.views[0].p) ensure(c.views[0].p, layout.rootSlot, true);
    if (overlay && changed)
        for (unsigned i : {QUEUE_PAGE_COVER, QUEUE_PAGE_HEADER, QUEUE_PAGE_TITLE, QUEUE_PAGE_SUBTITLE})
            ensure(overlay, i, active);
    if (overlay) {   // the PLAY or SFX page or, in its place, the hotkey keyboard
        ensure(overlay, QUEUE_PAGE_SUBTITLE_BOX, active && !c.setup);
        ensure(overlay, QUEUE_PAGE_SUBTITLE_BOX_WIDE, active && c.setup);
        for (unsigned i : {QUEUE_PAGE_TIMER, QUEUE_PAGE_TIMER_CAPTION})   // PLAY's status, in the strip
            ensure(overlay, i, play && !c.setup);
        ensure(overlay, QUEUE_PAGE_PAGE, play && !c.setup);
        ensure(overlay, QUEUE_PAGE_SOUND_PAGE, sounds && !c.setup);
        ensure(overlay, QUEUE_PAGE_HOLO_PAGE, holoPage && !c.setup);
        ensure(overlay, QUEUE_PAGE_KEYBOARD, active && c.setup);
        for (unsigned i : {QUEUE_PAGE_CLOCK, QUEUE_PAGE_CLOCK_FORMAT, QUEUE_PAGE_CLOCK_FORMAT_TEXT})  // (the hotkey needs the room)
            ensure(overlay, i, sounds && !c.setup);
    }
    if (overlay && active) {   // the title strip: PLAY or SFX, or what's being set and the hotkey picked so far
        std::string sub = sounds ? "SOUNDBOARD" : holoPage ? "HOLOGRAM ARM COLOURS" : "MATCHMAKING";
        if (c.setup && now < c.pickFirstUntil) sub = c.pickMessage;
        else if (c.setup && c.naming) sub = c.name.empty() ? "TYPE A NAME, THEN SAVE" : "NAME: " + c.name;
        else if (c.setup && !c.ctrl && !c.shift && !c.alt && c.key.empty()) sub = c.setupSlot < 0 ? "PICK YOUR CLIP APP'S HOTKEY" : "PICK YOUR SOUND'S HOTKEY";
        else if (c.setup)
            sub = "HOTKEY: " + std::string(c.ctrl ? "CTRL + " : "") + (c.shift ? "SHIFT + " : "") + (c.alt ? "ALT + " : "") +
                  (c.key.empty() ? "..." : c.key);
        std::string heading = !c.setup ? (sounds ? "SFX" : holoPage ? "HOLO" : "PLAY") : c.setupSlot < 0 ? "CLIP" : "SFX " + std::to_string(c.setupSlot + 1);
        setText(overlay, QUEUE_PAGE_TITLE, c.titleText, heading);
        setText(overlay, QUEUE_PAGE_SUBTITLE, c.subtitleText, sub);
        if (c.setup) {   // while naming, the bottom row's keys type instead
            setText(overlay, QUEUE_PAGE_KEY_NO_CLIP_APP_TEXT, c.keyOtherText, c.naming ? "NO NAME" : c.setupSlot < 0 ? "NO CLIP APP" : "CLEAR");
            setText(overlay, QUEUE_PAGE_KEY_CTRL_TEXT, c.keyCtrlText, c.naming ? "SPACE" : "CTRL");
            setText(overlay, QUEUE_PAGE_KEY_SHIFT_TEXT, c.keyShiftText, c.naming ? "DELETE" : "SHIFT");
            setText(overlay, QUEUE_PAGE_KEY_ALT_TEXT, c.keyAltText, c.naming ? "CLEAR" : "ALT");
        }
        SYSTEMTIME t;
        GetLocalTime(&t);
        bool twelve = clock12;
        char clock[16];
        if (twelve) snprintf(clock, sizeof clock, "%d:%02d %s", (t.wHour + 11) % 12 + 1, t.wMinute, t.wHour < 12 ? "AM" : "PM");
        else snprintf(clock, sizeof clock, "%02d:%02d", t.wHour, t.wMinute);
        setText(overlay, QUEUE_PAGE_CLOCK, c.clockText, clock);
        if (play) {   // PLAY's own clock: 12-hour, the time and AM / PM beside it; 24-hour, the time centred
            std::string full = clock;
            size_t space = full.find(' ');
            ensure(overlay, QUEUE_PAGE_PLAY_CLOCK, twelve);
            ensure(overlay, QUEUE_PAGE_PLAY_CLOCK_AMPM, twelve);
            ensure(overlay, QUEUE_PAGE_PLAY_CLOCK_24, !twelve);
            if (twelve) {
                setText(overlay, QUEUE_PAGE_PLAY_CLOCK, c.playClockText, full.substr(0, space));
                setText(overlay, QUEUE_PAGE_PLAY_CLOCK_AMPM, c.playAmpmText, space == std::string::npos ? "" : full.substr(space + 1));
            } else
                setText(overlay, QUEUE_PAGE_PLAY_CLOCK_24, c.playClock24Text, full);
            ensure(overlay, QUEUE_PAGE_CLOCK_12, twelve);   // 12H / 24H beside it: the one in use lit
            ensure(overlay, QUEUE_PAGE_CLOCK_24, !twelve);
        }
        setText(overlay, QUEUE_PAGE_CLOCK_FORMAT_TEXT, c.clockFormatText, twelve ? "12H" : "24H");
    }
    if (overlay && active && c.setup) {
        int mods = c.naming ? 0 : c.ctrl | c.shift << 1 | c.alt << 2;
        if (c.modsShown != mods) {
            ensure(overlay, QUEUE_PAGE_MOD_CTRL, mods & 1);
            ensure(overlay, QUEUE_PAGE_MOD_SHIFT, mods & 2);
            ensure(overlay, QUEUE_PAGE_MOD_ALT, mods & 4);
            c.modsShown = mods;
        }
    }
    // Tab highlight: ours while our page is up, otherwise the stock ones (which the game's
    // own scripts keep pointing at the right stock tab). While ours is up, the game may
    // re-show its highlight; re-checked a few times a second, not every frame.
    int nav = play ? 1 : sounds ? 2 : holoPage ? 3 : 0;
    if (overlay && c.views[1].p && (c.navState != nav || (active && now - c.navCheckedAt > 250))) {
        c.navCheckedAt = now;
        ensure(overlay, QUEUE_PAGE_TAB_SELECTED, nav == 1);
        ensure(overlay, QUEUE_PAGE_TAB_ICON, nav != 1);
        ensure(overlay, QUEUE_PAGE_SOUND_TAB_SELECTED, nav == 2);
        ensure(overlay, QUEUE_PAGE_SOUND_TAB_ICON, nav != 2);
        ensure(overlay, QUEUE_PAGE_HOLO_TAB_SELECTED, layout.holo && nav == 3);
        ensure(overlay, QUEUE_PAGE_HOLO_TAB_ICON, layout.holo && nav != 3);
        if (active) {
            for (auto i : layout.navStockSelected) ensure(c.views[1].p, i, false);
            for (auto i : layout.navOtherSelected) ensure(c.views[1].p, i, false);  // their own plugin shows them again
        } else if (c.page != Page::Other)  // (another mod's page draws its own highlight)
            for (auto i : layout.navStockSelected) show(c.views[1].p, i, true);  // once; the game's scripts pick the right one
        c.navState = nav;
    }
    void* page = c.views[2].p;
    if (sounds && page) {
        setText(page, QUEUE_PAGE_SOUND_STATUS, c.soundStatus, now < c.flashUntil ? c.flash : c.editing ? "TAP A SOUND TO SET ITS KEY" : "TAP A SOUND TO PRESS ITS KEY");
        setText(page, QUEUE_PAGE_SOUND_EDIT_TEXT, c.soundEditText, c.editing ? "DONE" : "EDIT");
        for (unsigned i = 0; i < SOUND_PADS; i++) {
            setText(page, QUEUE_SOUND_TEXTS[i], c.soundText[i], soundLabels[i]);
            setText(page, QUEUE_SOUND_CAPTIONS[i], c.soundCaption[i], soundNames[i]);
        }
    }
    if (holoPage && page && !c.setup) {
        int where = holo::where, found = holo::found;
        std::string status = now < c.flashUntil ? c.flash
                           : found < 0 ? "LOOKING FOR THE HOLOGRAM ARMS..."
                           : found == 0 ? "HOLOGRAM ARMS NOT FOUND YET"
                           : std::string("IN USE NOW: ") + HOLO_BAR_LABELS[where];
        setText(page, QUEUE_PAGE_HOLO_STATUS, c.holoStatus, status);
        // - / + held: one more step after 0.4 s, then 16 a second (until it's let go, or 8 s)
        if (c.holoHeld && held.count({reinterpret_cast<U>(c.gs), c.holoActor, QUEUE_HOLO_BUTTONS[c.holoHeld - 1]}) &&
            now - c.holoHeldAt > 400 && now - c.holoHeldAt < 8000 && now - c.holoRepeatAt >= 60) {
            holo::press(unsigned(c.holoHeld - 1));
            c.holoRepeatAt = now;
        }
        static holo::View v;
        holo::view(v);
        for (unsigned i = 0; i < HOLO_CONTROL_COUNT; i++) ensure(page, QUEUE_HOLO_FRAMES[i], v.lit[i]);
        setText(page, QUEUE_PAGE_HOLO_HEX, c.holoHex, v.hex);
        // The preview in the colour picked and the fine-tune buttons in theirs: written into the
        // elements' colours (+0x78), then the preview hidden and shown again so the tablet redraws.
        std::vector<float> tints(v.colour, v.colour + 3);
        for (unsigned i = 0; i < 2 * HOLO_FINE; i++) {
            tints.insert(tints.end(), v.fine[i], v.fine[i] + 3);
            tints.insert(tints.end(), v.fineText[i], v.fineText[i] + 3);
        }
        bool moved = tints.size() != c.holoTints.size();
        for (size_t i = 0; !moved && i < tints.size(); i++) moved = std::fabs(tints[i] - c.holoTints[i]) > 0.004f;
        if (moved && now - c.holoTintedAt >= 50) {   // (the rainbow's preview: 20 redraws a second at most)
            P rows = at<P>(page, 0x90);
            auto tint = [&](unsigned element, const float* rgb) {
                float* to = reinterpret_cast<float*>(rows + element * 224 + 0x78);
                to[0] = rgb[0], to[1] = rgb[1], to[2] = rgb[2], to[3] = 1;
            };
            tint(QUEUE_PAGE_HOLO_PREVIEW, v.colour);
            for (unsigned i = 0; i < 2 * HOLO_FINE; i++) {
                tint(QUEUE_HOLO_FINE_FILLS[i], v.fine[i]);
                tint(QUEUE_HOLO_FINE_TEXTS[i], v.fineText[i]);
            }
            show(page, QUEUE_PAGE_HOLO_PREVIEW, false);
            show(page, QUEUE_PAGE_HOLO_PREVIEW, true);
            c.holoTints = tints;
            c.holoTintedAt = now;
        }
    }
    if (play && page) {
        int state = c.state;
        bool searching = matchmaking(state);
        Queued r = Queued(lastResult.load());
        if (r != Queued::None && now - lastResultAt < 3000 && r != Queued::Started) {
            c.flash = r == Queued::Blocked ? "ECHOLAZYTABLET IS OFF FOR THIS ACCOUNT"
                    : r == Queued::PublicMatch ? "IN A PUBLIC MATCH - FINISH IT FIRST"
                    : r == Queued::AlreadySearching ? "ALREADY SEARCHING"
                    : r == Queued::NotReady ? "NOT READY YET - TRY AGAIN IN A MOMENT" : "COULDN'T QUEUE - SEE ECHOLAZYTABLET.LOG";
            c.flashUntil = lastResultAt + 3000;
        }
        if (gfxMessageAt && now - gfxMessageAt < 4000) { c.flash = gfxMessage; c.flashUntil = gfxMessageAt + 4000; }
        std::string message = now < c.flashUntil && !searching ? c.flash : statusFor(state);
        setText(page, QUEUE_PAGE_STATUS, c.status, message);
        // Beside 12H / 24H: DOWNLOAD V3.22 with a green dot while a newer release is out (tap: the
        // release page), NO UPDATE with a red one otherwise (tapping does nothing).
        std::string update = updateAvailable();
        if (update != c.updateText || c.updateShown < 0) {
            bool on = !update.empty();
            text(page, QUEUE_PAGE_UPDATE_TEXT, on ? ("DOWNLOAD V" + update).c_str() : "NO UPDATE");
            show(page, QUEUE_PAGE_UPDATE_DOT, on);
            show(page, QUEUE_PAGE_UPDATE_DOT_RED, !on);
            c.updateText = update;
            c.updateShown = 1;
        }
        // Searching layout: teal card, queue timer, CANCEL SEARCH in place of ARENA and COMBAT.
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
            show(page, QUEUE_PAGE_COMBAT, !searching);
            show(page, QUEUE_PAGE_COMBAT_TEXT, !searching);
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
        throwWantedAt = now;
        double speed = lastThrow;
        char throwText[16] = "-- M/S";
        if (speed > 0) snprintf(throwText, sizeof throwText, "%.1f M/S", speed);
        setText(page, QUEUE_PAGE_THROW_TEXT, c.throwText, throwText);
        // GRAPHICS: the chosen one lit; the caption says when it takes a restart.
        int chosen = gfxChosen;
        if (c.gfxShown != chosen) {
            ensure(page, QUEUE_PAGE_GFX_HIGH, chosen == GFX_HIGH);
            ensure(page, QUEUE_PAGE_GFX_LOW, chosen == GFX_LOW);
            ensure(page, QUEUE_PAGE_GFX_ICON, chosen != GFX_LOW);
            ensure(page, QUEUE_PAGE_GFX_ICON_LOW, chosen == GFX_LOW);
            c.gfxShown = chosen;
        }
        setText(page, QUEUE_PAGE_GFX_CAPTION, c.gfxCaption,
                chosen == GFX_UNKNOWN ? "NOT SET UP" : !texlod::deviceHooked && chosen != gfxLoaded ? "RESTART ECHO" : "GRAPHICS");
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
    U key = U(c.page == Page::Queue) | U(matchmaking(c.state)) << 1 | U(ready) << 2 | U(tabletFault.load()) << 3 | U(c.setup) << 4 |
            U(c.page == Page::Sounds) << 5 | U(c.page == Page::Holo) << 6 | U(n) << 8 | count << 24;
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
        else if (b.name == QUEUE_TAB || b.name == QUEUE_SOUND_TAB) hide = !ready;
        else if (b.name == QUEUE_HOLO_TAB) hide = !ready || !layout.holo;
        else if (keyFor(b.name)) hide = !onOurPage(c) || !c.setup;
        else if (c.setup && onOurPage(c)) hide = true;   // the page's own buttons, under the setup page
        else if (b.name == QUEUE_CLOCK) hide = c.page != Page::Sounds;
        else if (b.name == QUEUE_PLAY_CLOCK || b.name == QUEUE_CLOCK_12 || b.name == QUEUE_CLOCK_24) hide = c.page != Page::Queue;
        else if (b.name == QUEUE_UPDATE) hide = c.page != Page::Queue || updateAvailable().empty();
        else if (soundPad(b.name) >= 0 || b.name == QUEUE_SOUND_EDIT) hide = c.page != Page::Sounds;
        else if (holoButton(b.name) >= 0) hide = c.page != Page::Holo;
        else if (b.name == QUEUE_START || b.name == QUEUE_COMBAT) hide = c.page != Page::Queue || matchmaking(c.state);
        else if (b.name == QUEUE_CANCEL) hide = c.page != Page::Queue || !matchmaking(c.state);
        else if (b.name == QUEUE_LOBBY) hide = c.page != Page::Queue || matchmaking(c.state);
        else if (b.name == QUEUE_PRIVATE) hide = c.page != Page::Queue || matchmaking(c.state);
        else if (b.name == QUEUE_STATS || b.name == QUEUE_GFX_HIGH || b.name == QUEUE_GFX_LOW || b.name == QUEUE_CLIP ||
                 b.name == QUEUE_CLIP_KEY) hide = c.page != Page::Queue;
        else hide = onOurPage(c);                                // stock page content (another mod's page hides it itself)
        if (hide && !c.masked.count(b.handle)) {
            if (b.reasons & MASK_BIT) {  // someone else's: it's off already, and theirs to turn back on
                static std::set<U> warned;
                if (warned.insert(b.name).second)
                    logf("tablet: button %016llx already has disable bit %x from another mod; left to it", b.name, MASK_BIT);
                continue;
            }
            disableButton(cs, b.handle, MASK_BIT);
            c.masked.insert(b.handle);
        } else if (!hide && c.masked.count(b.handle)) {
            enableButton(cs, b.handle, MASK_BIT);
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
    graphicsLiveTick();
    U t1 = qpcNs();
    gate(it->second, cs);
    U t2 = qpcNs();
    render(it->second);
    nsGate += t2 - t1;
    nsRender += qpcNs() - t2;
}

// Every 5 s while anything happened: how often the hook ran, EchoLazyTablet's own time in it,
// and how many redraw-causing calls it made.
void perfReport() {
    static U last = 0;
    U now = GetTickCount64();
    if (!last) last = now;
    if (now - last < 30000) return;
    double secs = (now - last) / 1000.0;
    last = now;
    U s = perf.show.exchange(0), t = perf.text.exchange(0), a = perf.alpha.exchange(0), m = perf.mask.exchange(0);
    U ticks = perf.ticks.exchange(0), ns = perf.ns.exchange(0);
    U frames = perf.frames.exchange(0), hookNs = perf.hookNs.exchange(0), sounds = perf.sounds.exchange(0), soundNs = perf.soundNs.exchange(0);
    bool page = false;
    for (auto& [gs, c] : contexts) page |= c.page == Page::Queue;
    U hk = nsHousekeeping.exchange(0), g = nsGate.exchange(0), r = nsRender.exchange(0);
    // Every 30 s: the headset frame rate, and EchoLazyTablet's own time per second on Echo's threads.
    logf("perf: headset %.1f fps; EchoLazyTablet %.3f ms/s on Echo's threads (tablet %.3f: housekeeping %.3f, buttons %.3f, page %.3f; "
         "frame hook %.3f; sound hook %.3f for %.0f sounds/s); tablet redraws/s: show %.1f text %.1f alpha %.1f masks %.1f; "
         "%.0f tablet updates/s (PLAY page %s)",
         frames / secs, (ns + hookNs + soundNs) / 1e6 / secs, ns / 1e6 / secs, hk / 1e6 / secs, g / 1e6 / secs, r / 1e6 / secs,
         hookNs / 1e6 / secs, soundNs / 1e6 / secs, sounds / secs, s / secs, t / secs, a / secs, m / secs, ticks / secs, page ? "open" : "closed");
    texlod::report();
}

// Closes another mod's page (see Kick). Called without our lock held: the other mod's dispatch
// hook takes its own lock, and its button hook holds that lock while ours may be waiting.
void sendKick() {
    Kick k;
    {
        std::lock_guard<std::recursive_mutex> lock(tabletMutex);
        std::swap(k, kick);
    }
    if (!k.gs || tabletFault) return;
    logf("tablet: closing the other mod's page (stock tab %016llx press)", k.tab);
    auto entry = fn<Dispatch>(exe, RVA_DISPATCH);  // hooked: reaches every mod, then the game
    sendingKick = true;
    entry(k.gs, PRESS, k.actor, k.tab, k.arg);
    entry(k.gs, RELEASE, k.actor, k.tab, k.arg);
    sendingKick = false;
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
    sendKick();
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
                ctx.gfxCaption.clear(), ctx.titleText.clear(), ctx.subtitleText.clear(), ctx.playClockText.clear(), ctx.playAmpmText.clear(), ctx.throwText.clear();
                ctx.playClock24Text.clear(), ctx.updateText.clear(), ctx.updateShown = -1;  // a fresh overlay: set UPDATE again
                ctx.clockText.clear(), ctx.clockFormatText.clear(), ctx.keyOtherText.clear();
                ctx.keyCtrlText.clear(), ctx.keyShiftText.clear(), ctx.keyAltText.clear();
                ctx.soundStatus.clear(), ctx.soundEditText.clear();
                for (auto& t : ctx.soundText) t.clear();
                for (auto& t : ctx.soundCaption) t.clear();
                ctx.holoStatus.clear(), ctx.holoHex.clear(), ctx.holoTints.clear();
                ctx.modsShown = -1;
                ctx.searchShown = -1;
                ctx.gfxShown = -2;
            }
            canvases.erase(it);
        }
    }
    unloadOriginal(p, a, b, c);
}

// The hotkey keyboard, starting from the saved hotkey of CLIP (slot -1) or that SFX button.
void openKeySetup(Context& c, int slot) {
    c.setup = true;
    c.setupSlot = slot;
    c.naming = false;
    c.setupChangedAt = GetTickCount64();
    c.pickFirstUntil = 0;
    c.ctrl = c.shift = c.alt = false;
    c.key.clear();
    std::string spec = slot < 0 ? clipKeysSpec() : soundKeys[slot];
    Keys keys;
    if (parseKeys(spec, keys))
        for (WORD vk : keys.vks) {
            if (vk == VK_CONTROL) c.ctrl = true;
            else if (vk == VK_SHIFT) c.shift = true;
            else if (vk == VK_MENU) c.alt = true;
        }
    size_t plus = spec.rfind('+');
    std::string last = spec.substr(plus == std::string::npos ? 0 : plus + 1);
    for (auto& k : QUEUE_KEYS) {
        std::string label = k.label, lower;
        for (char ch : label) lower += char(tolower(static_cast<unsigned char>(ch)));
        if (lower == last && label != "CTRL" && label != "SHIFT" && label != "ALT") c.key = label;
    }
    logf("tablet: hotkey page opened (%s %d)", slot < 0 ? "CLIP" : "SFX", slot + 1);
}

void closeKeySetup(Context& c) {
    c.setup = c.naming = false;
    c.setupChangedAt = GetTickCount64();
}

// Naming an SFX button: letters and digits type, the bottom row is SPACE / DELETE / CLEAR / NO NAME.
void nameKey(Context& c, const std::string& label) {
    unsigned pad = unsigned(c.setupSlot);
    if (label == "CTRL") { if (!c.name.empty() && c.name.back() != ' ' && c.name.size() < 16) c.name += ' '; }
    else if (label == "SHIFT") { if (!c.name.empty()) c.name.pop_back(); }
    else if (label == "ALT") c.name.clear();
    else if (label == "CANCEL") closeKeySetup(c);   // the key's saved already; the name stays as it was
    else if (label == "SAVE" || label == "NO CLIP APP") {
        std::string name = label == "SAVE" ? c.name : "";
        name.erase(name.find_last_not_of(' ') + 1);
        std::wstring wide(name.begin(), name.end());
        WritePrivateProfileStringW(L"sounds", (L"name" + std::to_wstring(pad + 1)).c_str(), name.empty() ? nullptr : wide.c_str(), iniPath().c_str());
        soundNames[pad] = soundName(pad);
        closeKeySetup(c);
        c.flash = soundNames[pad] + " SAVED: " + soundLabels[pad];
        c.flashUntil = GetTickCount64() + 4000;
        logf("tablet: SFX %u named \"%s\"", pad + 1, name.c_str());
    } else if (label.size() == 1 && c.name.size() < 16) c.name += label;   // (F1..F12 don't type)
}

void onEvent(void* gs, U event, U actor, U component, int arg) {
    if (!isOurs(component) && !isStockTab(component) && !isOtherTab(component)) return;
    auto it = contexts.find(gs);
    if (it == contexts.end() || !it->second.views[0].p) return;
    auto& c = it->second;
    std::array<U, 3> key = {reinterpret_cast<U>(gs), actor, component};
    if (event == RELEASE) { held.erase(key); return; }
    if (!held.insert(key).second) return;  // still held: one press, one action
    // A finger still on the tablet sets off the button that replaces the one just pressed (FIND
    // MATCH started a search and CANCEL SEARCH, in its place, stopped it 80 ms later) or its
    // neighbour (LOW 1.3 s after HIGH): another of the page's buttons within a second is ignored.
    // The keyboard's keys and the SFX buttons are tapped in quick succession, so they're left out.
    if (isOurs(component) && component != QUEUE_TAB && component != QUEUE_SOUND_TAB && component != QUEUE_HOLO_TAB && !keyFor(component) &&
        soundPad(component) < 0 && holoButton(component) < 0) {
        static U lastAt = 0, lastButton = 0;
        U now = GetTickCount64();
        if (component != lastButton && now - lastAt < 1000) {
            logf("tablet: button %016llx ignored (%llu ms after another)", component, now - lastAt);
            return;
        }
        lastAt = now, lastButton = component;
    }
    // Instead, a finger still there when the keyboard opens or closes doesn't press what's underneath.
    if ((keyFor(component) || soundPad(component) >= 0) && GetTickCount64() - c.setupChangedAt < 400) {
        logf("tablet: button %016llx ignored (the keyboard just opened or closed)", component);
        return;
    }
    if (isStockTab(component)) {
        if (c.page == Page::Queue) logf("tablet: left the PLAY page");
        c.page = Page::Stock;
        c.stockTab = component;
    } else if (isOtherTab(component)) {
        if (c.page == Page::Queue) logf("tablet: left the PLAY page for another mod's tab (%016llx)", component);
        c.page = Page::Other;
        c.sawOther = true;
    } else if (component == QUEUE_TAB) {
        if (c.page != Page::Queue) { logf("tablet: PLAY page selected"); playSound(SOUND_CLICK); }
        if (c.setup && c.setupSlot >= 0) closeKeySetup(c);   // it was picking an SFX button's key
        static bool asked = false;   // first time this run with no clip key: ask for it
        if (!asked && clipKeysSpec().empty()) { asked = true; openKeySetup(c, -1); }
        if (c.page == Page::Other) kick = Kick{gs, actor, c.stockTab, arg};
        c.page = Page::Queue;
    } else if (component == QUEUE_SOUND_TAB) {
        if (c.page != Page::Sounds) { logf("tablet: SFX page selected"); playSound(SOUND_CLICK); c.editing = false; }
        if (c.setup && c.setupSlot < 0) closeKeySetup(c);    // it was picking CLIP's key
        if (c.page == Page::Other) kick = Kick{gs, actor, c.stockTab, arg};
        c.page = Page::Sounds;
    } else if (component == QUEUE_HOLO_TAB && layout.holo) {
        if (c.page != Page::Holo) { logf("tablet: HOLO page selected"); playSound(SOUND_CLICK); }
        if (c.setup) closeKeySetup(c);
        if (c.page == Page::Other) kick = Kick{gs, actor, c.stockTab, arg};
        c.page = Page::Holo;
        holo::rescan = holo::found <= 0;
    } else if (int hb = c.page == Page::Holo && !c.setup ? holoButton(component) : -1; hb >= 0) {
        playSound(SOUND_CLICK);
        std::string what = holo::press(unsigned(hb));
        if (!what.empty()) { c.flash = what; c.flashUntil = GetTickCount64() + 2000; }
        bool stepper = unsigned(hb) >= HOLO_FINE_FIRST && unsigned(hb) < HOLO_FINE_FIRST + 2 * HOLO_FINE;
        if (stepper) c.holoHeld = U(hb) + 1, c.holoHeldAt = c.holoRepeatAt = GetTickCount64(), c.holoActor = actor, c.gs = gs;
        else c.holoHeld = 0;
    } else if (component == QUEUE_SOUND_EDIT && c.page == Page::Sounds && !c.setup) {
        playSound(SOUND_CLICK);
        c.editing = !c.editing;
        c.flashUntil = 0;
    } else if (int pad = c.page == Page::Sounds && !c.setup ? soundPad(component) : -1; pad >= 0) {
        playSound(SOUND_CLICK);
        if (c.editing || soundKeys[pad].empty()) openKeySetup(c, pad);
        else {
            logf("tablet: SFX %d pressed", pad + 1);
            c.flash = soundNames[pad] + " - PRESSED " + soundLabels[pad];
            c.flashUntil = GetTickCount64() + 2000;
            pressKeys(soundKeys[pad]);
        }
    } else if ((component == QUEUE_START || component == QUEUE_COMBAT) && c.page == Page::Queue) {
        bool combat = component == QUEUE_COMBAT;
        logf("tablet: %s pressed (thread %lu)", combat ? "COMBAT" : "ARENA", GetCurrentThreadId());
        playSound(SOUND_CLICK);
        c.flash = "STARTING THE SEARCH...";
        c.flashUntil = GetTickCount64() + 3000;
        lastResult = int(Queued::None);
        if (!fault) requestQueue(combat ? "COMBAT" : "ARENA", false, combat);
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
    } else if ((component == QUEUE_CLOCK && c.page == Page::Sounds) ||
               ((component == QUEUE_PLAY_CLOCK || component == QUEUE_CLOCK_12 || component == QUEUE_CLOCK_24) && c.page == Page::Queue)) {
        bool twelve = component == QUEUE_CLOCK_12 ? true : component == QUEUE_CLOCK_24 ? false : !clock12;
        clock12 = twelve;
        playSound(SOUND_CLICK);
        WritePrivateProfileStringW(L"clock", L"format", twelve ? L"12" : L"24", iniPath().c_str());
        logf("tablet: clock shows %s", twelve ? "12-hour" : "24-hour");
    } else if (component == QUEUE_UPDATE && c.page == Page::Queue && !updateAvailable().empty()) {
        logf("tablet: UPDATE AVAILABLE pressed; opening the latest release in the browser");
        playSound(SOUND_CLICK);
        c.flash = "OPENING THE UPDATE ON YOUR PC'S BROWSER";
        c.flashUntil = GetTickCount64() + 4000;
        openLatestRelease();
    } else if (component == QUEUE_LOBBY && c.page == Page::Queue) {
        logf("tablet: LOBBY pressed (%s)", inMatch ? "in a match" : "not in a match");
        playSound(SOUND_CLICK);
        if (accessBlocked) { c.flash = "ECHOLAZYTABLET IS OFF FOR THIS ACCOUNT"; c.flashUntil = GetTickCount64() + 3000; return; }
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
    } else if (const QueueKey* k = onOurPage(c) && c.setup ? keyFor(component) : nullptr) {
        playSound(SOUND_CLICK);
        std::string label = k->label;
        if (c.naming) nameKey(c, label);
        else if (label == "CTRL") c.ctrl = !c.ctrl;
        else if (label == "SHIFT") c.shift = !c.shift;
        else if (label == "ALT") c.alt = !c.alt;
        else if (label == "CANCEL") { closeKeySetup(c); logf("tablet: hotkey page cancelled"); }
        else if (label == "NO CLIP APP" && c.setupSlot >= 0) {   // on an SFX button's keyboard, this key says CLEAR
            WritePrivateProfileStringW(L"sounds", soundIniKey(c.setupSlot).c_str(), nullptr, iniPath().c_str());
            setSoundKey(c.setupSlot, "");
            closeKeySetup(c);
            c.flash = "SOUND " + std::to_string(c.setupSlot + 1) + " CLEARED";
            c.flashUntil = GetTickCount64() + 3000;
            logf("tablet: SFX %d cleared", c.setupSlot + 1);
        }
        else if (label == "NO CLIP APP") {   // no clipping software: not asked again, CLIP does nothing
            WritePrivateProfileStringW(L"clip", L"keys", L"none", iniPath().c_str());
            closeKeySetup(c);
            c.flash = "CLIP OFF - NO CLIP APP (KEY TO SET ONE)";
            c.flashUntil = GetTickCount64() + 4000;
            logf("tablet: no clip app");
        }
        else if (label == "SAVE") {
            if (c.key.empty()) { c.pickMessage = "PICK A KEY, THEN SAVE"; c.pickFirstUntil = GetTickCount64() + 2500; return; }
            std::string spec = std::string(c.ctrl ? "ctrl+" : "") + (c.shift ? "shift+" : "") + (c.alt ? "alt+" : "");
            for (char ch : c.key) spec += char(tolower(static_cast<unsigned char>(ch)));
            std::wstring wide(spec.begin(), spec.end());
            Keys keys;
            parseKeys(spec, keys);
            if (c.setupSlot >= 0) {
                // Echo's window gets the press too, and its search hotkey doesn't look at modifiers.
                if (keys.vks.back() == hotkey) {
                    c.pickMessage = c.key + " STARTS A SEARCH - PICK ANOTHER";
                    c.pickFirstUntil = GetTickCount64() + 3000;
                    return;
                }
                WritePrivateProfileStringW(L"sounds", soundIniKey(c.setupSlot).c_str(), wide.c_str(), iniPath().c_str());
                setSoundKey(c.setupSlot, spec);
                logf("tablet: hotkey saved (SFX %d): %s", c.setupSlot + 1, spec.c_str());
                // Then its name, on the same keyboard, starting from the one it has.
                c.naming = true;
                c.name = soundNames[c.setupSlot] == "SOUND " + std::to_string(c.setupSlot + 1) ? "" : soundNames[c.setupSlot];
                c.setupChangedAt = GetTickCount64();
                return;
            } else {
                WritePrivateProfileStringW(L"clip", L"keys", wide.c_str(), iniPath().c_str());
                c.flash = "CLIP HOTKEY SAVED: " + keys.text;
            }
            closeKeySetup(c);
            c.flashUntil = GetTickCount64() + 4000;
            logf("tablet: hotkey saved (%s %d): %s", c.setupSlot < 0 ? "CLIP" : "SFX", c.setupSlot + 1, spec.c_str());
        } else {
            c.key = label;
        }
    } else if (component == QUEUE_CLIP_KEY && c.page == Page::Queue) {
        playSound(SOUND_CLICK);
        openKeySetup(c, -1);
    } else if (component == QUEUE_CLIP && c.page == Page::Queue && clipKeysSpec().empty()) {
        playSound(SOUND_CLICK);
        openKeySetup(c, -1);
    } else if (component == QUEUE_CLIP && c.page == Page::Queue && clipKeysSpec() == "none") {
        playSound(SOUND_CLICK);
        c.flash = remoteclip::send() ? "CLIP - SENT TO THE OTHER PC" : "NO CLIP APP - TAP KEY TO PICK ONE";
        c.flashUntil = GetTickCount64() + 3000;
    } else if (component == QUEUE_CLIP && c.page == Page::Queue) {
        logf("tablet: CLIP pressed");
        playSound(SOUND_CLICK);
        Keys keys;
        std::string spec = clipKeysSpec();
        bool valid = parseKeys(spec, keys);
        c.flash = !valid ? "CLIP KEYS NOT SET - SEE ECHOLAZYTABLET.INI" : remoteclip::send() ? "CLIP - " + keys.text + " ON BOTH PCS" : "CLIP - PRESSED " + keys.text;
        c.flashUntil = GetTickCount64() + 3000;
        pressKeys(spec);
    } else if ((component == QUEUE_GFX_HIGH || component == QUEUE_GFX_LOW) && c.page == Page::Queue) {
        int mode = component == QUEUE_GFX_LOW ? GFX_LOW : GFX_HIGH;
        logf("tablet: GRAPHICS %s pressed", mode == GFX_LOW ? "LOW" : "HIGH");
        playSound(SOUND_CLICK);
        if (gfxBusy.exchange(true)) return;
        c.flash = "SWITCHING GRAPHICS...";
        c.flashUntil = GetTickCount64() + 3000;
        // Off the game thread: it reads and writes a few MB of files.
        if (HANDLE t = CreateThread(nullptr, 0, graphicsSwitch, reinterpret_cast<LPVOID>(intptr_t(mode)), 0, nullptr)) CloseHandle(t);
        else gfxBusy = false;
    }
}

void dispatch(void* gs, U event, U actor, U component, int arg) {
    if (!tabletFault && !sendingKick && (event == PRESS || event == RELEASE)) {
        std::lock_guard<std::recursive_mutex> lock(tabletMutex);
        onEvent(gs, event, actor, component, arg);
    }
    dispatchOriginal(gs, event, actor, component, arg);
}

// ---- hotkey ----
LRESULT CALLBACK windowDetour(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_QUEUE || message == WM_PRIVATE) {
        Queued r = fault ? Queued::NotReady : queueNow(message == WM_PRIVATE, message == WM_QUEUE && wparam == 1);
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

// ---- package read log ([debug] package_reads=1, off by default) ----
// Every read Echo makes from its package files (48037dc70b0ecab2_<n>), one line each in
// plugins\EchoLazyTablet.reads.log: package, offset, bytes, ms since start. Matched against the
// manifest's frames (package, offset), it names every asset Echo loaded and when: which model and
// texture a lobby object is really drawn from. Hooks KernelBase's ReadFile and ReadFileEx; the
// handle's file is looked up on each read (handles are reused after a close).
namespace readlog {
using ReadFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using ReadExFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE);
ReadFn readOriginal = nullptr;
ReadExFn readExOriginal = nullptr;
std::mutex mutex;
FILE* out = nullptr;
DWORD started = 0;

int packageOf(HANDLE h) {
    if (GetFileType(h) != FILE_TYPE_DISK) return -1;
    wchar_t path[1024];
    DWORD n = GetFinalPathNameByHandleW(h, path, 1024, FILE_NAME_NORMALIZED);
    if (!n || n >= 1024) return -1;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return wcsncmp(name, L"48037dc70b0ecab2_", 17) == 0 ? _wtoi(name + 17) : -1;
}

void note(HANDLE h, DWORD size, LPOVERLAPPED ov) {
    int package = packageOf(h);
    if (package < 0) return;
    U offset = 0;
    if (ov) offset = U(ov->OffsetHigh) << 32 | ov->Offset;
    else {
        LARGE_INTEGER zero{}, at{};
        if (SetFilePointerEx(h, zero, &at, FILE_CURRENT)) offset = U(at.QuadPart);
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (out) {
        fprintf(out, "%d %llu %lu %lu\n", package, (unsigned long long)offset, size, GetTickCount() - started);
        fflush(out);
    }
}

BOOL WINAPI readDetour(HANDLE h, LPVOID buffer, DWORD size, LPDWORD done, LPOVERLAPPED ov) {
    note(h, size, ov);
    return readOriginal(h, buffer, size, done, ov);
}
BOOL WINAPI readExDetour(HANDLE h, LPVOID buffer, DWORD size, LPOVERLAPPED ov, LPOVERLAPPED_COMPLETION_ROUTINE done) {
    note(h, size, ov);
    return readExOriginal(h, buffer, size, ov, done);
}

void install() {
    if (!GetPrivateProfileIntW(L"debug", L"package_reads", 0, iniPath().c_str())) return;
    started = GetTickCount();
    out = _wfopen((dir + L"\\EchoLazyTablet.reads.log").c_str(), L"w");
    if (!out) { logf("reads: can't open EchoLazyTablet.reads.log"); return; }
    HMODULE base = GetModuleHandleW(L"kernelbase.dll");
    struct { const char* name; void* detour; void** original; } hooks[] = {
        {"ReadFile", reinterpret_cast<void*>(readDetour), reinterpret_cast<void**>(&readOriginal)},
        {"ReadFileEx", reinterpret_cast<void*>(readExDetour), reinterpret_cast<void**>(&readExOriginal)}};
    for (auto& k : hooks) {
        void* target = base ? reinterpret_cast<void*>(GetProcAddress(base, k.name)) : nullptr;
        MH_STATUS s = target ? MH_CreateHook(target, k.detour, k.original) : MH_ERROR_FUNCTION_NOT_FOUND;
        if (s == MH_OK) s = MH_EnableHook(target);
        logf("reads: %s %s", k.name, s == MH_OK ? "logged" : MH_StatusToString(s));
    }
}
}  // namespace readlog

// ---- sound output back when Echo closes ([audio] after_echo, off when empty) ----
// The Oculus runtime makes the headset Windows' default sound output; when Echo exits, the
// device named in [audio] after_echo (its name in Windows' sound list, e.g. "Speakers (High
// Definition Audio Device)") is made the default again. Done on the way out (ntdll's
// RtlExitUserProcess, or TerminateProcess on Echo itself), before Windows tears the process down;
// a crash skips it. Windows has no public call for this: IPolicyConfig is what its Sound panel uses.
namespace audioback {
struct IPolicyConfig : IUnknown {   // only SetDefaultEndpoint is called; the rest hold vtable slots
    virtual HRESULT STDMETHODCALLTYPE slot0() = 0; virtual HRESULT STDMETHODCALLTYPE slot1() = 0;
    virtual HRESULT STDMETHODCALLTYPE slot2() = 0; virtual HRESULT STDMETHODCALLTYPE slot3() = 0;
    virtual HRESULT STDMETHODCALLTYPE slot4() = 0; virtual HRESULT STDMETHODCALLTYPE slot5() = 0;
    virtual HRESULT STDMETHODCALLTYPE slot6() = 0; virtual HRESULT STDMETHODCALLTYPE slot7() = 0;
    virtual HRESULT STDMETHODCALLTYPE slot8() = 0; virtual HRESULT STDMETHODCALLTYPE slot9() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(LPCWSTR id, ERole role) = 0;
};
constexpr CLSID CLSID_PolicyConfig = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
constexpr IID IID_PolicyConfig = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};
constexpr PROPERTYKEY FRIENDLY_NAME = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::wstring wanted;
std::once_flag once;
using ExitFn = void(NTAPI*)(LONG);
using TerminateFn = BOOL(WINAPI*)(HANDLE, UINT);
ExitFn exitOriginal = nullptr;
TerminateFn terminateOriginal = nullptr;

void restore() {
    std::call_once(once, [] {
        HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // (another mode already on this thread is fine)
        IMMDeviceEnumerator* devices = nullptr;
        IMMDeviceCollection* list = nullptr;
        std::wstring id;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                       reinterpret_cast<void**>(&devices))) &&
            SUCCEEDED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list))) {
            UINT n = 0;
            list->GetCount(&n);
            for (UINT i = 0; i < n && id.empty(); i++) {
                IMMDevice* d = nullptr;
                IPropertyStore* props = nullptr;
                if (FAILED(list->Item(i, &d))) continue;
                if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props))) {
                    PROPVARIANT name;
                    PropVariantInit(&name);
                    LPWSTR devId = nullptr;
                    if (SUCCEEDED(props->GetValue(FRIENDLY_NAME, &name)) && name.vt == VT_LPWSTR &&
                        _wcsicmp(name.pwszVal, wanted.c_str()) == 0 && SUCCEEDED(d->GetId(&devId))) {
                        id = devId;
                        CoTaskMemFree(devId);
                    }
                    PropVariantClear(&name);
                    props->Release();
                }
                d->Release();
            }
        }
        IPolicyConfig* policy = nullptr;
        if (id.empty()) logf("audio: no sound output called \"%ls\"; left as it is", wanted.c_str());
        else if (FAILED(CoCreateInstance(CLSID_PolicyConfig, nullptr, CLSCTX_ALL, IID_PolicyConfig, reinterpret_cast<void**>(&policy))))
            logf("audio: Windows' sound settings can't be reached; left as it is");
        else {
            bool ok = true;
            for (ERole role : {eConsole, eMultimedia, eCommunications}) ok &= SUCCEEDED(policy->SetDefaultEndpoint(id.c_str(), role));
            logf("audio: %ls %s", wanted.c_str(), ok ? "is the sound output again" : "couldn't be made the sound output");
            policy->Release();
        }
        if (list) list->Release();
        if (devices) devices->Release();
        if (SUCCEEDED(init)) CoUninitialize();
    });
}

void NTAPI exitDetour(LONG code) {
    restore();
    exitOriginal(code);
}
BOOL WINAPI terminateDetour(HANDLE process, UINT code) {
    if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId()) restore();
    return terminateOriginal(process, code);
}

void install() {
    wchar_t buffer[256];
    GetPrivateProfileStringW(L"audio", L"after_echo", L"", buffer, 256, iniPath().c_str());
    wanted = buffer;
    if (wanted.empty()) return;
    void* exitTarget = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlExitUserProcess"));
    void* terminateTarget = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "TerminateProcess"));
    bool ok = exitTarget && terminateTarget &&
              MH_CreateHook(exitTarget, reinterpret_cast<void*>(exitDetour), reinterpret_cast<void**>(&exitOriginal)) == MH_OK &&
              MH_CreateHook(terminateTarget, reinterpret_cast<void*>(terminateDetour), reinterpret_cast<void**>(&terminateOriginal)) == MH_OK &&
              MH_EnableHook(exitTarget) == MH_OK && MH_EnableHook(terminateTarget) == MH_OK;
    logf(ok ? "audio: %ls becomes the sound output when Echo closes" : "audio: can't watch for Echo closing (%ls not restored)", wanted.c_str());
}
}  // namespace audioback

void initialize() {
    logf("EchoLazyTablet v3.25 starting (pid %lu)", GetCurrentProcessId());
    graphicsStartup();   // first: Echo reads its settings soon after start
    if (liveTextures()) texlod::loadAll();
    if (!verifyExecutable()) { fault = tabletFault = true; logf("EchoLazyTablet disabled"); return; }
    loadSettings();
    remoteclip::start();
    loadToday();
    voiceJoiningMatch = uint32_t(readHex(L"sounds", L"joining_match", 0));
    if (!voiceJoiningMatch) logf("settings: \"Joining Match\" not learnt yet (find one match from the menu or a terminal)");
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) { logf("MinHook init failed"); fault = tabletFault = true; return; }
    readlog::install();
    audioback::install();
    if (!hook(RVA_FIND_MATCH, reinterpret_cast<void*>(findDetour), reinterpret_cast<void**>(&findOriginal)) ||
        !hook(RVA_POST_EVENT, reinterpret_cast<void*>(postEventDetour), reinterpret_cast<void**>(&postEventOriginal))) { fault = tabletFault = true; return; }
    logf("matchmaking hooks installed");
    if (HANDLE t = CreateThread(nullptr, 0, shimWatch, nullptr, 0, nullptr)) CloseHandle(t);
    else shimWatchDone = true;
    installTablet();
    attachWindow();
    if (HANDLE t = CreateThread(nullptr, 0, matchWatch, nullptr, 0, nullptr)) CloseHandle(t);
    if (layout.holo) {
        holo::load();
        if (HANDLE t = CreateThread(nullptr, 0, holo::watch, nullptr, 0, nullptr)) CloseHandle(t);
    }
    if (HANDLE t = CreateThread(nullptr, 0, playersWatch, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, accessWatch, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, updateWatch, nullptr, 0, nullptr)) CloseHandle(t);
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

// nEVR (EchoTools/nevr-runtime) plugin exports: nEVR loads the plugins its config.yaml lists, and
// NevrPluginGetInfo is the one export it requires. Everything starts from DllMain, as under EchoLoader.
struct NevrPluginInfo {
    const char* name;
    const char* description;
    uint32_t version_major, version_minor, version_patch;
};
extern "C" __declspec(dllexport) NevrPluginInfo NevrPluginGetInfo() {
    return {"EchoLazyTablet", "Echo Arena and Echo Combat matchmaking from your hand tablet", 3, 25, 0};
}
extern "C" __declspec(dllexport) uint32_t NevrPluginGetApiVersion() { return 3; }
// COSMETIC | NETWORK | HOOKS_ENGINE: tablet pages, Echo's API and the update check, its own detours.
extern "C" __declspec(dllexport) uint32_t NevrPluginGetCapabilities() { return 0x02 | 0x10 | 0x20; }

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
