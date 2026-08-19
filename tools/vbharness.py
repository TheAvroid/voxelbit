"""A LONG-LIVED game instance that tests attach to, instead of booting a browser per test.

WHY THIS EXISTS
  Every CDP script in tools/ pays the same toll before it can ask its first question:
  launch Chrome, start a server, generate a world, wait for ready, click the canvas.
  That is seconds of boot plus, in practice, minutes of per-agent scaffolding — and a
  verification that wants ten measurements pays it ten times. This keeps ONE booted game
  alive and answers queries against it in milliseconds, so the cost is paid once per
  session rather than once per question.

  The win is largest for exactly the work this repo does most: change a number in src/,
  look at what it did, change it again. `reload` re-bundles from src/ in memory (see
  serve-nocache.py), so that loop never runs tools/bundle.py at all.

HOW IT IS SAFE
  tools/cdp.py deliberately ties the browser's life to the script's, via a Win32 Job
  Object with KILL_ON_JOB_CLOSE, because orphaned Chromes were a real problem here. A
  persistent instance has to bend that rule, so it bends it in one controlled place:

    * The DAEMON owns the job object, and the daemon is the only thing that outlives a
      command. Kill the daemon by any means — `stop`, Ctrl-C, a crash, TerminateProcess —
      and the job closes and Chrome dies with it. There is no path that leaks a browser.
    * An IDLE TTL (--ttl, default 30 min) makes a forgotten instance reap itself. Every
      client command bumps the clock; when nothing has touched it for the whole window,
      the daemon exits and takes Chrome with it.
    * The slot rules from cdp.py carry over unchanged: VB_SLOT names the instance, and
      the port and Chrome profile are derived from it, so agents running concurrently
      each get their own browser and can never collide.

  It never blanket-kills Chrome. `stop` kills one pid, read from this slot's state file.

USE
    python tools/vbharness.py start                     # ~10 s, once
    python tools/vbharness.py eval "__vb.badgeDbg()"    # ~0.2 s, as often as you like
    python tools/vbharness.py eval --file probe.js
    python tools/vbharness.py shot out.png
    python tools/vbharness.py errors                    # everything the page has logged
    python tools/vbharness.py reload                    # re-read src/, fresh world
    python tools/vbharness.py status
    python tools/vbharness.py stop

  Options that matter: --slot NAME (or VB_SLOT) to run more than one at a time,
  --win 1792x865 to measure at a real resolution rather than the CPU-bound default.

STATE THAT CARRIES OVER — READ THIS BEFORE TRUSTING A SECOND MEASUREMENT
  One browser answering many questions means one WORLD answering many questions, and this
  game's world is mutable. A test that chops a tree, stamps a bird, drops snow, teleports,
  toggles fly mode, opens the editor or unlocks an achievement has changed the thing the
  next test measures. `eval` cannot know that, so it does not pretend to: use `reload`
  between tests whose results depend on world state, and treat `eval` as free only for
  read-only probes. `reload` is a genuine fresh boot — it costs the boot, and it is the
  only reset this tool claims.
"""
import argparse, ctypes, json, os, subprocess, sys, threading, time, urllib.request, urllib.error

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
READY = 'typeof (window.__vb||{}).tp=="function"'

# ── T0 RESET ── everything a previous test can leave behind that a teleport does NOT clear,
# in ONE evaluate rather than a dozen round trips. Ordered so nothing re-arms what came before.
#
# snowHold(false) is the one that has to be here and is easy to get wrong: `__vb.snow(0)` — which
# every existing script in tools/ calls — only flips the CURRENT storm off and leaves snowNextT
# alone, so a storm still arrives 120 s after page load and every 5 minutes after that. Snow is a
# real write into the world, so on a page kept alive for half an hour that is not a cosmetic
# difference, it is silent contamination of every measurement taken after the second minute.
# snowHold(false) pushes the next storm out an hour, which is why it is re-applied on every reload.
T0 = """(() => { const o = {};
  const t = (k, f) => { try { o[k] = f() === undefined ? 'ok' : 'ok'; } catch (e) { o[k] = 'ERR ' + e.message; } };
  t('snow',   () => __vb.snowHold(false));
  t('bodies', () => { __vb.phys(0); __vb.phys(1); });
  t('editor', () => __vb.ed(false));
  t('escMenu',() => __vb.escMenu(false));
  t('fly',    () => { __vb.P.fly = false; });
  t('support',() => __vb.supReset());
  t('frame',  () => __vb.ftReset());
  t('nav',    () => __vb.navreset());
  t('eye',    () => __vb.eyeSync());
  return o; })()"""   # __vb is assigned in one statement — one callable key means the whole surface is up (same signal tools/vbtest.py waits on)


def state_path(slot):
    return os.path.join(os.environ['TEMP'], 'claude', 'vbharness_%s.json' % slot)


def read_state(slot):
    try:
        with open(state_path(slot)) as f:
            return json.load(f)
    except Exception:
        return None


def alive(pid):
    """Is that pid still running? tasklist rather than a signal — os.kill(pid, 0) on
    Windows raises for a pid we do not own, which reads as 'dead' when it is not."""
    try:
        out = subprocess.run(['tasklist', '/FI', 'PID eq %d' % pid, '/NH'],
                             capture_output=True, text=True, timeout=15).stdout
        return str(pid) in out
    except Exception:
        return False


# ────────────────────────────────── client side ──────────────────────────────────

def ctl(slot, path, body=None, timeout=180):
    """Talk to this slot's daemon. Every call bumps its idle clock."""
    st = read_state(slot)
    if not st:
        raise SystemExit('no harness on slot %r — run: python tools/vbharness.py start' % slot)
    if not alive(st['pid']):
        raise SystemExit('harness on slot %r is dead (stale state file) — run: python tools/vbharness.py start' % slot)
    url = 'http://127.0.0.1:%d%s' % (st['ctl'], path)
    data = body.encode('utf-8') if body is not None else None
    try:
        r = urllib.request.urlopen(urllib.request.Request(url, data=data, method='POST' if data is not None else 'GET'),
                                   timeout=timeout)
        return json.loads(r.read().decode('utf-8', 'replace'))
    except urllib.error.URLError as e:
        raise SystemExit('harness on slot %r is not answering: %s' % (slot, e))


# ────────────────────────────────── the daemon ──────────────────────────────────

def daemon(slot, win, ttl, url_only, hold_snow=True):
    """Owns the browser. Everything else in this file is a client of this function.

    One WS, one lock. tools/cdp.py already learned that two readers on one CDP socket
    race for frames and lose replies, so every command is serialised onto the single
    connection this holds rather than each client opening its own.
    """
    os.environ['VB_SLOT'] = slot
    if win:
        os.environ['VB_WIN'] = win
    sys.path.insert(0, HERE)
    import cdp                                    # imported AFTER the env is set: cdp derives its port and profile at import time

    srv_port = cdp.free_port(8100 + (sum(ord(c) for c in slot) % 200))   # never 8080 — that is the port a human's own server sits on
    # CREATE_NO_WINDOW is not cosmetic here. This daemon is spawned DETACHED, so it owns no
    # console; without the flag Windows hands each child process a BRAND NEW console window,
    # and the user watches stray terminals pile up on their desktop — one per instance.
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, 'serve-nocache.py'),
                            '--port', str(srv_port), '--no-watchdog'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           creationflags=0x08000000)
    # …and it joins CHROME'S job object. The `finally` below cannot be relied on: os._exit
    # (the TTL reaper and /stop both use it) skips it outright, and a TerminateProcess from
    # outside skips everything. Only the kernel closing the job handle is guaranteed, and it
    # was already guaranteed for Chrome — the server just was not in it, which is exactly how
    # the hard-kill test could report Chrome reaped while four servers stayed up.
    try:
        cdp.k32.AssignProcessToJobObject(cdp.JOB, int(ctypes.windll.kernel32.OpenProcess(0x1F0FFF, False, srv.pid)))
    except Exception:
        pass
    time.sleep(1.0)
    url = 'http://localhost:%d/?cdp' % srv_port

    t0 = time.time()
    proc, ws, errors = cdp.boot(url, ready_expr=READY)
    boot_s = round(time.time() - t0, 1)
    if hold_snow:
        cdp.ev(ws, '__vb.snowHold(false)')        # before anything is measured — see T0

    lock = threading.Lock()
    last = [time.time()]                          # a list so the http handler and the reaper share one cell
    box = {'boot_s': boot_s, 'reloads': 0, 'mark': 0}

    def do_reload():
        """A real fresh boot: serve-nocache re-bundles from src/, so this is also how an
        edit gets picked up. The canvas click has to happen again — under ?cdp the sim
        does not tick until it does."""
        t = time.time()
        cdp.ev(ws, 'location.reload()')
        time.sleep(1.0)
        for _ in range(360):
            try:
                if cdp.ev(ws, 'document.readyState') == 'complete':
                    break
            except Exception:
                pass
            time.sleep(0.5)
        cdp.ev(ws, "(()=>{const c=document.getElementById('c');const r=c.getBoundingClientRect();"
                   "const o={bubbles:true,clientX:r.width/2,clientY:r.height/2,button:0};"
                   "c.dispatchEvent(new MouseEvent('mousedown',o));"
                   "c.dispatchEvent(new MouseEvent('mouseup',o));"
                   "c.dispatchEvent(new MouseEvent('click',o));return 1})()")
        for _ in range(360):
            if cdp.ev(ws, READY) is True:
                break
            time.sleep(0.5)
        else:
            return {'ok': False, 'error': 'game never became ready after reload'}
        box['reloads'] += 1
        box['mark'] = len(ws.events)              # errors from the OLD page stop counting against the new one
        if hold_snow:
            cdp.ev(ws, '__vb.snowHold(false)')    # a reload re-arms the 120 s storm timer, so the hold has to be re-applied
        return {'ok': True, 'boot_s': round(time.time() - t, 1), 'reloads': box['reloads'],
                'note': 'spawn is re-randomised on every load (world/build.js) — probe absolute coordinates, not spawn-relative ones'}

    import http.server

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, obj, code=200):
            b = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(b)))
            self.end_headers()
            self.wfile.write(b)

        def _body(self):
            n = int(self.headers.get('Content-Length') or 0)
            return self.rfile.read(n).decode('utf-8', 'replace')

        def do_GET(self):
            last[0] = time.time()
            if self.path == '/ping':
                return self._send({'ok': True, 'slot': slot, 'url': url, 'boot_s': box['boot_s'],
                                   'reloads': box['reloads'], 'up_s': round(time.time() - t0, 1),
                                   'idle_s': 0, 'ttl_s': ttl, 'chrome_pid': proc.pid})
            if self.path.startswith('/errors'):
                allp = 'all=1' in self.path
                with lock:
                    ev = ws.events if allp else ws.events[box['mark']:]
                    out = []
                    for m in ev:
                        if m.get('method') == 'Runtime.exceptionThrown':
                            d = m['params']['exceptionDetails']
                            out.append('uncaught: ' + str(d.get('exception', {}).get('description') or d.get('text'))[:300])
                        elif m.get('method') == 'Log.entryAdded' and m['params']['entry'].get('level') == 'error':
                            out.append('console: ' + m['params']['entry'].get('text', '')[:300])
                    return self._send({'ok': True, 'since': 'boot' if allp else 'last reload',
                                       'n404': sum(1 for e in out if '404' in e),
                                       'errors': [e for e in out if '404' not in e], 'all_n': len(out)})
            return self._send({'ok': False, 'error': 'no such endpoint'}, 404)

        def do_POST(self):
            last[0] = time.time()
            body = self._body()
            try:
                if self.path == '/eval':
                    with lock:
                        return self._send({'ok': True, 'value': cdp.ev(ws, body)})
                if self.path == '/shot':
                    with lock:
                        png = cdp.shot(ws)
                    d = os.path.dirname(os.path.abspath(body))
                    if d:
                        os.makedirs(d, exist_ok=True)
                    with open(body, 'wb') as f:
                        f.write(png)
                    return self._send({'ok': True, 'path': os.path.abspath(body), 'bytes': len(png)})
                if self.path.startswith('/reset'):
                    with lock:
                        r = {'t0': cdp.ev(ws, T0)}
                        at = json.loads(body) if body.strip() else None
                        if at:
                            # T1: a teleport further than 200 voxels from the window centre triggers
                            # recenter(), which zeroes the world array — every felled tree, chop, snow
                            # blanket and stamp goes with it, and worldgen is a pure function of world
                            # coordinates, so what comes back is bit-identical to a virgin load.
                            cdp.ev(ws, '__vb.tp(%d,200,%d,0,0)' % (at[0], at[1]))
                            t1 = time.time()
                            while time.time() - t1 < 180:
                                d = cdp.ev(ws, 'JSON.stringify(__vb.gen().deficit)')
                                d = json.loads(d) if d else None
                                if d and max(d.values()) <= 0:
                                    break
                                time.sleep(1.0)
                            r['tp'] = at
                            r['regen_s'] = round(time.time() - t1, 1)
                        r['ok'] = True
                        return self._send(r)
                if self.path == '/reload':
                    with lock:
                        return self._send(do_reload())
                if self.path == '/stop':
                    self._send({'ok': True, 'stopping': True})

                    def bye():
                        time.sleep(0.2)
                        try:
                            srv.terminate()          # the job object would get it anyway; this just makes the port free sooner
                        except Exception:
                            pass
                        os._exit(0)
                    threading.Thread(target=bye, daemon=True).start()
                    return
            except SystemExit as e:                # cdp.ev raises SystemExit on a CDP timeout — a dead page must not take the daemon with it silently
                return self._send({'ok': False, 'error': str(e)}, 500)
            except Exception as e:
                return self._send({'ok': False, 'error': '%s: %s' % (type(e).__name__, e)}, 500)
            return self._send({'ok': False, 'error': 'no such endpoint'}, 404)

    ctl_port = cdp.free_port(8600 + (sum(ord(c) for c in slot) % 200))
    httpd = http.server.ThreadingHTTPServer(('127.0.0.1', ctl_port), H)

    def reaper():
        """The whole reason a persistent browser is allowed to exist. Nothing touches this
        instance for a full TTL window and it takes itself down — os._exit closes the job
        object, which kills Chrome, which is the guarantee the docstring makes."""
        while True:
            time.sleep(5)
            if time.time() - last[0] > ttl:
                try:
                    srv.terminate()
                except Exception:
                    pass
                os._exit(0)

    threading.Thread(target=reaper, daemon=True).start()

    with open(state_path(slot), 'w') as f:        # written LAST: its existence is the signal that the instance is up and answering
        json.dump({'slot': slot, 'pid': os.getpid(), 'ctl': ctl_port, 'srv': srv_port,
                   'dbg': cdp.PORT, 'url': url, 'boot_s': boot_s, 'started': time.time()}, f)
    if url_only:
        print(url)
    try:
        httpd.serve_forever()
    finally:
        try:
            srv.terminate()
        except Exception:
            pass


# ────────────────────────────────── commands ──────────────────────────────────

def cmd_start(a):
    old = read_state(a.slot)
    if old and alive(old['pid']):
        if not a.force:
            print(json.dumps({'ok': True, 'already': True, 'slot': a.slot, 'pid': old['pid'],
                              'url': old['url'], 'hint': 'use --force to replace it'}))
            return 0
        _kill(a.slot, old)
    try:
        os.remove(state_path(a.slot))
    except Exception:
        pass
    os.makedirs(os.path.dirname(state_path(a.slot)), exist_ok=True)
    log = os.path.join(os.path.dirname(state_path(a.slot)), 'vbharness_%s.log' % a.slot)
    args = [sys.executable, os.path.abspath(__file__), '--slot', a.slot, '_daemon', '--ttl', str(a.ttl)]   # --slot is a top-level flag, so it goes BEFORE the subcommand
    if a.snow:
        args += ['--snow']
    if a.win:
        args += ['--win', a.win]
    # DETACHED_PROCESS + a new process group: the daemon has to survive the shell that
    # started it, or the harness is just a slower version of booting per test.
    with open(log, 'wb') as lf:
        subprocess.Popen(args, stdout=lf, stderr=lf, stdin=subprocess.DEVNULL, cwd=ROOT,
                         creationflags=0x00000008 | 0x08000000 | subprocess.CREATE_NEW_PROCESS_GROUP,
                         close_fds=True)   # DETACHED_PROCESS | CREATE_NO_WINDOW
    t0 = time.time()
    while time.time() - t0 < a.timeout:
        st = read_state(a.slot)
        if st and alive(st['pid']):
            print(json.dumps({'ok': True, 'slot': a.slot, 'pid': st['pid'], 'url': st['url'],
                              'boot_s': st['boot_s'], 'started_in_s': round(time.time() - t0, 1)}))
            return 0
        time.sleep(0.5)
    print(json.dumps({'ok': False, 'error': 'harness did not come up in %ss' % a.timeout, 'log': log}))
    return 1


def _kill(slot, st):
    try:
        urllib.request.urlopen('http://127.0.0.1:%d/stop' % st['ctl'], data=b'', timeout=10).read()
        time.sleep(0.5)
    except Exception:
        pass
    if alive(st['pid']):
        # ONE pid, this slot's own daemon. Killing it closes the job object and Chrome goes
        # with it — never a blanket taskkill on chrome.exe, which once took out the user's
        # own browser.
        subprocess.run(['taskkill', '/PID', str(st['pid']), '/T', '/F'],
                       capture_output=True, timeout=30)
    try:
        os.remove(state_path(slot))
    except Exception:
        pass


def cmd_stop(a):
    st = read_state(a.slot)
    if not st:
        print(json.dumps({'ok': True, 'already': 'no harness on that slot'}))
        return 0
    _kill(a.slot, st)
    print(json.dumps({'ok': True, 'stopped': a.slot, 'pid': st['pid']}))
    return 0


def cmd_slots(a):
    """Every live harness on this machine, so a dispatching agent can pick a free slot instead of
    colliding with one already in use — or booting its own Chrome, which is the thing the harness
    exists to stop (see the dispatch rules in CLAUDE.md)."""
    base = os.path.join(os.environ['TEMP'], 'claude')
    rows = []
    for name in sorted(os.listdir(base)) if os.path.isdir(base) else []:
        if not (name.startswith('vbharness_') and name.endswith('.json')):
            continue
        slot = name[len('vbharness_'):-len('.json')]
        st = read_state(slot)
        if not st:
            continue
        live = alive(st['pid'])
        rows.append({'slot': slot, 'pid': st['pid'], 'alive': live,
                     'url': st.get('url'), 'stale_state': not live})
    print(json.dumps({'ok': True, 'live': [r for r in rows if r['alive']],
                      'stale': [r for r in rows if not r['alive']]}, indent=2))
    return 0


def cmd_status(a):
    st = read_state(a.slot)
    if not st:
        print(json.dumps({'ok': False, 'running': False, 'slot': a.slot}))
        return 1
    if not alive(st['pid']):
        print(json.dumps({'ok': False, 'running': False, 'slot': a.slot, 'stale_state': True}))
        return 1
    print(json.dumps(ctl(a.slot, '/ping')))
    return 0


def cmd_eval(a):
    expr = open(a.file, encoding='utf-8').read() if a.file else a.expr
    if not expr:
        raise SystemExit('nothing to evaluate — pass an expression or --file')
    r = ctl(a.slot, '/eval', expr)
    if not r.get('ok'):
        print(json.dumps(r))
        return 1
    v = r['value']
    print(v if isinstance(v, str) and not a.json else json.dumps(v, indent=2 if a.pretty else None))
    # cdp.ev hands back {'__error': ...} for a page-side throw rather than raising
    return 1 if isinstance(v, dict) and '__error' in v else 0


def cmd_shot(a):
    r = ctl(a.slot, '/shot', a.path)
    print(json.dumps(r))
    return 0 if r.get('ok') else 1


def cmd_reload(a):
    r = ctl(a.slot, '/reload', '')
    print(json.dumps(r))
    return 0 if r.get('ok') else 1


def cmd_reset(a):
    at = json.dumps([a.at[0], a.at[1]]) if a.at else ''
    r = ctl(a.slot, '/reset', at, timeout=300)
    print(json.dumps(r, indent=2))
    return 0 if r.get('ok') else 1


def cmd_reap(a):
    """Delete Chrome profiles from finished runs.

    tools/cdp.py never removes PROF, so every run this repo has ever done has left a full
    Chrome profile — shader cache, GPU cache and all — under %TEMP%/claude. They add up to
    real disk. A profile is only removed if no live chrome.exe names it, so a running
    instance (this harness's included) can never be pulled out from under itself.
    """
    import shutil
    base = os.path.join(os.environ['TEMP'], 'claude')
    try:
        out = subprocess.run(['powershell', '-NoProfile', '-Command',
                              "(Get-CimInstance Win32_Process -Filter \"Name='chrome.exe'\").CommandLine"],
                             capture_output=True, text=True, timeout=60).stdout
    except Exception:
        out = ''
    live, freed, gone, kept = out, 0, 0, 0
    for name in sorted(os.listdir(base)) if os.path.isdir(base) else []:
        if not name.startswith('vbtest_'):
            continue
        d = os.path.join(base, name)
        if name in live:
            kept += 1
            continue
        sz = 0
        for root, _, fs in os.walk(d):
            for f in fs:
                try:
                    sz += os.path.getsize(os.path.join(root, f))
                except Exception:
                    pass
        if a.dry_run:
            freed += sz; gone += 1
            continue
        try:
            shutil.rmtree(d, ignore_errors=True)
            freed += sz; gone += 1
        except Exception:
            kept += 1
    print(json.dumps({'ok': True, 'removed': gone, 'in_use_kept': kept,
                      'freed_mb': round(freed / 1048576, 1), 'dry_run': bool(a.dry_run)}))
    return 0


def cmd_errors(a):
    r = ctl(a.slot, '/errors?all=1' if a.all else '/errors')
    errs = list(r.get('errors', []))
    # ---- AND THE ONES THE GAME CAUGHT ITSELF (2026-08-18) ----------------------------------------
    # The CDP feed above only ever sees an UNCAUGHT throw or a console.error. The tick is wrapped in
    # its own try/catch (that wrapper is what stops one bad frame freezing the game permanently), so a
    # ReferenceError thrown every single frame inside the game loop reaches neither: it lands in the
    # game's own __vb.errLog() and in the on-screen banner, and this command printed "(none)".
    # That is exactly what happened to a `flamMate is not defined` bug -- it survived a full test pass,
    # a clean gtest/htest and a push, because the instrument could not see the failure it was asked
    # about. A gate that cannot observe the most common runtime failure is worse than no gate.
    try:
        rr = ctl(a.slot, '/eval', 'JSON.stringify((typeof __vb !== "undefined" && __vb.errLog) ? __vb.errLog() : [])')
        if rr.get('ok'):
            v = rr.get('value')
            for e in (json.loads(v) if isinstance(v, str) else (v or [])):
                errs.append('tick: ' + (e if isinstance(e, str) else json.dumps(e))[:300])
    except Exception:
        pass                        # the page may not be up yet; the CDP list above still stands
    if a.json:
        print(json.dumps(errs, indent=2))
    else:
        print('\n'.join(errs) if errs else '(none)')
    return 1 if errs else 0         # non-zero on ANY error, so a script chaining off this stops


def main():
    ap = argparse.ArgumentParser(description='persistent voxelbit game instance for tests')
    ap.add_argument('--slot', default=os.environ.get('VB_SLOT') or 'default',
                    help='name this instance; agents running at the same time must use different slots')
    sub = ap.add_subparsers(dest='cmd', required=True)

    s = sub.add_parser('start', help='boot an instance and leave it running')
    s.add_argument('--win', default=None, help="offscreen window size, e.g. 1792x865 (default 1280x760, which is CPU-bound — set a real size for timing work)")
    s.add_argument('--ttl', type=int, default=1800, help='seconds of idleness before the instance reaps itself (default 1800)')
    s.add_argument('--timeout', type=int, default=240, help='seconds to wait for the first boot')
    s.add_argument('--force', action='store_true', help='replace an instance already on this slot')
    s.add_argument('--snow', action='store_true',
                   help='leave snowstorms armed. Off by default: a page kept alive gets a storm at 120s and every 5min, and snow WRITES INTO THE WORLD, so it would quietly contaminate anything measured after the second minute')
    s.set_defaults(fn=cmd_start)

    s = sub.add_parser('stop', help='kill this slot (and its Chrome)')
    s.set_defaults(fn=cmd_stop)

    s = sub.add_parser('slots', help='list live harnesses — pick a free slot before dispatching an agent')
    s.set_defaults(fn=cmd_slots)

    s = sub.add_parser('status', help='is it up, and for how long')
    s.set_defaults(fn=cmd_status)

    s = sub.add_parser('eval', help='evaluate JS in the running game and print the result')
    s.add_argument('expr', nargs='?')
    s.add_argument('--file', help='read the expression from a file instead')
    s.add_argument('--json', action='store_true', help='always JSON-encode, even a bare string')
    s.add_argument('--pretty', action='store_true')
    s.set_defaults(fn=cmd_eval)

    s = sub.add_parser('shot', help='CDP screenshot to a path')
    s.add_argument('path')
    s.set_defaults(fn=cmd_shot)

    s = sub.add_parser('reload', help='fresh world, and re-reads src/ (no bundle.py needed)')
    s.set_defaults(fn=cmd_reload)

    s = sub.add_parser('errors', help='page errors since the last reload (404s counted separately)')
    s.add_argument('--json', action='store_true')
    s.add_argument('--all', action='store_true', help='every error since the instance booted, not just this page')
    s.set_defaults(fn=cmd_errors)

    s = sub.add_parser('reset', help='undo what a previous test left behind (storms, felled bodies, editor, camera, counters)')
    s.add_argument('--at', nargs=2, type=int, metavar=('X', 'Z'),
                   help='also wipe the WORLD by teleporting here (>200 voxels away triggers a full regen); omit for the cheap in-place reset')
    s.set_defaults(fn=cmd_reset)

    s = sub.add_parser('reap', help='delete Chrome profiles left by finished runs (never one in use)')
    s.add_argument('--dry-run', action='store_true')
    s.set_defaults(fn=cmd_reap)

    s = sub.add_parser('_daemon', help=argparse.SUPPRESS)
    s.add_argument('--win', default=None)
    s.add_argument('--ttl', type=int, default=1800)
    s.add_argument('--url-only', action='store_true')
    s.add_argument('--snow', action='store_true')
    s.set_defaults(fn=None)

    a = ap.parse_args()
    if a.cmd == '_daemon':
        win = (a.win or '').replace('x', ',') or None
        daemon(a.slot, win, a.ttl, a.url_only, hold_snow=not a.snow)
        return 0
    return a.fn(a)


if __name__ == '__main__':
    sys.exit(main())
