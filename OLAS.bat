@echo off
setlocal
cd /d "%~dp0"
start "" "%~dp0olas_win.exe" -l en,es -v
endlocal
