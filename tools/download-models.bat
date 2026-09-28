@echo off
rem Fetch Moonshine C++ model files used by OLAS.
rem Models are individual .ort/.bin files on download.moonshine.ai, not archives.
rem Layout produced:
rem   models\base-en\encoder_model.ort
rem   models\base-en\decoder_model_merged.ort
rem   models\base-en\tokenizer.bin
rem   models\base-es\... (same)

setlocal EnableDelayedExpansion
set "HERE=%~dp0"
set "MODELS=%HERE%..\models"
if not exist "%MODELS%" mkdir "%MODELS%"

set "CDN=https://download.moonshine.ai"

rem ---- languages to fetch (space-separated) ----
set "LANGS=en es"

for %%L in (%LANGS%) do (
    set "LANG=%%L"
    set "DEST=%MODELS%\base-%%L"
    set "URL=%CDN%/model/base-%%L/quantized/base-%%L"
    if not exist "!DEST!" mkdir "!DEST!"

    echo.
    echo === base-%%L ===
    for %%F in (encoder_model.ort decoder_model_merged.ort tokenizer.bin) do (
        if exist "!DEST!\%%F" (
            echo   [skip] %%F already present
        ) else (
            echo   [get ] %%F
            powershell -NoProfile -Command ^
              "$ProgressPreference='SilentlyContinue';" ^
              "Invoke-WebRequest -UseBasicParsing -Uri '!URL!/%%F' -OutFile '!DEST!\%%F'"
            if errorlevel 1 (
                echo   [FAIL] %%F  ^(see message above^)
            )
        )
    )
)

echo.
echo Done. Models are in %MODELS%.
echo Run:  olas_win.exe -l en,es -m "%MODELS%\base-en,%MODELS%\base-es"
endlocal
