from contr_emu.device import Device

def test_manual_debounce_read_clear_and_release():
    d=Device()
    d.dut.button('C3', True); d.advance(20)
    d.dut.button('C3', False); d.advance(50)
    assert d.generation == 0
    assert 'INTERVENTION,0' in d.execute('console','DUT:STAT?')
    d.dut.button('C3', True); d.advance(39)
    assert 'C3,OFF' in d.execute('console','DUT:STAT?')
    d.advance(1)
    assert d.generation == 1
    reply=d.execute('console','DUT:STAT?')
    assert 'C3,MANUAL' in reply and 'INTERVENTION,1' in reply
    assert 'INTERVENTION,0' in d.execute('console','DUT:STAT?')
    d.dut.button('C3', False); d.advance(40)
    assert d.generation == 2
    assert 'C3,OFF' in d.execute('console','DUT:STAT?')

def test_validation_and_safe_preserves_buttons():
    d=Device()
    assert d.execute('console','DUT:SET C1,C99,ON') == 'ERR:RANGE'
    assert not d.dut.on
    assert d.execute('console','DUT:SET C1,c1,ON') == 'ERR:RANGE'
    assert d.execute('console','DUT:SET '+','.join(f'C{i}' for i in range(1,21))+',ON') == 'OK'
    assert len(d.dut.on) == 20
    d.execute('console','DUT:OFF:ALL')
    d.dut.button('C1',True);d.advance(40)
    d.execute('console','SAFE')
    assert 'C1,MANUAL' in d.execute('console','DUT:STAT?')
    d.execute('console','DUT:SET C1,ON');d.advance(40)
    assert 'C1,ON' in d.execute('console','DUT:STAT?')
    d.execute('console','DUT:OFF:ALL');d.advance(40)
    assert 'C1,MANUAL' in d.execute('console','DUT:STAT?')
