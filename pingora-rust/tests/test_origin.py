import json
import threading
import unittest
from urllib.request import urlopen
from http.server import ThreadingHTTPServer
from scripts.origin import Handler

class TestOrigin(unittest.TestCase):
    def test_origin_response(self):
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.origin_name = "test-a"
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with urlopen(f"http://127.0.0.1:{server.server_address[1]}/hello", timeout=2) as r:
                self.assertEqual(r.status, 200)
                self.assertEqual(json.load(r), {"origin": "test-a", "path": "/hello"})
            with urlopen(f"http://127.0.0.1:{server.server_address[1]}/health", timeout=2) as r:
                self.assertTrue(json.load(r)["healthy"])
        finally:
            server.shutdown()
            thread.join(timeout=3)
            server.server_close()
