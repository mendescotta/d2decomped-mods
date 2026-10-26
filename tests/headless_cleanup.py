#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise a mod runner's failure cleanup with a small fake devctl server.

Usage: python3 tests/headless_cleanup.py tests/roomstate_d2d.py
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path


FAKE = '''#!/usr/bin/env python3
import os, socket, sys
from pathlib import Path
sock = sys.argv[sys.argv.index("--devctl") + 1]
Path(os.environ["FAKE_PID_FILE"]).write_text(str(os.getpid()) + "\\n" + os.environ["HOME"])
with socket.socket(socket.AF_UNIX) as server:
    server.bind(sock)
    server.listen()
    while True:
        connection, _ = server.accept()
        with connection:
            line = connection.recv(4096).decode().strip()
            connection.sendall(b"screen=ingame life=1500/1600\\nok\\n" if line == "state" else b"ok\\n")
            if line == "quit": break
'''


def main():
    runner = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="d2d_cleanup_check_") as directory:
        root = Path(directory)
        (root / "d2data.mpq").touch()
        patch = root / "patch.exe"
        patch.touch()
        fake = root / "fake_d2d.py"
        fake.write_text(FAKE)
        fake.chmod(0o755)
        pid_file = root / "pid"
        env = {**os.environ, "D2_MPQ_DIR": directory, "D2_PATCH_INSTALLER": str(patch),
               "D2D_REQUIRE_GAME_DATA": "1", "D2D_TEST_FAIL_AFTER_START": "1", "FAKE_PID_FILE": str(pid_file)}
        result = subprocess.run([sys.executable, str(runner), str(fake)], env=env, capture_output=True, text=True, timeout=30)
        assert result.returncode != 0 and "injected failure" in result.stderr, result.stdout + result.stderr
        pid_text, home = pid_file.read_text().splitlines()
        try:
            os.kill(int(pid_text), 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError(f"headless d2d pid {pid_text} survived runner failure")
        assert not Path(home).exists(), f"temporary home survived: {home}"
        print("headless cleanup: PASS")


if __name__ == "__main__":
    main()
