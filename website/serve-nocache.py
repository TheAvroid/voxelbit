import http.server, socketserver, functools

class NoCache(http.server.SimpleHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'   # KEEP-ALIVE: reuse each TCP connection for many files instead of a fresh handshake per request — the ~60 boot .vox fetches were paying a full connection setup EACH (HTTP/1.0 default), ~155 ms/file
    def _root_to_engine(self):
        # localhost:8080/  ->  engine.html directly (the game), preserving any ?query (e.g. ?cdp). The default index.html
        # would only redirect here anyway; serving engine.html at the root skips that hop.
        head, sep, tail = self.path.partition('?')
        if head in ('', '/'):
            self.path = '/engine.html' + sep + tail
    def do_GET(self):
        self._root_to_engine(); return super().do_GET()
    def do_HEAD(self):
        self._root_to_engine(); return super().do_HEAD()
    def end_headers(self):
        self.send_header('Cache-Control', 'no-store, no-cache, must-revalidate, max-age=0')
        self.send_header('Pragma', 'no-cache')
        self.send_header('Expires', '0')
        super().end_headers()

class ThreadingServer(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

handler = functools.partial(NoCache, directory=r'c:\voxelbit\website')
with ThreadingServer(('', 8080), handler) as httpd:
    print('no-cache server (threaded) on http://localhost:8080')
    httpd.serve_forever()
