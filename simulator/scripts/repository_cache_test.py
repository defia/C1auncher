#!/usr/bin/env python3
"""通过真实 HTTP 服务验证按需下载、并发、断网缓存及损坏恢复。"""
import concurrent.futures
import hashlib
import http.server
import pathlib
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from repository import Repository, RepositoryError
from start import Simulator, handler


class CacheTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="c1sim-cache-")
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.base = self.root / "repository"
        self.cache = self.root / "cache"
        self.base.mkdir()
        self.data = b"original-device-app"
        sha = hashlib.sha256(self.data).hexdigest()
        self.relative = "objects/" + sha + ".tar.gz"
        index = ("C1PKG-INDEX 2\nS\t179\nP\tfixture\t1.0\tFixture\t" + self.relative +
                 "\t" + sha + "\t" + str(len(self.data)) + "\tfixture\tTester\n").encode()
        signature = b"s" * 64
        (self.base / "index.v1").write_bytes(index)
        (self.base / "index.v1.sig").write_bytes(signature)
        self.selection = {"repository": {"url": "https://upstream.invalid/v2",
                          "index_sha256": hashlib.sha256(index).hexdigest(),
                          "signature_sha256": hashlib.sha256(signature).hexdigest()}}
        self.repository = Repository(self.base, self.cache, self.selection)
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler(Simulator(), 0, self.repository))
        self.server.daemon_threads = True
        thread = threading.Thread(target=lambda: self.server.serve_forever(poll_interval=.02), daemon=True)
        thread.start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.url = "http://127.0.0.1:" + str(self.server.server_port) + "/repo/"

    def get(self, relative=None):
        with urllib.request.urlopen(self.url + (relative or self.relative), timeout=5) as response:
            return response.read()

    def error(self, status, relative=None):
        with self.assertRaises(urllib.error.HTTPError) as error:
            self.get(relative)
        self.assertEqual(error.exception.code, status)
        error.exception.close()

    def test_cold_download_and_offline_cache_after_reload(self):
        with mock.patch("repository.download", return_value=self.data) as download:
            self.assertEqual(self.get(), self.data)
            download.assert_called_once_with(self.selection["repository"]["url"] + "/" + self.relative, len(self.data))
        self.assertEqual((self.cache / self.relative).read_bytes(), self.data)
        with mock.patch("repository.download", side_effect=OSError("offline")) as download:
            self.assertEqual(self.get(), self.data)
            reloaded = Repository(self.base, self.cache, self.selection)
            self.assertEqual(reloaded.get(self.relative), self.data)
            download.assert_not_called()

    def test_concurrent_requests_download_only_once(self):
        def slow_download(*args):
            time.sleep(.05)
            return self.data
        with mock.patch("repository.download", side_effect=slow_download) as download:
            with concurrent.futures.ThreadPoolExecutor(max_workers=5) as executor:
                self.assertEqual(list(executor.map(lambda _: self.get(), range(5))), [self.data] * 5)
            self.assertEqual(download.call_count, 1)

    def test_failed_download_is_not_cached_and_can_retry(self):
        with mock.patch("repository.download", side_effect=OSError("offline")):
            self.error(502)
        self.assertFalse((self.cache / self.relative).exists())
        with mock.patch("repository.download", return_value=self.data):
            self.assertEqual(self.get(), self.data)

    def test_bad_download_is_not_published(self):
        with mock.patch("repository.download", return_value=b"x" * len(self.data)):
            self.error(502)
        self.assertFalse((self.cache / self.relative).exists())
        self.assertFalse(list(self.cache.rglob("*.part")))

    def test_corrupted_cache_is_redownloaded(self):
        cached = self.cache / self.relative
        cached.parent.mkdir(parents=True)
        cached.write_bytes(b"x" * len(self.data))
        with mock.patch("repository.download", return_value=self.data) as download:
            self.assertEqual(self.get(), self.data)
            self.assertEqual(download.call_count, 1)
        self.assertEqual(cached.read_bytes(), self.data)

    def test_corrupted_local_package_is_not_served_offline(self):
        target = self.base / self.relative
        target.parent.mkdir()
        target.write_bytes(b"x" * len(self.data))
        with mock.patch("repository.download", side_effect=OSError("offline")):
            self.error(502)

    def test_bundled_package_needs_no_network(self):
        target = self.base / self.relative
        target.parent.mkdir()
        target.write_bytes(self.data)
        with mock.patch("repository.download", side_effect=OSError("offline")) as download:
            self.assertEqual(self.get(), self.data)
            download.assert_not_called()

    def test_unknown_and_traversal_requests_never_download(self):
        (self.root / "private").write_bytes(b"private")
        with mock.patch("repository.download") as download:
            self.error(404, "objects/unknown.tar.gz")
            self.error(404, "../private")
            download.assert_not_called()

    def test_tampered_index_is_rejected(self):
        (self.base / "index.v1").write_bytes(b"tampered")
        self.error(502, "index.v1")
        with self.assertRaises(RepositoryError):
            Repository(self.base, self.cache, self.selection)

    def test_local_media_and_core_files_still_work(self):
        (self.base / "sample.txt").write_bytes(b"example")
        self.assertEqual(self.get("sample.txt"), b"example")
        (self.base / "core").mkdir()
        (self.base / "core/manifest.v1").write_bytes(b"core")
        self.assertEqual(self.get("core/manifest.v1"), b"core")


if __name__ == "__main__":
    unittest.main(verbosity=2)
