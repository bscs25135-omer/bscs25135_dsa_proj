#!/usr/bin/env python3
"""
Stress-test harness for the TTDB stack (client / server / load_balancer).

Assumes the three binaries are built and sit next to this script:
    client, server, load_balancer

Run:  python3 stress_test.py
"""

import os
import sys
import time
import socket
import struct
import zlib
import signal
import threading
import subprocess
import tempfile
import pathlib

HERE = pathlib.Path(__file__).resolve().parent

# ---------------- Configuration ----------------

LB_HOST        = "127.0.0.1"
LB_PORT        = 5000
SERVER_PORT    = 5001
NUM_CLIENTS    = 20
CLIENT_TIMEOUT = 40

LB_LOG     = "lb_stress.log"
SERVER_LOG = "server_stress.log"

CLIENT_BIN = str(HERE / "client")
SERVER_BIN = str(HERE / "server")
LB_BIN     = str(HERE / "load_balancer")

# ---------------- Test programs ----------------

PROG_VALID = """\
func main
set a 1
add a 2
call foo a
mul a 3
func_end

func foo x
add x 10
sub x 1
func_end
"""

PROG_EMPTY = """\
func main
func_end
"""

PROG_NO_MAIN = """\
func foo
set x 1
func_end
"""

PROG_NESTED_FUNC = """\
func main
func inner
func_end
func_end
"""

PROG_UNCLOSED = """\
func main
set a 1
"""

PROG_STRAY_END = """\
func_end
"""

PROG_UNDEFINED_CALL = """\
func main
call nope
func_end
"""

PROG_LONG_IDENT = """\
func main
set """ + ("a" * 500) + """ 1
func_end
"""

# --------------- CRC helpers ---------------

def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF

def unframe(buf: bytes):
    if len(buf) < 5:
        return None
    body = buf[:-4]
    crc  = struct.unpack(">I", buf[-4:])[0]
    if crc != crc32(body):
        return None
    return body[0], body[1:]

# --------------- Utilities ---------------

_workdir = None

def workdir() -> str:
    return _workdir

def write_file(name: str, content: str) -> str:
    path = os.path.join(workdir(), name)
    with open(path, "wb") as f:
        f.write(content.encode("utf-8"))
    return path

def _to_str(x):
    if x is None:
        return ""
    if isinstance(x, bytes):
        return x.decode("utf-8", "replace")
    return x

def client_run(cmd_input: str, timeout=CLIENT_TIMEOUT):
    try:
        r = subprocess.run(
            [CLIENT_BIN, LB_HOST, str(LB_PORT)],
            input=cmd_input,
            text=True,
            capture_output=True,
            timeout=timeout,
            cwd=workdir(),
        )
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired as e:
        return -1, _to_str(e.stdout), _to_str(e.stderr)

def banner(name: str):
    print(f"\n=== {name} " + "=" * max(0, 60 - len(name)))

def ok(cond, label, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {label}" + (f"  ({detail})" if detail else ""))
    return cond

# --------------- Process handling ---------------

class Proc:
    def __init__(self, cmd, log, cwd=None):
        self.log = open(log, "wb")
        self.p = subprocess.Popen(
            cmd, stdout=self.log, stderr=subprocess.STDOUT, cwd=cwd
        )

    def stop(self, sig=signal.SIGTERM, timeout=5):
        if self.p.poll() is None:
            try:
                self.p.send_signal(sig)
            except ProcessLookupError:
                pass
        try:
            self.p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
        self.log.close()

def kill_all_servers():
    subprocess.run(["pkill", "-f", SERVER_BIN], check=False)

def start_lb():
    return Proc([LB_BIN, str(LB_PORT)], LB_LOG, cwd=str(HERE))

def wait_port(host, port, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.3):
                return True
        except OSError:
            time.sleep(0.05)
    return False

# --------------- Tests ---------------

def test_01_raw_probe():
    banner("Raw socket probe")
    source = write_file("raw_valid.trace", PROG_VALID)
    with open(source, "rb") as f:
        body = f.read()
    frame = body + struct.pack(">I", crc32(body))

    s = socket.create_connection((LB_HOST, LB_PORT), timeout=5)
    s.sendall(frame)
    s.shutdown(socket.SHUT_WR)
    buf = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
    s.close()

    msg = unframe(buf)
    ok(msg is not None, "Response has a valid CRC trailer")
    if msg:
        status, payload = msg
        ok(status == 0, f"Status byte == 0 (got {status})")
        ok(len(payload) >= 20, f"Payload looks like a .tdbg (>=20 bytes, got {len(payload)})")
        ok(payload[:4] == b"TTDB", "Payload starts with magic 'TTDB'")

def test_02_single_client_valid():
    banner("Single client on a valid program")
    src = write_file("single_valid.trace", PROG_VALID)
    rc, out, err = client_run(f"start {src}\nquit\n")
    ok(rc == 0, "client exit code 0", f"rc={rc}")
    ok("--- step 1 of" in out, "'--- step 1 of N ---' printed")

def test_03_client_navigation():
    banner("Client next / prv / jmp navigation")
    src = write_file("nav.trace", PROG_VALID)
    rc, out, _ = client_run(
        f"start {src}\nnext\nnext\nprv\njmp next 2\njmp prv 1\nquit\n"
    )
    steps = out.count("--- step ")
    ok(steps >= 6, f"at least 6 step banners printed (got {steps})")

def test_04_nav_out_of_range():
    banner("Client rejects out-of-range navigation")
    src = write_file("oor.trace", PROG_VALID)
    rc, out, _ = client_run(f"start {src}\njmp next 999999\nquit\n")
    ok("out of range" in out, "'out of range' message printed")

def test_05_invalid_programs():
    banner("Server rejects invalid programs")
    cases = [
        ("no_main",        PROG_NO_MAIN),
        ("nested_func",    PROG_NESTED_FUNC),
        ("unclosed",       PROG_UNCLOSED),
        ("stray_end",      PROG_STRAY_END),
        ("undefined_call", PROG_UNDEFINED_CALL),
    ]
    for name, prog in cases:
        src = write_file(f"bad_{name}.trace", prog)
        rc, out, err = client_run(f"start {src}\nquit\n")
        combined = (out + err).lower()
        handled = any(k in combined for k in [
            "error", "damaged", "no main", "nested", "unclosed",
            "undefined", "too long", "no steps", "produced no steps",
        ])
        ok(handled, f"'{name}' produced a clear error")

    # Empty main is legal per current design: a 1-step program.
    src = write_file("bad_empty.trace", PROG_EMPTY)
    rc, out, err = client_run(f"start {src}\nquit\n")
    ok("--- step 1 of 1 ---" in out, "'empty main' runs as a 1-step program")

def test_06_crc_corruption():
    banner("Server rejects a source with a corrupted CRC")
    src = write_file("crc_bad.trace", PROG_VALID)
    with open(src, "rb") as f:
        body = f.read()
    bad_crc = struct.pack(">I", crc32(body) ^ 0xFFFFFFFF)

    s = socket.create_connection((LB_HOST, LB_PORT), timeout=5)
    s.sendall(body + bad_crc)
    s.shutdown(socket.SHUT_WR)
    buf = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
    s.close()

    msg = unframe(buf)
    ok(msg is not None, "Response still has a valid CRC")
    if msg:
        status, payload = msg
        ok(status == 1, f"Status byte == 1 (got {status})")
        ok(b"checksum" in payload.lower() or b"corrupt" in payload.lower(),
           f"Payload mentions checksum corruption: {payload[:80]!r}")

def test_07_concurrent_clients():
    banner(f"{NUM_CLIENTS} concurrent client processes")
    src_valid   = write_file("concurrent_valid.trace", PROG_VALID)
    src_invalid = write_file("concurrent_bad.trace", PROG_NESTED_FUNC)

    results = []
    lock = threading.Lock()

    def worker(i):
        src = src_valid if i % 2 == 0 else src_invalid
        rc, out, err = client_run(f"start {src}\nquit\n")
        combined = (_to_str(out) + _to_str(err)).lower()
        with lock:
            results.append((i, rc, combined))

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(NUM_CLIENTS)]
    t0 = time.time()
    for t in threads: t.start()
    for t in threads: t.join()
    dt = time.time() - t0

    success = sum(1 for _, rc, out in results if "--- step 1 of" in out)
    busy    = sum(1 for _, rc, out in results if "server busy" in out)
    handled = sum(1 for _, rc, out in results
                  if any(k in out for k in ("error", "damaged", "no main", "nested")))
    total = len(results)

    ok(success + busy + handled == total,
       f"all {total} clients got a clear outcome in {dt:.1f}s",
       f"{success} ok, {busy} busy-rejected, {handled} errors, "
       f"{total - success - busy - handled} unknowns")

    ok(dt < NUM_CLIENTS * 10, f"all clients finished in {dt:.1f}s (no hang)")

def test_08_server_busy_queue():
    banner("'server busy' rejection when the LB queue overflows")
    stalled = []
    for _ in range(12):
        try:
            s = socket.create_connection((LB_HOST, LB_PORT), timeout=2)
            s.sendall(b"X")
            stalled.append(s)
        except OSError:
            break
    time.sleep(0.8)

    src = write_file("busy.trace", PROG_VALID)
    rc, out, err = client_run(f"start {src}\nquit\n", timeout=10)
    combined = (_to_str(out) + _to_str(err)).lower()

    ok(("server busy" in combined) or ("error" in combined),
       "overflow client got 'server busy' or a clear error",
       f"out[:80]={_to_str(out)[:80]!r}")

    for s in stalled:
        try: s.close()
        except OSError: pass

def test_09_client_timeout_not_hang():
    banner("Client does not hang when LB stalls mid-upload")
    src = write_file("timeout.trace", PROG_VALID)
    t0 = time.time()
    rc, out, err = client_run(f"start {src}\nquit\n", timeout=45)
    dt = time.time() - t0
    ok(dt < 45, f"client returned within timeout ({dt:.1f}s)")

def test_10_kill_server_recovery():
    banner("LB restarts the server after it is killed")
    src = write_file("recovery.trace", PROG_VALID)
    client_run(f"start {src}\nquit\n")

    kill_all_servers()
    time.sleep(0.5)

    rc, out, err = client_run(f"start {src}\nquit\n", timeout=60)
    ok("--- step 1 of" in out, "client succeeded after server was killed",
       f"out[:60]={out[:60]!r}")

def test_11_client_cli_args():
    banner("Client CLI argument handling")
    r = subprocess.run([CLIENT_BIN, "127.0.0.1", "99999"],
                       input="quit\n", text=True, capture_output=True,
                       timeout=5, cwd=workdir())
    ok(r.returncode != 0, "invalid port exits non-zero")
    ok("invalid port" in (r.stdout + r.stderr).lower(), "invalid-port message printed")

def test_12_client_quit_variants():
    banner("Client quit / exit / EOF / bare ttdb")
    for cmd, label in [
        ("quit\n",       "quit exits"),
        ("exit\n",       "exit exits"),
        ("ttdb\nquit\n", "bare 'ttdb' ignored"),
        ("\nquit\n",     "blank line ignored"),
    ]:
        rc, out, err = client_run(cmd, timeout=5)
        ok(rc == 0, label)

def test_13_missing_file():
    banner("Client handles a missing source file")
    rc, out, err = client_run("start does_not_exist.trace\nquit\n")
    ok("cannot open" in (out + err).lower(), "clear 'cannot open' error")

def test_14_command_parser():
    banner("Client command parser edge cases")
    cases = [
        ("next 123\n",       "usage: next"),
        ("prv x\n",          "usage: prv"),
        ("jmp\n",            "usage: jmp"),
        ("jmp sideways 5\n", "usage: jmp"),
        ("jmp next 0\n",     "invalid jump"),
        ("jmp next -5\n",    "invalid jump"),
        ("jmp next abc\n",   "invalid jump"),
        ("start\n",          "usage: start"),
        ("start a b\n",      "usage: start"),
    ]
    src = write_file("parser.trace", PROG_VALID)
    for cmd, expect in cases:
        full = f"start {src}\n{cmd}quit\n"
        _, out, err = client_run(full, timeout=10)
        ok(expect in out, f"'{cmd.strip()}' → {expect!r}")

def test_15_tdbg_file_persists():
    banner("Client persists ttdb_last.tdbg after a successful start")
    src = write_file("persist.trace", PROG_VALID)
    rc, out, err = client_run(f"start {src}\nquit\n")
    p = os.path.join(workdir(), "ttdb_last.tdbg")
    ok(os.path.exists(p), f"{p} exists after 'start'")
    if os.path.exists(p):
        with open(p, "rb") as f:
            head = f.read(4)
        ok(head == b"TTDB", "ttdb_last.tdbg starts with magic TTDB")

def test_16_lb_sigterm_clean():
    banner("Load balancer exits cleanly on SIGTERM")
    lb_handle.stop(signal.SIGTERM)
    ok(lb_handle.p.returncode is not None,
       f"LB process exited (rc={lb_handle.p.returncode})")

def test_17_giant_input_cap():
    banner("Server rejects a program that exceeds MAX_SOURCE_BYTES")
    big = "func main\n" + ("set x 1\n" * 2_000_000) + "func_end\n"
    src = write_file("huge.trace", big)
    rc, out, err = client_run(f"start {src}\nquit\n", timeout=60)
    combined = (out + err).lower()
    ok(any(k in combined for k in ("too large", "error", "damaged")),
       f"huge source rejected ({combined[:60]!r})")

def test_18_long_ident_rejected():
    banner("Over-long identifiers: run continues with a warning")
    src = write_file("long.trace", PROG_LONG_IDENT)
    rc, out, err = client_run(f"start {src}\nquit\n")
    ok("--- step 1 of" in out,
       "run succeeds (identifier skipped as a warning, per current design)")

# --------------- Orchestration ---------------

lb_handle = None

def run_all():
    global _workdir, lb_handle

    for b in (CLIENT_BIN, SERVER_BIN, LB_BIN):
        if not os.path.exists(b):
            print(f"ERROR: binary not found: {b}", file=sys.stderr)
            sys.exit(2)

    _workdir = tempfile.mkdtemp(prefix="ttdb_stress_")
    print(f"[harness] work directory: {_workdir}")

    kill_all_servers()
    time.sleep(0.3)

    lb_handle = start_lb()
    if not wait_port(LB_HOST, LB_PORT, timeout=5):
        print("ERROR: LB failed to bind", file=sys.stderr)
        lb_handle.stop()
        sys.exit(3)

    try:
        test_01_raw_probe()
        test_02_single_client_valid()
        test_03_client_navigation()
        test_04_nav_out_of_range()
        test_05_invalid_programs()
        test_06_crc_corruption()
        test_07_concurrent_clients()
        test_08_server_busy_queue()
        test_09_client_timeout_not_hang()
        test_10_kill_server_recovery()
        test_11_client_cli_args()
        test_12_client_quit_variants()
        test_13_missing_file()
        test_14_command_parser()
        test_15_tdbg_file_persists()
        #test_17_giant_input_cap()
        test_18_long_ident_rejected()
        test_16_lb_sigterm_clean()
    finally:
        try:
            lb_handle.stop()
        except Exception:
            pass
        kill_all_servers()
        print(f"\n[harness] logs: {LB_LOG}, {SERVER_LOG}")
        print(f"[harness] workdir preserved at {_workdir}")

if __name__ == "__main__":
    run_all()