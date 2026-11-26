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
#include "core_rewind.h"
#include "fixed_input.h"
#include "frame_timing.h"
#include "keyboard_input.h"
#include "libretro_host.h"
#include "viewport.h"   // ViewportCrop
#include "video_texture.h"

struct AppShell;
struct SDL_Gamepad;
union SDL_Event;

// Fast forward: while on, each loop runs as many core frames as fit in this
// share of one frame period (the loop's pacing, vsync or the Exact timer, is
// unchanged, so the UI stays responsive). Audio is muted, only the last
// picture of each loop is shown, and the core is told (GET_FASTFORWARDING).
constexpr double CORE_SESSION_FAST_FORWARD_BUDGET = 0.75;

// What the session's frames cost, for debug UIs and benchmarks. Times are
// wall-clock (SDL_GetTicksNS) totals since the core started.
struct CoreSessionStats
{
    uint64_t frames    = 0;   // core frames run (retro_run)
    uint64_t run_ns    = 0;   // inside retro_run: the core's work + storing its picture (libretro_video)
    uint64_t uploads   = 0;   // pictures uploaded into the texture
    uint64_t upload_ns = 0;   // CPU side of those uploads: copy into staging + recording the copy
};

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
    char     cache_directory[LIBRETRO_PATH_MAX] = {};                     // files extracted from archives ("" = none)
    uint32_t options_version = LIBRETRO_CORE_OPTIONS_VERSION_MAX;         // reported to the core
    uint32_t language = 0;   // RETRO_LANGUAGE_* for GET_LANGUAGE (0 = English)

    // The core's picture, uploaded once per new frame.
    VideoTexture texture         = {};   // texture.imgui_id: what ImGui draws, 0 = none
    uint64_t     uploaded_serial = 0;    // host->video.serial in the texture

    // Core options the frontend keeps (not owned; nullptr = not kept): the
    // stored values go to the core when it declares its options, and what
    // the core declares is cached. The frontend loads and saves both files.
    CoreOptionsStore* option_store   = nullptr;
    CoreOptionsCache* option_cache   = nullptr;
    uint32_t          options_serial = 0;   // host->options.serial last cached
    // The next start ignores the stored values (the core gets its own
    // defaults; nothing is deleted): a safe start after a crash. Cleared by it.
    bool              skip_stored_options_once = false;
    // Remote play: when set, the audio of every frame run that's heard (not
    // paused, not fast forward) is also added here, stereo pairs, for the
    // frontend to send and clear. Reserved when turned on; not grown per
    // frame after the first few.
    bool                 capture_audio = false;
    std::vector<int16_t> captured_audio;

    AudioOutput       audio        = {};   // open while a core runs
    AudioOutputConfig audio_config = {};   // the frontend's audio choices: applied at start and by core_session_update
    FrameTiming   timing      = {};   // Vsync / Exact
    FixedInput    fixed_input = {};
    KeyboardInput keyboard    = {};   // the core's libretro keyboard
    SDL_Gamepad*  gamepad     = nullptr;

    bool     paused     = false;
    bool     step_once  = false;      // run exactly one frame while paused

    // Fast forward (CORE_SESSION_FAST_FORWARD_BUDGET): on while the frontend
    // sets `fast_forward` (a toggle) or the fast forward key is held (Space,
    // core_session_handle_event). Ignored while paused.
    bool     fast_forward      = false;
    bool     fast_forward_held = false;

    // The frontend's own input layer instead of the fixed controls
    // (core_session_set_pads): then the fixed controls, the session's
    // gamepad and the Space key are left alone; fast_forward_held is the
    // frontend's to set.
    bool               external_pads = false;
    LibretroInputState pads          = {};   // joypad_buttons and analog only

    // Rewind (core_rewind.h; not owned, nullptr = none): the frontend starts
    // it once the game's settings are known (core_rewind_start); the session
    // records around every frame, stops it with the game, and while its DVR
    // is open runs the DVR's PLAY instead of the game.
    CoreRewind* rewind = nullptr;

    CoreSessionStats stats = {};      // reset by every start
    uint32_t state_slot = 0;          // any number: the frontend keeps it within the slots it offers

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
// given (resolve "~" etc. before). A path into an archive ("a.7z#Game.sfc",
// content_archive.h) is extracted into cache_directory first, and the core
// gets that copy; settings and the game's name still come from the archive
// path. On failure the core is unloaded again and `message` says why.
//
// Start and stop destroy `texture`. Never call them in a frame after the
// texture was handed to ImGui (image, draw list): that frame would render
// with a destroyed descriptor set. Call them before the UI draws the picture,
// or outside the frame (e.g. remember a UI choice and act on it next loop).
bool core_session_start(CoreSession* session, AppShell* shell, const char* core_path, const char* content_path);

// Starts the core's subsystem `subsystem` (its ident, LibretroSubsystem) with
// one content path per subsystem file, in order; nullptr or "" leaves an
// optional file out. Each path is handled like core_session_start's (one
// inside an archive is extracted). Core options use the first file's folder
// and the game name core_session_special_game_name gives; savestates are
// named the same way by the host.
bool core_session_start_special(CoreSession* session, AppShell* shell, const char* core_path, const char* subsystem,
                                const char* const* content_paths, uint32_t content_count);
void core_session_stop(CoreSession* session, AppShell* shell);
bool core_session_running(const CoreSession* session);

// The name a subsystem start goes by (core options' game layer, lists): the
// names of the files given (content_archive_game_name) joined with " + ",
// "SGB + Tetris". False if none is given or it doesn't fit.
bool core_session_special_game_name(const char* const* content_paths, uint32_t content_count, char* out, size_t out_size);

// Keyboard (to the core, see keyboard_input.h), gamepads coming and going,
// short taps on the fixed controls, the fast forward key (hold Space).
// Returns true if the frontend should ignore the event (none, for now).
// `keyboard_for_joypad`: false while the frontend's
// own UI wants the keyboard (a text field has focus).
bool core_session_handle_event(CoreSession* session, AppShell* shell, const SDL_Event* event, bool keyboard_for_joypad);

// The RetroPad of every port, from the frontend's input layer (sets
// external_pads). Call before core_session_run_frame.
void core_session_set_pads(CoreSession* session, const uint16_t buttons[LIBRETRO_MAX_PORTS],
                           const int16_t analog[LIBRETRO_MAX_PORTS][2][2]);

// Once per loop, before running frames: timing mode (from the user's choice,
// the core's fps and the display), vsync on/off and the audio speed.
// `keyboard_for_core`: the game has the keyboard (no UI up).
void core_session_update(CoreSession* session, AppShell* shell, bool keyboard_for_core);

// True while fast-forwarding (toggle or held key; not while paused).
bool core_session_fast_forwarding(const CoreSession* session);

// Runs one core frame if one is due (many while fast-forwarding): input (fixed controls, keyboard, and the
// ports' cursors of `pointer` if not nullptr: mouse, pointer, lightgun), retro_run, audio, texture size.
CoreSessionFrame core_session_run_frame(CoreSession* session, AppShell* shell, const LibretroInputState* pointer,
                                        bool keyboard_for_joypad);

// Uploads the newest core frame into `texture` (outside the rendering pass;
// call after building the UI, before app_shell_present).
void core_session_upload_video(CoreSession* session, AppShell* shell);

// The core's current picture copied into `target` (another texture: a DVR
// thumbnail), which is created / resized to fit. Same rules as
// core_session_upload_video (after app_shell_acquire, before present).
// False if there's no picture or it couldn't be copied.
bool core_session_copy_picture(CoreSession* session, AppShell* shell, VideoTexture* target);

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
    // The cropped frame as it is, not scaled (`width` / `height` only say
    // there is one): for a picture the frontend scales itself, sharp, to
    // the aspect it recorded (piccolo's CONTINUE card).
    bool         native = false;
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

// The core's last frame alone, as a StateThumbnail, written to `path` (PNG):
// a picture of the game without a state (a core that can't save them).
// False (logged) if nothing runs, there's no frame or it couldn't be written.
bool core_session_save_picture(const CoreSession* session, const StateThumbnail* thumbnail, const char* path);

// What slot `slot` holds for the running game. False if nothing runs
// or there's no states folder.
bool core_session_slot_info(const CoreSession* session, uint32_t slot, StateSlotInfo* info);
// `state_slot` moved by `delta`, wrapping around within first..last (the
// frontend's own range).
void core_session_change_slot(CoreSession* session, int32_t delta, uint32_t first, uint32_t last);
