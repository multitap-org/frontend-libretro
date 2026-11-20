// The core downloader in builds without one: store builds (Play, Steam:
// downloading code isn't allowed there) and platforms without an HTTP backend
// yet. Same API; nothing ever downloads. Chosen by CMake.

#include "core_updater.h"

bool core_updater_available()
{
    return false;
}

int32_t core_updater_key_count()
{
    return 0;
}

const char* core_updater_key_hex(int32_t)
{
    return nullptr;
}

bool core_updater_init(CoreUpdater* updater, const char*, bool, const char* const*, int32_t, const char*,
                       const char*, const char*)
{
    updater->message = "core downloads aren't part of this build";
    updater->message_is_error = true;
    return false;
}

void core_updater_shutdown(CoreUpdater*)
{
}

void core_updater_refresh(CoreUpdater*)
{
}

bool core_updater_install(CoreUpdater*, int32_t, const char*)
{
    return false;
}

bool core_updater_remove(CoreUpdater*, int32_t, const char*)
{
    return false;
}

void core_updater_cancel(CoreUpdater*)
{
}

void core_updater_poll(CoreUpdater*)
{
}

void core_updater_progress(CoreUpdater*, int64_t* received, int64_t* total)
{
    *received = 0;
    *total = 0;
}

CoreStatus core_updater_status(const CoreUpdater*, int32_t)
{
    return CoreStatus::not_installed;
}

std::string core_updater_installed_version(const CoreUpdater*, const char*)
{
    return std::string();
}
