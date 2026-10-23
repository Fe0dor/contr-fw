const dutCards=[];
let dutPending=false;
function dutStatus(){
  const result={};for(const item of (state?.replies['DUT:STAT?']||'').split(';')){const [name,...value]=item.split(',');result[name]=value.join(',');}return result;
}
async function dutCommand(cmd){
  if(!state?.connected||activeJob()||dutPending)return;
  dutPending=true;renderDutPanel();
  try{const r=await command(cmd);text('dut-reply',cmd+' → '+r.reply);if(cmd!=='DUT:STAT?')await command('DUT:STAT?');renderState(await (await fetch('/api/state?since='+cursor)).json());}
  finally{dutPending=false;renderDutPanel();}
}
for(let i=1;i<=20;i++){
  const name='C'+i,card=node('div','dut-card'),badge=pill('—');
  card.append(node('strong','',name),badge);
  const actions=node('div','button-row');const buttons=[];
  for(const mode of ['ON','OFF']){const b=node('button','button',mode==='ON'?'Подать':'Снять');b.setAttribute('aria-label',`${mode==='ON'?'Подать':'Снять'} ${name}`);b.onclick=()=>run(()=>dutCommand(`DUT:SET ${name},${mode}`));actions.append(b);buttons.push(b);}
  card.append(actions);$('dut-grid').append(card);dutCards.push({name,badge,buttons});
}
function renderDutPanel(){
  const status=dutStatus(),ready=state?.connected&&!activeJob()&&!dutPending&&state?.dut_supported;
  for(const c of dutCards){const value=state?.connected?status[c.name]||'—':'—';c.badge.textContent=value;c.badge.className='pill '+(value==='MANUAL'?'warn':value==='ON'?'good':'');for(const b of c.buttons)b.disabled=!ready;}
  $('dut-off').disabled=!ready;$('dut-refresh').disabled=!state?.connected||activeJob()||dutPending;
  text('dut-pwrok','PWROK1–3: '+(state?.connected&&status.PWROK?status.PWROK:'—'));
  text('dut-intervention',!state?.connected?'Нет связи':status.INTERVENTION==='1'?'Обнаружено ручное вмешательство — событие записано в журнал.':status.INTERVENTION==='0'?'После предыдущего опроса новых вмешательств нет. Предыдущие события сохранены в журнале.':'Линии недоступны или ещё не опрошены.');
}
$('dut-off').onclick=()=>run(()=>dutCommand('DUT:OFF:ALL'));
$('dut-refresh').onclick=()=>run(()=>dutCommand('DUT:STAT?'));
renderDutPanel();
