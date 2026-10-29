@echo off
rem The group's lobby (tools\net\ssbu_lobby.py, with the arena): open it on the PC whose address is PLAY_LOBBY in
rem scripts\run.py, before the games, and leave it open while you play. The other players reach it over the Radmin network.
rem The first time, Windows asks whether Python may use the network: allow it (private networks).
rem Arena: the winner stays; after 3 wins in a row the winner goes to the back of the queue too (--arena-streak, 0 = no limit).
cd /d "%~dp0"
set "PY="
if exist deps\python\python.exe set "PY=%~dp0deps\python\python.exe"
if not defined PY if exist build\python_path.txt set /p PY=<build\python_path.txt
if defined PY if not exist "%PY%" set "PY="
if not defined PY set "PY=python"
"%PY%" tools\net\ssbu_lobby.py --bind 0.0.0.0 --port 39500 --arena-streak 3
pause
