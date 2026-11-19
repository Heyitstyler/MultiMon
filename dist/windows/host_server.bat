@echo off
rem Hosts a multiplayer server for hg_pc on port 7777. Friends connect to this PC's address (Options, Multiplayer).
rem Add --password yourword to require a password. Windows may ask to allow it through the firewall.
rem Add --chat map to send chat only to players on the same map (default: everyone).
rem Add --admin yourword, then type /login yourword in the game's chat to use /kick, /ban, /unban, /mute, /unmute.
rem Player passwords: as admin, type /passwords on (or off). While on, the first time a name joins, the game asks
rem its player to choose a password, and after that only that password works for the name. /forget NAME deletes
rem a name's password if its owner forgot it. Players change their own with /password in the chat.
rem The setting is remembered in hg_server_accounts.txt.
rem The log goes to hg_server.log and bans to hg_server_bans.txt, next to this file.
"%~dp0hg_server.exe" --port 7777 --log "%~dp0hg_server.log" --bans "%~dp0hg_server_bans.txt" --accounts "%~dp0hg_server_accounts.txt"
pause
