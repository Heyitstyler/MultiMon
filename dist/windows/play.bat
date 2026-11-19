@echo off
rem Play the native Windows build. Esc in the game opens the options menu.
rem Settings, keys, the ROM and the save to load are in hg_pc.cfg next to the exe. With no ROM there,
rem the game asks for one. Running hg_pc.exe directly works the same; this only keeps a log in play.log.
rem Keep saves on the Windows disk: a save on the WSL share stops being writable once the WSL distro idles out.
"%~dp0hg_pc.exe" 2> "%~dp0play.log"
