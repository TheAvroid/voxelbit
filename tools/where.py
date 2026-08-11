"""Map a game/index.html line back to the fragment that wrote it, and back again.

The one debugging cost the src/ split introduced: the browser reports errors against
game/index.html, which is a build artifact nobody edits. Every stack trace therefore
needs a manual hop before it names a file you can open. This closes that.

  python tools/where.py 9412              index.html:9412  ->  sim/life/slots.js:121
  python tools/where.py sim/snow.js:143   the reverse, for "where did my edit land"

The line table re-runs bundle.py's own emit logic per fragment, so it cannot drift from
the build: a fragment is either verbatim bytes or wrap_module's output, exactly as
build() would have written it. The four lines wrap_module adds around a `// @module`
fragment belong to no source line and are reported as such rather than as an off-by-four
line number in the file you are about to open.
"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bundle


def table():
    """[(first, last, rel, off, nbody)] over game/index.html, every line 1-based.

    A generated line n inside a span is fragment line `n - first + off`, valid when it
    lands in 1..nbody; outside that range it is one of wrap_module's own lines - the IIFE
    opener before the body, or the three-line return/close/destructure tail after it.
    """
    rows, line = [], 1
    for rel in bundle.manifest():
        body = open(os.path.join(bundle.SRC, rel.replace('/', os.sep)), 'rb').read()
        wrapped = body.lstrip().startswith(bundle.MODULE_MARK)
        emitted = bundle.wrap_module(rel, body) if wrapped else body
        n = emitted.count(b'\n')
        rows.append((line, line + n - 1, rel, 0 if wrapped else 1, body.count(b'\n')))
        line += n
    return rows


def forward(n):
    for a, b, rel, off, nbody in table():
        if a <= n <= b:
            i = n - a + off
            if 1 <= i <= nbody:
                return '{}:{}'.format(rel, i)
            return '{}  (bundle.py module wrapper, no source line)'.format(rel)
    return 'line {} is past the end of the build'.format(n)


def reverse(rel, n):
    for a, b, r, off, nbody in table():
        if r == rel:
            if not 1 <= n <= nbody:
                return '{} has {} lines'.format(rel, nbody)
            return 'game/index.html:{}'.format(a + n - off)
    return '{} is not listed in src/manifest.txt'.format(rel)


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip())
        return 2
    arg = sys.argv[1]
    if ':' in arg:
        rel, _, n = arg.rpartition(':')
        print(reverse(rel.replace(os.sep, '/'), int(n)))
    else:
        print(forward(int(arg)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
