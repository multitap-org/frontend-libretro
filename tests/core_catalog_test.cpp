// Checks core_catalog: a core's subsystems come from peeking it, survive the
// cache file (read back without peeking again), and a cache from an older
// format is ignored. Also core_session_special_game_name.
//
// Usage: frontend_core_catalog_test <host-libretro's test core> <scratch folder>
// Exit code 0 = all checks passed. Each failure is printed.

#include <cstdio>
#include <cstring>
#include <string>

#include "core_catalog.h"
#include "core_session.h"
#include "host_fs.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

// What the test core declares (deps/libretro/host-libretro/tests/test_core.cpp).
static void check_test_core_subsystems(const CoreCatalog* catalog, const char* when)
{
    char what[256] = {};
    std::snprintf(what, sizeof(what), "%s: one core", when);
    check(catalog->count == 1, what);
    if (catalog->count != 1)
    {
        return;
    }
    const LibretroSubsystems* subsystems = &catalog->entries[0].info.subsystems;
    std::snprintf(what, sizeof(what), "%s: one subsystem (the one without ident dropped)", when);
    check(subsystems->list.size() == 1, what);
    const LibretroSubsystem* link = libretro_subsystem_find(subsystems, "testlink");
    std::snprintf(what, sizeof(what), "%s: testlink found", when);
    check(link != nullptr, what);
    if (link == nullptr)
    {
        return;
    }
    std::snprintf(what, sizeof(what), "%s: testlink's id and description", when);
    check(link->id == 0x10 && std::strcmp(link->description, "Test Link") == 0, what);
    std::snprintf(what, sizeof(what), "%s: two files", when);
    check(link->roms.size() == 2, what);
    if (link->roms.size() != 2)
    {
        return;
    }
    const LibretroSubsystemRom* a = &link->roms[0];
    const LibretroSubsystemRom* b = &link->roms[1];
    std::snprintf(what, sizeof(what), "%s: cart A", when);
    check(std::strcmp(a->description, "Test Cart A") == 0 && std::strcmp(a->extensions, "bin") == 0 && a->required &&
              !a->need_fullpath && a->memory_count == 1 && a->memory[0].type == 0x100 &&
              std::strcmp(a->memory[0].extension, "sava") == 0,
          what);
    std::snprintf(what, sizeof(what), "%s: cart B (memory extension without its dot)", when);
    check(std::strcmp(b->description, "Test Cart B") == 0 && std::strcmp(b->extensions, "bin|txt") == 0 && !b->required &&
              b->need_fullpath && b->memory_count == 1 && b->memory[0].type == 0x101 &&
              std::strcmp(b->memory[0].extension, "savb") == 0,
          what);
}

static void test_catalog(const char* test_core, const char* scratch)
{
    std::string cores = std::string(scratch) + "/cores";
    std::string cache = std::string(scratch) + "/core_info_cache.cfg";
    std::string copy = cores + "/test_core_libretro.so";

    uint8_t* data = nullptr;
    size_t size = 0;
    check(host_fs_read_file(test_core, &data, &size), "read the test core");
    check(data != nullptr && host_fs_write_file(copy.c_str(), data, size), "copy the test core");
    delete[] data;

    // A cache from before subsystems were cached (no version line): ignored.
    static const char OLD_CACHE[] = "[test_core_libretro.so]\nsize=1\nmtime=1\nusable=1\nname=stale\n\n";
    check(host_fs_write_file(cache.c_str(), OLD_CACHE, sizeof(OLD_CACHE) - 1), "write an old cache");

    CoreCatalog* catalog = new CoreCatalog{};
    core_catalog_init(catalog, cores.c_str(), cache.c_str());
    check(catalog->count == 0, "old cache ignored");
    core_catalog_scan(catalog, nullptr);
    check(catalog->peeked_last_scan == 1, "first scan peeks the core");
    check_test_core_subsystems(catalog, "peeked");
    delete catalog;

    // A new catalog reads the cache: same subsystems, nothing peeked.
    catalog = new CoreCatalog{};
    core_catalog_init(catalog, cores.c_str(), cache.c_str());
    check_test_core_subsystems(catalog, "from the cache");
    core_catalog_scan(catalog, nullptr);
    check(catalog->peeked_last_scan == 0, "second scan peeks nothing");
    check_test_core_subsystems(catalog, "after the second scan");
    delete catalog;
}

static void test_special_game_name()
{
    const char* paths[] = { "/games/SGB.sfc", nullptr, "/games/gb.7z#Puzzle/Tetris (World).gb" };
    char name[256] = {};
    check(core_session_special_game_name(paths, 3, name, sizeof(name)), "special name");
    check(std::strcmp(name, "SGB + Tetris (World)") == 0, "special name: file names joined, archive's inner name");

    const char* none[] = { nullptr, "" };
    check(!core_session_special_game_name(none, 2, name, sizeof(name)) && name[0] == '\0', "special name: no files");

    char tiny[8] = {};
    check(!core_session_special_game_name(paths, 3, tiny, sizeof(tiny)) && tiny[0] == '\0', "special name: too long");
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::printf("usage: frontend_core_catalog_test <test core> <scratch folder>\n");
        return 2;
    }
    test_catalog(argv[1], argv[2]);
    test_special_game_name();
    if (g_failures == 0)
    {
        std::printf("core catalog: all checks passed\n");
        return 0;
    }
    std::printf("core catalog: %d check(s) failed\n", g_failures);
    return 1;
}
