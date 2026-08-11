"""Pre-commit checks for the src/ fragment layout.

Splitting index.html into fragments makes several people editing at once possible, and
introduces exactly one new failure mode: a fragment is text, not a module, so nothing
tells you that the name you just declared is already declared four files away, or that
your edit left a brace open for the NEXT file to trip over. Both of those land as a
blank page with a console error, which is the most expensive way to find out.

Each check below is here because that class of bug has already cost a session:

  1. manifest integrity   a fragment that exists but is not listed is silently dropped
                          from the build - the code is simply gone at runtime
  2. byte hygiene         a stray CR or a missing trailing newline breaks the
                          byte-identical rebuild the split is founded on
  3. brace balance        the mid-line `//` boot-killer: a comment two-thirds of the way
                          into a dense one-liner eats the rest of the line, taking the
                          closing braces with it
  4. clean boundaries     a fragment must not end inside a string, template or comment,
                          or the two files can never be edited independently
  5. duplicate top-level  one shared scope still spans every fragment. Two agents adding
     declarations         `const rad` in two files is a SyntaxError and a black screen
  6. backtick in WGSL     a ` inside a shader comment closes the JS template literal
     comments             early and kills the boot with no useful error
  7. stale artifact       game/index.html is generated; committing it out of step with
                          src/ ships code nobody wrote
  8. uniform layout       the JS writes the uniform buffer at hardcoded float indices and
                          the GPU reads it as `struct U`. Insert a field anywhere but the
                          end and every index below it silently shifts by one slot
  9. worker contract      the gen worker is built from fn.toString(); a serialized
                          function may only name identifiers the worker re-declares
 10. module interface     a `// @module` fragment gets its own scope, so its @exports list
                          must be exactly what the rest of the build reaches for
 11. hooks installed      git runs NOTHING, silently, when core.hooksPath names a
                          directory that is not there - so an uncommitted tools/hooks/
                          leaves every other worktree unchecked while looking checked.
                          --push additionally requires the hooks to be in a COMMIT, which
                          only pre-push can ask without refusing the commit that adds them

  python tools/lint-vb.py          check
  python tools/lint-vb.py -v       check, and print what passed
  python tools/lint-vb.py --push   check, and require tools/hooks/ to be committed
"""
import os, sys, re

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SRC = os.path.join(ROOT, 'src')
VERBOSE = '-v' in sys.argv
PUSHED = '--push' in sys.argv        # pre-push: the commits exist, so check 11 can ask more
ERRORS = []


def err(where, msg):
    ERRORS.append('{}: {}'.format(where, msg))


# -- the tokenizer everything else is built on --------------------------------
# Returns the source with every comment body and every string/template *content*
# replaced by spaces: same length, newlines preserved. Structural questions (braces,
# declarations, statement ends) are then answered by plain scanning, with no chance of
# a brace inside a comment or inside a shader string being counted as real code.
#
# `${...}` inside a template holds REAL code, so it stays unmasked; the stack tracks how
# deep we are so the closing brace hands us back to the template.
def mask_js(s):
    out = list(s)
    n = len(s)
    i = 0
    stack = []            # 'tpl' = inside a template literal, 'sub' = inside its ${ }
    subd = []             # brace depth inside each 'sub', so a nested {} does not end it
    spans = []            # (kind, start, end) of every comment, for checks 4 and 6

    def blank(a, b):
        for k in range(a, min(b, n)):
            if out[k] != '\n':
                out[k] = ' '

    def prev_code_char(at):
        k = at - 1
        while k >= 0 and out[k] in ' \t\n':
            k -= 1
        return out[k] if k >= 0 else ''

    while i < n:
        c = s[i]
        if c == '/' and i + 1 < n and s[i + 1] == '/' and not (stack and stack[-1] == 'tpl'):
            j = s.find('\n', i)
            j = n if j < 0 else j
            blank(i, j)
            spans.append(('//', i, j))
            i = j
        elif c == '/' and i + 1 < n and s[i + 1] == '*' and not (stack and stack[-1] == 'tpl'):
            j = s.find('*/', i + 2)
            j = n if j < 0 else j + 2
            blank(i, j)
            spans.append(('/*', i, j))
            i = j
        elif c in ('"', "'") and not (stack and stack[-1] == 'tpl'):
            j = i + 1
            while j < n and s[j] != c:
                if s[j] == '\\':
                    j += 1
                elif s[j] == '\n':
                    break              # unterminated - the balance check will report it
                j += 1
            blank(i + 1, j)
            spans.append(('string', i, min(j + 1, n)))
            i = min(j + 1, n)
        elif c == '`':
            if stack and stack[-1] == 'tpl':
                stack.pop()
            else:
                stack.append('tpl')
            i += 1
        elif stack and stack[-1] == 'tpl':
            if c == '\\':
                blank(i, i + 2)
                i += 2
            elif c == '$' and i + 1 < n and s[i + 1] == '{':
                stack.append('sub')            # real code resumes
                subd.append(0)
                i += 2
            else:
                blank(i, i + 1)
                i += 1
        elif stack and stack[-1] == 'sub' and c in '{}':
            if c == '{':
                subd[-1] += 1
            elif subd[-1]:
                subd[-1] -= 1
            else:
                stack.pop()                    # THIS one closes the ${ }
                subd.pop()
            i += 1
        elif c == '/':
            p = prev_code_char(i)
            if p and (p.isalnum() or p in '_$)]'):
                i += 1                          # division
            else:                               # regex literal
                j = i + 1
                cls = False
                while j < n and s[j] != '\n':
                    if s[j] == '\\':
                        j += 1
                    elif s[j] == '[':
                        cls = True
                    elif s[j] == ']':
                        cls = False
                    elif s[j] == '/' and not cls:
                        break
                    j += 1
                if j < n and s[j] == '/':
                    blank(i + 1, j)
                    i = j + 1
                else:
                    i += 1                      # not a regex after all
        else:
            i += 1
    return ''.join(out), spans, stack


# -- 1 + 2: the manifest is the architecture, so it must match the disk exactly --
def check_manifest():
    listed = [ln.strip() for ln in open(os.path.join(SRC, 'manifest.txt'), encoding='utf8')
              if ln.strip() and not ln.startswith('#')]
    seen = set()
    for rel in listed:
        if rel in seen:
            err('manifest.txt', 'listed twice: ' + rel)
        seen.add(rel)
        if not os.path.exists(os.path.join(SRC, rel.replace('/', os.sep))):
            err('manifest.txt', 'listed but missing on disk: ' + rel)

    on_disk = set()
    for dirpath, _, files in os.walk(SRC):
        for f in files:
            if f == 'manifest.txt':
                continue
            rel = os.path.relpath(os.path.join(dirpath, f), SRC).replace(os.sep, '/')
            on_disk.add(rel)
    for rel in sorted(on_disk - seen):
        err('manifest.txt', 'file exists but is NOT listed, so it is not in the build: ' + rel)

    for rel in listed:
        path = os.path.join(SRC, rel.replace('/', os.sep))
        if not os.path.exists(path):
            continue
        b = open(path, 'rb').read()
        if b'\r' in b:
            err(rel, 'contains CR - fragments are LF only (see .gitattributes)')
        if not b.endswith(b'\n'):
            err(rel, 'does not end with a newline - the next fragment would be glued onto '
                     'its last line')
    if VERBOSE:
        print('  manifest: {} fragments, all present, all listed'.format(len(listed)))
    return listed


# -- 3 + 4 + 5: everything that needs the whole bundle seen as one scope --------
def module_info(rel):
    """(is_module, exports) for one fragment. Mirrors bundle.py's wrap_module."""
    t = open(os.path.join(SRC, rel.replace('/', os.sep)), encoding='utf8').read()
    if not t.lstrip().startswith('// @module'):
        return False, set()
    NL = chr(10)
    line = t.split(NL, 2)[1] if NL in t else ''
    if '// @exports' not in line:
        err(rel, 'has "// @module" but no "// @exports ..." line after it')
        return True, set()
    return True, {n.strip() for n in line.split('// @exports', 1)[1].split(',') if n.strip()}


def check_bundle(listed):
    texts, offsets, pos = [], [], 0
    for rel in listed:
        t = open(os.path.join(SRC, rel.replace('/', os.sep)), encoding='utf8').read()
        texts.append(t)
        offsets.append((pos, pos + len(t), rel))
        pos += len(t)
    whole = ''.join(texts)

    js0 = whole.index('<script type="module">')
    js1 = whole.rindex('</script>')
    masked, spans, stack = mask_js(whole[js0:js1])
    masked = ' ' * js0 + masked
    spans = [(k, a + js0, b + js0) for k, a, b in spans]

    def where(idx):
        for a, b, rel in offsets:
            if a <= idx < b:
                return '{}:{}'.format(rel, whole.count('\n', a, idx) + 1)
        return '?'

    # 4: a fragment boundary must land in ordinary code, never mid-string/comment
    for a, b, rel in offsets:
        if b <= js0 or b > js1:
            continue
        for kind, s0, s1 in spans:
            if s0 < b < s1:
                err(rel, 'ends inside a {} - the boundary must fall between statements'.format(kind))

    # 3: brace balance, reported at the first closer that goes negative
    depth = {'{': 0, '(': 0, '[': 0}
    pair = {'}': '{', ')': '(', ']': '['}
    for idx in range(js0, js1):
        ch = masked[idx]
        if ch in depth:
            depth[ch] += 1
        elif ch in pair:
            depth[pair[ch]] -= 1
            if depth[pair[ch]] < 0:
                err(where(idx), 'unbalanced "{}" - more closers than openers'.format(ch))
                depth[pair[ch]] = 0
    for k, v in depth.items():
        if v:
            err('bundle', '{} unclosed "{}" across the whole build - look for a "//" comment '
                          'part-way into a dense one-liner'.format(v, k))
    if stack:
        err('bundle', 'ends inside an unterminated template literal or ${ }')

    # 5: one shared scope spans every fragment, so top-level names must be unique.
    # Indent 2 is exactly the IIFE's own level; anything deeper is a nested scope.
    decl = re.compile(r'^  (?:(async function\*?|function\*?|class)|(const|let|var))\s*', re.M)
    ident = re.compile(r'[A-Za-z_$][A-Za-z0-9_$]*')
    mods = {rel: module_info(rel) for rel in listed if rel.endswith('.js')}

    def frag_at(idx):
        for a, b, rel in offsets:
            if a <= idx < b:
                return rel
        return '?'

    seen = {}                     # the SHARED scope: unmodularised names + module exports
    modseen = {}                  # (module, name) -> where, for names that never escape
    for m in decl.finditer(masked):
        i = m.end()
        if m.group(1):
            nm = ident.match(masked, i)
            names = [nm.group(0)] if nm else []
        else:
            j, d = i, 0                      # find the statement end at depth 0
            while j < len(masked):
                c = masked[j]
                if c in '([{':
                    d += 1
                elif c in ')]}':
                    if d == 0:
                        break
                    d -= 1
                elif c == ';' and d == 0:
                    break
                j += 1
            names, d, start = [], 0, i       # then the declarator names, depth-0 commas
            for k in range(i, j + 1):
                c = masked[k] if k < j else ','
                if c in '([{':
                    d += 1
                elif c in ')]}':
                    d -= 1
                elif c == ',' and d == 0:
                    head = masked[start:k].split('=')[0].strip()
                    if head.startswith(('{', '[')):
                        names += ident.findall(head)
                    elif ident.match(head):
                        names.append(ident.match(head).group(0))
                    start = k + 1
        rel = frag_at(m.start())
        is_mod, exports = mods.get(rel, (False, set()))
        for name in names:
            if is_mod and name not in exports:
                # module-private: bundle.py wraps this fragment in its own scope, so the
                # name cannot collide with anything outside it. Only check it against its
                # own module.
                key = (rel, name)
                if key in modseen:
                    err(where(m.start()), 'module-private "{}" is already declared at {}'
                        .format(name, modseen[key]))
                else:
                    modseen[key] = where(m.start())
                continue
            if name in seen:
                err(where(m.start()), 'top-level "{}" is already declared at {} - one scope '
                                      'spans every fragment'.format(name, seen[name]))
            else:
                seen[name] = where(m.start())
    if VERBOSE:
        nmod = sum(1 for v in mods.values() if v[0])
        print('  bundle:   braces balanced, boundaries clean, {} names in the shared scope'
              .format(len(seen)))
        print('  modules:  {} scoped, hiding {} names that can no longer collide'
              .format(nmod, len(modseen)))
    return whole, offsets, masked, seen, where, mods


# -- 6: a backtick inside a WGSL comment closes the JS template early ----------
def check_wgsl(whole, offsets):
    # Walked LINE BY LINE rather than by finding the literal's closing backtick, because
    # the offending backtick IS what the search would land on: a ` in a comment ends the
    # template there, so "the end of the shader" and "the bug" are the same character.
    # Only a backtick in CODE really closes the literal.
    n = 0
    for m in re.finditer(r'/\* wgsl \*/`', whole):
        n += 1
        at = m.end()
        while at < len(whole):
            nl = whole.find('\n', at)
            nl = len(whole) if nl < 0 else nl
            line = whole[at:nl]
            c = line.find('//')
            code, comment = (line, '') if c < 0 else (line[:c], line[c:])
            if '`' in comment:
                rel = next((r for a, b, r in offsets if a <= at + c < b), '?')
                err(rel, 'backtick inside a WGSL // comment - it ends the JS template literal '
                         'early and the boot dies silently')
                break
            if '`' in code:
                break                          # the literal genuinely ends here
            at = nl + 1
    if VERBOSE:
        print('  wgsl:     {} shader literals, no backticks in comments'.format(n))


def _is_member(text, pos):
    """Is the identifier at `pos` a property access (obj.name) rather than a free name?

    The `...` in `push(...edParseVox(x))` ends in a dot and is NOT a member access. Reading
    only the single preceding character got that wrong and quietly concluded that nothing
    outside the module used the name - which would have deleted it from the export list
    and broken the perched cardinal.
    """
    head = text[:pos].rstrip()
    return head.endswith('.') and not head.endswith('...')


def _declared_names(span):
    """Every name a masked source span declares at indent 2, multi-declarators included.

    `const cmdBar = $(...), cmdTxt = $(...), cmdMsg = $(...)` declares three names; taking
    only the first made check 10 report cmdMsg as undeclared.
    """
    ident = re.compile(r'[A-Za-z_$][A-Za-z0-9_$]*')
    out = set()
    for m in re.finditer(r'^  (?:(?:async )?function\s*\*?|class)\s*'
                         r'([A-Za-z_$][A-Za-z0-9_$]*)', span, re.M):
        out.add(m.group(1))
    for m in re.finditer(r'^  (?:const|let|var)\s', span, re.M):
        j, d = m.end(), 0
        while j < len(span):
            c = span[j]
            if c in '([{':
                d += 1
            elif c in ')]}':
                if d == 0:
                    break
                d -= 1
            elif c == ';' and d == 0:
                break
            j += 1
        seg, d, start = span[m.end():j], 0, 0
        for k, c in enumerate(seg + ','):
            if c in '([{':
                d += 1
            elif c in ')]}':
                d -= 1
            elif c == ',' and d == 0:
                head = seg[start:k].split('=')[0].strip()
                if head.startswith(('{', '[')):
                    out |= set(ident.findall(head))
                elif ident.match(head):
                    out.add(ident.match(head).group(0))
                start = k + 1
    return out


# -- 10: a module's declared interface must be the real one --------------------
# `// @exports` is the whole contract: bundle.py returns exactly those names and hides
# the rest. Get it wrong and the failure is not local - a name another fragment needs
# silently stops existing, and the game dies somewhere else entirely. This derives the
# right answer (which of the module's names are actually used outside it) and compares.
#
# It also refuses to export a `let` that another fragment ASSIGNS: the shared scope
# receives a const copy, so those writes would land on the copy and the module would
# never see them. That is a silent wrong-behaviour bug, not a crash, so it is barred
# rather than warned about.
def check_modules(masked, offsets, mods, where):
    if not any(v[0] for v in mods.values()):
        return
    span = {rel: (a, b) for a, b, rel in offsets}

    for rel, (is_mod, exports) in sorted(mods.items()):
        if not is_mod:
            continue
        a, b = span[rel]
        mine = _declared_names(masked[a:b])

        missing_decl = exports - mine
        if missing_decl:
            err(rel, '@exports names nothing this module declares: %s'
                % ', '.join(sorted(missing_decl)))

        # what the rest of the build actually reaches for
        needed = set()
        for n in mine:
            pat = re.compile(r'(?<![\w$])' + re.escape(n) + r'(?![\w$])')
            for a2, b2, rel2 in offsets:
                if rel2 == rel:
                    continue
                seg = masked[a2:b2]
                if any(not _is_member(seg, mm.start()) for mm in pat.finditer(seg)):
                    needed.add(n)
                    break
        for n in sorted(needed - exports):
            err(rel, 'other fragments use "%s" but @exports does not list it - after '
                     'bundling it would not exist outside this module' % n)
        for n in sorted(exports - needed - missing_decl):
            err(rel, '@exports lists "%s" but nothing outside this module uses it - drop '
                     'it and the name stays private' % n)

        # An exported `let` cannot survive the const copy - and the writer does not have
        # to be another fragment. `cmdOpen` is a `let` that ui/console.js assigns ITSELF
        # and ui/input.js only reads; the shared scope still gets a snapshot taken at
        # module-init, so the console silently stopped taking the keyboard. Checking only
        # for writes from OTHER fragments missed that and shipped it.
        for n in sorted(exports & mine):
            if not re.search(r'^  let\s[^;]*(?<![.\w$])' + re.escape(n) + r'(?![\w$])',
                             masked[a:b], re.M):
                continue
            w = re.compile(r'(?<![.\w$])' + re.escape(n)
                           + r'\s*(?:=(?!=)|[-+*/|&^%]=|\+\+|--)')
            writers = []
            for a2, b2, rel2 in offsets:
                seg2 = masked[a2:b2]
                if rel2 == rel:
                    # skip the declaration itself; any later write still counts
                    hits = [m for m in w.finditer(seg2)
                            if not re.match(r'^  let\s', seg2[seg2.rfind(chr(10), 0, m.start()) + 1:])]
                    if hits:
                        writers.append(rel2 + ' (itself)')
                elif w.search(seg2):
                    writers.append(rel2)
            if writers:
                err(rel, '"%s" is an exported `let`, assigned by %s. A module exports a '
                         'CONST SNAPSHOT taken at module-init, so every later write is '
                         'invisible outside. Fold it into an exported object, or move the '
                         'declaration out of the module.' % (n, ', '.join(writers[:3])))
    if VERBOSE:
        print('  exports:  every @exports list matches what the build actually needs')


# -- 9: the gen-worker serialization contract ---------------------------------
# The worker pool is built by pasting the SOURCE of 51 functions into a blob via
# fn.toString(), alongside a registry of 35 consts and 30 tables. toString() emits the
# text exactly as written, so a serialized function may only mention names the worker
# re-declares. Call a new helper from inside one of them and the text still says
# `myHelper(...)`, but the worker has never heard of it: a ReferenceError on a background
# thread, or worse, a silent fall back to the inline path that just runs slower.
#
# Precision matters more than reach here, so this only flags an identifier it can PROVE
# is a top-level game name (from check 5's map) and that is not registered. A local, a
# parameter, `Math`, or anything from another scope cannot reach this list, so a report
# is a real finding rather than something to argue with.
WORKER_DECLS = {          # names the pool writes into the worker preamble by hand
    'WX', 'WZ', 'OX', 'OZ', 'BX', 'W', 'hmap', 'touched', 'MROT', 'remap', 'rivScope',
    'gwrap', 'rivCache', 'caveCache', 'takeRows', 'ORPH_SCRATCH', 'ORPHAN_OK',
    'WY', 'onmessage', 'postMessage', 'self',
}


def _registry(js, name):
    """The identifier list out of `const <name> = { A, B, C };` in gen-pool.js."""
    m = re.search(r'const ' + name + r' = \{([^}]*)\}', js)
    if not m:
        return set()
    return {p.strip().split(':')[0].strip() for p in m.group(1).split(',') if p.strip()}


def _body_of(masked, whole, start):
    """Source span of the declaration beginning at `start`.

    The end is whichever comes first: the `}` that closes the function BODY, or a `;` at
    depth 0 for a concise arrow (`const f = (a) => a * 2;`). Tracking "have we entered a
    brace yet" is what separates the two - without it, `function f(a) { ... }` ended at
    the closing paren of its parameter list and the body was never examined at all.
    """
    d, i, entered = 0, start, False
    while i < len(masked):
        c = masked[i]
        if c in '([':
            d += 1
        elif c == '{':
            d += 1
            entered = True
        elif c in ')]}':
            d -= 1
            if d == 0 and entered:
                return whole[start:i + 1], masked[start:i + 1]
        elif c == ';' and d == 0:
            return whole[start:i], masked[start:i]
        i += 1
    return whole[start:i], masked[start:i]


def check_worker(whole, masked, toplevel, where):
    pool = os.path.join(SRC, 'world', 'gen-pool.js')
    if not os.path.exists(pool):
        return
    js = open(pool, encoding='utf8').read()
    consts, tables, fns = (_registry(js, k) for k in ('consts', 'tables', 'fns'))
    if not fns:
        err('world/gen-pool.js', 'the fns registry could not be read - check 9 is blind')
        return
    registered = consts | tables | fns | WORKER_DECLS

    ident = re.compile(r'[A-Za-z_$][A-Za-z0-9_$]*')
    decl = re.compile(r'\b(?:const|let|var|function)\s+([A-Za-z_$][A-Za-z0-9_$]*)')
    n_checked = 0
    for fname in sorted(fns):
        m = re.search(r'^  (?:const|let|(?:async )?function\s*\*?)\s*' + re.escape(fname) + r'\b',
                      masked, re.M)
        if not m:
            err('world/gen-pool.js', 'fns registers "%s" but nothing declares it at the top '
                                     'level' % fname)
            continue
        src, msrc = _body_of(masked, whole, m.start())
        n_checked += 1
        # names bound INSIDE the function cannot be the bug, whatever they shadow
        local = set(decl.findall(msrc))
        for pm in re.finditer(r'\(([^()]*)\)\s*=>', msrc):
            local |= set(ident.findall(pm.group(1)))
        head = msrc[:msrc.find('{') if '{' in msrc else len(msrc)]
        local |= set(ident.findall(head))
        for pos, tok in ((mm.start(), mm.group(0)) for mm in ident.finditer(msrc)):
            if tok in local or tok not in toplevel or tok in registered or tok == fname:
                continue
            if _is_member(msrc, pos):                    # a member access, not a free name
                continue
            after = msrc[pos + len(tok):].lstrip()
            if after.startswith(':'):                    # an object-literal key
                continue
            err(where(m.start()),
                '"%s" uses top-level "%s", which the gen worker never declares. Add it to '
                'the consts / tables / fns registry in world/gen-pool.js, or the worker '
                'thread throws ReferenceError.' % (fname, tok))
    if VERBOSE:
        print('  worker:   %d serialized fns checked against %d registered names'
              % (n_checked, len(registered)))


# -- 8: the WGSL uniform struct is the single source of truth for UF offsets ---
# The JS writes the uniform buffer as a flat Float32Array at HARDCODED float indices
# (UF[1108 + li * 4], UF[1272 + s * 4], ...) while the GPU reads it as `struct U`. WGSL
# lays a struct out in declaration order, so inserting a field ANYWHERE above the tail
# shifts every index below it and silently feeds each field its neighbour's numbers -
# there is no error, the picture just goes subtly wrong. The comments in both files say
# "APPENDED, never inserted" three times over, which is a rule someone has to remember.
#
# This computes the real layout from the struct text and fails the build if anything
# moved. Append a field at the end and nothing here changes; insert one and it names
# exactly which offsets shifted.
WGSL_SZ = {'f32': (4, 4), 'vec2': (8, 8), 'vec3': (16, 12), 'vec4': (16, 16)}

# Fields the JS addresses as BARE NUMERIC LITERALS, so nothing else can catch them
# moving. Deliberately pinned: changing a number here is a decision, not a typo.
UF_PINNED = {'drops': 68, 'pick2A': 1092, 'fflies': 1108, 'cshad': 1140, 'misc': 1268,
             'lifeMot': 1272, 'lifeCfg': 1528, 'physB': 1532, 'physC': 1852,
             'physBound': 1856, 'lgt': 1864, 'hurtB': 1868, 'hurtH': 1872}


def uf_layout(struct_body):
    """{field: float index} for a WGSL uniform struct, in declaration order."""
    off, out = 0, {}
    pat = r'(\w+)\s*:\s*(?:array<(vec4)<f32>,\s*([^>]+)>|(\w+)<f32>)'
    for m in re.finditer(pat, struct_body):
        name, arrty, arrn, ty = m.groups()
        if arrty:
            n = arrn.strip()
            if n.startswith('${'):                    # an interpolated count
                n = {'${(DROP_SLOTS - DROP_HALF) * 4}': 256,
                     '${DROP_SLOTS - DROP_HALF}': 64}.get(n)
                if n is None:
                    return None, 'struct has an array length this check cannot evaluate'
            a, s = 16, 16 * int(n)
        else:
            if ty not in WGSL_SZ:
                return None, 'unknown WGSL type ' + ty
            a, s = WGSL_SZ[ty]
        off = (off + a - 1) // a * a
        out[name] = off // 4
        off += s
    return out, (off + 15) // 16 * 4                  # total floats, rounded to a vec4


def check_uniforms():
    pre = os.path.join(SRC, 'render', 'wgsl', 'pre.js')
    buf = os.path.join(SRC, 'render', 'buffers.js')
    if not (os.path.exists(pre) and os.path.exists(buf)):
        return
    t = open(pre, encoding='utf8').read()
    if 'struct U {' not in t:
        err('render/wgsl/pre.js', 'the uniform struct U has moved or been renamed - '
                                  'check 8 can no longer verify the UF offsets')
        return
    body = t[t.index('struct U {') + 10:]
    body = body[:body.index('\n    }\n')]
    lay, total = uf_layout(body)
    if lay is None:
        err('render/wgsl/pre.js', total)
        return

    for name, want in sorted(UF_PINNED.items()):
        got = lay.get(name)
        if got is None:
            err('render/wgsl/pre.js', 'struct U no longer has a "%s" field, but the JS '
                                      'still writes float index %d' % (name, want))
        elif got != want:
            err('render/wgsl/pre.js', 'INSERTED FIELD: "%s" moved %d -> %d. The JS writes it '
                                      'as a bare literal, so every index at or below it now '
                                      'reads its neighbour\'s numbers. Append at the END '
                                      'instead.' % (name, want, got))

    # the named JS constants must agree with the struct they describe
    js = open(buf, encoding='utf8').read()
    for m in re.finditer(r'const (UF_[A-Z0-9]+) = (\d+)[,;]', js):
        field = {'UF_DROPSB': 'dropsB', 'UF_HELDCFG': 'heldCfg', 'UF_DOF': 'dof',
                 'UF_LIFEMOTB': 'lifeMotB'}.get(m.group(1))
        if field and lay.get(field) is not None and lay[field] != int(m.group(2)):
            err('render/buffers.js', '%s is %s but struct U puts %s at %d'
                % (m.group(1), m.group(2), field, lay[field]))

    m = re.search(r'new Float32Array\(UF_DOF \+ 4\)', js)
    if m and lay.get('dof') is not None and lay['dof'] + 4 != total:
        err('render/buffers.js', 'UF is UF_DOF+4 = %d floats but struct U needs %d - the '
                                 'last field is not the end of the struct'
            % (lay['dof'] + 4, total))
    if VERBOSE:
        print('  uniforms: struct U = %d floats, %d pinned offsets all where the JS expects'
              % (total, len(UF_PINNED)))


# -- 7: the committed artifact must match the source it was built from ---------
def check_fresh(_unused):
    # game/index.html is generated, and .gitattributes resolves its merge conflicts by
    # keeping our side - which is only safe because a stale artifact cannot get past
    # this check. Run tools/bundle.py and it goes away.
    # Compared against bundle.py's REAL output, not against the raw concatenation of the
    # fragments: a `// @module` fragment is wrapped on the way through, so the two stopped
    # being the same thing the moment the first module landed.
    out = os.path.join(ROOT, 'game', 'index.html')
    if not os.path.exists(out):
        err('game/index.html', 'missing - run: python tools/bundle.py')
        return
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import bundle
    if open(out, 'rb').read() != bundle.build():
        err('game/index.html', 'is stale, it does not match src/ - run: python tools/bundle.py')
    elif VERBOSE:
        print('  artifact: game/index.html matches src/')


def check_hooks():
    # The bundle+lint rule is enforced by tools/hooks/, and git enforces nothing at all
    # when core.hooksPath names a directory that is not there: no warning, no error, a
    # clean exit code. That is how three worktrees came to have hooksPath set and no hook
    # files - the directory was never committed, so it existed only in the working tree it
    # was written in, and commits made in the other three ran no checks while looking
    # exactly like commits that had.
    #
    # Three separate questions, and only the third one protects a worktree other than this
    # one: the files are here, git is pointed at them, and they are TRACKED.
    import subprocess

    def git(*a):
        try:
            r = subprocess.Popen(('git',) + a, cwd=ROOT, stdout=subprocess.PIPE,
                                 stderr=open(os.devnull, 'wb'))
            out = r.communicate()[0]
        except OSError:
            return None
        return out.decode('utf-8', 'replace').strip() if r.returncode == 0 else None

    if git('rev-parse', '--git-dir') is None:
        if VERBOSE:
            print('  hooks:    not a git repo, skipped')
        return

    names = ('pre-commit', 'pre-push', 'post-merge')
    missing = [n for n in names
               if not os.path.exists(os.path.join(ROOT, 'tools', 'hooks', n))]
    if missing:
        err('tools/hooks', 'missing {} in this working tree - commits made HERE run no '
            'checks at all. Merge the branch that carries tools/hooks/'
            .format(', '.join(missing)))

    if git('config', 'core.hooksPath') != 'tools/hooks':
        err('core.hooksPath', 'not set to tools/hooks, so the hooks are inert here. Run '
            'once per clone and per worktree: git config core.hooksPath tools/hooks')

    untracked = [n for n in names
                 if git('ls-files', '--error-unmatch', 'tools/hooks/' + n) is None]
    if untracked:
        err('tools/hooks', '{} not tracked by git - it exists only in THIS working tree, '
            'so every other worktree points core.hooksPath at a directory that is not '
            'there and commits made in them are unchecked. Run: git add tools/hooks/'
            .format(', '.join(untracked)))

    # ls-files reports the INDEX, where a `git add`-ed file already counts as tracked - and
    # a hook that is only staged still reaches no other worktree. The stricter question,
    # is it in a COMMIT, cannot be asked here: the commit that ADDS the hooks would be
    # refused by its own check, because HEAD cannot contain them yet. At push time the
    # commits exist and the deadlock is gone, so pre-push passes --push. This is the exact
    # hole tools/hooks/ fell through - staged on 2026-08-10, committed 2026-08-11, and for
    # a day every worktree ran no checks while this check reported clean.
    if PUSHED:
        uncommitted = [n for n in names
                       if git('cat-file', '-e', 'HEAD:tools/hooks/' + n) is None]
        if uncommitted:
            err('tools/hooks', '{} {} staged but in no commit, so {} only in THIS '
                'working tree. Every other worktree still points core.hooksPath at a '
                'directory that is not there and commits made in them run nothing at all. '
                'Commit tools/hooks/ before pushing.'
                .format(', '.join(uncommitted),
                        'is' if len(uncommitted) == 1 else 'are',
                        'it exists' if len(uncommitted) == 1 else 'they exist'))

    if not missing and not untracked and VERBOSE:
        print('  hooks:    {} present, tracked, and wired up'.format(', '.join(names)))


if __name__ == '__main__':
    listed = check_manifest()
    whole, offsets, masked, toplevel, where, mods = check_bundle(listed)
    check_wgsl(whole, offsets)
    check_worker(whole, masked, toplevel, where)
    check_modules(masked, offsets, mods, where)
    check_uniforms()
    check_fresh(whole)
    check_hooks()
    if ERRORS:
        print('lint-vb: {} problem(s)\n'.format(len(ERRORS)))
        for e in ERRORS:
            print('  ' + e)
        sys.exit(1)
    print('lint-vb: clean')
