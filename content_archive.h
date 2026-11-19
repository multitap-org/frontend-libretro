#pragma once

// libretro's rules for games inside zip / 7z archives (archive.h,
// content_cache.h), shared by the core session and frontends' file pickers.
//
// A core gets an archive in one of two ways:
//   - whole: the archive's own path, when the core handles archives itself
//     (arcade cores, dosbox-pure): it sets block_extract or lists the
//     archive's extension ("zip") among its own.
//   - one file from it: "a.7z#Game.sfc" is extracted into the content cache
//     and the core gets the extracted file's path. The host then reads it
//     (need_fullpath = false) or the core opens it (need_fullpath = true);
//     either way the core never sees an archive path, because nothing
//     guarantees a core sends every file access through libretro's VFS.

#include <cstddef>
#include <string>

#include "libretro_host.h"   // LibretroCoreInfo

// True if `core` takes the archive at `archive_path` whole (see above).
bool content_archive_core_takes_whole(const LibretroCoreInfo* core, const char* archive_path);

// The path the core gets for `content_path`: the same path for a normal file
// or a whole archive; for a path into an archive ("a.7z#Game.sfc") the file
// extracted into `cache_directory`. False (logged) if extraction failed or
// there's no cache folder.
bool content_archive_resolve(const char* content_path, const char* cache_directory, std::string* load_path);

// The game's name, for settings and lists: "Chrono Trigger (USA)" for both
// ".../Chrono Trigger (USA).sfc" and ".../snes.7z#USA/Chrono Trigger (USA).sfc".
bool content_archive_game_name(const char* content_path, char* out, size_t out_size);
