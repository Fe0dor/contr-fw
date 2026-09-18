"""All generated interlock pairs, atomic rejection and canonical model replies."""
from contr_emu.device import Device

def test_every_pair_matches_first_plugin_group():
    d=Device()
    by_number={r['number']:r['name'] for r in d.relays.values()}
    for pair in d.plugin['groups']:
        numbers=set(pair['relays'])
        first=next(g for g in d.plugin['groups'] if set(g['relays'])<=numbers)
        names=','.join(by_number[n] for n in sorted(numbers))
        assert d.execute('console','ROUT:SET '+names)=='ERR:INTERLOCK,'+first['name']+','+names
        assert not d.relays_on
    for name in d.relays:
        assert d.execute('console','ROUT:SET '+name)=='OK'
        assert d.execute('console','ROUT:STAT?')==name

def test_rejection_safe_reset_and_diagnostics():
    d=Device()
    assert d.execute('console','rout:high m4,v1.1,m4')=='OK'
    assert d.execute('console','ROUT:STAT?')=='V1.1,M4'
    assert d.execute('console','ROUT:HIGH M5,L1')=='ERR:INTERLOCK,M__M4__M5,M4,M5'
    assert d.execute('console','ROUT:SET M99')=='ERR:RANGE'
    assert d.execute('console','ROUT:SET M5,')=='ERR:RANGE'
    assert 'SR0:OK' in d.execute('console','TEST:ALL?')
    assert d.execute('console','ROUT:STAT?')=='V1.1,M4'
    d.sr_loop_ok=False
    assert d.execute('console','TEST:ALL?').startswith('FAIL;SR0:FAIL;')
    d.reboot()
    assert d.execute('console','ROUT:STAT?')==''
    assert d.execute('console','ROUT:SET L1,L2,L3,L4,L5,PM1,PM2,PM3,CH2.1')=='OK'
    d.connect()
    assert not d.relays_on
    assert d.execute('console','ROUT:HIGH M4')=='ERR:BUSY'
    assert d.execute('tcp','ROUT:HIGH M4').endswith(';OK')
    assert d.execute('console','SAFE')=='OK'
    assert not d.relays_on
