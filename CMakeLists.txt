cmake_minimum_required(VERSION 3.20)

project(OLAS VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
set(ONNXRUNTIME_MODE "bundled" CACHE STRING "Where to get ONNX Runtime from")
set(ONNXRUNTIME_ROOT "C:/onnxruntime" CACHE PATH "Path to prebuilt ONNX Runtime")

# ---------------------------------------------------------------------------
# Moonshine
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(
    moonshine
    GIT_REPOSITORY https://github.com/moonshine-ai/moonshine.git
    GIT_TAG main
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR core
)

set(MOONSHINE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MOONSHINE_BUILD_TESTS    OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(moonshine)
# Moonshine's public headers (moonshine-cpp.h) live under its source's core/ dir.
# The target doesn't export an INTERFACE include dir for them, so wire it up.
FetchContent_GetProperties(moonshine)

if(NOT moonshine_POPULATED)
    FetchContent_Populate(moonshine)
endif()

set(MOONSHINE_INCLUDE_DIR "${moonshine_SOURCE_DIR}/core")
if(NOT EXISTS "${MOONSHINE_INCLUDE_DIR}/moonshine-cpp.h")
    message(FATAL_ERROR
        "moonshine-cpp.h not found at ${MOONSHINE_INCLUDE_DIR}. "
        "Check the Moonshine source layout.")
endif()

message(STATUS "OLAS: Moonshine headers at ${MOONSHINE_INCLUDE_DIR}")

set(MOONSHINE_TARGET moonshine)

if(NOT TARGET ${MOONSHINE_TARGET})
    message(FATAL_ERROR
        "Could not find the 'moonshine' CMake target. "
        "Check the FetchContent output above. The library target may have been renamed.")
endif()

message(STATUS "OLAS: using Moonshine target '${MOONSHINE_TARGET}'")

# ---------------------------------------------------------------------------
# ONNX Runtime
# ---------------------------------------------------------------------------
if(ONNXRUNTIME_MODE STREQUAL "bundled")
    message(STATUS "OLAS: ONNX Runtime provided by Moonshine (bundled)")

elseif(ONNXRUNTIME_MODE STREQUAL "prebuilt")
    if(NOT EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
        message(FATAL_ERROR
            "ONNXRUNTIME_MODE=prebuilt but ${ONNXRUNTIME_ROOT}/include/"
            "onnxruntime_cxx_api.h does not exist.")
    endif()
    message(STATUS "OLAS: using prebuilt ONNX Runtime at ${ONNXRUNTIME_ROOT}")

elseif(ONNXRUNTIME_MODE STREQUAL "vcpkg")
    find_package(onnxruntime CONFIG REQUIRED)
    message(STATUS "OLAS: using vcpkg ONNX Runtime")

else()
    message(FATAL_ERROR "Unknown ONNXRUNTIME_MODE='${ONNXRUNTIME_MODE}'")
endif()

# ---------------------------------------------------------------------------
# Build provenance + update-check metadata
# ---------------------------------------------------------------------------
find_package(Git QUIET)

if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE OLAS_GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE OLAS_GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

if(NOT OLAS_GIT_SHA)
    set(OLAS_GIT_SHA "unknown")
endif()
if(NOT OLAS_GIT_BRANCH)
    set(OLAS_GIT_BRANCH "unknown")
endif()

string(TIMESTAMP OLAS_BUILD_DATE "%Y-%m-%d %H:%M:%S UTC" UTC)

# GitHub repo to check for updates. "owner/name" form.
# Leave EMPTY to disable the update check entirely.
# Change this to your actual public repo before shipping.
set(OLAS_UPDATE_REPO "Igna-Mendez/Olas" CACHE STRING
    "GitHub repo (owner/name) for update checks; empty to disable")

# ---------------------------------------------------------------------------
# OLAS executable
# ---------------------------------------------------------------------------
add_executable(olas_win WIN32
    src/main_win.cpp
    src/capture.c
    src/win32_ui.cpp
    src/moonshine_engine.cpp
    src/miniaudio_impl.c
    src/update_check.cpp
)

target_include_directories(olas_win PRIVATE
    src
    "${MOONSHINE_INCLUDE_DIR}"

)

if(ONNXRUNTIME_MODE STREQUAL "prebuilt")
    target_include_directories(olas_win PRIVATE "${ONNXRUNTIME_ROOT}/include")
endif()

target_compile_definitions(olas_win PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    OLAS_GIT_SHA="${OLAS_GIT_SHA}"
    OLAS_GIT_BRANCH="${OLAS_GIT_BRANCH}"
    OLAS_BUILD_DATE="${OLAS_BUILD_DATE}"
    OLAS_UPDATE_REPO="${OLAS_UPDATE_REPO}"
)

target_link_libraries(olas_win PRIVATE
    comctl32
    ole32
    oleaut32
    uuid
    user32
    gdi32
    advapi32
    shell32
    winmm
    avrt
    dwmapi
    winhttp
)

# Link Moonshine (pulls in ONNX Runtime in bundled mode).
target_link_libraries(olas_win PRIVATE ${MOONSHINE_TARGET})

if(ONNXRUNTIME_MODE STREQUAL "prebuilt")
    target_link_directories(olas_win PRIVATE "${ONNXRUNTIME_ROOT}/lib")
    target_link_libraries(olas_win PRIVATE onnxruntime)
endif()

if(ONNXRUNTIME_MODE STREQUAL "vcpkg")
    target_link_libraries(olas_win PRIVATE onnxruntime::onnxruntime)
endif()

# --- Entry point / subsystem -------------------------------------------------
# `add_executable(... WIN32 ...)` sets /SUBSYSTEM:WINDOWS, whose CRT entry
# stub is WinMainCRTStartup (expects WinMain).  We use int main() plus
# CommandLineToArgvW, so retarget the CRT at mainCRTStartup on MSVC.
# (MinGW ships a WinMain shim that calls main, so it's already fine there.)
if(MSVC)
    set_target_properties(olas_win PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
    target_compile_options(olas_win PRIVATE
        /utf-8
        /O2
        /arch:AVX2
        /fp:fast
        /GL
    )
    set_target_properties(olas_win PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
    )
endif()

if(MINGW)
    target_link_options(olas_win PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
        -mwindows
        -pthread
    )
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR
        "Debug builds are ~5x slower. Reconfigure with -DCMAKE_BUILD_TYPE=Release")
endif()
# ---------------------------------------------------------------------------
# Post-build: copy runtime DLLs next to olas_win.exe
# ---------------------------------------------------------------------------
if(ONNXRUNTIME_MODE STREQUAL "prebuilt")
    add_custom_command(TARGET olas_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
            "$<TARGET_FILE_DIR:olas_win>"
        COMMENT "Copying onnxruntime.dll next to olas_win.exe"
        VERBATIM
    )
endif()

if(ONNXRUNTIME_MODE STREQUAL "vcpkg" AND DEFINED VCPKG_TARGET_TRIPLET)
    if(VCPKG_TARGET_TRIPLET MATCHES "^x64-windows$" OR VCPKG_TARGET_TRIPLET MATCHES "^x64-windows-static-md$")
        add_custom_command(TARGET olas_win POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/installed/${VCPKG_TARGET_TRIPLET}/bin"
                "$<TARGET_FILE_DIR:olas_win>"
            COMMENT "Copying vcpkg runtime DLLs next to olas_win.exe"
            VERBATIM
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# Convenience target: distributable folder
# ---------------------------------------------------------------------------
add_custom_target(dist
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dist/OLAS-win64"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:olas_win>" "${CMAKE_BINARY_DIR}/dist/OLAS-win64/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/models" "${CMAKE_BINARY_DIR}/dist/OLAS-win64/models"
    DEPENDS olas_win
    COMMENT "Packaging OLAS-win64 distribution folder"
    VERBATIM
)
# Copy the correct ONNX Runtime DLL next to olas_win.exe on Windows
if(WIN32)
    add_custom_command(TARGET olas_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:moonshine>/onnxruntime.dll"
            "$<TARGET_FILE_DIR:olas_win>/onnxruntime.dll"
        COMMENT "Copying onnxruntime.dll next to olas_win.exe"
        VERBATIM
    )
endif()
