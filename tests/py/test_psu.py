from contr_emu.device import Device as BareDevice

def Device():
    d=BareDevice()
    d.psu_cal_flash=[i*4000 for i in range(15)]
    d.psu.cal=list(d.psu_cal_flash)
    return d


def command(d, text):
    return d.execute("console", text)


def test_voltage_validation_and_mapping():
    d = Device()
    for volts, code in [("40", 0), ("47", 28000), ("54", 56000), ("48.125", 32500)]:
        assert command(d, "PSU:VOLT " + volts) == "OK"
        assert d.psu.code == code
    before = d.psu.status()
    for value in ["39.999", "54.001", "NaN", "48e0", "-42", "42.", "42.0000", "9" * 300]:
        assert command(d, "PSU:VOLT " + value).startswith("ERR:RANGE")
        assert d.psu.status() == before


def test_ready_local_fault_and_explicit_restart():
    d = Device()
    assert command(d, "PSU:OUTP HLG,ON") == "OK"
    assert command(d, "DUT:SET C1,ON") == "OK"
    assert command(d, "PSU:OUTP RSP,ON") == "OK"
    assert d.now_ms >= 50
    g = d.generation
    d.psu.inject("DCOK", "0")
    assert not d.psu.rsp and d.psu.hlg and d.dut.on
    assert d.generation == g
    assert d.psu.fault == "PSU_FAULT"
    d.psu.inject("DCOK", "AUTO"); d.advance(10000)
    assert not d.psu.rsp
    assert command(d, "PSU:OUTP RSP,ON") == "OK"
    g = d.generation
    d.psu.inject("ALARM", "1")
    assert not d.psu.rsp and d.psu.fault == "PSU_ALARM"
    assert command(d, "PSU:OUTP RSP,ON").startswith("ERR:PSU_ALARM")
    d.psu.inject("ALARM", "0"); d.advance(100)
    assert not d.psu.rsp and d.psu.hlg and d.generation == g


def test_timeout_safe_and_dac_failure():
    d = Device(); d.psu.inject("DCOK", "0")
    start = d.now_ms
    assert command(d, "PSU:OUTP RSP,ON") == "ERR:PSU_FAULT,DCOK timeout"
    assert d.now_ms - start == 2000 and not d.psu.rsp
    d.psu.inject("DCOK", "1")
    command(d, "DUT:SET C1,ON"); command(d, "PSU:OUTP HLG,ON")
    assert command(d, "SAFE").startswith("ERR:SAFE_INCOMPLETE")
    assert not d.psu.rsp and not d.psu.hlg and not d.dut.on
    assert d.safe.steps[3] == "FAIL"
    assert command(d, "SYST:UPD:BEGIN 1024,0,0.6.1").startswith("ERR:NOT_SAFE")
    d.psu.inject("DCOK", "0"); d.psu.inject("DAC_FAIL", "1")
    assert command(d, "PSU:VOLT 48").startswith("ERR:PSU_FAULT")
    assert command(d, "PSU:OUTP RSP,ON").startswith("ERR:PSU_FAULT")
    assert command(d, "SAFE").startswith("ERR:SAFE_INCOMPLETE")
    assert d.safe.steps[0] == "FAIL" and not d.psu.known
    d.psu.inject("DAC_FAIL", "0")
    assert command(d, "SAFE") == "OK"
    assert d.psu.code == 0 and d.psu.known


def test_external_faults_survive_reset_and_trial_rollback():
    from contr_ui.workflows import UpdateImage
    from test_workflows import image
    for trial in (False, True):
        d = Device()
        if trial:
            for cmd in UpdateImage.read(image()).commands():
                assert command(d, cmd) == 'OK'
        d.psu.inject('ALARM', '1')
        d.psu.inject('DCOK', '0')
        d.psu.inject('DAC_FAIL', '1')
        for _ in range(2):
            d.reboot()
            assert d.psu.alarm and d.psu.dcok_mode == '0' and d.psu.dac_fail
            assert not d.psu.rsp and not d.psu.hlg and not d.psu.known
            assert d.safe.steps[0] == 'FAIL'
            assert command(d, 'PSU:OUTP RSP,ON') == 'ERR:PSU_ALARM'
        assert d.active_bank == 1
        d.psu.inject('ALARM', '0'); d.psu.inject('DAC_FAIL', '0')
        d.psu.inject('DCOK', '1'); d.reboot()
        assert d.psu.dcok() and command(d, 'SAFE').startswith('ERR:SAFE_INCOMPLETE')


def test_dcok_raw_level_is_opposite_logical_status():
    d = Device()
    assert d.psu.status().endswith(",0,0")
    assert ";DCOK:1;" in command(d, "TEST:ALL?")
    assert command(d, "PSU:OUTP RSP,ON") == "OK"
    assert d.psu.status().endswith(",1,0")
    assert ";DCOK:0;" in command(d, "TEST:ALL?")
