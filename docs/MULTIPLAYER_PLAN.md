# Multiplayer plan

**Goal:**
- Several people play in the same world at once and see each other walking around.
- Any two of them can trade or battle.
- A dedicated server hosts a world for a group.

Code paths are in the WSL repo (`~/pokeheartgold`). Background on the game's own networking is in `NETWORK.md`.

## What "same world" can mean here

- **Separate games:** each player runs their own copy of the game with their own save. Story progress, items, badges and NPC state stay separate, because the game logic is single-player and can't share them.
- **What the server shares:** presence (who is on which map and where they stand), chat, and the link sessions for trades and battles.
- **What players see:** others walking around their maps, like an MMO lobby on top of normal single-player saves.
- **Not on the table:** a truly shared world state (one set of NPCs and items for everyone). That would mean rewriting the game's logic.

## Architecture

```
 hg_pc (player A) ──┐                         ┌── hg_pc (player C)
   presence, chat,  │   TCP, one connection   │
   link tunnel      ├──────► hg_server ◄──────┤
 hg_pc (player B) ──┘     (no ROM, no game)   └── ...
```

- **Server** (`hg_server`): a small standalone program. It doesn't run the game and doesn't need a ROM. It does:
  - **Logins:** name, trainer ID, and the build ID, which must match.
  - **Presence:** relays positions to everyone on the same map.
  - **Chat:** relays text messages.
  - **Link sessions:** pairs players for a trade or battle and forwards their link traffic.
  - **Config:** port, player limit, optional password.
- **Clients connect outbound** to the server, so no player needs port forwarding. Only the host opens one port.
- **Transport:** one TCP connection per client, with length-prefixed binary messages. Positions go out about 10 times a second, or on each step, which is plenty for a handful of players. Switch to UDP for positions only if TCP lag ever shows.
- **Builds must match:** link battles run the battle engine in lockstep on both machines, so every client needs the exact same build. The login handshake carries a build hash and refuses mismatches. The Linux and Windows builds are already bit-identical, so they can play together. Real DS consoles and emulators can't join.

## Pieces

### 1. Presence: seeing each other

- **Client sends** the current map (`LocalFieldData_GetCurrentPosition`), tile (`PlayerAvatar_GetXCoord`/`GetZCoord`), facing (`PlayerAvatar_GetFacingDirection`), movement state (`PlayerAvatar_GetState`: walk, run, bike, surf), gender (`PlayerAvatar_GetGender`) and name.
- **Client draws** each remote player on the same map as a map object (`MapObject_Create(manager, x, z, dir, sprite, movement, mapNo)` in `src/map_object.c`, decompiled C).
  - **Sprite:** use the Ethan/Lyra player sprite. Its graphics are always loaded, so no extra resource loading is needed.
  - **Movement:** walk them between tiles with the game's own movement commands (`MapObject_SetMovementCommand`), so they animate like NPCs instead of teleporting.
  - **Cleanup:** remove them with `MapObject_Delete` when they leave the map or disconnect.
- **When to sync:** only while the player is free to move in the overworld, with no script, menu or battle running. Remote objects are removed before any of those start and re-created after. That keeps them out of cutscenes and scripted events.
- **Limits:** each map has a fixed number of map object slots, so cap remote players per map (around 8). Remote players don't block movement at first; collision can be a later option.

### 2. Trades and battles: using the game's own link code

The game already does trades, battles (including 4-player multi battles) and the Union Room over local wireless. Battles stay in sync because the game's link layer exchanges each player's choices every turn. Rather than rewrite that, the port makes the local wireless work over the internet.

- **Stand in for the radio:**
  - **On a DS:** the radio side of the wireless driver (WM) runs on the second CPU (ARM7).
  - **On the PC today:** nothing answers it, and only the wireless power-up is faked (`pc/src/pc_pxi.c`).
  - **The new stand-in:** answers WM requests the way the radio would, in the order and with the payloads of the SDK's ARM7 code (`libraries/wm/src/ARM7/req_*.c`). The game uses about 30 WM functions (listed in `NETWORK.md`).
- **Server as the airwaves:**
  - **Finding players:** when the game scans for wireless players (Wireless Club, Union Room), the stand-in reports the server's other players who are in the same kind of room.
  - **Connecting:** a WM connect becomes a server pairing request.
  - **Exchanging data:** each multi-player exchange (the per-frame packets of a link session) is forwarded through the server to the peer.
- **Result:** trades, single and double battles, multi battles and the Union Room run the game's unchanged code: screens, rules, the trade animation, evolution on trade and saving.

**Biggest risk: timing.**
- **On a DS:** wireless partners swap a packet every frame with almost no delay.
- **Over the internet:** 30–150 ms between partners.
- **The lever:** the stand-in controls when the game sees data, so it can make the game wait for the partner's packet. That costs a lower frame rate during link screens on high latency, but keeps both sides in step.
- **Unknown:** how much delay and missing data the game's comm layer tolerates before it shows a communication error (`asm/unk_02032844.s`, `asm/unk_02034B0C.s`, `src/unk_02037C94.c`). It now has real timers (`TimersFrame`), so its timeouts are live. The first spike below answers this.

### 3. Challenging someone in the overworld

- **Phase 2:** trades and battles start the normal way, in a Pokémon Center's Wireless Club or Union Room.
- **Later:** talking to a remote player anywhere shows Trade / Battle / Cancel, using the game's own message box and yes/no menu. If the other player accepts, both games enter the same link flow directly, as the Union Room does after you talk to someone. This depends on phase 2 working.

### 4. Chat

- **Typing:** a key (T by default, rebindable) opens a one-line text box, drawn the same way as the options menu (SDL debug font).
- **Showing:** messages appear at the top of the screen for a few seconds, with a short history.
- **Server:** relays chat to everyone, or to the same map only, as a server setting.

### 5. Menu and settings

- **New menu page "Multiplayer…":** server address, player name, connect/disconnect, and a list of who is online.
- **Settings:** stored in `hg_pc.cfg` (`server=`, `name=`). The game stays fully playable offline.
- **Starting the server:** run `hg_server --port 7777 [--max 16] [--password x]`. It needs no game files, so anyone can host it on a PC or a small VPS.

## Phases

| Phase | Result | Main work |
|---|---|---|
| **S1. Spike: wireless between two local copies** | Two hg_pc windows on one PC trade through the Wireless Club | WM stand-in and a local loopback link (no server yet). Measure how much latency the comm layer tolerates by adding artificial delay. This decides how link play over the internet must work. |
| **S2. Spike: a fake remote player** | A scripted fake player walks around next to you in New Bark, survives map changes, menus and cutscenes | Overworld hook, map-object lifecycle, sync-pause rules |
| **1. Server + presence** | Friends on a server see each other walking on the same maps | `hg_server`, client network thread, protocol, Multiplayer menu page |
| **2. Link over the server** | Trades and battles through the Wireless Club/Union Room between people online | S1's stand-in connected to the server: scanning, pairing, tunnelling |
| **3. Overworld challenge** | Talk to a player anywhere to trade or battle | Interaction hook, request/accept flow, entering the link flow |
| **4. Chat** | Text chat | Chat overlay and server relay |
| **5. Hardening** | Usable with strangers on the internet | Reconnect, timeouts, kick/ban, rate limits, a server log |

S1 and S2 can run in parallel; they touch different parts of the code.

### S1 result (2026-11-17): done, trades work

Two copies of hg_pc on one PC traded Pokémon through the **Union Room** (Cherrygrove Pokémon Center 1F). The Wireless Colosseum desk is next to it.
- **Flow tested:** B's Pidgey went to A and A's Sentret to B. Along the way: beacons and figures in the room, talking to the other figure, connecting, greeting, the Trainer Card exchange, the trade offer and accept, both parties shown, the trade animation, and the save afterwards. There were 0 asserts.
- **How:**
  - **Radio stand-in:** `pc/src/pc_wm.c` plays the DS radio chip's side of the wireless driver and answers the game's WM requests per the SDK's own ARM7 code. The airwaves are UDP between copies (`HG_WM_PORT`, `HG_WM_PEERS`).
  - **Unchanged game code:** everything above WM (the game's comm layer, the Union Room, the trade code) is the original.
- **Latency:** with `HG_WM_DELAY=9`, every packet is held 9 frames, about 150 ms one way and 300 ms round trip. The same trade still went through, with 0 asserts. The link ran at about half its normal packet rate, and the "Communicating…" screens took a few seconds longer. So the game's own link code copes with internet-like delay; no extra lockstep is needed for trades. Battles and higher delays haven't been measured yet.
- **Fixes S1 needed, all in the port:**
  - `PXI_IsCallbackReady` now reports for the ARM9 side whether a receive callback is registered. The SDK only registers its WVR reply handler when told there isn't one, so before this, the wireless power-up never completed.
  - The allowed-channel word in the system work area is set. `WM_GetAllowedChannel` read 0, and the game showed a comm error.
  - Port-send failures match the ARM7: a partner that has left drops out of the send instead of failing it, which used to cause an assert and a comm error.
- **Test tools added:**
  - `HG_WARP` (map plus warp point or tile, through the game's own warp task) and `HG_GIVEMON`, in `pc/src/pc_debug.c` with a small `SDK_PC` getter in `src/field_system.c`.
  - `HG_PACE` (60 Hz headless).
  - `HG_INPUT_FILE` and `HG_DUMP_EVERY`: a running headless copy can be steered by appending input while looking at its latest screen.
- **What phase 2 needs:**
  - Replace the UDP peer list with the server: beacons become "who's in the Union Room", and data goes through the server.
  - Two copies that enter at the same instant both become hosts and never meet; real players walk in at different times. The server can avoid this by telling one side to scan.
  - The Union Room's figures wander, so talking to one takes a moment, as on a DS.

### S2 result (2026-11-17): done, a fake remote player walks the overworld

`HG_GHOST=N` adds a stand-in remote player that replays your own position N frames late (default 48, about one second). Phase 1 feeds the same driver from the server instead. Code: `pc/src/pc_ghost.c`, called at each frame end from `pc_main.c`.
- **Drawing:** a map object (`MapObject_Create`) with the player sprite, id `PC_GHOST_ID` (240). It walks with the game's held-movement commands (`MapObject_SetHeldMovement`: step, run when behind, face), so it animates like an NPC.
- **Lifecycle rules, all tested live:**
  - It is only touched while the player could move (`FieldSystem_IsPlayerMovementAllowed`). During scripts, menus, battles and map loads it stays frozen.
  - **Doors:** the stream on another map removes it; it is created again once the stream arrives. Tested in and out of the player's house and Elm's lab.
  - **Outdoor borders:** maps on the main matrix share coordinates, so it is kept across seamless borders. `MAPOBJECTFLAG_KEEP` stops the game removing it at the border. Tested New Bark ↔ Route 29.
  - **Distance:** more than 6 tiles off, it teleports (removed, then created at the target).
  - **Occupied tiles:** it never steps onto or appears on a tile another object stands on (player, following Pokémon, NPCs); it waits instead. Without this rule it covered the following Pokémon.
  - **Talking:** does nothing. `MAPOBJECTFLAG_UNK19` makes it untalkable (`sub_0203DC64`). Without the flag, A ran script 0, which printed the following-Pokémon line.
  - **Saving:** it never enters the save. `FieldSystem_SyncMapObjectsToSaveEx` skips id 240 (`src/map_object.c`, `SDK_PC`). Tested: a save made with the ghost on screen reloads without it.
  - **Battles and menus:** the start menu, the Options app, saving, the Elm's lab police cutscene and a wild battle all left it in place, with no re-creation needed.
- **Soak:** random walking for 72,000 frames from the user's save, 3 seeds × Linux and Windows, with the ghost on: 0 asserts, no crashes. One Windows copy failed to start the first time, while six copies launched at once: `cannot reserve DS address range 0x01000000-0x08000000 (error 487)`, meaning something else already held part of that range. Rerun alone, it passed. A startup retry may be worth adding if users ever see this. Normal play is unchanged: regression is bit-identical on old Linux, new Linux and Windows.
- **What phase 1 needs:**
  - Feed `Drive()` from the server's position stream instead of the local trail. Keep one object per remote player, with ids from 240 up; the save filter must skip that range.
  - Use the remote player's gender for the sprite.
  - Show surfing and biking states.
  - Blocking: remote players currently block the local player like an NPC. That is fine for now; collision could become optional later.
  - A remote player standing on the local player's tile waits next to it, as above.

### Phase 1 result (2026-11-17): server and presence work

Players on one server see each other walk around the same maps. A Linux copy and a Windows copy were tested on one server.

**How to play together:**
1. The host runs `win\host_server.bat`, which runs `hg_server.exe --port 7777`. Add `--password word` or `--max N` (up to 16) as needed. The host must let port 7777 through the firewall (Windows asks the first time). For friends outside the home network, the router must also forward TCP 7777 to the host.
2. Each player opens Options (Esc), then Multiplayer...:
   - **Server:** the host's address, as `host` or `host:port`.
   - **Name:** up to 16 characters.
   - **Password:** if the host set one.
   - **Connect.**
3. The game remembers these settings (`server=`, `name=`, `password=`, `online=` in `hg_pc.cfg`) and reconnects at the next start.

**Pieces:**
- **`pc/server/hg_server.c` (server):**
  - A standalone program: no ROM, no game code. It is built next to the game by `pc_build.py` (`build/pc/hg_server`, `build/pc-win/hg_server.exe`).
  - One `select()` loop. It checks logins: protocol version, password, player limit, and the same game data. The first player online sets the game data; it is the ROM header CRC plus version byte, so a romhack and the clean ROM can't share a server.
  - It relays positions to everyone and logs joins, leaves and refusals with timestamps.
- **`pc/src/pc_net_proto.h` (protocol):** TCP, length-prefixed packed structs. Messages: HELLO, WELCOME/REJECT, JOIN/LEAVE, POS (map, tile, facing, gender, state).
- **`pc/src/pc_net.c` (client):**
  - Non-blocking. It is polled each frame from `pc_frame_end`, and while the options menu is open, so the game never waits on the network.
  - It retries a lost connection every 5 seconds. A refusal is shown and not retried.
  - Test overrides: `HG_SERVER`, `HG_NAME`, `HG_PASSWORD`, `HG_NET_LOG`.
- **`pc/src/pc_ghost.c` (drawing):**
  - Draws every remote player with S2's rules. Each player gets map object id 224+slot, and the save filter skips ids 224–239.
  - It sends the local position whenever it changes (map, tile, facing).
  - The S2 fake (`HG_GHOST`) is now slot 15.
- **Menu:** Options has a new Multiplayer… page that shows the status and who is online, with their map.

**Tested:**
- **Two copies on one server** (Linux "Linus", Windows "Winnie", both in New Bark): each saw the other. Linus's walk showed on Winnie's screen.
- **Leaving:** closing the Windows copy removed Winnie from Linus's world (server: "Winnie left").
- **Server restart:** the client reconnected by itself within 5 seconds.
- **Password:** with a password set, a player without it got "Refused: wrong password"; with it, they joined.
- **Menu page:** screenshot while connected: status "Online as Shotty", list "Linus map 60".
- **Normal play:** regression bit-identical on old Linux, new Linux and Windows.
- **Soak:** two networked copies random-walking 30k frames each on one server, plus a 72k-frame run with the fake: 0 asserts, each copy drew the other, joins and leaves logged by the server.

**Not tested:**
- Typing in the menu's text fields (needs a person at the window).
- Play across the internet (only localhost so far).
- More than 2 players at once.

**Limits:**
- **Sprites:** remote players always use the walking sprite. A cycling or surfing player shows walking, though their state is already sent.
- **Collision:** remote players block your way like NPCs.
- **No names over heads.**
- **Interiors:** two players in the "same" house (the same map id) see each other, though each house is in their own game.

### Phase 2 result (2026-11-18): trades and battles over the server

Players on the same server can trade and battle through any Pokémon Center's **Union Room**, exactly as over local wireless. Everyone online is in "wireless range" of everyone else.

**How to use it:**
1. Both players connect to the server (Options → Multiplayer…), as in phase 1.
2. Both go into a Union Room (the Pokémon Center 1F, right-hand desk). Different towns are fine.
3. Walk up to the other player's figure and press A, then choose Trade or Battle as on a DS.

**How it works:**
- **The server carries the radio.** The S1 radio stand-in (`pc/src/pc_wm.c`) now sends its packets to the server (`NM_WM`) instead of UDP. The server delivers each packet to the player whose MAC address it is addressed to, or to everyone for broadcasts (beacons). It learns each player's MAC from the packets they send. UDP between local copies (`HG_WM_PORT`) still works for tests.
- **A MAC address per player.** Connecting gives the game a random MAC address, so players can tell each other apart. The game compares the save's MAC with the console's to detect "this save moved to another DS" and then resets berry pots and time-based events. On PC that check now always passes (`Save_SysInfo_MacAddressIsMine`, `SDK_PC`), so a new MAC never triggers the reset.
- **No double figures.** Phase 1's walking players are not drawn in the Union Room or the link rooms (maps 2–5); the game draws players there itself.
- **Bigger messages:** `NET_MAX_MSG` is 1024, and the protocol version is 2, so phase 1 builds are refused by a phase 2 server with "different hg_pc version".

**Radio fixes found on the way, all in `pc_wm.c`, following the SDK's ARM7 code:**
- **BEACON_SENT and BEACON_RECV indications.** The real radio tells a host game each time it sends a beacon, and a connected guest each time it hears its host. The stand-in sent neither. Without them, the Union Room stayed in host mode forever after the first session, so two players who had just traded could not see each other again. This was also S1's "both become hosts and never meet". With the indications, the game alternates between hosting and scanning as on a DS.
- **End of a session.** When the game queued its last packet and closed the link in the same frame, the stand-in failed that packet, and the game asserted. It now sends it first, as the radio would have during the frame.

**Tested (two Linux copies on one server, same save, scripted input, read from screenshots):**
- **Trade:** found each other in the Union Room, talked, swapped Trainer Cards, traded Pidgey for Sentret, the trade animation, both saves written. About 14,000 radio packets each way.
- **Battle:** a single battle with two Pokémon each, played to the end. Both screens showed the same moves and HP every turn, including a faint and a switch on each side. Then back to the Union Room and the session closed cleanly. About 25,000 packets each way, 0 asserts.
- **After the session:** both copies went back to alternating host/scan and saw each other again.
- **Normal play:** regression bit-identical between the new Linux build, the new Windows build and the phase 1 build. (The regression hash changed today for all builds, the phase 1 exe included. The run started after midnight, and the game reads the real date.)

**Not tested:**
- Linux with Windows in a link session (the builds are bit-identical, and phase 1 tested them together on a server).
- Real internet latency. S1 showed trades survive 300 ms round trip; battles over a slow link are not measured.
- Two different saves. Both test copies used the same save and trainer.
- Double and multi battles, the Wireless Colosseum, Spin trades, records mixing.

**Known messages:**
- At the end of a trade the game asserts once in `sub_0203A1C4` (`src/unk_02037C94.c`): the partner's friend-code data is empty because these saves never registered for Nintendo WFC. That assert is compiled out on a retail DS, and the game carries on the same way.
- The first link battle logs "skipped a 2048-byte copy 00000000 -> 06000000 (stale pointer)" once: a graphics load with no source. Nothing looked wrong on screen. Not explained yet.

**Limits:**
- Everyone online is in one big room: a server with many players fills the Union Room (the game shows a limited number of figures).
- Figures wander as on a DS, so talking to one can take a couple of tries.

### Phase 3 result (2026-11-18): invite a player from anywhere

Walk up to another online player anywhere in the overworld and press A to invite them to the Union Room. If they accept, both games go straight there from where they stand, and the inviter's game starts talking to the other player's figure by itself. You then pick Trade or Battle from the game's own menu.

**How it plays:**
1. Face another player and press A: "Invite NAME to the Union Room? A: Yes B: No".
2. The other player sees "NAME invites you to the Union Room. A: Go B: No". They have 30 seconds to answer. The inviter can cancel with B while waiting.
3. On yes, both enter the Union Room as soon as their game is free (not mid-menu or mid-script). The inviter's game finds the other player's figure and opens the usual "Is there something you wanted to do?" menu with Greet, Draw, Battle, Trade, and so on.
4. Walking out of the room returns each player to the tile they left. A save made inside the room, for example by a trade, also continues on that tile.

**How it works** (`pc/src/pc_challenge.c`):
- **Prompts:** a text box over the top screen, drawn into the frame with SDL's debug font through a software renderer (`pc_video_overlay` in `pc_video.c`). It shows in the window and in headless dumps. While a prompt waits for an answer, it takes the A and B presses, so the game doesn't see them.
- **Invitations:** server message `NM_INVITE` (ask, yes, no, cancel), relayed to the named player. It carries the sender's in-game trainer ID. The protocol version is 3.
- **Entering the room:** the Pokémon Center reception script ends with the game's own entry, `sub_02054030`, after setting the Union Room variable and flag. The challenge does the same from a field task. `sub_02054030` stores the current tile as the way out, and on Continue flag 966 sends the player there. That is why both walking out and reloading return to the overworld spot.
- **Talking automatically:** the game keeps a list of up to 16 heard players. `sub_02035798(slot)` returns that slot's PlayerProfile, and the figure's map object id is slot + 1 (as in `sub_0205A200`). The inviter's game finds the slot whose trainer ID matches the invitation and starts that figure's script with it as the one talked to (`StartMapSceneScript`), exactly what pressing A in front of it does.
- **Test aid:** `HG_WARP=union` enters the Union Room from wherever the save stands.

**Tested (two Linux copies on one server, New Bark):**
- **Invite and accept:** both arrived in the Union Room. The inviter's game opened the talk menu with the other player by itself. A trade was then done all the way through, and both saves were written.
- **Reload:** continuing from the save made by that trade put the player back on their New Bark tile.
- **Walk out:** both players returned to their own tiles.
- **Decline and cancel:** the inviter saw "can't come now"; the invitee saw "took back the invitation". 0 asserts throughout.
- **Normal play:** regression bit-identical to the phase 1 and phase 2 builds.

**Not tested:**
- The prompts in the Windows window (same code as the headless Linux dumps).
- Battles started this way (same Union Room flow as phase 2).
- Inviting from inside a building or cave.
- Three or more players in the room at once with the same trainer ID. The figure lookup picks the first match; real players have different IDs.

**Limits and shortcuts:**
- Entering skips the reception's save prompt, party heal and Griseous Orb check (`CommSanitizeParty`). A trade saves by itself, so nothing is lost there. The heal is left out on purpose: inviting someone would otherwise be a free Pokémon Center anywhere. Whether a link battle then starts with the party's current HP is not checked. A Giratina holding the Griseous Orb could also enter the room in Origin Forme, which the desk would have refused.
- Invitations only work in the overworld, not from inside the Union Room or link rooms.
- If the other figure hasn't appeared after 30 seconds, the inviter gets "NAME isn't in sight. Talk to their figure." The two can still meet by hand.

### Phase 4 result (2026-11-18): text chat

While online, press **T** to type a chat line, **Enter** to send it and **Esc** to close without sending. Chat shows in a dark box at the top of the top screen. New lines stay for 10 seconds; while you type, the last few stay visible. Notices show when players join or leave.

**How it works:**
- **Typing** (`pc/src/pc_chat.c`, keys in `pc_video.c`): the chat key is a bindable action (Options → Controls → Chat, default T). While the line is open, every key goes to it. The game sees no buttons, and Esc closes the chat instead of opening the options menu. Text is printable ASCII, up to 79 characters, since that is all the font has.
- **Drawing:** the same in-frame overlay as the phase 3 prompts, so it shows in the window and in headless dumps, on Linux and Windows. Long lines wrap at spaces.
- **Server:** `NM_CHAT`, relayed to everyone else. With `hg_server --chat map`, it goes only to players on the sender's map. The server replaces control characters and logs every line ("chat NAME: text"). The protocol version is 4.
- **Test aid:** `HG_CHAT="frame:text;frame:text"` sends lines from a headless run. `HG_NET_LOG=1` logs chat to stderr.

**Tested (headless, two Linux copies on one server, plus the Windows build alone):**
- **Both directions:** each copy showed its own and the other's lines, in the same order as the server log.
- **Display:** wrapping at spaces; the box gone after 10 seconds.
- **Notices:** the "joined" notice for a player who arrives later, but not for players already online; the "left" notice when a copy closed.
- **Map mode:** with `--chat map`, one player outdoors and one in a house, neither received the other's line.
- **Windows:** the Windows build drew the chat box (headless dump).
- **Normal play:** regression unchanged.

**Not tested:**
- Typing in the window: opening with T, typing, Backspace, Enter and Esc. That path needs a real keyboard and window; the headless tests send lines through `HG_CHAT`.

**Limits:**
- No profanity filter. Rate limits and mute came in phase 5.
- The chat key does nothing while offline.

### Phase 5 result (2026-11-18): hardening for strangers online

The server now protects itself and its players, and the host can remove people from inside the game.

**For the host:**
- `hg_server --admin word` sets a host password. In the game's chat, type `/login word`, then use:
  - `/who`: list players with their ids and IP addresses.
  - `/kick NAME` or `/kick ID`: remove a player.
  - `/ban NAME`: remove the player and ban their IP address.
  - `/unban IP`: lift a ban.
  - `/mute NAME`, `/unmute NAME`: block or allow a player's chat (by IP address, until the server restarts).
- Anyone can use `/who`, which then shows only ids and names. Lines starting with `/` are never relayed to other players, and the game doesn't echo them on screen, so the password stays private.
- `--bans FILE` holds the banned IP addresses, one per line; it defaults to `hg_server_bans.txt`. `/ban` and `/unban` rewrite the file, so bans survive a restart.
- `--log FILE` appends the log to a file as well as to the console. The log records joins (with IP address), leaves (with the player's peak data rate), refusals, timeouts, kicks, bans, mutes, admin logins and chat.
- `win\host_server.bat` writes `hg_server.log` and `hg_server_bans.txt` next to itself. A comment in it explains `--admin`.

**Protections (`pc/server/hg_server.c`):**
- **Login timeout:** a connection that doesn't log in within 10 seconds is closed, so idle connections can't fill the player slots.
- **Silence timeout:** the server and each game ping each other every 5 seconds (`NM_PING`). After 30 seconds with no message at all, the server drops the player. This catches games that crashed or lost their network without closing the connection.
- **Flood limit:** each player may send 64 KB per second on average, with a 256 KB burst; more than that gets them kicked ("sending too much data"). A full trade peaked at 15 KB per second.
- **Chat limit:** 5 lines at once, then one line every 2 seconds. Extra lines are dropped, and the sender sees "Slow down".
- **Invitation limit:** one invitation every 3 seconds. A faster one is answered "can't come now" on the invited player's behalf.
- **Names:** the server replaces non-printable characters in names; an empty name becomes "Player".
- **Kicked or banned players** get the reason (`NM_REJECT`), and their game doesn't retry.

**In the game (`pc/src/pc_net.c`):**
- **Dead server:** the game gives up on a server it hasn't heard from in 30 seconds (10 while connecting or logging in), then retries every 5 seconds as before. This catches a server that froze or a network that dropped without closing the connection.
- **Notices:** losing the connection, a refusal and a kick show as chat notices ("Connection lost (no reply)", "Refused: kicked by the host"), not only on the Multiplayer page. Server replies (`NM_CHAT` with id 255) show as notices too.
- The protocol version is 5, so every player needs the new hg_pc.

**Tested:**
- **Server, with scripted fake clients (24 checks, all pass):** login timeout; server pings; silence drop; chat burst (5 relayed, 3 "Slow down"); invitation limit; `/kick` before login refused; wrong and right `/login`; commands not relayed; `/who` with IPs; mute and unmute; kick; ban, refusal on return, the ban file; unban; flood kick; the log file.
- **Two real games (Linux):**
  - One game logged in through `HG_CHAT` and kicked the other. The kicked game showed "Refused: kicked by the host" and stayed offline.
  - Then the server was frozen for 45 seconds (SIGSTOP). The remaining game showed "Connection lost (no reply)" and reconnected on its own after the server resumed. 0 asserts.
- **Trade over the server:** still works, 0 asserts.
- **Windows game with the Windows server:** `/login`, `/who`, chat, the log file, and 40 seconds online with pings and no false timeout.
- **Normal play:** regression identical to the phase 4 exe.

**Not done, or not tested:**
- **Battle data rate:** not measured. Battles probably need less data than trades, but the flood limit leaves room either way (64 KB/s against a 15 KB/s trade peak).
- **Tunnels and shared IP addresses:** players reaching the server through a tunnel or relay (such as playit.gg) all share one IP address. Then `/ban` and `/mute` hit all of them together. Players behind the same home router share an address too.
- **Fake connections:** someone can still reconnect over and over to take player slots for 10 seconds at a time. There's no limit on connections per IP address, because tunnels would break it.
- **Duplicate names:** without player passwords (below), names aren't unique, so a stranger can pick the host's name. `/who` shows the ids, which tell players with the same name apart.
- **Admin over the server console:** there are no commands on the console; the host uses the in-game chat.
- **Reconnecting mid-trade:** a connection dropped during a trade or battle ends it with the game's own communication error. Reconnecting doesn't resume it.
- **Freezes in a real window:** a Windows window drag longer than 30 seconds would time the game out. Games read waiting data before checking for silence, so shorter stalls are safe.

### Player passwords (2026-11-18)

A server setting that gives each player name its own password, so nobody else can use that name.

**Turning it on and off:** the host types `/passwords on` or `/passwords off` in the game's chat, after `/login`. `/passwords` alone shows the current setting. It's off at first.
- The setting is saved as the first line of the accounts file (`passwords on` or `passwords off`), so it lasts across restarts.
- Turning it off keeps every account. Turning it back on asks for the same passwords again.
- A change applies to the next logins. Players already online stay online.
- `--accounts FILE` only chooses the file; the default is `hg_server_accounts.txt`. `win\host_server.bat` keeps it next to itself.
- Tested with 9 more scripted checks:
  - Off at first; `/passwords` reports the setting; on; the state line in the file; the host stays online when it's turned on.
  - Off lets any name in without a password.
  - Off survives a restart, with the accounts kept.
  - Turned on again, the old passwords are required.
  - A Windows server reads the setting from the file.

**What the player sees:**
- **First time with a name:** on connecting, the chat line opens with "Choose a password for your name on this server:". The player types it twice; if the two don't match, the game asks again.
- **After that:** "Your password on this server:". A wrong password gets "Wrong password. Try again:".
- **Typing:** the password shows as stars. Enter sends it, and Esc cancels and stays offline.
- **If the options menu is open:** the Multiplayer status reads "Type your password in the game". The question waits until the menu closes.
- **Reconnecting:** the game keeps the password in memory for the session. A dropped connection reconnects without asking again. The password isn't saved to disk, so the game asks again after a restart. It's sent only to the server and name it was typed for.

**Server side:**
- The first login with a new name creates the account. The file stores the name, a random 16-byte salt and a stretched hash: SHA-256 of the salt and the password, then 9,999 more rounds. It never stores the password.
- Names are matched in any case ("Ann" is "ann").
- Five wrong passwords in a row lock the name for a minute.
- A correct login replaces an older connection with that name that the server hasn't timed out yet.
- `/forget NAME` (host only) deletes an account, so its owner can choose a new password.
- The file is rewritten through a temporary copy, so a crash during a save can't lose the accounts.
- Protocol version 6. The server now checks the version before anything else, so older games get "different hg_pc version" and not "bad hello".

**Tested:**
- **Server, scripted (15 checks, all pass):**
  - A new name is asked to choose a password, and a new name with one is created.
  - The stored hash matches Python's `hashlib`, and the file has no plain password.
  - Asked for the password; a wrong password is refused; the right one works in any case and replaces the older connection.
  - The lock after 5 wrong passwords; other names unaffected.
  - An old game version is told so.
  - Accounts survive a server restart.
  - `/forget`, then choosing a new password.
- **Real game, Linux and Windows (game and server):**
  - The questions show on screen.
  - A mistyped second entry is asked again; the account is created only after a match.
  - A wrong password, then the right one.
  - The server restarted mid-run; the game reconnected without asking again. The Windows server's hash also matches `hashlib`.
- **Normal play:** regression identical on Linux and Windows.

**Limits:**
- **No encryption:** the link is plain TCP, so passwords travel unencrypted, as the server password always has. Players shouldn't reuse an important password here.
- **Forgotten password:** the host can `/forget` it.

**Changing your own password:** type `/password` in the chat.
- The game asks for the current password, then the new one twice, all shown as stars. Esc cancels.
- On a name with no password yet (for example while passwords are off), press Enter at the "current" question; this sets the name's first password.
- The server checks the current password and saves the new one with a new salt. It answers "Password saved", "Wrong current password" or "The new password is empty".
- Wrong current passwords count toward the five-in-a-row lock. Change requests share the chat's rate limit.
- The game keeps the new password for reconnects.
- Protocol version 7 (`NM_PASSWORD`, and `ACCOUNT_CHANGED` in `NM_ACCOUNT`).
- **Tested:**
  - 8 more scripted checks: a wrong current password; an empty new one; a change; the old password refused afterwards and the new one accepted; the lock after 5 wrong current passwords; setting a first password while passwords are off, then needing it when they're on.
  - A real game on Linux and on Windows changed its password through `/password`. The question showed on screen. After a server restart the game reconnected with the new password without asking, and the stored hash matches the new password.
- **Not tested:** typing the password, and Esc, in a real window. The tests answer through `HG_CHAT`.

### Followers and shared NPC movement (2026-11-18)

Other players' following Pokémon now walk behind them, and NPCs walk the same way for every player on a map.

**Following Pokémon (`pc/src/pc_ghost.c`):**
- Each game adds its follower to the position message: species, form, gender, shininess, and the tile it stands on. Nothing is sent while the follower is hidden, for example in its ball while cycling or surfing.
- Other games show it as a second figure with the same overworld sprite. It walks to that tile, the same way the remote player's figure does.
- A remote player and their own Pokémon can pass through each other, so they never block each other when the player turns around. Nobody can talk to either figure.
- The figures use map object ids 208–223 (players use 224–239). `src/map_object.c` keeps all of 208–239 out of the save.
- Data from other players is checked first: a species outside 1–493, or a form the species doesn't have, shows no follower or the base form, so it never reads past the sprite tables.

**NPC movement (`pc/src/pc_npc.c`, server):**
- **Why:** each game moves its NPCs with its own random numbers, so the same NPC used to wander differently for each player.
- **Who leads:** the server names a leader for each map, the player who has been on it longest (`NM_LEAD`). When the leader leaves or quits, the next-longest player takes over.
- **Leader:** sends where that map's NPCs stand and which way they face (`NM_NPC`) whenever one moves or turns, and every 2 seconds anyway. The server relays it only from that map's current leader.
- **Other games on that map:** pause those NPCs' own movement and walk them to the leader's tiles with the same steps as remote players. An NPC more than 6 tiles off is moved there directly. When no fresh data has come for 3 seconds, the NPCs move on their own again.
- **Which objects:** only objects that move by themselves (movement type not 0): wanderers, walkers and NPCs that look around. Objects that stand still are left alone, such as boulders, story characters and item balls. Players, followers and the Union Room and link rooms are left alone too.
- **Scripts:** they run as usual, including ones that walk a synced NPC. Afterwards the NPC walks back to where the leader has it.
- **Protocol:** version 8 (`NM_LEAD`, `NM_NPC`, follower fields in `NM_POS`).
- **Test aids:** `HG_NPC_LOG=1` logs leadership and what's sent; `=2` also compares each followed NPC with the leader once a second. `HG_NPC_OFF=1` turns NPC sync off.

**Tested:**
- **Followers:**
  - Two live games in New Bark, from copies of the user's save, with Chikorita following.
  - Each game showed the other's Chikorita trailing its player, on the correct side after turns.
- **NPC sync:**
  - The follower game's NPCs matched the leader's tile for tile, facing included, with one sample catching a turn in progress.
  - After the leader quit, the other game took over and started sending.
- **Trading:**
  - Pidgey for Sentret over the server, through the invite flow and the Union Room, with both features on: about 11,800 packets, 0 asserts.
- **Server tests:**
  - The scripted server checks (24 and 32) still pass.
  - Normal play is identical on Linux and Windows.

**Not done, or not tested:**
- **The older scripted trade test** (`trade_srv_run.sh`) stopped reaching the partner's figure. Fixed on the same day; see "Trade test fixed" below.
- **Followers while cycling or surfing:** shown hidden, as the game does.
- **Followers in a real window, or other species:** not watched yet.
- **NPC sync with three or more players:** not tried.
- **Outdoor map borders:** an NPC on a neighbouring map follows only if someone is on that map.

### Trade test fixed (2026-11-18)

The scripted server trade test failed for two reasons. Neither was a bug in the game or the server.
- **It walked a fixed path to the partner's figure.** Union Room figures wander, and how far depends on network timing. At frame 4500 the figure stood at the end of the path; by 5320 it had moved a tile, and the test walked past it.
- **It never really traded.** Its string of A presses picked GREET, the first menu item, and it only checked that link data flowed.

**The new test:**
- **Reaching the figure:** the second game talks to the first player's figure with a new test aid, `HG_UNION_TALK=frame` (`pc/src/pc_debug.c`). It works like pressing A in front of the figure, wherever the figure is, and is the same code the invite flow uses (`pc_challenge.c`, `TalkFigure`).
- **The trade itself:** the second game then picks TRADE and the first game accepts. Both offer their second Pokémon (Pidgey and Sentret) and confirm.
- **Pass check:** each game logs its party at the end with another new aid, `HG_PARTY_LOG=frame`. The test passes only if Pidgey and Sentret swapped and neither game asserted.
- **Results:** passed 4 runs out of 4, one of them with NPC sync off.

**Found on the way: the Windows build sometimes failed to start.** In 2 of about 15 Windows runs, the game quit at launch with "cannot reserve DS address range 0x01000000-0x08000000 (error 487)". Something else already held part of the memory range the emulated DS needs. 50 more starts, alone and in pairs, didn't repeat it.
- **Fix:** in `pc/src/pc_mem.c`, the game now restarts itself, at most twice, with the same command line, and passes on the new run's exit code. A new process gets a fresh memory layout.
- **Diagnosis:** before restarting, it logs what held the range (image and file name, mapped file, or private memory), so the cause shows if it happens again.
- **Tested:** with `HG_MEM_FAKEFAIL=1`, a test aid that fakes the first failure, the game restarted once, and that run's regression result matched a normal run.

### Linux server (2026-11-18)

`HGRebuild\linux\` holds a server for Linux hosts such as a home PC or a VPS:
- `hg_server`: the same server as `hg_server.exe`, built statically for 64-bit x86 Linux. It is one 944 KB file that needs no libraries, so it runs on older distributions too. Before this, the Linux build needed glibc 2.38, which ruled out Ubuntu 22.04 and Debian 12. `pc/tools/pc_build.py` now links the Linux server with `-static -s`.
- `hg_server.sh`: starts it on port 7777 and keeps the log, bans and player passwords next to itself. Extra options go on the end, such as `--password` and `--admin`.
- `hg_server.service`: a systemd unit that starts the server at boot and restarts it if it stops. Install steps are in its comments.
- `README.txt`: setup, firewall, host commands and security notes.

**Tested:**
- Started through `hg_server.sh` from another folder; it wrote its files next to the script.
- Both scripted server suites pass against the static build.
- With two real games over it, the trade test passed 2 runs out of 3. The failed run was the game's own "Whoops! Sorry, but I have to do something else." answer: connecting to the partner timed out at the moment of the talk. With the earlier server build the test had passed 5 runs out of 5. The server code is the same, so this looks like a timing flake in the test.

**Not done:**
- **ARM builds** (Raspberry Pi, ARM cloud servers): this needs a cross compiler, which isn't installed here; installing it needs sudo.
- **Other distributions:** the binary hasn't been run on any distribution besides the build machine's Ubuntu.

### Trades and battles in the overworld (2026-11-18)

Trades and battles no longer go through a link room. Face another player, press A and choose:

    Trade or battle with NAME?
    A: Trade   X: Battle
    Y: Double  B: No

The other player sees "NAME wants to trade" (or "battle", "have a double battle") and answers A or B. On yes, the link forms right there and the trade screen or the battle opens. Afterwards both players stand where they were. Double battles need two Pokémon that can fight on both sides. Battles use the whole party, and the Union Room's level limit doesn't apply.

**How it works (`pc/src/pc_link.c`):**
- **Forming the link:** the game's own Wireless Club group code (`src/overlay_03/communication_club.c`) does it, started from a field task instead of the Pokémon Center desk. The inviter leads a group (`ov03_02255C18`) and the other player joins it (`ov03_02255BB0`). The link type is TRADE, SINGLE_BATTLE or DOUBLE_BATTLE.
- **The club's prompts** (the group list, "1 more needed", "accept this member?") show over the overworld. The game answers them by pressing A every 40 frames. B still cancels, and after 30 seconds without a link the game cancels by itself.
- **Then, as the link room scripts do:**
  1. The desk's sync point (96).
  2. For a trade: a fade to black, then the game's trade task (`CallTask_WirelessTrade`: pick, trade, evolve). Afterwards the overworld is restored and faded in.
  3. For a battle: the game's link battle task (`CallTask_020508B8`: transition, battle, results, overworld restored), as the Union Room's battle uses, then a fade in.
  4. The link closes (`sub_02058284`, as `ScrCmd_283`).
- **Link-error check:** the club turns on a check that the link keeps two players. The trade screen and the battle end the link themselves, so only the game's general error check stays on during them.
- **Protocol 9:** the invitation carries the kind (`NetInvite.kind`).

**A PC-only fix found on the way (`src/error_handling.c`):** this PC build keeps `GF_ASSERT` live, and the game's `GF_AssertFail` shows "A communication error has occurred" and resets whenever any assert fires during a link. Retail games compile asserts out, so on a DS those asserts never run. Two harmless ones fire at the end of every link trade: friend-data registration (`sub_0203A1C4`) and a send failing as the link closes. On PC an assert now only logs, as retail effectively behaves.

**Test aids:**
- `HG_LINK=frame:leader|member[:trade|single|double]` starts a link without an invitation.
- `HG_LINK_LOG=1` logs each step.

**Tested** with two live Linux copies in New Bark:
- **Trade through the invite:** Trade, accept, automatic link, Pidgey for Sentret, Quit. Both returned to their spots with the link closed, and a's party then held Chikorita and Sentret. Only the two harmless asserts were logged.
- **Battle through `HG_LINK`:** a single battle fought to the end (both sides switched Pokémon after a faint), then back to the overworld with the link closed.
- **Battle and double battle through the invite:** both started (FIGHT menus on both screens).
- **Other checks:**
  - The Union Room trade test still passes.
  - Normal play is identical on Linux and Windows.

**Not done, or not tested:**
- **A full battle through the invite:** not played to the end. The same task code finished one through `HG_LINK`.
- **The Windows build:** compiles but wasn't run in a link.
- **The club's prompts** still show briefly over the overworld while the link forms.
- **Choosing battle Pokémon:** added later the same day; see "Battle party picker" below.
- **Three or more players inviting at once:** the joiner presses A on the first group in its list, which could be another leader's.
- **The Union Room** is no longer reached through invitations. It still works through the Pokémon Center desk, and through `HG_WARP=union` for tests.

### Battle party picker (2026-11-18)

Before an overworld battle, both players now pick their Pokémon on the game's own battle party screen, the one the Union Room uses.
- **Picking:** each Pokémon shows "NOT ENTERED". Press A, then ENTER, and it becomes FIRST, SECOND and so on. That order is the battle order, so FIRST leads. SUMMARY opens the Pokémon's summary.
- **Limits:**
  - Single battle: 1 to 6 Pokémon.
  - Double battle: 2 to 6.
  - No level limit (the Union Room allows exactly 2, at level 30 or lower).
- **Confirming:** CONFIRM to battle, CANCEL to back out.
- **Waiting:** whoever finishes first sees "Waiting for NAME to choose..." over the map.
- **Cancelling:** if either player cancels, nobody battles. The other player gets "NAME cancelled the battle" in chat, and both stay where they are.

**How it works (`pc/src/pc_link.c`):**
- **The picker:** after the link syncs, the screen fades to black and the party menu opens, set up as `TaskManager_LaunchPartyMenu_UnionRoomBattleSelect` does it but with the limits above. "Summary" opens the summary app and then the picker again, as `Task_OpenPartyMenuForUnionRoomBattleSelect` does. The overworld is then restored and faded in.
- **Telling the partner:** each game sends the result through the server, `NM_INVITE` with the new `INVITE_READY`, or `INVITE_CANCEL`. The Union Room uses its own link message (0x65) for this, which may not exist in battle link mode. Protocol 10.
- **The battle:** when both chose, the link battle runs with the chosen slots in the chosen order. They are the same 1-based slot list the Union Room passes to `CallTask_020508B8`.
- **Error check:** off from one's own pick until both have decided, so one side closing the link first after a cancel is no error. A partner who leaves the server counts as a cancel. It's on again for the battle.

**Tested** with two live Linux copies:
- a entered Pidgey then Chikorita, and b entered only Chikorita. The battle opened with a's Pidgey leading and b's party bar showing one Pokémon. a waited with the message on screen while b chose.
- a cancelled and b chose: both were back in the overworld with the link closed, and b got the cancel notice.
- The invite trade still works, and normal play is unchanged.
- An earlier version closed the link while the other player was still restoring the overworld, which showed b the link error screen. That's fixed by the error-check timing above.

**Not tested:**
- **SUMMARY** from inside the picker.
- **A double battle** with the picker.
- **A full battle** played to the end after picking (the end of a battle was tested before the picker existed).
- **The Windows build** in a link.

### Remote figures follow the real path (2026-11-18)

**Reported problem:** a remote player's figure ended up in the right place, but it got there by its own route. It walked straight toward the latest position, sideways first and then up or down. When an NPC or the local player stood on that route, the figure waited there and looked stuck, while the real player had walked around.

**Fix:**
- **Recording the path:** each position a game hears is now kept in order, as a ring of 32 per remote player (`PcRemote.path`, filled in `pc/src/pc_net.c`), and likewise for their following Pokémon. Games already sent one position per tile moved, so the path is complete. There's no protocol change.
- **Walking it (`pc/src/pc_ghost.c`):**
  - The figure walks those tiles in order, and runs when it is 2 or more steps behind.
  - When it falls more than 28 steps behind, it starts again at the latest position.
  - If an object stands on its next step for 40 frames (about two thirds of a second), the figure moves onto that tile anyway, so it can't stay stuck.
  - The same applies to the following Pokémon.
- **Test aid:** `HG_GHOST_LOG=3` logs each tile a game moves to and each tile a remote figure walks onto.

**Tested:**
- b walked an L around New Bark (down 2, left 4, up 2). On a's screen, b's figure walked the same 13 tiles in the same order.
- Normal play is unchanged on Linux and Windows.

**Not tested:** the "step onto it anyway" rule, which needs an object to stand in the way at the right moment.

## Open questions and risks

- **Comm-layer tolerance:** S1 showed that trades work at 150 ms one way (see above). Battles, packet loss and larger delays are still unmeasured. The server link is TCP, so loss turns into delay.
- **Lockstep determinism:** link battles stay in sync only if both machines compute identically. The regression tests already show this holds between our Linux and Windows builds. Any bug that behaves differently per machine would desync battles, so a desync check (hash of battle state per turn) would help debugging.
- **Cheating:** each client owns its save, so the server can't verify traded Pokémon. That's fine among friends; a public server would need validation, out of scope for now.
- **Union Room limits:** the game supports a fixed number of people in the Union Room and in multi battles. The server must respect these when presenting players to a client.
- **Overworld edge cases:** what remote players look like while surfing, biking or flying, and indoors versus outdoors. Phase 1 can start with walking and running and show others as walking otherwise.
- **Scope:** shared story progress, co-op battles against the AI in the overworld, and seeing remote players' Pokémon following them are not part of this plan.
