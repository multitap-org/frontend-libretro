#include "core_rewind.h"

#include <cstring>
#include <new>

#include <SDL3/SDL.h>

#include "host_log.h"

// The recorder's pool and queue (rewind_recorder.h).
constexpr uint32_t REWIND_SLOTS      = 4;
constexpr uint32_t REWIND_QUEUE      = 256;
constexpr uint32_t REWIND_EVENT_MAX  = static_cast<uint32_t>(LIBRETRO_OPTION_EVENT_MAX + LIBRETRO_TEXT_ANSWER_MAX + 2);
// The SCRUB valve looks at this many frames at a time; more than a tenth of
// the states skipped there spaces them out.
constexpr uint64_t REWIND_VALVE_FRAMES = 60;
constexpr uint32_t REWIND_VALVE_MAX    = 3;

static const char* state_name(CoreRewindState state)
{
    switch (state)
    {
    case CoreRewindState::off:         return "off";
    case CoreRewindState::unsupported: return "unsupported";
    case CoreRewindState::measuring:   return "measuring";
    case CoreRewindState::scrub:       return "SCRUB";
    case CoreRewindState::snapshots:   return "SNAPSHOTS";
    }
    return "?";
}

bool core_rewind_recording(const CoreRewind* rewind)
{
    return rewind->has_recorder && (rewind->state == CoreRewindState::measuring ||
                                    rewind->state == CoreRewindState::scrub ||
                                    rewind->state == CoreRewindState::snapshots);
}

// The history for the current mode: the memory setting minus the state
// buffers (the pool, the buffer's newest, the DVR's cursor, live and check
// copies), an eighth of the rest for the record index.
static bool create_recorder(CoreRewind* rewind)
{
    bool   scrub = rewind->state != CoreRewindState::snapshots;
    size_t pool  = (REWIND_SLOTS + 4) * rewind->state_size;
    size_t memory = rewind->settings.memory_bytes;
    if (memory < pool + (static_cast<size_t>(4) << 20))
    {
        host_log(LogLevel::warning, "Rewind: %zu MB of memory can't hold states of %zu bytes", memory >> 20,
                 rewind->state_size);
        return false;
    }
    size_t usable = memory - pool;
    size_t index  = usable / 8;
    size_t records = index / sizeof(RewindRecord);
    if (records > 0xFFFFFFF0u)
    {
        records = 0xFFFFFFF0u;
    }

    if (scrub)
    {
        rewind->state_every = 1;
    }
    else
    {
        double every = rewind->settings.snapshot_seconds * rewind->fps;
        rewind->state_every = every >= 1.0 ? static_cast<uint32_t>(every + 0.5) : 1;
    }

    RewindRecorderConfig config = {};
    config.buffer.state_size  = rewind->state_size;
    config.buffer.input_size  = sizeof(LibretroInputState);
    config.buffer.arena_bytes = usable - index;
    config.buffer.max_records = static_cast<uint32_t>(records);
    if (scrub && rewind->settings.scrub_seconds > 0)
    {
        config.buffer.max_frames = static_cast<uint64_t>(rewind->settings.scrub_seconds * rewind->fps);
    }
    else if (!scrub && rewind->settings.snapshots_kept > 0)
    {
        config.buffer.max_frames = static_cast<uint64_t>(rewind->settings.snapshots_kept) * rewind->state_every;
    }
    config.slots          = REWIND_SLOTS;
    config.queue          = REWIND_QUEUE;
    config.max_event_size = REWIND_EVENT_MAX;
    if (!rewind_recorder_create(&rewind->recorder, &config))
    {
        return false;
    }
    rewind->has_recorder  = true;
    rewind->has_state     = false;
    rewind->force_state   = false;
    rewind->valve_frame   = rewind->frame;
    rewind->valve_skipped = 0;
    host_log(LogLevel::info, "Rewind: %s, states of %zu bytes, %zu MB for %u records%s", state_name(rewind->state),
             rewind->state_size, config.buffer.arena_bytes >> 20, config.buffer.max_records,
             scrub ? "" : " (a snapshot every few seconds)");
    return true;
}

static void destroy_recorder(CoreRewind* rewind)
{
    if (rewind->has_recorder)
    {
        rewind_recorder_destroy(&rewind->recorder);
        rewind->has_recorder = false;
    }
    // The kept lane goes with the history it was cut from (its sizes too).
    if (rewind->kept.arena != nullptr)
    {
        rewind_buffer_destroy(&rewind->kept);
    }
    rewind->has_kept  = false;
    rewind->on_kept   = false;
    rewind->has_state = false;
}

// What libretro_host heard the frontend do to the core: into the history
// next to the input (never while the DVR replays it).
static void on_event(void* userdata, LibretroHostEvent event, const uint8_t* data, uint32_t size)
{
    CoreRewind* rewind = static_cast<CoreRewind*>(userdata);
    if (!core_rewind_recording(rewind) || rewind->dvr_open || !rewind->has_state)
    {
        return;
    }
    rewind_recorder_push_event(&rewind->recorder, static_cast<uint8_t>(event), data, size, rewind->frame);
    if (event == LibretroHostEvent::state_loaded)
    {
        // The game jumped: a state right away, so nothing is replayed across it.
        rewind->force_state = true;
    }
}

static void free_copies(CoreRewind* rewind)
{
    delete[] rewind->live;
    delete[] rewind->check;
    rewind->live  = nullptr;
    rewind->check = nullptr;
}

static bool alloc_copies(CoreRewind* rewind)
{
    free_copies(rewind);
    rewind->live  = new (std::nothrow) uint8_t[rewind->state_size];
    rewind->check = new (std::nothrow) uint8_t[rewind->state_size];
    return rewind->live != nullptr && rewind->check != nullptr;
}

// Reads the core's state size and starts the history for the mode.
static bool begin(CoreRewind* rewind)
{
    LibretroHost* host = rewind->host;
    rewind->state_size = libretro_host_state_size(host);
    if (rewind->state_size == 0)
    {
        rewind->state = CoreRewindState::unsupported;
        return true;
    }

    RewindMode mode = rewind->settings.mode;
    if (mode == RewindMode::automatic)
    {
        mode = rewind->settings.known;
    }
    if (mode == RewindMode::automatic &&
        (host->serialization_quirks & (RETRO_SERIALIZATION_QUIRK_INCOMPLETE | RETRO_SERIALIZATION_QUIRK_SINGLE_SESSION)) != 0)
    {
        mode = RewindMode::snapshots;   // its states aren't whole: never a state every frame
    }
    if (mode == RewindMode::scrub)
    {
        rewind->state = CoreRewindState::scrub;
    }
    else if (mode == RewindMode::snapshots)
    {
        rewind->state = CoreRewindState::snapshots;
    }
    else
    {
        rewind->state = CoreRewindState::measuring;   // records as SCRUB meanwhile
        rewind->measure.start_frame = rewind->frame + static_cast<uint64_t>(CORE_REWIND_WARMUP_SECONDS * rewind->fps);
    }

    if (!alloc_copies(rewind) || !create_recorder(rewind))
    {
        free_copies(rewind);
        rewind->state = CoreRewindState::off;
        return false;
    }
    libretro_host_set_event_listener(host, on_event, rewind);
    return true;
}

bool core_rewind_start(CoreRewind* rewind, LibretroHost* host, double fps)
{
    core_rewind_stop(rewind);
    rewind->host       = host;
    rewind->fps        = fps > 1.0 ? fps : 60.0;
    rewind->frame      = 0;
    rewind->measure    = {};
    rewind->verdict    = false;
    rewind->mismatches = 0;
    rewind->retry_size = false;

    if (rewind->settings.mode == RewindMode::off)
    {
        rewind->state = CoreRewindState::off;
        return true;
    }
    bool ok = begin(rewind);
    if (rewind->state == CoreRewindState::unsupported)
    {
        // Some cores only know their state size once a frame has run.
        rewind->retry_size = true;
    }
    return ok;
}

static void close_cursor(CoreRewind* rewind)
{
    if (rewind->dvr_open)
    {
        rewind_cursor_destroy(&rewind->cursor);
        rewind->dvr_open = false;
        rewind->playing  = false;
        rewind->on_kept  = false;   // the next open starts on the recorded lane
    }
}

void core_rewind_stop(CoreRewind* rewind)
{
    if (rewind->host != nullptr && rewind->host->event_userdata == rewind)
    {
        libretro_host_set_event_listener(rewind->host, nullptr, nullptr);
    }
    if (rewind->dvr_open)
    {
        close_cursor(rewind);
        rewind_recorder_resume(&rewind->recorder);
    }
    destroy_recorder(rewind);
    free_copies(rewind);
    rewind->state      = CoreRewindState::off;
    rewind->state_size = 0;
}

// The core's state size changed (some cores grow theirs): a new history.
static void restart_for_size(CoreRewind* rewind, size_t size)
{
    host_log(LogLevel::info, "Rewind: the core's states went from %zu to %zu bytes, history restarted",
             rewind->state_size, size);
    destroy_recorder(rewind);
    rewind->state_size = size;
    if (size == 0 || !alloc_copies(rewind) || !create_recorder(rewind))
    {
        free_copies(rewind);
        rewind->state = size == 0 ? CoreRewindState::unsupported : CoreRewindState::off;
    }
}

void core_rewind_before_frame(CoreRewind* rewind, const LibretroInputState* input)
{
    if (!core_rewind_recording(rewind) || rewind->dvr_open)
    {
        return;
    }
    bool due = !rewind->has_state || rewind->force_state ||
               rewind->frame - rewind->last_state_frame >= rewind->state_every;
    if (due)
    {
        size_t size = libretro_host_state_size(rewind->host);
        if (size != rewind->state_size)
        {
            restart_for_size(rewind, size);
            if (!core_rewind_recording(rewind))
            {
                return;
            }
        }
        bool measuring = rewind->state == CoreRewindState::measuring && rewind->frame >= rewind->measure.start_frame;
        if (measuring && rewind->measure.frames == 0)
        {
            rewind_recorder_stats(&rewind->recorder, &rewind->measure.start_stats);
        }

        uint64_t started = SDL_GetTicksNS();
        uint8_t* slot    = rewind_recorder_acquire_state(&rewind->recorder);
        if (slot != nullptr)
        {
            if (libretro_host_serialize_to(rewind->host, slot, rewind->state_size))
            {
                rewind_recorder_submit_state(&rewind->recorder, slot, rewind->frame);
                rewind->has_state        = true;
                rewind->last_state_frame = rewind->frame;
                rewind->force_state      = false;
            }
            else
            {
                rewind_recorder_release_state(&rewind->recorder, slot);
            }
        }
        if (measuring && rewind->measure.frames < CORE_REWIND_MEASURE_FRAMES)
        {
            rewind->measure.serialize_ns[rewind->measure.frames] = SDL_GetTicksNS() - started;
        }
    }
    if (rewind->has_state)
    {
        rewind_recorder_push_input(&rewind->recorder, reinterpret_cast<const uint8_t*>(input), rewind->frame);
    }
}

// The 95th percentile of `count` samples (sorts a copy; count <= MEASURE_FRAMES).
static uint64_t percentile_95(const uint64_t* samples, uint32_t count)
{
    uint64_t sorted[CORE_REWIND_MEASURE_FRAMES];
    for (uint32_t i = 0; i < count; i++)
    {
        uint64_t value = samples[i];
        uint32_t j     = i;
        while (j > 0 && sorted[j - 1] > value)
        {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = value;
    }
    return count == 0 ? 0 : sorted[(count * 95) / 100 < count ? (count * 95) / 100 : count - 1];
}

// AUTO's verdict (the criteria in core_rewind.h).
static void finish_measure(CoreRewind* rewind)
{
    CoreRewindMeasure* measure = &rewind->measure;
    double period = 1e9 / rewind->fps;

    // Loading: the game's own state, loaded again (the game doesn't change).
    uint64_t load_ns = 0;
    if (libretro_host_serialize_to(rewind->host, rewind->live, rewind->state_size))
    {
        for (uint32_t i = 0; i < CORE_REWIND_MEASURE_LOADS; i++)
        {
            uint64_t started = SDL_GetTicksNS();
            libretro_host_unserialize(rewind->host, rewind->live, rewind->state_size);
            uint64_t took = SDL_GetTicksNS() - started;
            if (took > load_ns)
            {
                load_ns = took;
            }
        }
    }
    measure->load_ns = load_ns;

    RewindRecorderStats stats = {};
    rewind_recorder_drain(&rewind->recorder);
    rewind_recorder_stats(&rewind->recorder, &stats);
    uint64_t states  = stats.states - measure->start_stats.states;
    uint64_t skipped = stats.skipped - measure->start_stats.skipped;
    double   encode  = states > 0 ? static_cast<double>(stats.encode_ns_sum - measure->start_stats.encode_ns_sum) /
                                      static_cast<double>(states)
                                  : period;
    double serialize = static_cast<double>(percentile_95(measure->serialize_ns, measure->frames));
    double frame     = static_cast<double>(percentile_95(measure->frame_ns, measure->frames));

    measure->record = serialize / period;
    measure->load   = static_cast<double>(load_ns) / period;
    measure->encode = encode / period;
    measure->total  = (frame + serialize) / period;
    measure->skip   = static_cast<double>(skipped) / static_cast<double>(measure->frames);
    bool scrub = measure->record <= CORE_REWIND_SCRUB_RECORD_MAX && measure->load <= CORE_REWIND_SCRUB_LOAD_MAX &&
                 measure->encode <= CORE_REWIND_SCRUB_ENCODE_MAX && measure->skip <= CORE_REWIND_SCRUB_SKIP_MAX;
    host_log(LogLevel::info,
             "Rewind AUTO: serialize %.1f%% (%.3f ms), load %.1f%%, encode %.1f%% (%.3f ms, worker), skipped %.1f%%; "
             "the core's frame + serialize %.1f%%: %s",
             measure->record * 100.0, serialize / 1e6, measure->load * 100.0, measure->encode * 100.0, encode / 1e6,
             measure->skip * 100.0, measure->total * 100.0, scrub ? "SCRUB" : "SNAPSHOTS");

    rewind->verdict = true;
    if (scrub)
    {
        rewind->state = CoreRewindState::scrub;   // the history so far stays
        return;
    }
    // The few seconds measured go: snapshots keep their own spacing.
    destroy_recorder(rewind);
    rewind->state = CoreRewindState::snapshots;
    if (!create_recorder(rewind))
    {
        free_copies(rewind);
        rewind->state = CoreRewindState::off;
    }
}

// SCRUB's valve: states spaced out when the worker can't keep up.
static void check_valve(CoreRewind* rewind)
{
    if (rewind->frame - rewind->valve_frame < REWIND_VALVE_FRAMES)
    {
        return;
    }
    RewindRecorderStats stats = {};
    rewind_recorder_stats(&rewind->recorder, &stats);
    uint64_t skipped  = stats.skipped - rewind->valve_skipped;
    uint64_t attempts = REWIND_VALVE_FRAMES / rewind->state_every;
    if (skipped * 10 > attempts && rewind->state_every < REWIND_VALVE_MAX)
    {
        rewind->state_every++;
        host_log(LogLevel::warning, "Rewind: %llu of %llu states skipped, now a state every %u frames",
                 static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(attempts),
                 rewind->state_every);
    }
    rewind->valve_frame   = rewind->frame;
    rewind->valve_skipped = stats.skipped;
}

void core_rewind_after_frame(CoreRewind* rewind, uint64_t frame_ns)
{
    if (rewind->retry_size)
    {
        rewind->retry_size = false;
        begin(rewind);
        if (rewind->state == CoreRewindState::unsupported)
        {
            host_log(LogLevel::info, "Rewind: the core has no savestates");
        }
        return;
    }
    if (!core_rewind_recording(rewind) || rewind->dvr_open)
    {
        return;
    }
    if (rewind->state == CoreRewindState::measuring && rewind->frame >= rewind->measure.start_frame)
    {
        rewind->measure.frame_ns[rewind->measure.frames] = frame_ns;
        rewind->measure.frames++;
        if (rewind->measure.frames == CORE_REWIND_MEASURE_FRAMES)
        {
            rewind->frame++;
            finish_measure(rewind);
            return;
        }
    }
    else if (rewind->state == CoreRewindState::scrub)
    {
        check_valve(rewind);
    }
    rewind->frame++;
}

// ---------------------------------------------------------------------------
// The DVR
// ---------------------------------------------------------------------------

// The history the cursor walks: the recorded one, or the kept lane.
static RewindBuffer* lane_buffer(CoreRewind* rewind)
{
    return rewind->on_kept ? &rewind->kept : &rewind->recorder.buffer;
}

const RewindBuffer* core_rewind_dvr_buffer(const CoreRewind* rewind)
{
    return rewind->on_kept ? &rewind->kept : &rewind->recorder.buffer;
}

static const RewindRecord* cursor_record(const CoreRewind* rewind)
{
    return rewind_buffer_record(core_rewind_dvr_buffer(rewind), rewind->cursor.position);
}

// The frame the game is at after a record: an input's frame has run.
static uint64_t frame_after(const RewindRecord* record)
{
    return record->kind == RewindRecordKind::input ? record->frame + 1 : record->frame;
}

// Where the kept lane ends.
static void read_lanes(CoreRewind* rewind)
{
    rewind->has_kept    = rewind->has_kept && rewind->kept.count > 0;
    rewind->kept_newest = rewind->has_kept ? frame_after(rewind_buffer_record(&rewind->kept, rewind->kept.count - 1)) : 0;
}

bool core_rewind_dvr_open(CoreRewind* rewind)
{
    if (!core_rewind_recording(rewind) || rewind->dvr_open || !rewind->has_state)
    {
        return false;
    }
    rewind_recorder_pause(&rewind->recorder);
    rewind->on_kept      = false;
    RewindBuffer* buffer = &rewind->recorder.buffer;
    bool ok = buffer->states > 0 && libretro_host_serialize_to(rewind->host, rewind->live, rewind->state_size) &&
              rewind_cursor_create(&rewind->cursor, buffer);
    if (ok)
    {
        ok = rewind_cursor_to_newest(buffer, &rewind->cursor);
        if (ok && cursor_record(rewind)->kind != RewindRecordKind::state)
        {
            ok = rewind_cursor_back_to_state(buffer, &rewind->cursor);
        }
        if (!ok)
        {
            rewind_cursor_destroy(&rewind->cursor);
        }
    }
    if (!ok)
    {
        rewind_recorder_resume(&rewind->recorder);
        return false;
    }
    rewind->dvr_open   = true;
    rewind->playing    = false;
    rewind->synced     = false;
    rewind->after_load = false;
    read_lanes(rewind);
    return true;
}

// The picture at the cursor (a state record): the state loaded and its
// frame run with its own input. The core is then one frame past the cursor.
static bool show(CoreRewind* rewind)
{
    RewindBuffer* buffer = lane_buffer(rewind);
    if (!libretro_host_unserialize(rewind->host, rewind->cursor.state, rewind->state_size))
    {
        return false;
    }
    LibretroInputState input = {};
    std::memcpy(&input, rewind->cursor.input, sizeof(input));
    if (rewind_cursor_forward(buffer, &rewind->cursor))
    {
        if (cursor_record(rewind)->kind == RewindRecordKind::input)
        {
            std::memcpy(&input, rewind->cursor.input, sizeof(input));
        }
        rewind_cursor_back(buffer, &rewind->cursor);
    }
    libretro_host_set_input(rewind->host, &input);
    libretro_host_run_frame(rewind->host);
    rewind->synced = false;
    return true;
}

bool core_rewind_dvr_step(CoreRewind* rewind, int32_t steps)
{
    if (!rewind->dvr_open)
    {
        return false;
    }
    rewind->playing      = false;
    RewindBuffer* buffer = lane_buffer(rewind);
    bool          moved  = false;
    int32_t       left   = steps < 0 ? -steps : steps;
    for (int32_t i = 0; i < left; i++)
    {
        bool ok = steps < 0 ? rewind_cursor_back_to_state(buffer, &rewind->cursor)
                            : rewind_cursor_forward_to_state(buffer, &rewind->cursor);
        if (!ok)
        {
            break;
        }
        moved = true;
    }
    // After PLAY the cursor may sit on an input: never show from there.
    if (cursor_record(rewind)->kind != RewindRecordKind::state && rewind_cursor_back_to_state(buffer, &rewind->cursor))
    {
        moved = true;
    }
    return moved && show(rewind);
}

bool core_rewind_dvr_seek(CoreRewind* rewind, uint64_t frame)
{
    if (!rewind->dvr_open)
    {
        return false;
    }
    rewind->playing      = false;
    RewindBuffer* buffer = lane_buffer(rewind);
    if (cursor_record(rewind)->kind != RewindRecordKind::state)
    {
        rewind_cursor_back_to_state(buffer, &rewind->cursor);
    }
    while (cursor_record(rewind)->frame > frame && rewind_cursor_back_to_state(buffer, &rewind->cursor))
    {
    }
    while (cursor_record(rewind)->frame < frame)
    {
        if (!rewind_cursor_forward_to_state(buffer, &rewind->cursor))
        {
            break;
        }
        if (cursor_record(rewind)->frame > frame)
        {
            rewind_cursor_back_to_state(buffer, &rewind->cursor);
            break;
        }
    }
    return show(rewind);
}

void core_rewind_dvr_set_playing(CoreRewind* rewind, bool playing)
{
    rewind->playing = rewind->dvr_open && playing;
}

bool core_rewind_dvr_tick(CoreRewind* rewind)
{
    if (!rewind->dvr_open || !rewind->playing)
    {
        return false;
    }
    RewindBuffer* buffer = lane_buffer(rewind);
    if (!rewind->synced)
    {
        if (cursor_record(rewind)->kind != RewindRecordKind::state &&
            !rewind_cursor_back_to_state(buffer, &rewind->cursor))
        {
            rewind->playing = false;
            return false;
        }
        libretro_host_unserialize(rewind->host, rewind->cursor.state, rewind->state_size);
        rewind->synced     = true;
        rewind->after_load = false;
    }
    for (;;)
    {
        if (!rewind_cursor_forward(buffer, &rewind->cursor))
        {
            rewind->playing = false;   // the end of the history
            return false;
        }
        const RewindRecord* record = cursor_record(rewind);
        if (record->kind == RewindRecordKind::event)
        {
            uint32_t       size  = 0;
            const uint8_t* data  = rewind_buffer_event_data(buffer, rewind->cursor.position, &size);
            LibretroHostEvent event = static_cast<LibretroHostEvent>(record->event);
            if (event == LibretroHostEvent::state_loaded)
            {
                rewind->after_load = true;
            }
            else
            {
                libretro_host_replay_event(rewind->host, event, data, size);
            }
            continue;
        }
        if (record->kind == RewindRecordKind::state)
        {
            // Between snapshots the input was replayed: the state recorded
            // here says whether the core replays exactly. Either way the
            // recorded one goes on (no drift).
            if (rewind->state_every > 1 && !rewind->after_load &&
                libretro_host_serialize_to(rewind->host, rewind->check, rewind->state_size) &&
                std::memcmp(rewind->check, rewind->cursor.state, rewind->state_size) != 0)
            {
                if (rewind->mismatches == 0)
                {
                    host_log(LogLevel::warning, "Rewind: the core didn't replay frame %llu exactly (not deterministic?)",
                             static_cast<unsigned long long>(record->frame));
                }
                rewind->mismatches++;
            }
            libretro_host_unserialize(rewind->host, rewind->cursor.state, rewind->state_size);
            rewind->after_load = false;
            continue;
        }
        LibretroInputState input = {};
        std::memcpy(&input, rewind->cursor.input, sizeof(input));
        libretro_host_set_input(rewind->host, &input);
        libretro_host_run_frame(rewind->host);
        return true;
    }
}

static uint64_t earlier(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

// B's lanes, the cursor at `here` (the frame recording goes on from):
//   - on the kept lane: it's cut there and becomes the recorded history; the
//     recorded one, whole, is the kept lane now (the two swap, nothing copied).
//   - on the recorded lane with a state after the cursor: the history is
//     copied into the kept lane first (the old kept lane goes), then cut.
//   - on the recorded lane with only inputs after the cursor (B right after
//     opening): just cut; the kept lane stays.
static void keep_future(CoreRewind* rewind, uint64_t here)
{
    RewindBuffer* recorded = &rewind->recorder.buffer;
    if (rewind->on_kept)
    {
        rewind_buffer_truncate(&rewind->kept, &rewind->cursor);
        RewindBuffer swap = *recorded;
        *recorded         = rewind->kept;
        rewind->kept      = swap;
        rewind->fork_frame = earlier(rewind->fork_frame, here);
        rewind->on_kept   = false;
        return;
    }
    bool ahead = false;
    for (uint32_t i = rewind->cursor.position + 1; i < recorded->count && !ahead; i++)
    {
        ahead = rewind_buffer_record(recorded, i)->kind == RewindRecordKind::state;
    }
    if (ahead)
    {
        bool ready = rewind->kept.arena != nullptr || rewind_buffer_create(&rewind->kept, &recorded->config);
        uint64_t started = SDL_GetTicksNS();
        if (ready && rewind_buffer_copy(&rewind->kept, recorded))
        {
            rewind->has_kept   = true;
            rewind->fork_frame = here;
            host_log(LogLevel::info, "Rewind: the future after frame %llu kept as a lane (%zu MB copied in %.1f ms)",
                     static_cast<unsigned long long>(here), rewind_buffer_bytes_used(recorded) >> 20,
                     static_cast<double>(SDL_GetTicksNS() - started) / 1e6);
        }
        else
        {
            host_log(LogLevel::warning, "Rewind: no memory for a kept lane, the future after frame %llu is dropped",
                     static_cast<unsigned long long>(here));
            rewind->has_kept = false;
        }
    }
    else if (rewind->has_kept)
    {
        rewind->fork_frame = earlier(rewind->fork_frame, here);
    }
    rewind_buffer_truncate(recorded, &rewind->cursor);
}

void core_rewind_dvr_resume_here(CoreRewind* rewind)
{
    if (!rewind->dvr_open)
    {
        return;
    }
    RewindBuffer*       buffer = lane_buffer(rewind);
    const RewindRecord* record = cursor_record(rewind);
    if (record->kind == RewindRecordKind::input && rewind->synced)
    {
        // PLAY stopped after running this input's frame: the core is the game
        // at the next frame, recorded as a state right away.
        rewind->frame       = record->frame + 1;
        rewind->force_state = true;
    }
    else
    {
        if (record->kind != RewindRecordKind::state)
        {
            rewind_cursor_back_to_state(buffer, &rewind->cursor);
            record = cursor_record(rewind);
        }
        libretro_host_unserialize(rewind->host, rewind->cursor.state, rewind->state_size);
        rewind->frame            = record->frame;
        rewind->last_state_frame = record->frame;   // the cursor's state is the newest: frame `frame` runs next
        rewind->force_state      = false;
    }
    keep_future(rewind, frame_after(record));
    rewind->valve_frame = rewind->frame;
    close_cursor(rewind);
    rewind_recorder_resume(&rewind->recorder);
}

bool core_rewind_dvr_switch_lane(CoreRewind* rewind)
{
    if (!rewind->dvr_open || !rewind->has_kept)
    {
        return false;
    }
    rewind->playing    = false;
    RewindBuffer* from = lane_buffer(rewind);
    RewindBuffer* to   = rewind->on_kept ? &rewind->recorder.buffer : &rewind->kept;
    if (cursor_record(rewind)->kind != RewindRecordKind::state)
    {
        rewind_cursor_back_to_state(from, &rewind->cursor);
    }
    // The other lane's newest state at or before the cursor's frame (or its oldest).
    uint64_t frame  = cursor_record(rewind)->frame;
    uint32_t target = 0;
    for (uint32_t i = 0; i < to->count; i++)
    {
        const RewindRecord* record = rewind_buffer_record(to, i);
        if (record->kind == RewindRecordKind::state && record->frame <= frame)
        {
            target = i;
        }
    }
    const RewindRecord* there = rewind_buffer_record(to, target);
    if (frame < rewind->fork_frame && there->frame == frame)
    {
        // Before the fork both lanes are the same game: the cursor's state
        // and input are already the ones there.
        rewind->cursor.position = target;
    }
    else
    {
        // Down from the other lane's newest.
        if (!rewind_cursor_to_newest(to, &rewind->cursor))
        {
            return false;
        }
        while (rewind->cursor.position > target && rewind_cursor_back(to, &rewind->cursor))
        {
        }
    }
    rewind->on_kept = !rewind->on_kept;
    return show(rewind);
}

void core_rewind_dvr_close(CoreRewind* rewind)
{
    if (!rewind->dvr_open)
    {
        return;
    }
    libretro_host_unserialize(rewind->host, rewind->live, rewind->state_size);
    close_cursor(rewind);
    rewind_recorder_resume(&rewind->recorder);
}

void core_rewind_dvr_position(const CoreRewind* rewind, CoreRewindPosition* position)
{
    *position        = {};
    position->newest = rewind->frame;
    position->cursor = rewind->frame;
    if (!rewind->dvr_open)
    {
        return;
    }
    const RewindBuffer* buffer = core_rewind_dvr_buffer(rewind);
    position->oldest = rewind_buffer_record(buffer, 0)->frame;
    position->cursor = cursor_record(rewind)->frame;
    position->states = buffer->states;
    position->lanes       = rewind->has_kept;
    position->on_kept     = rewind->has_kept && rewind->on_kept;
    position->fork        = rewind->fork_frame;
    position->kept_newest = rewind->kept_newest;
}
