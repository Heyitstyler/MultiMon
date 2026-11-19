# How connections work in HeartGold, and where the PC port stands

The game has two separate networking stacks. Both end at hardware or servers the PC doesn't have.
File paths are in the WSL repo (`~/pokeheartgold`). The comm modules are still unnamed in pret
(`unk_XXXXXXXX`), so the roles given for them below are inferred from the WM/DWC calls they make.

## 1. Local wireless (DS to DS)

Trades, link battles, the Union Room, Pokéathlon and wireless Mystery Gift.

| Layer | Where | Notes |
|---|---|---|
| Feature screens | many overlays | Call the game's comm API, never WM directly. |
| Comm core | `src/unk_02037C94.c` (176 functions, C), `asm/unk_02034B0C.s`, `asm/unk_02033AE0.s`, `asm/unk_02034354.s`, `asm/unk_020379A0.s` | Inferred: sessions, beacon/TGID, command dispatch, link level (`WM_GetLinkLevel`). |
| Radio wrapper | `asm/unk_02032844.s` (75 functions) | The only game code that drives WM: `WM_Initialize`, `WM_StartScan`, `WM_StartParent`, `WM_StartConnectEx`, `WM_StartMP`, `WM_SetMPDataToPortEx`, `WM_SetPortCallback`, `WM_Disconnect`, `WM_End`, … |
| Other WM users | `asm/overlay_74_thumb.s` (it also does RSA checks and RC4, so probably wireless Mystery Gift), `asm/overlay_12_022378C0.s` (battle) | |
| NitroSDK WM, ARM9 side | `subprojects/NitroSDK-*/libraries/wm/src/*.c` (compiled into the port) | Each `WM_*` call is a request sent to the ARM7 over the PXI FIFO. The answer comes back as a callback. |
| ARM7 WM + radio | `libraries/wm/src/ARM7/req_*.c` | On a DS, the ARM7 runs the radio: MP (multi-poll) frames between one parent and up to 15 children. |

**On PC (since 2026-11-17):** `pc/src/pc_wm.c` stands in for the ARM7 radio side. Wireless power (WVR) is answered in `pc/src/pc_pxi.c`. The radio carries the MP data between copies over UDP (`HG_WM_PORT`, `HG_WM_PEERS`). Two local copies traded through the Union Room this way, including with 150 ms one-way delay; see `MULTIPLAYER_PLAN.md`, "S1 result". Without `HG_WM_PORT`, the radio works but hears nobody. The game sits waiting and then shows its communication error.

**Hook for a custom multiplayer:** stand in for the ARM7 WM side on the host. That means answering the WM PXI requests, which is how `pc_snd.c` stands in for the ARM7 sound driver, and carrying the MP data between PCs over UDP. Replacing the `WM_*` API in `pc/src` would also work. Either way, the game's own Union Room, trade and battle logic runs unchanged. The WM API surface the game uses is the ~30 functions listed above.

## 2. Nintendo Wi-Fi Connection (online)

GTS, Wi-Fi Club, the Wi-Fi Plaza, Battle Videos and voice chat.

| Layer | Where | State in the port |
|---|---|---|
| Wi-Fi session wrapper | `asm/overlay_00_thumb.s`, loaded from `asm/unk_020915B0.s` | Translated. Uses DWC friend match (`DWC_ProcessFriendsMatch`), per-player AIDs (`DWC_GetMyAID`, `DWC_GetAIDBitmap`), `DWC_SendReliableBitmap`, receive and timeout callbacks, and VCT voice chat. |
| Other DWC callers | `src/unk_02037C94.c`, `asm/unk_02034354.s`, `asm/unk_02035900.s`, overlays 13, 38, 39, 43, 44, 70, 72–75 | Translated. |
| Wi-Fi Plaza lobby | `lib/NitroDWC/asm/overlay_45_arm.s` (`libdwcppwlobby`, 570 ARM functions), with the game side in `asm/overlay_45_thumb.s` and minigames in overlays 46, 47, 89, 91 and 93 | **Translated now.** |
| Global Data Server (Battle Videos, uploads) | `lib/NitroDWC/asm/overlay_39_arm.s` (`libgds`, 53 functions) | **Translated now.** |
| DWC RC4 setup | `lib/NitroDWC/asm/crypto_static.s` (3 functions) | **Translated now.** The self-test checks `CRYPTO_RC4Init`. |
| DWC core | `subprojects/NitroDWC-*/libraries/{base,account,bm,gs}` (C source; `gs` is GameSpy) | Not compiled. Offline stubs return 0. |
| DWC prebuilt parts | `libdwcac.a` (AP connect), `libdwcauth.a`, `libdwcnd.a`, `libdwcnhttp.a`, `libdwcranking.a`, `libdwcutil.a` | Only ARM binaries; no source to compile. Offline stubs. |
| NitroWiFi | `subprojects/NitroWiFi-*/libraries/{soc,wcm}` (C: sockets, AP connect), `cps`/`ssl` (prebuilt TCP/IP and SSL) | Not compiled. Offline stubs. |
| Voice chat | `subprojects/libvct-*` (C) | Not compiled. Offline stubs. |

**On PC:** `OFFLINE_LIBS` in `pc/tools/pc_build.py` turns every unresolved DWC/WiFi/VCT/IR/DSProt call into a stub that logs once (`hg_pc: offline: NAME`) and returns 0. The translated lobby and GDS code now runs, but everything under it is still those stubs. Nintendo's servers closed in 2014. Going online would mean compiling the DWC/NitroWiFi C against host sockets, standing in for the prebuilt archives, and pointing it at a replacement server.

## 3. Infrared (Pokéwalker)

The HeartGold cartridge has an IR transceiver that sits on the cartridge's SPI bus. `lib/asm/libirc.s` (`libirc`, which stands for IR communication, not IRC chat) is 9 raw SPI routines such as `CARD_SpiWaitGetStatus`, which polls the bus until the chip answers. I tried translating them: the game then hung in the overworld, because the field code calls them and nothing on the PC ever answers. They are left as offline stubs that return 0.

## Not translated on purpose

`lib/syscall/asm/_secure_IP{GE,KE}.s` (DSProt anti-piracy): these are copies of the BIOS `SVC_*` calls, which `pc/src` already replaces. If DSProt's checks ran for real on a PC, they could flag the game as pirated. They stay stubbed as "not detected".
