"""Local test origin, Python stdlib only."""
import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        body = {"healthy": True, "origin": self.server.origin_name} if self.path == "/health" else {"origin": self.server.origin_name, "path": self.path}
        data = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, format, *args):
        pass

def serve(port, name):
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.origin_name = name
    print(f"{name} ready on {port}", flush=True)
    try:
        server.serve_forever()
    finally:
        server.server_close()

if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--name", required=True)
    a = p.parse_args()
    serve(a.port, a.name)
