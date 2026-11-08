"""Installer download recovery against a local HTTP server; no external downloads."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest

from test_installer import PS, SCRIPT


@unittest.skipUnless(PS.exists(), 'Windows PowerShell required')
class DownloadTests(unittest.TestCase):
    def download(self, statuses, accept='*/*'):
        calls = []
        accepts = []

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                status = statuses[min(len(calls), len(statuses) - 1)]
                calls.append(status)
                accepts.append(self.headers.get('Accept'))
                self.send_response(status)
                self.end_headers()
                self.wfile.write(b'archive' if status == 200 else b'failed response')

            def log_message(self, *args):
                pass

        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as folder:
                dest = Path(folder) / 'SDL3.zip'
                command = (
                    f". '{SCRIPT}'; function Start-Sleep {{ param($Seconds) }}; "
                    "$ProgressPreference = 'Continue'; "
                    f"try {{ Invoke-Download 'http://127.0.0.1:{server.server_port}/SDL3.zip' '{dest}' 'SDL3' '{accept}' }} "
                    "catch { Write-Host ('EXPECTED FAILURE: ' + $_.Exception.Message) }; "
                    "if ($ProgressPreference -ne 'Continue') { throw 'progress preference changed' }"
                )
                result = subprocess.run(
                    [str(PS), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', command],
                    capture_output=True, text=True, timeout=30,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse(Path(str(dest) + '.partial').exists())
                self.assertEqual(accepts, [accept] * len(calls))
                return calls, dest.read_bytes() if dest.exists() else None, result.stdout
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_gateway_timeout_then_success(self):
        calls, data, output = self.download([504, 200])
        self.assertEqual(calls, [504, 200])
        self.assertEqual(data, b'archive')
        self.assertIn('attempt 2/4', output)

    def test_repeated_timeout_stops_without_cached_error(self):
        calls, data, output = self.download([504])
        self.assertEqual(calls, [504] * 4)
        self.assertIsNone(data)
        self.assertIn('EXPECTED FAILURE:', output)

    def test_missing_asset_is_not_retried(self):
        calls, data, output = self.download([404])
        self.assertEqual(calls, [404])
        self.assertIsNone(data)
        self.assertIn('EXPECTED FAILURE:', output)

    def test_asset_api_requests_binary_content(self):
        calls, data, _ = self.download([200], 'application/octet-stream')
        self.assertEqual(calls, [200])
        self.assertEqual(data, b'archive')
