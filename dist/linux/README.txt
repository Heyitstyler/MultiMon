hg_server for Linux
===================

The multiplayer server for hg_pc. One self-contained file for any 64-bit x86 Linux, such as a home PC
or a VPS. ARM machines (Raspberry Pi, ARM cloud servers) are not supported yet. It needs no game files,
no ROM and nothing else installed.

Quick start
-----------
1. Copy this folder to the Linux machine, for example to /opt/hg_server.
2. Make the files runnable:
       chmod +x hg_server hg_server.sh
3. Start it:
       ./hg_server.sh
   With a server password and host commands:
       ./hg_server.sh --password yourword --admin yourword
4. Open TCP port 7777 in the firewall, for example on Ubuntu:
       sudo ufw allow 7777/tcp
   On a cloud VPS, also allow TCP 7777 in the provider's firewall or security group.
5. Players set Server to this machine's address in the game (Esc, Multiplayer, Server), for example
   203.0.113.5 or 203.0.113.5:7777.

The log (hg_server.log), bans (hg_server_bans.txt) and player passwords (hg_server_accounts.txt) are
written next to hg_server.sh.

Keeping it running
------------------
To start it at boot and restart it if it stops, use the systemd service in hg_server.service. The
comments at its top say how to install it. Without systemd, run it inside tmux or screen.

Host commands
-------------
Start with --admin word. In the game's chat, type /login word, then:
  /who, /kick NAME, /ban NAME, /unban IP, /mute NAME, /unmute NAME,
  /passwords on|off (player passwords), /forget NAME (deletes a forgotten player password).

Security
--------
Anyone on the internet can reach an open port. Set --password to keep strangers out, and --admin to
be able to remove people. Passwords travel unencrypted, so don't reuse an important one.

Everyone must use the same hg_pc version as this server, and the same ROM.
