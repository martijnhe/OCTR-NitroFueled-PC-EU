// SPDX-License-Identifier: GPL-2.0-or-later
// See gamepads.h. SDL3's functions are resolved at run time (dlsym), from the program itself.

#include "gamepads.h"

#include <SDL3/SDL.h>

#include <cctype>
#include <chrono>
#include <cstring>

#include <dlfcn.h>

static_assert(int(Pads::BtnSouth) == SDL_GAMEPAD_BUTTON_SOUTH && int(Pads::BtnBack) == SDL_GAMEPAD_BUTTON_BACK &&
              int(Pads::BtnTouchpad) == SDL_GAMEPAD_BUTTON_TOUCHPAD && int(Pads::BtnButtonCount) == SDL_GAMEPAD_BUTTON_COUNT,
              "Pads::Button follows SDL_GamepadButton");
static_assert(int(Pads::AxLeftX) == SDL_GAMEPAD_AXIS_LEFTX && int(Pads::AxTriggerRight) == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER &&
              int(Pads::AxAxisCount) == SDL_GAMEPAD_AXIS_COUNT, "Pads::Axis follows SDL_GamepadAxis");

namespace Pads {
namespace {

// The SDL3 functions used, resolved from the DLL.
#define PADS_SDL_FUNCTIONS(X)                                                                   \
    X(SDL_SetHint) X(SDL_InitSubSystem) X(SDL_QuitSubSystem) X(SDL_GetError) X(SDL_free)        \
    X(SDL_GetGamepads) X(SDL_OpenGamepad) X(SDL_CloseGamepad) X(SDL_GamepadConnected)           \
    X(SDL_GetGamepadNameForID) X(SDL_GetGamepadGUIDForID) X(SDL_GetGamepadTypeForID)            \
    X(SDL_GUIDToString) X(SDL_UpdateGamepads) X(SDL_GetGamepadButton) X(SDL_GetGamepadAxis)     \
    X(SDL_RumbleGamepad) X(SDL_GetGamepadPowerInfo) X(SDL_GetGamepadConnectionState)            \
    X(SDL_GetGamepadStringForButton) X(SDL_GetGamepadButtonLabelForType)

struct Api {
#define PADS_MEMBER(f) decltype(&::f) f = nullptr;
    PADS_SDL_FUNCTIONS(PADS_MEMBER)
#undef PADS_MEMBER
};

Api sdl;
void* library = nullptr;
bool running = false, loaded = false;
std::string error;

struct Open {
    SDL_Gamepad* pad = nullptr;
    Info info;
};
std::vector<Open> open;
std::vector<Info> list;
std::chrono::steady_clock::time_point last_scan{};

// Capture: the inputs held when it was armed, per controller, are ignored until released.
bool capturing = false, captured = false;
std::string captured_name;
uint32_t captured_id = 0;
struct Held {
    uint32_t id;
    bool down[BtnInputCount];
};
std::vector<Held> held;

void* Symbol(const char* name) { return dlsym(library, name); }

bool Load(const std::string& path) {
    if (loaded) return true;
    library = dlopen(path.empty() ? nullptr : path.c_str(), RTLD_NOW);
    if (!library) {
        error = "SDL3 not found";
        return false;
    }
#define PADS_RESOLVE(f)                                                     \
    sdl.f = reinterpret_cast<decltype(&::f)>(Symbol(#f));                    \
    if (!sdl.f) {                                                            \
        error = "SDL3 lacks " #f;                                        \
        return false;                                                        \
    }
    PADS_SDL_FUNCTIONS(PADS_RESOLVE)
#undef PADS_RESOLVE
    loaded = true;
    return true;
}

void Sample(SDL_Gamepad* pad, State& s) {
    for (int b = 0; b < BtnButtonCount; ++b) s.buttons[b] = sdl.SDL_GetGamepadButton(pad, SDL_GamepadButton(b));
    for (int a = 0; a < AxAxisCount; ++a) s.axes[a] = sdl.SDL_GetGamepadAxis(pad, SDL_GamepadAxis(a));
    s.buttons[BtnLeftTrigger] = s.axes[AxTriggerLeft] > 16000;
    s.buttons[BtnRightTrigger] = s.axes[AxTriggerRight] > 16000;
}

void Refresh() {
    int count = 0;
    SDL_JoystickID* ids = sdl.SDL_GetGamepads(&count);
    std::vector<Open> next;
    for (int i = 0; ids && i < count; ++i) {
        Open o;
        for (auto& old : open)
            if (old.pad && old.info.id == ids[i]) {
                o = old;
                old.pad = nullptr;  // moved
            }
        if (!o.pad) {
            o.pad = sdl.SDL_OpenGamepad(ids[i]);
            if (!o.pad) continue;
            char guid[33]{};
            sdl.SDL_GUIDToString(sdl.SDL_GetGamepadGUIDForID(ids[i]), guid, sizeof guid);
            const char* name = sdl.SDL_GetGamepadNameForID(ids[i]);
            o.info.id = ids[i];
            o.info.guid = guid;
            o.info.name = name && *name ? name : "Gamepad";
            o.info.type = int(sdl.SDL_GetGamepadTypeForID(ids[i]));
            o.info.family = FamilyOf(o.info.type);
        }
        int percent = -1;
        const SDL_PowerState power = sdl.SDL_GetGamepadPowerInfo(o.pad, &percent);
        o.info.battery = power == SDL_POWERSTATE_ON_BATTERY || power == SDL_POWERSTATE_CHARGING ? percent : -1;
        o.info.charging = power == SDL_POWERSTATE_CHARGING;
        const SDL_JoystickConnectionState c = sdl.SDL_GetGamepadConnectionState(o.pad);
        o.info.wireless = c == SDL_JOYSTICK_CONNECTION_WIRELESS;
        o.info.wired = c == SDL_JOYSTICK_CONNECTION_WIRED;
        next.push_back(o);
    }
    if (ids) sdl.SDL_free(ids);
    for (auto& old : open)
        if (old.pad) sdl.SDL_CloseGamepad(old.pad);
    open = std::move(next);
    list.clear();
    for (const auto& o : open) list.push_back(o.info);
}

}  // namespace

bool Start(const std::string& library_path) {
    if (running) return true;
    if (!Load(library_path)) return false;
    // The launcher's window is often not in front while a controller is tried.
    sdl.SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!sdl.SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        const char* e = sdl.SDL_GetError();
        error = std::string("SDL: ") + (e ? e : "gamepad subsystem failed");
        return false;
    }
    running = true;
    error.clear();
    last_scan = {};
    return true;
}

void Stop() {
    if (!running) return;
    for (auto& o : open)
        if (o.pad) sdl.SDL_CloseGamepad(o.pad);
    open.clear();
    list.clear();
    held.clear();
    capturing = captured = false;
    sdl.SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    running = false;
}

bool Running() { return running; }
const std::string& Error() { return error; }

void Update() {
    if (!running) return;
    sdl.SDL_UpdateGamepads();
    const auto now = std::chrono::steady_clock::now();
    bool lost = false;
    for (const auto& o : open) lost |= !sdl.SDL_GamepadConnected(o.pad);
    if (lost || now - last_scan > std::chrono::milliseconds(500)) {
        last_scan = now;
        Refresh();
    }
    if (!capturing) return;
    for (const auto& o : open) {
        State s;
        Sample(o.pad, s);
        Held* h = nullptr;
        for (auto& e : held)
            if (e.id == o.info.id) h = &e;
        if (!h) {  // connected while waiting: what it holds now does not count either
            held.push_back({o.info.id, {}});
            std::memcpy(held.back().down, s.buttons, sizeof s.buttons);
            continue;
        }
        for (int i = 0; i < BtnInputCount; ++i) {
            if (i == BtnGuide) continue;  // Windows' Game Bar / Steam take it
            if (s.buttons[i] && !h->down[i]) {
                capturing = false;
                captured = true;
                captured_name = InputName(i);
                captured_id = o.info.id;
                return;
            }
            if (!s.buttons[i]) h->down[i] = false;
        }
    }
}

const std::vector<Info>& List() { return list; }

bool Read(uint32_t id, State& out) {
    for (const auto& o : open)
        if (o.info.id == id && o.pad) {
            Sample(o.pad, out);
            return true;
        }
    return false;
}

bool Rumble(uint32_t id, int ms) {
    for (const auto& o : open)
        if (o.info.id == id && o.pad) return sdl.SDL_RumbleGamepad(o.pad, 0xA000, 0xC000, Uint32(ms));
    return false;
}

void ArmCapture() {
    held.clear();
    for (const auto& o : open) {
        State s;
        Sample(o.pad, s);
        held.push_back({o.info.id, {}});
        std::memcpy(held.back().down, s.buttons, sizeof s.buttons);
    }
    captured = false;
    capturing = running;
}

void CancelCapture() { capturing = captured = false; }
bool Capturing() { return capturing; }

bool TakeCapture(std::string& name, uint32_t& id) {
    if (!captured) return false;
    captured = false;
    name = captured_name;
    id = captured_id;
    return true;
}

const char* InputName(int input) {
    // SDL's own names (SDL_GetGamepadStringForButton), fixed here so the list works before the
    // DLL is loaded; runtime_pad.c reads them with SDL_GetGamepadButtonFromString.
    static const char* const names[BtnInputCount] = {
        "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
        "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2",
        "paddle3", "paddle4", "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6",
        "lefttrigger", "righttrigger"};
    return input >= 0 && input < BtnInputCount ? names[input] : "";
}

int InputFromName(const std::string& name) {
    auto same = [](const std::string& a, const char* b) {  // ASCII, any case (no SDL call: the DLL may not be loaded)
        size_t i = 0;
        for (; i < a.size() && b[i]; ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
        return i == a.size() && !b[i];
    };
    for (int i = 0; i < BtnInputCount; ++i)
        if (same(name, InputName(i))) return i;
    return -1;
}

Family FamilyOf(int type) {
    switch (type) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
    case SDL_GAMEPAD_TYPE_STANDARD: return Family::Xbox;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return Family::PlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
    case SDL_GAMEPAD_TYPE_GAMECUBE: return Family::Nintendo;
    default: return Family::Other;
    }
}

std::string TypeName(int type, bool es) {
    switch (type) {
    case SDL_GAMEPAD_TYPE_XBOX360: return "Xbox 360";
    case SDL_GAMEPAD_TYPE_XBOXONE: return "Xbox One / Series";
    case SDL_GAMEPAD_TYPE_PS3: return "DualShock 3 (PS3)";
    case SDL_GAMEPAD_TYPE_PS4: return "DualShock 4 (PS4)";
    case SDL_GAMEPAD_TYPE_PS5: return "DualSense (PS5)";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return es ? "Mando Pro de Switch" : "Switch Pro Controller";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Joy-Con";
    case SDL_GAMEPAD_TYPE_GAMECUBE: return "GameCube";
    case SDL_GAMEPAD_TYPE_STEAM: return "Steam";
    case SDL_GAMEPAD_TYPE_STANDARD: return es ? "Mando estándar (XInput)" : "Standard gamepad (XInput)";
    default: return es ? "Mando genérico" : "Generic gamepad";
    }
}

std::string Label(int input, Family family, int type, bool es) {
    const bool ps = family == Family::PlayStation, nin = family == Family::Nintendo;
    switch (input) {
    case BtnSouth:
    case BtnEast:
    case BtnWest:
    case BtnNorth: {
        // The printed label of that position (a Switch pad has B at the bottom).
        const SDL_GamepadButtonLabel l =
            loaded ? sdl.SDL_GetGamepadButtonLabelForType(SDL_GamepadType(type), SDL_GamepadButton(input))
                   : SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
        switch (l) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return es ? "Cruz \xE2\x9C\x95" : "Cross \xE2\x9C\x95";
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return es ? "C\xC3\xADrculo \xE2\x97\x8B" : "Circle \xE2\x97\x8B";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return es ? "Cuadrado \xE2\x96\xA1" : "Square \xE2\x96\xA1";
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return es ? "Tri\xC3\xA1ngulo \xE2\x96\xB3" : "Triangle \xE2\x96\xB3";
        default: break;
        }
        static const char* const es_pos[] = {"Bot\xC3\xB3n de abajo", "Bot\xC3\xB3n derecho", "Bot\xC3\xB3n izquierdo",
                                              "Bot\xC3\xB3n de arriba"};
        static const char* const en_pos[] = {"Bottom button", "Right button", "Left button", "Top button"};
        return es ? es_pos[input] : en_pos[input];
    }
    case BtnBack:
        if (ps) return type == SDL_GAMEPAD_TYPE_PS5 ? "Create" : type == SDL_GAMEPAD_TYPE_PS3 ? "Select" : "Share";
        if (nin) return "\xE2\x88\x92 (Minus)";
        return type == SDL_GAMEPAD_TYPE_XBOXONE ? "View" : "Back";
    case BtnGuide: return ps ? "PS" : nin ? "Home" : "Xbox";
    case BtnStart:
        if (ps) return type == SDL_GAMEPAD_TYPE_PS3 ? "Start" : "Options";
        if (nin) return "+ (Plus)";
        return type == SDL_GAMEPAD_TYPE_XBOXONE ? "Menu" : "Start";
    case BtnLeftStick: return ps ? "L3" : es ? "Stick izq. (pulsar)" : "Left stick (click)";
    case BtnRightStick: return ps ? "R3" : es ? "Stick der. (pulsar)" : "Right stick (click)";
    case BtnLeftShoulder: return ps ? "L1" : nin ? "L" : "LB";
    case BtnRightShoulder: return ps ? "R1" : nin ? "R" : "RB";
    case BtnLeftTrigger: return ps ? "L2" : nin ? "ZL" : "LT";
    case BtnRightTrigger: return ps ? "R2" : nin ? "ZR" : "RT";
    case BtnDpadUp: return es ? "Cruceta \xE2\x86\x91" : "D-pad \xE2\x86\x91";
    case BtnDpadDown: return es ? "Cruceta \xE2\x86\x93" : "D-pad \xE2\x86\x93";
    case BtnDpadLeft: return es ? "Cruceta \xE2\x86\x90" : "D-pad \xE2\x86\x90";
    case BtnDpadRight: return es ? "Cruceta \xE2\x86\x92" : "D-pad \xE2\x86\x92";
    case BtnMisc1:
        if (ps) return es ? "Micr\xC3\xB3" "fono" : "Mic";
        if (nin) return es ? "Captura" : "Capture";
        return "Share";
    case BtnRightPaddle1: return "P1";
    case BtnLeftPaddle1: return "P3";
    case BtnRightPaddle2: return "P2";
    case BtnLeftPaddle2: return "P4";
    case BtnTouchpad: return es ? "Panel t\xC3\xA1""ctil" : "Touchpad";
    default:
        if (input >= BtnMisc2 && input <= BtnMisc6) return "Misc " + std::to_string(input - BtnMisc2 + 2);
        return "?";
    }
}

}  // namespace Pads
