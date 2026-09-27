@echo off
rem Fetch Moonshine C++ model directories used by OLAS.
rem Each model is a folder containing encoder/decoder ONNX + config.
rem Adjust MOONSHINE_BASE if the release URL changes.

setlocal
set "HERE=%~dp0"
set "MODELS=%HERE%..\models"
if not exist "%MODELS%" mkdir "%MODELS%"

set "MOONSHINE_BASE=https://github.com/moonshine-ai/moonshine/releases/latest/download"

echo Downloading base-en ...
powershell -NoProfile -Command ^
  "iwr -UseBasicParsing -OutFile '%MODELS%\base-en.zip' '%MOONSHINE_BASE%/base-en.zip'"
powershell -NoProfile -Command ^
  "Expand-Archive -Force '%MODELS%\base-en.zip' '%MODELS%'"

echo Downloading base-es ...
powershell -NoProfile -Command ^
  "iwr -UseBasicParsing -OutFile '%MODELS%\base-es.zip' '%MOONSHINE_BASE%/base-es.zip'"
powershell -NoProfile -Command ^
  "Expand-Archive -Force '%MODELS%\base-es.zip' '%MODELS%'"

echo.
echo Done. Models are in %MODELS%.
echo Run olas_win.exe with -m "%MODELS%\base-en,%MODELS%\base-es"
endlocal