from contr_emu.device import Device
from contr_ui.workflows import UpdateImage
from test_workflows import image

CODES=[38000+i*1000+i*i for i in range(15)]
def cmd(d,c):return d.execute('console',c)
def install(d):
    assert cmd(d,'PSU:CAL:SET '+','.join(map(str,CODES)))=='OK'
    assert cmd(d,'PSU:CAL:SAVE')=='OK'

def test_missing_calibration_service_and_disconnect():
    d=Device()
    assert cmd(d,'PSU:OUTP RSP,ON')=='ERR:CAL_REQUIRED'
    assert cmd(d,'PSU:CAL:MODE ON')=='OK'
    assert cmd(d,'PSU:OUTP RSP,ON')=='ERR:CAL_REQUIRED'
    assert cmd(d,'PSU:CAL:CODE 47765')=='OK'
    assert cmd(d,'PSU:OUTP RSP,ON')=='OK'
    assert cmd(d,'PSU:VOLT 48')=='ERR:CAL_MODE'
    d.psu.inject('ALARM','1');assert not d.psu.rsp
    d.psu.inject('ALARM','0');assert not d.psu.rsp
    assert d.connect();assert not d.psu.service_mode
    assert d.execute('tcp','PSU:CAL:MODE ON').endswith(';OK')
    d.disconnect();assert not d.psu.service_mode and not d.psu.rsp

def test_calibration_persistence_failures_and_update():
    d=Device();install(d)
    for mv in range(40000,54001):
        assert cmd(d,f'PSU:VOLT {mv//1000}.{mv%1000:03d}')=='OK'
        i,f=divmod(mv-40000,1000)
        expected=CODES[i]+((CODES[i+1]-CODES[i])*f+500)//1000 if f else CODES[i]
        assert d.psu.code==expected
    assert cmd(d,'PSU:CAL:SAVE')=='ERR:NOT_SAFE'
    assert cmd(d,'SAFE')=='OK'
    changed=[v+1 for v in CODES]
    assert cmd(d,'PSU:CAL:SET '+','.join(map(str,changed)))=='OK'
    d.psu.inject('CAL_FAIL','1');assert cmd(d,'PSU:CAL:SAVE')=='ERR:CAL_STORAGE'
    assert d.psu_cal_flash==CODES and d.psu.cal==changed
    assert cmd(d,'PSU:CAL:LOAD')=='OK';assert d.psu.cal==CODES
    d.reboot();assert d.psu.cal==CODES
    for c in UpdateImage.read(image()).commands():assert cmd(d,c)=='OK'
    d.reboot();assert d.psu.cal==CODES
    assert cmd(d,'PSU:CAL:SAVE')=='ERR:BUSY,update'
    assert cmd(d,'PSU:CAL:MODE ON')=='ERR:BUSY,update'
    d.advance(60001);assert d.active_bank==1 and d.psu.cal==CODES
