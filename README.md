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

These steps run on Linux (or WSL on Windows), in this order:

1. Set up the tree:

   ```bash
   ./setup.sh
   ```

2. Build the DS version once, as described in pret's `INSTALL.md` in the new `pokeheartgold/`
   directory. The PC build reuses its compile settings.
3. Install a 32-bit host compiler and SDL3: for Linux, gcc with multilib and the i386 SDL3 development
   files; for Windows, `gcc-mingw-w64-i686` and the official SDL3 mingw development release unpacked at
   `~/sdl3-mingw/i686-w64-mingw32`.
4. Build:

   ```bash
   cd pokeheartgold
   python3 pc/tools/pc_build.py         # Linux: build/pc/hg_pc and build/pc/hg_server
   python3 pc/tools/pc_build.py --win   # Windows: build/pc-win/hg_pc.exe, SDL3.dll, hg_server.exe
   ```

5. Run `hg_pc` with your ROM (or start it and pick the ROM when asked). Esc opens the options menu.

## Playing together

One player runs the server (`dist/windows/host_server.bat`, or `dist/linux/hg_server.sh` on a Linux
machine or VPS). Everyone sets the server's address in the options menu (Multiplayer). Everyone must
use the same MultiMon version and the same ROM. See `dist/linux/README.txt` for hosting details and
security notes.

## Credits

The decompilation this builds on is the work of [pret](https://github.com/pret) and its
contributors. Pokémon is © Nintendo, Game Freak and The Pokémon Company; this project is not
affiliated with them.
