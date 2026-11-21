#include "core_session.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include <SDL3/SDL.h>

#include "app_shell.h"
#include "content_archive.h"
#include "host_fs.h"
#include "host_log.h"

static void set_message(CoreSession* session, const char* format, ...) HOST_PRINTF_FORMAT(2, 3);
static void set_message(CoreSession* session, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vsnprintf(session->message, sizeof(session->message), format, args);
    va_end(args);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s", session->message);
}

// ---------------------------------------------------------------------------
// Video texture
// ---------------------------------------------------------------------------

static void destroy_texture(CoreSession* session, AppShell* shell)
{
    video_texture_destroy(&session->texture, shell);   // waits for the GPU: it may still be drawing it
    session->uploaded_serial = 0;
}

// The texture format for a libretro pixel format (the layouts are the same).
static FramePixelFormat texture_format_for(retro_pixel_format format)
{
    switch (format)
    {
        case RETRO_PIXEL_FORMAT_RGB565:   return FramePixelFormat::rgb565;
        case RETRO_PIXEL_FORMAT_0RGB1555: return FramePixelFormat::xrgb1555;
        default:                          return FramePixelFormat::xrgb8888;
    }
}

// Whether the host should keep frames in the core's own format (uploaded as
// is) or convert them to XRGB8888 (the software fallback): native when this
// GPU takes all three formats. MULTITAP_CONVERT_PIXELS=1 forces the fallback
// (to test it, or compare the two).
static bool use_native_pixel_formats(const AppShell* shell)
{
    const char* force = SDL_getenv("MULTITAP_CONVERT_PIXELS");
    if (force != nullptr && std::strcmp(force, "1") == 0)
    {
        return false;
    }
    return video_texture_format_supported(shell, FramePixelFormat::rgb565) &&
           video_texture_format_supported(shell, FramePixelFormat::xrgb1555) &&
           video_texture_format_supported(shell, FramePixelFormat::xrgb8888);
}

// The texture format the host's frames come in: the core's format when the
// host keeps it native, XRGB8888-with-alpha when it converts.
static FramePixelFormat frame_texture_format(const LibretroHost* host)
{
    return host->native_pixel_formats ? texture_format_for(host->pixel_format) : FramePixelFormat::xrgb8888;
}

// Makes sure the texture can hold width x height in `format`. Recreating it
// waits for the GPU, so it's sized to the core's maximum geometry up front
// (and the format only changes if the core changes its pixel format).
static bool ensure_texture(CoreSession* session, AppShell* shell, uint32_t width, uint32_t height,
                           FramePixelFormat format)
{
    VideoTexture* texture = &session->texture;
    if (video_texture_created(texture) && width <= texture->capacity_width && height <= texture->capacity_height &&
        texture->format == format)
    {
        return true;
    }
    uint32_t new_width = width > texture->capacity_width ? width : texture->capacity_width;
    uint32_t new_height = height > texture->capacity_height ? height : texture->capacity_height;

    destroy_texture(session, shell);
    // Opaque: a core's picture has no alpha (native frames leave those bits unused).
    if (!video_texture_create(texture, shell, new_width, new_height, format, true))
    {
        set_message(session, "Could not create a %ux%u texture for the core's video", new_width, new_height);
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Core video texture: %ux%u, %s", new_width, new_height,
                session->host->native_pixel_formats ? "the core's own pixel format" : "converted to XRGB8888");
    return true;
}

void core_session_upload_video(CoreSession* session, AppShell* shell)
{
    const LibretroVideoFrame* video = &session->host->video;
    if (!core_session_running(session) || video->pixels == nullptr || video->serial == session->uploaded_serial)
    {
        return;
    }
    // A frame in another format than the texture (the core changed it) waits
    // for run_frame's ensure_texture.
    if (video->bytes_per_pixel != session->texture.bytes_per_pixel ||
        (session->host->native_pixel_formats ? texture_format_for(video->format) : FramePixelFormat::xrgb8888) !=
            session->texture.format)
    {
        return;
    }
    if (!app_shell_acquire(shell))
    {
        return;   // no frame this time (e.g. swapchain being recreated); try next frame
    }
    uint64_t started = SDL_GetTicksNS();
    if (video_texture_upload(&session->texture, shell, video->pixels, video->width, video->height))
    {
        session->uploaded_serial = video->serial;
        session->stats.uploads++;
        session->stats.upload_ns += SDL_GetTicksNS() - started;
    }
}

bool core_session_copy_picture(CoreSession* session, AppShell* shell, VideoTexture* target)
{
    const LibretroVideoFrame* video = &session->host->video;
    if (!core_session_running(session) || video->pixels == nullptr || video->width == 0 || video->height == 0)
    {
        return false;
    }
    FramePixelFormat format = frame_texture_format(session->host);
    if (video->bytes_per_pixel != (format == FramePixelFormat::xrgb8888 ? 4u : 2u))
    {
        return false;   // the core just changed its format: run_frame catches up first
    }
    if (!video_texture_created(target) || target->format != format || video->width > target->capacity_width ||
        video->height > target->capacity_height)
    {
        // Sized like the session's texture (the core's maximum), so it's made once.
        uint32_t width = session->texture.capacity_width > video->width ? session->texture.capacity_width : video->width;
        uint32_t height = session->texture.capacity_height > video->height ? session->texture.capacity_height : video->height;
        video_texture_destroy(target, shell);
        if (!video_texture_create(target, shell, width, height, format, true))
        {
            return false;
        }
    }
    return app_shell_acquire(shell) && video_texture_upload(target, shell, video->pixels, video->width, video->height);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool core_session_create(CoreSession* session)
{
    *session = CoreSession{};
    // ~20 KB of path buffers: on the heap.
    session->host = new (std::nothrow) LibretroHost{};
    return session->host != nullptr;
}

void core_session_destroy(CoreSession* session, AppShell* shell)
{
    if (session->host != nullptr)
    {
        core_session_stop(session, shell);
        delete session->host;
    }
    if (session->gamepad != nullptr)
    {
        SDL_CloseGamepad(session->gamepad);
    }
    *session = CoreSession{};
}

bool core_session_running(const CoreSession* session)
{
    return session->host != nullptr && session->host->game_loaded;
}

void core_session_stop(CoreSession* session, AppShell* shell)
{
    if (core_session_running(session))
    {
        // How the session's pacing went: missed refreshes, catch-up frames,
        // audio underruns (what a log is read for after a stuttery run).
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Session: %llu missed refreshes, %llu frames caught up, %llu audio underruns",
                    static_cast<unsigned long long>(session->timing.missed_total),
                    static_cast<unsigned long long>(session->timing.caught_up_total),
                    static_cast<unsigned long long>(session->audio.underruns));
    }
    session->timing.missed_total = 0;
    session->timing.caught_up_total = 0;
    if (session->rewind != nullptr)
    {
        core_rewind_stop(session->rewind);
    }
    destroy_texture(session, shell);
    audio_output_destroy(&session->audio);
    libretro_host_unload_core(session->host);   // writes save RAM first
    session->paused = false;
    session->step_once = false;
    session->fast_forward = false;
    session->fast_forward_held = false;
    session->stats = CoreSessionStats{};
}

// The content folder's name ("Super Nintendo" for ".../Super Nintendo/smw.sfc"),
// the core options' folder layer.
static void folder_name(const char* content_path, char* out, size_t out_size)
{
    out[0] = '\0';
    if (content_path == nullptr)
    {
        return;
    }
    const char* end = std::strrchr(content_path, '/');
    if (end == nullptr || end == content_path)
    {
        return;
    }
    const char* start = end - 1;
    while (start > content_path && *(start - 1) != '/')
    {
        start--;
    }
    size_t length = static_cast<size_t>(end - start);
    if (length >= out_size)
    {
        length = out_size - 1;
    }
    std::memcpy(out, start, length);
    out[length] = '\0';
}

// The running core declared (or re-declared) its options: cache them.
static void cache_options(CoreSession* session)
{
    if (session->option_cache == nullptr || session->host->options.serial == session->options_serial)
    {
        return;
    }
    session->options_serial = session->host->options.serial;
    char core[256] = {};
    if (libretro_core_short_name(session->host->core_path, core, sizeof(core)))
    {
        core_options_cache_update(session->option_cache, core, &session->host->options);
    }
}

bool core_session_set_option(CoreSession* session, CoreOptionLayer layer, const char* key, const char* value)
{
    const LibretroOption* option = libretro_options_find(&session->host->options, key);
    if (session->option_store == nullptr || option == nullptr)
    {
        return false;
    }
    if (!core_options_store_set(session->option_store, layer, key, value, option->values[option->default_index]))
    {
        return false;
    }
    const char* effective = core_options_store_get(session->option_store, key, nullptr);
    return libretro_host_set_option(session->host, key, effective != nullptr ? effective : value);
}

bool core_session_reset_option(CoreSession* session, CoreOptionLayer layer, const char* key)
{
    const LibretroOption* option = libretro_options_find(&session->host->options, key);
    if (session->option_store == nullptr || option == nullptr)
    {
        return false;
    }
    core_options_store_reset(session->option_store, layer, key);
    const char* effective = core_options_store_get(session->option_store, key, nullptr);
    return libretro_host_set_option(session->host, key,
                                    effective != nullptr ? effective : option->values[option->default_index]);
}

// Loads the core with the stored option values for this core, folder and
// game (the host applies them as the core declares its options). `folder_of`
// is the content path the folder layer comes from, `game` the game layer's
// name; both nullptr / "" without content.
static bool load_core(CoreSession* session, AppShell* shell, const char* core_path, const char* folder_of, const char* game)
{
    std::vector<LibretroOptionValue> saved;
    if (session->option_store != nullptr)
    {
        char core[256] = {};
        char folder[LIBRETRO_PATH_MAX] = {};
        libretro_core_short_name(core_path, core, sizeof(core));
        folder_name(folder_of, folder, sizeof(folder));
        core_options_store_set_context(session->option_store, core, folder, game != nullptr ? game : "");
        core_options_store_effective(session->option_store, &saved);
        if (session->skip_stored_options_once)
        {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Core options: starting with the core's defaults (%u stored value(s) kept, not used)",
                        static_cast<unsigned>(saved.size()));
            saved.clear();
        }
    }
    session->skip_stored_options_once = false;

    LibretroHostConfig config = {};
    config.core_options_version = session->options_version;
    config.language = session->language;
    config.saves_directory = session->saves_directory;
    config.states_directory = session->states_directory;
    config.system_directory = session->system_directory;
    config.saved_options = saved.data();
    config.saved_option_count = static_cast<uint32_t>(saved.size());
    config.native_pixel_formats = use_native_pixel_formats(shell);
    // Cores that can make any rate (SameBoy) make the device's: no resampling.
    config.target_sample_rate = static_cast<uint32_t>(audio_output_device_rate(&session->audio_config));
    if (!libretro_host_load_core(session->host, core_path, &config))
    {
        set_message(session, "Could not load the core %s (see the log)", core_path);
        return false;
    }
    if (!saved.empty())
    {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Core options: %u stored value(s) for this core / folder / game",
                    static_cast<unsigned>(saved.size()));
    }
    session->options_serial = 0;
    cache_options(session);   // declared in retro_set_environment
    return true;
}

// After the game loaded: the texture and the sound. False (core unloaded) if
// the texture couldn't be made.
static bool finish_start(CoreSession* session, AppShell* shell)
{
    // Texture at the core's maximum size, so it rarely needs recreating.
    const retro_game_geometry* geometry = &session->host->av_info.geometry;
    uint32_t width = geometry->max_width > 0 ? geometry->max_width : geometry->base_width;
    uint32_t height = geometry->max_height > 0 ? geometry->max_height : geometry->base_height;
    if (width == 0 || height == 0)
    {
        width = 640;
        height = 480;
    }
    if (!ensure_texture(session, shell, width, height, frame_texture_format(session->host)))
    {
        libretro_host_unload_core(session->host);
        return false;
    }

    // No sound is not fatal: the core still runs (the reason is logged).
    audio_output_create(&session->audio, session->host->av_info.timing.sample_rate, &session->audio_config);

    const char* name = session->host->system_info.library_name;
    set_message(session, "Running %s", name != nullptr ? name : "core");
    return true;
}

bool core_session_start(CoreSession* session, AppShell* shell, const char* core_path, const char* content_path)
{
    core_session_stop(session, shell);
    if (core_path == nullptr || core_path[0] == '\0')
    {
        set_message(session, "Choose a core first");
        return false;
    }

    // A file inside an archive: extracted (or found in the cache) before the
    // core loads, so a failure leaves nothing to undo.
    const char* content = content_path != nullptr && content_path[0] != '\0' ? content_path : nullptr;
    std::string load_path;
    if (content != nullptr && !content_archive_resolve(content, session->cache_directory, &load_path))
    {
        set_message(session, "Could not extract %s (see the log)", content);
        return false;
    }

    char game[LIBRETRO_PATH_MAX] = {};
    if (content != nullptr)
    {
        content_archive_game_name(content, game, sizeof(game));
    }
    if (!load_core(session, shell, core_path, content, game))
    {
        return false;
    }

    if (!libretro_host_load_game(session->host, content != nullptr ? load_path.c_str() : nullptr))
    {
        set_message(session, "The core could not start%s (see the log)", content != nullptr ? " this content" : " without content");
        libretro_host_unload_core(session->host);
        return false;
    }
    return finish_start(session, shell);
}

bool core_session_special_game_name(const char* const* content_paths, uint32_t content_count, char* out, size_t out_size)
{
    if (out_size == 0)
    {
        return false;
    }
    out[0] = '\0';
    size_t used = 0;
    for (uint32_t i = 0; i < content_count; i++)
    {
        char name[LIBRETRO_PATH_MAX] = {};
        if (content_paths[i] == nullptr || content_paths[i][0] == '\0')
        {
            continue;
        }
        if (!content_archive_game_name(content_paths[i], name, sizeof(name)))
        {
            out[0] = '\0';
            return false;
        }
        int written = std::snprintf(out + used, out_size - used, "%s%s", used > 0 ? " + " : "", name);
        if (written < 0 || static_cast<size_t>(written) >= out_size - used)
        {
            out[0] = '\0';
            return false;
        }
        used += static_cast<size_t>(written);
    }
    return used > 0;
}

bool core_session_start_special(CoreSession* session, AppShell* shell, const char* core_path, const char* subsystem,
                                const char* const* content_paths, uint32_t content_count)
{
    core_session_stop(session, shell);
    if (core_path == nullptr || core_path[0] == '\0')
    {
        set_message(session, "Choose a core first");
        return false;
    }
    if (content_count == 0 || content_count > LIBRETRO_SUBSYSTEM_ROMS_MAX)
    {
        set_message(session, "%s: no content files", subsystem != nullptr ? subsystem : "Subsystem");
        return false;
    }

    // Each file on its own: one inside an archive is extracted, a whole
    // archive or a plain file is passed as it is. Before the core loads.
    std::vector<std::string> load_paths(content_count);
    std::vector<const char*> paths(content_count, nullptr);
    const char* first = nullptr;
    for (uint32_t i = 0; i < content_count; i++)
    {
        const char* content = content_paths[i];
        if (content == nullptr || content[0] == '\0')
        {
            continue;   // an optional file left out; the host checks required ones
        }
        if (!content_archive_resolve(content, session->cache_directory, &load_paths[i]))
        {
            set_message(session, "Could not extract %s (see the log)", content);
            return false;
        }
        paths[i] = load_paths[i].c_str();
        first = first != nullptr ? first : content;
    }

    // Core options: the folder of the first file, the game layer named like
    // the savestates ("SGB + Tetris").
    char game[LIBRETRO_PATH_MAX] = {};
    core_session_special_game_name(content_paths, content_count, game, sizeof(game));
    if (!load_core(session, shell, core_path, first, game))
    {
        return false;
    }

    if (!libretro_host_load_game_special(session->host, subsystem, paths.data(), content_count))
    {
        set_message(session, "The core could not start %s with these files (see the log)",
                    subsystem != nullptr ? subsystem : "the subsystem");
        libretro_host_unload_core(session->host);
        return false;
    }
    return finish_start(session, shell);
}

// ---------------------------------------------------------------------------
// Per loop
// ---------------------------------------------------------------------------

bool core_session_handle_event(CoreSession* session, AppShell* shell, const SDL_Event* event, bool keyboard_for_joypad)
{
    // Keys and typed text go to the core too while the game has the keyboard.
    keyboard_input_handle_event(&session->keyboard, event, session->host);
    bool keyboard_enabled = keyboard_for_joypad;
    if (!session->external_pads)
    {
        fixed_input_handle_event(&session->fixed_input, event, keyboard_enabled, session->gamepad);
    }

    // Fast forward while Space is held. A key-up always counts, so focus moving
    // to a text field mid-press can't leave it stuck on.
    if (!session->external_pads && (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) &&
        event->key.scancode == SDL_SCANCODE_SPACE)
    {
        if (event->type == SDL_EVENT_KEY_UP)
        {
            session->fast_forward_held = false;
        }
        else if (keyboard_enabled && !event->key.repeat)
        {
            session->fast_forward_held = true;
        }
    }
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST && event->window.windowID == SDL_GetWindowID(shell->window))
    {
        session->fast_forward_held = false;   // the key-up goes to another window
    }

    if ((event->type == SDL_EVENT_WINDOW_FOCUS_LOST || event->type == SDL_EVENT_WINDOW_FOCUS_GAINED) &&
        event->window.windowID == SDL_GetWindowID(shell->window))
    {
        audio_output_set_background(&session->audio, event->type == SDL_EVENT_WINDOW_FOCUS_LOST);
    }
    if (event->type == SDL_EVENT_AUDIO_DEVICE_ADDED || event->type == SDL_EVENT_AUDIO_DEVICE_REMOVED)
    {
        if (!event->adevice.recording)
        {
            audio_output_devices_changed(&session->audio);
        }
    }
    if (event->type == SDL_EVENT_GAMEPAD_ADDED && session->gamepad == nullptr && !session->external_pads)
    {
        session->gamepad = SDL_OpenGamepad(event->gdevice.which);
    }
    if (event->type == SDL_EVENT_GAMEPAD_REMOVED && session->gamepad != nullptr &&
        SDL_GetGamepadID(session->gamepad) == event->gdevice.which)
    {
        SDL_CloseGamepad(session->gamepad);
        session->gamepad = nullptr;
    }
    return false;
}

void core_session_update(CoreSession* session, AppShell* shell, bool keyboard_for_core)
{
    keyboard_input_update(&session->keyboard, keyboard_for_core, session->host, &shell->text_input);

    bool running = core_session_running(session);
    // vsync_forced comes from the swapchain as it is now (set_vsync below may
    // change it; the next loop sees the result).
    frame_timing_update(&session->timing, running, running ? session->host->av_info.timing.fps : 0.0,
                        app_shell_display_refresh_hz(shell), app_shell_vsync_forced(shell));
    // Missed refreshes (and the catch-up frames for them) only count while
    // core frames run: a paused or minimized game has nothing to make up.
    bool minimized = (SDL_GetWindowFlags(shell->window) & SDL_WINDOW_MINIMIZED) != 0;
    // Fast forward runs loops of its own length on purpose: not a missed refresh.
    frame_timing_measure(&session->timing,
                         running && !session->paused && !minimized && !core_session_fast_forwarding(session));
    bool vsync = session->timing.effective == TimingMode::vsync;
    if (!app_shell_set_vsync(shell, vsync))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not switch vsync %s", vsync ? "on" : "off");
    }
    // DRC in both modes. Vsync: on top of display Hz / core fps. Exact: on top
    // of ratio 1, because the frame timer (SDL_GetTicksNS) and the audio device
    // clock differ by tens of ppm, enough to drain or fill the queue in minutes.
    // DRC can be turned off in the frontend's settings (then only the base speed applies).
    audio_output_configure(&session->audio, &session->audio_config);
    audio_output_set_speed(&session->audio, frame_timing_audio_ratio(&session->timing),
                           session->audio_config.rate_control);
}

void core_session_set_pads(CoreSession* session, const uint16_t buttons[LIBRETRO_MAX_PORTS],
                           const int16_t analog[LIBRETRO_MAX_PORTS][2][2])
{
    session->external_pads = true;
    for (unsigned port = 0; port < LIBRETRO_MAX_PORTS; port++)
    {
        session->pads.joypad_buttons[port] = buttons[port];
        for (int stick = 0; stick < 2; stick++)
        {
            session->pads.analog[port][stick][0] = analog[port][stick][0];
            session->pads.analog[port][stick][1] = analog[port][stick][1];
        }
    }
}

bool core_session_fast_forwarding(const CoreSession* session)
{
    return core_session_running(session) && !session->paused && (session->fast_forward || session->fast_forward_held);
}

// How long one loop may spend running core frames while fast-forwarding: a
// share of the period the loop is paced at (the display's refresh in Vsync,
// the core's frame in Exact).
static uint64_t fast_forward_budget_ns(const CoreSession* session)
{
    double rate = session->timing.effective == TimingMode::vsync ? session->timing.display_hz : session->timing.core_fps;
    if (rate <= 0.0)
    {
        rate = 60.0;
    }
    return static_cast<uint64_t>(CORE_SESSION_FAST_FORWARD_BUDGET * 1e9 / rate);
}

// The DVR is open: the game waits; PLAY runs the recorded frames, one per
// frame due, heard and shown like the game's.
static CoreSessionFrame run_dvr_frame(CoreSession* session, AppShell* shell)
{
    if (!core_rewind_dvr_tick(session->rewind))
    {
        return CoreSessionFrame::skipped;
    }
    audio_output_set_sample_rate(&session->audio, session->host->av_info.timing.sample_rate);
    audio_output_push(&session->audio, session->host->audio.samples, session->host->audio.frames);
    const LibretroVideoFrame* video = &session->host->video;
    if (video->width > 0 && video->height > 0)
    {
        ensure_texture(session, shell, video->width, video->height, frame_texture_format(session->host));
    }
    return CoreSessionFrame::ran;
}

CoreSessionFrame core_session_run_frame(CoreSession* session, AppShell* shell, const LibretroInputState* pointer,
                                        bool keyboard_for_joypad)
{
    // The due check is a loop tick, not "the core will run": it comes BEFORE
    // the paused/minimized checks on purpose, so the Exact timer keeps
    // advancing while paused. With vsync off, frame_timing_wait() is the only
    // thing throttling the loop; if the timer stood still, a paused session
    // would spin at 100% CPU. Don't move it below those checks.
    if (!frame_timing_frame_due(&session->timing))
    {
        return CoreSessionFrame::skipped;
    }
    if (!core_session_running(session))
    {
        return CoreSessionFrame::skipped;
    }
    if (session->rewind != nullptr && session->rewind->dvr_open)
    {
        return run_dvr_frame(session, shell);
    }
    if (session->paused && !session->step_once)
    {
        return CoreSessionFrame::skipped;
    }
    // Minimized: nothing is presented, so vsync wouldn't pace the core.
    if (SDL_GetWindowFlags(shell->window) & SDL_WINDOW_MINIMIZED)
    {
        // The queue runs dry on purpose: clear it so restoring isn't an underrun.
        audio_output_clear(&session->audio);
        return CoreSessionFrame::skipped;
    }
    session->step_once = false;

    LibretroInputState input = {};
    if (session->external_pads)
    {
        for (unsigned port = 0; port < LIBRETRO_MAX_PORTS; port++)
        {
            input.joypad_buttons[port] = session->pads.joypad_buttons[port];
            for (int stick = 0; stick < 2; stick++)
            {
                input.analog[port][stick][0] = session->pads.analog[port][stick][0];
                input.analog[port][stick][1] = session->pads.analog[port][stick][1];
            }
        }
    }
    else
    {
        fixed_input_read(&session->fixed_input, &input, keyboard_for_joypad,
                         session->gamepad);
    }
    keyboard_input_read(&session->keyboard, &input);
    if (pointer != nullptr)
    {
        for (unsigned port = 0; port < LIBRETRO_MAX_PORTS; port++)
        {
            input.cursor[port] = pointer->cursor[port];
        }
    }

    libretro_host_set_input(session->host, &input);

    // The frame, plus the ones a missed refresh lost (Vsync catch-up,
    // frame_timing.h). Each one's audio is queued; only the last picture is
    // uploaded and shown. Fast forward instead runs frames until its budget
    // is used up, silently.
    bool fast = core_session_fast_forwarding(session);
    libretro_host_set_fast_forward(session->host, fast);
    uint64_t loop_started = SDL_GetTicksNS();
    uint64_t budget = fast ? fast_forward_budget_ns(session) : 0;
    uint32_t frames = 1 + (session->paused ? 0 : session->timing.catch_up_frames);
    for (uint32_t i = 0; fast || i < frames; i++)
    {
        uint64_t frame_started = SDL_GetTicksNS();
        if (fast && i > 0 && frame_started - loop_started >= budget)
        {
            break;
        }
        if (session->rewind != nullptr)
        {
            core_rewind_before_frame(session->rewind, &input);
        }
        libretro_host_run_frame(session->host);
        uint64_t frame_ns = SDL_GetTicksNS() - frame_started;
        session->stats.frames++;
        session->stats.run_ns += frame_ns;
        if (session->rewind != nullptr)
        {
            core_rewind_after_frame(session->rewind, frame_ns);
        }

        if (session->host->shutdown_requested)
        {
            core_session_stop(session, shell);
            set_message(session, "The core asked to shut down (RETRO_ENVIRONMENT_SHUTDOWN)");
            return CoreSessionFrame::shut_down;
        }

        // A core may change its sample rate (SET_SYSTEM_AV_INFO): follow it first.
        // Single-stepping stays silent: one frame of audio after a gap is just a blip.
        audio_output_set_sample_rate(&session->audio, session->host->av_info.timing.sample_rate);
        if (!session->paused && !fast)
        {
            audio_output_push(&session->audio, session->host->audio.samples, session->host->audio.frames);
            if (session->capture_audio && session->host->audio.frames > 0)
            {
                const int16_t* samples = session->host->audio.samples;
                session->captured_audio.insert(session->captured_audio.end(), samples,
                                               samples + session->host->audio.frames * 2);
            }
        }
    }
    if (fast)
    {
        // Muted: an empty queue isn't an underrun after a clear, and normal
        // speed starts again from a fresh cushion.
        audio_output_clear(&session->audio);
    }

    cache_options(session);   // a core may declare its options again at runtime

    // A core may grow its image at runtime (SET_SYSTEM_AV_INFO).
    const LibretroVideoFrame* video = &session->host->video;
    if (video->width > 0 && video->height > 0)
    {
        ensure_texture(session, shell, video->width, video->height, frame_texture_format(session->host));
    }
    return CoreSessionFrame::ran;
}

void core_session_wait(const CoreSession* session)
{
    frame_timing_wait(&session->timing);
}

// ---------------------------------------------------------------------------
// Savestates
// ---------------------------------------------------------------------------

// The core's last frame, cropped and scaled as in StateThumbnail, as a PNG.
static bool save_thumbnail(const LibretroVideoFrame* video, const StateThumbnail* thumbnail, const char* path)
{
    if (thumbnail == nullptr || thumbnail->width == 0 || thumbnail->height == 0 || video->pixels == nullptr ||
        video->width == 0 || video->height == 0)
    {
        return false;
    }
    // A crop that leaves nothing on an axis is ignored on it (like viewport).
    uint32_t left = thumbnail->crop.left;
    uint32_t top = thumbnail->crop.top;
    uint32_t width = video->width;
    uint32_t height = video->height;
    if (left + thumbnail->crop.right < width)
    {
        width -= left + thumbnail->crop.right;
    }
    else
    {
        left = 0;
    }
    if (top + thumbnail->crop.bottom < height)
    {
        height -= top + thumbnail->crop.bottom;
    }
    else
    {
        top = 0;
    }

    // The frame as the host keeps it (converted or the core's own format);
    // SDL converts while blitting. XRGB1555 is libretro's 0RGB1555.
    SDL_PixelFormat frame_format = SDL_PIXELFORMAT_XRGB8888;
    if (video->format == RETRO_PIXEL_FORMAT_RGB565)
    {
        frame_format = SDL_PIXELFORMAT_RGB565;
    }
    else if (video->format == RETRO_PIXEL_FORMAT_0RGB1555)
    {
        frame_format = SDL_PIXELFORMAT_XRGB1555;
    }
    SDL_Surface* frame = SDL_CreateSurfaceFrom(static_cast<int>(video->width), static_cast<int>(video->height),
                                               frame_format, video->pixels,
                                               static_cast<int>(video->width * video->bytes_per_pixel));
    SDL_Surface* cropped = SDL_CreateSurface(static_cast<int>(width), static_cast<int>(height), SDL_PIXELFORMAT_XRGB8888);
    SDL_Surface* scaled = nullptr;
    bool ok = false;
    if (frame != nullptr && cropped != nullptr)
    {
        SDL_Rect source = {static_cast<int>(left), static_cast<int>(top), static_cast<int>(width), static_cast<int>(height)};
        if (SDL_BlitSurface(frame, &source, cropped, nullptr))
        {
            scaled = SDL_ScaleSurface(cropped, static_cast<int>(thumbnail->width), static_cast<int>(thumbnail->height),
                                      SDL_SCALEMODE_LINEAR);
        }
        ok = scaled != nullptr && SDL_SavePNG(scaled, path);
    }
    if (!ok)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Savestate thumbnail: could not write %s: %s", path, SDL_GetError());
    }
    SDL_DestroySurface(scaled);
    SDL_DestroySurface(cropped);
    SDL_DestroySurface(frame);
    return ok;
}

bool core_session_save_state(CoreSession* session, const StateThumbnail* thumbnail)
{
    if (!core_session_running(session))
    {
        return false;
    }
    if (!libretro_host_save_state(session->host, session->state_slot))
    {
        set_message(session, "Could not save state slot %u (see the log)", session->state_slot);
        return false;
    }
    char path[LIBRETRO_PATH_MAX + 8] = {};
    if (libretro_host_state_path(session->host, session->state_slot, path, LIBRETRO_PATH_MAX))
    {
        std::strncat(path, ".png", sizeof(path) - std::strlen(path) - 1);
        if (!save_thumbnail(&session->host->video, thumbnail, path))
        {
            SDL_RemovePath(path);   // never another state's picture
        }
    }
    set_message(session, "Saved state slot %u", session->state_slot);
    return true;
}

bool core_session_slot_info(const CoreSession* session, uint32_t slot, StateSlotInfo* info)
{
    *info = StateSlotInfo{};
    char path[LIBRETRO_PATH_MAX] = {};
    if (!core_session_running(session) || !libretro_host_state_path(session->host, slot, path, sizeof(path)))
    {
        return false;
    }
    SDL_PathInfo path_info = {};
    if (SDL_GetPathInfo(path, &path_info) && path_info.type == SDL_PATHTYPE_FILE)
    {
        info->used = true;
        info->modified = path_info.modify_time;
        std::snprintf(info->thumbnail_path, sizeof(info->thumbnail_path), "%s.png", path);
        info->has_thumbnail = host_fs_exists(info->thumbnail_path);
    }
    return true;
}

bool core_session_load_state(CoreSession* session)
{
    if (!core_session_running(session))
    {
        return false;
    }
    if (!libretro_host_load_state(session->host, session->state_slot))
    {
        set_message(session, "Could not load state slot %u (see the log)", session->state_slot);
        return false;
    }
    // What was queued belongs to the time before the load.
    audio_output_clear(&session->audio);
    set_message(session, "Loaded state slot %u", session->state_slot);
    return true;
}

void core_session_change_slot(CoreSession* session, int32_t delta)
{
    // Wraps around within 1..9.
    int32_t first = static_cast<int32_t>(LIBRETRO_STATE_SLOT_FIRST);
    int32_t slots = static_cast<int32_t>(LIBRETRO_STATE_SLOT_LAST - LIBRETRO_STATE_SLOT_FIRST + 1);
    int32_t index = static_cast<int32_t>(session->state_slot) - first + delta;
    session->state_slot = static_cast<uint32_t>(first + (index % slots + slots) % slots);
}
