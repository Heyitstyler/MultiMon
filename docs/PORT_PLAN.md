# HeartGold native x86 port — plan

Base: [pret/pokeheartgold](https://github.com/pret/pokeheartgold) @ `6bf138d` (2026-11-08), cloned in WSL at `~/pokeheartgold`
(Windows path: `\\wsl$\Ubuntu\home\<user>\pokeheartgold`). Game data is read at runtime from the user's own ROM; nothing from it is redistributed.

## Facts measured (2026-11-14)

| | |
|---|---|
| ROM `Pokemon_HeartGold_USA_NDS-XPA.nds` | `IPKE` rev 0. All 313 hashed NitroFS files and ARM7 match pret. Whole-ROM SHA-1 differs only because the secure area at `0x4000` is stored decrypted (`ffdeffe7…`). Usable as-is. |
| Game code still in asm | 18,674 functions (18,590 Thumb, 84 ARM), ~1.05M lines in `asm/`, all translated by asm2c |
| Game code in C | ~10,500 functions, ~220k lines in `src/` — **about 36% decompiled** by function count |
| Biggest asm files | `overlay_96` (1435 funcs), `overlay_07` (812), `overlay_14` (690), `overlay_49` (665), `overlay_40` (558) |
| SDK | NitroSDK / NitroSystem / NitroDWC / NitroWiFi pulled as meson wraps from `github.com/ntrtwl/*` |
| SDK API used by game | ~987 distinct symbols; heaviest: `GX_*` 2414 calls, `NNS_*` 1063, `PAD_*` 646, `MI_*` 584, `FS_*` 444 |

## Key decisions

1. **32-bit x86 (i686) first.** The game assumes 4-byte pointers in thousands of structs and `u32` casts. `-m32` keeps every layout unchanged. x86-64 is a later, separate job.
2. **Don't wait for 100% decomp.** Remaining asm is machine-translated from pret's `.s` files to C. These files use symbols, not raw addresses, so the output links natively against the real C code. Hand-decompiled C replaces the translated C function by function as pret progresses.
3. **Replace the hardware, not the game.** Keep NitroSystem (`NNS_*`, mostly C) and reimplement the NitroSDK layer under it on the host: `GX/G2/G3`, `OS`, `FS`, `MI`, `PAD`, `TP`, `RTC`, `CARD`, `SND`. VRAM, OAM and palettes stay as host arrays. A software 2D renderer composes them each frame, the same way an emulator's PPU does.
4. **Overlays are all linked at once.** `FS_LoadOverlay` becomes "reset that overlay's `.data`/`.bss` from a pristine copy, then run its static initializers". Overlays that share an address are safe because pret names their symbols uniquely.
4b. **DS memory map stays real.** In the 32-bit process, reserve the DS regions at their real addresses: main RAM `0x02000000`, IO `0x04000000`, palette `0x05000000`, VRAM `0x06000000`, OAM `0x07000000`. Register and VRAM writes from C and translated asm then land in plain memory, and the renderer reads them at frame end. Only registers with side effects (DMA, divider/sqrt, G3 FIFO, IRQ ack) need handlers.
5. **Files come from the ROM.** `FS_*` reads NitroFS (FNT/FAT) straight from the user's `.nds` file.
6. **Single-threaded main loop.** VBlank and IRQ waits become host frame pacing (60 Hz), and the VBlank callback runs at the end of each frame. The few `OS_Thread` users map to coroutines.
7. **ARM7 work moves to the host.** Sound becomes an SSEQ/SBNK/SWAR sequencer feeding an SDL audio stream. Touch, RTC and saves are served directly by the platform layer.

## Hard parts (known risks)

- **Calls between C and translated asm.** Translated functions use an emulated register file, so each crossing needs a bridge. Bridges are generated from the prototypes in `include/`. Asm functions with no prototype need one written.
- **Hardware register access** (`0x04000000+`) inside translated asm and the SDK. Loads and stores in that range are routed to platform handlers.
- **3D engine (`G3_*`)** for battles, overworld models and the Pokéathlon. This needs a DS-accurate geometry and raster path (GL or software). It is the largest single piece after the asm translator.
- **Timing.** Code that relies on DMA or IRQ ordering may need per-case fixes.

## Milestones

- **M0** ✅ 2026-11-14 — `make -j20` in WSL builds `build/heartgold.us/pokeheartgold.us.nds` in 1m47s. SHA-1 is `4fcded0e2713dc03929845de631d0932ea2b5a37`, and all 4 meson checksum tests pass.
- **M1** ✅ 2026-11-14 — `python3 pc/tools/pc_build.py` (branch `pc-port`, not committed) compiles all 414 game C files with `gcc -m32` in about 10 s and links `build/pc/hg_pc`, a 32-bit i386 ELF. It runs `NitroMain` and exits at the first unported call, `OS_Init`. The DS build still matches `4fcded0e…`.
  - It reuses `-I`/`-D` from the DS `compile_commands.json`, plus `pc/include/pc_compat.h` (`SDK_PC`, `clz` builtin, `__declspec` → gcc attributes), `-fpermissive`, and an empty `pc/include/extras.h`.
  - Game source changed in only 2 files, both matching-neutral. In `berry_pots_app_tasks.c`, 9 header prototypes became static forward declarations. In `weather_manager.c`, an mwcc array assignment is `memcpy` under `SDK_PC`.
  - Unresolved symbols are stubbed from `main.nef` symbol info: functions abort with their name, and data gets zeroed storage of the DS size. Overlay IDs are passed with `--defsym`. The full list is in `build/pc/missing.txt`.
  - **1970 unresolved symbols:** 1491 game asm, 230 NitroSDK, 139 NitroSystem, 18 NitroDWC, 7 DSProt/IRC. The other 85 are linker-script symbols and libc, already resolved.
  - Only asm functions called directly from C are counted. Asm that only other asm calls appears once translation starts.
- **Platform layer, first pass** ✅ 2026-11-14 (done before M2, by choice)
  - The ARM9 NitroSDK and NitroSystem now compile for x86 too: 230 of 273 files, with symbols weakened by `objcopy --weaken-symbols` so `pc/src` overrides any single function. The 43 that don't compile (ARM asm) are listed in `build/pc/lib_failed.txt`.
  - Host modules in `pc/src/`:
    - `pc_mem`: DS address range `0x01000000`–`0x08000000` mapped at fixed addresses.
    - `pc_rom`: the user's ROM is opened, its header copied into the system work area, and the IPKE game code checked.
    - `pc_os`: interrupts, locks, caches, a single main `OSThread`, and VBlank wait as the 60 Hz frame boundary.
    - `pc_mi`: memory copies, plus LZ77 decompression.
    - `pc_dma`: DMA transfers complete immediately.
    - `pc_dgt`: MD5 and SHA-1, with a known-answer self-test at startup.
    - `pc_card`: ROM reads go straight to the file. The SDK FS code runs unchanged on top.
    - `pc_backup`: 512 KB flash saved to `<rom>.sav` (or `$HG_SAVE`). It is written through on every write or erase, in the raw DS layout.
    - `pc_pxi`: no ARM7.
    - `pc_rtc_pm`: local time, and PM calls are no-ops.
    - `pc_input`: active-low keypad registers and touch through the TP API.
    - `pc_main`: entry point, a 5 s hang/crash watchdog with backtrace, and `HG_MAX_FRAMES`.
  - **Result:** with `HG_STUB_CONTINUE=1` (stubs log and return), `NitroMain` finishes init (FS, saves, sound archive, RTC, touch) and runs its main loop for 600+ frames without hang or crash. 42 distinct stubs are hit, almost all game asm: `OamManager_*`, `GF_InitG2dRenderer`, `HandleFadeUpdateFrame`, `Sound_*`, `Save_*` init.
  - In normal mode it still stops at the first one, `sub_02027010`, an asm function that finishes FS setup. **The boot path is now gated on M2.**
- **M2** ✅ 2026-11-14 — `pc/tools/asm2c.py` runs automatically from `pc_build.py` and takes about 8 s.
  - **Coverage:** all 18,674 asm functions translate (18,590 Thumb, 84 ARM) since 2026-11-16. Before that, 12 Thumb functions were unsupported (11 `bl` to a local label, 1 cross-function branch) and the 84 ARM-mode functions only reported themselves.
  - **Build:** all 562 generated files (281 `.c` + 281 `.S`) compile. Unresolved symbols dropped from 1745 to 459, mostly online libraries (now offline stubs) and SDK 3D.
  - **How it works:**
    - Each asm function becomes `asm_NAME(void)` on a global ARM register file. The emulated stack sits at `0x07FFFF00`. Flags are locals, so dead flag updates compile away.
    - Each function also gets an `ASM_EXPORT` C-ABI wrapper under its real name. r0–r3 map to args 1–4 and 8 more words go to the emulated stack; the return is r0:r1. C, asm and function pointers interoperate without prototypes.
    - Each file's `.text` is rebuilt as an x86 `.S` byte image, with every instruction as padding of its exact Thumb size. Literal pools, `add rX, pc` and jump tables (`add pc, rX`, resolved by comparing against label addresses) land where the code expects.
    - Labels defined in more than one file (overlays at the same address) are renamed per file.
    - `lr` holds a magic return value, so `pop {r3}; bx r3` returns correctly.
  - **Compiler runtime:** the Metrowerks helpers are in `pc/src/pc_mwrt.c` (`_s32_div_f` gives quotient and remainder, `_ll_*`, soft float and double), with a self-test. Doubles are low word first, as confirmed from asm constants. The float compare helpers return their result in CPU flags on the DS; asm2c converts the host sign result into flags.
  - **Overlays:** `pc_overlay.c` turns load, start and unload into no-ops, because all overlay code is linked in.
  - **Online:** NitroDWC, NitroWiFi, vct, GDS, PPW lobby, IRC and DSProt get generated "offline" stubs that log once and return 0.
  - **Result (strict mode):** init → intro movie overlay → scenes 1–2 with sprites drawn through NitroSystem 2D → about 1,740 frames, about 29 s. It stops at intro scene 3 on `G3X_Init`, the 3D engine (M5).
- **ARM mode** ✅ 2026-11-16 — `ArmTranslator` in asm2c translates the 84 ARM-mode functions: 67 in `asm/nitrocrypto.s` (RC4, RSA, bignum) and 17 in `asm/overlay_93_arm.s` (3D code of an overlay launched from the ov45 lobby table, probably a Wi-Fi Plaza minigame).
  - Handles the pre-UAL mnemonics (`ldmeqia`, `streqb`, `ldrhs` = `ldr` + `hs`) via `split_arm_mnemonic`, conditions on any instruction, S-suffix-only flags, barrel-shifter operand 2 (`pc_lslv`/`pc_lsrv`/`pc_asrv`/`pc_rorv` in `asm_rt.h`), pre/post-indexed and writeback addressing, `ldm`/`stm` in all four modes, `mul`/`mla`/`umull`/`smull`, and pc reading as address + 8.
  - Refused, with none in the game: S-suffixed logical ops with a shifted operand (they would need the shifter carry), `rrx`, writes to pc other than `mov pc, lr` and `ldm {…, pc}`.
  - **Check:** `pc_crypto_selftest.c` runs at every startup. RC4 is checked against the published "Key"/"Plaintext" vector. Three RSA-1024 PKCS#1 v1.5 SHA-1 signatures made with test-only keys must verify, and three bad ones must fail: a corrupted signature, the wrong message and the wrong key. This runs `BN_mod_exp_mont`, `BN_div`, padding and ASN.1 parsing. A trace confirmed 3 accepted and 3 rejected. The overlay 93 functions compile but have not been run.
  - **Cross-function branch:** `ov45_0222AC14` jumps into `ov45_0222AC58`, which the disassembler split off from it. Branches into another function's straight-line code up to a return (calls allowed) are now inlined (`shared_tail`).
  - **Library asm (2026-11-16):** asm2c also translates `LIB_ASM`, writing `build/pc/asm/lib_*.c`: `lib/NitroDWC/asm/overlay_45_arm.s` (`libdwcppwlobby`, Wi-Fi Plaza lobby, 570 functions), `overlay_39_arm.s` (`libgds`, 53) and `crypto_static.s` (3, including `CRYPTO_RC4Init`, now in the self-test). That is 19,300 functions in total, 710 of them ARM, and 0 unsupported. The ARM translator gained jump tables (`addls pc, pc, rX, lsl #2`, which becomes a `switch` whose entries are `b`/`bx`) and flag-setting logical ops with a shifted operand (the shifter carry goes through the `LSL_I`/`LSR_R`… macros).
    - Left out on purpose: `lib/asm/libirc.s` is the infrared port (Pokéwalker), raw cartridge-SPI polling. Translated, `CARD_SpiWaitGetStatus` spun forever in the overworld (watchdog hang), so it stays an offline stub. `lib/syscall/asm/_secure_IP*.s` is DSProt's copy of the BIOS calls, already replaced in `pc/src`.
    - The network stack map is in `HGRebuild\NETWORK.md`.
  - **Calls into shared overlay RAM (2026-11-16):** several overlays load at the same address, and pret's disassembly names a call there after just one of them. The port links all overlays at once, so such a call always went to the named one. Wild battles crashed this way: the battle controller (ov12) calls `bl ov10_0221BE20` with one param block, but in a normal battle OVY_7 is loaded at 0x0221BE20, so the real callee is `ov07_0221BE20`. ov10 is the trainer AI, which takes 4 args, and it wrote through `ctx` = a stray byte + 0x354. A random-walk test from the user's save reproduced it in 6 of 6 runs.
    - **Fix:** `pc_overlay.c` keeps `pc_ovl_seq[id]`, a load sequence number per overlay, set by `FS_LoadOverlay`/`FS_StartOverlay` and cleared by `FS_UnloadOverlay`. asm2c's `overlay_tables()` reads `main.lsf` and the DS objects' symbols to find functions whose address other overlays' functions share. Every `bl`, `b` tail call and `.word` function pointer from another overlay to such a function goes through a thunk in the generated `build/pc/asm/ovl_dispatch.c`. The thunk calls whichever candidate's overlay was loaded most recently, falling back to pret's choice. There are 212 thunks.
    - **Check:** 6 walks of 72,000 frames from the user's save (Linux and Windows, 3 seeds) have no crash, asserts or guard skips. A trace shows the battle calling `ov07_0221BE20` and never `ov10_0221BE20`. The new-game regression is unchanged.
    - **Not covered:** C callers in `src/` name their targets directly. A wrong name there would need fixing in the source.
  - **Hardware timers and play time (2026-11-16):** play time never advanced, so the CONTINUE screen and trainer card showed 0:00. The game's play-time clock (`src/timer3.c`, timer 3, prescaler 64) and `OS_GetTick` (timer 0) count overflow IRQs plus the live counter, and no timer ran on the host. `TimersFrame()` in `pc_os.c` now advances timers 0–3 by one DS frame of cycles (560,190) at each frame boundary. It polls the registers, because C writes them as plain memory, and fires the timer IRQ handlers on overflow. Time therefore follows frames: deterministic headless, and real time at the 60 Hz windowed pace. Check: 2 minutes of play and a save took the CONTINUE screen from 0:00 to 0:02.
    - **Side effect, fixed:** with time flowing, the main menu's wireless scan for Mystery Gift, Ranger and Wii beacons timed out and powered the radio up a second time. The SDK reported `ILLEGAL_STATUS`, because the first power-up never got its ARM7 answer, and the game called `OS_Terminate`. `pc_pxi.c` now answers WVR power-up and power-down with success one frame later. WM itself still gets no answers, so no beacons are ever found. A 20,000-frame idle on the main menu runs clean. `OS_Terminate` now prints a backtrace (`pc_backtrace`).
  - **Windows crash reports:** `OnCrash` and the watchdog write through `WriteFile` (`ErrOut` in `pc_main.c`), not stdio. msvcrt's unbuffered stderr writes one character at a time under the stream lock. On 2026-11-16 a crash while walking (an access violation in `SysTask_GetData`, found in the Windows event log) left only `hg_pc: ` in `play.log`, cut off mid-line, with no report. `HG_CRASHTEST=1` faults on purpose at startup to check the report. Both builds print the exception and a backtrace.
  - **Regression note:** the intro/title frames (2000, 4000) and the WAV depend on the real-time clock. Around 19:00 they changed for the old baseline binary too, so rerun `hg_base` before taking an intro diff as a regression.
- **M2 leftovers** — Map 64-bit args, which need ARM register-pair alignment and are not handled by the wrapper. Handle struct-return (sret) functions if any are implemented in asm.
- **M3** ✅ 2026-11-14 — video and input. The user confirmed the window output looks right.
  - **`pc_vram.c`:** VRAM banks A–I live in one memfd, mapped permanently at the LCDC window and remapped into BG/OBJ views whenever VRAMCNT changes. Extended palette slots are exposed to the renderer as pointers. The hook is `-finstrument-functions` on the only two SDK files that write VRAMCNT (`gx_vramcnt.c`, `gxstate.c`).
  - **`pc_gpu2d.c`:** software renderer for both 2D engines, scanline by scanline.
    - BG modes 0–5: text, affine, extended affine, 256-colour and direct bitmaps.
    - OBJ: tiled 1D/2D, bitmap, affine and double-size.
    - Extended palettes, windows 0/1/OBJ, alpha/brighten/darken, semi-transparent and bitmap-alpha OBJ, master brightness, display off and VRAM display.
    - Screens are swapped per POWCNT1. The game's HBlank handler runs after each line.
    - Shortcuts: no mosaic, no mode-6 large bitmap, no display capture, and the 3D layer is blank.
  - **`pc_cp.c`** plus shadow headers `pc/include/nitro/cp/{divider,sqrt}.h`: the DS divider/sqrt unit. Every `CP_Set*` setter is wrapped so the result registers are filled at once, with a self-test at startup. Without it `FX_Div`/`FX_Inv` returned 0 and affine sprites drew as solid rectangles.
  - **`pc_video.c`:** SDL3 window with both screens stacked, keyboard pad, mouse touch, and 60 Hz pacing. It runs headless if 32-bit SDL3 is not installed or `HG_HEADLESS=1`. `HG_DUMP=60,300` writes PPM frames.
  - **Result (headless):** the copyright screen, sunset, Ho-Oh fly-in, grass and Lyra scenes render correctly. That is about 1,740 frames, in 4 s unpaced, up to the 3D stop.
  - **Result (windowed):** with `libsdl3-dev:i386` installed, `hg_pc` opens a WSLg window and plays the intro at 60 Hz for about 29 s until `G3X_Init`.
  - `pc_build.py` treats SDL3's exports as host symbols, so they are never stubbed.
- **3D layer** ✅ 2026-11-14
  - **Command capture.** C stores to the geometry ports are moved by shadow headers `pc/include/nitro/hw/ARM9/ioreg_G3*.h` onto a read-only trap page at `0x04800000`. Each store faults, is single-stepped (x86 TF), and the value is fed to the engine. This keeps `&reg_G3X_GXFIFO` and port address arithmetic working with no source edits. Translated asm stores into the IO page go through `pc_io_store` (asm2c `ST8/16/32`). DMA, `MI_CpuSend32` and `GX_SendFifo*` feed the engine directly.
  - **`pc_g3.c` (geometry):** packed FIFO and port decoding; DS fixed-point matrix stacks; clip and vector result registers; lighting (melonDS formulas); texcoord transforms; triangle/quad/strip assembly; culling; homogeneous clipping; viewport; box, position and vector tests; GXSTAT.
  - **`pc_g3r.c` (raster):** float scanline rasterizer with perspective correction.
    - All 7 texture formats, including 4x4-compressed.
    - Modulate, decal, toon and highlight modes.
    - Alpha test, blending, the translucent polygon-ID rule, and fog.
    - Shortcuts: no edge marking, anti-aliasing or shadow polygons.
  - **3D in the 2D compositor:** BG0 shows the 3D layer, with its own alpha and BG0HOFS scroll.
  - **SDK `asm` functions.** `pc_build.py` retries a failing library file with its mwcc `asm` functions stripped, which recovered fx_mtx*, g3x, g3b, g3imm and mi_memory. `pc_fx.c` provides the stripped MTX/MI/GX functions.
  - **DMA:** `pc_dma.c` implements `MIi_DmaSetParams*`, so every SDK DMA path does its transfer. VBlank and HBlank DMA run at those points. Before this, about 12 SDK DMA functions silently did nothing.
  - **Build flags and shims:** `-fwrapv` (ARM-like signed overflow), plus MSL `null.h`/`wchar_t.h` shims.
  - **Sound handshake:** sound command lists are acknowledged with `finishCommandTag`; no audio yet.
  - **Bug found by tracing:** `VTX_DIFF` deltas are raw 4.12, not ×8.
- **M4** ✅ 2026-11-14 — Boots to the title screen (3D Ho-Oh, logo, "Touch to Start") after the full intro: about 4,200 frames, 5,200 frames in 30 s headless. Input past the title is not yet tested.
- **M5 (in progress)** 2026-11-15 — New game to the overworld, walking. Headless scripted input (`HG_INPUT="frame:BUTTON,..."`, `Tx.y` for touch) gets past the title, the tutorial menu, Oak's speech, the gender choice and name entry. The game then loads the player's bedroom in 3D, and the player walks in all directions with the camera following. Zero `GF_ASSERT` failures.
  - **asm↔C ABI gaps fixed in asm2c / `asm_rt.h`:**
    - Calls forward 16 words, not 12. `CreateSpriteResourcesHeader` takes 15, and its last two args arrived as garbage.
    - Struct returns: ARM returns ≤4 bytes in r0 and larger ones through r0; i386 always uses a hidden pointer the callee pops. The 8 affected C functions are listed in `STRUCT_RETURN` and called through `pc_callc_struct`.
    - Sub-word returns: after calling a C function that returns `u8`/`u16`/`s8`/`s16`/`BOOL8`, r0 is extended as ARM does (2,376 call sites, found automatically).
    - Sub-word params: the `ASM_EXPORT` wrappers narrow `u8`/`u16` args from header prototypes (157 functions), because the asm assumes mwcc-extended args.
    - **`bl` to a local label (2026-11-16):** mwcc emits `bl` as a long in-function branch, since a Thumb `b` only reaches 2 KB. asm2c refused these, so 11 functions stayed untranslated. One was `ov12_0225E830` (battle controller), which crashed when the bag was opened in battle. All 11 targets return with `pop {..., pc}` or make their own calls before reading LR, so they are now translated as `goto`. The 11 are `ov12_0225E830`, `ov83_0223E14C`, `ov96_02210C98`, `ov96_02216C38`, `ov74_0222C2EC`, `ov115_0225F978`, `sub_02075E14`, `ov49_02262028`, `ov49_02262FB4`, `ov49_02263B74` and `ov112_021E6164`.
    - **The bug that blocked walking:** `ROR_R` passed its temporary `_r` into `LOGIC`, which declares its own `_r`, giving `au32 _r = (_r);`. Every register rotate (243 sites) returned garbage, so tile `x % 32` lookups read the wrong attribute and every step was blocked. Fixed, with a shift-macro self-check at startup.
  - **Game source fixes** (correct for both builds):
    - `ov01_02204698` and `ov01_021FFF5C` were declared `void`, but asm callers use their r0 result. They now return it (`u16`, `void *`). Any others can be found with scratchpad `voidret.py`.
    - Under `SDK_PC`: the VBlank task queue holds 128, not 32. VBlank only runs between frames here, not mid-load, so one-shot tasks pile up.
    - Under `SDK_PC`: the Options app's teardown (`ov54_021E5DBC`) clears the VBlank callback before it frees `bgConfig`. The game only clears it a frame later, so one VBlank ran on freed memory and DMA'd from a junk pointer (0x0BA08580). On the DS that address is the empty GBA slot and the read does nothing; here it crashed both builds on leaving Options.
    - Under `SDK_PC`: `BerryPotsApp_Exit` clears its VBlank callback. The game never clears it, so it ran on the freed app data until the next screen set its own callback.
    - **Same-bug sweep (2026-11-15):**
      - **C screens:** scratchpad `vbscan.py` walks each screen's teardown state machine, inlining local helpers, and flags any frame boundary reached while the VBlank callback is registered and its BgConfig is already freed. Run on the original `options_app.c`, it flags `OptionsMenu_Main`. Across `src/` it found only Berry Pots, plus two false positives. Alph Puzzle frees and clears in the same call. The `ov44` wireless lobby frees its struct and rebuilds it in the same frame.
      - **Asm screens:** 61 translated-asm files register VBlank callbacks, and their teardowns can't be scanned this way. Instead, `pc_mem_copy_ok` (`pc_mem.c`) now guards DMA and MI CPU copies. A source or destination outside game memory is skipped and logged: `hg_pc: skipped a N-byte copy … (stale pointer)`. Game memory is the DS range up to the end of the host image's `.bss`, or the stack. Test: with the Options fix disabled, both builds log the stale copy and carry on, and the next frame matches the fixed build. A log line like that names a screen with the same bug. Only copies are guarded; a stale pointer used some other way can still crash. Zero-byte copies always pass. The game makes some from address 0 (VRAM transfers with nothing to send), and they used up the 8-line log limit.
    - Under `SDK_PC`: `GF_AssertFail` calls `pc_assert_hit`, which prints a backtrace when `HG_ASSERT_LOG` is set.
  - **Platform:** GBA slot is always empty (`pc_ctrdg.c`); it used to segfault at the main menu. The crash report prints the faulting address and EIP.
  - **Debugging aid:** `HG_TRACE=fn1,fn2` logs entry r0–r3 and the returned r0 of the named asm functions, plus what a returned RAM pointer and the entry r3 point at.
  - **Sound** (2026-11-15): music and sound effects play.
    - **The driver is the SDK's own ARM7 sound code.** `libsnd_arm7` C covers the sequencer, the channel allocator and command processing.
      - `pc_build.py` compiles it as kind `snd7` with `SDK_ARM7`, merges it into `snd7.o`, and gives every symbol it defines an `a7_` prefix.
      - The prefix keeps it apart from the ARM9 copies of the shared snd files.
      - `snd_main.c` (the ARM7 thread and alarm) is replaced by `pc_snd.c`.
      - `pc_snd.c` runs the 5.2 ms ticks per frame and carries PXI words both ways: command lists, wakeups, alarms.
    - **Mixer** (`pc_snd.c`): plays the channel registers the way the DS mixer does.
      - Handles PCM8, PCM16, IMA-ADPCM with loop state, PSG duty and the noise LFSR.
      - No interpolation. 10-bit output stage, so one full channel is ¼ of full scale.
      - Output is ~32728 Hz to an SDL3 stream, through WSLg's PulseAudio.
      - **Latency:** about 1.25 frames sit in the queue and the device buffer is 256 samples, roughly 23 ms on our side (it was about 95 ms).
        - These are the smallest sizes measured with zero device starvation at 60 fps.
        - WSLg's PulseAudio adds about 88 ms of its own (measured with `pa_simple_get_latency`), plus the RDP hop to Windows.
        - Only a native Windows build would remove that part.
        - Playback runs 60/59.83 faster, since frames are paced at 60 Hz but each carries a 59.83 Hz frame's worth of samples.
        - A further ±1% nudge holds the queue level.
        - After a hitch, a backlog over 6 frames is dropped.
        - The intro/title stretch around frames 3300–3600 runs at about 50 fps on the CPU, and audio starves there.
      - `HG_WAV=path` writes the mix to a WAV, for headless checks.
    - **Shadow headers:**
      - `nitro/hw/ARM7/ioreg_SND.h` moves the ARM7 sound registers to 0x04700400, off the ARM9 geometry FIFO addresses.
      - `nitro/snd/common/channel.h` makes `SND_StartChannel` call the mixer, so a channel restarted within one tick restarts.
      - `nitro/snd/common/util.h`: `SND_CalcDecibel(128)` returns 0 dB.
    - **Found:**
      - Sample addresses must keep all 32 bits, because the sound heap is host `.bss` (0x0a3xxxxx).
      - The game deliberately passes volume 128 (`GF_SndHandleMoveVolume(0, 128, 15)`). That reads one past `SNDi_DecibelTable`, which on the DS lands on `PitchTable[0] = 0`. On the host it made the field music +21.6 dB.
    - **Not emulated:** sound capture (reverb), the output selectors that route channels 1/3 around the mixer, and the HOLD bit. `NNS_SndArcStrm*` streams are still no-ops.
    - **Not handled:** `SND_COMMAND_SETUP_CHANNEL_PCM` masks addresses to 27 bits, so direct channel playback of host-memory data would break. HG doesn't appear to use it.
  - The ROM build's matching is not re-checked after the source edits.
  - **Next:** leave the house, the first scripted events, battles, saves.
- **Native Windows build** (2026-11-15) — `python3 pc/tools/pc_build.py --win` cross-compiles in WSL with `gcc-mingw-w64-i686` (13.2) into `build/pc-win/hg_pc.exe`. It removes WSLg from the audio, video and input path; WSLg's PulseAudio alone added about 88 ms of audio latency.
  - **Headless parity:** the new-game-and-walk script (7,800 frames, ending with the player walking in the bedroom) gives bit-identical frame dumps at 8 points and a bit-identical `HG_WAV`. That holds across the Linux binary from before the Windows work, the current Linux build and the Windows build. There are zero `GF_ASSERT` failures and no guard skips. (The first parity runs had used an input file with a stray quote, so they only covered the intro and title with no input. They were redone.)
  - **Memory map:** `pc_mem.c` reserves 0x01000000–0x08000000 with `VirtualAlloc` as the first thing in `main`. The exe links at a fixed base of 0x08000000 (`--image-base`, `--disable-dynamicbase`, `--large-address-aware`). The stack stays at mingw's default of 2 MB, because the main thread's stack is reserved bottom-up and 8 MB reached 0x01000000.
  - **VRAM (both builds):** `pc_vram.c` no longer aliases banks with a memfd. Windows maps views only at 64 KB granularity, and banks F, G and I sit at 16 KB. Each bank's data now lives in one place, either its BG/OBJ view or its LCDC slot. A VRAMCNT change copies every bank from its old place to its new one and zeroes the view it left. The Linux output did not change.
  - **Geometry trap:** a vectored exception handler replaces SIGSEGV/SIGTRAP. The trap still single-steps with EFLAGS.TF, and `pc_mem_protect_page` hides `VirtualProtect`/`mprotect`.
  - **`pc_main.c`:** the watchdog is a thread that suspends the game thread after 5 s without a frame and walks its EBP chain. Crashes go through `SetUnhandledExceptionFilter`. Assert and stub backtraces use `CaptureStackBackTrace`. Addresses print as `[0x…]`; resolve them with `addr2line -e build/pc-win/hg_pc.exe`, since the image base is fixed.
  - **Pacing (both builds):** `SDL_GetTicksNS` and `SDL_DelayPrecise` replace `clock_nanosleep`. `pc_rtc_pm.c` uses `localtime`.
  - **Build differences under `--win`:**
    - Flags: `-mno-ms-bitfields` keeps gcc bitfield layout. `-fpack-struct=4` keeps 64-bit struct fields 4-aligned, as on i386 SysV and the DS's mwcc. mingw aligns them to 8, and `-mno-align-double` doesn't change that. With 8-byte alignment, save blocks with 64-bit fields changed size, so a save made by the Linux build failed its checksums and the game showed no CONTINUE. Checked: a copy of the WSL save gives identical CONTINUE-menu and overworld frames from both builds. `-fpcc-struct-return` returns structs of 8 bytes or less in memory as i386 SysV does, which `pc_callc_struct` relies on. `-fno-omit-frame-pointer` is for the backtraces.
    - **Weak symbols:** PE/COFF gas binds calls inside one object at assembly time, and ld can't resolve a weak definition that nothing overrides. So `objcopy --weaken-symbols` is not used. After compiling, `weaken_duplicates` finds every symbol defined twice and recompiles the losing objects with `#pragma weak` for those names only (94 symbols in 23 objects). The winner is chosen in order: pc, then game, then asm, then lib. `__declspec(weak)` is empty on mingw for the same reason.
    - **`_WIN32` hidden from DS sources:** GameSpy's `nonport.h` includes `windows.h` and `DWCi_Nd.h` redefines `BOOL` under `_WIN32`. `pc_compat.h` (with `-DPC_DS_SOURCE` for game, lib and snd7 files) first includes the C library headers, which mingw needs `_WIN32` for, and then undefines it. `pc_compat.h` also maps the `__declspec` forms that the mingw headers use.
    - **Symbol prefix:** COFF adds `_` to C symbols. The `.S` images from asm2c get their labels renamed with `objcopy --redefine-syms`. The `a7_` prefixing, `--defsym` and host-symbol scans handle the prefix, using the mingw import libs and `crt2.o`. asm2c's `.note.GNU-stack` line is now under `#ifdef __ELF__`.
    - **SDL3:** comes from the official `SDL3-devel-3.4.2-mingw` release, unpacked in WSL at `~/sdl3-mingw` (override with `$SDL3_MINGW`, which points at the `i686-w64-mingw32` folder). `SDL3.dll` is copied next to the exe. Without SDL3 the exe runs headless.
    - **SDL3.dll is delay-loaded.** Importing it normally brings in user32 and gdi32, which map about 22 MB at 0x010F0000–0x02681000 before `main` runs, and the DS range could not be reserved. `pc_build.py` makes a delay-load import lib from the DLL's export table (`dlltool -y`, plus `-ldelayimp`). The DLL now loads at the first SDL call, after `pc_mem_init`. The exe imports only `KERNEL32` and `msvcrt` at startup.
  - **Windowed:** SDL3 uses WASAPI at 44.1 kHz with a 441-sample (10 ms) device buffer, the shared-mode minimum. The startup log prints the driver and buffer. Our queue is still the ~1.25 frames tuned for WSLg, so the total is roughly 21 ms queue + 10 ms device + the WASAPI engine. That replaces WSLg's ~88 ms PulseAudio path plus the RDP hop. A 900-frame windowed run took 15.7 s, which is 60 Hz pacing. Headless output with SDL linked is still bit-identical.
  - **Running:** `HGRebuild\win\play.bat` runs a copy of `hg_pc.exe` and `SDL3.dll` from a Windows folder and writes its log to `win\play.log`. The save comes from `win\hg_pc.cfg` (`save=…\win\heartgold.sav`).
  - **Options menu (2026-11-16, `pc/src/pc_menu.c`):** Esc in the window pauses the game and opens a menu drawn with SDL's 8×8 debug font. Window close still quits.
    - Volume (0–100 in steps of 5, `SDL_SetAudioStreamGain`).
    - Game speed 0.25x–4x in steps of 0.25, or Max: `Pace()` divides the frame period, and the audio frequency ratio is multiplied by the speed, so sound plays faster or slower with it. Max mutes. The config stores a decimal (`speed=1.25`, 0 = Max).
    - Fullscreen.
    - Load another save: SDL file dialog. The chosen path goes into the config, and the process restarts itself with `HG_SAVE` cleared (`CreateProcessW` with the same command line, or `execv` on Linux), because the game reads its save only at boot.
    - Save a copy: `pc_backup_copy_to` writes the 512 KB save memory as it is now.
    - Quit.
    - Fast forward: hold Tab (bindable) to run at the fast-forward speed (`ffspeed=`, 0.25x–4x or Max, default Max). `pc_cfg_speed()` returns it while the key is held, so pacing and sound follow.
    - Controls…: rebind the 12 DS buttons and fast forward (press Enter on a row, then the key; Esc cancels; a key already in use swaps). "Reset to defaults" is included. Bindings are stored as `key_<action>=<SDL key name>`. Esc always opens the menu and can't be bound. Inside the menu, Enter or the bound A key activates, and Esc or the bound B key goes back.
    - ROM: the order is the command line, then `$HG_ROM`, then `rom=`, then a file picker at startup (windowed only). The picker accepts only game code IPKE and reopens after an error box. The ROM in use is remembered as an absolute path, except in headless runs. "Change ROM…" in the menu restarts with the new one. Restarts now launch with no arguments, so the config decides the ROM and save.
    - Settings persist in `hg_pc.cfg` next to the executable (`SDL_GetBasePath`). The save path order is `$HG_SAVE`, then `save=`, then `<rom>.sav`. While the menu is open, `pc_watchdog_kick` keeps the watchdog quiet; on Windows the watchdog now watches `sAlive`, not `sFrames`.
    - `HG_MENU_SHOT=file.bmp` saves the menu as drawn at frame 900. Checked that way on both builds with `SDL_VIDEODRIVER=offscreen`. The file dialogs, the restart and fullscreen haven't been driven by a test; they need a person at the window.
    - **Don't keep the save on the WSL share.** `play.bat` used to point `HG_SAVE` at a save on the WSL share (`\\wsl.localhost\...\hg_test.sav`). The distro shuts down a few seconds after its last Linux process exits; it was seen restarting between two commands. The exe's open handle then goes dead, and every later save write fails. A Pokécenter save was lost this way (2026-11-16): the file was never written. `heartgold.sav` started as a copy of `hg_test.sav` from before that session. `pc_backup.c` now logs `hg_pc: save write failed: <reason>` when a write fails. To run it directly: `hg_pc.exe <path to your .nds>`. The `HG_*` variables work as on Linux. From WSL, pass them with `WSLENV` (scratchpad `run.sh` does this).
  - **Not yet checked by ear:** the user still has to confirm that menu sounds are prompt and play past the bedroom. The queue target could probably go below 1.25 frames on WASAPI; that hasn't been measured.
