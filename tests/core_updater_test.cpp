// core_updater against fake core sites served with file:// URLs
// (tests/data/core_updater, README.md there):
//   core_updater_test <tests/data/core_updater folder> <scratch folder>
// Refresh (signature, target filter), install, update (.old kept), the
// running core and non-free cores refused, remove, a core put there by hand,
// a changed index and a changed zip refused.

#include <cstdio>
#include <cstring>
#include <string>

#include <SDL3/SDL.h>

#include "core_updater.h"
#include "host_fs.h"
#include "net_http.h"

static int32_t failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

// The throwaway key the test sites are signed with.
static const char* const TEST_KEYS[] = {
    "66ed9135cc62c8225c4e459dd0f48665bd73e05e673deaa0c500068a222dd032",
};

static std::string g_data;
static std::string g_scratch;

static bool init_site(CoreUpdater* updater, const char* site)
{
    std::string base = "file://" + g_data + "/" + site;
    return core_updater_init(updater, "multitap-test", base.c_str(), true, TEST_KEYS, 1, "test-target",
                             (g_scratch + "/cores").c_str(), (g_scratch + "/work").c_str());
}

// Polls like a frontend's frame loop until the task ends (10 s at most).
static void wait_idle(CoreUpdater* updater)
{
    for (int32_t i = 0; i < 10000 && updater->state != CoreUpdaterState::idle; i++)
    {
        SDL_Delay(1);
        core_updater_poll(updater);
    }
}

static std::string read_text(const std::string& path)
{
    uint8_t* data = nullptr;
    size_t size = 0;
    if (!host_fs_read_file(path.c_str(), &data, &size))
    {
        return std::string();
    }
    std::string text(reinterpret_cast<const char*>(data), size);
    delete[] data;
    return text;
}

static int32_t find(const CoreUpdater* updater, const char* name)
{
    for (size_t i = 0; i < updater->index.entries.size(); i++)
    {
        if (updater->index.entries[i].name == name)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::printf("usage: core_updater_test <tests/data/core_updater folder> <scratch folder>\n");
        return 2;
    }
    g_data    = argv[1];
    g_scratch = argv[2];
    std::string cores = g_scratch + "/cores";
    std::string core  = cores + "/fake_libretro.so";
    check(host_fs_make_directories(cores.c_str()), "cores folder");
    check(net_http_init("multitap-test"), "net_http_init");
    check(core_updater_available(), "available in this build");
    check(core_updater_key_count() >= 1 && std::strlen(core_updater_key_hex(0)) == 64, "project key present");

    CoreUpdater updater;
    check(!core_updater_init(&updater, "multitap-test", "http://example.invalid", false, TEST_KEYS, 1, "test-target",
                             cores.c_str(), (g_scratch + "/work").c_str()),
          "plain http refused without allow_insecure");

    // Refresh: two entries for test-target (the other target's block skipped), by name.
    check(init_site(&updater, "site1"), "init site1");
    core_updater_refresh(&updater);
    check(updater.state == CoreUpdaterState::refreshing, "refreshing");
    wait_idle(&updater);
    check(!updater.message_is_error, "refresh ok");
    check(updater.index.entries.size() == 2, "two entries");
    int32_t fake    = find(&updater, "Fake");
    int32_t nonfree = find(&updater, "Nonfree");
    check(fake == 0 && nonfree == 1, "sorted by name");
    check(core_updater_status(&updater, fake) == CoreStatus::not_installed, "not installed yet");

    // Install.
    check(core_updater_install(&updater, fake, nullptr), "install starts");
    check(!core_updater_install(&updater, fake, nullptr), "second task refused while busy");
    wait_idle(&updater);
    check(!updater.message_is_error, "install ok");
    check(read_text(core) == "fake core version 1\n", "installed bytes");
    check(!host_fs_exists((core + ".new").c_str()), "no .new left");
    check(core_updater_status(&updater, fake) == CoreStatus::installed, "status installed");
    check(updater.cores_changed, "cores_changed set");
    check(core_updater_installed_version(&updater, "fake_libretro.so") == "aaaaaaa", "installed version recorded");

    // The running core and non-free cores in free-only builds are refused.
    check(!core_updater_install(&updater, fake, "fake_libretro.so"), "running core refused");
    check(!core_updater_remove(&updater, fake, "fake_libretro.so"), "running core not removed");
    updater.free_only = true;
    check(!core_updater_install(&updater, nonfree, nullptr), "non-free refused when free_only");
    updater.free_only = false;

    // A newer index: update, the old one kept as .old.
    check(init_site(&updater, "site2"), "init site2");
    core_updater_refresh(&updater);
    wait_idle(&updater);
    fake = find(&updater, "Fake");
    check(core_updater_status(&updater, fake) == CoreStatus::update, "update offered");
    check(core_updater_install(&updater, fake, nullptr), "update starts");
    wait_idle(&updater);
    check(read_text(core) == "fake core version 2, a bit longer\n", "updated bytes");
    check(read_text(core + ".old") == "fake core version 1\n", ".old is the previous one");
    check(core_updater_status(&updater, fake) == CoreStatus::installed, "up to date after update");

    // Remove: the core and its .old.
    check(core_updater_remove(&updater, fake, nullptr), "remove");
    check(!host_fs_exists(core.c_str()) && !host_fs_exists((core + ".old").c_str()), "files removed");
    check(core_updater_status(&updater, fake) == CoreStatus::not_installed, "not installed after remove");

    // A core put there by hand.
    check(host_fs_write_file(core.c_str(), "mine\n", 5), "core by hand");
    check(core_updater_status(&updater, fake) == CoreStatus::installed_by_hand, "installed by hand");

    // An index changed after signing: nothing from it is used.
    check(init_site(&updater, "bad_sig"), "init bad_sig");
    core_updater_refresh(&updater);
    wait_idle(&updater);
    check(updater.message_is_error && updater.index.entries.empty(), "changed index refused");

    // A zip changed after the index was made: refused, the core there untouched.
    check(init_site(&updater, "bad_zip"), "init bad_zip");
    core_updater_refresh(&updater);
    wait_idle(&updater);
    fake = find(&updater, "Fake");
    check(fake >= 0, "bad_zip index ok");
    if (fake >= 0)
    {
        check(core_updater_install(&updater, fake, nullptr), "bad zip install starts");
        wait_idle(&updater);
        check(updater.message_is_error, "changed zip refused");
        check(read_text(core) == "mine\n", "the core there is untouched");
        check(!host_fs_exists((core + ".new").c_str()), "no .new after a bad zip");
    }

    // A missing zip (the non-free entry points nowhere).
    nonfree = find(&updater, "Nonfree");
    check(core_updater_install(&updater, nonfree, nullptr), "missing zip install starts");
    wait_idle(&updater);
    check(updater.message_is_error && !host_fs_exists((cores + "/nonfree_libretro.so").c_str()),
          "missing zip fails");

    // Wrong target.
    CoreIndex index;
    const char* text = "release=x\ntarget=linux-x86_64\n";
    check(!core_index_parse(&index, text, std::strlen(text), "test-target"), "index for another target refused");

    core_updater_shutdown(&updater);
    net_http_shutdown();
    if (failures == 0)
    {
        std::printf("core updater: all checks passed\n");
        return 0;
    }
    return 1;
}
