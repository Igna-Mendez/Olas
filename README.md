# OLAS for Windows
-Disclaimer- 99% of the code was writing by an AI
Open Local Audio Scribe — This program uses **Moonshine C++** for local speech-to-text
and **WASAPI loopback** (via miniaudio) for capture. Native Win32 UI.

- Two languages side-by-side (one pane each), matching the GTK build.
- Each pane has its own Start/Stop, Detach-to-own-window button.
- Timestamps, per-pane pause accounting, global zoom and stamp toggle.
- Transcript committed line-by-line to `olas-moonshine-notes.txt` next to
  the executable.

## Reused from the Whisper Windows port

These files are taken **unchanged** from the Whisper port:

- `src/capture.c` / `src/capture.h` — WASAPI loopback + 16 kHz mono s16 ring
- `src/miniaudio.h` / `src/miniaudio_impl.c` — vendored miniaudio 0.11

`capture.h` must keep (or gain) `extern "C"` guards since `main_win.cpp`
includes it from C++.

## Prerequisites

- Visual Studio 2022 Build Tools (MSVC) with CMake, Git
- vcpkg (or an equivalent) with the **static** triplet:
  `vcpkg install onnxruntime:x64-windows-static`
- Moonshine C++ sources under `third_party/moonshine-cpp/`
  (submodule, see its `README.md`)

## Build (native Windows)

```bat
git clone --recursive <this repo>
cd olas_win
cmake -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
              -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
```

Produces `build\Release\olas_win.exe` (GUI subsystem, static runtime).

## Build (Linux → Windows, mingw-w64)

The CMake file supports mingw-w64 (`-static -static-libgcc -static-libstdc++`
flags come from your toolchain file). The same `third_party/moonshine-cpp`
sources must be available.

```sh
cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-mingw
```

## Running

```bat
build\Release\olas_win.exe -l en,es -m models\base-en,models\base-es
```

CLI options (mirrors the GTK build):

```
-m, --model PATH[,PATH]      Model directory per language
-a, --arch N[,N]             0..5 (Base default)
-l, --language CODE[,CODE]   en,es (or extended set if compiled in)
-r, --rms THRESHOLD          Silence RMS gate [100.0]
-q, --chunk-ms MS            Capture chunk (20..1000) [20]
-h, --help
```

## Architecture

```
WASAPI loopback (miniaudio)
        │  16 kHz mono s16, chunk = 20 ms
        ▼
  capture.c ring buffer
        │  capture_read_chunk()
        ▼
  main_win.cpp capture thread
        │  Engine::feed() → per-slot AudioQueue
        ▼
  Worker thread (one per language)
        │  StatefulVad → Moonshine Transcriber
        ▼
  PrintListener → TranscriptSink (Win32Sink)
        ├── PostMessage → Win32 RichEdit control
        └── fwrite → olas-moonshine-notes.txt
```

The engine (`moonshine_engine.cpp`) is a direct port of the classes from
`olas-gtk.cpp` (`AudioQueue`, `StatefulVad`, `PrintListener`,
`StreamingWorker`, `NonStreamingWorker`, `WorkerSlot`) with the GTK
`SplitView` and PulseAudio backends replaced by the Win32 UI and miniaudio
respectively.

## License

MIT (Moonshine and miniaudio are both MIT).
