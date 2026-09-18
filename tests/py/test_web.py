import base64
import json
import os
import queue
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, build_opener, ProxyHandler

import pytest

from contr_ui.bridge import Bridge, DeviceError
from contr_ui.web import Server
from contr_ui.workflows import UpdateImage
from test_workflows import image


ROOT = Path(__file__).resolve().parents[2]
urlopen = build_opener(ProxyHandler({})).open


def eventually(check, timeout=8):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        if check():
            return
        time.sleep(.02)
    raise AssertionError("Condition did not become true")


@pytest.fixture
def emulator():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    env = dict(os.environ, PYTHONPATH=str(ROOT / 'emu') + os.pathsep + os.environ.get('PYTHONPATH', ''))
    proc = subprocess.Popen([sys.executable, '-m', 'contr_emu', '--port', str(port), '--console'],
                            env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    # A thread keeps startup failures bounded instead of blocking on readline.
    ready = threading.Event()
    threading.Thread(target=lambda: (proc.stderr.readline(), ready.set()), daemon=True).start()
    assert ready.wait(5)
    assert proc.poll() is None
    yield {'mode': 'emulator', 'host': '127.0.0.1', 'port': port}, proc
    proc.kill()
    proc.wait(timeout=5)


@pytest.fixture
def bridge(tmp_path):
    b = Bridge(tmp_path)
    yield b
    b.close()


def connect(bridge, emulator):
    bridge.submit('connect', target=emulator[0]).result(10)


def test_panels_connect_and_busy_owner(bridge, emulator, tmp_path):
    connect(bridge, emulator)
    s = bridge.snapshot()
    assert s['connected']
    assert s['replies']['*IDN?'].endswith('0.2.0')
    assert len(s['relays']) == 81
    assert 'M4' in s['commands']
    assert 'BOARD_REV=1' in s['replies']['SYST:CONF?']
    other = Bridge(tmp_path / 'other')
    try:
        with pytest.raises(DeviceError, match='BUSY'):
            other.submit('connect', target=emulator[0]).result(5)
        assert not other.snapshot()['connected']
    finally:
        other.close()
    bridge.safe()
    eventually(lambda: bridge.snapshot()['safe_acknowledged'])
    assert 'SAFE' in bridge.log_path.read_text('utf-8')


def test_script_stop_on_error_and_operator_prompt(bridge, emulator):
    connect(bridge, emulator)
    bridge.start_job('script', text='*IDN?\nBAD\nNEVER_SENT')
    eventually(lambda: bridge.snapshot()['job']['status'] == 'failed')
    assert not any(e['kind'] == 'tx' and e['text'] == 'NEVER_SENT' for e in bridge.snapshot()['events'])
    bridge.start_job('script', text='#prompt Нажмите USER\n*IDN?')
    eventually(lambda: bridge.snapshot()['job']['status'] == 'waiting')
    bridge.answer_prompt(True)
    eventually(lambda: bridge.snapshot()['job']['status'] == 'done')


def test_safe_interrupts_script_wait(bridge, emulator):
    connect(bridge, emulator)
    bridge.start_job('script', text='#wait 60000\nSHOULD_NOT_RUN')
    eventually(lambda: bridge.snapshot()['job']['message'] == '60000')
    bridge.safe()
    eventually(lambda: bridge.snapshot()['job']['status'] == 'cancelled')
    eventually(lambda: bridge.snapshot()['safe_acknowledged'])
    assert not any(e['kind'] == 'tx' and e['text'] == 'SHOULD_NOT_RUN' for e in bridge.snapshot()['events'])


def test_master_upload_confirm_and_version(bridge, emulator):
    connect(bridge, emulator)
    bridge.start_job('update', data=image(), confirm=True)
    eventually(lambda: bridge.snapshot()['job']['status'] in {'done', 'failed'}, timeout=12)
    s = bridge.snapshot()
    assert s['job']['status'] == 'done', s['job']
    assert s['replies']['*IDN?'].endswith('0.3.0')
    assert s['replies']['SYST:UPD:STAT?'].startswith('CONFIRMED')


def test_master_rollback_reports_device_evidence(bridge, emulator):
    connect(bridge, emulator)
    bridge.start_job('update', data=image(), confirm=False)
    eventually(lambda: bridge.snapshot()['job']['status'] == 'trial')
    emulator[1].stdin.write('#advance 60001\n')
    emulator[1].stdin.flush()
    eventually(lambda: bridge.snapshot()['job']['status'] in {'rolled_back', 'failed'}, timeout=12)
    s = bridge.snapshot()
    assert s['job']['status'] == 'rolled_back', s['job']
    assert s['replies']['*IDN?'].endswith('0.2.0')


def test_safe_reply_cannot_be_mistaken_for_next_command(bridge):
    class FakeLink:
        label = 'fake'
        def __init__(self):
            self.lines = queue.Queue()
            self.started = threading.Event()
        def send(self, command):
            if command == 'LONG':
                self.started.set()
            elif command == 'SAFE':
                self.lines.put('G1;ERR:ABORTED')
                self.lines.put('G2;OK')
            else:
                self.lines.put('G2;TESTDUT,CONTR-R1,X,0.2.0')
        def close(self):
            pass
    wire = FakeLink()
    bridge.link = wire
    bridge.state['connected'] = True
    bridge.next_poll = time.monotonic() + 100
    future = bridge.submit('command', command='LONG')
    assert wire.started.wait(2)
    bridge.safe()
    assert future.result(3)['reply'] == 'G1;ERR:ABORTED'
    assert bridge.submit('command', command='*IDN?').result(3)['reply'].endswith('0.2.0')
    assert wire.lines.empty()


def test_master_does_not_confirm_unexpected_version(bridge, monkeypatch):
    sent = []
    bridge.target = {'mode': 'emulator'}
    bridge.state['job'] = {'status': 'running'}
    monkeypatch.setattr(bridge, '_require_ok', lambda cmd: sent.append(cmd))
    monkeypatch.setattr(bridge, '_open', lambda target: None)
    monkeypatch.setattr(bridge, '_exchange', lambda cmd: 'G1;TRIAL,0,0' if cmd == 'SYST:UPD:STAT?' else 'G1;TESTDUT,CONTR-R1,X,0.2.0')
    with pytest.raises(DeviceError, match='CONFIRM'):
        bridge._run_update(UpdateImage.read(image()))
    assert 'SYST:UPD:CONFIRM' not in sent


def test_http_origin_token_validation_and_assets(bridge):
    server = Server(('127.0.0.1', 0), bridge)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    url = f'http://127.0.0.1:{server.server_port}'
    try:
        with urlopen(url) as response:
            assert b'CONTR' in response.read()
            assert "frame-ancestors 'none'" in response.headers['Content-Security-Policy']
        with urlopen(url + '/api/bootstrap') as response:
            token = json.load(response)['token']
        for headers in ({'Content-Type': 'application/json'},
                        {'Content-Type': 'application/json', 'X-Contr-Token': token, 'Origin': 'https://other.example'}):
            with pytest.raises(HTTPError) as exc:
                urlopen(Request(url + '/api/poll', data=b'{"ms":1000}', headers=headers))
            assert exc.value.code == 403
        with urlopen(Request(url + '/api/poll', data=b'{"ms":1000}',
                             headers={'Content-Type':'application/json','X-Contr-Token':token})) as response:
            assert response.status == 200
        assert bridge.snapshot()['poll_ms'] == 1000
    finally:
        server.shutdown()
        server.server_close()


def test_safe_cancels_queued_manual_commands(bridge):
    from contr_ui.bridge import Cancelled

    class Wire:
        label = 'fake'
        def __init__(self):
            self.lines = queue.Queue()
            self.started = threading.Event()
            self.sent = []
        def send(self, command):
            self.sent.append(command)
            if command == 'LONG':
                self.started.set()
            elif command == 'SAFE':
                self.lines.put('G1;ERR:ABORTED')
                self.lines.put('G2;OK')
            else:
                self.lines.put('G2;NEW')
        def close(self):
            pass

    wire = Wire()
    bridge.link = wire
    bridge.state['connected'] = True
    bridge.next_poll = time.monotonic() + 100
    active = bridge.submit('command', command='LONG')
    assert wire.started.wait(2)
    queued = bridge.submit('command', command='OLD_SET')
    bridge.safe()
    assert active.result(3)['reply'] == 'G1;ERR:ABORTED'
    with pytest.raises(Cancelled):
        queued.result(3)
    assert wire.sent == ['LONG', 'SAFE']
    assert bridge.snapshot()['safe_acknowledged']
    assert bridge.submit('command', command='NEW_SET').result(3)['reply'] == 'G2;NEW'
    assert wire.sent == ['LONG', 'SAFE', 'NEW_SET']


def test_safe_cancels_command_already_dequeued(bridge, monkeypatch):
    from contr_ui.bridge import Cancelled
    entered, release = threading.Event(), threading.Event()
    sent = []
    class Wire:
        label = 'fake'
        lines = queue.Queue()
        def send(self, command):
            sent.append(command)
            self.lines.put('G2;OK' if command == 'SAFE' else 'G2;NEW')
        def close(self):
            pass
    bridge.link = Wire()
    bridge.state['connected'] = True
    bridge.next_poll = time.monotonic() + 100
    exchange = bridge._exchange
    def barrier(command, **kwargs):
        if command == 'OLD_SET':
            entered.set()
            assert release.wait(3)
        return exchange(command, **kwargs)
    monkeypatch.setattr(bridge, '_exchange', barrier)
    future = bridge.submit('command', command='OLD_SET')
    try:
        assert entered.wait(2)
        bridge.safe()
    finally:
        release.set()
    with pytest.raises(Cancelled):
        future.result(3)
    eventually(lambda: bridge.snapshot()['safe_acknowledged'])
    assert sent == ['SAFE']
    assert bridge.submit('command', command='NEW_SET').result(3)['reply'] == 'G2;NEW'
    assert sent == ['SAFE', 'NEW_SET']
