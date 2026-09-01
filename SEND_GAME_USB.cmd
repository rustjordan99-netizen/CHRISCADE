@echo off
setlocal
set "PYTHONPATH=%~dp0.pio-tools"
"C:\Users\chris\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe" "%~dp0send_game_usb.py" %*
pause
