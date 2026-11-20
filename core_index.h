#pragma once

// The core downloader's index: what one target's cores are, as the server
// publishes them (https://cores.get-piccolo.app/v1/index/<target>.cfg, signed
// with index_sign; core_updater checks the signature before parsing). The
// format is piccolo's cfg dialect (settings_file):
//
//   release=cores-20261120-3
//   target=linux-x86_64
//   [linux-x86_64/sameboy]
//   name=SameBoy
//   file=sameboy_libretro.so
//   version=v1.0.5 ... free=true type=main ... url=https://... (or relative to the site)
//
// Built by multitap-org/cores (scripts/make-release.sh) and the server's
// publisher; see multitap-org/cores' README for every field.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// How the downloader lists a core (the index's `type=`, chosen in
// multitap-org/cores): main ones always, the others when the user asks.
enum class CoreType
{
    main,           // listed by default ("type=main", or no type: older indexes)
    legacy,         // older versions of a core (Snes9x 2005, ...): OLDER VERSIONS
    experimental,   // unsupported, or not fully working in our frontends: EXPERIMENTAL
};

struct CoreIndexEntry
{
    std::string core;             // "sameboy"
    std::string name;             // "SameBoy"
    std::string file;             // "sameboy_libretro.so": the name it's installed under
    std::string version;          // upstream tag, "v1.0.5"
    std::string commit;           // upstream commit (what "up to date" compares)
    std::string license;          // SPDX-style, "MIT", "GPL-2.0", "Non-commercial"
    bool        free = false;     // free-licensed (store builds offer only these)
    CoreType    type = CoreType::main;
    std::string systems;          // "Game Boy|Game Boy Color"
    std::string url;              // the zip
    std::string sha256;           // of the zip
    int64_t     size = 0;         // of the zip
    std::string library_sha256;   // of the library inside
    int64_t     library_size = 0;
    std::string source_url;       // the exact source (GPL source offer)
};

struct CoreIndex
{
    std::string release;
    std::string target;
    std::vector<CoreIndexEntry> entries;   // sorted by name
};

// The target this build downloads cores for ("linux-x86_64", "windows-x86_64",
// "android-arm64-v8a", ...; set by CMake), "" if the build has none.
const char* core_index_build_target();

// Parses an index. Entries missing a needed field, with a file name that
// isn't a plain file name, or for another target are logged and skipped.
// False (logged) if the index is for another target than `target`.
bool core_index_parse(CoreIndex* index, const char* text, size_t size, const char* target);

// Finds the entry installed as `file`, or -1.
int32_t core_index_find_file(const CoreIndex* index, const char* file);
