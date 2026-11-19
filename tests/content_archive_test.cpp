// Checks content_archive.h: which cores take an archive whole, what path a
// core gets (normal files unchanged, a file in a 7z extracted into the
// cache), and game names from archive paths.
//
// Usage: frontend_content_archive_test <frontends-common tests/data> <scratch folder>
// Exit code 0 = all checks passed. Each failure is printed.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "content_archive.h"
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

static LibretroCoreInfo core_with(const char* extensions, bool block_extract)
{
    LibretroCoreInfo core = {};
    core.usable = true;
    std::snprintf(core.extensions, sizeof(core.extensions), "%s", extensions);
    core.block_extract = block_extract;
    return core;
}

static void test_takes_whole()
{
    LibretroCoreInfo snes = core_with("sfc|smc", false);
    LibretroCoreInfo arcade = core_with("zip|7z", false);
    LibretroCoreInfo blocking = core_with("exe|com", true);
    LibretroCoreInfo anything = core_with("", false);
    check(!content_archive_core_takes_whole(&snes, "/r/snes.zip"), "SNES core: archive is browsed");
    check(content_archive_core_takes_whole(&arcade, "/r/pacman.zip"), "arcade core: zip whole");
    check(content_archive_core_takes_whole(&arcade, "/r/PACMAN.7Z"), "arcade core: 7z whole, any case");
    check(content_archive_core_takes_whole(&blocking, "/r/doom.zip"), "block_extract: whole");
    check(!content_archive_core_takes_whole(&anything, "/r/games.zip"), "empty extension list: browsed");
    check(!content_archive_core_takes_whole(&arcade, "/r/pacman.sfc"), "not an archive: never whole");
    check(!content_archive_core_takes_whole(nullptr, "/r/pacman.zip"), "no core: not whole");
}

static ArchiveEntry entry(const char* name, bool is_directory)
{
    ArchiveEntry result = {};
    result.name = name;
    result.is_directory = is_directory;
    return result;
}

static void test_loadable()
{
    LibretroCoreInfo snes = core_with("sfc|smc", false);
    std::vector<ArchiveEntry> entries;
    entries.push_back(entry("Game (Europe).sfc", false));
    entries.push_back(entry("readme.txt", false));
    entries.push_back(entry("__MACOSX", true));
    entries.push_back(entry("__MACOSX/._Game (Europe).sfc", false));
    entries.push_back(entry(".hidden.sfc", false));
    std::string single;
    check(content_archive_loadable(&snes, &entries, &single) == 1 && single == "Game (Europe).sfc",
          "loadable: one game next to a readme and macOS leftovers");

    entries.push_back(entry("disk/Game (USA).smc", false));
    check(content_archive_loadable(&snes, &entries, &single) == 2 && single.empty(), "loadable: two games");

    std::vector<ArchiveEntry> none;
    none.push_back(entry("readme.txt", false));
    none.push_back(entry("dir.sfc", true));
    check(content_archive_loadable(&snes, &none, &single) == 0 && single.empty(),
          "loadable: nothing (a folder named .sfc doesn't count)");
    check(content_archive_loadable(nullptr, &entries, &single) == 0, "loadable: no core");
    check(content_archive_hidden_name(".git") && content_archive_hidden_name("__MACOSX") &&
          !content_archive_hidden_name("Game.sfc"), "hidden names");
}

static void test_game_name()
{
    char name[256] = {};
    check(content_archive_game_name("/r/Chrono Trigger (USA).sfc", name, sizeof(name)) &&
          std::strcmp(name, "Chrono Trigger (USA)") == 0, "name: plain file");
    check(content_archive_game_name("/r/snes.7z#USA/Chrono Trigger (USA).sfc", name, sizeof(name)) &&
          std::strcmp(name, "Chrono Trigger (USA)") == 0, "name: file in an archive folder");
    check(content_archive_game_name("/r/pacman.zip", name, sizeof(name)) && std::strcmp(name, "pacman") == 0,
          "name: whole archive");
}

static void test_resolve(const char* data_folder, const char* scratch)
{
    std::string cache = std::string(scratch) + "/cache";
    std::string path;
    check(content_archive_resolve("/r/Game.sfc", cache.c_str(), &path) && path == "/r/Game.sfc",
          "resolve: normal file unchanged");
    check(content_archive_resolve("/r/pacman.zip", cache.c_str(), &path) && path == "/r/pacman.zip",
          "resolve: whole archive unchanged");

    std::string content = std::string(data_folder) + "/lzma2_solid.7z#Game One.sfc";
    check(content_archive_resolve(content.c_str(), cache.c_str(), &path), "resolve: file in a 7z");
    check(path.compare(0, cache.size(), cache) == 0 && host_fs_exists(path.c_str()),
          "resolve: extracted into the cache");
    char name[256] = {};
    check(host_fs_stem(path.c_str(), name, sizeof(name)) && std::strcmp(name, "Game One") == 0,
          "resolve: the copy keeps the game's name (save files)");

    std::string top = std::string(data_folder) + "/lzma2_solid.7z#";
    check(!content_archive_resolve(top.c_str(), cache.c_str(), &path), "resolve: archive's top folder fails");
    check(!content_archive_resolve(content.c_str(), "", &path), "resolve: no cache folder fails");
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::printf("usage: %s <frontends-common tests/data> <scratch folder>\n", argv[0]);
        return 2;
    }
    test_takes_whole();
    test_loadable();
    test_game_name();
    test_resolve(argv[1], argv[2]);

    if (g_failures == 0)
    {
        std::printf("content archive: all checks passed\n");
        return 0;
    }
    std::printf("content archive: %d check(s) failed\n", g_failures);
    return 1;
}
