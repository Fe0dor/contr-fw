/* Volatile section codes and operator-entered measurements. */
const sectionElements=[];
for(let ch=1;ch<=4;ch++)for(const kind of (ch%2?['POT','MTX']:['POT','POTH','MTX']))
  sectionElements.push({channel:'R'+ch,kind,bus:ch<=2?'A':'B',max:kind==='POT'?256:kind==='POTH'?1023:ch%2?31:7});
let sectionPending=false,calPending=null,sectionControls=[];
const calibrationKey='contr-calibration-v1';
let calibrationRows=[];
try{const saved=JSON.parse(localStorage.getItem(calibrationKey)||'[]');if(Array.isArray(saved))calibrationRows=saved;}catch(e){notice('Не удалось прочитать сохранённые точки: '+e.message);}
function sectionStatus(raw=state?.replies['RES:STAT?']){
  const result={};for(const item of (raw||'').split(';')){if(/^[AB]:(ON|OFF)$/.test(item))result[item[0]]=item.slice(2);else {const [ch,kind,value]=item.split(',');if(/^R[1-4]$/.test(ch)&&['POT','POTH','MTX'].includes(kind))result[ch+','+kind]=value;}}return result;
}
function sectionReady(){const status=sectionStatus();return !!(state?.connected&&!activeJob()&&!sectionPending&&status.A&&status.B);}
function selectedElement(){return sectionElements[Number($('cal-element').value)||0];}
function discardPoint(message='Выберите код из сетки, затем введите показание мультиметра.'){calPending=null;text('cal-pending',message);$('cal-save').disabled=true;}
async function sectionCommand(cmd){
  if(!state?.connected||activeJob()||sectionPending)throw new Error('Дождитесь завершения операции и подключения.');
  sectionPending=true;discardPoint();renderSectionPanel();
  try{
    const r=await command(cmd);text('sections-reply',cmd+' → '+(r.reply||'Нет ответа'));
    const reply=await command('RES:STAT?');
    const fresh=await (await fetch('/api/state?since='+cursor)).json();renderState(fresh);
    return {ok:body(r.reply||'')==='OK',status:body(reply.reply||'')};
  }finally{sectionPending=false;renderSectionPanel();}
}
function buildSections(){
  for(const bus of ['A','B']){
    const card=node('article','card'),heading=node('div','card-heading'),label=node('h2','','Секция '+bus),badge=pill('—');
    heading.append(label,badge);card.append(heading);
    const actions=node('div','button-row');
    for(const action of ['ON','OFF']){const b=node('button','button '+(action==='ON'?'primary':''),action==='ON'?'Включить питание':'Выключить');b.onclick=()=>run(()=>sectionCommand(`RES:PWR ${bus},${action}`));actions.append(b);sectionControls.push({button:b,bus,power:action});}
    card.append(actions);sectionControls.push({badge,bus});
    for(const el of sectionElements.filter(e=>e.bus===bus)){
      const row=node('form','section-element'),label=node('label','',el.channel+' · '+el.kind),input=node('input');input.type='number';input.min=0;input.max=el.max;input.step=1;input.value=el.kind==='POT'?128:el.kind==='POTH'?512:0;input.required=true;input.setAttribute('aria-label',`Код ${el.channel} ${el.kind}`);label.append(input);
      const current=node('span','section-current','Прочитано: ?'),button=node('button','button','Установить');button.type='submit';
      row.append(label,node('small','',`0…${el.max}`),button,current);
      row.onsubmit=e=>{e.preventDefault();run(()=>sectionCommand(`RES:SET ${el.channel},${el.kind},${input.value}`));};
      card.append(row);sectionControls.push({button,input,current,...el});
    }
    $('sections-cards').append(card);
  }
  sectionElements.forEach((el,i)=>{const opt=node('option','',el.channel+' · '+el.kind);opt.value=i;$('cal-element').append(opt);});
  buildCalibrationGrid();renderCalibrationRows();
}
function renderSectionPanel(){
  const status=sectionStatus(),ready=sectionReady();
  for(const control of sectionControls){
    if(control.badge){control.badge.textContent=status[control.bus]||'—';control.badge.className='pill '+(state?.connected&&status[control.bus]==='ON'?'good':'');continue;}
    control.button.disabled=!ready||(!control.power&&status[control.bus]!=='ON');
    if(control.input)control.input.disabled=control.button.disabled;
    if(control.current)control.current.textContent='Прочитано: '+(status[control.channel+','+control.kind]||'?')+(state?.connected?'':' · нет связи');
  }
  const el=selectedElement();for(const b of $('cal-grid').querySelectorAll('button'))b.disabled=!ready||status[el.bus]!=='ON';
  if(calPending && (!state.connected||activeJob()||state.generation!==calPending.generation||JSON.stringify(state.target)!==calPending.target||state.replies['RES:STAT?']!==calPending.status))discardPoint('Состояние изменилось. Повторно установите код перед записью измерения.');
  $('cal-save').disabled=!calPending||!ready;
}
function buildCalibrationGrid(){
  discardPoint();const el=selectedElement();
  const values=el.kind==='POT'?[0,64,128,192,256]:el.kind==='POTH'?[0,256,512,768,1023]:[0,...Array.from({length:el.max===31?5:3},(_,i)=>1<<i),el.max];
  const others=sectionElements.filter(e=>e.channel===el.channel&&e.kind!==el.kind).map(e=>e.kind).join(', ');
  text('cal-instruction',`${el.channel}: оставьте в цепи ${el.kind}; шунтируйте ${others}. Включите секцию ${el.bus}, затем выбирайте код по одному.`);
  $('cal-grid').replaceChildren(...values.map(value=>{
    const b=node('button','button',String(value));b.onclick=()=>run(async()=>{
      const selected=$('cal-element').value;const result=await sectionCommand(`RES:SET ${el.channel},${el.kind},${value}`);
      if(!result.ok||selected!==$('cal-element').value)return;
      const status=sectionStatus(result.status);if(status[el.channel+','+el.kind]!==String(value))throw new Error('Код не подтверждён чтением.');
      calPending={...el,code:value,generation:state.generation,target:JSON.stringify(state.target),status:result.status,idn:state.replies['*IDN?'],time:new Date().toISOString()};
      text('cal-pending',`${el.channel} ${el.kind}: код ${value} подтверждён. Введите показание мультиметра в омах.`);$('cal-ohms').value='';renderSectionPanel();$('cal-ohms').focus();
    });return b;
  }));
  renderSectionPanel();
}
function persistCalibration(){try{localStorage.setItem(calibrationKey,JSON.stringify(calibrationRows));}catch(e){notice('Точки видны только до закрытия страницы. Скачайте CSV: '+e.message);}renderCalibrationRows();}
function renderCalibrationRows(){
  $('cal-table').replaceChildren(...calibrationRows.map((r,i)=>{const row=node('tr');row.append(node('td','',r.time),node('td','',r.idn),node('td','',r.channel+' '+r.kind),node('td','',r.code),node('td','',r.ohms));const cell=node('td'),del=node('button','button','Удалить');del.onclick=()=>{calibrationRows.splice(i,1);persistCalibration();};cell.append(del);row.append(cell);return row;}));
  $('cal-export').disabled=!calibrationRows.length;
}
$('cal-element').onchange=buildCalibrationGrid;
$('cal-form').onsubmit=e=>{e.preventDefault();run(async()=>{
  if(!calPending)throw new Error('Сначала установите код из сетки.');
  const pending=calPending,ohms=Number($('cal-ohms').value);
  if($('cal-ohms').value===''||!Number.isFinite(ohms)||ohms<0)throw new Error('Введите неотрицательное сопротивление в омах.');
  const reply=await command('RES:STAT?');
  const fresh=await (await fetch('/api/state?since='+cursor)).json();renderState(fresh);
  if(calPending!==pending||body(reply.reply||'')!==pending.status)throw new Error('Состояние изменилось. Установите код заново.');
  calibrationRows.push({...pending,time:new Date().toISOString(),ohms,board:$('cal-board').value,setup:$('cal-setup').value,note:$('cal-note').value,source:'manual'});
  persistCalibration();discardPoint('Точка записана. Выберите следующий код.');
});};
$('cal-export').onclick=()=>{
  const keys=['time','idn','board','target','channel','kind','code','ohms','generation','status','setup','note','source'];
  const quote=v=>'"'+String(v??'').replaceAll('"','""')+'"';
  // Protect spreadsheet readers from formulas entered as notes/board labels.
  const cell=v=>quote(typeof v==='string'&&/^[=+@\-\t\r]/.test(v)?"'"+v:v);
  const csv='\ufeff'+[keys.join(','),...calibrationRows.map(r=>keys.map(k=>cell(r[k])).join(','))].join('\r\n');
  const url=URL.createObjectURL(new Blob([csv],{type:'text/csv;charset=utf-8'})),a=node('a');a.href=url;a.download='contr-resistance-'+new Date().toISOString().slice(0,10)+'.csv';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
};
buildSections();
