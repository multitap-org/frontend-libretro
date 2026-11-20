# frontend_libretro: what every libretro frontend shares on top of
# host-libretro and frontend_common: the core session (run a core in a window:
# video texture, audio, timing, input, states), keyboard and fixed controls,
# and the catalog of installed cores. Used by piccolo and libretro-test-frontend
# (later Multitap); each adds only its own UI.
# A library: it doesn't build alone. The frontend adds host-libretro and
# frontends-common first.

multitap_require(frontend-libretro host_libretro frontend_common)

# Games load from zip / 7z (content_archive.cpp): frontends-common builds that
# only when the frontend has deps/miniz and deps/7zip.
if(NOT TARGET multitap_dep_miniz OR NOT TARGET multitap_dep_7z)
    message(FATAL_ERROR "frontend-libretro needs deps/miniz and deps/7zip (zip / 7z reading in frontends-common)")
endif()

add_library(frontend_libretro STATIC
    core_session.cpp
    core_catalog.cpp
    keyboard_input.cpp
    fixed_input.cpp
    core_options_store.cpp
    core_options_cache.cpp
    content_archive.cpp
    core_index.cpp
)

target_include_directories(frontend_libretro PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(frontend_libretro PUBLIC host_libretro frontend_common)

# The core downloader (core_updater.h). Built where frontends-common has
# downloads (frontend_download: libcurl + Monocypher) and not switched off;
# store builds (Play, Steam) pass -DMULTITAP_CORE_UPDATER=OFF: stores don't
# allow downloading code. Otherwise the same API with nothing behind it.
option(MULTITAP_CORE_UPDATER "Build the core downloader (OFF for store builds)" ON)
if(MULTITAP_CORE_UPDATER AND TARGET frontend_download)
    target_sources(frontend_libretro PRIVATE core_updater.cpp)
    target_link_libraries(frontend_libretro PUBLIC frontend_download)
    message(STATUS "frontend-libretro: core downloader on")
else()
    target_sources(frontend_libretro PRIVATE core_updater_none.cpp)
    message(STATUS "frontend-libretro: core downloader off")
endif()

# The target cores are downloaded for (core_index_build_target), named as
# multitap-org/cores builds them. "" where there's no build of the cores.
set(MULTITAP_CORE_TARGET "")
if(ANDROID)
    set(MULTITAP_CORE_TARGET "android-arm64-v8a")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(MULTITAP_CORE_TARGET "linux-x86_64")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(MULTITAP_CORE_TARGET "linux-aarch64")
elseif(WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(MULTITAP_CORE_TARGET "windows-x86_64")
endif()
set_source_files_properties(core_index.cpp PROPERTIES COMPILE_DEFINITIONS "MULTITAP_CORE_TARGET=\"${MULTITAP_CORE_TARGET}\"")

multitap_set_project_flags(frontend_libretro)

if(MULTITAP_BUILD_TESTS)
    add_executable(frontend_core_options_store_test tests/core_options_store_test.cpp)
    target_link_libraries(frontend_core_options_store_test PRIVATE frontend_libretro)
    multitap_set_project_flags(frontend_core_options_store_test)
    add_test(NAME frontend_core_options_store COMMAND frontend_core_options_store_test)
    set_tests_properties(frontend_core_options_store PROPERTIES PASS_REGULAR_EXPRESSION "core options store: all checks passed")

    add_executable(frontend_core_options_cache_test tests/core_options_cache_test.cpp)
    target_link_libraries(frontend_core_options_cache_test PRIVATE frontend_libretro)
    multitap_set_project_flags(frontend_core_options_cache_test)
    add_test(NAME frontend_core_options_cache COMMAND frontend_core_options_cache_test)
    set_tests_properties(frontend_core_options_cache PROPERTIES PASS_REGULAR_EXPRESSION "core options cache: all checks passed")
    # Uses frontends-common's archive test data.
    add_executable(frontend_content_archive_test tests/content_archive_test.cpp)
    target_link_libraries(frontend_content_archive_test PRIVATE frontend_libretro)
    multitap_set_project_flags(frontend_content_archive_test)
    set(CONTENT_ARCHIVE_SCRATCH "${CMAKE_CURRENT_BINARY_DIR}/content_archive_test")
    add_test(NAME frontend_content_archive_clean COMMAND ${CMAKE_COMMAND} -E rm -rf "${CONTENT_ARCHIVE_SCRATCH}")
    set_tests_properties(frontend_content_archive_clean PROPERTIES FIXTURES_SETUP content_archive_scratch)
    add_test(NAME frontend_content_archive
             COMMAND frontend_content_archive_test "$<TARGET_PROPERTY:frontend_common,SOURCE_DIR>/tests/data"
                     "${CONTENT_ARCHIVE_SCRATCH}")
    set_tests_properties(frontend_content_archive PROPERTIES
                         FIXTURES_REQUIRED content_archive_scratch
                         PASS_REGULAR_EXPRESSION "content archive: all checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    if(MULTITAP_CORE_UPDATER AND TARGET frontend_download)
        # Fake core sites in tests/data/core_updater, served with file:// URLs.
        add_executable(frontend_core_updater_test tests/core_updater_test.cpp)
        target_link_libraries(frontend_core_updater_test PRIVATE frontend_libretro)
        multitap_set_project_flags(frontend_core_updater_test)
        set(CORE_UPDATER_SCRATCH "${CMAKE_CURRENT_BINARY_DIR}/core_updater_test")
        add_test(NAME frontend_core_updater_clean COMMAND ${CMAKE_COMMAND} -E rm -rf "${CORE_UPDATER_SCRATCH}")
        set_tests_properties(frontend_core_updater_clean PROPERTIES FIXTURES_SETUP core_updater_scratch)
        add_test(NAME frontend_core_updater
                 COMMAND frontend_core_updater_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/core_updater"
                         "${CORE_UPDATER_SCRATCH}")
        set_tests_properties(frontend_core_updater PROPERTIES
                             FIXTURES_REQUIRED core_updater_scratch
                             PASS_REGULAR_EXPRESSION "core updater: all checks passed"
                             FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")
    endif()
endif()
