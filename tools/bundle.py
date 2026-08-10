"""Concatenate src/ back into game/index.html.

voxelbit ships as ONE self-contained file — double-click start.bat, no build step for
the player, no dependencies. That has not changed. What changed is that the 16k-line
file is no longer what you EDIT: src/ holds it as ~78 ordered fragments so several
people (or several agents) can work on different subsystems at once without landing in
the same file.

  src/manifest.txt   the order. This file IS the architecture — the fragments are
                     concatenated top to bottom exactly as listed, nothing is sorted,
                     nothing is inferred from the directory names.
  game/index.html    a BUILD ARTIFACT. Never edit it; bundle.py overwrites it.

A fragment is a slice of text, not a module: no imports, no exports, no scope of its
own. Everything from core/boot.js to main/99-close.js lives inside the single
`(async () => { ... })()` that core/boot.js opens and main/99-close.js closes, so a
fragment may freely use anything declared in a fragment above it — exactly as the one
big file did. That is the point: the split is provably behaviour-neutral, because
concatenation reproduces the original bytes.

  python tools/bundle.py            rebuild game/index.html
  python tools/bundle.py --check    rebuild in memory and diff, write nothing
"""
import os, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SRC = os.path.join(ROOT, 'src')
OUT = os.path.join(ROOT, 'game', 'index.html')


def manifest():
    """Ordered fragment paths. Blank lines and # comments are ignored."""
    with open(os.path.join(SRC, 'manifest.txt'), encoding='utf8') as f:
        return [ln.strip() for ln in f if ln.strip() and not ln.startswith('#')]


MODULE_MARK = b'// @module'


def wrap_module(rel, body):
    """Give one fragment its own scope.

    A fragment that opens with

        // @module
        // @exports foo, bar

    is wrapped in an IIFE that returns exactly those names, and the shared scope picks
    them up by destructuring. Everything else it declares becomes invisible to the other
    77 fragments - which is the point: two agents can then both invent `edIdx` without
    the merge being a SyntaxError and a black screen.

    The body is emitted byte-for-byte, so nothing inside the module changes; only what
    escapes it does. Names arrive in the shared scope as ordinary consts at the module's
    own position, so every use below it reads exactly as before, at the same cost.

    An exported `let` that other fragments ASSIGN cannot go through here - the shared
    scope would get a const copy and the writes would land nowhere. lint-vb.py check 10
    refuses that case rather than letting it become a silent bug.
    """
    NL = b'\n'
    lines = body.split(NL, 2)
    if len(lines) < 2 or not lines[1].strip().startswith(b'// @exports'):
        sys.exit(rel + ': has "// @module" but no "// @exports ..." line after it')
    names = [n.strip() for n in lines[1].split(b'// @exports', 1)[1].split(b',') if n.strip()]
    if not names:
        sys.exit(rel + ': "// @exports" lists no names')
    slug = rel.rsplit('.', 1)[0].replace('/', '_').replace('-', '_').encode()
    lst = b', '.join(names)
    return (b'  const __m_' + slug + b' = (() => {' + NL
            + body
            + b'    return { ' + lst + b' };' + NL
            + b'  })();' + NL
            + b'  const { ' + lst + b' } = __m_' + slug + b';' + NL)


def build():
    """The whole build: read every fragment in order, join the bytes.

    Binary throughout, and no separator — a fragment already ends in its own newline.
    Anything else (text mode, os.linesep, an added '\n') would rewrite line endings on
    Windows and silently break the byte-identical guarantee this file exists to hold.

    A fragment marked `// @module` is wrapped in its own scope on the way through; every
    other fragment is emitted verbatim, exactly as before.
    """
    out = bytearray()
    for rel in manifest():
        path = os.path.join(SRC, rel.replace('/', os.sep))
        if not os.path.exists(path):
            sys.exit('missing fragment: ' + rel + '  (listed in src/manifest.txt)')
        with open(path, 'rb') as f:
            body = f.read()
        out += wrap_module(rel, body) if body.lstrip().startswith(MODULE_MARK) else body
    return bytes(out)


def main():
    built = build()
    old = open(OUT, 'rb').read() if os.path.exists(OUT) else None

    if '--check' in sys.argv:
        if old == built:
            print('OK  game/index.html matches src/ ({:,} bytes)'.format(len(built)))
            return 0
        print('DIFFERS  src/ rebuilds to {:,} bytes, game/index.html is {:,}'
              .format(len(built), len(old) if old else 0))
        if old:                                  # first differing line, so the report is actionable
            a, b = old.split(b'\n'), built.split(b'\n')
            for i in range(min(len(a), len(b))):
                if a[i] != b[i]:
                    print('  first difference at line', i + 1)
                    print('    on disk:', a[i][:100])
                    print('    rebuilt:', b[i][:100])
                    break
            else:
                print('  one is a prefix of the other: {} vs {} lines'.format(len(a), len(b)))
        return 1

    if old == built:
        print('unchanged  game/index.html ({:,} bytes)'.format(len(built)))
        return 0
    with open(OUT, 'wb') as f:
        f.write(built)
    print('built  game/index.html  {:,} bytes from {} fragments'.format(len(built), len(manifest())))
    return 0


if __name__ == '__main__':
    sys.exit(main())
