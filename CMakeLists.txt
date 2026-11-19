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
)

target_include_directories(frontend_libretro PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(frontend_libretro PUBLIC host_libretro frontend_common)

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
endif()
