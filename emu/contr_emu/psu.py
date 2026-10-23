"""RSP/HLG model with DCOK, ALARM and DAC failure events."""
import re
class Psu:
    def __init__(self, device, error):
        self.device,self.error=device,error
        self.rsp=self.hlg=self.starting=False
        self.mv=40000;self.code=0;self.known=True;self.fault=''
        self.cal=list(device.psu_cal_flash) if device.psu_cal_flash is not None else None
        self.service_mode=False;self.raw_ready=False;self.cal_fail=False
        self.code=self.cal[0] if self.cal else 0
        self.dcok_mode='AUTO';self.alarm=False;self.dac_fail=False;self.started=0
    def dcok(self):
        return (self.rsp and self.device.now_ms-self.started>=50) if self.dcok_mode=='AUTO' else self.dcok_mode=='1'
    def inject(self,signal,value):
        if signal=='DCOK' and value in ('AUTO','0','1'):self.dcok_mode=value
        elif signal=='ALARM' and value in ('0','1'):self.alarm=value=='1'
        elif signal=='DAC_FAIL' and value in ('0','1'):self.dac_fail=value=='1'
        elif signal=='CAL_FAIL' and value in ('0','1'):self.cal_fail=value=='1'
        else:raise ValueError('PSU event')
        self.poll()
    def trip(self,code,detail):
        self.rsp=self.starting=False;self.fault=code
        self.device.push_error(code,detail);self.device.log_event(f'RSP {code} {detail}')
    def poll(self):
        if not self.rsp:return
        if self.alarm:self.trip('PSU_ALARM','ALARM')
        elif not self.starting and not self.dcok():self.trip('PSU_FAULT','DCOK lost')
    def volt(self,args):
        s=args[0]
        if not re.fullmatch(r'[0-9]+(?:\.[0-9]{1,3})?',s):raise self.error('RANGE','40..54')
        whole,_,frac=s.partition('.')
        whole=whole.lstrip('0') or '0'
        if len(whole)>2:raise self.error('RANGE','40..54')
        mv=int(whole)*1000+int(frac.ljust(3,'0'))
        if not 40000<=mv<=54000:raise self.error('RANGE','40..54')
        if self.service_mode:raise self.error('CAL_MODE')
        if self.cal is None:raise self.error('CAL_REQUIRED')
        if self.dac_fail:
            self.device.safe.steps[0]='FAIL';self.device.in_safe_state=False;self.known=False;self.rsp=self.starting=False;self.fault='PSU_FAULT';raise self.error('PSU_FAULT','DAC')
        i,f=divmod(mv-40000,1000)
        self.mv=mv;self.code=self.cal[i]+((self.cal[i+1]-self.cal[i])*f+500)//1000 if f else self.cal[i];self.known=True
        return 'OK'
    def outp(self,args):
        source,action=(a.upper() for a in args)
        if source not in ('RSP','HLG') or action not in ('ON','OFF'):raise self.error('RANGE')
        if source=='HLG':self.hlg=action=='ON';return 'OK'
        if action=='OFF':self.rsp=self.starting=False;return 'OK'
        if (not self.raw_ready if self.service_mode else self.cal is None):raise self.error('CAL_REQUIRED')
        if self.alarm:
            self.rsp=self.starting=False;self.fault='PSU_ALARM';raise self.error('PSU_ALARM')
        if self.rsp and self.dcok():return 'OK'
        if not self.known or self.dac_fail:
            self.device.safe.steps[0]='FAIL';self.device.in_safe_state=False;self.known=False;self.rsp=self.starting=False;self.fault='PSU_FAULT';raise self.error('PSU_FAULT','DAC')
        self.rsp=self.starting=True;self.fault='';self.started=self.device.now_ms
        while True:
            self.device.advance(1)
            if not self.rsp:raise self.error(self.fault)
            if self.dcok():self.starting=False;return 'OK'
            if self.device.now_ms-self.started>=2000:
                self.rsp=self.starting=False;self.fault='PSU_FAULT';raise self.error('PSU_FAULT','DCOK timeout')
    def safe_off(self):
        self.rsp=self.hlg=self.starting=False;self.mv=40000
        self.service_mode=self.raw_ready=False
        if self.dac_fail:self.device.safe.steps[0]='FAIL';self.device.in_safe_state=False;self.known=False;self.trip('PSU_FAULT','DAC safe');return False
        self.known=True;self.code=self.cal[0] if self.cal else 0;return True
    def wait_off(self):
        start=self.device.now_ms
        while self.dcok():
            self.device.advance(1)
            if self.device.now_ms-start>=2000:self.trip('PSU_FAULT','DCOK stuck');return False
        return True
    def status(self):
        voltage=str(self.mv//1000)+(f'.{self.mv%1000:03d}' if self.mv%1000 else '')
        rsp=('ON' if self.rsp else 'OFF')+(':'+self.fault if self.fault else '')
        return f"{rsp},{'ON' if self.hlg else 'OFF'},{voltage},{self.code if self.known else '?'},{int(self.dcok())},{int(self.alarm)}"

    def cal_guard(self,safe=True):
        if self.device.upd_state in ('TRIAL','RECEIVING'):raise self.error('BUSY','update')
        if safe and (not self.device.in_safe_state or 'FAIL' in self.device.safe.steps or self.service_mode):raise self.error('NOT_SAFE')
    def cal_apply(self,codes):
        if self.dac_fail:
            self.device.safe.steps[0]='FAIL';self.device.in_safe_state=False;self.known=False;self.rsp=False;self.fault='PSU_FAULT';self.device.in_safe_state=False;raise self.error('PSU_FAULT','DAC')
        self.cal=list(codes);self.code=codes[0];self.known=True;self.mv=40000
    def calibration(self,action,args):
        if action=='query':
            state='NONE' if self.cal is None else 'SAVED' if self.cal==self.device.psu_cal_flash else 'RAM'
            return f"1,40,54,{state},{'ON' if self.service_mode else 'OFF'},"+','.join(map(str,self.cal)) if self.cal is not None else f"1,40,54,NONE,{'ON' if self.service_mode else 'OFF'},"+','.join(['?']*15)
        if action in ('set','code'):
            if any(not re.fullmatch('[0-9]+',x) or len(x.lstrip('0'))>5 or int(x)>65535 for x in args):raise self.error('RANGE','cal code' if action=='set' else 'code')
            codes=list(map(int,args))
            if action=='set' and any(b<=a for a,b in zip(codes,codes[1:])):raise self.error('RANGE','cal monotonic')
        self.cal_guard(action not in ('code','mode'))
        if action=='set':self.cal_apply(codes)
        elif action=='save':
            if self.cal is None:raise self.error('CAL_REQUIRED')
            if self.cal_fail and self.cal!=self.device.psu_cal_flash:raise self.error('CAL_STORAGE')
            self.device.psu_cal_flash=list(self.cal)
        elif action=='load':
            if self.device.psu_cal_flash is None:raise self.error('CAL_REQUIRED')
            self.cal_apply(self.device.psu_cal_flash)
        elif action=='mode':
            if args[0].upper() not in ('ON','OFF'):raise self.error('RANGE')
            self.rsp=self.starting=False;self.service_mode=args[0].upper()=='ON';self.raw_ready=False;self.device.in_safe_state=False
            if not self.service_mode:self.mv=40000
            if self.dac_fail:self.device.safe.steps[0]='FAIL';self.device.in_safe_state=False;self.known=False;self.fault='PSU_FAULT';raise self.error('PSU_FAULT','DAC')
            self.code=self.cal[0] if self.cal else 0;self.known=True
        elif action=='code':
            if not self.service_mode:raise self.error('CAL_MODE')
            if self.dac_fail:
                self.device.safe.steps[0]='FAIL';self.device.in_safe_state=False;self.known=False;self.rsp=self.raw_ready=False;self.fault='PSU_FAULT';raise self.error('PSU_FAULT','DAC')
            self.code=codes[0];self.known=self.raw_ready=True
        return 'OK'
