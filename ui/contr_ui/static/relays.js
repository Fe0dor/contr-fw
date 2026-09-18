/* Relay names, groups and addresses come from this connected device. */
let relayPending=false, relayTableKey='', relayCards=[], relayConflict=[];
function relayReady(){return state?.connected && !activeJob() && !relayPending && Object.hasOwn(state.replies,'ROUT:STAT?');}
function renderRelayPanel(){
  const rows=state?.relays||[], key=JSON.stringify(rows);
  if(key!==relayTableKey){
    relayTableKey=key;relayCards=[];$('relays-blocks').replaceChildren();
    const groups=[...new Set(rows.flatMap(r=>r.groups.split('+').filter(Boolean)))];
    const selected=$('relays-group').value;
    $('relays-group').replaceChildren(new Option('Все группы',''),...groups.map(g=>new Option(g,g)));
    if(groups.includes(selected))$('relays-group').value=selected;
    $('relays-names').replaceChildren(...rows.map(r=>new Option(r.name,r.name)));
    const blocks=new Map();
    for(const r of rows){
      if(!blocks.has(r.block)){const card=node('article','card');card.append(node('h3','',({ext:'Внешние измерения',mux:'Мультиплексор',test:'Тестовые цепи',int:'Внутренние измерения',load:'Нагрузки'})[r.block]||r.block));const grid=node('div','relay-switch-grid');card.append(grid);$('relays-blocks').append(card);blocks.set(r.block,{card,grid});}
      const button=node('button','relay-switch');button.type='button';button.setAttribute('aria-label','Переключить '+r.name);
      const status=node('span','relay-level');button.append(node('strong','',r.name),status,node('small','',r.address),node('small','relay-groups',r.groups||'Без блокировок'));
      button.onclick=()=>sendRelay((new Set((state.replies['ROUT:STAT?']||'').split(',')).has(r.name)?'ROUT:LOW ':'ROUT:HIGH ')+r.name);
      blocks.get(r.block).grid.append(button);relayCards.push({r,button,status,block:blocks.get(r.block)});
    }
  }
  const known=state?.connected && Object.hasOwn(state.replies,'ROUT:STAT?');
  const on=new Set((state?.replies['ROUT:STAT?']||'').split(',').filter(Boolean));
  const group=$('relays-group').value, search=$('relays-search').value.trim().toUpperCase();
  for(const card of relayCards){const {r,button,status}=card;const active=known&&on.has(r.name);
    button.disabled=!relayReady();button.classList.toggle('on',active);button.classList.toggle('conflict',relayConflict.includes(r.name));button.classList.toggle('group-selected',!!group&&r.groups.split('+').includes(group));button.setAttribute('aria-pressed',String(active));status.textContent=known?(active?'ВКЛ':'ВЫКЛ'):'НЕТ ДАННЫХ';
    button.hidden=!(r.name+' '+r.block+' '+r.address).toUpperCase().includes(search)||(!!group&&!r.groups.split('+').includes(group));
  }
  for(const {block} of relayCards)block.card.hidden=![...block.grid.children].some(b=>!b.hidden);
  text('relays-status',!state?.connected?'Нет связи — состояние неизвестно':!known?'Команды реле недоступны в этой прошивке или состояние ещё не прочитано':`Включено ${on.size} из ${rows.length} · по последнему ответу ROUT:STAT?`);
  $('relays-apply').disabled=$('relays-off').disabled=!relayReady();$('relays-refresh').disabled=!state?.connected||relayPending||activeJob();
}
async function sendRelay(cmd){
  if(!relayReady())return;
  relayPending=true;relayConflict=[];renderRelayPanel();
  try{
    const result=await api('command',{command:cmd});const reply=body(result.reply);text('relays-result',cmd+'\n'+result.reply);
    if(reply.startsWith('ERR:INTERLOCK,')){const [,pair,a,b]=reply.split(',');relayConflict=[a,b];text('relays-result',`Блокировка ${pair.split('__')[0]}: ${a} и ${b} нельзя включить вместе.\n${result.reply}`);}
    $('relays-result').classList.toggle('failed',reply.startsWith('ERR:'));
    await api('command',{command:'ROUT:STAT?'});
    const response=await fetch('/api/state?since=999999999');if(response.ok)renderState(await response.json());
  }catch(e){text('relays-result',e.message);$('relays-result').classList.add('failed');}
  finally{relayPending=false;renderRelayPanel();}
}
$('relays-search').oninput=renderRelayPanel;$('relays-group').onchange=renderRelayPanel;
$('relays-set-form').onsubmit=e=>{e.preventDefault();sendRelay('ROUT:SET'+($('relays-target').value.trim()?' '+$('relays-target').value.trim():''));};
$('relays-off').onclick=()=>sendRelay('ROUT:LOW:ALL');
$('relays-refresh').onclick=()=>run(async()=>{await command('ROUT:STAT?');const r=await fetch('/api/state?since=999999999');renderState(await r.json());});
$('relays-target').onkeydown=e=>{if(e.key!=='Tab')return;const input=e.target,parts=input.value.split(','),prefix=parts.at(-1).toUpperCase();const matches=(state?.relays||[]).filter(r=>r.name.toUpperCase().startsWith(prefix));if(matches.length===1){e.preventDefault();parts[parts.length-1]=matches[0].name;input.value=parts.join(',');}};
