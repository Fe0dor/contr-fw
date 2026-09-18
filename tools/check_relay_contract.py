"""Check compiled C and emulator against every generated interlock pair.

Optional --sdk-src PATH imports the real platform InterlockTable. CI without the
platform uses the published ordered-subset rule over the same JSON file.
This tool only starts a LOCAL host process, never connects to physical hardware.
"""
import argparse
import json
import sys
from pathlib import Path
from run_dialogs import start_host, Target

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'emu'))
from contr_emu.device import Device

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host',type=Path,required=True)
    p.add_argument('--port',type=int,default=15029)
    p.add_argument('--sdk-src',type=Path)
    a=p.parse_args()
    data=json.loads((ROOT/'gen/interlock-table.json').read_text('utf-8'))
    sdk=None
    if a.sdk_src:
        sys.path.insert(0,str(a.sdk_src))
        from testdut.sdk.interlocks import InterlockTable
        sdk=InterlockTable.model_validate(data)
    emu=Device();emu.connect()
    proc=start_host(a.host.resolve(),a.port)
    target=Target('127.0.0.1',a.port,proc)
    count=0
    def check(cmd,expected):
        nonlocal count
        target.send(cmd)
        actual=target.recv_line().split(';',1)[1]
        model=emu.execute('tcp',cmd).split(';',1)[1]
        assert actual==expected, (cmd,expected,actual)
        assert model==expected, (cmd,expected,model)
        count+=1
    try:
        target.connect()
        names={r['number']:r['name'] for r in emu.relays.values()}
        check('INTERLOCK:LIST?',emu.cmd_interlock_list_q([]))
        for r in emu.relays.values():
            check('ROUT:SET '+r['name'],'OK')
            check('ROUT:STAT?',r['name'])
        check('ROUT:LOW:ALL','OK')
        for pair in data['groups']:
            numbers=set(pair['relays'])
            group=next(g for g in data['groups'] if set(g['relays'])<=numbers)
            if sdk:
                assert sdk.violated_group(numbers).name==group['name']
            relay_names=','.join(names[n] for n in sorted(numbers))
            check('ROUT:SET '+relay_names,'ERR:INTERLOCK,'+group['name']+','+relay_names)
            check('ROUT:STAT?','')
        # Multiple violated groups: first pair by file order, not input order.
        all_names=','.join(reversed(list(emu.relays)))
        first=data['groups'][0]
        check('ROUT:SET '+all_names,'ERR:INTERLOCK,'+first['name']+','+','.join(names[n] for n in first['relays']))
        check('ROUT:STAT?','')
        check('SAFE','OK')
    finally:
        target.close()
        proc.kill();proc.wait(timeout=5)
    print(f'{count} C/emulator checks; {len(data["groups"])} pairs; SDK={sdk is not None}')

if __name__=='__main__':main()
