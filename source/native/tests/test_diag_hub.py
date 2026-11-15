"""Exercise the shared diagnostic hub against a loopback HSFZ gateway; no car needed.

Checks the rules that keep the gauges from crowding out the car's own systems:
one gateway connection for both apps, read services only, a request-rate
ceiling, yielding the shared lock to one-shot tools, and stopping completely
while another tester has the DME in a non-default session.
"""
import os
import random
import socket
import struct
import subprocess
import threading
import time
import unittest
from pathlib import Path

BIN = os.environ.get("HUB_BIN", str(Path(__file__).resolve().parents[1] / "build/diag-hub"))
VALUES = {0x4201: bytes.fromhex("63ca"), 0x4205: bytes.fromhex("37e5"),
          0x5889: bytes.fromhex("104e"), 0x4A49: bytes.fromhex("004e"),
          0x580F: bytes.fromhex("82"), 0xDA12: bytes.fromhex("008e"),
          0x56D7: bytes.fromhex("1169"), 0x586F: bytes.fromhex("09c1"),
          0x4300: bytes.fromhex("0bf8")}


def exact(conn, n):
    out = b""
    while len(out) < n:
        data = conn.recv(n - len(out))
        if not data:
            raise EOFError
        out += data
    return out


class FakeGateway:
    """DME (0x12) and EGS (0x18) behind an HSFZ gateway."""

    def __init__(self, multi=True, session=1):
        self.multi, self.session = multi, session
        self.requests = []          # (time, target, uds bytes)
        self.connections = 0
        self.open = 0
        self.max_open = 0
        self.lock = threading.Lock()
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(4)
        self.port = self.server.getsockname()[1]
        self.stopped = False
        threading.Thread(target=self.accept, daemon=True).start()

    def accept(self):
        self.server.settimeout(0.2)
        while not self.stopped:
            try:
                conn, _ = self.server.accept()
            except OSError:
                continue
            with self.lock:
                self.connections += 1
                self.open += 1
                self.max_open = max(self.max_open, self.open)
            threading.Thread(target=self.serve, args=(conn,), daemon=True).start()

    def answer(self, target, uds):
        sid = uds[0]
        if target == 0x12 and sid == 0x22:
            dids = [int.from_bytes(uds[i:i + 2], "big") for i in range(1, len(uds), 2)]
            if dids == [0xF186]:
                return bytes([0x62, 0xF1, 0x86, self.session])
            if len(dids) > 1 and not self.multi:
                return bytes([0x7F, 0x22, 0x13])
            out = b"\x62"
            for did in dids:
                if did not in VALUES:
                    return bytes([0x7F, 0x22, 0x31])
                out += did.to_bytes(2, "big") + VALUES[did]
            return out
        if target == 0x18 and uds == bytes([0x22, 0xDA, 0x12]):
            return bytes([0x62, 0xDA, 0x12]) + VALUES[0xDA12]
        if target == 0x12 and sid == 0x01:
            data = {0x0C: bytes([0x1A, 0xF8]), 0x0D: bytes([0x32])}
            out = b"\x41"
            for pid in uds[1:]:
                out += bytes([pid]) + data[pid]
            return out
        return bytes([0x7F, sid, 0x11])

    def serve(self, conn):
        try:
            with conn:
                conn.settimeout(5)
                while not self.stopped:
                    first = conn.recv(1)
                    if not first:
                        break
                    length, ctrl = struct.unpack(">IH", first + exact(conn, 5))
                    body = exact(conn, length)
                    if ctrl != 1:
                        continue
                    src, target, uds = body[0], body[1], body[2:]
                    with self.lock:
                        self.requests.append((time.monotonic(), target, uds))
                    time.sleep(0.004)  # a quick DME
                    reply = bytes([target, src]) + self.answer(target, uds)
                    conn.sendall(struct.pack(">IH", len(reply), 1) + reply)
        except (OSError, EOFError):
            pass
        finally:
            with self.lock:
                self.open -= 1

    def close(self):
        self.stopped = True
        self.server.close()


class Sink:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.05)
        self.port = self.sock.getsockname()[1]
        self.packets = []
        self.stopped = False
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while not self.stopped:
            try:
                self.packets.append((time.monotonic(), self.sock.recv(1024)))
            except OSError:
                pass

    def close(self):
        self.stopped = True
        self.sock.close()


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class HubTest(unittest.TestCase):
    def setUp(self):
        self.procs = []
        self.hub_port, self.lock_port = free_udp_port(), free_udp_port()
        self.boost, self.drive = Sink(), Sink()

    def tearDown(self):
        for p in self.procs:
            if p.poll() is None:
                p.kill()
            p.wait()
        self.boost.close()
        self.drive.close()

    def start(self, gw, client, extra=()):
        """Starts one hub instance under its own sh parent (like a plugin)."""
        cmd = [BIN, "--host", "127.0.0.1", "--port", str(gw.port), "--client", client,
               "--hub-port", str(self.hub_port), "--lock-port", str(self.lock_port),
               "--boost-port", str(self.boost.port), "--drive-port", str(self.drive.port), *extra]
        parent = subprocess.Popen(["sh", "-c", '"$@" & wait', "sh", *cmd],
                                  stderr=subprocess.DEVNULL)
        self.procs.append(parent)
        return parent

    def boost_frames(self, since=0):
        out = []
        for t, p in self.boost.packets:
            if t < since:
                continue
            f = p.decode().split()
            self.assertEqual(f[0], "NBG3")
            values = [float(x) for x in f[5::2]]
            out.append((t, int(f[3]), values))
        return out

    def drive_frames(self, since=0):
        return [(t,) + struct.unpack("=Qii", p) for t, p in self.drive.packets if t >= since]

    def test_both_apps_share_one_connection_and_get_data(self):
        gw = FakeGateway()
        self.start(gw, "boost")
        time.sleep(0.3)
        self.start(gw, "shift")
        time.sleep(3)
        self.assertEqual(gw.max_open, 1)
        frames = self.boost_frames()
        self.assertGreater(len(frames), 10)
        last = frames[-1]
        self.assertTrue(last[1] & (1 << 1), "AFR valid")
        self.assertAlmostEqual(last[2][1], 0x104E * 16 / 65535 * 14.7, places=3)
        self.assertTrue(last[1] & (1 << 0), "boost valid")
        self.assertAlmostEqual(last[2][0], (0x37E5 / 128 - 0x63CA / 256) * .145037738, places=3)
        drives = self.drive_frames()
        self.assertGreater(len(drives), 10)
        self.assertEqual(drives[-1][2:], (0x1AF8 // 4, 0x32))
        # Fast boost values come from one request, not five.
        fast = [u for _, t, u in gw.requests if u[:3] == bytes([0x22, 0x42, 0x05])]
        self.assertTrue(fast and all(len(u) == 11 for u in fast))
        gw.close()

    def test_read_only_and_rate_ceiling(self):
        gw = FakeGateway()
        self.start(gw, "boost")
        self.start(gw, "shift")
        time.sleep(4)
        reqs = list(gw.requests)
        self.assertTrue(all(u[0] in (0x22, 0x01) for _, _, u in reqs), "read services only")
        times = [t for t, _, _ in reqs]
        gaps = [b - a for a, b in zip(times, times[1:])]
        self.assertGreater(min(gaps), 0.034, "minimum gap between requests")
        for i, t in enumerate(times):
            window = [x for x in times[i:] if x < t + 1]
            self.assertLessEqual(len(window), 26, "at most ~25 requests per second")
        gw.close()

    def test_yields_lock_to_one_shot_tool(self):
        gw = FakeGateway()
        self.start(gw, "boost")
        time.sleep(1)
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        with self.assertRaises(OSError):
            probe.bind(("127.0.0.1", self.lock_port))  # hub holds it while connected
        probe.sendto(b"\x01", ("127.0.0.1", self.lock_port))  # what reserve_gateway() sends
        probe.close()
        got = None
        deadline = time.monotonic() + 0.3
        while time.monotonic() < deadline and got is None:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                s.bind(("127.0.0.1", self.lock_port))
                got = s
            except OSError:
                s.close()
                time.sleep(0.01)
        self.assertIsNotNone(got, "lock handed over within 300 ms")
        taken = time.monotonic()
        time.sleep(0.2)
        self.assertEqual(gw.open, 0, "hub closed its gateway connection")
        time.sleep(1.3)
        quiet = [t for t, _, _ in gw.requests if taken < t]
        self.assertEqual(quiet, [], "no requests while the tool holds the lock")
        got.close()
        time.sleep(3.5)
        self.assertTrue([t for t, _, _ in gw.requests if t > taken + 1.5], "resumes afterwards")
        gw.close()

    def test_stops_while_another_tester_holds_a_session(self):
        gw = FakeGateway(session=3)  # extended session: MHD / ISTA at work
        self.start(gw, "boost", ("--guard-pause-ms", "1500"))
        time.sleep(1.2)
        reqs = list(gw.requests)
        self.assertEqual([u for _, _, u in reqs], [bytes([0x22, 0xF1, 0x86])],
                         "only the session check, nothing else")
        self.assertEqual(gw.open, 0)
        gw.session = 1
        time.sleep(1.5)
        later = [u for _, _, u in gw.requests]
        self.assertGreater(len(later), 3, "resumes once the session is back to default")
        frames = self.boost_frames()
        self.assertTrue(any(v == 0 for _, v, _ in frames), "gauges shown as unavailable while paused")
        gw.close()

    def test_falls_back_to_single_reads(self):
        gw = FakeGateway(multi=False)
        self.start(gw, "boost")
        time.sleep(2.5)
        last = self.boost_frames()[-1]
        self.assertTrue(last[1] & (1 << 1), "AFR still valid")
        multi = [u for _, _, u in gw.requests if u[0] == 0x22 and len(u) > 3]
        self.assertLessEqual(len(multi), 2, "multi-DID tried at most once per group")
        gw.close()

    def test_survivor_takes_over_and_instances_exit_with_parent(self):
        gw = FakeGateway()
        boost = self.start(gw, "boost")
        time.sleep(0.4)
        shift = self.start(gw, "shift")
        time.sleep(1)
        boost.kill()  # BoostDeck screen closed: its plugin went away
        boost.wait()
        t = time.monotonic()
        time.sleep(1.5)
        self.assertGreater(len(self.drive_frames(since=t + 0.8)), 5, "shift data continues")
        self.assertEqual(gw.max_open, 1, "never two connections at once")
        shift.kill()
        shift.wait()
        time.sleep(1)
        self.assertEqual(gw.open, 0, "nothing left talking to the car")
        out = subprocess.run(["pgrep", "-f", f"--hub-port {self.hub_port}"],
                             capture_output=True, text=True).stdout.split()
        self.assertEqual(out, [], "no hub process left running")
        gw.close()


if __name__ == "__main__":
    unittest.main()
