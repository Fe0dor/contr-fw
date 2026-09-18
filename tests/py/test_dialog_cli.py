import os
import socket
import subprocess
import sys
from pathlib import Path


def test_dialog_cli_reports_utf8_with_cp1252_environment():
    root = Path(__file__).resolve().parents[2]
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    result = subprocess.run(
        [sys.executable, str(root/'tools/run_dialogs.py'), '--emu', '--port', str(port)],
        cwd=root, env=dict(os.environ, PYTHONIOENCODING='cp1252', PYTHONUTF8='0'),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=25,
    )
    output = result.stdout.decode('utf-8')
    assert result.returncode == 0, output + result.stderr.decode('utf-8', 'replace')
    assert '57 проверок, 0 с ошибками, 0 пропущено' in output
