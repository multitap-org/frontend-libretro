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
    check(position.newest == 240 && position.states == 240, "scrub: the future after B was dropped");
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
