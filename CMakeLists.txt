cmake_minimum_required(VERSION 3.16)

project(mvstab VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(CTest)
option(MVSTAB_BUILD_CLI "Build the FFmpeg-backed command-line application" ON)
option(MVSTAB_REQUIRE_EXACT_METADATA_TESTS
       "Require the patched H.264 metadata path in end-to-end tests" OFF)

add_library(mvstab_core
    src/confidence.c
    src/mv_normalize.c
    src/pose_graph.c
    src/timeline.c
    src/translation_fit.c
)
target_include_directories(mvstab_core PUBLIC include)
if(NOT MSVC)
    target_link_libraries(mvstab_core PUBLIC m)
endif()

add_library(mvstab_writers
    src/csv_writer.c
    src/json_writer.c
    src/trf_writer.c
)
target_include_directories(mvstab_writers PUBLIC include PRIVATE src)
target_link_libraries(mvstab_writers PUBLIC mvstab_core)

if(MSVC)
    target_compile_definitions(mvstab_core PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(mvstab_writers PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mvstab_core PRIVATE /W4 /WX)
    target_compile_options(mvstab_writers PRIVATE /W4 /WX)
else()
    target_compile_options(mvstab_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(mvstab_writers PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(MVSTAB_BUILD_CLI)
    add_library(mvstab_ffmpeg INTERFACE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
            libavformat
            libavcodec
            libavutil
        )
    endif()
    if(FFMPEG_FOUND)
        target_link_libraries(mvstab_ffmpeg INTERFACE PkgConfig::FFMPEG)
    else()
        find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h REQUIRED)
        find_library(AVFORMAT_LIBRARY avformat REQUIRED)
        find_library(AVCODEC_LIBRARY avcodec REQUIRED)
        find_library(AVUTIL_LIBRARY avutil REQUIRED)
        target_include_directories(mvstab_ffmpeg INTERFACE ${FFMPEG_INCLUDE_DIR})
        target_link_libraries(mvstab_ffmpeg INTERFACE
            ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY})
    endif()
    add_executable(mvstab
        src/cli.c
        src/ffmpeg_reader.c
        src/inspect.c
        src/main.c
    )
    target_include_directories(mvstab PRIVATE include src)
    target_link_libraries(mvstab PRIVATE mvstab_writers mvstab_ffmpeg)
    if(MSVC)
        target_compile_definitions(mvstab PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(mvstab PRIVATE /W4)
    else()
        target_compile_options(mvstab PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(test_mv_normalize tests/test_mv_normalize.c)
    target_link_libraries(test_mv_normalize PRIVATE mvstab_core)
    add_test(NAME mv_normalize COMMAND test_mv_normalize)

    add_executable(test_translation tests/test_translation.c)
    target_link_libraries(test_translation PRIVATE mvstab_core)
    add_test(NAME translation COMMAND test_translation)

    add_executable(test_timeline tests/test_timeline.c)
    target_link_libraries(test_timeline PRIVATE mvstab_core)
    add_test(NAME timeline COMMAND test_timeline)

    add_executable(test_writers tests/test_writers.c)
    target_include_directories(test_writers PRIVATE src)
    target_link_libraries(test_writers PRIVATE mvstab_writers)
    add_test(NAME writers COMMAND test_writers)

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME compare_tools
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compare_tools.py
        )
        set_tests_properties(compare_tools PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        )
    endif()

    find_program(MVSTAB_BASH_EXECUTABLE bash)
    if(MVSTAB_BASH_EXECUTABLE)
        add_test(
            NAME build_helper_guards
            COMMAND ${MVSTAB_BASH_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_build_helper.sh
                    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_patched_ffmpeg.sh
        )
    endif()
    if(MVSTAB_BUILD_CLI AND MVSTAB_BASH_EXECUTABLE)
        add_test(
            NAME end_to_end
            COMMAND ${MVSTAB_BASH_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_end_to_end.sh
                    $<TARGET_FILE:mvstab>
        )
        set_tests_properties(end_to_end PROPERTIES SKIP_RETURN_CODE 77)
        if(MVSTAB_REQUIRE_EXACT_METADATA_TESTS)
            set_tests_properties(end_to_end PROPERTIES
                ENVIRONMENT "MVSTAB_REQUIRE_EXACT_METADATA=1")
        endif()
    endif()
endif()
