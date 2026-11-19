#!/usr/bin/env python3
"""asm2c: translate the game's asm (pret asm/*.s and the LIB_ASM files, mwasm syntax) into C for the PC port.

For each asm/X.s this writes build/pc/asm/X.c and build/pc/asm/X.S:
  X.c  one `void asm_NAME(void)` per Thumb function, operating on the global ARM register file
       (pc/include/asm_rt.h), plus an ASM_EXPORT C-ABI wrapper under the function's real name.
  X.S  x86 assembly holding the file's data: .data/.rodata/.bss as written, and an image of .text
       where each instruction is replaced by padding of its exact Thumb size. Literal pools, jump
       tables and pc-relative arithmetic therefore land where the translated code expects.

ARM-mode functions are translated too (ArmTranslator).

    python3 pc/tools/asm2c.py            # translate all of asm/
"""
import collections, glob, json, os, re, shlex, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
DS_BUILD = os.path.join(ROOT, 'build', 'heartgold.us')
OUT = os.path.join(ROOT, 'build', 'pc', 'asm')

REGS = {f'r{i}': i for i in range(16)}
REGS.update(sp=13, lr=14, pc=15, ip=12, sb=9, sl=10, fp=11)
CONDS = {
    'eq': 'z', 'ne': '!z', 'cs': 'c', 'hs': 'c', 'cc': '!c', 'lo': '!c', 'mi': 'n', 'pl': '!n',
    'vs': 'v', 'vc': '!v', 'hi': '(c && !z)', 'ls': '(!c || z)', 'ge': '(n == v)', 'lt': '(n != v)',
    'gt': '(!z && n == v)', 'le': '(z || n != v)',
}
DATA_DIRECTIVES = {'.word': '.long', '.4byte': '.long', '.long': '.long', '.short': '.short', '.hword': '.short',
                   '.2byte': '.short', '.byte': '.byte', '.space': '.space', '.skip': '.space', '.ascii': '.ascii',
                   '.asciz': '.asciz', '.string': '.asciz', '.balign': '.balign', '.fill': '.fill', '.incbin': '.incbin'}
DROP_DIRECTIVES = {'.public', '.global', '.globl', '.extern', '.type', '.size', '.thumb', '.arm', '.code',
                   '.thumb_func', '.syntax', '.cpu', '.fpu', '.ltorg', '.pool', '.hidden'}
SECTIONS = {'.text': 'text', '.data': 'data', '.rodata': 'rodata', '.bss': 'bss'}
CMP_HELPERS = {'_fls', '_fleq', '_fgr', '_fgeq', '_feq', '_fneq', '_dls', '_dleq', '_dgr', '_dgeq', '_deq', '_dneq'}
# C functions returning a struct by value, with its size. ARM returns <= 4 bytes in r0 and larger ones
# through a pointer in r0; i386 always uses a hidden pointer the callee pops. Found by scanning src/ for
# struct returns that asm calls; re-scan if more are decompiled.
STRUCT_RETURN = {
    'BillboardGfxSequence_GetTexPlttIndexAt': 2,
    'ov42_02228270': 4, 'ov42_022282DC': 4, 'ov42_022282E8': 4, 'ov42_022282F4': 4,
    'Camera_GetAngle': 8,
    'Camera_GetLookAtCamTarget': 12, 'Camera_GetLookAtCamPos': 12,
}
# C functions returning a sub-word integer -> C cast type. ARM extends such results to 32 bits in r0;
# i386 leaves eax's upper bits undefined, so asm testing r0 would see garbage. Filled by scan_narrow_returns().
NARROW_RETURN = {}
NARROW_TYPES = {'u8': 'au8', 's8': 'as8', 'u16': 'au16', 's16': 'as16', 'BOOL8': 'au8', 'bool': 'au8', 'char': 'as8',
                'unsigned char': 'au8', 'signed char': 'as8', 'unsigned short': 'au16', 'short': 'as16'}


def scan_narrow_returns():
    pat = re.compile(r'^(?:static\s+)?(?:inline\s+)?(?:const\s+)?(unsigned char|signed char|unsigned short|u8|s8|u16|s16|BOOL8|bool|char|short)'
                     r'\s+(\w+)\s*\([^;]*?\)\s*\{', re.M)
    for pattern in ('src/**/*.c', 'lib/**/*.c', 'subprojects/*/libraries/**/*.c'):
        for f in glob.glob(os.path.join(ROOT, pattern), recursive=True):
            for m in pat.finditer(open(f, errors='replace').read()):
                NARROW_RETURN[m.group(2)] = NARROW_TYPES[m.group(1)]


# asm functions taking sub-word integer params -> [(wrapper arg name, C cast type)]. mwcc callers extend
# such args to 32 bits and the asm relies on it; i386 callers may leave the upper bits as garbage.
NARROW_PARAMS = {}


def split_params(text):
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch == ',' and depth == 0:
            out.append(cur); cur = ''
            continue
        depth += ch in '(['
        depth -= ch in ')]'
        cur += ch
    return out + [cur]


def scan_narrow_params(asm_funcs):
    proto = re.compile(r'^[A-Za-z_][\w \*]*?\b(\w+)\s*\(([^;{}]*)\)\s*;', re.M)
    names = ['a0', 'a1', 'a2', 'a3'] + [f's{i}' for i in range(12)]
    for pattern in ('include/**/*.h', 'src/**/*.c'):
        for f in glob.glob(os.path.join(ROOT, pattern), recursive=True):
            for m in proto.finditer(open(f, errors='replace').read()):
                fn = m.group(1)
                if fn not in asm_funcs or fn in NARROW_PARAMS:
                    continue
                params = [p.strip() for p in split_params(m.group(2))]
                fix = []
                for i, prm in enumerate(params[:len(names)]):
                    if re.search(r'\b(u64|s64|long long|double)\b', prm) and '*' not in prm:
                        break  # takes two slots; later args would be misnumbered
                    typ = re.sub(r'\s*\b\w+$', '', prm) if re.search(r'\w\s+\w+$', prm) else prm
                    typ = re.sub(r'\b(const|volatile)\s+', '', typ).strip()
                    if '*' not in prm and '[' not in prm and typ in NARROW_TYPES:
                        fix.append((names[i], NARROW_TYPES[typ]))
                NARROW_PARAMS[fn] = fix


LABEL_RE = re.compile(r'^([A-Za-z_.$?][\w.$?]*):')
IDENT_RE = re.compile(r'[A-Za-z_.$?][\w.$?]*')


class Unsupported(Exception):
    pass


def overlay_tables():
    """From the DS build: overlay of each object, global function -> overlay, FS ID per overlay, and
    for functions whose address other overlays' functions share, all the candidates there.
    Several overlays load into the same RAM; pret's disassembly names a call into such a region
    after one of them (it once picked trainer AI ov10_0221BE20 where battle has ov07_0221BE20)."""
    obj_ovl, cur = {}, 'main'
    for line in open(os.path.join(ROOT, 'main.lsf')):
        m = re.match(r'\s*Overlay (\w+)', line)
        if m:
            cur = m.group(1)
        elif re.match(r'\s*(Static|Autoload)\b', line):
            cur = 'main'
        m = re.search(r'Object main\.nef\.p/(\S+)', line)
        if m:
            obj_ovl[m.group(1)] = cur
    nm = lambda path: subprocess.run(['arm-none-eabi-nm', path], capture_output=True, text=True).stdout.splitlines()
    sym_ovl = {}
    for obj, ovl in obj_ovl.items():
        path = os.path.join(DS_BUILD, 'main.nef.p', obj)
        if os.path.exists(path):
            for line in nm(path):
                p = line.split()
                if len(p) == 3 and p[1] == 'T':
                    sym_ovl.setdefault(p[2], ovl)
    ids, addr = {}, {}
    for line in nm(os.path.join(DS_BUILD, 'main.nef')):
        p = line.split()
        if len(p) != 3:
            continue
        m = re.fullmatch(r'SDK_OVERLAY_(\w+)_ID', p[2])
        if m:
            ids[m.group(1)] = int(p[0], 16)
        elif p[1] == 'T' and p[2] in sym_ovl and sym_ovl[p[2]] != 'main':
            addr[p[2]] = int(p[0], 16) & ~1
    at = collections.defaultdict(list)
    for name, a in addr.items():
        at[a].append(name)
    ambig = {}
    for names in at.values():
        if len({sym_ovl[n] for n in names}) > 1:
            cands = [(n, ids[sym_ovl[n]]) for n in sorted(names) if sym_ovl[n] in ids and n not in STRUCT_RETURN]
            for n in names:
                ambig[n] = cands
    return obj_ovl, sym_ovl, ambig


def dispatch_c(used, ambig, sym_ovl, ids_of):
    """C thunks: call the candidate whose overlay was loaded last, else the disassembly's choice."""
    args = ', '.join(['au32 a0', 'au32 a1', 'au32 a2', 'au32 a3'] + [f'au32 s{i}' for i in range(12)])
    fwd = ', '.join(['a0', 'a1', 'a2', 'a3'] + [f's{i}' for i in range(12)])
    out = ['/* Generated by pc/tools/asm2c.py: calls into RAM shared by several overlays. Do not edit. */',
           '#include "asm_rt.h"', 'extern au32 pc_ovl_seq[];', '']
    names = sorted({c for t in used for c, _ in ambig[t]} | set(used))
    out += [f'extern au8 {n}[];' for n in names] + ['']
    for t in sorted(used):
        out.append(f'au64 pc_ovlx_{t}({args}) {{')
        out.append(f'    const void *f = {t}; au32 best = pc_ovl_seq[{ids_of[t]}];')
        for c, cid in ambig[t]:
            if c != t:
                out.append(f'    if (pc_ovl_seq[{cid}] > best) {{ best = pc_ovl_seq[{cid}]; f = {c}; }}')
        out.append(f'    return ((PcCFn)(uintptr_t)f)({fwd});')
        out += ['}', '']
    return '\n'.join(out)


# ---------------------------------------------------------------- reading
def asm_envs():
    """path -> (cwd, -I/-D flags) for preprocessing each asm file, from the DS build's commands."""
    out = {}
    for e in json.load(open(os.path.join(DS_BUILD, 'compile_commands.json'))):
        path = os.path.normpath(os.path.join(e['directory'], e['file']))
        if path.endswith('.s'):
            toks = shlex.split(e['command'])
            out.setdefault(path, (e['directory'], [t for t in toks if t.startswith(('-I', '-D'))]))
    if not any(os.path.dirname(p) == os.path.join(ROOT, 'asm') for p in out):
        raise SystemExit('no asm entry in compile_commands.json; run make first')
    return out


# Library asm translated too: Wi-Fi Plaza lobby (libdwcppwlobby), global data server (libgds) and
# DWC's RC4 setup. Not lib/syscall (DSProt's copies of the BIOS calls, which pc/src replaces) and not
# lib/asm/libirc.s (raw cartridge-SPI polling for the infrared port; it would spin forever here).
LIB_ASM = ['lib/NitroDWC/asm/crypto_static.s', 'lib/NitroDWC/asm/overlay_39_arm.s',
           'lib/NitroDWC/asm/overlay_45_arm.s']


def strip_comment(line):
    out, quote = [], None
    for ch in line:
        if quote:
            out.append(ch)
            if ch == quote:
                quote = None
        elif ch in '"\'':
            quote = ch
            out.append(ch)
        elif ch == ';':
            break
        else:
            out.append(ch)
    return ''.join(out).rstrip()


def read_source(path, cwd, flags, inc_dirs):
    r = subprocess.run(['gcc', '-E', '-P', '-w', '-x', 'assembler-with-cpp'] + flags + [path],
                       cwd=cwd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f'cpp failed on {path}:\n{r.stderr[:2000]}')
    return expand_includes(r.stdout.splitlines(), os.path.dirname(path), inc_dirs, cwd)


def expand_includes(lines, here, inc_dirs, cwd, depth=0):
    out = []
    for raw in lines:
        line = strip_comment(raw)
        m = re.match(r'\s*\.include\s+"([^"]+)"', line)
        if not m:
            out.append(line)
            continue
        name = m.group(1)
        if name == 'macros.inc':  # function macros are handled natively
            continue
        for d in [here] + inc_dirs:
            p = os.path.normpath(os.path.join(cwd, d, name)) if not os.path.isabs(d) else os.path.join(d, name)
            if os.path.exists(p):
                text = open(p, errors='replace').read().splitlines()
                out.extend(expand_includes(text, os.path.dirname(p), inc_dirs, cwd, depth + 1))
                break
        else:
            raise SystemExit(f'.include "{name}" not found')
    return out


def split_operands(s):
    parts, depth, cur = [], 0, []
    for ch in s:
        if ch in '[{(':
            depth += 1
        elif ch in ']})':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if ''.join(cur).strip():
        parts.append(''.join(cur).strip())
    return parts


def parse(lines):
    """-> list of items: ('label', name) | ('dir', name, args) | ('ins', mnem, operands, text)"""
    items = []
    for line in lines:
        s = line.strip()
        while True:
            m = LABEL_RE.match(s)
            if not m:
                break
            items.append(('label', m.group(1)))
            s = s[m.end():].strip()
        if not s or s.startswith('#'):
            continue
        word, _, rest = s.partition(' ')
        if '\t' in word:
            word, _, rest2 = s.partition('\t')
            rest = rest2
        rest = rest.strip()
        if word.startswith('.'):
            items.append(('dir', word, rest))
        else:
            items.append(('ins', word.lower(), rest, s))
    return items


# ---------------------------------------------------------------- structure
class Func:
    def __init__(self, name, mode):
        self.name, self.mode = name, mode
        self.items = []  # ('label', name) | ('ins', ...) | ('dir', ...)
        self.labels = []


def structure(items):
    """Split a file into an ordered list of (section, item) and functions."""
    section = 'text'
    funcs, out, cur = [], [], None
    for it in items:
        if it[0] == 'dir' and it[1] in SECTIONS:
            section = SECTIONS[it[1]]
            out.append(('section', section))
            continue
        if it[0] == 'dir' and it[1] == '.section':
            name = it[2].split(',')[0].strip()
            section = SECTIONS.get(name, 'skip:' + name)
            out.append(('section', section))
            continue
        if it[0] == 'ins' and it[1] in ('thumb_func_start', 'non_word_aligned_thumb_func_start',
                                        'arm_func_start', 'local_arm_func_start'):
            cur = Func(it[2].strip(), 'arm' if 'arm' in it[1] else 'thumb')
            funcs.append(cur)
            out.append(('func_start', cur, it[1]))
            continue
        if it[0] == 'ins' and it[1] in ('thumb_func_end', 'arm_func_end'):
            out.append(('func_end', cur))
            cur = None
            continue
        if cur is not None and section == 'text':
            cur.items.append(it)
            if it[0] == 'label':
                cur.labels.append(it[1])
        out.append(('item', it, cur, section))
    return out, funcs


# ---------------------------------------------------------------- emit: data (.S)
def ins_size(func, ins):
    if func is None or func.mode == 'arm':
        return 4
    return 4 if ins[1] in ('bl', 'blx') and not ins[2].strip() in REGS else 2


def rename_expr(expr, rename):
    return IDENT_RE.sub(lambda m: rename.get(m.group(0), m.group(0)), expr)


def names_referenced(structured):
    """Identifiers used in the data directives of a file (to find function pointers)."""
    out = set()
    for e in structured:
        if e[0] == 'item' and e[1][0] == 'dir' and e[1][1] in ('.word', '.4byte', '.long'):
            out.update(IDENT_RE.findall(e[1][2]))
    return out


def emit_data(structured, rename, anchors, fname, ptr_rename=None):
    out = [f'# Generated by pc/tools/asm2c.py from {fname}. Do not edit.', '#ifdef __ELF__', '.section .note.GNU-stack,"",@progbits', '#endif']
    sec_names = {'text': '.section .rodata.asmtext', 'data': '.data', 'rodata': '.section .rodata', 'bss': '.bss'}
    skipping = False
    in_macro = False
    for entry in structured:
        kind = entry[0]
        if kind == 'section':
            skipping = entry[1].startswith('skip:')
            if not skipping:
                out.append(sec_names[entry[1]])
            continue
        if skipping:
            continue
        if kind == 'func_start':
            if entry[2] != 'non_word_aligned_thumb_func_start':
                out.append('.balign 4, 0')
            continue
        if kind == 'func_end':
            continue
        it, func, section = entry[1], entry[2], entry[3]
        if it[0] == 'label':
            name = it[1]
            if func is not None and name == func.name:
                out.append(f'__af_{name}:')
            else:
                sym = rename.get(name, name)
                out.append(f'.global {sym}\n{sym}:')
            continue
        if it[0] == 'dir':
            d, args = it[1], it[2]
            if d == '.macro':
                in_macro = True
                out.append(f'.macro {args}')
            elif d == '.endm':
                in_macro = False
                out.append('.endm')
            elif d in DROP_DIRECTIVES:
                pass
            elif d == '.align':
                out.append(f'.p2align {args}')
            elif d in DATA_DIRECTIVES:
                if d == '.incbin':
                    args = args  # paths were made absolute in prepass
                if d in ('.word', '.4byte', '.long') and ptr_rename:
                    args = rename_expr(args, ptr_rename)
                out.append(f'{DATA_DIRECTIVES[d]} {rename_expr(args, rename) if d not in (".ascii", ".asciz", ".string", ".incbin") else args}')
            elif d == '.previous':
                out.append('.previous')
            else:
                out.append(f'{d} {args}')  # let gas decide
            continue
        # instruction (or data macro invocation)
        if func is not None and section == 'text':
            for a in anchors.get(id(it), ()):
                out.append(f'.global {a}\n{a}:')
            out.append(f'.space {ins_size(func, it)}')
        else:
            out.append(f'{it[1]} {rename_expr(it[2], rename)}')  # macro invocation in data
    return '\n'.join(out) + '\n'


# ---------------------------------------------------------------- emit: code (.c)
def reg(tok):
    t = tok.strip().lower().rstrip('!')
    if t not in REGS:
        raise Unsupported(f'register {tok}')
    return REGS[t]


def imm(tok):
    t = tok.strip()
    if t.startswith('#'):
        t = t[1:]
    t = t.strip()
    if not re.fullmatch(r'[\w\s()+\-*/<>|&^~x]+', t):
        raise Unsupported(f'immediate {tok}')
    try:
        return int(eval(t.replace('/', '//'), {'__builtins__': {}}, {})) & 0xFFFFFFFF
    except Exception:
        raise Unsupported(f'immediate {tok}')


def is_imm(tok):
    return tok.strip().startswith('#')


def reglist(tok):
    t = tok.strip()
    if not (t.startswith('{') and t.endswith('}')):
        raise Unsupported(f'reglist {tok}')
    regs = []
    for part in t[1:-1].split(','):
        part = part.strip()
        if '-' in part:
            a, b = part.split('-')
            regs.extend(range(reg(a), reg(b) + 1))
        elif part:
            regs.append(reg(part))
    return sorted(set(regs))


def R(n):
    return 'SP' if n == 13 else f'R({n})'


def low(n):
    return n < 8


class FuncTranslator:
    def __init__(self, func, ctx, next_func):
        self.f, self.ctx, self.next_func = func, ctx, next_func
        self.labels = set(func.labels)
        self.lines = []
        self.anchor_n = 0

    def anchor(self, it):
        name = f'__a{self.ctx.fid}_pc{self.ctx.anchor_seq}'
        self.ctx.anchor_seq += 1
        self.ctx.anchors[id(it)].append(name)
        self.ctx.extern_data.add(name)
        return name

    def sym(self, name):
        s = self.ctx.rename.get(name, name)
        self.ctx.extern_data.add(s)
        return s

    def clabel(self, name):
        return 'L_' + re.sub(r'\W', '_', name)

    def jump(self, target, cond=None):
        """goto a local label, or tail-call another function."""
        pre = f'if ({cond}) ' if cond else ''
        if target in self.labels and target != self.f.name:
            return f'{pre}goto {self.clabel(target)};'
        if target == self.f.name:
            return f'{pre}goto L__entry;'
        thunk = self.ovl_thunk(target)
        if thunk:
            return f'{pre}{{ pc_callc({thunk}); return; }}'
        if target in self.ctx.asm_funcs:
            return f'{pre}{{ R(14) = PC_LR_RETURN; asm_{target}(); return; }}'
        if target in self.ctx.label_owner:
            return f'{pre}{{ {self.shared_tail(target)} }}'
        raise Unsupported(f'branch to {target}')

    def shared_tail(self, target):
        """Branch into another function: an epilogue shared by mwcc, or code the disassembler split
        off into a separate function. Inline it: straight-line code (calls allowed) up to a return."""
        f, i = self.ctx.label_owner[target]
        out = []
        for it in f.items[i + 1:]:
            if it[0] == 'label':
                continue
            if it[0] != 'ins':
                break
            if re.fullmatch(r'b[a-z]*', it[1]) and it[1] not in ('bl', 'blx', 'bic', 'bics') and not self.terminates(it):
                raise Unsupported(f'branch to {target}')
            out.append(self.ins(it))
            if self.terminates(it):
                return ' '.join(out)
        raise Unsupported(f'branch to {target}')

    def ovl_thunk(self, target):
        """Thunk name if `target` sits in RAM other overlays share and belongs to another overlay."""
        c = self.ctx
        if (target in c.ambig and c.sym_ovl.get(target) != c.file_ovl and target not in NARROW_RETURN
                and target not in STRUCT_RETURN and target not in CMP_HELPERS):
            c.used_thunks.add(target)
            c.extern_data.add('pc_ovlx_' + target)
            return 'pc_ovlx_' + target
        return None

    def call(self, target):
        thunk = self.ovl_thunk(target)
        if thunk:
            return f'pc_callc({thunk});'
        if target in CMP_HELPERS:
            # The DS compare helpers return their answer in the flags; the host ones return sign(a - b).
            self.ctx.extern_data.add(target)
            return (f'pc_callc({target}); z = R(0) == 0; c = (as32)R(0) >= 0; '
                    f'n = (as32)R(0) < 0; v = 0;')
        if target in self.ctx.asm_funcs:
            self.ctx.called_asm.add(target)
            return f'R(14) = PC_LR_RETURN; asm_{target}(); PC_TRACE_OUT("{target}");'
        if target in self.labels:
            # mwcc uses `bl` as a long in-function branch (Thumb `b` reaches only 2 KB). Every target
            # in the game returns via `pop {..., pc}` or makes its own calls, so the LR it sets is dead.
            return f'goto {self.clabel(target)};'
        self.ctx.extern_data.add(target)
        if target in STRUCT_RETURN:
            return f'pc_callc_struct({target}, {STRUCT_RETURN[target]});'
        if target in NARROW_RETURN:
            return f'pc_callc({target}); R(0) = (au32)({NARROW_RETURN[target]})R(0);'
        return f'pc_callc({target});'

    def mem_operand(self, tok):
        t = tok.strip()
        if not (t.startswith('[') and t.endswith(']')):
            raise Unsupported(f'memory operand {tok}')
        parts = split_operands(t[1:-1])
        base = reg(parts[0])
        if base == 15:
            raise Unsupported('pc-relative bracket operand')
        if len(parts) == 1:
            return R(base)
        if is_imm(parts[1]):
            v = imm(parts[1])
            return f'{R(base)} + {v}u' if v else R(base)
        return f'{R(base)} + {R(reg(parts[1]))}'

    def translate(self):
        out = self.lines
        for it in self.f.items:
            if it[0] == 'label':
                if it[1] != self.f.name:
                    out.append(f'{self.clabel(it[1])}:;')
                continue
            if it[0] == 'dir':
                continue  # inline data lives in the .S image
            out.append(f'    {self.ins(it)}  /* {it[3]} */')
        last = next((i for i in reversed(self.f.items) if i[0] == 'ins'), None)
        if last is None or not self.terminates(last):
            if self.next_func is not None:
                out.append(f'    asm_{self.next_func}(); return; /* falls through */')
            else:
                out.append('    return;')
        return out

    @staticmethod
    def terminates(it):
        m, ops = it[1], split_operands(it[2])
        return m == 'b' or (m == 'bx') or (m == 'pop' and 'pc' in it[2]) or (m == 'mov' and ops and ops[0] == 'pc')

    def ins(self, it):
        m, ops = it[1], split_operands(it[2])
        if m == 'nop':
            return ';'
        # ---- branches
        if m == 'b':
            return self.jump(ops[0])
        if m.startswith('b') and m[1:] in CONDS and len(ops) == 1 and not m.startswith('bl'):
            return self.jump(ops[0], CONDS[m[1:]])
        if m in ('ble', 'blt', 'bls', 'blo') and len(ops) == 1 and ops[0] not in REGS:
            return self.jump(ops[0], CONDS[m[1:]])
        if m == 'bl':
            return self.call(ops[0])
        if m == 'blx':
            if ops[0] in REGS:
                return f'pc_callc((void *)(uintptr_t){R(reg(ops[0]))});'
            return self.call(ops[0])
        if m == 'bx':
            r = reg(ops[0])
            if r == 14:
                return 'return;'
            return f'if ({R(r)} == PC_LR_RETURN) return; pc_callc((void *)(uintptr_t){R(r)}); return;'
        # ---- stack and multiple
        if m == 'push':
            regs = reglist(ops[0])
            s = [f'SP -= {4 * len(regs)};'] + [f'M32(SP + {4 * i}) = {R(r)};' for i, r in enumerate(regs)]
            return ' '.join(s)
        if m == 'pop':
            regs = reglist(ops[0])
            s = [f'{R(r)} = M32(SP + {4 * i});' for i, r in enumerate(regs) if r != 15]
            s.append(f'SP += {4 * len(regs)};')
            if 15 in regs:
                s.append('return;')
            return ' '.join(s)
        if m in ('stmia', 'ldmia', 'stm', 'ldm'):
            base, regs = reg(ops[0]), reglist(ops[1])
            wb = ops[0].strip().endswith('!')
            s = [f'{{ au32 _b = {R(base)};']
            for i, r in enumerate(regs):
                s.append(f'ST32(_b + {4 * i}, {R(r)});' if m.startswith('st') else f'{R(r)} = M32(_b + {4 * i});')
            if wb and not (m.startswith('ld') and base in regs):
                s.append(f'{R(base)} = _b + {4 * len(regs)};')
            s.append('}')
            return ' '.join(s)
        # ---- loads / stores
        if m in ('ldr', 'ldrb', 'ldrh', 'ldrsb', 'ldrsh', 'str', 'strb', 'strh'):
            rd = reg(ops[0])
            acc = {'ldr': 'M32', 'str': 'M32', 'ldrb': 'M8', 'strb': 'M8', 'ldrh': 'M16', 'strh': 'M16',
                   'ldrsb': 'MS8', 'ldrsh': 'MS16'}[m]
            src = ops[1].strip()
            if src.startswith('['):
                parts = split_operands(src[1:-1])
                if reg(parts[0]) == 15:
                    a = self.anchor(it)
                    addr = f'((ADDR({a}) + 4) & ~3u) + {imm(parts[1]) if len(parts) > 1 else 0}u'
                else:
                    addr = self.mem_operand(src)
            elif src.startswith('='):
                raise Unsupported('ldr =')
            else:  # pc-relative literal load by label
                addr = f'ADDR({self.sym(src)})'
            if m.startswith('ld'):
                return f'{R(rd)} = (au32){acc}({addr});'
            return f'ST{acc[1:]}({addr}, {R(rd)});'  # ST8/ST16/ST32 route IO-page stores
        # ---- data processing
        return self.alu(m, ops, it)

    def alu(self, m, ops, it):
        if m in ('mov', 'movs'):
            rd = reg(ops[0])
            if is_imm(ops[1]):
                return f'LOGIC({R(rd)}, {imm(ops[1])}u);'
            rs = reg(ops[1])
            if rd == 15:
                if rs == 14:
                    return 'return;'
                raise Unsupported('mov pc')
            if low(rd) and low(rs):
                return f'LOGIC({R(rd)}, {R(rs)});'
            return f'{R(rd)} = {R(rs)};'
        if m in ('add', 'adds', 'sub', 'subs'):
            add = m.startswith('add')
            rd = reg(ops[0])
            if rd == 15:
                if not add:
                    raise Unsupported('sub pc')
                return self.computed_jump(it, reg(ops[1]))
            if len(ops) == 2:
                if is_imm(ops[1]):
                    v = imm(ops[1])
                    if rd == 13:
                        return f'SP {"+" if add else "-"}= {v}u;'
                    return f'{"ADDS" if add else "SUBS"}({R(rd)}, {R(rd)}, {v}u);'
                rs = reg(ops[1])
                if rs == 15:
                    a = self.anchor(it)
                    return f'{R(rd)} {"+" if add else "-"}= ADDR({a}) + 4;'
                if low(rd) and low(rs):
                    return f'{"ADDS" if add else "SUBS"}({R(rd)}, {R(rd)}, {R(rs)});'
                return f'{R(rd)} {"+" if add else "-"}= {R(rs)};'
            rs = reg(ops[1])
            if rs == 15:
                a = self.anchor(it)
                return f'{R(rd)} = ((ADDR({a}) + 4) & ~3u) {"+" if add else "-"} {imm(ops[2])}u;'
            if rs == 13 or rd == 13:
                rhs = f'{imm(ops[2])}u' if is_imm(ops[2]) else R(reg(ops[2]))
                return f'{R(rd)} = {R(rs)} {"+" if add else "-"} {rhs};'
            rhs = f'{imm(ops[2])}u' if is_imm(ops[2]) else R(reg(ops[2]))
            return f'{"ADDS" if add else "SUBS"}({R(rd)}, {R(rs)}, {rhs});'
        if m in ('cmp', 'cmn'):
            rn = R(reg(ops[0]))
            rhs = f'{imm(ops[1])}u' if is_imm(ops[1]) else R(reg(ops[1]))
            return f'{"CMP" if m == "cmp" else "CMN"}({rn}, {rhs});'
        if m == 'tst':
            return f'TST({R(reg(ops[0]))} & {R(reg(ops[1]))});'
        two = {'and': '&', 'orr': '|', 'eor': '^', 'ands': '&', 'orrs': '|', 'eors': '^'}
        if m in two:
            rd = reg(ops[0])
            a, b = (R(rd), R(reg(ops[1]))) if len(ops) == 2 else (R(reg(ops[1])), R(reg(ops[2])))
            return f'LOGIC({R(rd)}, {a} {two[m]} {b});'
        if m in ('bic', 'bics'):
            rd = reg(ops[0])
            return f'LOGIC({R(rd)}, {R(rd)} & ~{R(reg(ops[-1]))});'
        if m in ('mvn', 'mvns'):
            return f'LOGIC({R(reg(ops[0]))}, ~{R(reg(ops[1]))});'
        if m in ('neg', 'negs'):
            return f'SUBS({R(reg(ops[0]))}, 0u, {R(reg(ops[1]))});'
        if m in ('mul', 'muls'):
            rd = reg(ops[0])
            a, b = (R(rd), R(reg(ops[1]))) if len(ops) == 2 else (R(reg(ops[1])), R(reg(ops[2])))
            return f'LOGIC({R(rd)}, {a} * {b});'
        if m in ('adc', 'adcs', 'sbc', 'sbcs'):
            rd = reg(ops[0])
            return f'{"ADCS" if m.startswith("adc") else "SBCS"}({R(rd)}, {R(rd)}, {R(reg(ops[-1]))});'
        if m in ('lsl', 'lsr', 'asr', 'ror', 'lsls', 'lsrs', 'asrs', 'rors'):
            op = m[:3].upper()
            rd = reg(ops[0])
            if len(ops) == 3:
                if not is_imm(ops[2]):
                    raise Unsupported('3-operand register shift')
                return f'{op}_I({R(rd)}, {R(reg(ops[1]))}, {imm(ops[2])});'
            if is_imm(ops[1]):
                return f'{op}_I({R(rd)}, {R(rd)}, {imm(ops[1])});'
            return f'{op}_R({R(rd)}, {R(rd)}, {R(reg(ops[1]))});'
        raise Unsupported(f'instruction {m}')

    def computed_jump(self, it, rs):
        """add pc, rX: jump to (address of this instruction + 4) + rX, matched against local labels."""
        a = self.anchor(it)
        s = [f'{{ au32 _t = ADDR({a}) + 4 + {R(rs)};']
        for lbl in self.f.labels:
            if lbl != self.f.name:
                s.append(f'if (_t == ADDR({self.sym(lbl)})) goto {self.clabel(lbl)};')
        s.append(f'pc_asm_unsupported("{self.f.name}", "computed jump to unknown target"); return; }}')
        return ' '.join(s)


class ArmTranslator(FuncTranslator):
    """ARM-mode functions (pre-UAL syntax: `ldmeqia`, `streqb`). Every instruction may be conditional,
    only S-suffixed ops and compares set flags, operand 2 goes through the barrel shifter, and pc
    reads as the instruction's address + 8."""

    def ins(self, it):
        base, cond, suf = split_arm_mnemonic(it[1])
        ops = split_operands(it[2])
        if base == 'b':
            return self.jump(ops[0], CONDS[cond] if cond else None)
        s = self.arm_body(it, base, suf, ops)
        return f'if ({CONDS[cond]}) {{ {s} }}' if cond else s

    def terminates(self, it):
        base, cond, suf = split_arm_mnemonic(it[1])
        ops = split_operands(it[2])
        if cond:
            return False
        return (base == 'b' or base == 'bx' or (base == 'ldm' and 15 in reglist(ops[1]))
                or (base == 'mov' and reg(ops[0]) == 15))

    def rv(self, tok, it):
        r = reg(tok)
        return f'(ADDR({self.anchor(it)}) + 8)' if r == 15 else R(r)

    def op2(self, toks, it):
        """Shifter operand -> (C expression, rotated-immediate carry or None)."""
        if len(toks) == 1:
            if is_imm(toks[0]):
                v = imm(toks[0])
                return f'{v}u', (f'{v >> 31}u' if v > 0xFF else None)
            return self.rv(toks[0], it), None
        if len(toks) != 2:
            raise Unsupported(f'operand {",".join(toks)}')
        rm = self.rv(toks[0], it)
        kind, _, amt = toks[1].strip().partition(' ')
        kind, amt = kind.lower(), amt.strip()
        if kind not in ('lsl', 'lsr', 'asr', 'ror'):
            raise Unsupported(f'shift {toks[1]}')
        if not is_imm(amt):
            return f'pc_{kind}v({rm}, {R(reg(amt))})', None
        n = imm(amt)
        if kind == 'lsl':
            return (f'({rm} << {n})' if n else rm), None
        if kind == 'lsr':
            return (f'({rm} >> {n})' if 0 < n < 32 else '0u'), None
        if kind == 'asr':
            return f'(au32)((as32){rm} >> {n if 0 < n < 32 else 31})', None
        if not 0 < n < 32:
            raise Unsupported('rrx')
        return f'pc_rorv({rm}, {n})', None

    def shifted_flags(self, toks, it):
        """Shifted register operand of a flag-setting logical op -> code that leaves it in _sh, with
        the shifter carry in c (the Thumb shift macros have exactly ARM's carry rules)."""
        rm = self.rv(toks[0], it)
        kind, _, amt = toks[1].strip().partition(' ')
        kind, amt = kind.upper(), amt.strip()
        if kind not in ('LSL', 'LSR', 'ASR', 'ROR'):
            raise Unsupported(f'shift {toks[1]}')
        if not is_imm(amt):
            return f'au32 _sh; {kind}_R(_sh, {rm}, {R(reg(amt))});'
        if kind == 'ROR':
            return f'au32 _sh; ROR_R(_sh, {rm}, {imm(amt)}u);'
        return f'au32 _sh; {kind}_I(_sh, {rm}, {imm(amt)});'

    def jump_table(self, it, ops):
        """`addls pc, pc, rX, lsl #2`, then the default and one entry per index, each a `b` or `bx`
        (pc reads as here + 8, so index 0 is the second instruction after this one)."""
        if [o.strip() for o in ops[:2]] != ['pc', 'pc'] or len(ops) != 4 or ops[3].strip() != 'lsl #2':
            raise Unsupported('add to pc')
        items = self.f.items
        i = next(k for k, x in enumerate(items) if x is it)
        entries = []
        for x in items[i + 1:]:
            if x[0] == 'label':
                continue
            if x[0] != 'ins' or x[1] not in ('b', 'bx'):
                break
            entries.append(x)
        if len(entries) < 2:
            raise Unsupported('add to pc')
        cases = ' '.join(f'case {k}: {self.ins(x)}' for k, x in enumerate(entries[1:]))
        return f'switch ({R(reg(ops[2]))}) {{ {cases} default: break; }}'

    def arm_body(self, it, base, suf, ops):
        S = suf == 's'
        if base == 'add' and reg(ops[0]) == 15:
            return self.jump_table(it, ops)
        if base == 'bl':
            return self.call(ops[0])
        if base in ('bx', 'blx'):
            return FuncTranslator.ins(self, ('ins', base, it[2], it[3]))
        if base in ('ldm', 'stm'):
            return self.ldm_stm(base, suf, ops)
        if base in ('ldr', 'str'):
            return self.load_store(it, base, suf, ops)
        if base in MUL_OPS:
            return self.multiply(base, S, ops)
        if base in ('cmp', 'cmn', 'tst', 'teq'):
            a = self.rv(ops[0], it)
            b, rc = self.op2(ops[1:], it)
            if base in ('cmp', 'cmn'):
                return f'{base.upper()}({a}, {b});'
            op = "&" if base == "tst" else "^"
            if len(ops) > 2:
                return f'{{ {self.shifted_flags(ops[1:], it)} TST({a} {op} _sh); }}'
            pre = f'c = {rc}; ' if rc else ''
            return f'{pre}TST({a} {op} {b});'
        rd = reg(ops[0])
        if base in ('mov', 'mvn'):
            b, rc = self.op2(ops[1:], it)
            if rd == 15:
                if base == 'mov' and ops[1].strip() == 'lr':
                    return 'return;'
                raise Unsupported('mov pc')
            v = b if base == 'mov' else f'~{b}'
        else:
            if rd == 15:
                raise Unsupported(f'{base} to pc')
            a = self.rv(ops[1], it)
            b, rc = self.op2(ops[2:], it)
            arith = {'add': ('ADDS', a, b, '{a} + {b}'), 'sub': ('SUBS', a, b, '{a} - {b}'),
                     'rsb': ('SUBS', b, a, '{a} - {b}'), 'adc': ('ADCS', a, b, '{a} + {b} + c'),
                     'sbc': ('SBCS', a, b, '{a} - {b} - !c'), 'rsc': ('SBCS', b, a, '{a} - {b} - !c')}
            if base in arith:
                mac, x, y, plain = arith[base]
                if S:
                    return f'{mac}({R(rd)}, {x}, {y});'
                return f'{R(rd)} = {plain.format(a=x, b=y)};'
            logic = {'and': '{a} & {b}', 'orr': '{a} | {b}', 'eor': '{a} ^ {b}', 'bic': '{a} & ~{b}'}[base]
            v = logic.format(a=a, b=b)
            if S and len(ops) > 3:
                return f'{{ {self.shifted_flags(ops[2:], it)} LOGIC({R(rd)}, {logic.format(a=a, b="_sh")}); }}'
        if not S:
            return f'{R(rd)} = {v};'
        if base in ('mov', 'mvn') and len(ops) > 2:
            return f'{{ {self.shifted_flags(ops[1:], it)} LOGIC({R(rd)}, {"" if base == "mov" else "~"}_sh); }}'
        return f'{"c = " + rc + "; " if rc else ""}LOGIC({R(rd)}, {v});'

    def multiply(self, base, S, ops):
        if S:
            raise Unsupported(f'{base}s')
        r = [R(reg(o)) for o in ops]
        if base == 'mul':
            return f'{r[0]} = {r[1]} * {r[2]};'
        if base == 'mla':
            return f'{r[0]} = {r[1]} * {r[2]} + {r[3]};'
        wide = '(au64)' if base[0] == 'u' else '(au64)(int64_t)(as32)'
        acc = f' + (((au64){r[1]} << 32) | {r[0]})' if base.endswith('lal') else ''
        return f'{{ au64 _m = {wide}{r[2]} * {wide}{r[3]}{acc}; {r[0]} = (au32)_m; {r[1]} = (au32)(_m >> 32); }}'

    def offset(self, toks):
        neg = toks[0].strip().startswith('-')
        if is_imm(toks[0]):
            return f'{imm(toks[0])}u'
        first = toks[0].strip().lstrip('-+')
        e, _ = self.op2([first] + toks[1:], None)
        return f'-{e}' if neg else e

    def load_store(self, it, base, suf, ops):
        rd = reg(ops[0])
        load = base == 'ldr'
        acc = {'': 'M32', 'b': 'M8', 'h': 'M16', 'sb': 'MS8', 'sh': 'MS16'}[suf]
        if rd == 15 or (not load and suf in ('sb', 'sh')):
            raise Unsupported(f'{it[1]} {it[2]}')
        wb = None
        if not ops[1].startswith('['):  # pc-relative literal by label
            addr = f'ADDR({self.sym(ops[1])})'
        else:
            inner = ops[1].rstrip('!').strip()
            parts = split_operands(inner[1:-1])
            rn = reg(parts[0])
            bv = self.rv(parts[0], it)
            if len(ops) > 2:  # post-indexed: [rn], offset
                addr = bv
                wb = f'{R(rn)} = {bv} + {self.offset(ops[2:])};'
            else:
                addr = f'{bv} + {self.offset(parts[1:])}' if len(parts) > 1 else bv
                if ops[1].endswith('!'):
                    wb = f'{R(rn)} = _ea;'
        if load:
            return f'{{ au32 _ea = {addr}; au32 _v = (au32){acc}(_ea); {wb or ""} {R(rd)} = _v; }}'
        return f'{{ au32 _ea = {addr}; ST{acc[1:]}(_ea, {R(rd)}); {wb or ""} }}'

    def ldm_stm(self, base, mode, ops):
        mode = {'': 'ia', 'fd': 'ia' if base == 'ldm' else 'db', 'ed': 'ib' if base == 'ldm' else 'da',
                'fa': 'da' if base == 'ldm' else 'ib', 'ea': 'db' if base == 'ldm' else 'ia'}.get(mode, mode)
        rn, regs = reg(ops[0]), reglist(ops[1])
        wb = ops[0].strip().endswith('!')
        n = 4 * len(regs)
        start = {'ia': '_b', 'ib': '_b + 4', 'da': f'_b - {n - 4}', 'db': f'_b - {n}'}[mode]
        s = [f'{{ au32 _b = {R(rn)}, _s = {start};']
        for i, r in enumerate(regs):
            if base == 'stm':
                if r == 15:
                    raise Unsupported('stm pc')
                s.append(f'ST32(_s + {4 * i}, {R(r)});')
            elif r != 15:
                s.append(f'{R(r)} = M32(_s + {4 * i});')
        if wb and not (base == 'ldm' and rn in regs):
            s.append(f'{R(rn)} = _b {"+" if mode in ("ia", "ib") else "-"} {n};')
        if base == 'ldm' and 15 in regs:
            s.append('return;')
        s.append('}')
        return ' '.join(s)


DP_OPS = {'mov', 'mvn', 'add', 'adc', 'sub', 'sbc', 'rsb', 'rsc', 'and', 'orr', 'eor', 'bic', 'cmp', 'cmn', 'tst', 'teq'}
MUL_OPS = {'mul', 'mla', 'umull', 'smull', 'umlal', 'smlal'}
ARM_BASES = sorted(DP_OPS | MUL_OPS | {'ldm', 'stm', 'ldr', 'str', 'bx', 'blx', 'bl', 'b'}, key=len, reverse=True)


def split_arm_mnemonic(m):
    """'ldmeqia' -> ('ldm', 'eq', 'ia'); 'bls' -> ('b', 'ls', ''); 'ldrhs' -> ('ldr', 'hs', '')."""
    for base in ARM_BASES:
        if not m.startswith(base):
            continue
        rest = m[len(base):]
        tries = [(rest[:2], rest[2:]), ('', rest)] if rest[:2] in CONDS or rest[:2] == 'al' else [('', rest)]
        for cond, suf in tries:
            if base in DP_OPS or base in MUL_OPS:
                ok = suf in ('', 's') and not (suf == 's' and base in ('cmp', 'cmn', 'tst', 'teq'))
            elif base in ('ldr', 'str'):
                ok = suf in ('', 'b', 'h', 'sb', 'sh')
            elif base in ('ldm', 'stm'):
                ok = suf in ('', 'ia', 'ib', 'da', 'db', 'fd', 'ed', 'fa', 'ea')
            else:
                ok = suf == ''
            if ok:
                return base, '' if cond == 'al' else cond, suf
    raise Unsupported(f'ARM instruction {m}')


class FileCtx:
    def __init__(self, fid, rename, asm_funcs, ovl=None):
        self.fid, self.rename, self.asm_funcs = fid, rename, asm_funcs
        self.file_ovl, self.sym_ovl, self.ambig, self.used_thunks = ovl or (None, {}, {}, set())
        self.anchors = collections.defaultdict(list)
        self.anchor_seq = 0
        self.extern_data = set()
        self.called_asm = set()


def translate_file(fid, rel, structured, funcs, rename, asm_funcs, stats, ovl=None):
    ctx = FileCtx(fid, rename, asm_funcs, ovl)
    ctx.label_owner = {it[1]: (f, i) for f in funcs for i, it in enumerate(f.items) if it[0] == 'label' and it[1] != f.name}
    bodies = []
    for i, f in enumerate(funcs):
        nxt = funcs[i + 1].name if i + 1 < len(funcs) else None
        try:
            body = (ArmTranslator if f.mode == 'arm' else FuncTranslator)(f, ctx, nxt).translate()
            stats['translated'] += 1
            stats['arm'] += f.mode == 'arm'
        except Unsupported as e:
            body = [f'    pc_asm_unsupported("{f.name}", "{e}");']
            stats['unsupported'] += 1
            stats['unsupported_reasons'][re.sub(r'\b(_?[0-9A-F]{8}|\w+_[0-9A-F]{8}|r\d+)\b', 'X', str(e))] += 1
        bodies.append((f, body))
    c = [f'/* Generated by pc/tools/asm2c.py from {rel}. Do not edit. */', '#include "asm_rt.h"', '']
    for s in sorted(ctx.extern_data):
        if s not in asm_funcs or s in rename.values():
            c.append(f'extern au8 {s}[];')
    for s in sorted(ctx.called_asm | {f.name for f in funcs}):
        c.append(f'void asm_{s}(void);')
    c.append('')
    for f, body in bodies:
        fix = ' '.join(f'{arg} = ({t}){arg};' for arg, t in NARROW_PARAMS.get(f.name, ()))
        c.append(f'ASM_EXPORT_FIX({f.name}, {fix})' if fix else f'ASM_EXPORT({f.name})')
        c.append(f'void asm_{f.name}(void) {{')
        c.append('    au32 n = 0, z = 0, c = 0, v = 0; (void)n; (void)z; (void)c; (void)v;')
        c.append(f'    PC_TRACE_IN("{f.name}");')
        c.append('L__entry:;')
        c.extend(body)
        c.append('}')
        c.append('')
    return '\n'.join(c) + '\n', ctx.anchors


def main():
    envs = asm_envs()
    asm_dir = os.path.join(ROOT, 'asm')
    # (source path relative to ROOT, output base name); lib files get a lib_ prefix
    files = [('asm/' + f, f[:-2]) for f in sorted(os.listdir(asm_dir)) if f.endswith('.s')]
    files += [(rel, 'lib_' + os.path.basename(rel)[:-2]) for rel in LIB_ASM]
    parsed = {}
    for rel, _ in files:
        path = os.path.join(ROOT, rel)
        cwd, flags = envs.get(path) or next(v for k, v in envs.items() if os.path.dirname(k) == asm_dir)
        inc_dirs = [f[2:] for f in flags if f.startswith('-I')]
        lines = read_source(path, cwd, flags, inc_dirs + [os.path.join(os.path.dirname(path), 'include'), os.path.dirname(path)])
        # .incbin paths are relative to the DS build dir; make them absolute
        lines = [re.sub(r'(\.incbin\s+")([^"/][^"]*)"', lambda m: m.group(1) + os.path.normpath(os.path.join(cwd, m.group(2))) + '"', l) for l in lines]
        parsed[rel] = structure(parse(lines))

    # labels defined in more than one file (e.g. same address in two overlays) get per-file names
    defined = collections.Counter()
    asm_funcs = set()
    for f, (structured, funcs) in parsed.items():
        names = {e[1][1] for e in structured if e[0] == 'item' and e[1][0] == 'label'}
        defined.update(names)
        asm_funcs.update(fn.name for fn in funcs)

    scan_narrow_returns()
    scan_narrow_params(asm_funcs)
    obj_ovl, sym_ovl, ambig = overlay_tables()
    used_thunks = set()
    os.makedirs(OUT, exist_ok=True)
    stats = {'translated': 0, 'unsupported': 0, 'arm': 0, 'unsupported_reasons': collections.Counter()}
    for fid, (rel, outname) in enumerate(files):
        structured, funcs = parsed[rel]
        names = {e[1][1] for e in structured if e[0] == 'item' and e[1][0] == 'label'}
        fnames = {fn.name for fn in funcs}
        rename = {n: f'__a{fid}_{n}' for n in names if defined[n] > 1 and n not in fnames}
        file_ovl = obj_ovl.get('asm_' + os.path.basename(rel) + '.o') if rel.startswith('asm/') else None
        c_text, anchors = translate_file(fid, rel, structured, funcs, rename, asm_funcs, stats,
                                         (file_ovl, sym_ovl, ambig, used_thunks))
        ptrs = {n: 'pc_ovlx_' + n for n in names_referenced(structured)
                if n in ambig and sym_ovl.get(n) != file_ovl and n not in NARROW_RETURN}
        used_thunks.update(ptrs)
        base = os.path.join(OUT, outname)
        open(base + '.c', 'w').write(c_text)
        open(base + '.S', 'w').write(emit_data(structured, rename, anchors, rel, ptrs))
    ids_of = {n: c for cands in ambig.values() for n, c in cands}
    used_thunks &= set(ids_of)
    open(os.path.join(OUT, 'ovl_dispatch.c'), 'w').write(dispatch_c(used_thunks, ambig, sym_ovl, ids_of))
    stats['ovl_thunks'] = len(used_thunks)
    reasons = stats.pop('unsupported_reasons')
    print(f"asm2c: {len(files)} files, {stats['translated']} functions translated ({stats['arm']} ARM), "
          f"{stats['unsupported']} unsupported, {stats['ovl_thunks']} shared-overlay call thunks")
    for why, n in reasons.most_common(15):
        print(f'  {n:6}  {why}')


if __name__ == '__main__':
    main()
