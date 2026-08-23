cmake_minimum_required(VERSION 3.20)
project(Prose2000SAPI VERSION 1.0.0 LANGUAGES CXX)

# ProseHost.exe is a self-contained 64-bit console program, and a 32-bit process can spawn
# it perfectly well under WOW64. Both architectures therefore use the identical
# out-of-process design and share every source file here - there is no 32-bit helper and
# no COM surrogate. Configure with -A Win32 and -A x64 into separate build directories.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(STATUS "Prose 2000 SAPI5: building x64")
else()
    message(STATUS "Prose 2000 SAPI5: building x86")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(PROSE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# --------------------------------------------------------------------------------------
# Everything that does not need SAPI: logging, path discovery, the settings store, the
# voice and parameter catalogue, and the emulator client itself.
# --------------------------------------------------------------------------------------
add_library(prose_core STATIC
    ${PROSE_SRC}/prose_log.cpp
    ${PROSE_SRC}/prose_paths.cpp
    ${PROSE_SRC}/prose_voices.cpp
    ${PROSE_SRC}/prose_settings.cpp
    ${PROSE_SRC}/prose_host.cpp
)
target_include_directories(prose_core PUBLIC ${PROSE_SRC})
target_link_libraries(prose_core PUBLIC ole32 oleaut32 uuid advapi32 shell32)

set(PROSE_TARGETS prose_core)

# --------------------------------------------------------------------------------------
# The SAPI 5 engine DLL. Identical sources for both architectures.
# --------------------------------------------------------------------------------------
add_library(Prose2000SAPI5 SHARED
    ${PROSE_SRC}/prose_main.cpp
    ${PROSE_SRC}/prose_com.cpp
    ${PROSE_SRC}/prose_registry.cpp
    ${PROSE_SRC}/prose_datakey.cpp
    ${PROSE_SRC}/prose_token.cpp
    ${PROSE_SRC}/prose_enum_tokens.cpp
    ${PROSE_SRC}/prose_tts_engine.cpp
    ${PROSE_SRC}/prose2000_sapi.def
)
target_link_libraries(Prose2000SAPI5 PRIVATE prose_core sapi)
list(APPEND PROSE_TARGETS Prose2000SAPI5)

# --------------------------------------------------------------------------------------
# Renders WAV files straight through the emulator client, with no SAPI and no
# registration. This is the check that the engine itself works.
# --------------------------------------------------------------------------------------
add_executable(prose_render ${CMAKE_CURRENT_SOURCE_DIR}/tools/prose_render.cpp)
target_link_libraries(prose_render PRIVATE prose_core)
list(APPEND PROSE_TARGETS prose_render)

# Drives the DLL through its class objects without registering it, so the whole SAPI path
# can be tested without administrator rights.
add_executable(prose_sapitest ${CMAKE_CURRENT_SOURCE_DIR}/tools/prose_sapitest.cpp)
target_link_libraries(prose_sapitest PRIVATE prose_core sapi)
list(APPEND PROSE_TARGETS prose_sapitest)

# Speaks out loud through the registered SAPI 5 stack. This is the post-install check the
# installer offers to run, and the one that proves registration worked.
add_executable(prose_speak ${CMAKE_CURRENT_SOURCE_DIR}/tools/prose_speak.cpp)
target_link_libraries(prose_speak PRIVATE prose_core sapi)
list(APPEND PROSE_TARGETS prose_speak)

# --------------------------------------------------------------------------------------
# The configuration utility. Built only as 64-bit: one copy edits the per-user settings
# that both engines read, so a second 32-bit build would be the same program twice.
# --------------------------------------------------------------------------------------
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(Prose2000Config WIN32
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/prose_config.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/prose_config.rc
    )
    target_link_libraries(Prose2000Config PRIVATE prose_core comctl32 sapi)
    # The manifest is embedded from prose_config.rc, so the linker must not also generate
    # one: both land on resource id 1 and the resource compiler refuses the duplicate.
    set_target_properties(Prose2000Config PROPERTIES LINK_FLAGS "/MANIFEST:NO")
    list(APPEND PROSE_TARGETS Prose2000Config)
endif()

foreach(target ${PROSE_TARGETS})
    target_compile_definitions(${target} PRIVATE
        UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3 /MP /utf-8)
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endforeach()
