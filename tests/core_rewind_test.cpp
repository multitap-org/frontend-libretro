// Checks core_rewind.h with host-libretro's test core, whose state is its
// frame counter plus a mix of port 0's buttons, the speed option and resets:
// any frame recorded, replayed or played on differs if a single input or
// event is wrong.
//
//   SCRUB: the DVR steps back frame by frame and shows exactly the game at
//   that frame; PLAY replays it; A goes back to the present; B plays on from
//   the cursor (a new branch) and the history stays right.
//   SNAPSHOTS: PLAY replays the input and the events (an option, a reset)
//   between snapshots exactly (no determinism mismatches).
//   AUTO: measures and picks SCRUB for this cheap core; a known verdict
//   skips the measurement.
//   LANES: B keeps the future it left as the kept lane; switching shows the
//   other lane's game at the same frame; B on the kept lane plays on there
//   and the lane it left becomes the kept one; B with no state ahead keeps
//   the kept lane; when the history is full the kept lane goes before any
//   old frame does (SCRUB and SNAPSHOTS).
//
// Usage: frontend_core_rewind_test <test core>
// Exit code 0 = all checks passed. Each failure is printed.

#include <cstdio>
#include <cstring>
#include <vector>

#include "core_rewind.h"
#include "host_log.h"
#include "libretro_host.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

// The game at every frame, as played (the test core's 8-byte state).
static std::vector<uint64_t> g_expected;

static uint64_t core_state(LibretroHost* host)
{
    std::vector<uint8_t> state;
    uint64_t value = 0;
    if (libretro_host_serialize(host, &state) && state.size() >= 8)
    {
        std::memcpy(&value, state.data(), 8);
    }
    return value;
}

static void remember(uint64_t frame, uint64_t value)
{
    if (g_expected.size() <= frame)
    {
        g_expected.resize(frame + 1);
    }
    g_expected[frame] = value;
}

// One frame of play: buttons that change often (from `seed`), recorded.
// g_expected[N]: the game after frame N - 1 ran (before any event at N),
// what PLAY shows after N ticks from frame 0.
static void play_frame(CoreRewind* rewind, LibretroHost* host, uint32_t seed)
{
    if (g_expected.size() <= rewind->frame)
    {
        remember(rewind->frame, core_state(host));
    }
    LibretroInputState input = {};
    input.joypad_buttons[0] = static_cast<uint16_t>((rewind->frame * 2654435761u + seed) >> 7);
    libretro_host_set_input(host, &input);
    core_rewind_before_frame(rewind, &input);
    libretro_host_run_frame(host);
    core_rewind_after_frame(rewind, 100000);
    remember(rewind->frame, core_state(host));
    // A game makes 60 frames a second, this loop as many as it can: waiting
    // for the worker stands in for the frame pacing (no skipped states).
    if (core_rewind_recording(rewind))
    {
        rewind_recorder_drain(&rewind->recorder);
    }
}

static void play(CoreRewind* rewind, LibretroHost* host, uint32_t frames, uint32_t seed)
{
    for (uint32_t i = 0; i < frames; i++)
    {
        play_frame(rewind, host, seed);
    }
}

static bool at_expected(LibretroHost* host, uint64_t frame)
{
    return frame < g_expected.size() && core_state(host) == g_expected[frame];
}

// The same against a lane's game saved before another one was played.
static bool at_game(LibretroHost* host, const std::vector<uint64_t>* game, uint64_t frame)
{
    return frame < game->size() && core_state(host) == (*game)[frame];
}

static void print_position(const CoreRewindPosition* position)
{
    std::printf("  oldest %llu newest %llu cursor %llu states %u lanes %d on_kept %d fork %llu kept_newest %llu\n",
                static_cast<unsigned long long>(position->oldest), static_cast<unsigned long long>(position->newest),
                static_cast<unsigned long long>(position->cursor), position->states, position->lanes ? 1 : 0,
                position->on_kept ? 1 : 0, static_cast<unsigned long long>(position->fork),
                static_cast<unsigned long long>(position->kept_newest));
}

static void test_scrub(LibretroHost* host)
{
    CoreRewind rewind = {};
    rewind.settings.mode = RewindMode::scrub;
    rewind.settings.memory_bytes = static_cast<size_t>(16) << 20;
    g_expected.clear();
    check(core_rewind_start(&rewind, host, 60.0) && rewind.state == CoreRewindState::scrub, "scrub: started");

    play(&rewind, host, 300, 1);
    uint64_t present = rewind.frame;
    check(core_rewind_dvr_open(&rewind), "scrub: DVR opens");
    CoreRewindPosition position = {};
    core_rewind_dvr_position(&rewind, &position);
    if (position.oldest != 0 || position.newest != present || position.cursor != present - 1 || position.states != 300)
    {
        std::printf("  oldest %llu newest %llu cursor %llu states %u\n", static_cast<unsigned long long>(position.oldest),
                    static_cast<unsigned long long>(position.newest), static_cast<unsigned long long>(position.cursor),
                    position.states);
    }
    check(position.oldest == 0 && position.newest == present && position.cursor == present - 1 && position.states == 300,
          "scrub: the whole history, cursor at the newest state");

    // Back 100 frames: the state there, and the core shows that frame.
    check(core_rewind_dvr_step(&rewind, -100), "scrub: step back");
    core_rewind_dvr_position(&rewind, &position);
    uint64_t at = position.cursor;
    check(at == present - 101, "scrub: one state per frame");
    uint64_t cursor_state = 0;
    std::memcpy(&cursor_state, rewind.cursor.state, 8);
    check(cursor_state == g_expected[at], "scrub: the cursor holds the game at that frame");
    check(at_expected(host, at + 1), "scrub: shown = that frame run");
    check(core_rewind_dvr_step(&rewind, 1), "scrub: step forward");
    check(at_expected(host, at + 2), "scrub: forward one frame");
    check(core_rewind_dvr_seek(&rewind, 10) && at_expected(host, 11), "scrub: seek");

    // PLAY: the recorded game, frame by frame.
    core_rewind_dvr_set_playing(&rewind, true);
    bool replayed = true;
    for (uint64_t i = 1; i <= 50; i++)
    {
        replayed = replayed && core_rewind_dvr_tick(&rewind) && at_expected(host, 10 + i);
    }
    check(replayed, "scrub: PLAY replays the recorded frames");
    core_rewind_dvr_set_playing(&rewind, false);

    // A: the present again, history untouched.
    core_rewind_dvr_close(&rewind);
    check(at_expected(host, present) && rewind.frame == present, "scrub: A goes back to the present");
    play(&rewind, host, 20, 1);
    check(core_rewind_dvr_open(&rewind), "scrub: DVR opens again");
    core_rewind_dvr_position(&rewind, &position);
    check(position.states == 320, "scrub: recording went on after A");

    // B at a state: play on from there with other input (a new branch).
    check(core_rewind_dvr_seek(&rewind, 200), "scrub: seek 200");
    core_rewind_dvr_resume_here(&rewind);
    check(rewind.frame == 200 && at_expected(host, 200), "scrub: B loads the cursor's state");
    play(&rewind, host, 40, 99);
    check(core_rewind_dvr_open(&rewind), "scrub: DVR after B");
    core_rewind_dvr_position(&rewind, &position);
    check(position.newest == 240 && position.states == 240, "scrub: after B the recorded lane is the new future");
    check(core_rewind_dvr_seek(&rewind, 220) && at_expected(host, 221), "scrub: the new branch recorded");
    check(core_rewind_dvr_seek(&rewind, 150) && at_expected(host, 151), "scrub: before B still right");

    // B after PLAY (the cursor on an input): plays on from the core as it is.
    core_rewind_dvr_set_playing(&rewind, true);
    for (int i = 0; i < 5; i++)
    {
        core_rewind_dvr_tick(&rewind);
    }
    core_rewind_dvr_resume_here(&rewind);
    check(rewind.frame == 155 && at_expected(host, 155), "scrub: B after PLAY");
    play(&rewind, host, 10, 7);
    check(core_rewind_dvr_open(&rewind), "scrub: DVR after B after PLAY");
    check(core_rewind_dvr_seek(&rewind, 158) && at_expected(host, 159), "scrub: recorded on from PLAY's frame");
    core_rewind_dvr_close(&rewind);

    core_rewind_stop(&rewind);
}

static void test_snapshots(LibretroHost* host)
{
    CoreRewind rewind = {};
    rewind.settings.mode = RewindMode::snapshots;
    rewind.settings.memory_bytes = static_cast<size_t>(16) << 20;
    rewind.settings.snapshot_seconds = 1;   // a state every 60 frames
    g_expected.clear();
    check(core_rewind_start(&rewind, host, 60.0) && rewind.state == CoreRewindState::snapshots, "snapshots: started");
    check(rewind.state_every == 60, "snapshots: every 60 frames");

    play(&rewind, host, 200, 3);
    libretro_host_set_option(host, "test_core_speed", "fast");   // an event at frame 200
    play(&rewind, host, 200, 3);
    libretro_host_reset(host);                                   // and at 400
    play(&rewind, host, 200, 3);
    libretro_host_set_option(host, "test_core_speed", "normal");
    uint64_t present = rewind.frame;

    check(core_rewind_dvr_open(&rewind), "snapshots: DVR opens");
    CoreRewindPosition position = {};
    core_rewind_dvr_position(&rewind, &position);
    check(position.states == 10 && position.cursor == 540, "snapshots: ten states, the newest at 540");
    check(core_rewind_dvr_step(&rewind, -9), "snapshots: back nine");
    core_rewind_dvr_position(&rewind, &position);
    check(position.cursor == 0 && at_expected(host, 1), "snapshots: at the first snapshot");

    // PLAY across both events and every snapshot: exact all the way.
    libretro_host_set_option(host, "test_core_speed", "normal");   // as it was at frame 0
    core_rewind_dvr_set_playing(&rewind, true);
    bool replayed = true;
    uint64_t ticks = 0;
    while (core_rewind_dvr_tick(&rewind))
    {
        ticks++;
        if (replayed && !at_expected(host, ticks))
        {
            std::printf("  first wrong frame: %llu\n", static_cast<unsigned long long>(ticks));
        }
        replayed = replayed && at_expected(host, ticks);
    }
    if (!replayed || ticks != present)
    {
        std::printf("  replayed %d, %llu ticks of %llu\n", replayed ? 1 : 0, static_cast<unsigned long long>(ticks),
                    static_cast<unsigned long long>(present));
    }
    check(replayed && ticks == present, "snapshots: PLAY replays inputs and events to the end");
    check(rewind.mismatches == 0, "snapshots: no determinism mismatch");
    check(!rewind.playing, "snapshots: PLAY stops at the end");

    core_rewind_dvr_close(&rewind);
    check(at_expected(host, present), "snapshots: A");
    core_rewind_stop(&rewind);
}

static void test_auto(LibretroHost* host)
{
    CoreRewind rewind = {};
    rewind.settings.mode = RewindMode::automatic;
    rewind.settings.memory_bytes = static_cast<size_t>(16) << 20;
    g_expected.clear();
    check(core_rewind_start(&rewind, host, 60.0) && rewind.state == CoreRewindState::measuring, "auto: measuring");
    play(&rewind, host, 300 + CORE_REWIND_MEASURE_FRAMES, 5);
    check(rewind.verdict && rewind.state == CoreRewindState::scrub, "auto: a cheap core gets SCRUB");
    std::printf("auto: record %.2f%%, load %.2f%%, total %.2f%%, skipped %.2f%%\n", rewind.measure.record * 100.0,
                rewind.measure.load * 100.0, rewind.measure.total * 100.0, rewind.measure.skip * 100.0);
    play(&rewind, host, 10, 5);
    check(core_rewind_dvr_open(&rewind), "auto: the measured seconds are kept");
    core_rewind_dvr_close(&rewind);
    core_rewind_stop(&rewind);

    rewind.settings.known = RewindMode::snapshots;
    check(core_rewind_start(&rewind, host, 60.0) && rewind.state == CoreRewindState::snapshots,
          "auto: a known verdict isn't measured again");
    core_rewind_stop(&rewind);

    rewind.settings.mode = RewindMode::off;
    check(core_rewind_start(&rewind, host, 60.0) && rewind.state == CoreRewindState::off, "off: nothing recorded");
    play(&rewind, host, 5, 5);
    check(!core_rewind_dvr_open(&rewind), "off: no DVR");
    core_rewind_stop(&rewind);
}

// Two lanes in SCRUB: A (frames 0..300), B played from 200, C from A's 290.
static void test_lanes(LibretroHost* host)
{
    CoreRewind rewind = {};
    rewind.settings.mode = RewindMode::scrub;
    rewind.settings.memory_bytes = static_cast<size_t>(16) << 20;
    g_expected.clear();
    check(core_rewind_start(&rewind, host, 60.0), "lanes: started");
    play(&rewind, host, 300, 1);
    std::vector<uint64_t> game_a = g_expected;

    // Fork: B at 200 keeps 201..300 as the kept lane.
    check(core_rewind_dvr_open(&rewind), "lanes: DVR opens");
    CoreRewindPosition position = {};
    core_rewind_dvr_position(&rewind, &position);
    check(!position.lanes && !core_rewind_dvr_switch_lane(&rewind), "lanes: one lane before any B");
    check(core_rewind_dvr_seek(&rewind, 200), "lanes: seek 200");
    core_rewind_dvr_resume_here(&rewind);
    play(&rewind, host, 60, 99);
    std::vector<uint64_t> game_b = g_expected;
    check(game_b[230] != game_a[230], "lanes: B differs from A");
    check(core_rewind_dvr_open(&rewind), "lanes: DVR after B");
    core_rewind_dvr_position(&rewind, &position);
    if (!position.lanes || position.on_kept || position.fork != 200 || position.kept_newest != 300 ||
        position.newest != 260)
    {
        print_position(&position);
    }
    check(position.lanes && !position.on_kept && position.fork != 0 && position.fork == 200 &&
              position.kept_newest == 300 && position.newest == 260,
          "lanes: fork, B recorded, A's future kept");

    // Switch: the same frame on the other lane.
    check(core_rewind_dvr_seek(&rewind, 230) && at_game(host, &game_b, 231), "lanes: B at 230");
    check(core_rewind_dvr_switch_lane(&rewind), "lanes: switch to the kept lane");
    core_rewind_dvr_position(&rewind, &position);
    check(position.on_kept && position.cursor == 230 && at_game(host, &game_a, 231), "lanes: A at 230");
    check(core_rewind_dvr_step(&rewind, 20) && at_game(host, &game_a, 251), "lanes: steps along A");
    check(core_rewind_dvr_seek(&rewind, 280) && at_game(host, &game_a, 281), "lanes: A past B's end");
    check(core_rewind_dvr_switch_lane(&rewind), "lanes: switch back");
    core_rewind_dvr_position(&rewind, &position);
    check(!position.on_kept && position.cursor == 259 && at_game(host, &game_b, 260), "lanes: B's newest state");
    check(core_rewind_dvr_seek(&rewind, 100) && core_rewind_dvr_switch_lane(&rewind) &&
              at_game(host, &game_a, 101),
          "lanes: before the fork both lanes are the same game");

    // PLAY on the kept lane replays A.
    check(core_rewind_dvr_seek(&rewind, 270), "lanes: A at 270");
    core_rewind_dvr_set_playing(&rewind, true);
    bool replayed = true;
    for (uint64_t i = 1; i <= 10; i++)
    {
        replayed = replayed && core_rewind_dvr_tick(&rewind) && at_game(host, &game_a, 270 + i);
    }
    check(replayed, "lanes: PLAY on the kept lane replays it");
    core_rewind_dvr_set_playing(&rewind, false);

    // B on the kept lane: A goes on from 290, B becomes the kept lane.
    check(core_rewind_dvr_seek(&rewind, 290), "lanes: A at 290");
    core_rewind_dvr_resume_here(&rewind);
    check(rewind.frame == 290 && at_game(host, &game_a, 290), "lanes: B on the kept lane loads A");
    play(&rewind, host, 30, 5);
    std::vector<uint64_t> game_c = g_expected;
    check(core_rewind_dvr_open(&rewind), "lanes: DVR after B on the kept lane");
    core_rewind_dvr_position(&rewind, &position);
    if (!position.lanes || position.fork != 200 || position.kept_newest != 260 || position.newest != 320)
    {
        print_position(&position);
    }
    check(position.lanes && !position.on_kept && position.fork == 200 && position.kept_newest == 260 &&
              position.newest == 320,
          "lanes: the switched-from lane (B) is now the kept one");
    check(core_rewind_dvr_seek(&rewind, 250) && at_game(host, &game_a, 251), "lanes: the recorded lane is A");
    check(core_rewind_dvr_seek(&rewind, 300) && at_game(host, &game_c, 301), "lanes: and A's new future");
    check(core_rewind_dvr_switch_lane(&rewind), "lanes: switch from 300");
    core_rewind_dvr_position(&rewind, &position);
    check(position.on_kept && position.cursor == 259 && at_game(host, &game_b, 260), "lanes: B's newest from 300");
    check(core_rewind_dvr_seek(&rewind, 250) && at_game(host, &game_b, 251), "lanes: the kept lane is B");

    // B right after opening (nothing moved, only the newest frame's input
    // after the cursor): the kept lane stays.
    core_rewind_dvr_close(&rewind);
    check(core_rewind_dvr_open(&rewind), "lanes: open again");
    core_rewind_dvr_resume_here(&rewind);
    play(&rewind, host, 5, 5);
    check(core_rewind_dvr_open(&rewind), "lanes: open after B at the newest");
    core_rewind_dvr_position(&rewind, &position);
    check(position.lanes && position.kept_newest == 260, "lanes: B at the newest keeps the kept lane");
    core_rewind_dvr_close(&rewind);
    core_rewind_stop(&rewind);
}

// The history full: the kept lane goes before any old frame does. Tiny
// states: the record index fills first (SCRUB), or the arena (SNAPSHOTS).
static void test_lane_eviction(LibretroHost* host, RewindMode mode)
{
    const char* name = mode == RewindMode::scrub ? "eviction scrub" : "eviction snapshots";
    char what[96] = {};
    CoreRewind rewind = {};
    rewind.settings.mode = mode;
    rewind.settings.memory_bytes = (static_cast<size_t>(4) << 20) + 4096;
    rewind.settings.snapshot_seconds = 1;
    g_expected.clear();
    check(core_rewind_start(&rewind, host, 60.0), "eviction: started");
    // Until the history is full (old frames dropped).
    uint32_t played = 0;
    while (rewind.recorder.buffer.dropped == 0 && played < 400000)
    {
        play(&rewind, host, 100, 11);
        played += 100;
    }
    std::snprintf(what, sizeof(what), "%s: the history fills (%u frames)", name, played);
    check(rewind.recorder.buffer.dropped > 0, what);

    check(core_rewind_dvr_open(&rewind), "eviction: open");
    check(core_rewind_dvr_step(&rewind, mode == RewindMode::scrub ? -120 : -2), "eviction: step back");
    core_rewind_dvr_resume_here(&rewind);
    CoreRewindPosition position = {};
    check(core_rewind_dvr_open(&rewind), "eviction: open after B");
    core_rewind_dvr_position(&rewind, &position);
    std::snprintf(what, sizeof(what), "%s: a kept lane", name);
    check(position.lanes, what);
    uint64_t oldest  = position.oldest;
    uint64_t dropped = rewind.recorder.buffer.dropped;
    core_rewind_dvr_close(&rewind);

    // Play until the kept lane goes: no old record may go before it.
    bool kept_first = true;
    bool gone = false;
    for (uint32_t i = 0; i < 100000 && !gone; i++)
    {
        play(&rewind, host, 1, 13);
        gone = rewind.recorder.buffer.kept == 0;
        kept_first = kept_first && rewind.recorder.buffer.dropped == dropped;
    }
    std::snprintf(what, sizeof(what), "%s: the kept lane goes first", name);
    check(gone && kept_first, what);
    check(core_rewind_dvr_open(&rewind), "eviction: open at the end");
    core_rewind_dvr_position(&rewind, &position);
    std::snprintf(what, sizeof(what), "%s: the oldest frame kept, one lane", name);
    check(!position.lanes && position.oldest == oldest, what);
    bool sought = core_rewind_dvr_seek(&rewind, position.oldest + 1);
    core_rewind_dvr_position(&rewind, &position);
    check(sought && at_expected(host, position.cursor + 1),
          "eviction: the oldest frame shown right");
    core_rewind_dvr_close(&rewind);
    core_rewind_stop(&rewind);
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("FAIL usage: frontend_core_rewind_test <test core>\n");
        return 1;
    }
    host_log_set_min_level(LogLevel::warning);
    LibretroHost* host = new LibretroHost{};
    LibretroHostConfig config;
    check(libretro_host_load_core(host, argv[1], &config) && libretro_host_load_game(host, nullptr), "test core loaded");

    test_scrub(host);
    test_snapshots(host);
    test_auto(host);
    test_lanes(host);
    test_lane_eviction(host, RewindMode::scrub);
    test_lane_eviction(host, RewindMode::snapshots);

    libretro_host_unload_core(host);
    delete host;
    if (g_failures == 0)
    {
        std::printf("core rewind: all checks passed\n");
        return 0;
    }
    std::printf("core rewind: %d checks failed\n", g_failures);
    return 1;
}
