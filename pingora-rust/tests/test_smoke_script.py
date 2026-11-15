import unittest
from unittest.mock import patch
from io import BytesIO
import json
from scripts import smoke_test

class FakeResponse:
    def __init__(self, body):
        self.data = json.dumps(body).encode()
    def __enter__(self):
        return self
    def __exit__(self, *_):
        pass
    status = 200
    def read(self, *args):
        return self.data

class TestSmoke(unittest.TestCase):
    def test_distribution(self):
        def fake_open(url, timeout):
            i = int(url.rsplit("i=",1)[1])
            return FakeResponse({"path": f"/test?i={i}", "origin": "a" if i%2 else "b"})
        with patch.object(smoke_test.urllib.request, "urlopen", side_effect=fake_open):
            counts=smoke_test.check("http://localhost:6188",10)
        self.assertEqual(counts["a"],5)
        self.assertEqual(counts["b"],5)

    def test_one_origin_fails_check(self):
        def fake_open(url, timeout):
            i = int(url.rsplit("i=",1)[1])
            return FakeResponse({"path": f"/test?i={i}", "origin":"only"})
        with patch.object(smoke_test.urllib.request,"urlopen",side_effect=fake_open):
            with self.assertRaises(AssertionError):
                smoke_test.check("http://localhost:6188",4)
