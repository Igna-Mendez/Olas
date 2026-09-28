@echo off
setlocal
cd /d "%~dp0"
start "" "%~dp0olas_win.exe" -l en,es -m "%~dp0models\base-en,%~dp0models\base-es"
endlocal