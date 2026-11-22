#pragma once

// core_rewind: the rewind DVR of a running libretro core. Records the
// game's history (frontends-common's rewind_recorder: state deltas on a
// worker thread, the input of every frame, the events libretro_host hears)
// and moves through it while the DVR is open.
//
//   SCRUB       a state every frame: the DVR steps frame by frame.
//   SNAPSHOTS   a state every SNAPSHOT EVERY seconds plus every frame's
//               input: the DVR steps snapshot by snapshot, PLAY replays the
//               input between them.
//   AUTO        measures the core for a few seconds (after a warm-up) and
//               picks one: SCRUB if a state is cheap enough to take and load
//               every frame. The verdict is kept per game by the frontend
//               (settings.known) so it's measured once.
//
// Frames: state N is the game before frame N runs; input N is what frame N
// runs with; an event at N happened between frames N - 1 and N.
//
// The DVR (decided by Andrés, 2026-11-20): opened by a hotkey, the game
// stops. Moving shows the picture at the cursor (the state loaded and one
// frame run). Y plays the recorded game from the cursor, B plays on from
// the cursor, A goes back to the present (the game as it was when the DVR
// opened).
//
// Two lanes (Andrés, 2026-11-22, "Timelines: two for now"): B keeps the
// future it leaves as a second lane. The kept lane is a history of its own,
// the same size as the recorded one (Andrés: "same size buffer as the
// original"), so it never costs the recorded history anything and lasts
// until the next B replaces it; it takes that memory only once there is one.
// B on the recorded lane copies the history into the kept one and cuts the
// original; B on the kept lane cuts it and swaps the two (the lane left
// becomes the kept one). The DVR switches between them at the same frame.
// A lane's states carry the game's SRAM, so playing on from one brings its
// save back.
//
// Known gap: core options aren't in a state. Going back across an option
// change keeps the option as it is now; PLAY over it applies it.

#include <cstddef>
#include <cstdint>

#include "libretro_host.h"
#include "rewind_recorder.h"

enum class RewindMode : uint8_t
{
    off,
    automatic,
    scrub,
    snapshots,
};

struct CoreRewindSettings
{
    RewindMode mode             = RewindMode::automatic;
    size_t     memory_bytes     = static_cast<size_t>(256) << 20;   // MEMORY: the whole history, pool included
    uint32_t   scrub_seconds    = 0;    // SCRUB LENGTH (0: what memory holds)
    uint32_t   snapshot_seconds = 10;   // SNAPSHOT EVERY
    uint32_t   snapshots_kept   = 0;    // SNAPSHOTS KEPT (0: what memory holds)
    // AUTO's verdict for this game from an earlier run (scrub or snapshots);
    // automatic: measure.
    RewindMode known            = RewindMode::automatic;
};

enum class CoreRewindState : uint8_t
{
    off,           // no game, or REWIND OFF
    unsupported,   // the core has no savestates (serialize size 0)
    measuring,     // AUTO: warm-up and measurement (recording as SCRUB meanwhile)
    scrub,
    snapshots,
};

// AUTO's measurement: after the warm-up, this many frames with a state each.
constexpr double   CORE_REWIND_WARMUP_SECONDS = 5.0;
constexpr uint32_t CORE_REWIND_MEASURE_FRAMES = 120;
constexpr uint32_t CORE_REWIND_MEASURE_LOADS  = 4;
// AUTO picks SCRUB when (shares of one frame period):
//   the game thread's serialize (95th percentile) is small, a load (each DVR
//   step and WATCH frame) fits, the worker's encode (average) keeps up with
//   room to spare, and it skipped next to nothing while measured.
// The core's own frame time isn't a criterion: measured paced (as played),
// a CPU that clocks down between frames makes a light core look busy
// (SameBoy at 95% of a frame on the RK3588, 35% unpaced); the governor
// clocks up when there's more to do.
constexpr double CORE_REWIND_SCRUB_RECORD_MAX = 0.15;   // serialize, on the game's thread
constexpr double CORE_REWIND_SCRUB_LOAD_MAX   = 0.50;   // unserialize
constexpr double CORE_REWIND_SCRUB_ENCODE_MAX = 0.50;   // the worker's encode
constexpr double CORE_REWIND_SCRUB_SKIP_MAX   = 0.05;   // states skipped (the worker behind)

struct CoreRewindMeasure
{
    uint32_t frames = 0;   // samples taken
    uint64_t serialize_ns[CORE_REWIND_MEASURE_FRAMES] = {};
    uint64_t frame_ns[CORE_REWIND_MEASURE_FRAMES]     = {};
    uint64_t load_ns     = 0;   // the slowest of CORE_REWIND_MEASURE_LOADS
    uint64_t start_frame = 0;
    RewindRecorderStats start_stats = {};

    // The verdict's numbers (shares of a frame), for the log and docs/rewind.md.
    double record = 0.0;   // serialize
    double load   = 0.0;
    double encode = 0.0;
    double total  = 0.0;   // the core's frame + serialize (shown, not a criterion)
    double skip   = 0.0;
};

struct CoreRewind
{
    CoreRewindSettings settings = {};
    CoreRewindState    state    = CoreRewindState::off;
    LibretroHost*      host     = nullptr;

    RewindRecorder recorder     = {};
    bool           has_recorder = false;
    size_t         state_size   = 0;
    double         fps          = 60.0;

    uint64_t frame            = 0;   // the frame about to run
    uint32_t state_every      = 1;   // a state every N frames
    uint64_t last_state_frame = 0;
    bool     has_state        = false;
    bool     force_state      = false;   // the next frame records a state (a savestate was loaded)
    bool     retry_size       = false;   // serialize size 0 at start: asked again after the first frame

    // SCRUB's safety valve: when the worker skips too many states, a state
    // every 2nd, then every 3rd frame.
    uint64_t valve_frame   = 0;
    uint64_t valve_skipped = 0;

    CoreRewindMeasure measure = {};
    bool              verdict = false;   // AUTO just decided: the frontend keeps state as settings.known (then clears this)

    // The DVR.
    bool         dvr_open   = false;
    bool         playing    = false;
    bool         synced     = false;   // the core is at the cursor (PLAY can go on without loading)
    bool         after_load = false;   // PLAY passed a savestate load: the next state isn't checked
    RewindCursor cursor     = {};
    uint8_t*     live       = nullptr;   // the game when the DVR opened (A)
    uint8_t*     check      = nullptr;   // PLAY's determinism check
    uint64_t     mismatches = 0;         // replayed states that differ from the recorded ones
    // The kept lane: its own history, created with the recorded one's config
    // at the first B that leaves a future (Andrés: the same size).
    RewindBuffer kept        = {};
    bool         has_kept    = false;
    bool         on_kept     = false;   // the DVR's cursor walks the kept lane
    uint64_t     fork_frame  = 0;       // where the lanes part
    uint64_t     kept_newest = 0;       // read at open (nothing records while it's open)
};

// Starts recording for the game just loaded in `host` (settings set before).
// Nothing to record (REWIND OFF, serialize size 0) leaves state off /
// unsupported. False only if the history couldn't be created (logged).
bool core_rewind_start(CoreRewind* rewind, LibretroHost* host, double fps);
void core_rewind_stop(CoreRewind* rewind);
bool core_rewind_recording(const CoreRewind* rewind);

// Around every frame the game runs (not while the DVR is open): before
// libretro_host_run_frame with the input it'll get, after with how long it took.
void core_rewind_before_frame(CoreRewind* rewind, const LibretroInputState* input);
void core_rewind_after_frame(CoreRewind* rewind, uint64_t frame_ns);

// The DVR. Open: false if there's nothing to go back to. The cursor starts at
// the newest state; the screen keeps the live picture until it moves.
bool core_rewind_dvr_open(CoreRewind* rewind);
// Moves by `steps` states (frames in SCRUB, snapshots in SNAPSHOTS; < 0 =
// back) and shows the picture there. False if it couldn't move at all.
bool core_rewind_dvr_step(CoreRewind* rewind, int32_t steps);
// To the newest state at or before `frame`, shown.
bool core_rewind_dvr_seek(CoreRewind* rewind, uint64_t frame);
// Y: PLAY on / off.
void core_rewind_dvr_set_playing(CoreRewind* rewind, bool playing);
// While playing, once per frame due: runs the next recorded frame (its
// picture in host->video, its audio in host->audio). False when nothing ran
// (not playing, or the end of the history: playing stops).
bool core_rewind_dvr_tick(CoreRewind* rewind);
// B: plays on from the cursor, on the cursor's lane; the future it leaves
// becomes the kept lane (only a future with a state in it: otherwise the
// kept lane stays).
void core_rewind_dvr_resume_here(CoreRewind* rewind);
// To the other lane, at its state nearest the cursor's frame, shown. False
// if there's no kept lane.
bool core_rewind_dvr_switch_lane(CoreRewind* rewind);
// A: back to the game as it was when the DVR opened.
void core_rewind_dvr_close(CoreRewind* rewind);

// Where the cursor is, for the DVR's strip.
struct CoreRewindPosition
{
    uint64_t oldest = 0;   // frame of the oldest state (on the cursor's lane)
    uint64_t newest = 0;   // the frame the game had reached
    uint64_t cursor = 0;   // frame at the cursor
    uint32_t states = 0;   // on the cursor's lane

    // The kept lane (lanes false: there's none).
    bool     lanes       = false;
    bool     on_kept     = false;   // the cursor is on the kept lane
    uint64_t fork        = 0;       // the frame where the lanes part
    uint64_t kept_newest = 0;       // the frame the kept lane had reached
};
void core_rewind_dvr_position(const CoreRewind* rewind, CoreRewindPosition* position);
// The history the DVR's cursor walks (the recorded one, or the kept lane's),
// for the frontend's previews. Only while the DVR is open.
const RewindBuffer* core_rewind_dvr_buffer(const CoreRewind* rewind);
