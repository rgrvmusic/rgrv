# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)

project(rgrv VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(RGRV_WITH_RTMIDI "Build live MIDI output through RtMidi" ON)
option(RGRV_USE_SYSTEM_RTMIDI "Prefer an installed RtMidi over fetching one" ON)

# ---------------------------------------------------------------------------
# WebAssembly / WebMIDI
#
# phrase.c behind src/wasm.c, loaded by the page in web/. The page owns MIDI
# and its clock; the grammar stays in C.
# ---------------------------------------------------------------------------
if(EMSCRIPTEN)
    add_executable(rgrv_web
        src/wasm.c
        src/phrase.c
        src/midi.c
    )
    set_target_properties(rgrv_web PROPERTIES
        OUTPUT_NAME rgrv
        SUFFIX ".js"
    )

    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(rgrv_web PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    target_link_options(rgrv_web PRIVATE
        "--no-entry"
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=createRGRV"
        "-sENVIRONMENT=web,node"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sFILESYSTEM=0"
        "-sASSERTIONS=0"
        "-sEXPORTED_RUNTIME_METHODS=['ccall','UTF8ToString','HEAPU8']"
        "-sEXPORTED_FUNCTIONS=['_rgrv_web_build','_rgrv_web_build_midi','_rgrv_web_midi_ptr','_rgrv_web_midi_size','_rgrv_web_error','_rgrv_web_version','_rgrv_web_note_count','_rgrv_web_note_tick','_rgrv_web_note_dur','_rgrv_web_note_number','_rgrv_web_note_velocity','_rgrv_web_steps','_rgrv_web_loop_start','_rgrv_web_loop_end','_rgrv_web_has_loop','_rgrv_web_clamped','_rgrv_web_kick','_rgrv_web_snare','_rgrv_web_step_ticks','_rgrv_web_steps_per_bar','_rgrv_web_base_note']"
    )

    # web/index.html loads wasm/rgrv.js.
    add_custom_command(TARGET rgrv_web POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_SOURCE_DIR}/web/wasm"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/rgrv.js"
                "${CMAKE_CURRENT_BINARY_DIR}/rgrv.wasm"
                "${CMAKE_SOURCE_DIR}/web/wasm"
    )
    return()
endif()

add_executable(rgrv
    src/main.c
    src/phrase.c
    src/midi.c
)

# Capitalised alias beside the lowercase binary, for the examples.
if(NOT WIN32)
    add_custom_command(TARGET rgrv POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_CURRENT_BINARY_DIR}/RGRV
        COMMAND ${CMAKE_COMMAND} -E create_symlink rgrv
                ${CMAKE_CURRENT_BINARY_DIR}/RGRV
    )
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rgrv PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# RtMidi
#
# Taken from the wint14syssys container through find_path/find_library, which
# follows CMAKE_FIND_ROOT_PATH when cross-compiling; only if absent is 6.0.0
# fetched. rtmidi_c.h keeps the build plain C.
# ---------------------------------------------------------------------------
set(RGRV_RTMIDI_TARGET "")
if(RGRV_WITH_RTMIDI)
    if(RGRV_USE_SYSTEM_RTMIDI)
        find_path(RGRV_RTMIDI_INCLUDE_DIR NAMES rtmidi/rtmidi_c.h rtmidi_c.h)
        find_library(RGRV_RTMIDI_LIBRARY NAMES rtmidi)
    endif()

    if(RGRV_RTMIDI_INCLUDE_DIR AND RGRV_RTMIDI_LIBRARY)
        message(STATUS "RtMidi: ${RGRV_RTMIDI_LIBRARY}")
        target_include_directories(rgrv PRIVATE ${RGRV_RTMIDI_INCLUDE_DIR})
        target_link_libraries(rgrv PRIVATE ${RGRV_RTMIDI_LIBRARY})
        target_compile_definitions(rgrv PRIVATE RGRV_HAVE_RTMIDI=1)
    else()
        message(STATUS "RtMidi: not installed, fetching 6.0.0")
        include(FetchContent)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(RTMIDI_API_JACK OFF CACHE BOOL "" FORCE)
        set(RTMIDI_API_AMIDI OFF CACHE BOOL "" FORCE)
        if(WIN32)
            set(RTMIDI_API_WINMM ON CACHE BOOL "" FORCE)
        elseif(APPLE)
            set(RTMIDI_API_CORE ON CACHE BOOL "" FORCE)
        else()
            find_package(ALSA QUIET)
            if(ALSA_FOUND)
                set(RTMIDI_API_ALSA ON CACHE BOOL "" FORCE)
            else()
                message(WARNING
                    "RtMidi: libasound headers not found; the fetched RtMidi "
                    "will have no ALSA backend")
            endif()
        endif()
        FetchContent_Declare(rtmidi
            URL https://github.com/thestk/rtmidi/archive/refs/tags/6.0.0.tar.gz
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(rtmidi)
        target_link_libraries(rgrv PRIVATE rtmidi)
        target_include_directories(rgrv PRIVATE ${rtmidi_SOURCE_DIR})
        target_compile_definitions(rgrv PRIVATE RGRV_HAVE_RTMIDI=1)
        set(RGRV_RTMIDI_TARGET rtmidi)
    endif()
else()
    message(STATUS "RtMidi: disabled (--raw and --out still work)")
endif()

# ---------------------------------------------------------------------------
# RtMidi runtime beside the native binary
#
# The container has RtMidi's development files, but the host has neither the
# library nor /dev/snd/seq. Bundle librtmidi.so and put $ORIGIN on the runtime
# path, so ./build/rgrv also runs on the host. The Windows build links a static
# archive and needs none of this.
# ---------------------------------------------------------------------------
if(RGRV_RTMIDI_LIBRARY AND NOT RGRV_RTMIDI_TARGET AND NOT WIN32 AND NOT APPLE)
    get_filename_component(_rgrv_rtmidi_real "${RGRV_RTMIDI_LIBRARY}" REALPATH)

    find_program(RGRV_READELF NAMES readelf llvm-readelf)
    if(RGRV_READELF)
        execute_process(COMMAND "${RGRV_READELF}" -d "${_rgrv_rtmidi_real}"
            OUTPUT_VARIABLE _rgrv_rtmidi_dyn
            ERROR_QUIET)
        string(REGEX MATCH "SONAME[^\n]*\\[([^]]+)\\]" _match "${_rgrv_rtmidi_dyn}")
        set(_rgrv_rtmidi_soname "${CMAKE_MATCH_1}")
    endif()
    if(NOT _rgrv_rtmidi_soname)
        get_filename_component(_rgrv_rtmidi_soname "${_rgrv_rtmidi_real}" NAME)
    endif()

    message(STATUS "RtMidi runtime: bundling ${_rgrv_rtmidi_soname}")
    add_custom_command(TARGET rgrv POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_rgrv_rtmidi_real}"
                "${CMAKE_CURRENT_BINARY_DIR}/${_rgrv_rtmidi_soname}"
    )
    set_target_properties(rgrv PROPERTIES
        BUILD_RPATH "\$ORIGIN"
        INSTALL_RPATH "\$ORIGIN/../lib"
    )
    install(FILES "${_rgrv_rtmidi_real}"
            DESTINATION lib
            RENAME "${_rgrv_rtmidi_soname}")
endif()

# A static RtMidi is C++; on Linux naming libstdc++ twice is harmless.
if(RGRV_WITH_RTMIDI AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
    target_link_libraries(rgrv PRIVATE stdc++)
endif()

if(WIN32)
    target_compile_definitions(rgrv PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(rgrv PRIVATE winmm ole32)
    if(MINGW)
        # RtMidi comes as a static C++ archive; -static keeps the runtime
        # inside the .exe.
        target_link_options(rgrv PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(rgrv PRIVATE Threads::Threads)
endif()

install(TARGETS rgrv DESTINATION bin)
