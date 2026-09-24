#!/usr/bin/env python3
"""Exercise preset management against the real frontend with isolated user files.
Usage: python3 frontends/gpu/tests/test_editor_ipc.py build/bin/mynes_gpu
Requires a window-capable SDL/Metal session; never touches the user's presets.
"""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[3]
binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="mynes-editor-") as tmp:
    # A foreign checkout in cwd must not shadow the executable's library.
    foreign=Path(tmp)/"presets"; foreign.mkdir()
    (foreign/"studio_pvm.json").write_text('{"name":"Wrong checkout","description":"stale"}')
    sockpath = str(Path(tmp) / "editor.sock")
    env = dict(os.environ, XDG_CONFIG_HOME=tmp, MYNES_DEBUG_SOCKET=sockpath, MYNES_REVIEW_NO_INPUT="1")
    with open(Path(tmp) / "frontend.log", "w+") as log:
        process = subprocess.Popen([str(binary), "--debug-server", "--offscreen", "640x480", "--preset", "studio_pvm"], cwd=tmp, env=env, stdout=log, stderr=log)
        try:
            for _ in range(100):
                if Path(sockpath).exists():
                    break
                if process.poll() is not None:
                    raise RuntimeError("Frontend exited during startup")
                time.sleep(0.1)
            client = socket.socket(socket.AF_UNIX)
            client.settimeout(8)
            client.connect(sockpath)

            def read(n):
                data = b""
                while len(data) < n:
                    chunk = client.recv(n - len(data))
                    assert chunk, "Disconnected"
                    data += chunk
                return data

            def receive(kind):
                for _ in range(1000):
                    t, n = struct.unpack("<II", read(8))
                    data = read(n)
                    if t == kind:
                        return data
                raise AssertionError("Missing message")

            def catalog():
                data = receive(7)
                version, revision, count, active, dirty = struct.unpack_from("<IIIiI", data)
                assert version == 1 and len(data) == 20 + count * 136
                entries = []
                for i in range(count):
                    ident, user, name = struct.unpack_from("<II128s", data, 20 + i * 136)
                    entries.append((ident, bool(user), name.split(b"\0")[0].decode()))
                return revision, active, bool(dirty), entries

            def command(op, ident, revision, name="", success=True):
                body = struct.pack("<III128s", op, ident & 0xffffffff, revision, name.encode())
                client.sendall(struct.pack("<II", 8, len(body)) + body)
                result = receive(9)
                actual_op, ok = struct.unpack_from("<II", result)
                assert actual_op == op and bool(ok) == success, result
                return catalog()

            def edit(value, name=b"Saturation"):
                data = receive(5)
                _, count = struct.unpack_from("<II", data)
                records = [struct.unpack_from("<Ifff32s24s", data, 8 + i * 72) for i in range(count)]
                ident = next(r[0] for r in records if r[4].split(b"\0")[0] == name)
                packet = struct.pack("<IIIf", 6, 8, ident, value)
                client.sendall(packet[:3])
                time.sleep(0.02)
                client.sendall(packet[3:])
                for _ in range(40):
                    state = catalog()
                    if state[2]:
                        return state
                raise AssertionError("Live edit not marked modified")

            revision, active, dirty, entries = catalog()
            assert active >= 0 and not dirty, (active, dirty)
            expected=json.loads((root/"presets/studio_pvm.json").read_text())["name"]
            assert entries[active][2]==expected, "Loaded the launch directory's stale preset"
            assert all(e[2]!="Wrong checkout" for e in entries)
            # A macOS menu tracking loop can temporarily stall MainActor reads.
            # Backpressure must not disconnect the editor or lose its selection.
            time.sleep(8)
            edit(0.85)
            revision, active, dirty, entries = command(3, active, revision, "IPC test CRT")
            assert not dirty and entries[active][1:]==(True,"IPC test CRT")
            user_id = active
            paths = list((Path(tmp)/"mynes/presets").glob("*.json"))
            assert len(paths)==1 and abs(json.loads(paths[0].read_text())["tv"]["saturation"]-.85)<1e-5
            edit(0.75)
            edit(1000, b"Reservoir (uF)")
            edit(9.5, b"Adaptor (VAC)")
            edit(0.18, b"Horizontal streaks")
            edit(12, b"Slow decay (ms)")
            edit(.03, b"Slow decay energy")
            edit(.65, b"RF IF asymmetry")
            edit(160, b"Y/C delay ns")
            edit(.2, b"Matte scatter")
            edit(24, b"Recovery (us)")
            revision, active, dirty, entries = command(2, active, revision)
            assert not dirty and abs(json.loads(paths[0].read_text())["tv"]["saturation"]-.75)<1e-5
            saved = json.loads(paths[0].read_text())
            assert saved["psu"]["reservoir_uf"]==1000
            assert abs(saved["psu"]["adaptor_vac"]-9.5)<1e-5
            assert abs(saved["tv"]["video_black_droop"]-.18)<1e-5
            assert saved["tv"]["persistence_tail_ms"]==12
            assert abs(saved["tv"]["persistence_tail_weight"]-.03)<1e-5
            assert abs(saved["rf"]["if_asymmetry"]-.65)<1e-5
            assert saved["vhs"]["yc_delay_ns"]==160
            assert abs(saved["tv"]["antiglare_blur"]-.2)<1e-5
            assert saved["tv"]["video_recovery_us"]==24
            revision, active, dirty, entries = command(4, active, revision, "Renamed CRT")
            assert entries[active][2]=="Renamed CRT"
            bundled = next(e[0] for e in entries if not e[1] and "Bedroom" in e[2])
            time.sleep(8)  # Preset selection must also survive a stalled reader.
            revision, active, dirty, entries = command(1, bundled, revision)
            assert active == bundled and not dirty
            command(5, bundled, revision, success=False)
            command(1, user_id, revision-1, success=False)
            revision, active, dirty, entries = command(1, user_id, revision)
            assert active == user_id and not dirty
            data = receive(5)
            _, count = struct.unpack_from("<II", data)
            controls = [struct.unpack_from("<Ifff32s24s", data, 8+i*72) for i in range(count)]
            hum = next(c[1] for c in controls if c[4].split(b"\0")[0] == b"Mains hum")
            assert abs(hum-.012)<1e-5
            revision, active, dirty, entries = command(5, user_id, revision)
            assert active == -1 and dirty and not paths[0].exists()
            print("Preset IPC: executable-relative library, active preset, dirty state, stalled reader, fragmented edits, save-as, save, rename, topology switch, bundled protection, stale revision rejection, delete: PASS")
            client.close()
        except Exception:
            print(f"Frontend exit status: {process.poll()}",file=sys.stderr)
            log.flush(); log.seek(0); print(log.read(), file=sys.stderr)
            raise
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
