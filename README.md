# OLAS — Open Local Audio Scribe

Real-time local speech-to-text for Windows, powered by Moonshine C++ and WASAPI loopback capture. No cloud, no telemetry, no network at runtime. Native Win32 UI, dark theme by default.

- **Two languages side by side** — English and Spanish, each with independent Start/Stop and Detach-to-window controls
- **Small Streaming models** — MIT-licensed, high accuracy, low latency
- **Focus mode** — collapse the toolbar and pane headers so the transcript fills the window
- **Auto-scroll** with proper user-scroll detection
- **Options popup** — capture device, zoom, timestamp toggle, dark/light theme, auto-scroll reset
- **Live transcript file** — line-by-line to `olas-moonshine-notes.txt` next to the exe
- **Diagnostic log** — run with `-v` for per-line latency in `olas-debug.log`

---

## Download and run

1. Open the [Releases page](https://github.com/Igna-Mendez/Olas/releases).
2. Download the latest `OLAS-win64-x.y.z.zip`.
3. Extract it anywhere.
4. Double-click **`OLAS.bat`**.

First launch takes a few seconds while the models load. Play any audio on the machine and captions start within a second or two of speech.

**Requirements**

- Windows 10 21H2 or newer (Windows 11 recommended)
- x64 CPU with AVX2 — Intel Haswell (2013) or AMD Excavator (2015) and newer
- 4+ logical CPU threads recommended
- ~500 MB free disk
- No GPU required

**What's in the zip**

```
OLAS-win64/
    OLAS.bat              ← double-click this
    olas_win.exe
    onnxruntime.dll
    models/
        small-streaming-en/
        small-streaming-es/
    README.md
    LICENSE
```

Keep the folder structure intact — the launcher and the exe expect the models in `models\` next to them.

---

## Using the app

**Pane header** (one per language):

- **Stop / Start** — pauses or resumes that language. While stopped, the pane uses no CPU.
- **▾ / ▸** — collapse the pane to a narrow strip.
- **Decouple / Recouple** — detach the pane into its own OS window. The remaining pane expands to fill. The float window can be minimized and restored from the taskbar.
- **Follow** — re-engage auto-scroll after scrolling up to read history.

**Toolbar**

- **Options ▾** — capture device, zoom, timestamps, auto-scroll reset, appearance.
- **Restore** — rebuilds a pane whose window got into a bad state. No-op if healthy.
- **Focus** — hides the toolbar and pane headers so the transcript fills the window. The button itself stays visible to exit.

The window can be resized freely; there's no minimum size.

---

## Command-line options

| Flag | Description | Default |
|---|---|---|
| `-l, --language CODE[,CODE]` | Language codes | `en,es` |
| `-m, --model PATH[,PATH]` | Model directory per language | `models\small-streaming-{lang}` |
| `-a, --arch N[,N]` | 0=Tiny, 1=Base, 2=TinyStreaming, 4=SmallStreaming, 5=MediumStreaming | `4` |
| `-q, --chunk-ms MS` | Capture chunk (20..1000) | `50` |
| `-v, --verbose` | Write `olas-debug.log` | off |
| `-h, --help` | Show help | |

`3=BaseStreaming` is rejected — declared in Moonshine's C API for forward compatibility but not currently supported.

**Examples**

```bat
olas_win.exe -l en,es               :: default (both languages, Small Streaming)
olas_win.exe -l en                  :: single language, lower CPU
olas_win.exe -l en,es -v            :: verbose diagnostics
olas_win.exe -l en,es -a 1,1 ^
  -m "models\base-en,models\base-es"  :: older Base models if you have them
```

---

## Models

The release zip includes Small Streaming models for English and Spanish under `models\small-streaming-{en,es}\` — nothing to do for end users.

For source builds, fetch them with:

```powershell
powershell -ExecutionPolicy Bypass -File tools\fetch-streaming-models.ps1
```

The script probes known versions and downloads 8 files per language (~330 MB total). If it 404s, Moonshine has published a newer dated folder; the script's `$KnownVersions` list is where you add it.

---

## Building from source

**Prerequisites**

- Visual Studio 2022 Build Tools with "Desktop development with C++"
- CMake ≥ 3.20
- Git
- PowerShell 5.1+
- Ninja (optional, faster — `winget install Ninja-build.Ninja`)

No vcpkg. ONNX Runtime is built as part of Moonshine's CMake project, and the Moonshine sources are pulled via `FetchContent` on first configure.

**Build**

```bat
git clone https://github.com/Igna-Mendez/Olas.git
cd Olas
powershell -ExecutionPolicy Bypass -File tools\fetch-streaming-models.ps1

mkdir build
cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DONNXRUNTIME_MODE=bundled ..
cmake --build . --config Release -j
```

Produces `build\olas_win.exe`. First configure takes 5–15 minutes (ONNX Runtime is built from source); subsequent builds are seconds.

**Build a release zip**

```bat
cmake --build . --config Release --target zip -j
```

Creates `build\dist\OLAS-win64-<version>.zip` with everything an end user needs.

**Cross-compile (Linux → Windows)**

```sh
cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-mingw
```

Debug builds are rejected at configure time — 5× slower and never useful for testing transcription quality.

---

## Architecture

```
WASAPI loopback (miniaudio)
        │  16 kHz mono s16, 50 ms chunks
        ▼
capture.c ring buffer
        │  capture_read_chunk()
        ▼
main_win.cpp capture thread
        │  Engine::feed() → per-slot AudioQueue
        ▼
Worker thread (one per language)
        │  Moonshine Transcriber (streaming)
        ▼
PrintListener → TranscriptSink (Win32Sink)
        ├── PostMessage → Win32 RichEdit
        └── fwrite → olas-moonshine-notes.txt
```

Each language runs on its own worker thread with its own `Transcriber`. The engine handles the shared capture feed, per-slot pause accounting, and graceful shutdown.

**Source layout**

```
src/
    main_win.cpp         entry point, CLI, capture thread
    moonshine_engine.*   Engine, worker slots, Streaming/NonStreaming workers
    win32_ui.*           panes, toolbar overlay, buttons, popup menu
    capture.c / .h       WASAPI loopback + ring buffer
    miniaudio.h          vendored miniaudio 0.11
    miniaudio_impl.c
    update_check.*       GitHub release check
    transcript_sink.h    engine ↔ UI abstraction
tools/
    fetch-streaming-models.ps1
```

---

## Performance notes

The decisions that keep OLAS light enough to run alongside a browser and video playback:

- **One core per language.** `MOONSHINE_ORT_SINGLE_THREAD=1` is set before any `Transcriber` is built. Two languages = two cores max. Without it, ORT spawns thread pools that oversubscribe a 4-core machine and starve audio and video.
- **Normal thread priority.** Workers run at default priority. Reducing the *amount* of work is what lowers CPU; deprioritizing just defers it.
- **Bounded audio queue.** 6 s (120 × 50 ms). If the model falls behind, oldest chunks drop with a stderr warning instead of growing unbounded.
- **Bounded transcript buffer.** 4000 lines per pane, trimmed to 2000 when exceeded. Keeps long sessions responsive.
- **`transcription_interval` = 0.5 s.** Moonshine's default. Lower values double inference cost for imperceptible latency gains.
- **AVX2 + LTO.** Release builds use `/arch:AVX2 /GL /fp:fast` with INTERPROCEDURAL_OPTIMIZATION_RELEASE.

On a 4-vCPU VM running alongside HD video, both languages transcribe in real time with no audio glitches or video stalls.

---

## Reporting bugs

Open an issue at <https://github.com/Igna-Mendez/Olas/issues> with:

- Windows version and CPU model
- `olas_win.exe -l en,es -v` output (`olas-debug.log` has the latency numbers that make failure modes obvious)
- Which models you're running
- What you expected vs. what happened

---

## License

MIT. Moonshine and miniaudio are both MIT; see `LICENSE` for the full text.