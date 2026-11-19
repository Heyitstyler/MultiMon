#!/usr/bin/env python3
"""PC port build: compile the game's C plus the ARM9 NitroSDK/NitroSystem C with host gcc -m32,
link build/pc/hg_pc, and report what is still missing.

Library objects (SDK, NitroSystem, lib/) are made weak, so a host replacement in pc/src/
overrides any single library function. (Windows: PE/COFF binds calls inside one object at assembly
time and can't resolve a weak symbol nothing overrides, so there only the symbols that really are
defined twice are made weak, with `#pragma weak` at compile time; see weaken_duplicates.) Library files that don't compile for x86 (ARM asm)
are skipped; their functions become stubs until replaced in pc/src/.

Reuses the include paths and defines from the DS build's compile_commands.json, so run
`make` once first. Output goes to build/pc/.

    python3 pc/tools/pc_build.py            # compile + report
    python3 pc/tools/pc_build.py --check    # syntax check only
    python3 pc/tools/pc_build.py --win      # Windows: build/pc-win/hg_pc.exe (gcc-mingw-w64-i686)

--win cross-compiles with mingw-w64 into build/pc-win/. SDL3 comes from the official mingw dev
release unpacked at $SDL3_MINGW (default ~/sdl3-mingw/i686-w64-mingw32); SDL3.dll is copied next to
the exe. Without it the exe runs headless.
"""
import collections, concurrent.futures as cf, json, os, re, shlex, shutil, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
DS_BUILD = os.path.join(ROOT, 'build', 'heartgold.us')
WIN = '--win' in sys.argv
OUT = os.path.join(ROOT, 'build', 'pc-win' if WIN else 'pc')
TOOL = 'i686-w64-mingw32-' if WIN else ''
CC, LD, NM, OBJCOPY = TOOL + 'gcc', TOOL + 'ld', TOOL + 'nm', TOOL + 'objcopy'
SYM = '_' if WIN else ''  # i386 PE/COFF prefixes C symbols with '_'
PC_INC = os.path.join(ROOT, 'pc', 'include')
CFLAGS = ['-m32', '-std=gnu99', '-fsigned-char', '-fno-strict-aliasing', '-fwrapv', '-fpermissive', '-w',
          '-O1', '-g', '-fno-common', '-msse2', '-mfpmath=sse', '-fno-pie', '-fno-stack-protector', '-U_FORTIFY_SOURCE', '-I' + PC_INC, '-include', os.path.join(PC_INC, 'pc_compat.h')]
LDLIBS = ['-lm']
LDFLAGS = ['-no-pie']
SDL_INC = []
if WIN:
    # Same struct layouts as the Linux build: gcc bitfields, and 64-bit fields 4-aligned (mingw puts
    # them at 8, which moved save-block fields, so Linux/DS saves failed their checksums). Struct
    # returns in memory as on i386 SysV, which asm2c's pc_callc_struct relies on. Frame pointers make
    # the backtraces work.
    CFLAGS += ['-mno-ms-bitfields', '-fpack-struct=4', '-fpcc-struct-return', '-fno-omit-frame-pointer']
    # The exe sits at a fixed base just above the DS range 0x01000000-0x08000000 that pc_mem.c reserves.
    LDFLAGS = ['-Wl,--image-base,0x08000000', '-Wl,--disable-dynamicbase', '-Wl,--large-address-aware']
    LDLIBS.append('-lws2_32')  # pc_wm.c: UDP for the wireless stand-in
    # (No bigger stack: the main thread's stack is reserved bottom-up, and 8 MB would reach 0x01000000.)
    SDL_DIR = os.environ.get('SDL3_MINGW', os.path.expanduser('~/sdl3-mingw/i686-w64-mingw32'))
    SDL_LIB = os.path.join(SDL_DIR, 'lib', 'libSDL3.dll.a')
    HAVE_SDL = os.path.exists(SDL_LIB)
    if HAVE_SDL:
        SDL_INC = ['-I' + os.path.join(SDL_DIR, 'include')]
else:
    # 32-bit SDL3 (apt: libsdl3-dev:i386) gives a window; without it hg_pc runs headless.
    SDL_LIB = '/usr/lib/i386-linux-gnu/libSDL3.so'
    HAVE_SDL = os.path.exists(SDL_LIB)
    if HAVE_SDL:
        LDLIBS.append('-lSDL3')
# SDK files that write VRAMCNT: every function exit calls pc_vram_sync() (pc/src/pc_vram.c).
INSTRUMENT = ('gx_vramcnt.c', 'gxstate.c')
# Wi-Fi Connection, voice chat, GTS/global data, IRC and DS anti-piracy: no host service behind them.
OFFLINE_LIBS = re.compile(r'NitroDWC|NitroWiFi|libvct|libgds|libdwcppwlobby|libirc|dsprot|libcrypto')
GAME_TARGET = 'main.nef.p/'
LIB_TARGETS = ('subprojects/NitroSDK-', 'subprojects/NitroSystem-', 'lib/')
LIB_SKIP = ('makelcf', 'ichneumon')
# The ARM7 sound driver (sequencer, channel allocator) runs on the host too, driven by pc/src/pc_snd.c.
# Its objects are merged and every symbol they define gets an a7_ prefix, so they don't clash with
# the ARM9 copies of the shared snd files; so do the calls below, which pc_snd.c answers for the ARM7.
SND7_TARGET = 'libsnd_arm7.a.p/'
SND7_SKIP = ('snd_main.c', 'snd_data.c')  # snd_main.c is the ARM7 thread and timer, replaced by pc_snd.c
SND7_HOST_CALLS = ('PXI_SendWordByFifo', 'PXI_SetFifoRecvCallback', 'SND_SendWakeupMessage', 'SND_StartIntervalTimer',
                   'SND_StopIntervalTimer', 'SNDi_LockMutex', 'SNDi_UnlockMutex', 'PMi_ResetControl', 'PMi_SetControl',
                   'SVC_ResetSoundBias', 'SVC_SetSoundBias')


def sources():
    """Yield (kind, cwd, src, flags); kind is 'game', 'lib', or 'pc' (host platform layer in pc/src)."""
    game_env = None
    for e in json.load(open(os.path.join(DS_BUILD, 'compile_commands.json'))):
        toks = shlex.split(e['command'])
        if not e['file'].endswith('.c') or '-o' not in toks:
            continue
        out = toks[toks.index('-o') + 1]
        flags = [t for t in toks if t.startswith(('-I', '-D', '-U'))]
        if '-DSDK_ARM7' in toks:
            if SND7_TARGET in out and os.path.basename(e['file']) not in SND7_SKIP:
                yield 'snd7', e['directory'], e['file'], flags
            continue
        if out.startswith(GAME_TARGET):
            game_env = game_env or (e['directory'], flags)
            yield 'game', e['directory'], e['file'], flags
        elif out.startswith(LIB_TARGETS) and not any(k in out for k in LIB_SKIP):
            yield 'lib', e['directory'], e['file'], flags
    pc_src = os.path.join(ROOT, 'pc', 'src')
    for f in sorted(os.listdir(pc_src)):
        if f.endswith('.c'):  # same include paths and defines as the game
            yield 'pc', game_env[0], os.path.join(pc_src, f), game_env[1]
    asm_out = os.path.join(ROOT, 'build', 'pc', 'asm')  # written by pc/tools/asm2c.py, shared by both targets
    for f in sorted(os.listdir(asm_out)):
        if f.endswith(('.c', '.S')):
            yield 'asm', asm_out, os.path.join(asm_out, f), []


def compile_one(job, check, weak=()):
    kind, cwd, src, flags = job
    rel = os.path.relpath(os.path.normpath(os.path.join(cwd, src)), ROOT)
    obj = os.path.join(OUT, 'obj', 'arm7' if kind == 'snd7' else '', rel + '.o')
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    out = ['-fsyntax-only'] if check else ['-c', '-o', obj]
    if WIN and kind not in ('pc', 'asm'):
        out = ['-DPC_DS_SOURCE'] + out  # pc_compat.h hides _WIN32 from it
    if weak:
        with open(obj + '.weak.h', 'w') as f:
            f.writelines(f'#pragma weak {n}\n' for n in sorted(weak))
        out = ['-include', obj + '.weak.h'] + out
    if src.endswith('.S'):  # x86 data image of a translated asm file
        cmd = [CC, '-m32'] + out + [src]
    else:
        cmd = [CC] + CFLAGS + flags + out + [src]
        if os.path.basename(src) in INSTRUMENT:
            cmd.append('-finstrument-functions')
        if kind == 'pc' and HAVE_SDL:
            cmd += ['-DPC_HAVE_SDL'] + SDL_INC
        if kind == 'snd7':
            cmd[-1:-1] = ['-include', 'nitro/misc.h']  # some ARM7 files use SDK_ASSERT without including it
            if os.path.basename(src) == 'snd_util.c':
                # Use the file's own pitch/volume tables instead of the ARM7 BIOS ones (SVC_Get*Table).
                full = os.path.normpath(os.path.join(cwd, src))
                alt = os.path.join(OUT, 'libsrc', 'arm7', rel)
                os.makedirs(os.path.dirname(alt), exist_ok=True)
                open(alt, 'w').write(open(full).read().replace('#define SND_USE_SYSTEM_ROM_TABLE', ''))
                cmd[-1:] = ['-iquote', os.path.dirname(full), alt]
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode and kind == 'lib' and not src.endswith('.S'):
        # mwcc `asm` functions don't compile with gcc; drop them and keep the file's C functions.
        # The dropped functions become unresolved and are provided by pc/src.
        full = os.path.normpath(os.path.join(cwd, src))
        text = open(full, errors='replace').read()
        stripped = strip_asm_functions(text)
        if stripped != text:
            alt = os.path.join(OUT, 'libsrc', rel)
            os.makedirs(os.path.dirname(alt), exist_ok=True)
            open(alt, 'w').write(stripped)
            cmd2 = [CC] + CFLAGS + flags + ['-iquote', os.path.dirname(full)] + out + [alt]  # out has -U/-include
            r = subprocess.run(cmd2, cwd=cwd, capture_output=True, text=True)
    if r.returncode == 0 and WIN and src.endswith('.S') and not check:
        # The .S labels are C symbol names written bare; give them (and its references) the PE '_'.
        names = {name for _, name in nm(obj, args=()) if not name.startswith(('.', '@'))}
        if names:
            with open(obj + '.syms', 'w') as f:
                f.writelines(f'{n} _{n}\n' for n in sorted(names))
            subprocess.run([OBJCOPY, '--redefine-syms=' + obj + '.syms', obj], check=True)
    if r.returncode == 0 and kind == 'lib' and not check and not WIN:
        # Weaken only what this object defines; weakening undefined refs would hide missing symbols.
        names = [name for _, name in nm(obj, args=('-g', '--defined-only'))]
        if names:
            syms = obj + '.weak'
            with open(syms, 'w') as f:
                f.writelines(n + '\n' for n in names)
            w = subprocess.run([OBJCOPY, '--weaken-symbols=' + syms, obj], capture_output=True, text=True)
            if w.returncode:
                raise RuntimeError(f'objcopy {obj}: {w.stderr}')
    return kind, rel, obj, r.returncode, r.stderr


PRIORITY = {'pc': 0, 'game': 1, 'asm': 2, 'lib': 3}  # which definition wins when a symbol is defined twice


def weaken_duplicates(jobs, results):
    """Windows: recompile every object that defines a symbol a higher-priority object also defines,
    with `#pragma weak` for those symbols, so the winner takes all references, even inside that object."""
    owner, weak = {}, collections.defaultdict(set)
    for job, r in sorted(zip(jobs, results), key=lambda jr: PRIORITY.get(jr[1][0], 9)):
        if r[3] or r[0] not in PRIORITY or job[2].endswith('.S'):
            continue
        for _, name in nm(r[2], args=('-g', '--defined-only')):
            if name.startswith('_'):
                if name in owner:
                    weak[id(job)].add(name[1:])
                else:
                    owner[name] = job
    redo = [(i, j) for i, j in enumerate(jobs) if id(j) in weak]
    print(f'weakened {sum(len(v) for v in weak.values())} overridden symbols in {len(redo)} objects')
    with cf.ThreadPoolExecutor(os.cpu_count()) as ex:
        for (i, j), r in zip(redo, ex.map(lambda ij: compile_one(ij[1], False, weak[id(ij[1])]), redo)):
            results[i] = r


def sdl_delay_lib():
    """Windows: a delay-load import lib for SDL3.dll. Loading it with the exe would bring in user32/gdi32,
    which map about 22 MB at 0x010F0000-0x02681000 before main; delayed, it loads at the first SDL call,
    after pc_mem_init has reserved the DS range."""
    dll = os.path.join(SDL_DIR, 'bin', 'SDL3.dll')
    dump = subprocess.run([TOOL + 'objdump', '-p', dll], capture_output=True, text=True, check=True).stdout
    table = dump.split('[Ordinal/Name Pointer] Table', 1)[1]
    names = re.findall(r'^\s*\[\s*\d+\]\s+\+base\[\s*\d+\]\s+[0-9a-f]+\s+(\w+)$', table, re.M)
    deff, lib = os.path.join(OUT, 'SDL3.def'), os.path.join(OUT, 'libSDL3_delay.a')
    with open(deff, 'w') as f:
        f.write('LIBRARY SDL3.dll\nEXPORTS\n' + ''.join(n + '\n' for n in names))
    subprocess.run([TOOL + 'dlltool', '-d', deff, '-D', 'SDL3.dll', '-y', lib], check=True)
    return [lib, '-ldelayimp']


def link_snd7(objs):
    """Merge the ARM7 sound driver objects into one and a7_-prefix its symbols (see SND7_HOST_CALLS)."""
    out = os.path.join(OUT, 'obj', 'snd7.o')
    subprocess.run([LD, '-m', 'i386pe' if WIN else 'elf_i386', '-r', '-o', out] + objs, check=True)
    names = {name for _, name in nm(out, args=('-g', '--defined-only'))} | {SYM + n for n in SND7_HOST_CALLS}
    with open(out + '.syms', 'w') as f:
        f.writelines(f'{n} {SYM}a7_{n[len(SYM):]}\n' for n in sorted(names))
    subprocess.run([OBJCOPY, '--redefine-syms=' + out + '.syms', out], check=True)
    return out


ASM_FUNC = re.compile(r'^[ \t]*((?:static\s+|inline\s+)*)asm\s+([^;{}]*?\))\s*(;|\{)', re.M)


def strip_asm_functions(text):
    """Turn each mwcc `asm` function into a plain extern prototype (body removed, `static`,
    `register` dropped), so the rest of the file compiles and the function resolves to pc/src."""
    out, pos = [], 0
    for m in ASM_FUNC.finditer(text):
        if m.start() < pos:
            continue
        out.append(text[pos:m.start()])
        proto = re.sub(r'\bregister\s+', '', m.group(2))
        out.append(proto + ';')
        pos = m.end()
        if m.group(3) == '{':  # skip the body up to its matching brace
            depth = 1
            while depth and pos < len(text):
                depth += {'{': 1, '}': -1}.get(text[pos], 0)
                pos += 1
    out.append(text[pos:])
    return ''.join(out)


def nm(path, tool=None, args=('-g',)):
    r = subprocess.run([tool or NM, *args, path], capture_output=True, text=True)
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            yield parts[-2], parts[-1]  # (type, name)


def ds_definers():
    """symbol -> (DS object/library that defines it, nm type), from the matching build."""
    where = {}
    for dirpath, _, files in os.walk(DS_BUILD):
        for f in files:
            if f.endswith(('.o', '.a')):
                p = os.path.join(dirpath, f)
                for typ, name in nm(p, 'arm-none-eabi-nm', ('-g', '--defined-only')):
                    where.setdefault(name, (os.path.relpath(p, DS_BUILD), typ))
    return where


def ds_sizes():
    """symbol -> (address, size) from the linked DS ELF."""
    out = {}
    r = subprocess.run(['arm-none-eabi-nm', '-S', os.path.join(DS_BUILD, 'main.nef')], capture_output=True, text=True)
    for line in r.stdout.splitlines():
        p = line.split()
        if len(p) == 4:
            out[p[3]] = (int(p[0], 16), int(p[1], 16))
        elif len(p) == 3:
            out[p[2]] = (int(p[0], 16), 0)
    return out


def host_symbols():
    """Symbols the 32-bit host libc/libm/libgcc provide; never stub these."""
    out = set()
    def gcc_path(arg):
        return subprocess.run([CC, '-m32', arg], capture_output=True, text=True).stdout.strip()
    if WIN:  # import libs and mingw runtime; names lose the '_' prefix and any stdcall '@n'
        libs = [(gcc_path('-print-file-name=lib' + l + '.a'), ('--defined-only',))
                for l in ('msvcrt', 'mingwex', 'mingw32', 'moldname', 'kernel32')]
        libs.append((gcc_path('-print-file-name=crt2.o'), ('--defined-only',)))  # atexit
        libs.append((gcc_path('-print-libgcc-file-name'), ('--defined-only',)))
        if HAVE_SDL:
            libs.append((SDL_LIB, ('--defined-only',)))
        for path, args in libs:
            out.update(name[1:].split('@')[0] for _, name in nm(path, args=args) if name.startswith('_'))
        return out
    libs = [(gcc_path('-print-file-name=libc.so.6'), ('-D', '--defined-only')),
            (gcc_path('-print-file-name=libm.so.6'), ('-D', '--defined-only')),
            (gcc_path('-print-libgcc-file-name'), ('--defined-only',))]
    if HAVE_SDL:
        libs.append((SDL_LIB, ('-D', '--defined-only')))
    for path, args in libs:
        out.update(name.split('@')[0] for _, name in nm(path, args=args))
    return out


def write_stubs(missing, where, sizes):
    """Generate stubs so the PC binary links: functions abort with their name, data is zeroed storage.
    Returns linker args for absolute linker-script symbols (overlay IDs etc.)."""
    defsyms, lines = [], ['/* Generated by pc/tools/pc_build.py. Do not edit. */', 'void pc_stub_hit(const char *name);',
                          'void pc_offline_call(const char *name);']
    host = host_symbols()
    for s in missing:
        if s in host:
            continue
        if s.startswith('SDK_') and s in sizes:  # linker-script absolute symbol
            defsyms.append(f'-Wl,--defsym={SYM}{s}=0x{sizes[s][0]:x}')
        elif s in where and OFFLINE_LIBS.search(where[s][0]) and where[s][1] not in 'DdBbRrC':
            # shortcut: online/anti-piracy libraries answer 0 (offline / not detected); port them if online play is wanted.
            lines.append(f'unsigned long long {s}(void) {{ pc_offline_call("{s}"); return 0; }}')
        elif where.get(s, (None, 'T'))[1] in 'DdBbRrC' or (s not in where and s not in sizes):
            # data, or mwcc runtime / linker-made symbols (_binary_*, __global_destructor_chain)
            size = max(sizes.get(s, (0, 4))[1], 4)
            lines.append(f'unsigned char {s}[{size}] __attribute__((aligned(4)));')
        else:  # function (DS nm sometimes types these '?', so main.nef presence is enough)
            lines.append(f'void {s}(void) {{ pc_stub_hit("{s}"); }}')
    path = os.path.join(OUT, 'stubs.c')
    open(path, 'w').write('\n'.join(lines) + '\n')
    return path, defsyms


def classify(definer):
    if definer is None:
        return 'not found in DS build'
    definer = definer[0]
    m = re.search(r'subprojects/(Nitro\w+|libvct)', definer)
    if m:
        return m.group(1).split('-')[0]
    if '.s.o' in definer:
        return 'game asm'
    return 'game C/other: ' + definer.split('/')[-1]


def main():
    check = '--check' in sys.argv
    subprocess.run([sys.executable, os.path.join(ROOT, 'pc', 'tools', 'asm2c.py')], check=True)
    jobs = list({(j[0], os.path.normpath(os.path.join(j[1], j[2]))): j for j in sources()}.values())  # same .c can appear twice
    os.makedirs(OUT, exist_ok=True)
    with cf.ThreadPoolExecutor(os.cpu_count()) as ex:
        results = list(ex.map(lambda j: compile_one(j, check), jobs))
    if WIN and not check:
        weaken_duplicates(jobs, results)
    for kind in ('game', 'lib', 'pc', 'asm', 'snd7'):
        mine = [r for r in results if r[0] == kind]
        print(f'compiled {sum(1 for r in mine if r[3] == 0)}/{len(mine)} {kind} files')
    game_failed = [r for r in results if r[3] and r[0] in ('game', 'pc', 'asm', 'snd7')]
    lib_failed = [r for r in results if r[3] and r[0] == 'lib']
    with open(os.path.join(OUT, 'lib_failed.txt'), 'w') as f:
        for _, rel, _, _, err in lib_failed:
            first = next((l for l in err.splitlines() if 'error' in l), '')
            f.write(f'{rel}\t{first}\n')
    if lib_failed:
        print(f'  {len(lib_failed)} library files skipped (build/pc/lib_failed.txt)')
    for _, rel, _, _, err in game_failed:
        lines = err.splitlines()
        print('FAIL', rel, '\n', '\n'.join(([l for l in lines if 'error' in l] or lines)[:8]))
    results_all = results
    if check or game_failed:
        return 1 if game_failed else 0
    results = [r for r in results if r[3] == 0 and r[0] != 'snd7']
    results.append(('snd7', 'snd7.o', link_snd7(sorted(r[2] for r in results_all if r[0] == 'snd7')), 0, ''))

    defined, undefined = set(), set()
    for _, _, obj, _, _ in results:  # game and library objects
        for typ, name in nm(obj):
            if WIN and (not name.startswith('_') or '@' in name):
                continue  # not a C symbol (section, stdcall Windows API)
            (undefined if typ in 'Uwv' else defined).add(name[len(SYM):])
    missing = sorted(undefined - defined - host_symbols())
    where = ds_definers()
    groups = collections.defaultdict(list)
    for s in missing:
        groups[classify(where.get(s))].append(s)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'missing.txt'), 'w') as f:
        for g in sorted(groups):
            for s in groups[g]:
                f.write(f'{g}\t{s}\n')
    print(f'{len(missing)} unresolved symbols (full list: build/pc/missing.txt)')
    for g, syms in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print(f'{len(syms):6}  {g}')

    stubs, defsyms = write_stubs(missing, where, ds_sizes())
    exe = os.path.join(OUT, 'hg_pc.exe' if WIN else 'hg_pc')
    if WIN and HAVE_SDL:
        LDLIBS[:0] = sdl_delay_lib()
    cmd = [CC] + CFLAGS + LDFLAGS + ['-o', exe, stubs] + [obj for _, _, obj, _, _ in results] + defsyms + LDLIBS
    r = subprocess.run(cmd, capture_output=True, text=True)
    errs = [l for l in r.stderr.splitlines() if 'error' in l or 'undefined' in l or 'multiple definition' in l]
    if r.returncode == 0 and WIN and HAVE_SDL:
        shutil.copy(os.path.join(SDL_DIR, 'bin', 'SDL3.dll'), OUT)
    print(f'link: {"OK -> " + os.path.relpath(exe, ROOT) if r.returncode == 0 else "FAILED"} ({len(errs)} error lines)'
          f'{"" if HAVE_SDL else " [headless: no 32-bit SDL3 at " + SDL_LIB + "]"}')
    for l in errs[:30]:
        print(' ', l)
    # The multiplayer server: a small standalone program, no game code. On Linux it is linked
    # statically, so the one file runs on any x86-64 Linux (older glibc, a VPS) with nothing installed.
    server = os.path.join(OUT, 'hg_server.exe' if WIN else 'hg_server')
    s = subprocess.run([CC, '-O2', '-Wall', '-I', os.path.join(ROOT, 'pc', 'src'), '-o', server,
                        os.path.join(ROOT, 'pc', 'server', 'hg_server.c')] + (['-lws2_32'] if WIN else ['-static', '-s']),
                       capture_output=True, text=True)
    print(f'server: {"OK -> " + os.path.relpath(server, ROOT) if s.returncode == 0 else "FAILED"}')
    for l in s.stderr.splitlines()[:10]:
        print(' ', l)
    return r.returncode or s.returncode


if __name__ == '__main__':
    sys.exit(main())
