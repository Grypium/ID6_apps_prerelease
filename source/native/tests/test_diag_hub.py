"""Exercise the shared diagnostic hub against a loopback HSFZ gateway; no car needed.

Checks the rules that keep the gauges from crowding out the car's own systems:
one gateway connection for both apps, read services only, a request-rate
ceiling, yielding the shared lock to one-shot tools, and stopping completely
while another tester has the DME in a non-default session.
"""
import os
import random
import socket
import shutil
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

BIN = os.environ.get("HUB_BIN", str(Path(__file__).resolve().parents[1] / "build/diag-hub"))
VALUES = {0x4201: bytes.fromhex("63ca"), 0x4205: bytes.fromhex("37e5"),
          0x5889: bytes.fromhex("104e"), 0x4A49: bytes.fromhex("004e"),
          0x580F: bytes.fromhex("82"), 0xDA12: bytes.fromhex("008e"),
          0x56D7: bytes.fromhex("1169"), 0x586F: bytes.fromhex("09c1"),
          0x4300: bytes.fromhex("0bf8"), 0x4408: bytes.fromhex("0ef4")}
WHEELS = [1000, 1010, 1300, 1290]  # raw - 0x7FFF: fronts slower (rear wheelspin)
EGS = {0xDA12: bytes.fromhex("008e"), 0xDA2E: bytes.fromhex("0004"), 0xDA34: bytes.fromhex("0690"),
       0xDA1D: b"".join((0x7FFF + w).to_bytes(2, "big") for w in WHEELS)}
OBD = {0x0C: bytes([0x1A, 0xF8]), 0x0D: bytes([0x32]), 0x11: bytes([0x40]), 0x62: bytes([0xA5]),
       0x63: bytes([0x02, 0x3B]), 0x2F: bytes([0x7B])}


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

    def __init__(self, multi=True, session=1, egs_multi=True, dme_delay=0.0, ramp=None, obd_ethanol=None,
                 mhd_ethanol=None, port=0):
        self.multi, self.session, self.egs_multi = multi, session, egs_multi
        self.ramp = ramp  # rpm per second: engine and turbine climb together (locked converter)
        self.obd_ethanol, self.mhd_ethanol = obd_ethanol, mhd_ethanol  # raw A for 01 52 / byte 17 of 6C 88
        self.t0 = time.monotonic()
        self.dme_delay = dme_delay  # seconds before the DME answers (the car: ~30-50 ms)
        self.requests = []          # (time, target, uds bytes)
        self.connections = 0
        self.open = 0
        self.max_open = 0
        self.lock = threading.Lock()
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("127.0.0.1", port))
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
        if target == 0x18 and sid == 0x22:
            dids = [int.from_bytes(uds[i:i + 2], "big") for i in range(1, len(uds), 2)]
            if len(dids) > 1 and not self.egs_multi:
                return bytes([0x7F, 0x22, 0x13])
            out = b"\x62"
            for did in dids:
                if did not in EGS:
                    return bytes([0x7F, 0x22, 0x31])
                value = EGS[did]
                if did == 0xDA34 and self.ramp:
                    value = int(self.ramp_rpm()).to_bytes(2, "big")
                out += did.to_bytes(2, "big") + value
            return out
        if target == 0x12 and sid == 0x2C and uds[1:3] == bytes([0x88, 0x01]):
            if self.mhd_ethanol is None:
                return bytes([0x7F, 0x2C, 0x31])
            block = bytearray(38)
            block[0:2] = b"\x6c\x88"
            block[17] = self.mhd_ethanol
            return bytes(block)
        if target == 0x12 and sid == 0x01:
            out = b"\x41"
            for pid in uds[1:]:
                if pid == 0x52:
                    if self.obd_ethanol is None:
                        return bytes([0x7F, 0x01, 0x12])
                    out += bytes([pid, self.obd_ethanol])
                    continue
                value = OBD[pid]
                if pid == 0x0C and self.ramp:
                    value = int(self.ramp_rpm(0.09) * 4).to_bytes(2, "big")
                out += bytes([pid]) + value
            return out
        return bytes([0x7F, sid, 0x11])

    def ramp_rpm(self, lag=0.0):
        # lag: the DME's OBD rpm describes the engine about 90 ms earlier
        return 1000 + (time.monotonic() - lag - self.t0) * self.ramp % 4000

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
                    if target == 0x12 and self.dme_delay:
                        time.sleep(self.dme_delay)
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


def free_tcp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


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
        self.boost, self.drive, self.timer, self.temps = Sink(), Sink(), Sink(), Sink()
        self.deck, self.gear, self.fuel = Sink(), Sink(), Sink()
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        for p in self.procs:
            if p.poll() is None:
                p.kill()
            p.wait()
        self.boost.close()
        self.drive.close()
        self.timer.close()
        self.temps.close()
        self.deck.close()
        self.gear.close()
        self.fuel.close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def start(self, gw, client, extra=()):
        """Starts one hub instance under its own sh parent (like a plugin)."""
        cmd = [BIN, "--host", "127.0.0.1", "--port", str(gw.port), "--client", client,
               "--hub-port", str(self.hub_port), "--lock-port", str(self.lock_port),
               "--boost-port", str(self.boost.port), "--drive-port", str(self.drive.port),
               "--timer-port", str(self.timer.port), "--temps-port", str(self.temps.port),
               "--deck-port", str(self.deck.port), "--gear-port", str(self.gear.port),
               "--fuel-port", str(self.fuel.port), *extra]
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

    def timer_frames(self, since=0):
        return [(t,) + struct.unpack("=Qiii4x", p) for t, p in self.timer.packets if t >= since]

    def test_timer_client_gets_wheel_speeds(self):
        gw = FakeGateway()
        self.start(gw, "timer")
        time.sleep(3)
        first = self.timer.packets[0][0]
        frames = self.timer_frames(first + 1)
        self.assertGreater(len(frames), 20, "about 16 wheel-speed samples a second")
        gaps = [b[1] - a[1] for a, b in zip(frames, frames[1:])]
        self.assertLess(sorted(gaps)[len(gaps) // 2], 90, "median sample spacing")
        _, _, kph, front, driven = frames[-1]
        self.assertEqual(kph, 0x32, "OBD speed rides along")
        self.assertEqual(front, round((1000 + 1010) / 2 * 0.01728 * 100))
        self.assertEqual(driven, round((1300 + 1290) / 2 * 0.01728 * 100))
        reqs = [t for t, _, _ in gw.requests if t > first + 1]
        self.assertLessEqual(len(reqs) / (reqs[-1] - reqs[0]), 25, "within the rate ceiling")
        self.assertEqual(self.drive.packets, [], "nothing sent to ShiftDeck's port when it is not open")
        gw.close()

    def test_timer_falls_back_to_obd_speed(self):
        gw = FakeGateway()
        del EGS[0xDA1D]
        try:
            self.start(gw, "timer")
            time.sleep(2.5)
        finally:
            EGS[0xDA1D] = b"".join((0x7FFF + w).to_bytes(2, "big") for w in WHEELS)
        frames = self.timer_frames(self.timer.packets[0][0] + 1)
        self.assertGreater(len(frames), 15, "OBD speed every 50 ms instead")
        self.assertEqual(frames[-1][2], 0x32)
        self.assertEqual(frames[-1][3], -2**31, "no wheel speeds")
        wheel = [u for _, _, u in gw.requests if u == bytes([0x22, 0xDA, 0x1D])]
        self.assertEqual(len(wheel), 1, "asked once, then stopped")
        gw.close()

    def test_deck_client_gets_torque_power_throttle_oil(self):
        gw = FakeGateway()
        self.start(gw, "deck")
        time.sleep(3)
        frames = [struct.unpack("=Qiiiii4x", p) for _, p in self.deck.packets]
        self.assertGreaterEqual(len(frames), 7, "about three a second")
        _, oil, torque, power, throttle, ethanol = frames[-1]
        nm = (0xA5 - 125) / 100 * 571
        self.assertAlmostEqual(torque, round(nm * 10), delta=1)
        self.assertAlmostEqual(power, round(nm * (0x1AF8 / 4) / 9549.3 * 10), delta=1)
        self.assertAlmostEqual(throttle, round(0x40 * 100 / 255 * 10), delta=1)
        self.assertAlmostEqual(oil, round((0x0EF4 / 10 - 273.15) * 10), delta=1)
        self.assertEqual(ethanol, -2**31, "no ethanol read without --ethanol")
        self.assertGreater(len(self.boost_frames()), 10, "NBG3 frames for the dials too")
        reqs = [u for _, _, u in gw.requests]
        self.assertTrue(all(u[0] in (0x22, 0x01) for u in reqs), "read services only")
        self.assertEqual(sum(1 for u in reqs if u[:2] == bytes([0x01, 0x0C]) and 0x63 in u[2:]), 1,
                         "reference torque read once")
        self.assertEqual(gw.max_open, 1)
        gw.close()

    def test_gear_client_gets_gear_and_turbine(self):
        gw = FakeGateway()
        self.start(gw, "gear")
        time.sleep(2.5)
        frames = [struct.unpack("=Qiiii", p) for _, p in self.gear.packets]
        self.assertGreaterEqual(len(frames), 8, "about five a second")
        _, rpm, turbine, gear, sample = frames[-1]
        self.assertEqual((rpm, turbine, gear), (0x1AF8 // 4, 0x690, 4))
        self.assertGreater(len(self.drive_frames()), 10, "ShiftDeck's rpm/speed frames too")
        egs = [u for _, t, u in gw.requests if t == 0x18 and u[1:3] == bytes([0xDA, 0x2E])]
        self.assertTrue(egs and all(len(u) == 5 for u in egs), "gear and turbine in one request")
        gw.close()

    def test_fuel_client_reads_tank_level_slowly(self):
        gw = FakeGateway()
        self.start(gw, "fuel")
        time.sleep(4.5)
        frames = [struct.unpack("=Qiiii", p) for _, p in self.fuel.packets]
        self.assertGreaterEqual(len(frames), 3)
        self.assertAlmostEqual(frames[-1][1], round(0x7B * 100 / 255 * 10), delta=1)
        reqs = [u for _, _, u in gw.requests]
        self.assertTrue(all(u in (bytes([0x01, 0x2F]), bytes([0x22, 0xF1, 0x86])) for u in reqs), reqs)
        self.assertLessEqual(len(reqs), 6, "a request every two seconds plus the session check")
        gw.close()

    def fuel_frames(self):
        return [struct.unpack("=Qiiii", p) for _, p in self.fuel.packets]

    def test_fuel_ethanol_off_unless_asked(self):
        gw = FakeGateway(obd_ethanol=0x80, mhd_ethanol=55)
        self.start(gw, "fuel")
        time.sleep(3)
        _, level, ethanol, source, _ = self.fuel_frames()[-1]
        self.assertEqual((ethanol, source), (-2**31, 0))
        self.assertFalse([u for _, _, u in gw.requests if u[:2] in (b"\x01\x52", b"\x2c\x88")], "no ethanol reads")
        gw.close()

    def test_fuel_ethanol_obd(self):
        gw = FakeGateway(obd_ethanol=0x80, mhd_ethanol=55)
        self.start(gw, "fuel", ("--ethanol", "obd"))
        time.sleep(3)
        _, level, ethanol, source, _ = self.fuel_frames()[-1]
        self.assertAlmostEqual(ethanol, round(0x80 * 100 / 255 * 10), delta=1)
        self.assertEqual(source, 1)
        self.assertFalse([u for _, _, u in gw.requests if u[:2] == b"\x2c\x88"], "obd mode never sends the MHD request")
        ethanol_reads = [u for _, _, u in gw.requests if u[:2] == b"\x01\x52"]
        self.assertLessEqual(len(ethanol_reads), 2, "one ethanol read every five seconds")
        gw.close()

    def test_fuel_ethanol_obd_mode_never_falls_back_to_mhd(self):
        gw = FakeGateway(obd_ethanol=None, mhd_ethanol=55)
        self.start(gw, "fuel", ("--ethanol", "obd"))
        time.sleep(3)
        self.assertEqual(self.fuel_frames()[-1][2], -2**31)
        self.assertFalse([u for _, _, u in gw.requests if u[:2] == b"\x2c\x88"])
        gw.close()

    def test_fuel_ethanol_auto_falls_back_to_mhd(self):
        gw = FakeGateway(obd_ethanol=None, mhd_ethanol=55)
        self.start(gw, "fuel", ("--ethanol", "auto"))
        time.sleep(7)
        _, level, ethanol, source, _ = self.fuel_frames()[-1]
        self.assertEqual((ethanol, source), (550, 2))
        gw.close()

    def responder(self, tag):
        """Fake HSFZ identification responder on 127.0.0.1 (UDP); returns (socket, port)."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        sock.settimeout(0.2)
        seen = []
        stop = threading.Event()
        self.addCleanup(stop.set)
        self.addCleanup(sock.close)
        def serve():
            while not stop.is_set():
                try:
                    data, addr = sock.recvfrom(64)
                except OSError:
                    continue
                seen.append(data)
                body = b"DIAGADR" + tag + b"BMWMAC001122334455BMWVINTESTVIN0000000000"
                sock.sendto(struct.pack(">IH", len(body), 0x11) + body, addr)
        threading.Thread(target=serve, daemon=True).start()
        return sock, sock.getsockname()[1], seen

    def start_discovering(self, gw, responder_port, log):
        cmd = [BIN, "--host", "127.0.0.2", "--port", str(gw.port), "--client", "fuel",
               "--hub-port", str(self.hub_port), "--lock-port", str(self.lock_port),
               "--fuel-port", str(self.fuel.port), "--discover", "127.0.0.1", str(responder_port), "--log", log]
        parent = subprocess.Popen(["sh", "-c", '"$@" & wait', "sh", *cmd], stderr=subprocess.DEVNULL)
        self.procs.append(parent)

    def test_discovery_finds_moved_gateway(self):
        # gateway-ip.txt points at an address nobody answers on (127.0.0.2); the
        # identification broadcast finds the gateway at 127.0.0.1 and data flows.
        gw = FakeGateway()
        sock, port, seen = self.responder(b"10")
        log = os.path.join(self.tmp, "hub.log")
        self.start_discovering(gw, port, log)
        time.sleep(4)
        frames = self.fuel_frames()
        self.assertTrue(frames and frames[-1][1] != -2**31, "tank level arrives via the discovered gateway")
        self.assertTrue(seen and seen[0] == bytes([0, 0, 0, 0, 0, 0x11]), seen)
        text = open(log).read()
        self.assertIn("cannot connect to 127.0.0.2", text)
        self.assertIn("discovery: gateway answered at 127.0.0.1", text)
        self.assertIn("connected to 127.0.0.1", text)
        self.assertNotIn("TESTVIN", text, "nothing from the reply is logged")
        gw.close()

    def test_discovery_ignores_head_unit_reply(self):
        gw = FakeGateway()
        sock, port, seen = self.responder(b"63")  # only the head unit answers
        log = os.path.join(self.tmp, "hub.log")
        self.start_discovering(gw, port, log)
        time.sleep(3)
        self.assertFalse([f for f in self.fuel_frames() if f[1] != -2**31])
        self.assertIn("discovery: no gateway answered", open(log).read())
        self.assertLessEqual(len(seen), 1, "one broadcast per five seconds")
        gw.close()

    def gear_frames(self):
        return [struct.unpack("=Qiiii", p) for _, p in self.gear.packets]

    def start_most(self, port, log, client="boost", mode="auto"):
        """A hub whose gateway address has nobody listening, with the fake MOST tunnel up."""
        cmd = [BIN, "--host", "127.0.0.1", "--port", str(port), "--client", client,
               "--hub-port", str(self.hub_port), "--lock-port", str(self.lock_port),
               "--boost-port", str(self.boost.port), "--gear-port", str(self.gear.port),
               "--discover", "127.0.0.1", str(free_udp_port()), "--log", log, "--transport", mode]
        env = dict(os.environ, ID6_FAKE_MOST="up")
        parent = subprocess.Popen(["sh", "-c", '"$@" & wait', "sh", *cmd], stderr=subprocess.DEVNULL, env=env)
        self.procs.append(parent)

    def test_most_fallback_reads_the_dme_without_a_gateway(self):
        log = os.path.join(self.tmp, "hub.log")
        self.start_most(free_tcp_port(), log)
        time.sleep(11)
        text = open(log).read()
        self.assertIn("using the MOST tunnel (DME only)", text)
        frames = self.boost_frames()
        self.assertTrue(frames, "BoostDeck frames arrive over MOST")
        self.assertTrue(any(v > 0 for v in frames[-1][2]), "with live DME values")

    def test_hsfz_only_never_opens_most(self):
        log = os.path.join(self.tmp, "hub.log")
        self.start_most(free_tcp_port(), log, mode="hsfz")
        time.sleep(9)
        self.assertNotIn("MOST", open(log).read())
        self.assertFalse([f for f in self.boost_frames() if any(v > 0 for v in f[2])])

    def test_leaves_most_when_the_gateway_appears(self):
        # Cold start: the gateway shows up a while after the hub went to MOST.
        port = free_tcp_port()
        log = os.path.join(self.tmp, "hub.log")
        self.start_most(port, log, client="gear")
        time.sleep(10)
        self.assertIn("using the MOST tunnel", open(log).read())
        self.assertFalse([g for g in self.gear_frames() if g[3] > 0], "no gear over MOST")
        gw = FakeGateway(port=port)
        time.sleep(33)
        self.assertIn("leaving the MOST tunnel", open(log).read())
        self.assertTrue([g for g in self.gear_frames() if g[3] > 0], "gear back on the gateway")
        self.assertTrue([u for _, t, u in gw.requests if t == 0x18], "EGS read over HSFZ")
        gw.close()

    def test_gear_not_starved_by_slow_dme(self):
        # On the car the DME takes 30-50 ms, which stretches the request gap to
        # about the rpm period; the gear read must still get its turn.
        gw = FakeGateway(dme_delay=0.045)
        self.start(gw, "gear")
        time.sleep(3)
        frames = [struct.unpack("=Qiiii", p) for _, p in self.gear.packets]
        self.assertTrue(frames and frames[-1][2:4] == (0x690, 4), frames[-3:])
        self.assertGreater(len(self.drive_frames()), 10, "rpm frames keep coming")
        gw.close()

    def test_no_false_slip_while_rpm_climbs(self):
        # Locked converter during a pull: engine and turbine rise together at
        # 2000 rpm/s; the two reads are taken at different times, so the hub
        # must line them up or ShiftDeck shows hundreds of rpm of slip. The fake
        # DME also reports rpm 90 ms late, as the car's does.
        gw = FakeGateway(dme_delay=0.035, ramp=2000)
        self.start(gw, "gear")
        time.sleep(3)
        frames = [struct.unpack("=Qiiii", p) for _, p in self.gear.packets]
        pairs = [(rpm, turbine) for _, rpm, turbine, _, _ in frames[3:] if rpm != -2**31 and turbine != -2**31
                 and 1100 < turbine < 4900]
        self.assertGreaterEqual(len(pairs), 5, frames[-5:])
        slips = sorted(abs(rpm - turbine) for rpm, turbine in pairs)
        self.assertLess(slips[len(slips) // 2], 40, slips)
        samples = [f[4] for f in frames if f[4]]
        self.assertEqual(samples, sorted(samples), "pair ids only go up")
        self.assertGreaterEqual(len(set(samples)), 8, "a new pair about every 200 ms")
        gw.close()

    def test_gear_falls_back_to_single_egs_reads(self):
        gw = FakeGateway(egs_multi=False)
        self.start(gw, "gear")
        time.sleep(3)
        frames = [struct.unpack("=Qiiii", p) for _, p in self.gear.packets]
        self.assertEqual(frames[-1][2:4], (0x690, 4))
        gw.close()

    def temps_frames(self):
        return [(t,) + struct.unpack("=Qiiii", p) for t, p in self.temps.packets]

    def test_temps_client_alone_reads_little(self):
        gw = FakeGateway()
        self.start(gw, "temps")
        time.sleep(4.5)
        frames = self.temps_frames()
        self.assertGreaterEqual(len(frames), 3, "about one frame a second")
        self.assertLessEqual(len(frames), 6)
        _, _, oil, coolant, intake, trans = frames[-1]
        self.assertAlmostEqual(oil, round((0x0EF4 / 10 - 273.15) * 10), delta=1)
        self.assertAlmostEqual(coolant, round((0x0BF8 / 10 - 273.15) * 10), delta=1)
        self.assertAlmostEqual(intake, round((0x82 * .75 - 48) * 10), delta=1)
        self.assertEqual(trans, (0x8E - 48) * 10)
        reqs = list(gw.requests)
        self.assertFalse([u for _, _, u in reqs if u[:3] == bytes([0x22, 0x42, 0x05])], "no BoostDeck fast reads")
        self.assertFalse([u for _, _, u in reqs if u[0] == 0x01], "no rpm/speed reads")
        self.assertLessEqual(len(reqs) / 4.5, 3.5, "about two requests a second")
        temps = [u for _, _, u in reqs if u[:3] == bytes([0x22, 0x44, 0x08])]
        self.assertTrue(temps and all(len(u) == 7 for u in temps), "oil, coolant, intake in one request")
        self.assertEqual(self.boost.packets, [], "nothing sent to BoostDeck's port")
        gw.close()

    def test_temps_and_boost_share_one_connection(self):
        gw = FakeGateway()
        self.start(gw, "boost")
        time.sleep(0.3)
        self.start(gw, "temps")
        time.sleep(3.5)
        self.assertEqual(gw.max_open, 1)
        frames = self.temps_frames()
        self.assertGreaterEqual(len(frames), 2)
        self.assertNotEqual(frames[-1][3], -2**31, "oil valid")
        self.assertGreater(len(self.boost_frames()), 10, "BoostDeck still served")
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
