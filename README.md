# MultiMon

A native PC port (Linux and Windows) of Pokémon HeartGold, built on the
[pret/pokeheartgold](https://github.com/pret/pokeheartgold) decompilation, with online multiplayer:
players see each other walk around the overworld, trade and battle anywhere in the world, and chat.

This repository holds only MultiMon's own work. It contains no game code, graphics, sound or other
data from the game. You need your own copy of the game (a ROM you dumped) to play.

## What it adds

- **Native port** (`pc/`): runs the decompiled game as a normal program, with an SDL3 window, sound,
  an options menu (volume, game speed and fast forward, key bindings, ROM and save selection) and
  scripted headless runs for testing.
- **Multiplayer** (`pc/src/pc_net.c`, `pc/server/hg_server.c`):
  - **Presence:** a small server that needs no game files; everyone on it sees the other players walk
    around, with their following Pokémon. NPCs move the same way for everyone on a map.
  - **Trades and battles anywhere:** face another player and press A for a trade, a battle or a double
    battle, with the game's own trade screen, battle party picker and link battle.
  - **Chat:** press T.
  - **For hosts:** server passwords, per-player passwords, and host commands (`/kick`, `/ban`, `/mute`,
    `/passwords`, ...). Flood and chat limits, timeouts and a log.

The design notes and test results are in [docs/MULTIPLAYER_PLAN.md](docs/MULTIPLAYER_PLAN.md);
the port itself is described in [docs/PORT_PLAN.md](docs/PORT_PLAN.md) and
[docs/NETWORK.md](docs/NETWORK.md).

## Layout

| Path | What |
|---|---|
| `pc/` | The PC port: host replacements for the DS hardware and SDK, the build script, the server |
| `patches/pokeheartgold.patch` | MultiMon's changes to pret's sources (PC hooks, behind `SDK_PC`) |
| `patches/BASE_COMMIT` | The pret commit the patch applies to |
| `setup.sh` | Clones pret at that commit, applies the patch and adds `pc/` |
| `dist/windows/`, `dist/linux/` | Helper scripts: play, host a server, run the server as a service |
| `docs/` | Design notes and test results |

## Building

Both the Linux and the Windows versions are built on Linux. You need Ubuntu 25.04 or newer, or Debian 13
(trixie) or newer: older releases don't have the SDL3 packages. On Windows, use either of them in WSL2.
Do these steps in order.

### 1. Windows only: install Linux in WSL2

1. Open PowerShell as administrator (right-click it in the Start menu, then **Run as administrator**).
2. Install Ubuntu, or Debian by using `-d Debian` instead. Restart Windows if it asks you to:

   ```powershell
   wsl --install -d Ubuntu
   ```

3. Open **Ubuntu** (or **Debian**) from the Start menu. The first time, it asks for a new user name and
   password. The password doesn't show while you type it.
4. Work in the Linux home folder, not under `/mnt/c`, which is much slower:

   ```bash
   cd ~
   ```

### 2. Install the build tools

Ubuntu and Debian use the same packages and commands.

1. Debian only: if `sudo` isn't set up (a Debian installed with a root password), become root first
   with `su -`, and leave out `sudo` in the next two commands.
2. Allow 32-bit packages:

   ```bash
   sudo dpkg --add-architecture i386 && sudo apt update
   ```

3. Install the tools:

   ```bash
   sudo apt install bison flex g++ gcc-arm-none-eabi git make ninja-build pkg-config wget python3 xz-utils nasm libc6:i386 libpng-dev gcc-multilib libsdl3-dev:i386 gcc-mingw-w64-i686
   ```

   The DS build needs the first group of these. `gcc-multilib` and `libsdl3-dev:i386` are for the
   Linux version, and `gcc-mingw-w64-i686` is for the Windows version.

   On Debian 12 (bookworm) and older, `libsdl3-dev:i386` doesn't exist. Leave it out and you can still
   build the Windows version, and the server for both, but the Linux version then runs without a
   window or sound.
4. If you did step 1 as root, go back to your own user with `exit`.

For the Windows version, also get SDL3 for mingw:

```bash
mkdir -p ~/sdl3-mingw && wget -qO- https://github.com/libsdl-org/SDL/releases/download/release-3.4.2/SDL3-devel-3.4.2-mingw.tar.gz | tar -xz --strip-components=1 -C ~/sdl3-mingw
```

### 3. Get the code

```bash
git clone https://github.com/Heyitstyler/MultiMon
```

```bash
cd MultiMon && ./setup.sh ~/pokeheartgold
```

`setup.sh` downloads pret's decompilation into `~/pokeheartgold`, applies MultiMon's changes to it
and adds the PC port.

### 4. Build the DS version once

```bash
cd ~/pokeheartgold && make
```

The first run downloads the libraries the build uses and takes a while. It ends with
`build/pokeheartgold.us.nds`. The PC build reuses the compile settings this step creates; you don't
need that file for anything else.

### 5. Build the PC version

Linux version (`build/pc/hg_pc` and the server `build/pc/hg_server`):

```bash
python3 pc/tools/pc_build.py
```

Windows version (`build/pc-win/hg_pc.exe`, `SDL3.dll` and the server `hg_server.exe`):

```bash
python3 pc/tools/pc_build.py --win
```

For Windows, copy the three files from `build/pc-win/` to a folder on the Windows side. The helper
scripts from `dist/windows/` can go in the same folder.

### 6. Play

You need a US HeartGold ROM that you dumped from your own cartridge.
- **Linux:** run `./build/pc/hg_pc /path/to/heartgold.nds`.
- **Windows:** run `hg_pc.exe`, or `play.bat` to keep a log. It asks for the ROM the first time and
  remembers it.

Esc opens the options menu (volume, speed, keys, multiplayer).

## Playing together

One player runs the server (`dist/windows/host_server.bat`, or `dist/linux/hg_server.sh` on a Linux
machine or VPS). Everyone sets the server's address in the options menu (Multiplayer). Everyone must
use the same MultiMon version and the same ROM. See `dist/linux/README.txt` for hosting details and
security notes.

## Credits

The decompilation this builds on is the work of [pret](https://github.com/pret) and its
contributors. Pokémon is © Nintendo, Game Freak and The Pokémon Company; this project is not
affiliated with them.
