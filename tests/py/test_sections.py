from contr_emu.device import Device

def test_sections_defaults_ranges_and_matrix_neighbours():
    d=Device()
    assert d.execute('console','RES:PWR A,ON')=='OK'
    assert 'R2,POTH,512' in d.execute('console','RES:STAT?')
    assert d.execute('console','RES:SET R2,MTX,7')=='OK'
    assert d.execute('console','RES:SET R1,MTX,0')=='OK'
    assert 'R2,MTX,7' in d.execute('console','RES:STAT?')
    for raw in ('-1','100.5','257','abc','999999999999999999999999999'):
        assert d.execute('console','RES:SET R1,POT,'+raw)=='ERR:RANGE,0..256'
    d.execute('console','SAFE')
    assert d.sections.on == [False,False]
    assert all(v is None for c in d.sections.codes for v in c)

def test_sections_recovery_ladder_and_generation():
    d=Device();d.connect();d.execute('tcp','RES:PWR A,ON')
    d.sections.inject(0,0x2f,'transient')
    assert d.execute('tcp','RES:SET R1,POT,10').endswith(';OK')
    assert 'RETRY' in d.execute('tcp','SYST:ERR?')
    d.execute('tcp','RES:SET R2,MTX,7');d.sections.inject(0,0x27,'reset')
    assert d.execute('tcp','RES:SET R1,MTX,1').endswith(';OK')
    assert d.sections.codes[0][4]==7
    assert 'RESET' in d.execute('tcp','SYST:ERR?')
    d.execute('tcp','RES:SET R2,POT,10');g=d.generation
    d.sections.inject(0,0x2f,'power')
    assert d.execute('tcp','RES:SET R1,POT,20')==f'G{g+1};OK'
    assert d.sections.codes[0][2]==128
    d.sections.inject(0,0x2e,'missing')
    assert ';ERR:I2C_FAIL,R2,POT,' in d.execute('tcp','RES:SET R2,POT,10')
    assert not d.sections.on[0]

def test_section_boot_failure_and_console_ownership():
    d=Device();d.sections.inject(1,0x2c,'missing')
    assert d.execute('console','RES:PWR B,ON')=='ERR:SECTION_FAIL,4'
    d.sections.pwrok_present=True
    assert d.execute('console','RES:PWR A,ON')=='ERR:SECTION_FAIL,2'
    d.connect()
    assert d.execute('console','RES:PWR A,ON')=='ERR:BUSY'
    assert d.execute('console','RES:STAT?').startswith('A:OFF;B:OFF')

def test_reboot_invalidates_section_codes():
    d=Device();d.execute('console','RES:PWR A,ON');d.reboot()
    assert not any(d.sections.on)
    assert all(v is None for c in d.sections.codes for v in c)
