#pragma once

// The keyboard as a libretro keyboard, for cores that want one (typing,
// computers, the Test libretro core's SRAM text field).
//
// There is no separate "game focus": while the game has the keyboard (no
// UI up), keys always go to the core too, as RETRO_DEVICE_KEYBOARD state
// and key events for its keyboard callback, and typed text as characters.
// Hotkeys are reached with the hotkey modifier (the input router's HOTKEY
// layer), so no key has to be kept from the core to toggle a mode.

#include <cstdint>

#include "libretro_host.h"

struct TextInput;
union SDL_Event;

struct KeyboardInput
{
    bool    to_core = false;                 // the game has the keyboard now
    uint8_t keys[LIBRETRO_KEY_COUNT] = {};   // RETROK_* -> held (as the core saw them)
};

// Call for every SDL event: sends key and text events to the core while
// the game has the keyboard. Never consumes the event (the input router
// still sees the keys: they are the RetroPad and the hotkeys too).
void keyboard_input_handle_event(KeyboardInput* keyboard, const SDL_Event* event, LibretroHost* host);

// Once per frame: whether the game has the keyboard. Taking it away releases
// every key the core saw as held. Tells `text_input` (frontends-common) what
// the game wants: characters while it has the keyboard (no on-screen
// keyboard), and the on-screen keyboard while the core asks for one (KEYS in
// Multitap's text input extension) and the game has the screen.
void keyboard_input_update(KeyboardInput* keyboard, bool to_core, LibretroHost* host, TextInput* text_input);

// Copies the held keys into this frame's input state.
void keyboard_input_read(const KeyboardInput* keyboard, LibretroInputState* input);
