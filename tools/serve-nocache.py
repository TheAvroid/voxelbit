"""Local no-cache dev server for voxelbit.

Serves the game/ folder only (so tools/, source/ and docs/ are unreachable from the
browser and can never be deployed by accident), sends no-cache headers so edits show
up on refresh, and SHUTS ITSELF DOWN when the last game tab is closed.

How the shutdown detection works
--------------------------------
An idle timeout does not work here: the game fetches its assets at boot and then goes
quiet for the whole session, so "no requests for N seconds" would kill the server while
you are still playing. Instead the page holds ONE open EventSource to /__alive. That
connection lives exactly as long as the tab does - close the tab and the socket drops,
which the server sees immediately.

The heartbeat script is INJECTED into index.html as it is served, so game/index.html
itself stays clean - none of this dev-only plumbing exists in the file you deploy.

A refresh also drops the connection, so the shutdown waits GRACE seconds for a
reconnect before exiting, and only ever arms after the first tab has connected (that
way starting the server before opening a browser doesn't exit instantly).

index.html is BUILT, not read
-----------------------------
The game's source lives in src/ as ~78 ordered fragments (see tools/bundle.py). This
server concatenates them in memory on every request for /, so editing a fragment and
hitting refresh shows the change with no build step - exactly the loop the single file
had. game/index.html on disk is only rebuilt when you run tools/bundle.py, which keeps
the artifact out of the way while several people edit different fragments at once.

If src/ is missing (someone unpacked only game/), it falls back to the file on disk.
"""
import http.server, socketserver, functools, os, sys, time, threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bundle   # tools/bundle.py - src/ fragments -> one index.html

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'game'))   # serve game/ - this script lives in tools/. Derived from __file__ rather than hardcoded, so moving or renaming the project never breaks the server (an earlier version pinned r'c:\voxelbit\website', which stopped existing the moment the site moved).

GRACE = 1.5          # seconds a refresh may take before the tab counts as gone. MEASURED: a real Chrome reload of this game reconnects the beacon 346 ms after requesting the page, so 1.5 s is ~4x margin. Must also stay well clear of RETRY_MS, or a transient drop could race the watchdog and kill the server mid-session.
RETRY_MS = 500       # tell EventSource how fast to reconnect instead of relying on the browser default (~3 s in Chrome), which now exceeds GRACE outright
POLL = 0.15          # watchdog tick — small, so shutdown lands close to GRACE instead of up to a poll-interval late
PING = 0.25          # SSE keep-alive interval. This is the DETECTION latency: a dropped socket is only noticed when the next write fails, so a 1 s ping added a full second before the grace timer even started.
_LOCK = threading.Lock()
_TABS = {'open': 0, 'ever': False, 'zero_at': None}

# Injected just before </body>. EventSource reconnects on transient errors but dies with
# the tab, which is exactly the signal we want.
_BEACON = (b'<script>/* dev server: closes the terminal when this tab closes */'
           b'try{new EventSource("/__alive");}catch(e){}</script>\n')


class NoCache(http.server.SimpleHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'   # KEEP-ALIVE: reuse each TCP connection for many files instead of a fresh handshake per request — the ~60 boot .vox fetches were paying a full connection setup EACH (HTTP/1.0 default), ~155 ms/file

    # ── ALWAYS REACH THIS SERVER AS 127.0.0.1, NEVER AS 'localhost' ── on Windows 'localhost'
    # resolves to ::1 first and the v6 attempt has to time out before the v4 one is tried, and
    # that penalty is paid PER REQUEST, not once per connection. Measured on this box against the
    # same server and the same client: 103 ms/request via localhost vs 0.95 ms/request via
    # 127.0.0.1. Boot fetches ~380 .vox files, so it was ~1.2 s of the loading screen doing
    # nothing at all. Nagle was the obvious suspect and was measured and cleared (no change).

    def log_message(self, fmt, *args):
        if '/__alive' not in (self.path or ''):    # don't spam the window with heartbeats
            super().log_message(fmt, *args)

    def do_GET(self):
        p = (self.path or '').split('?')[0]
        if p == '/__alive':
            return self._alive()
        if p in ('/', '/index.html'):
            return self._index()
        return super().do_GET()

    def do_HEAD(self):
        if (self.path or '').split('?')[0] == '/__alive':
            self.send_response(200); self.end_headers(); return
        return super().do_HEAD()

    def _index(self):
        """Build src/ -> index.html in memory, inject the tab-alive beacon, serve it.

        Rebuilt PER REQUEST rather than cached: the whole point of the fragment layout is
        that an edit shows up on refresh, and 78 small reads are ~2 ms - far below the
        cost of the world the page is about to generate. A fragment with a syntax error
        still bundles fine (this is text concatenation); the browser console is where you
        find out, same as before the split.
        """
        try:
            body = bundle.build()
        except SystemExit as e:                # a fragment listed in the manifest is missing
            self.send_error(500, str(e)); return
        except OSError:
            try:                               # no src/ - serve whatever was last built
                with open(os.path.join(ROOT, 'index.html'), 'rb') as f:
                    body = f.read()
            except OSError:
                self.send_error(404, 'no src/ to build and no index.html in ' + ROOT); return
        body = (body.replace(b'</body>', _BEACON + b'</body>', 1)
                if b'</body>' in body else body + _BEACON)
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _alive(self):
        """Held-open SSE stream. Its lifetime == the tab's lifetime."""
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Connection', 'close')   # SSE has no Content-Length; close-delimited keeps HTTP/1.1 happy
        self.end_headers()
        with _LOCK:
            _TABS['open'] += 1
            _TABS['ever'] = True
            _TABS['zero_at'] = None
        try:
            self.wfile.write(b'retry: %d\n\n' % RETRY_MS)   # pin the client's reconnect delay (see RETRY_MS)
            self.wfile.flush()
            while True:
                self.wfile.write(b': ping\n\n')   # comment frame - keeps the socket warm, ignored by EventSource. The write is also the probe: it fails once the tab is gone.
                self.wfile.flush()
                time.sleep(PING)
        except Exception:
            pass                                   # tab closed / navigated away
        finally:
            with _LOCK:
                _TABS['open'] -= 1
                if _TABS['open'] <= 0:
                    _TABS['open'] = 0
                    _TABS['zero_at'] = time.time()

    def end_headers(self):
        self.send_header('Cache-Control', 'no-store, no-cache, must-revalidate, max-age=0')
        self.send_header('Pragma', 'no-cache')
        self.send_header('Expires', '0')
        # ── CROSS-ORIGIN ISOLATION, SO LOCAL MATCHES PRODUCTION ── voxelbit.net serves these two from
        # game/.htaccess, and without them here `crossOriginIsolated` is false locally and SharedArrayBuffer
        # is undefined — so any SAB work would appear broken in the harness and fine on the site, or worse,
        # the reverse. Nothing in the game is cross-origin (no external scripts or stylesheets, every runtime
        # fetch is same-origin under assets/), so require-corp costs nothing here.
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        super().end_headers()


def watchdog():
    """Exit once every tab has been gone for GRACE seconds."""
    while True:
        time.sleep(POLL)
        with _LOCK:
            gone = _TABS['ever'] and _TABS['open'] == 0 and _TABS['zero_at']
            waited = (time.time() - _TABS['zero_at']) if gone else 0
        if gone and waited > GRACE:
            print('\n  Game tab closed - shutting the server down.')
            sys.stdout.flush()
            os._exit(0)      # _exit, not sys.exit: this is a daemon thread and the server is blocked in serve_forever


class ThreadingServer(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


if __name__ == '__main__':
    # --port lets a test harness serve on its own port instead of fighting the game the
    # user is actually playing on 8080, and lets two agents test at the same time.
    # --no-watchdog keeps the server up for a run that opens and closes tabs.
    port = 8080
    for i, a in enumerate(sys.argv):
        if a == '--port' and i + 1 < len(sys.argv):
            port = int(sys.argv[i + 1])
    if '--no-watchdog' not in sys.argv:
        threading.Thread(target=watchdog, daemon=True).start()
    handler = functools.partial(NoCache, directory=ROOT)
    with ThreadingServer(('', port), handler) as httpd:
        print('no-cache server (threaded) on http://127.0.0.1:%d' % port)
        print('serving', ROOT)
        print('building from', os.path.join(os.path.dirname(ROOT), 'src'))
        if '--no-watchdog' not in sys.argv:
            print('closes automatically when the last game tab is closed')
        httpd.serve_forever()
