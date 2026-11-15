#pragma once

// A libretro core running in a frontend window: everything between "the user
// picked a core and content" and "the picture is on screen and the sound is
// playing". Shared by every libretro frontend (piccolo, the host-libretro test
// frontend), which only add their own UI around it.
//
// Per loop:
//   while (SDL_PollEvent(&event))
//       if (!core_session_handle_event(&session, &event, ...)) { ...frontend's own events... }
//   core_session_update(&session, &shell);                 timing mode -> vsync + audio speed
//   core_session_run_frame(&session, &shell, ...);         when a frame is due
//   app_shell_begin_frame(...); ...UI, showing session.texture...; core_session_upload_video(...);
//   app_shell_present(...);
//   core_session_wait(&session);                           Exact mode: sleep until the next frame

#include <cstdint>

#include "audio_output.h"
#include "core_options_cache.h"
#include "core_options_store.h"
#include "fixed_input.h"
#include "frame_timing.h"
#include "keyboard_input.h"
#include "libretro_host.h"
#include "viewport.h"   // ViewportCrop
#include "vk_texture.h"

struct AppShell;
struct SDL_Gamepad;
union SDL_Event;

enum class CoreSessionFrame
{
    skipped,     // nothing ran (no core, paused, minimized, or not due)
    ran,
    shut_down,   // the core asked to shut down; the session stopped it
};

struct CoreSession
{
    LibretroHost* host = nullptr;   // owned; allocated by core_session_create

    // Settings for the next start.
    char     saves_directory[LIBRETRO_PATH_MAX] = {};                     // SRAM ("" = not kept)
    char     states_directory[LIBRETRO_PATH_MAX] = {};                    // savestates ("" = none)
    char     system_directory[LIBRETRO_PATH_MAX] = {};                    // BIOS etc. ("" = none)
    uint32_t options_version = LIBRETRO_CORE_OPTIONS_VERSION_MAX;         // reported to the core
    uint32_t language = 0;   // RETRO_LANGUAGE_* for GET_LANGUAGE (0 = English)

    // The core's picture, uploaded once per new frame.
    StreamingTexture texture         = {};
    uint64_t         texture_id      = 0;   // ImGui texture id, 0 = none
    uint64_t         uploaded_serial = 0;   // host->video.serial in the texture

    // Core options the frontend keeps (not owned; nullptr = not kept): the
    // stored values go to the core when it declares its options, and what
    // the core declares is cached. The frontend loads and saves both files.
    CoreOptionsStore* option_store   = nullptr;
    CoreOptionsCache* option_cache   = nullptr;
    uint32_t          options_serial = 0;   // host->options.serial last cached
    // The next start ignores the stored values (the core gets its own
    // defaults; nothing is deleted): a safe start after a crash. Cleared by it.
    bool              skip_stored_options_once = false;

    AudioOutput       audio        = {};   // open while a core runs
    AudioOutputConfig audio_config = {};   // the frontend's audio choices: applied at start and by core_session_update
    FrameTiming   timing      = {};   // Vsync / Exact
    FixedInput    fixed_input = {};
    KeyboardInput keyboard    = {};   // libretro keyboard + game focus
    SDL_Gamepad*  gamepad     = nullptr;

    bool     paused     = false;
    bool     step_once  = false;      // run exactly one frame while paused
    uint32_t state_slot = LIBRETRO_STATE_SLOT_FIRST;   // 1..9

    char message[512] = {};           // what the last start/stop/state call did, for the UI
};

// Allocates the host. False only if out of memory.
bool core_session_create(CoreSession* session);
// Stops the core and frees everything (safe after a failed create).
void core_session_destroy(CoreSession* session, AppShell* shell);

// Changes a core option at `layer` (kept in option_store; a value equal to
// what applies below isn't written) and gives the running core the new
// effective value. False if no store, no such option, or the layer doesn't
// exist (no content: no folder / game layer).
bool core_session_set_option(CoreSession* session, CoreOptionLayer layer, const char* key, const char* value);

// Removes `key` from `layer` and gives the running core what applies now
// (the layer below, or the core's default).
bool core_session_reset_option(CoreSession* session, CoreOptionLayer layer, const char* key);

// Loads the core, then the content (nullptr or "" = none). Paths are used as
// given (resolve "~" etc. before). On failure the core is unloaded again and
// `message` says why.
//
// Start and stop destroy `texture`. Never call them in a frame after the
// texture was handed to ImGui (image, draw list): that frame would render
// with a destroyed descriptor set. Call them before the UI draws the picture,
// or outside the frame (e.g. remember a UI choice and act on it next loop).
bool core_session_start(CoreSession* session, AppShell* shell, const char* core_path, const char* content_path);
void core_session_stop(CoreSession* session, AppShell* shell);
bool core_session_running(const CoreSession* session);

// Keyboard (game focus), gamepads coming and going, short taps on the fixed
// controls. Returns true if the event was the core's keyboard input and the
// frontend should ignore it. `keyboard_for_joypad`: false while the frontend's
// own UI wants the keyboard (a text field has focus).
bool core_session_handle_event(CoreSession* session, AppShell* shell, const SDL_Event* event, bool keyboard_for_joypad);

// Once per loop, before running frames: timing mode (from the user's choice,
// the core's fps and the display), vsync on/off and the audio speed.
void core_session_update(CoreSession* session, AppShell* shell);

// Runs one core frame if one is due: input (fixed controls, keyboard, and the
// pointer fields of `pointer` if not nullptr), retro_run, audio, texture size.
CoreSessionFrame core_session_run_frame(CoreSession* session, AppShell* shell, const LibretroInputState* pointer,
                                        bool keyboard_for_joypad);

// Uploads the newest core frame into `texture` (outside the rendering pass;
// call after building the UI, before app_shell_present).
void core_session_upload_video(CoreSession* session, AppShell* shell);

// Exact timing mode: sleeps until the next frame is due. Call at the end of the loop.
void core_session_wait(const CoreSession* session);

// Savestates in `state_slot`. False if nothing runs or it failed (`message` says why).
// A savestate's thumbnail: the picture as it is shown (decided by Andrés,
// 2026-11-14: "con el aspect ratio que estamos usando, para que no se vean
// feos"): the frame cropped like on screen, scaled (smoothly) to the
// picture's size at x1 with the aspect in use (viewport_picture_size).
// Saved as <state file>.png, e.g. "Super Mario World (USA).state1.png".
struct StateThumbnail
{
    ViewportCrop crop   = {};
    uint32_t     width  = 0;   // 0 = no thumbnail
    uint32_t     height = 0;
};

// What a slot holds, for a slot picker.
struct StateSlotInfo
{
    bool    used          = false;
    int64_t modified      = 0;     // SDL_Time (ns since 1970) of the state file
    bool    has_thumbnail = false;
    char    thumbnail_path[LIBRETRO_PATH_MAX + 8] = {};   // the state file + ".png"
};

// Savestates in `state_slot`. False if nothing runs or it failed (`message`
// says why). `thumbnail` (may be nullptr: none) is written next to the state;
// an old thumbnail of the slot is removed when no new one could be made, so
// it never shows another state's picture.
bool core_session_save_state(CoreSession* session, const StateThumbnail* thumbnail);
bool core_session_load_state(CoreSession* session);

// What slot `slot` (1..9) holds for the running game. False if nothing runs
// or there's no states folder.
bool core_session_slot_info(const CoreSession* session, uint32_t slot, StateSlotInfo* info);
void core_session_change_slot(CoreSession* session, int32_t delta);
