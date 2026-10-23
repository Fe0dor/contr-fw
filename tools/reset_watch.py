"""Read-only COM observer. Never connects TCP or changes physical outputs."""
import argparse,json,os,time,urllib.request
from datetime import datetime,timezone
from pathlib import Path
import serial

def restarted(previous,current):
    # Small modulo delta across uint32 wrap is ordinary uptime progression.
    return previous is not None and current<previous and ((current-previous)&0xffffffff)>0x80000000

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--port',default='COM5');ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    import msvcrt
    lock=(args.out/'watch.lock').open('a+b');lock.seek(0)
    try:msvcrt.locking(lock.fileno(),msvcrt.LK_NBLCK,1)
    except OSError:raise SystemExit('Observer already running')
    state={'pid':os.getpid(),'port':args.port,'status':'starting','incidents':0,'last_uptime_ms':None}
    path=args.out/'events.jsonl';stream=path.open('a',encoding='utf-8',buffering=1)
    previous=None;link=None;last_log=None;next_snapshot=0;lost=False
    def event(kind,**data):
        row={'utc':datetime.now(timezone.utc).isoformat(),'monotonic':time.monotonic(),'kind':kind,**data}
        stream.write(json.dumps(row,ensure_ascii=False)+'\n');stream.flush()
        if kind!='sample':os.fsync(stream.fileno())
        return row
    def status():
        state['updated_utc']=datetime.now(timezone.utc).isoformat()
        temp=args.out/'status.tmp';temp.write_text(json.dumps(state,ensure_ascii=False,indent=2),encoding='utf-8');temp.replace(args.out/'status.json')
    def query(command):
        link.write((command+'\n').encode());raw=link.readline()
        if not raw or not raw.endswith(b'\n'):raise TimeoutError(command)
        reply=raw.decode('ascii',errors='replace').strip()
        if reply.startswith('ERR:'):raise RuntimeError(command+': '+reply)
        return reply
    def incident(before,after):
        state['incidents']+=1
        item=event('reset_detected',before_ms=before,after_ms=after)
        state['last_incident']=item
        folder=args.out/('incident-'+datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'));folder.mkdir()
        (folder/'reset.json').write_text(json.dumps(item,indent=2),encoding='utf-8')
        for command in ['TEST:ALL?','SYST:SAFE?','SYST:LOG?','PSU:STAT?','ROUT:STAT?']:
            event('post_reset',command=command,reply=query(command))
        try:
            opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
            with opener.open('http://127.0.0.1:8765/api/state',timeout=2) as response:
                data=response.read(4*1024*1024)
            (folder/'ui-state.json').write_bytes(data)
        except Exception as error:event('ui_capture_failed',error=str(error))
    event('observer_started',pid=os.getpid(),port=args.port)
    try:
        while not (args.out/'STOP').exists():
            try:
                if link is None:
                    link=serial.Serial(args.port,115200,timeout=2,write_timeout=2);link.reset_input_buffer()
                uptime=int(query('SYST:UPTIME?'))
                if not 0<=uptime<=0xffffffff:raise ValueError('Invalid uptime')
                if restarted(previous,uptime):incident(previous,uptime)
                if lost:event('link_restored',uptime_ms=uptime);lost=False
                previous=uptime;state.update(status='watching',last_uptime_ms=uptime)
                event('sample',uptime_ms=uptime)
                if time.monotonic()>=next_snapshot:
                    current=query('SYST:LOG?')
                    if current!=last_log:event('board_log',reply=current);last_log=current
                    event('state',safe=query('SYST:SAFE?'),psu=query('PSU:STAT?'),relays=query('ROUT:STAT?'))
                    next_snapshot=time.monotonic()+1
                status();time.sleep(.25)
            except (OSError,ValueError,RuntimeError,serial.SerialException) as error:
                if not lost:event('link_lost',error=str(error),last_uptime_ms=previous)
                lost=True;state.update(status='disconnected',last_error=str(error));status()
                if link is not None:link.close();link=None
                time.sleep(2)
    finally:
        if link is not None:link.close()
        state['status']='stopped';status();event('observer_stopped');stream.close();lock.close()

if __name__=='__main__':main()
