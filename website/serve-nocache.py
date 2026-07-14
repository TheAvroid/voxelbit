import http.server, socketserver, functools

class NoCache(http.server.SimpleHTTPRequestHandler):
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
