import http.server
import sys


class CrossOriginIsolatedHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        super().end_headers()


http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1])), CrossOriginIsolatedHandler).serve_forever()
