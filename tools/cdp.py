"""Minimal CDP driver for voxelbit tests. No dependencies - the websocket client below
is ~30 lines rather than a pip install, so this runs on a bare Python.

Rules baked in (learned the hard way, see memory):
  * NO --headless: there is no WebGPU adapter in headless Chrome.
  * The window is placed far OFFSCREEN instead, and never activated (SWP_NOACTIVATE),
    so it can never steal the user's cursor or foreground window.
  * --mute-audio, always.
  * VB_WIN overrides the offscreen window size ('W,H'). It exists because the small
    default makes the game CPU-bound: a timing run wants a real 1792x865 or it measures
    the wrong bottleneck. Placement stays offscreen whatever the size.
  * The debug port and the Chrome profile are per-RUN, so two agents testing at the same
    time can never share either. VB_SLOT names the slot when you want a stable one (the
    reaper matches that exact token at a boundary); with it unset the slot is this
    process's own pid, because a fixed default meant every agent picked the same port and
    the same profile - which is exactly the collision the slot exists to prevent.
  * A Win32 Job Object with KILL_ON_JOB_CLOSE takes the browser down with this script
    even when `timeout` TerminateProcess's us.
"""
import json, os, subprocess, sys, time, urllib.request, threading, ctypes
from ctypes import wintypes

SLOT = os.environ.get('VB_SLOT') or 'p%d' % os.getpid()


def free_port(base, span=300):
    """First port at or after `base` that nothing is listening on.

    Hashing the slot alone still collides - two slots can hash to the same number, and a
    leftover browser from a killed run keeps its port. Probing is what actually makes
    concurrent runs safe.
    """
    import socket as _sk
    for p in range(base, base + span):
        with _sk.socket(_sk.AF_INET, _sk.SOCK_STREAM) as t:
            t.setsockopt(_sk.SOL_SOCKET, _sk.SO_REUSEADDR, 1)
            try:
                t.bind(('127.0.0.1', p))
                return p
            except OSError:
                continue
    raise SystemExit('no free port in %d..%d' % (base, base + span))


PORT = free_port(9500 + (sum(ord(c) for c in SLOT) % 300))
PROF = os.path.join(os.environ['TEMP'], 'claude', 'vbtest_' + SLOT)
CHROME = None
for c in [r'C:\Program Files\Google\Chrome\Application\chrome.exe',
          r'C:\Program Files (x86)\Google\Chrome\Application\chrome.exe',
          os.path.expandvars(r'%LOCALAPPDATA%\Google\Chrome\Application\chrome.exe')]:
    if os.path.exists(c): CHROME = c; break
assert CHROME, 'chrome not found'

# ── job object: the browser dies with this script, however this script dies ──
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
class JOBLIM(ctypes.Structure):
    _fields_ = [('PerProcessUserTimeLimit', ctypes.c_int64), ('PerJobUserTimeLimit', ctypes.c_int64),
                ('LimitFlags', wintypes.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wintypes.DWORD),
                ('Affinity', ctypes.POINTER(wintypes.ULONG)), ('PriorityClass', wintypes.DWORD),
                ('SchedulingClass', wintypes.DWORD)]
class IOCTR(ctypes.Structure):
    _fields_ = [('ReadOperationCount', ctypes.c_uint64), ('WriteOperationCount', ctypes.c_uint64),
                ('OtherOperationCount', ctypes.c_uint64), ('ReadTransferCount', ctypes.c_uint64),
                ('WriteTransferCount', ctypes.c_uint64), ('OtherTransferCount', ctypes.c_uint64)]
class EXTLIM(ctypes.Structure):
    _fields_ = [('BasicLimitInformation', JOBLIM), ('IoInfo', IOCTR), ('ProcessMemoryLimit', ctypes.c_size_t),
                ('JobMemoryLimit', ctypes.c_size_t), ('PeakProcessMemoryUsed', ctypes.c_size_t),
                ('PeakJobMemoryUsed', ctypes.c_size_t)]
JOB = k32.CreateJobObjectW(None, None)
_l = EXTLIM(); _l.BasicLimitInformation.LimitFlags = 0x2000   # KILL_ON_JOB_CLOSE
k32.SetInformationJobObject(JOB, 9, ctypes.byref(_l), ctypes.sizeof(_l))

def launch(url):
    os.makedirs(PROF, exist_ok=True)
    args = [CHROME, '--remote-debugging-port=%d' % PORT, '--user-data-dir=' + PROF,
            '--mute-audio', '--no-first-run', '--no-default-browser-check',
            '--window-position=-4000,-4000', '--window-size=' + os.environ.get('VB_WIN', '1280,760'),
            '--disable-backgrounding-occluded-windows', '--disable-renderer-backgrounding',
            '--disable-background-timer-throttling', '--disable-features=CalculateNativeWinOcclusion',
            '--enable-unsafe-webgpu', '--no-sandbox', url]
    p = subprocess.Popen(args, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    k32.AssignProcessToJobObject(JOB, int(ctypes.windll.kernel32.OpenProcess(0x1F0FFF, False, p.pid)))
    return p

def _get(path):
    return json.loads(urllib.request.urlopen('http://127.0.0.1:%d%s' % (PORT, path), timeout=5).read())

def wait_target(timeout=30):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            for t in _get('/json/list'):
                if t['type'] == 'page' and 'ws' in t.get('webSocketDebuggerUrl', ''):
                    return t['webSocketDebuggerUrl']
        except Exception: pass
        time.sleep(0.3)
    raise SystemExit('no CDP target')

# ── tiny websocket client (no deps) ──
import socket, base64, struct, os as _os
class WS:
    def __init__(self, url):
        u = url.split('://', 1)[1]; hostport, path = u.split('/', 1); host, port = hostport.split(':')
        self.s = socket.create_connection((host, int(port)), timeout=60)
        key = base64.b64encode(_os.urandom(16)).decode()
        self.s.sendall(('GET /%s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                        'Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n' % (path, hostport, key)).encode())
        buf = b''
        while b'\r\n\r\n' not in buf: buf += self.s.recv(4096)
        self.buf = buf.split(b'\r\n\r\n', 1)[1]; self.id = 0
        self.events = []      # every non-reply message call() saw, in order
    def _recv(self, n):
        while len(self.buf) < n: self.buf += self.s.recv(65536)
        out, self.buf = self.buf[:n], self.buf[n:]; return out
    def send(self, obj):
        d = json.dumps(obj).encode(); h = b'\x81'
        m = _os.urandom(4)
        if len(d) < 126: h += bytes([0x80 | len(d)])
        elif len(d) < 65536: h += bytes([0x80 | 126]) + struct.pack('>H', len(d))
        else: h += bytes([0x80 | 127]) + struct.pack('>Q', len(d))
        self.s.sendall(h + m + bytes(b ^ m[i % 4] for i, b in enumerate(d)))
    def recv(self):
        b0, b1 = self._recv(2); ln = b1 & 127
        if ln == 126: ln = struct.unpack('>H', self._recv(2))[0]
        elif ln == 127: ln = struct.unpack('>Q', self._recv(8))[0]
        return json.loads(self._recv(ln).decode('utf-8', 'replace'))
    def call(self, method, params=None, timeout=120):
        self.id += 1; mid = self.id
        self.send({'id': mid, 'method': method, 'params': params or {}})
        t0 = time.time()
        while time.time() - t0 < timeout:
            m = self.recv()
            if m.get('id') == mid: return m
            if 'method' in m: self.events.append(m)   # NOT discarded: page errors arrive here
        raise SystemExit('timeout on ' + method)

def ev(ws, expr, timeout=120):
    r = ws.call('Runtime.evaluate', {'expression': expr, 'returnByValue': True,
                                     'awaitPromise': True, 'userGesture': True}, timeout)
    res = r.get('result', {})
    if 'exceptionDetails' in res:
        return {'__error': str(res['exceptionDetails'].get('exception', {}).get('description'))[:600]}
    return res.get('result', {}).get('value')


def shot(ws):
    """PNG bytes of the page.

    Always a CDP capture, never an in-page readback: drawImage of the WebGPU canvas
    returns an all-zero bitmap, which reads as a black screen that isn't there.
    """
    r = ws.call('Page.captureScreenshot', {'format': 'png'})
    return base64.b64decode(r['result']['data'])


def boot(url, ready_expr='!!(window.__vb && __vb.pos)', timeout=180, quiet=True):
    """Launch, attach, click the canvas, wait for the game to be up.

    The click is not optional: under ?cdp the engine never takes pointer lock (it would
    ClipCursor-pin the REAL cursor to the offscreen window), so the sim only starts
    ticking once the canvas has been clicked. Returns (proc, ws, errors) where errors is
    a FUNCTION - call it for everything the page has complained about up to that moment.
    """
    # VB_IGPU=1 boots every test instance on the INTEGRATED adapter (appends ?igpu, which asks for
    # powerPreference 'low-power'), so an AI test browser stops competing with the played game for the
    # discrete card. Central here because every tool reaches Chrome through boot() -- no per-tool edit.
    # Needs the iGPU enabled in BIOS: with no integrated adapter present the hint is silently ignored and
    # the test runs on the dGPU anyway, which is exactly what window.__vbAdapter exists to make assertable.
    if os.environ.get('VB_IGPU') == '1' and 'igpu' not in url:
        url += ('&' if '?' in url else '?') + 'igpu'
    p = launch(url)
    ws = WS(wait_target())
    ws.call('Page.enable'); ws.call('Runtime.enable'); ws.call('Log.enable')

    def errors():
        """Everything the page has complained about so far.

        Read out of the events call() collected rather than from a pump thread: two
        readers on one socket race for frames and lose replies.
        """
        out = []
        for m in ws.events:
            if m.get('method') == 'Runtime.exceptionThrown':
                d = m['params']['exceptionDetails']
                out.append('uncaught: ' + str(d.get('exception', {}).get('description')
                                              or d.get('text'))[:300])
            elif m.get('method') == 'Log.entryAdded' and m['params']['entry'].get('level') == 'error':
                out.append('console: ' + m['params']['entry'].get('text', '')[:300])
        return out

    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            if ev(ws, 'document.readyState') == 'complete':
                break
        except Exception:
            pass
        time.sleep(0.5)
    ev(ws, "(()=>{const c=document.getElementById('c');const r=c.getBoundingClientRect();"
           "const o={bubbles:true,clientX:r.width/2,clientY:r.height/2,button:0};"
           "c.dispatchEvent(new MouseEvent('mousedown',o));"
           "c.dispatchEvent(new MouseEvent('mouseup',o));"
           "c.dispatchEvent(new MouseEvent('click',o));return 1})()")
    while time.time() - t0 < timeout:
        v = ev(ws, ready_expr)
        if v is True:
            break
        time.sleep(0.5)
    else:
        raise SystemExit('game never became ready within %ds' % timeout)
    if not quiet:
        print('  booted in %.1fs' % (time.time() - t0))
    return p, ws, errors
