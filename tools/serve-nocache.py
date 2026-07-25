import http.server, socketserver, functools, os

class NoCache(http.server.SimpleHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'   # KEEP-ALIVE: reuse each TCP connection for many files instead of a fresh handshake per request — the ~60 boot .vox fetches were paying a full connection setup EACH (HTTP/1.0 default), ~155 ms/file
    # NOTE: the game is index.html, so localhost:8080/ serves it automatically — SimpleHTTPRequestHandler
    # looks for index.html by default. (Was a _root_to_engine() override mapping / -> /engine.html back when
    # the game had that name; renaming it to index.html made the whole redirect hop unnecessary.)
    def end_headers(self):
        self.send_header('Cache-Control', 'no-store, no-cache, must-revalidate, max-age=0')
        self.send_header('Pragma', 'no-cache')
        self.send_header('Expires', '0')
        super().end_headers()

class ThreadingServer(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'game'))   # serve the game/ folder — this script lives in tools/, so go up one and into game/. Derived from __file__ instead of hardcoded, so moving or renaming the project never breaks the server (an earlier version pinned r'c:\voxelbit\website', which stopped existing the moment the site moved).
handler = functools.partial(NoCache, directory=ROOT)
with ThreadingServer(('', 8080), handler) as httpd:
    print('no-cache server (threaded) on http://localhost:8080')
    print('serving', ROOT)
    httpd.serve_forever()
