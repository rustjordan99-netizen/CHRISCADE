@echo off
setlocal
set "PYTHONPATH=%~dp0.pio-tools"
echo On CHRISCADE, open Game Library then ADD GAME.
echo This adds CRYSTALR.GBZ without overwriting any game or save.
"C:\Users\chris\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe" "%~dp0send_game_usb.py" "%~dp0CRYSTALR.GBZ"
pause
