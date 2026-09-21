/* Command rows retain their own replies, independent of background polling. */
const commandRows = [];
const commandSections = [];
function commandUnavailable(item) {
  if (!state?.connected) return 'Подключите устройство';
  if (item.name !== 'SAFE' && activeJob()) return 'Выполняется сценарий или обновление';
  if (item.console && state.target?.mode !== 'serial') return 'Доступно через COM без TCP-клиента';
  return '';
}
function renderCommandAvailability() {
  for (const row of commandRows) {
    const reason = commandUnavailable(row.item);
    row.button.disabled = row.running || !!reason;
    row.button.title = reason;
    row.hint.textContent = reason;
    if (row.input) row.input.disabled = row.running;
  }
}
function filterCommandRows() {
  const query = $('commands-search').value.trim().toLocaleLowerCase('ru');
  const filter = $('commands-filter').value;
  let shown = 0;
  for (const row of commandRows) {
    const item = row.item;
    row.el.hidden = !(`Шаг ${item.step} ${item.step_title} ${item.name} ${item.description} ${item.args}`.toLocaleLowerCase('ru').includes(query)
      && (filter === 'all' || (filter === 'bringup') === item.bringup));
    if (!row.el.hidden) shown++;
  }
  for (const section of commandSections) {
    const count = commandRows.filter(row => row.item.step === section.step && !row.el.hidden).length;
    section.el.hidden = count === 0;
    section.count.textContent = `${count} команд`;
  }
  text('commands-count', `${shown} из ${commandRows.length}`);
  $('commands-empty').hidden = shown !== 0;
}
async function commandSafeReply(since) {
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    const response = await fetch('/api/state?since=' + since);
    if (!response.ok) throw new Error('Не удалось получить ответ SAFE');
    const snapshot = await response.json();
    const reply = snapshot.events.find(e => e.kind === 'rx' && e.command === 'SAFE');
    if (reply) return reply.text;
    if (!snapshot.connected) throw new Error('Соединение закрыто до получения ответа SAFE');
    await new Promise(resolve => setTimeout(resolve, 100));
  }
  throw new Error('Ответ SAFE не получен за 10 с. Проверьте терминал и связь.');
}
async function executeCommandRow(row) {
  if (row.running || commandUnavailable(row.item)) return;
  const args = row.input?.value.trim() || '';
  if (row.input?.required && !args) { row.input.reportValidity(); row.input.focus(); return; }
  const cmd = row.item.name + (args ? ' ' + args : '');
  row.running = true;
  renderCommandAvailability();
  try {
    if (row.item.confirm && !await review('Выполнить команду?', row.item.confirm, cmd)) return;
    // A connection/job may have changed while the review dialog was open.
    const reason = commandUnavailable(row.item);
    if (reason) throw new Error(reason);
    row.output.textContent = 'Ожидание ответа…';
    row.output.className = 'command-response';
    row.stamp.textContent = cmd;
    row.button.textContent = 'Выполняется…';
    // Take a fresh event cursor so SAFE cannot reuse an earlier acknowledgement.
    let since = 0;
    if (row.item.name === 'SAFE') {
      const response = await fetch('/api/state?since=999999999');
      if (!response.ok) throw new Error('Нет связи с локальным пультом');
      since = (await response.json()).cursor;
    }
    const result = await api('command', {command: cmd});
    const reply = result.reply ?? (result.accepted ? await commandSafeReply(since) : null);
    if (typeof reply !== 'string') throw new Error('Пульт не вернул ответ устройства');
    row.output.textContent = reply;
    row.output.classList.toggle('failed', body(reply).startsWith('ERR:'));
    row.stamp.textContent = new Date().toLocaleTimeString('ru-RU') + ' · ' + cmd;
  } catch (error) {
    row.output.textContent = 'Ошибка пульта: ' + error.message;
    row.output.className = 'command-response failed';
    row.stamp.textContent = new Date().toLocaleTimeString('ru-RU') + ' · ' + cmd;
  } finally {
    row.running = false;
    row.button.textContent = 'Выполнить';
    renderCommandAvailability();
  }
}
function addCommandRow(item, index) {
  const el = node('tr');
  el.dataset.commandName = item.name;
  const commandCell = node('td');
  commandCell.append(node('code', 'command-name', item.name));
  let input;
  if (item.args) {
    const label = node('label', 'command-arguments', 'Аргументы');
    input = node('input');
    input.id = 'command-args-' + index;
    input.placeholder = item.args;
    input.required = item.name !== 'ROUT:SET';
    input.maxLength = 480 - item.name.length - 1;
    input.autocomplete = 'off';
    input.spellcheck = false;
    input.setAttribute('aria-label', 'Аргументы ' + item.name);
    label.append(input);
    commandCell.append(label);
  }
  const description = node('td', 'command-description');
  description.append(node('p', '', item.description));
  description.append(pill(item.bringup ? 'Только bringup' : item.console ? 'Только COM' : 'Рабочая прошивка', item.bringup ? 'warn' : 'neutral'));
  const action = node('td');
  const button = node('button', 'button', 'Выполнить');
  button.type = 'button';
  button.setAttribute('aria-label', 'Выполнить ' + item.name);
  const hint = node('small', 'command-hint');
  action.append(button, hint);
  const response = node('td');
  const output = node('pre', 'command-response', 'Ещё не выполнялась');
  output.setAttribute('aria-live', 'polite');
  output.setAttribute('aria-label', 'Ответ ' + item.name);
  const stamp = node('small', 'command-stamp');
  response.append(output, stamp);
  el.append(commandCell, description, action, response);
  const row = {item, el, input, button, hint, output, stamp, running: false};
  button.onclick = () => executeCommandRow(row);
  if (input) input.onkeydown = e => { if (e.key === 'Enter') { e.preventDefault(); executeCommandRow(row); } };
  commandRows.push(row);
  $('commands-table').append(el);
}
$('commands-search').oninput = filterCommandRows;
$('commands-filter').onchange = filterCommandRows;
run(async () => {
  const response = await fetch('/api/commands');
  if (!response.ok) throw new Error('Не удалось загрузить список команд');
  const catalog = await response.json();
  catalog.sort((a, b) => a.step - b.step);
  let lastStep = null;
  catalog.forEach((item, index) => {
    if (item.step !== lastStep) {
      const el = node('tr', 'command-step');
      const heading = node('th');
      heading.colSpan = 4;
      heading.scope = 'rowgroup';
      const count = node('span', 'pill neutral');
      heading.append(node('strong', '', `Шаг ${item.step}. ${item.step_title}`), count);
      el.append(heading);
      $('commands-table').append(el);
      commandSections.push({step: item.step, el, count});
      lastStep = item.step;
    }
    addCommandRow(item, index);
  });
  renderCommandAvailability();
  filterCommandRows();
});
