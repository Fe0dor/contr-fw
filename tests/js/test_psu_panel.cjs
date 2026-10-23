const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
class E{constructor(){this.value='';this.children=[];}append(...x){this.children.push(...x)}setAttribute(){}click(){}}
const els=new Map(),get=id=>{if(!els.has(id))els.set(id,new E());return els.get(id)},codes=Array.from({length:15},(_,i)=>38000+i*1000);
const s={connected:true,safe_acknowledged:true,replies:{'PSU:STAT?':'OFF,OFF,40,38000,0,0','PSU:CAL?':'1,40,54,SAVED,OFF,'+codes.join(','),'SYST:UPD:STAT?':'IDLE,0,0'}};
let fail='',lie=false;const calls=[];
const c=vm.createContext({state:s,cursor:0,$:get,node:()=>new E(),text:(id,v)=>get(id).textContent=v,properties:()=>{},activeJob:()=>false,body:x=>x,run:async f=>f(),review:async()=>true,Blob,URL,setTimeout,renderState:()=>{},fetch:async()=>({json:async()=>s}),command:async cmd=>{calls.push(cmd);if(fail&&cmd.endsWith(':SAVE'))return {reply:fail};if(cmd.startsWith('PSU:CAL:SET '))s.replies['PSU:CAL?']='1,40,54,RAM,OFF,'+cmd.split(' ')[1];if(cmd==='PSU:CAL:SAVE'&&!lie)s.replies['PSU:CAL?']=s.replies['PSU:CAL?'].replace(',RAM,',',SAVED,');return {reply:cmd.endsWith('?')?s.replies[cmd]:'OK'}}});
vm.runInContext(fs.readFileSync(require('node:path').resolve(__dirname,'../../ui/contr_ui/static/psu.js'),'utf8'),c);const js=x=>vm.runInContext(x,c);
(async()=>{
 assert.equal(js('psuCalInputs.length'),15);assert.equal(js('psuCalInputs[0].value'),'38000');
 js("psuCalInputs[0].value='38001';psuCalInputs[0].oninput();renderPsuPanel()");assert.equal(js('psuCalInputs[0].value'),'38001');assert.equal(get('psu-cal-save').disabled,true);
 await js("psuCalAction('PSU:CAL:SET '+readPsuDraft().join(','))");assert.equal(js('psuCalDirty'),false);
 fail='ERR:CAL_STORAGE';await assert.rejects(()=>js("psuCalAction('PSU:CAL:SAVE')"),/CAL_STORAGE/);fail='';lie=true;await assert.rejects(()=>js("psuCalAction('PSU:CAL:SAVE')"),/не подтверждено/);lie=false;
 await js("psuCalAction('PSU:CAL:SAVE')");assert.equal(get('psu-cal-state').textContent,'Сохранена в STM');
 js("psuCalInputs[0].value='38002';psuCalInputs[0].oninput()");s.connected=false;js('renderPsuPanel()');assert.equal(get('psu-cal-apply').disabled,true);s.connected=true;js('renderPsuPanel()');assert.equal(js('psuCalInputs[0].value'),'38002');
 await js("psuCalAction('PSU:CAL?',true)");assert.equal(js('psuCalInputs[0].value'),'38001');s.safe_acknowledged=false;js('renderPsuPanel()');assert.equal(get('psu-cal-apply').disabled,true);s.safe_acknowledged=true;
 s.replies['SYST:UPD:STAT?']='TRIAL,0,0';js('renderPsuPanel()');assert.equal(get('psu-cal-save').disabled,true);assert.equal(get('psu-rsp-on').disabled,true);s.replies['SYST:UPD:STAT?']='IDLE,0,0';
 s.replies['PSU:CAL?']=s.replies['PSU:CAL?'].replace(',OFF,',',ON,');js('renderPsuPanel()');assert.equal(get('psu-voltage').disabled,true);assert.equal(get('psu-cal-code-apply').disabled,false);
 const before=js('psuCalInputs.map(x=>x.value).join()'),event={target:{files:[{size:20,text:async()=>JSON.stringify({format:1,min_v:42,max_v:52,codes})}]}};await assert.rejects(()=>get('psu-cal-import').onchange(event),/JSON/);assert.equal(js('psuCalInputs.map(x=>x.value).join()'),before);
 const n=calls.length;event.target.files[0].text=async()=>JSON.stringify({format:1,min_v:40,max_v:54,codes});await get('psu-cal-import').onchange(event);assert.equal(calls.length,n);assert.equal(js('psuCalDirty'),true);
 console.log('PASS: editor polling, apply, save/readback, flash failure, reconnect, SAFE/update, service and JSON');
})().catch(e=>{console.error(e);process.exitCode=1});

