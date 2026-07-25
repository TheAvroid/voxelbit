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
"""
import http.server, socketserver, functools, os, sys, time, threading

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'game'))   # serve game/ - this script lives in tools/. Derived from __file__ rather than hardcoded, so moving or renaming the project never breaks the server (an earlier version pinned r'c:\voxelbit\website', which stopped existing the moment the site moved).

GRACE = 6.0          # seconds a refresh may take before we treat the tab as gone. Must comfortably exceed RETRY_MS below, or a transient drop could race the watchdog and kill the server mid-session.
RETRY_MS = 1000      # tell EventSource how fast to reconnect instead of relying on the browser default (~3 s in Chrome), which sat too close to GRACE
_LOCK = threading.Lock()
_TABS = {'open': 0, 'ever': False, 'zero_at': None}

# Injected just before </body>. EventSource reconnects on transient errors but dies with
# the tab, which is exactly the signal we want.
_BEACON = (b'<script>/* dev server: closes the terminal when this tab closes */'
           b'try{new EventSource("/__alive");}catch(e){}</script>\n')


class NoCache(http.server.SimpleHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'   # KEEP-ALIVE: reuse each TCP connection for many files instead of a fresh handshake per request — the ~60 boot .vox fetches were paying a full connection setup EACH (HTTP/1.0 default), ~155 ms/file

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
        """Serve index.html with the tab-alive beacon injected (file on disk untouched)."""
        try:
            with open(os.path.join(ROOT, 'index.html'), 'rb') as f:
                body = f.read()
        except OSError:
            self.send_error(404, 'index.html not found in ' + ROOT); return
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
                self.wfile.write(b': ping\n\n')   # comment frame - keeps the socket warm, ignored by EventSource
                self.wfile.flush()
                time.sleep(1)
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
        super().end_headers()


def watchdog():
    """Exit once every tab has been gone for GRACE seconds."""
    while True:
        time.sleep(0.4)
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
    threading.Thread(target=watchdog, daemon=True).start()
    handler = functools.partial(NoCache, directory=ROOT)
    with ThreadingServer(('', 8080), handler) as httpd:
        print('no-cache server (threaded) on http://localhost:8080')
        print('serving', ROOT)
        print('closes automatically when the last game tab is closed')
        httpd.serve_forever()
