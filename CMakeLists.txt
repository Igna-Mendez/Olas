cmake_minimum_required(VERSION 3.16)
project(olas_win C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static runtime so the exe has no CRT DLL deps.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8 /W3)
else()
  add_compile_options(-O2 -Wall)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT MSVC)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Moonshine C++ — vendored under third_party/moonshine-cpp.
# Expected: an imported CMake target `moonshine` and `onnxruntime` (both
# built/installed by vcpkg static triplet, or provided by an add_subdirectory).
# ---------------------------------------------------------------------------
find_package(onnxruntime CONFIG REQUIRED)

if(NOT TARGET moonshine)
  add_subdirectory(third_party/moonshine-cpp EXCLUDE_FROM_ALL)
endif()

# ---------------------------------------------------------------------------
# App
# ---------------------------------------------------------------------------
add_executable(olas_win WIN32
  src/main_win.cpp
  src/moonshine_engine.cpp
  src/win32_ui.cpp
  src/capture.c
  src/miniaudio_impl.c
)

target_include_directories(olas_win PRIVATE
  src
  third_party/moonshine-cpp/include
)

target_link_libraries(olas_win PRIVATE moonshine onnxruntime)

# WASAPI COM (capture), common controls (status bar), shell (folder picker).
if(MSVC)
  target_link_libraries(olas_win PRIVATE ole32 oleaut32 uuid comctl32 shell32)
else()
  target_link_libraries(olas_win PRIVATE stdc++ ole32 oleaut32 uuid comctl32 shell32)
endif()