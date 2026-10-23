let psuPending=false,psuCalDirty=false;
const psuCalInputs=[];
function psuStatus(){return (state?.replies['PSU:STAT?']||'').split(',');}
function renderPsuPanel(){
  const p=psuStatus(),known=state?.connected&&p.length===6,ready=known&&!activeJob()&&!psuPending;
  for(const id of ['psu-rsp-on','psu-rsp-off','psu-hlg-on','psu-hlg-off','psu-voltage-apply'])$(id).disabled=!ready;
  $('psu-refresh').disabled=!state?.connected||activeJob()||psuPending;
  text('psu-rsp',known?p[0]:'—');text('psu-hlg',known?p[1]:'—');
  $('psu-rsp').className='pill '+(known&&p[0].includes(':')?'bad':known&&p[0]==='ON'?'good':'');
  $('psu-hlg').className='pill '+(known&&p[1]==='ON'?'good':'');
  const cal=psuCalStatus(),updating=['TRIAL','RECEIVING'].includes((state?.replies['SYST:UPD:STAT?']||'').split(',')[0]);
  $('psu-voltage-apply').disabled=!ready||updating||!cal||cal.mode||cal.status==='NONE';
  $('psu-voltage').disabled=$('psu-voltage-apply').disabled;
  $('psu-rsp-on').disabled=!ready||updating||!cal||(!cal.mode&&cal.status==='NONE');
  $('psu-hlg-on').disabled=!ready||updating;
  properties('psu-values',[[cal?.mode?'Последняя обычная уставка (неактивна)':'Заданное напряжение',known?p[2]+' В':'—'],['Код ЦАП',known?p[3]:'—']]);
  properties('psu-signals',[['DC-OK',known?p[4]:'—'],['ALARM',known?p[5]:'—']]);
  renderPsuCalibration(ready);
  text('psu-fault',!known?'Нет актуальных данных':p[0].includes(':')?'Сохранённая авария: '+p[0].split(':')[1]+' · RSP выключен':p[5]==='1'?'ALARM активен · включение RSP запрещено':'Авария RSP не зарегистрирована');
}
async function psuCommand(cmd){
  if(!state?.connected||activeJob()||psuPending)return;
  psuPending=true;renderPsuPanel();text('psu-reply','Выполняется '+cmd+'…');
  try{
    if(cmd.endsWith(',ON')&&!(await review('Включить выход БП?','Подтвердите готовность стенда: силовые соединения проверены, вы находитесь у стенда и контролируете напряжение.',cmd))){text('psu-reply','Включение отменено');return;}
    const r=await command(cmd);text('psu-reply',cmd+' → '+r.reply);
    if(cmd!=='PSU:STAT?')await command('PSU:STAT?');
    await command('PSU:CAL?');
    renderState(await (await fetch('/api/state?since='+cursor)).json());
  }finally{psuPending=false;renderPsuPanel();}
}
for(const source of ['rsp','hlg'])for(const action of ['on','off'])$('psu-'+source+'-'+action).onclick=()=>run(()=>psuCommand(`PSU:OUTP ${source.toUpperCase()},${action.toUpperCase()}`));
$('psu-refresh').onclick=()=>run(()=>psuCommand('PSU:STAT?'));
$('psu-voltage-form').onsubmit=e=>{e.preventDefault();run(()=>psuCommand('PSU:VOLT '+$('psu-voltage').value));};

function psuCalStatus(){
 const p=(state?.replies['PSU:CAL?']||'').split(',');
 if(p.length!==20||p[0]!=='1'||p[1]!=='40'||p[2]!=='54'||!['NONE','RAM','SAVED'].includes(p[3])||!['ON','OFF'].includes(p[4]))return null;
 return {status:p[3],mode:p[4]==='ON',codes:p.slice(5)};
}
function fillPsuCalibration(cal){if(!cal)return;psuCalInputs.forEach((x,i)=>x.value=cal.status==='NONE'?'':cal.codes[i]);psuCalDirty=false;}
function readPsuDraft(){
 const values=psuCalInputs.map(x=>{if(!/^\d+$/.test(x.value))throw new Error('Заполните все 15 целых кодов.');return Number(x.value);});
 if(values.some((x,i)=>!Number.isInteger(x)||x<0||x>65535||(i&&x<=values[i-1])))throw new Error('Коды должны строго возрастать и находиться в диапазоне 0…65535.');
 return values;
}
function renderPsuCalibration(ready){
 const cal=psuCalStatus(),known=state?.connected&&!!cal,available=known&&!activeJob()&&!psuPending&&!['TRIAL','RECEIVING'].includes((state?.replies['SYST:UPD:STAT?']||'').split(',')[0]);
 if(known&&!psuCalDirty)fillPsuCalibration(cal);
 text('psu-cal-state',known?({NONE:'Калибровка отсутствует',RAM:'Применена только в RAM',SAVED:'Сохранена в STM'}[cal.status]):'Нет актуальных данных');
 text('psu-cal-note',psuCalDirty?'Есть локальные правки: ещё не отправлены на плату.':known?'Редактор соответствует действующей таблице.':'Данные недоступны; локальный редактор сохранён.');
 text('psu-cal-mode-label',known&&cal.mode?'Управление кодом ЦАП. Обычная уставка отключена.':'Обычный режим: калиброванная уставка напряжения.');
 const safe=available&&state.safe_acknowledged&&!cal.mode&&!['TRIAL','RECEIVING'].includes((state.replies['SYST:UPD:STAT?']||'').split(',')[0]);
 for(const id of ['psu-cal-apply','psu-cal-save','psu-cal-load'])$(id).disabled=!safe;
 $('psu-cal-save').disabled=!safe||cal?.status==='NONE'||psuCalDirty;
 $('psu-cal-read').disabled=!available;$('psu-cal-safe').disabled=!available;
 $('psu-cal-mode-on').disabled=!available||cal?.mode;
 $('psu-cal-mode-off').disabled=!available||!cal?.mode;
 $('psu-cal-code-apply').disabled=!available||!cal?.mode;
 $('psu-cal-code').disabled=$('psu-cal-code-apply').disabled;
 psuCalInputs.forEach((x,i)=>{x.disabled=!!psuPending||!!activeJob();text('psu-cal-current-'+i,known?cal.codes[i]:'—');});
}
async function psuCalAction(cmd,replaceDraft=false){
 if(psuPending||!state?.connected||activeJob())return;
 if(replaceDraft&&psuCalDirty&&!(await review('Заменить локальные правки?','Редактор будет заполнен данными с платы.',cmd)))return;
 psuPending=true;renderPsuPanel();
 try{
  const r=await command(cmd);text('psu-reply',cmd+' → '+r.reply);
  if(body(r.reply)!=='OK'&&cmd!=='PSU:CAL?')throw new Error(body(r.reply));
  const fresh=await command('PSU:CAL?');
  if(body(fresh.reply).startsWith('ERR:'))throw new Error(body(fresh.reply));
  await command('PSU:STAT?');await command('SYST:UPD:STAT?');
  if(cmd.startsWith('PSU:CAL:SET ')||replaceDraft)psuCalDirty=false;
  renderState(await(await fetch('/api/state?since='+cursor)).json());
  if(cmd==='PSU:CAL:SAVE'&&psuCalStatus()?.status!=='SAVED')throw new Error('Сохранение не подтверждено чтением платы.');
 }finally{psuPending=false;renderPsuPanel();}
}
for(let i=0;i<15;i++){
 const tr=node('tr'),input=node('input');input.type='number';input.min=0;input.max=65535;input.step=1;input.setAttribute('aria-label','Код для '+(40+i)+' В');
 input.oninput=()=>{psuCalDirty=true;renderPsuCalibration(false);};psuCalInputs.push(input);
 const cell=node('td');cell.append(input);const current=node('td','','—');current.id='psu-cal-current-'+i;
 tr.append(node('td','',String(40+i)),cell,current);$('psu-cal-rows').append(tr);
}
$('psu-cal-read').onclick=()=>run(()=>psuCalAction('PSU:CAL?',true));
$('psu-cal-safe').onclick=()=>run(()=>psuCalAction('SAFE'));
$('psu-cal-apply').onclick=()=>run(()=>psuCalAction('PSU:CAL:SET '+readPsuDraft().join(',')));
$('psu-cal-save').onclick=()=>run(()=>psuCalAction('PSU:CAL:SAVE'));
$('psu-cal-load').onclick=()=>run(()=>psuCalAction('PSU:CAL:LOAD',true));
for(const mode of ['on','off'])$('psu-cal-mode-'+mode).onclick=()=>run(()=>psuCalAction('PSU:CAL:MODE '+mode.toUpperCase()));
$('psu-cal-code-form').onsubmit=e=>{e.preventDefault();run(()=>psuCalAction('PSU:CAL:CODE '+$('psu-cal-code').value));};
$('psu-cal-import').onchange=e=>run(async()=>{
 const file=e.target.files[0];if(!file)return;if(file.size>65536)throw new Error('Файл слишком велик.');
 const data=JSON.parse(await file.text());
 if(data.format!==1||data.min_v!==40||data.max_v!==54||!Array.isArray(data.codes)||data.codes.length!==15||data.codes.some((v,i)=>!Number.isInteger(v)||v<0||v>65535||(i&&v<=data.codes[i-1])))throw new Error('Ожидается JSON версии 1, диапазон 40–54 В и 15 возрастающих целых кодов.');
 psuCalInputs.forEach((x,i)=>x.value=data.codes[i]);psuCalDirty=true;renderPsuPanel();e.target.value='';
});
$('psu-cal-export').onclick=()=>run(async()=>{
 const data={format:1,min_v:40,max_v:54,codes:readPsuDraft()},url=URL.createObjectURL(new Blob([JSON.stringify(data,null,2)],{type:'application/json'})),a=node('a');a.href=url;a.download='rsp-calibration-40-54.json';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
});
renderPsuPanel();
