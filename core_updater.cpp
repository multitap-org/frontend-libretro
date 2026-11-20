#include "core_updater.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>

#include "archive.h"
#include "file_hash.h"
#include "host_fs.h"
#include "host_log.h"
#include "net_http.h"
#include "signature.h"

// The project's index signing keys, public, in hex (multitap-org/cores,
// keys/current.public). A second "next" key goes here before the current one
// is retired, so builds already out there keep trusting the server.
static const char* const CORE_UPDATER_KEYS[] = {
    "19c0ec4760edd7a7878b71bfd63d197191077c37193aef0bfa94a41d421eae5b",   // key_id 20ad1a6e8a2bfdca (2026-11-20)
};
constexpr int32_t CORE_UPDATER_KEY_COUNT = static_cast<int32_t>(sizeof(CORE_UPDATER_KEYS) / sizeof(CORE_UPDATER_KEYS[0]));

constexpr int64_t INDEX_MAX_BYTES     = 4 * 1024 * 1024;
constexpr int64_t SIGNATURE_MAX_BYTES = 4096;

static const char* const INSTALLED_FILE   = "installed.cfg";
static const char* const INSTALLED_HEADER = "Cores installed by the core downloader (core_updater). Written by the program.";

bool core_updater_available()
{
    return true;
}

int32_t core_updater_key_count()
{
    return CORE_UPDATER_KEY_COUNT;
}

const char* core_updater_key_hex(int32_t index)
{
    if (index < 0 || index >= CORE_UPDATER_KEY_COUNT)
    {
        return nullptr;
    }
    return CORE_UPDATER_KEYS[index];
}

static bool starts_with(const std::string& text, const char* prefix)
{
    return text.compare(0, std::strlen(prefix), prefix) == 0;
}

// https:// always; http:// and file:// only for development.
static bool url_allowed(const std::string& url, bool allow_insecure)
{
    if (starts_with(url, "https://"))
    {
        return true;
    }
    return allow_insecure && (starts_with(url, "http://") || starts_with(url, "file://"));
}

// An index URL: absolute, or relative to the site ("v1/linux-x86_64/x.zip").
static std::string resolve_url(const CoreUpdaterWorker* worker, const std::string& url)
{
    if (url.find("://") != std::string::npos)
    {
        return url;
    }
    return worker->base_url + "/" + url;
}

static std::string installed_path(const CoreUpdater* updater)
{
    return updater->cores_dir + "/" + INSTALLED_FILE;
}

// --- Worker thread -------------------------------------------------------

static void worker_fail(CoreUpdaterWorker* worker, const char* format, const char* detail)
{
    char text[512];
    std::snprintf(text, sizeof(text), format, detail);
    worker->ok = false;
    worker->message = text;
}

// One download, waiting for it here (the job runs on its own thread so the
// main thread can read its progress and cancel it).
static bool worker_download(CoreUpdaterWorker* worker, const std::string& url, const std::string& path,
                            int64_t max_bytes)
{
    if (!url_allowed(url, worker->allow_insecure))
    {
        worker_fail(worker, "refused (not HTTPS): %s", url.c_str());
        return false;
    }
    if (SDL_GetAtomicInt(&worker->cancel) != 0)
    {
        worker_fail(worker, "%s", "cancelled");
        return false;
    }
    net_http_job_start(&worker->job, url.c_str(), path.c_str(), max_bytes);
    NetHttpState state = net_http_job_finish(&worker->job);
    if (state == NetHttpState::done)
    {
        return true;
    }
    if (state == NetHttpState::cancelled)
    {
        worker_fail(worker, "%s", "cancelled");
    }
    else
    {
        worker_fail(worker, "download failed: %s", worker->job.error);
    }
    return false;
}

static void worker_refresh(CoreUpdaterWorker* worker)
{
    std::string url       = worker->base_url + "/v1/index/" + worker->target + ".cfg";
    std::string index     = worker->work_dir + "/" + worker->target + ".cfg";
    std::string signature = index + ".sig";
    if (!worker_download(worker, url, index, INDEX_MAX_BYTES) ||
        !worker_download(worker, url + ".sig", signature, SIGNATURE_MAX_BYTES))
    {
        return;
    }

    uint8_t* index_data = nullptr;
    size_t   index_size = 0;
    uint8_t* sig_data   = nullptr;
    size_t   sig_size   = 0;
    if (!host_fs_read_file(index.c_str(), &index_data, &index_size) ||
        !host_fs_read_file(signature.c_str(), &sig_data, &sig_size))
    {
        delete[] index_data;
        worker_fail(worker, "can't read the downloaded index in %s", worker->work_dir.c_str());
        return;
    }
    SignatureResult result = signature_verify(worker->keys, worker->key_count, index_data, index_size,
                                              reinterpret_cast<const char*>(sig_data), sig_size);
    if (result != SignatureResult::ok)
    {
        // Nothing from an index that doesn't check out is used.
        worker_fail(worker, "the index's signature doesn't check out (%s)", signature_result_name(result));
    }
    else if (!core_index_parse(&worker->index, reinterpret_cast<const char*>(index_data), index_size,
                               worker->target.c_str()))
    {
        worker_fail(worker, "the index isn't for %s", worker->target.c_str());
    }
    else
    {
        worker->ok = true;
        char text[256];
        std::snprintf(text, sizeof(text), "%d cores available (%s)",
                      static_cast<int>(worker->index.entries.size()), worker->index.release.c_str());
        worker->message = text;
    }
    delete[] index_data;
    delete[] sig_data;
    SDL_RemovePath(index.c_str());
    SDL_RemovePath(signature.c_str());
}

static bool hash_matches(const std::string& path, const std::string& expected)
{
    char hash[FILE_HASH_SHA256_HEX];
    return file_hash_sha256(path.c_str(), hash) && expected == hash;
}

static void worker_install(CoreUpdaterWorker* worker)
{
    const CoreIndexEntry* entry = &worker->entry;
    std::string zip = worker->work_dir + "/" + entry->file + ".zip";
    // The zip must be exactly the size the index says: a bigger one fails
    // while downloading, a different one fails the hash.
    if (!worker_download(worker, resolve_url(worker, entry->url), zip, entry->size))
    {
        return;
    }
    if (!hash_matches(zip, entry->sha256))
    {
        SDL_RemovePath(zip.c_str());
        worker_fail(worker, "%s: the download doesn't match the index (SHA-256)", entry->name.c_str());
        return;
    }

    // Extracted straight into the cores folder as <file>.new (same folder, so
    // the final rename can't cross file systems), checked, then renamed.
    std::string dest     = worker->cores_dir + "/" + entry->file;
    std::string new_path = dest + ".new";
    std::string old_path = dest + ".old";
    bool extracted = archive_extract(zip.c_str(), entry->file.c_str(), new_path.c_str());
    SDL_RemovePath(zip.c_str());
    if (!extracted)
    {
        worker_fail(worker, "%s: can't extract the core from the zip", entry->name.c_str());
        return;
    }
    if (!hash_matches(new_path, entry->library_sha256))
    {
        SDL_RemovePath(new_path.c_str());
        worker_fail(worker, "%s: the core doesn't match the index (SHA-256)", entry->name.c_str());
        return;
    }
    if (host_fs_exists(dest.c_str()) && !SDL_RenamePath(dest.c_str(), old_path.c_str()))
    {
        SDL_RemovePath(new_path.c_str());
        worker_fail(worker, "can't move the old core aside: %s", SDL_GetError());
        return;
    }
    if (!SDL_RenamePath(new_path.c_str(), dest.c_str()))
    {
        std::string error = SDL_GetError();
        // Put the old one back rather than leave no core at all.
        if (host_fs_exists(old_path.c_str()))
        {
            SDL_RenamePath(old_path.c_str(), dest.c_str());
        }
        SDL_RemovePath(new_path.c_str());
        worker_fail(worker, "can't install the core: %s", error.c_str());
        return;
    }
    worker->ok = true;
    worker->message = entry->name + " " + entry->version + " installed";
}

static int worker_thread(void* user)
{
    CoreUpdaterWorker* worker = static_cast<CoreUpdaterWorker*>(user);
    worker->ok = false;
    worker->message.clear();
    if (worker->task == CoreUpdaterTask::refresh)
    {
        worker_refresh(worker);
    }
    else
    {
        worker_install(worker);
    }
    // Last: the main thread reads the results after it sees this.
    SDL_SetAtomicInt(&worker->finished, 1);
    return 0;
}

static bool worker_start(CoreUpdater* updater, CoreUpdaterTask task)
{
    CoreUpdaterWorker* worker = &updater->worker;
    worker->task = task;
    worker->index = {};
    SDL_SetAtomicInt(&worker->finished, 0);
    SDL_SetAtomicInt(&worker->cancel, 0);
    worker->thread = SDL_CreateThread(worker_thread, "core_updater", worker);
    if (worker->thread == nullptr)
    {
        updater->message = std::string("can't start the downloader thread: ") + SDL_GetError();
        updater->message_is_error = true;
        return false;
    }
    return true;
}

// --- Main thread ---------------------------------------------------------

bool core_updater_init(CoreUpdater* updater, const char* base_url, bool allow_insecure,
                       const char* const* key_hex, int32_t key_count, const char* target,
                       const char* cores_dir, const char* work_dir)
{
    core_updater_shutdown(updater);
    updater->base_url = base_url != nullptr ? base_url : "";
    while (!updater->base_url.empty() && updater->base_url.back() == '/')
    {
        updater->base_url.pop_back();
    }
    updater->target    = target != nullptr ? target : "";
    updater->cores_dir = cores_dir;
    updater->work_dir  = work_dir;
    updater->index     = {};
    updater->state     = CoreUpdaterState::idle;
    updater->message.clear();
    updater->message_is_error = false;

    if (updater->target.empty())
    {
        updater->message = "no core downloads for this platform";
        updater->message_is_error = true;
        return false;
    }
    if (!url_allowed(updater->base_url, allow_insecure))
    {
        updater->message = "the core server must be an https:// address";
        updater->message_is_error = true;
        host_log(LogLevel::error, "core_updater: refused server %s (not HTTPS)", updater->base_url.c_str());
        return false;
    }
    if (!host_fs_make_directories(work_dir))
    {
        updater->message = std::string("can't create ") + work_dir;
        updater->message_is_error = true;
        return false;
    }

    CoreUpdaterWorker* worker = &updater->worker;
    *worker = CoreUpdaterWorker();
    worker->base_url       = updater->base_url;
    worker->target         = updater->target;
    worker->cores_dir      = updater->cores_dir;
    worker->work_dir       = updater->work_dir;
    worker->allow_insecure = allow_insecure;
    for (int32_t i = 0; i < key_count && worker->key_count < CORE_UPDATER_MAX_KEYS; i++)
    {
        if (signature_key_from_hex(key_hex[i], &worker->keys[worker->key_count]))
        {
            worker->key_count++;
        }
        else
        {
            host_log(LogLevel::error, "core_updater: key %d isn't a public key", static_cast<int>(i));
        }
    }
    updater->initialized = true;

    updater->installed = {};
    settings_file_load(&updater->installed, installed_path(updater).c_str());
    host_log(LogLevel::info, "core_updater: %s, target %s, cores in %s", updater->base_url.c_str(),
             updater->target.c_str(), updater->cores_dir.c_str());
    return true;
}

void core_updater_shutdown(CoreUpdater* updater)
{
    if (!updater->initialized)
    {
        return;
    }
    core_updater_cancel(updater);
    if (updater->worker.thread != nullptr)
    {
        SDL_WaitThread(updater->worker.thread, nullptr);
        updater->worker.thread = nullptr;
    }
    net_http_job_finish(&updater->worker.job);
    updater->initialized = false;
    updater->state = CoreUpdaterState::idle;
}

void core_updater_refresh(CoreUpdater* updater)
{
    if (!updater->initialized || updater->state != CoreUpdaterState::idle)
    {
        return;
    }
    if (worker_start(updater, CoreUpdaterTask::refresh))
    {
        updater->state = CoreUpdaterState::refreshing;
        updater->message = "getting the list of cores";
        updater->message_is_error = false;
    }
}

static bool refuse(CoreUpdater* updater, const std::string& message)
{
    updater->message = message;
    updater->message_is_error = true;
    return false;
}

static bool is_running(const CoreIndexEntry* entry, const char* running_file)
{
    return running_file != nullptr && entry->file == running_file;
}

bool core_updater_install(CoreUpdater* updater, int32_t entry, const char* running_file)
{
    if (!updater->initialized || updater->state != CoreUpdaterState::idle)
    {
        return refuse(updater, "the downloader is busy");
    }
    if (entry < 0 || entry >= static_cast<int32_t>(updater->index.entries.size()))
    {
        return false;
    }
    const CoreIndexEntry* chosen = &updater->index.entries[static_cast<size_t>(entry)];
    if (is_running(chosen, running_file))
    {
        return refuse(updater, chosen->name + " is running: close the game first");
    }
    if (updater->free_only && !chosen->free)
    {
        return refuse(updater, chosen->name + " isn't free-licensed: not offered in this build");
    }
    updater->worker.entry = *chosen;
    updater->task_entry    = *chosen;
    if (!worker_start(updater, CoreUpdaterTask::install))
    {
        return false;
    }
    updater->state = CoreUpdaterState::installing;
    updater->message = "installing " + chosen->name;
    updater->message_is_error = false;
    return true;
}

bool core_updater_remove(CoreUpdater* updater, int32_t entry, const char* running_file)
{
    if (!updater->initialized || updater->state != CoreUpdaterState::idle)
    {
        return refuse(updater, "the downloader is busy");
    }
    if (entry < 0 || entry >= static_cast<int32_t>(updater->index.entries.size()))
    {
        return false;
    }
    const CoreIndexEntry* chosen = &updater->index.entries[static_cast<size_t>(entry)];
    if (is_running(chosen, running_file))
    {
        return refuse(updater, chosen->name + " is running: close the game first");
    }
    std::string dest = updater->cores_dir + "/" + chosen->file;
    if (host_fs_exists(dest.c_str()) && !SDL_RemovePath(dest.c_str()))
    {
        return refuse(updater, std::string("can't remove ") + chosen->file + ": " + SDL_GetError());
    }
    SDL_RemovePath((dest + ".old").c_str());
    settings_file_remove(&updater->installed, chosen->file.c_str(), "version");
    settings_file_remove(&updater->installed, chosen->file.c_str(), "commit");
    settings_file_remove(&updater->installed, chosen->file.c_str(), "sha256");
    settings_file_remove(&updater->installed, chosen->file.c_str(), "release");
    settings_file_save(&updater->installed, installed_path(updater).c_str(), INSTALLED_HEADER);
    updater->cores_changed = true;
    updater->message = chosen->name + " removed";
    updater->message_is_error = false;
    host_log(LogLevel::info, "core_updater: removed %s", dest.c_str());
    return true;
}

void core_updater_cancel(CoreUpdater* updater)
{
    if (!updater->initialized)
    {
        return;
    }
    SDL_SetAtomicInt(&updater->worker.cancel, 1);
    net_http_job_cancel(&updater->worker.job);
}

void core_updater_poll(CoreUpdater* updater)
{
    CoreUpdaterWorker* worker = &updater->worker;
    if (!updater->initialized || updater->state == CoreUpdaterState::idle || SDL_GetAtomicInt(&worker->finished) == 0)
    {
        return;
    }
    SDL_WaitThread(worker->thread, nullptr);
    worker->thread = nullptr;

    updater->message = worker->message;
    updater->message_is_error = !worker->ok;
    if (worker->ok && updater->state == CoreUpdaterState::refreshing)
    {
        updater->index = worker->index;
    }
    else if (worker->ok && updater->state == CoreUpdaterState::installing)
    {
        const CoreIndexEntry* entry = &updater->task_entry;
        const char* section = entry->file.c_str();
        settings_file_set(&updater->installed, section, "version", entry->version.c_str());
        settings_file_set(&updater->installed, section, "commit", entry->commit.c_str());
        settings_file_set(&updater->installed, section, "sha256", entry->library_sha256.c_str());
        settings_file_set(&updater->installed, section, "release", updater->index.release.c_str());
        if (!settings_file_save(&updater->installed, installed_path(updater).c_str(), INSTALLED_HEADER))
        {
            host_log(LogLevel::warning, "core_updater: can't write %s", installed_path(updater).c_str());
        }
        updater->cores_changed = true;
    }
    host_log(worker->ok ? LogLevel::info : LogLevel::warning, "core_updater: %s", updater->message.c_str());
    updater->state = CoreUpdaterState::idle;
}

void core_updater_progress(CoreUpdater* updater, int64_t* received, int64_t* total)
{
    *received = 0;
    *total = 0;
    if (updater->initialized && updater->state != CoreUpdaterState::idle)
    {
        net_http_job_progress(&updater->worker.job, received, total);
    }
}

CoreStatus core_updater_status(const CoreUpdater* updater, int32_t entry)
{
    const CoreIndexEntry* chosen = &updater->index.entries[static_cast<size_t>(entry)];
    std::string dest = updater->cores_dir + "/" + chosen->file;
    if (!host_fs_exists(dest.c_str()))
    {
        return CoreStatus::not_installed;
    }
    const char* commit = settings_file_get(&updater->installed, chosen->file.c_str(), "commit");
    const char* sha256 = settings_file_get(&updater->installed, chosen->file.c_str(), "sha256");
    if (commit == nullptr || sha256 == nullptr)
    {
        return CoreStatus::installed_by_hand;
    }
    if (chosen->commit == commit && chosen->library_sha256 == sha256)
    {
        return CoreStatus::installed;
    }
    return CoreStatus::update;
}

std::string core_updater_installed_version(const CoreUpdater* updater, const char* file)
{
    const char* version = settings_file_get(&updater->installed, file, "version");
    return version != nullptr ? std::string(version) : std::string();
}
