"""Heuristic BARR-C:2018 scanner (approximate: regex-based, expect some false positives).
Usage, from the repo root: python tools/barrc_scan.py . barrc_scan.json
Counts likely violations of the mechanically checkable rules, per file; vendored code is skipped."""
import json, os, re, subprocess, sys
from collections import defaultdict

repo, out_path = sys.argv[1], sys.argv[2]
files = subprocess.run(['git', '-C', repo, 'ls-files', '*.c', '*.h'], capture_output=True, text=True).stdout.split()
VENDOR = ('third_party/mtk3smp_rp2040/', 'third_party/lora_mesh/src/lr1121/')
files = [f for f in files if not f.startswith(VENDOR)]

def strip(src):
    """Blank out comments and string/char literals, keep newlines. Return (code, comments_by_line)."""
    out = []; com = defaultdict(str); i = 0; n = len(src); line = 1
    while i < n:
        c = src[i]
        if src.startswith('//', i):
            j = src.find('\n', i); j = n if j < 0 else j
            com[line] += src[i:j]; out.append(' ' * (j - i)); i = j; continue
        if src.startswith('/*', i):
            j = src.find('*/', i + 2); j = n if j < 0 else j + 2
            seg = src[i:j]
            for k, part in enumerate(seg.split('\n')):
                com[line + k] += part
            out.append(re.sub(r'[^\n]', ' ', seg)); line += seg.count('\n'); i = j; continue
        if c in '"\'':
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == '\\' else 1
            j += 1; out.append(c + ' ' * (j - i - 2) + c); i = j; continue
        if c == '\n': line += 1
        out.append(c); i += 1
    return ''.join(out), com

KW_CTL = re.compile(r'\b(if|for|while|switch)\s*\(')
res = defaultdict(lambda: defaultdict(list))
func_lengths = []
basenames = defaultdict(list)

def hit(rule, f, ln, txt=''):
    res[rule][f].append((ln, txt.strip()[:100]))

for f in files:
    p = os.path.join(repo, f)
    raw = open(p, 'rb').read()
    src = raw.decode('utf-8', 'replace')
    lines = src.split('\n')
    code, com = strip(src.replace('\r', ''))
    cl = code.split('\n')
    base = os.path.basename(f)
    basenames[base[:8].rsplit('.', 1)[0] if '.' in base[:8] else base[:8]].append(f)

    # 3.6.a CRLF, 3.5.a tabs, 1.2.a width
    # 3.6.a: use 'git ls-files --eol' instead; a Windows checkout turns LF into CRLF on disk.
    for i, l in enumerate(lines, 1):
        l = l.rstrip('\r')
        if '\t' in l: hit('3.5.a tab character', f, i, l)
        if len(l) > 80: hit('1.2.a line > 80 chars', f, i, l)
    # 4.1.a file names
    if re.search(r'[^a-z0-9_.]', base): hit('4.1.a module name not lowercase', f, 0, base)
    # 3.3.c end-of-file comment
    tail = [l for l in lines if l.strip()]
    if tail and not re.search(r'(/\*|//).*\b(end|eof)\b', tail[-1], re.I) and not (tail[-1].startswith('#endif') and '/*' in tail[-1] and 'end' in tail[-1].lower()):
        hit('3.3.c no end-of-file comment', f, len(lines), tail[-1])
    # 4.2.b include guard
    if f.endswith('.h'):
        if not re.search(r'^\s*#\s*ifndef\s+\w+\s*\n\s*#\s*define', code, re.M) and '#pragma once' not in src:
            hit('4.2.b header lacks include guard', f, 0)
        if re.search(r'^(?!\s*(extern|typedef|#|static\s+inline|static\s+const))[^\n(]*\b\w+\s*(\[[^\]]*\])?\s*=\s*[^=]', code, re.M):
            pass
    # 4.3.c .c includes own .h
    if f.endswith('.c'):
        stem = base[:-2]
        has_h = (stem + '.h') in [os.path.basename(x) for x in files]
        if has_h and not re.search(r'#\s*include\s*"([^"]*/)?' + re.escape(stem) + r'\.h"', src):
            hit('4.3.c .c does not include its own .h', f, 0)
        if not has_h and 'main' not in stem:
            hit('4.2.a .c without matching .h', f, 0)
        if re.search(r'\bmain\s*\(', code) and re.search(r'^\s*(int|void)\s+main\s*\(', code, re.M) and 'main' not in stem:
            hit('4.1.d main() in file not named *main*', f, 0)
    if re.search(r'#\s*include\s*"[^"]*\.c"', src): hit('4.3.f #include of a .c file', f, 0)

    in_func = None; depth = 0
    for i, l in enumerate(cl, 1):
        s = l.strip()
        if not s: continue
        # 1.3.b brace on same line as statement (K&R)
        if s.endswith('{') and s != '{' and not s.startswith('#') and not re.search(r'=\s*\{$', s):
            hit('1.3.b "{" not on its own line', f, i, lines[i-1])
        if re.match(r'^\}\s*else\b', s): hit('1.3.b "}" not on its own line', f, i, lines[i-1])
        # 1.3.a braceless control bodies
        m = re.match(r'^(\}\s*)?(else\s+)?(if|for|while)\s*\(', s)
        if m:
            # find matching paren
            k = s.index('(', m.end() - 1); d = 0; e = None
            for q in range(k, len(s)):
                if s[q] == '(': d += 1
                elif s[q] == ')':
                    d -= 1
                    if d == 0: e = q; break
            if e is not None:
                rest = s[e+1:].strip()
                if rest and rest not in ('{',) and not rest.startswith('{') and rest != ';':
                    hit('1.3.a braceless if/for/while body', f, i, lines[i-1])
                elif rest == '' :
                    nxt = next((cl[j].strip() for j in range(i, min(i+3, len(cl))) if cl[j].strip()), '')
                    if nxt and not nxt.startswith('{') and not nxt.startswith('&&') and not nxt.startswith('||') and m.group(3) != 'while' and not nxt.startswith(')') and not re.match(r'^[\w\s\->\.\[\]\(\)!&|<>=+*/%-]*\)\s*\{?$', nxt):
                        hit('1.3.a braceless if/for/while body', f, i, lines[i-1])
                elif rest == ';' and m.group(3) == 'while' and not s.startswith('}'):
                    hit('8.4.d empty loop body without braces+comment', f, i, lines[i-1])
        if re.match(r'^(\}\s*)?else\s*$', s):
            nxt = next((cl[j].strip() for j in range(i, min(i+3, len(cl))) if cl[j].strip()), '')
            if nxt and not nxt.startswith('{') and not nxt.startswith('if'):
                hit('1.3.a braceless if/for/while body', f, i, lines[i-1])
        if re.match(r'^(\}\s*)?else\s+(?!if\b)[^{\s]', s): hit('1.3.a braceless if/for/while body', f, i, lines[i-1])
        # keywords
        if re.search(r'\bgoto\b', l): hit('1.7.c goto used', f, i, lines[i-1])
        if re.search(r'\bcontinue\s*;', l): hit('1.7.d continue used (preferred: avoid)', f, i, lines[i-1])
        if re.search(r'\b(auto|register)\b\s+\w', l): hit('1.7.a/b auto/register keyword', f, i, lines[i-1])
        if re.search(r'\b(abort|exit|setjmp|longjmp)\s*\(', l) and not re.search(r'\w(abort|exit)\s*\(', l): hit('8.5.b abort/exit/setjmp/longjmp', f, i, lines[i-1])
        # 5.2.b short / long, 5.2.a plain int types
        if re.search(r'\b(short|long)\b', l) and not s.startswith('#'): hit('5.2.b short/long keyword', f, i, lines[i-1])
        if re.search(r'(^|[(,;{]\s*|\b(static|const|volatile|extern)\s+)(unsigned\s+|signed\s+)?int\b(?!\w)', l) and not s.startswith('#'):
            hit('5.2.a plain int/unsigned (check width matters)', f, i, lines[i-1])
        if re.search(r'\bunsigned\s*(\)|\s+\w+\s*[;=,)])', l) and not re.search(r'unsigned\s+(int|char|short|long)', l): hit('5.2.a plain int/unsigned (check width matters)', f, i, lines[i-1])
        if re.search(r'(^|[(,;{]\s*|\b(static|const|volatile)\s+)(unsigned\s+|signed\s+)?char\b(?!\s*\*|\s+\w+\s*\[)', l) and not s.startswith('#'):
            hit('5.2.c char used for non-string', f, i, lines[i-1])
        # 5.4 floats
        if re.search(r'\b(float|double)\b', l) and not s.startswith('#'): hit('5.4.b.i float/double instead of float32_t/float64_t', f, i, lines[i-1])
        for mm in re.finditer(r'(?<![\w.])(\d+\.\d*|\.\d+)([eE][-+]?\d+)?(?![fFlL\w])', l):
            hit('5.4.b.ii float constant without f suffix', f, i, lines[i-1]); break
        # 8.6.a constant on right of ==
        for mm in re.finditer(r'([\w\)\]]+)\s*([=!]=)\s*(-?\b(0x[0-9a-fA-F]+|\d+)[uUlL]*\b|NULL\b|[A-Z][A-Z0-9_]{2,}\b|true\b|false\b)', l):
            left = mm.group(1)
            if not re.match(r'^(-?\d|0x|NULL|true|false|[A-Z][A-Z0-9_]+$)', left):
                hit('8.6.a constant not on left of ==/!=', f, i, lines[i-1]); break
        # 8.4.c infinite loops
        if re.search(r'\bwhile\s*\(\s*(1|true|1u|TRUE)\s*\)', l): hit('8.4.c infinite loop not for (;;)', f, i, lines[i-1])
        # 8.2.c assignment in if
        mm = re.search(r'\bif\s*\((.*)', l)
        if mm and re.search(r'[^=!<>+\-*/%&|^]=[^=]', mm.group(1)): hit('8.2.c assignment inside if test', f, i, lines[i-1])
        mm = re.search(r'\bwhile\s*\((.*)', l)
        if mm and re.search(r'[^=!<>+\-*/%&|^]=[^=]', mm.group(1)): hit('8.4.b assignment in loop condition', f, i, lines[i-1])
        # 8.1.a comma in declarations
        if re.match(r'^(static\s+|const\s+|volatile\s+)*(u?int\d+_t|int|char|bool|float|double|size_t|unsigned)\s+\**\w+(\s*\[[^\]]*\])?(\s*=\s*[^,;(]+)?\s*,\s*\**\w+', s):
            hit('8.1.a comma in variable declaration', f, i, lines[i-1])
        # 3.3.a more than one statement per line
        st = re.sub(r'\bfor\s*\(.*\)', '', s)
        if st.count(';') > 1: hit('3.3.a more than one statement on a line', f, i, lines[i-1])
        # 7.1.e short variable names
        mm = re.search(r'\b(u?int\d*_t|int|size_t|unsigned|char|float|double|bool)\s+\**([a-z]{1,2}\d?)\s*[=;,\[)]', l)
        if mm and mm.group(2) not in ('if', 'do'): hit('7.1.e variable name < 3 chars', f, i, lines[i-1])
        # 1.4.b && || operands w/o parens (comparisons not parenthesised)
        if re.search(r'(&&|\|\|)', l):
            parts = re.split(r'&&|\|\|', l)
            for pt in parts:
                pt2 = pt.strip()
                if re.search(r'[^(]\s*\b[\w\]\)]+\s*(==|!=|<=|>=|<(?!<)|>(?!>))\s*[\w(]', ' ' + pt2) and not (pt2.startswith('(') or pt2.endswith(')')):
                    hit('1.4.b &&/|| operand not parenthesised', f, i, lines[i-1]); break
        # 6.5.b ISR naming: functions registered as irq handlers
        mm = re.search(r'irq_(set_exclusive|add_shared)_handler\s*\([^,]+,\s*(\w+)', l)
        if mm and not mm.group(2).endswith('_isr'): hit('6.5.b ISR not named *_isr', f, i, mm.group(2))
        mm = re.search(r'gpio_set_irq_enabled_with_callback\s*\([^;]*,\s*(\w+)\s*\)', l)
        if mm and not mm.group(1).endswith('_isr'): hit('6.5.b ISR not named *_isr', f, i, mm.group(1))
        # 6.4.a task naming via xTaskCreate / tk_cre_tsk
        mm = re.search(r'xTaskCreate(AffinitySet)?\s*\(\s*(\w+)', l)
        if mm and not re.search(r'_(task|thread|process)$', mm.group(2)): hit('6.4.a task function not named *_task', f, i, mm.group(2))
        # 6.3 function-like macros
        mm = re.match(r'^\s*#\s*define\s+(\w+)\(', lines[i-1])
        if mm: hit('6.3.a function-like macro (use a function if possible)', f, i, lines[i-1])
        mm = re.match(r'^\s*#\s*define\s+(\w*[a-z]\w*)\b', lines[i-1])
        if mm and not mm.group(1).startswith('config') and not mm.group(1).startswith('INCLUDE_'): hit('6.1.f macro name has lowercase', f, i, mm.group(1))
        # 3.2.d '#' not at start of line
        if re.match(r'^\s+#\s*(if|define|include|endif|else|elif|ifdef|ifndef|pragma|error)', lines[i-1]): hit('3.2.d "#" not in column 0', f, i, lines[i-1])
        # 1.1.c pragma/asm
        if re.search(r'#\s*pragma|\b__asm\b|\basm\s*\(|__attribute__', lines[i-1]): hit('1.1.c #pragma/asm/attribute extension', f, i, lines[i-1])
        # 1.6.a casts without a comment on the line
        if re.search(r'\((const\s+)?(u?int\d+_t|int|unsigned|char|float|double|size_t|bool|void|uintptr_t)\s*\**\)\s*[\w(&*-]', l) and not com.get(i):
            hit('1.6.a cast without explanatory comment', f, i, lines[i-1])
        # 2.1.c commented-out code
        c = com.get(i, '')
        if re.search(r'(//|/\*)\s*[a-z_]\w*(\s*\(.*\)|\s*=\s*[^=].*)\s*;\s*(\*/)?\s*$', c): hit('2.1.c commented-out code', f, i, c)
        # 2.2.i markers
        if re.search(r'\b(FIXME|XXX|HACK)\b', c): hit('2.2.i non-standard marker (use WARNING/NOTE/TODO)', f, i, c)

    # function bodies: lengths, switch default, 6.1 naming, 7.1 global naming, 3.1.j decl space
    text = code
    for mm in re.finditer(r'^(?!\s)(?!#)(?!typedef)(?!struct\s+\w+\s*\{)([\w\s\*]+?)\b([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*\{', text, re.M):
        name = mm.group(2)
        if name in ('if', 'for', 'while', 'switch', 'return', 'sizeof'): continue
        start_ln = text.count('\n', 0, mm.start()) + 1
        # find matching close brace
        k = mm.end() - 1; d = 0; j = k
        while j < len(text):
            if text[j] == '{': d += 1
            elif text[j] == '}':
                d -= 1
                if d == 0: break
            j += 1
        end_ln = text.count('\n', 0, j) + 1
        length = end_ln - start_ln + 1
        func_lengths.append((length, f, start_ln, name))
        if length > 100: hit('6.2.a function > 100 lines', f, start_ln, f'{name} ({length} lines)')
        rets = len(re.findall(r'\breturn\b', text[mm.end():j]))
        if rets > 1: hit('6.2.c multiple exit points (preferred: one)', f, start_ln, f'{name}: {rets} returns')
        if re.search(r'[A-Z]', name) and name != 'main': hit('6.1.e function name has uppercase', f, start_ln, name)
        if name.startswith('_'): hit('6.1.c function name begins with _', f, start_ln, name)
        if len(name) > 31: hit('6.1.d function name > 31 chars', f, start_ln, name)
        hdr = text[mm.start():mm.end()]
        if not re.search(r'\b' + re.escape(name) + r' \(', hdr) and name != 'main':
            hit('3.1.j no space between function name and ( in definition', f, start_ln, name)
        is_static = re.search(r'\bstatic\b', mm.group(1))
        if not is_static and name != 'main':
            # 6.1.i public function prefixed with module name
            mod = re.sub(r'\.(c|h)$', '', base).lower()
            if not name.lower().startswith(mod.split('_')[0]): hit('6.1.i public function not prefixed with module name', f, start_ln, f'{name} (module {mod})')
        body = text[mm.end():j]
        # params: 7.1.k pointer params must start with p_
        for prm in mm.group(3).split(','):
            pm = re.search(r'\*\s*(const\s+)?(\w+)\s*(\[\s*\])?$', prm.strip())
            if pm and not pm.group(2).startswith('p'): hit('7.1.k pointer variable not prefixed p_', f, start_ln, f'{name}: {pm.group(2)}')
        for lm in re.finditer(r'^\s+(?:const\s+|static\s+|volatile\s+)*[\w]+\s*(?:const\s*)?\*+\s*(?:const\s+)?(\w+)\s*[=;\[]', body, re.M):
            v = lm.group(1)
            if not v.startswith('p') and v not in ('return',):
                hit('7.1.k pointer variable not prefixed p_', f, text.count('\n', 0, mm.end() + lm.start()) + 1, f'{name}: {v}')
        for lm in re.finditer(r'^\s+(?:const\s+|static\s+)*bool\s+(\w+)', body, re.M):
            if not lm.group(1).startswith('b'): hit('7.1.m bool variable not prefixed b_', f, text.count('\n', 0, mm.end() + lm.start()) + 1, f'{name}: {lm.group(1)}')
        # 8.3.b switch without default
        for sm in re.finditer(r'\bswitch\s*\(', body):
            q = body.find('{', sm.end()); d = 0; e = q
            while e < len(body):
                if body[e] == '{': d += 1
                elif body[e] == '}':
                    d -= 1
                    if d == 0: break
                e += 1
            if not re.search(r'\bdefault\s*:', body[q:e]):
                hit('8.3.b switch without default', f, text.count('\n', 0, mm.end() + sm.start()) + 1, name)
        # 8.2.d else-if chain without final else
        for em in re.finditer(r'\belse\s+if\s*\(', body):
            # walk to end of this else-if's block, then see if chain continues
            pos = em.end(); d = 1
            while pos < len(body) and d:
                d += {'(': 1, ')': -1}.get(body[pos], 0); pos += 1
            nb = body.find('{', pos)
            if nb < 0 or body[pos:nb].strip(): continue
            d = 0; e = nb
            while e < len(body):
                if body[e] == '{': d += 1
                elif body[e] == '}':
                    d -= 1
                    if d == 0: break
                e += 1
            after = body[e+1:].lstrip()
            if not after.startswith('else'):
                hit('8.2.d else-if chain without final else', f, text.count('\n', 0, mm.end() + em.start()) + 1, name)
    # file-scope variables: 7.1.j g_ prefix for non-static globals, s_/static fine
    depth = 0
    for i, l in enumerate(cl, 1):
        if depth == 0:
            m2 = re.match(r'^(?!static|typedef|extern|#|\s|return|struct\s+\w+\s*$)(?:const\s+|volatile\s+)*[\w]+(?:\s+const|\s+volatile)?\s*\**\s*(\w+)\s*(\[[^\]]*\])*\s*(=|;)', l)
            if m2 and f.endswith('.c') and not m2.group(1).startswith('g_') and m2.group(1) not in ('void',):
                hit('7.1.j global variable not prefixed g_', f, i, lines[i-1])
            m3 = re.match(r'^extern\s+(?:const\s+|volatile\s+)*[\w]+\s*\**\s*(\w+)\s*(\[[^\]]*\])*\s*;', l)
            if m3 and not m3.group(1).startswith('g_'): hit('7.1.j global variable not prefixed g_', f, i, lines[i-1])
            if m3 and f.endswith('.h'): hit('4.2.c.i extern variable in header (preferred: none)', f, i, lines[i-1])
        depth += l.count('{') - l.count('}')

# 4.1.b first 8 chars unique
seen = defaultdict(list)
for f in files:
    b = os.path.basename(f).rsplit('.', 1)[0]
    seen[b[:8]].append(b)
for k, v in seen.items():
    if len(set(v)) > 1: res['4.1.b module names not unique in first 8 chars'][k] = [(0, ', '.join(sorted(set(v))))]

summary = {}
for rule in sorted(res):
    tot = sum(len(v) for v in res[rule].values())
    summary[rule] = {'total': tot, 'files': {f: v for f, v in sorted(res[rule].items(), key=lambda kv: -len(kv[1]))}}
func_lengths.sort(reverse=True)
json.dump({'files': files, 'rules': summary, 'longest_functions': func_lengths[:40]}, open(out_path, 'w'), indent=1)
for rule, d in summary.items():
    print(f"{d['total']:6d}  {len(d['files']):4d} files  {rule}")
print('files scanned:', len(files))
