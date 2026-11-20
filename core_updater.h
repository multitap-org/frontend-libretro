#pragma once

// The core downloader: lists the cores the server offers for this build's
// target and installs, updates or removes them in the cores folder.
//
// Where cores come from: a static site (default https://cores.get-piccolo.app)
// that serves v1/index/<target>.cfg, signed with the project's key
// (index_sign, multitap-org/cores), and the zips it points to. Nothing is
// trusted before the signature checks out: the index then carries the SHA-256
// of every zip and of the library inside, and both are checked before a core
// lands in the cores folder.
//
// Network and disk work run on one worker thread; the frontend calls
// core_updater_poll() once per frame and reads the state. One task at a time
// (refresh or one install).
//
// The cores folder keeps installed.cfg: what we installed ([<file>] version,
// commit, sha256), so "up to date" is known without peeking. A core already
// there that we didn't install (dropped in by hand) is listed as such and is
// only replaced when asked. Installing writes <file>.new next to the core and
// renames it over the old one, which is kept as <file>.old (one step back).
// The running core is never replaced or removed: the frontend passes its file
// name and those requests are refused.
//
// Store builds (Play, Steam) are built without it (core_updater_none.cpp):
// stores don't allow downloading code; core_updater_available() says which.

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

#include "core_index.h"
#include "net_http.h"
#include "settings_file.h"
#include "signature.h"

enum class CoreUpdaterState
{
    idle,          // nothing running; `index` holds the last good index (maybe none yet)
    refreshing,    // downloading and checking the index
    installing,    // downloading and installing `task_entry`
};

enum class CoreStatus
{
    not_installed,
    installed,          // what the index offers now
    update,             // we installed an older one
    installed_by_hand,  // the file is there, but we didn't install it
};

enum class CoreUpdaterTask
{
    refresh,
    install,
};

constexpr int32_t CORE_UPDATER_MAX_KEYS = 4;

// What the worker thread touches. The main thread writes the inputs before
// starting the thread and reads the results only after `finished` becomes 1;
// while it runs, the main thread only touches the atomics (`cancel`, and
// `job`'s progress and cancel).
struct CoreUpdaterWorker
{
    SDL_Thread*     thread   = nullptr;
    SDL_AtomicInt   finished = {};
    SDL_AtomicInt   cancel   = {};
    NetHttpJob      job;

    // Inputs.
    CoreUpdaterTask task = CoreUpdaterTask::refresh;
    CoreIndexEntry  entry;
    std::string     base_url;
    std::string     target;
    std::string     cores_dir;
    std::string     work_dir;
    bool            allow_insecure = false;
    SignatureKey    keys[CORE_UPDATER_MAX_KEYS];
    int32_t         key_count = 0;

    // Results.
    bool            ok = false;
    std::string     message;
    CoreIndex       index;
};

struct CoreUpdater
{
    std::string        base_url;          // the site, no trailing '/'
    std::string        target;            // "linux-x86_64" ...
    std::string        cores_dir;
    std::string        work_dir;          // downloads in progress (inside the cache folder)
    bool               free_only = false; // store-style builds: free-licensed cores only

    CoreIndex          index;             // the last index that checked out
    SettingsFile       installed;         // <cores_dir>/installed.cfg
    CoreUpdaterState   state = CoreUpdaterState::idle;
    CoreIndexEntry     task_entry;        // the core being installed
    std::string        message;           // the last result, for the status line
    bool               message_is_error = false;
    bool               cores_changed = false;   // set when files in cores_dir changed; the frontend rescans and clears it

    bool               initialized = false;   // core_updater_init succeeded
    CoreUpdaterWorker  worker;
};

// False in builds without a downloader (store builds, or no libcurl yet).
bool core_updater_available();

// The project's index signing keys (current + next), as public keys in hex.
int32_t     core_updater_key_count();
const char* core_updater_key_hex(int32_t index);

// Sets up (nothing runs yet). `base_url` must be https:// unless
// `allow_insecure` (development: http:// and file:// too). Keys: public keys
// in hex (normally core_updater_key_hex). `target`: core_index_build_target()
// except in tests. False (logged, `message` says why) if the URL is refused or
// the work folder can't be made.
bool core_updater_init(CoreUpdater* updater, const char* base_url, bool allow_insecure,
                       const char* const* key_hex, int32_t key_count, const char* target,
                       const char* cores_dir, const char* work_dir);

// Cancels whatever runs and waits for the worker.
void core_updater_shutdown(CoreUpdater* updater);

// Starts downloading the index. Ignored while a task runs.
void core_updater_refresh(CoreUpdater* updater);

// Starts installing or updating index entry `entry`. Refused (message set,
// false) while a task runs, for the running core (`running_file`, may be
// nullptr or ""), or for a non-free core when free_only.
bool core_updater_install(CoreUpdater* updater, int32_t entry, const char* running_file);

// Removes an installed core (index entry `entry`) and its .old copy. Refused
// for the running core and while a task runs.
bool core_updater_remove(CoreUpdater* updater, int32_t entry, const char* running_file);

// Asks the running task to stop.
void core_updater_cancel(CoreUpdater* updater);

// Once per frame: picks up a finished task (new index, installed core).
void core_updater_poll(CoreUpdater* updater);

// Bytes of the current download (0, 0 when none).
void core_updater_progress(CoreUpdater* updater, int64_t* received, int64_t* total);

CoreStatus core_updater_status(const CoreUpdater* updater, int32_t entry);

// The version we installed for `file` ("" if none recorded).
std::string core_updater_installed_version(const CoreUpdater* updater, const char* file);
