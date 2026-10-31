(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const number = (value, digits = 1) => Number.isFinite(value) ? value.toLocaleString('pt-BR', {minimumFractionDigits: digits, maximumFractionDigits: digits}) : 'Não disponível';
  const value = (data, key, divisor = 1) => data && Number.isFinite(data[key]) ? data[key] / divisor : null;
  const format = (n, unit, digits = 1) => n === null ? 'Não disponível' : number(n, digits) + ' ' + unit;
  const age = ms => ms === null ? 'Não disponível' : ms < 1000 ? ms + ' ms' : ms < 60000 ? number(ms / 1000, 1) + ' s' : number(ms / 60000, 1) + ' min';
  const bytes = n => n === null ? 'Não disponível' : n >= 1048576 ? number(n / 1048576, 1) + ' MiB' : number(n / 1024, 1) + ' KiB';
  const set = (id, text) => { const el = $(id); if (el && el.textContent !== text) el.textContent = text; };
  const tone = (id, name) => { const el = $(id); if (el) el.className = 'dot ' + name; };
  const badge = (id, text, name) => { const el = $(id); if (el) { el.textContent = text; el.className = 'badge ' + name; } };
  const message = (text, kind = '') => { const el = $('notice'); el.hidden = false; el.textContent = text; el.className = 'notice ' + kind; };
  const state = { tab: 'telemetria', range: 30, signal: 'power', telemetry: null, local: null, localAt: 0, master: {}, visitors: {}, browserOnline: false, browserChecked: false, localChecked: false, lastHttp: 0, masterOnline: false, masterChecked: false, visitorsOnline: false, visitorsChecked: false, lastSample: 0, lastSequence: null, samples: [], deviceSamples: {mestre:[],equipe:[],visitantes:[]}, deviceSignal:{mestre:'cpu',equipe:'cpu',visitantes:'rtt'}, events: [], commandEvents: [], commandRuns: [], selected: {mestre:'mestre',equipe:'equipe', visitantes:'visitantes'}, busy: false, energyWh: 0, distanceStart: null, pending: null, polls: {}, commandRefreshMs: 5000 };
  const devices = {
    mestre: {group:'mestre', name:'ESP32 mestre', id:'MESTRE', role:'Mestre', function:'Aquisição e telemetria do veículo'},
    equipe: {group:'equipe', name:'ESP32 da equipe', id:'EQUIPE', role:'Satélite / gateway', function:'Recepção UART, Wi-Fi e painel web'},
    visitantes: {group:'visitantes', name:'ESP32 de visitantes', id:'VISITANTES', role:'Satélite', function:'Painel e recepção de telemetria'}
  };
  const deviceChartOptions={
    mestre:[{key:'cpu',label:'CPU total',unit:'%',color:'#8BC4DF'},{key:'core0',label:'CPU núcleo 0',unit:'%',color:'#8BC4DF'},{key:'core1',label:'CPU núcleo 1',unit:'%',color:'#8BC4DF'},{key:'heap',label:'Memória interna livre',unit:'KiB',color:'#79C9A3'},{key:'temp',label:'Temperatura interna',unit:'°C',color:'#EBC17F'},{key:'rtt',label:'Tempo de resposta',unit:'ms',color:'#A9B7CC'}],
    equipe:[{key:'cpu',label:'CPU total',unit:'%',color:'#8BC4DF'},{key:'core0',label:'CPU núcleo 0',unit:'%',color:'#8BC4DF'},{key:'core1',label:'CPU núcleo 1',unit:'%',color:'#8BC4DF'},{key:'heap',label:'Memória interna livre',unit:'KiB',color:'#79C9A3'},{key:'temp',label:'Temperatura interna',unit:'°C',color:'#EBC17F'},{key:'rate',label:'Taxa de telemetria',unit:'Hz',color:'#A9B7CC'}],
    visitantes:[{key:'rtt',label:'Tempo de resposta',unit:'ms',color:'#8BC4DF'},{key:'availability',label:'Disponibilidade',unit:'%',color:'#79C9A3'}]
  };
  const CAP_SYNC = 1 << 11;
  const refreshChoices = [3000,5000,10000,20000];
  try {const saved=Number(localStorage.getItem('nexus-command-refresh-ms'));if(refreshChoices.includes(saved))state.commandRefreshMs=saved;} catch (_) { /* O painel funciona sem armazenamento local. */ }
  $('command-refresh-rate').value=String(state.commandRefreshMs);
  $('command-refresh-rate').addEventListener('change',event=>{
    const selected=Number(event.target.value);if(!refreshChoices.includes(selected))return;
    state.commandRefreshMs=selected;state.lastCommandPoll=0;state.lastAuxPoll=0;
    try {localStorage.setItem('nexus-command-refresh-ms',String(selected));} catch (_) { /* Preferência válida apenas nesta sessão. */ }
    pollCommandStates();
  });
  const marchas = ['Estacionado','Neutro','Frente','Ré'];

  function showTab(tab, historyMode = 'replace') {
    if (!['telemetria','mestre','equipe','visitantes'].includes(tab)) tab = 'telemetria';
    state.tab = tab;
    document.querySelectorAll('.page').forEach(el => el.classList.toggle('active', el.id === tab));
    document.querySelectorAll('.tab').forEach(el => { const active = el.dataset.tab === tab; el.classList.toggle('active', active); el.setAttribute('aria-selected', String(active)); el.tabIndex = active ? 0 : -1; });
    if (location.hash !== '#' + tab) history[historyMode+'State'](null, '', '#' + tab);
    if (tab === 'telemetria') requestAnimationFrame(drawCharts);
    else if (tab === 'mestre') renderDevice('mestre','mestre');
    else renderGroup(tab);
  }
  document.querySelectorAll('.tab').forEach(el => el.addEventListener('click', () => showTab(el.dataset.tab, 'push')));
  $('tab-telemetria').closest('.tabs').addEventListener('keydown', event => {
    const tabs=[...document.querySelectorAll('.tab')],index=tabs.indexOf(document.activeElement);
    if(index<0)return;
    const next=event.key==='ArrowRight'?tabs[(index+1)%tabs.length]:event.key==='ArrowLeft'?tabs[(index+tabs.length-1)%tabs.length]:event.key==='Home'?tabs[0]:event.key==='End'?tabs[tabs.length-1]:null;
    if(next){event.preventDefault();showTab(next.dataset.tab, 'push');next.focus();}
  });
  addEventListener('hashchange', () => showTab(location.hash.slice(1)));
  addEventListener('popstate', () => showTab(location.hash.slice(1)));
  document.querySelectorAll('.range').forEach(el => el.addEventListener('click', () => { state.range = Number(el.dataset.range); document.querySelectorAll('.range').forEach(b => b.classList.toggle('active', b === el)); drawCharts(); }));
  $('electric-signal').addEventListener('change', e => { state.signal = e.target.value; drawCharts(); });
  function updateArc() {const speed=state.telemetry&&state.telemetry.valido?value(state.telemetry,'velocidade_centesimos_kmh',100):0;$('speed-fill').style.strokeDasharray=(Math.max(0,Math.min(1,(speed||0)/30))*260)+' 260';}

  function event(text, kind = 'info', command = false) {
    const item = {time: Date.now(), text, kind};
    (command ? state.commandEvents : state.events).unshift(item);
    if (state.events.length > 24) state.events.length = 24;
    if (state.commandEvents.length > 24) state.commandEvents.length = 24;
    renderEvents();
  }
  function renderEvents() {
    const el = $('events'); el.replaceChildren();
    const items = state.events.slice(0, 5);
    if (!items.length) { const li = document.createElement('li'); li.textContent = 'Aguardando a primeira leitura.'; el.append(li); return; }
    items.forEach(item => { const li = document.createElement('li'), time = document.createElement('time'); time.textContent = new Date(item.time).toLocaleTimeString('pt-BR'); li.append(time, document.createTextNode(item.text)); el.append(li); });
  }
  function updateStatus() {
    tone('browser-dot', !state.browserChecked ? 'neutral' : state.browserOnline ? 'good' : 'bad'); set('browser-status', !state.browserChecked ? 'Conectando' : state.browserOnline ? 'Conectado' : 'Sem resposta');
    tone('master-dot', !state.masterChecked ? 'neutral' : state.masterOnline ? 'good' : 'bad'); set('master-status', !state.masterChecked ? 'Não consultado' : state.masterOnline ? 'Respondendo' : 'Sem resposta');
    tone('esp-dot', !state.visitorsChecked ? 'neutral' : state.visitorsOnline ? 'good' : 'bad'); set('esp-status', !state.visitorsChecked ? 'Não consultado' : state.visitorsOnline ? 'Respondendo' : 'Sem resposta');
    const elapsed = state.lastSample ? Date.now() - state.lastSample : null;
    set('updated-status', elapsed === null ? 'Não disponível' : 'há ' + age(elapsed));
    set('record-status', state.samples.length ? 'Histórico nesta página' : 'Sem gravação no dispositivo');
    const stale = !state.telemetry || !state.telemetry.valido || state.telemetry.idade_ms >= 2000 || elapsed === null || elapsed > 4000;
    document.querySelectorAll('.speed-card,.metric-card,.quick-strip,.chart-card,.detail-card').forEach(el => el.classList.toggle('stale', stale));
    if (state.telemetry) badge('data-badge', state.telemetry.simulado ? 'Dados simulados' : stale ? 'Dados desatualizados' : 'Dados do mestre', state.telemetry.simulado ? 'warn' : stale ? 'warn' : 'good');
  }
  async function getJSON(url, timeout = 7000) {
    const controller = new AbortController(); const timer = setTimeout(() => controller.abort(), timeout);
    try { const response = await fetch(url, {cache:'no-store', signal:controller.signal}); state.lastHttp = Date.now(); state.browserOnline = true; const body = await response.json(); if (!response.ok || body.ok === false) throw new Error(body.erro || 'Resposta ' + response.status); return body; }
    finally { clearTimeout(timer); }
  }
  let remoteQueue = Promise.resolve();
  function remote(task) { const next = remoteQueue.then(task); remoteQueue = next.catch(() => {}); return next; }
  function appendSample(d) {
    const now = Date.now();
    if (state.lastSequence === d.sequencia) return;
    if (state.samples.length && now - state.samples[state.samples.length - 1].time > 3000) state.samples.push({time:now - 1, gap:true});
    const sample = {time:now, simulated:d.simulado, speed:value(d,'velocidade_centesimos_kmh',100), accel:d.simulado || (d.estado_mpu & 1) ? value(d,'aceleracao_milesimos_ms2',1000) : null, power:d.simulado ? value(d,'potencia_w') : null, current:d.simulado ? value(d,'corrente_centesimos_a',100) : null, voltage:d.simulado ? value(d,'tensao_pacote_mv',1000) : null};
    if (state.samples.length && !state.samples[state.samples.length-1].gap) {
      const previous = state.samples[state.samples.length-1];
      const hours = Math.min((now - previous.time) / 3600000, 2 / 3600);
      if (previous.power !== null && sample.power !== null) state.energyWh += (previous.power + sample.power) / 2 * hours;
    }
    state.samples.push(sample); state.samples = state.samples.filter(s => now - s.time <= 610000);
    state.lastSequence = d.sequencia; state.lastSample = now;
    if (state.distanceStart === null && Number.isFinite(d.percurso_metros)) state.distanceStart = d.percurso_metros;
    $('export-csv').disabled = false;
    drawCharts();
  }
  function deviceSample(id, measurements) { const history=state.deviceSamples[id]; history.push({time:Date.now(),...measurements}); if(history.length>180)history.shift(); if(state.tab===devices[id].group) drawDeviceChart(devices[id].group,id); }
  function renderTelemetry(d) {
    const available = !!(d && d.valido);
    if (!available) return;
    const speed = value(d,'velocidade_centesimos_kmh',100), accel = d.simulado || (d.estado_mpu & 1) ? value(d,'aceleracao_milesimos_ms2',1000) : null, soc = d.simulado ? value(d,'carga_decimos_percentual',10) : null, power = d.simulado ? value(d,'potencia_w') : null;
    set('speed', speed === null ? '—' : number(speed,1)); set('speed-state', d.idade_ms >= 2000 ? 'Última leitura desatualizada' : 'Leitura atual');
    set('accel', accel === null ? '—' : number(accel,2)); set('accel-origin', d.simulado ? 'Dados simulados' : (d.estado_mpu & 1) ? 'Sensor MPU6050' : 'Sensor sem leitura');
    set('soc', soc === null ? '—' : number(soc,1)); $('soc-bar').style.width = Math.max(0,Math.min(100,soc || 0)) + '%';
    set('power', power === null ? '—' : number(Math.abs(power) >= 1000 ? power/1000 : power, Math.abs(power) >= 1000 ? 2 : 0)); set('power-unit', Math.abs(power || 0) >= 1000 ? 'kW' : 'W');
    set('gear', d.simulado ? marchas[d.marcha] || 'Não disponível' : 'Não disponível'); set('distance', format(value(d,'percurso_metros',1000),'km',2)); set('master-time', age(value(d,'tempo_mestre_ms'))); set('source', d.simulado ? 'Dados simulados' : 'Mestre');
    set('voltage', format(d.simulado ? value(d,'tensao_pacote_mv',1000) : null,'V',1)); set('current', format(d.simulado ? value(d,'corrente_centesimos_a',100) : null,'A',2)); set('soc-detail', format(soc,'%',1));
    set('energy', d.simulado && state.samples.length > 1 ? format(state.energyWh,'Wh',1) : 'Não disponível');
    const distanceKm = state.distanceStart === null ? null : (d.percurso_metros - state.distanceStart)/1000;
    set('consumption', d.simulado && distanceKm !== null && distanceKm >= 0.1 && state.energyWh >= 0 ? format(state.energyWh/distanceKm,'Wh/km',1) : 'Não disponível');
    const throttle = d.simulado ? value(d,'acelerador_decimos_percentual',10) : null, brake = d.simulado ? value(d,'freio_decimos_percentual',10) : null;
    set('throttle', format(throttle,'%',1)); set('brake',format(brake,'%',1)); $('throttle-bar').style.width = Math.max(0,Math.min(100,throttle || 0))+'%'; $('brake-bar').style.width = Math.max(0,Math.min(100,brake || 0))+'%';
    const mpu = d.estado_mpu || 0; set('mpu', d.simulado ? 'Dados simulados' : mpu & 1 ? (mpu & 2 ? 'Ativo · calibrado' : 'Ativo · sem calibração') : 'Inativo'); set('mpu-temp', mpu & 1 ? format(value(d,'temperatura_mpu_centesimos_c',100),'°C',1) : 'Não disponível');
    set('packet-rate',format(value(d,'taxa_pacotes_centesimos_hz',100),'Hz',2)); set('sample-age',age(value(d,'idade_ms'))); set('packet-loss',format(value(d,'taxa_perda_centesimos_percentual',100),'%',2)); set('crc-fails',Number.isFinite(d.falhas_crc) ? String(d.falhas_crc) : 'Não disponível');
    badge('link-badge', d.idade_ms < 2000 ? 'Atual' : 'Desatualizado', d.idade_ms < 2000 ? 'good' : 'warn');
    updateArc();
  }
  async function pollTelemetry() {
    if (state.polls.telemetry) return; state.polls.telemetry = true;
    try {
      const d = await getJSON('/api/telemetria'); state.browserOnline = true; state.telemetry = d;
      if (d.valido && d.idade_ms < 2000) { const first = state.lastSequence === null; appendSample(d); if (first) event(d.simulado ? 'Telemetria simulada recebida.' : 'Telemetria recebida do mestre.'); renderTelemetry(d); observeOdometerRun(d); }
      else if (state.lastSequence !== null && (!state.samples.length || !state.samples[state.samples.length-1].gap)) { state.samples.push({time:Date.now(),gap:true}); event('Telemetria desatualizada.','warn'); drawCharts(); }
    } catch (error) { state.browserOnline = Date.now() - state.lastHttp < 5000; if (state.lastSequence !== null && (!state.samples.length || !state.samples[state.samples.length-1].gap)) {state.samples.push({time:Date.now(),gap:true}); event('Conexão com a equipe interrompida.','warn'); drawCharts();} }
    finally {state.browserChecked=true;state.polls.telemetry = false; updateStatus();}
  }
  async function pollLocal() {
    if (state.polls.local) return; state.polls.local = true;
    try { state.local = await getJSON('/api/diagnostico'); state.localAt=Date.now(); state.browserOnline = true; deviceSample('equipe',{cpu:state.local.cpu_valida?value(state.local,'cpu_total_decimos_percentual',10):null,core0:state.local.cpu_valida?value(state.local,'cpu_nucleo_0_decimos_percentual',10):null,core1:state.local.cpu_valida?value(state.local,'cpu_nucleo_1_decimos_percentual',10):null,heap:value(state.local,'heap_interno_livre',1024),temp:state.local.temperatura_valida?value(state.local,'temperatura_centesimos_c',100):null,rate:value(state.local,'taxa_pacotes_centesimos_hz',100)}); }
    catch (_) { state.browserOnline = Date.now() - state.lastHttp < 5000; deviceSample('equipe',{cpu:null,core0:null,core1:null,heap:null,temp:null,rate:null}); }
    finally {state.localChecked=true;state.browserChecked=true;state.polls.local = false; updateStatus(); renderGroup('equipe');}
  }
  async function masterQuery(consult, quiet = true) {
    const key = 'master-' + consult; if (state.polls[key]) return state.polls[key];
    const pending=(async()=>{
      try { const d = await remote(() => getJSON('/api/mestre?consulta=' + encodeURIComponent(consult), 18000)); const restored=consult==='ping'&&!state.masterOnline;state.master[consult] = d; state.masterOnline = true; if(restored){state.lastCommandPoll=0;state.lastAuxPoll=0;}updateMasterUI(consult,d); if(consult==='ping')deviceSample('mestre',{rtt:value(d,'rtt_ms')});if(consult==='diagnostico_mestre')deviceSample('mestre',{cpu:d.cpu_valida?value(d,'cpu_total_decimos_percentual',10):null,core0:d.cpu_valida?value(d,'cpu_nucleo_0_decimos_percentual',10):null,core1:d.cpu_valida?value(d,'cpu_nucleo_1_decimos_percentual',10):null,heap:value(d,'heap_interno_livre',1024),temp:d.temperatura_valida?value(d,'temperatura_centesimos_c',100):null}); if (!quiet) commandEvent('MESTRE', consult, 'Executado', d); return d; }
      catch (error) { if (consult === 'ping'){state.masterOnline = false;deviceSample('mestre',{rtt:null,cpu:null,core0:null,core1:null,heap:null,temp:null});} if (!quiet) commandEvent('MESTRE', consult, error.name === 'AbortError' ? 'Tempo esgotado' : 'Falhou', null, error.message); return null; }
      finally {if(consult==='ping')state.masterChecked=true;updateStatus();availability();renderDevice('mestre','mestre');}
    })();
    state.polls[key]=pending;
    try{return await pending;}finally{state.polls[key]=null;}
  }
  async function visitorsQuery(consult, quiet = true) {
    const key = 'visitor-' + consult; if (state.polls[key]) return null; state.polls[key] = true;
    const name = 'satelite_visitantes_' + consult;
    try {const d = await remote(() => getJSON('/api/mestre?consulta=' + name, 18000)); state.visitors[consult] = d; state.visitorsOnline = true; if(consult==='ping')deviceSample('visitantes',{rtt:value(d,'rtt_ms'),availability:100}); if (!quiet) commandEvent('VISITANTES', consult, 'Executado', d); return d;}
    catch (error) {if (consult === 'ping') {state.visitorsOnline = false; state.visitors.ping = {error:true};deviceSample('visitantes',{rtt:null,availability:0});} if (!quiet) commandEvent('VISITANTES', consult, error.name === 'AbortError' ? 'Tempo esgotado' : 'Falhou', null, error.message); return null;}
    finally {if(consult==='ping')state.visitorsChecked=true;state.polls[key] = false; updateStatus(); renderGroup('visitantes');}
  }
  function commandEvent(target, action, status, result, error) {
    const id = result && result.solicitacao ? ' #' + result.solicitacao : '';
    event(target + ' · ' + action + id + ' · ' + status + (error ? ' · ' + error : ''), status === 'Falhou' || status === 'Tempo esgotado' ? 'bad' : 'info', true);
    if (state.tab === 'mestre') renderDevice('mestre','mestre');
    else if (state.tab === 'equipe' || state.tab === 'visitantes') renderGroup(state.tab);
    renderMasterCommandHistory();
  }
  function deviceStatus(id) {
    if (id === 'equipe') return !state.localChecked ? 'Não consultado' : state.local && Date.now()-state.localAt<7000 ? 'Conectado' : 'Sem resposta';
    if (id === 'mestre') return !state.masterChecked ? 'Não consultado' : state.masterOnline ? 'Conectado' : 'Sem resposta';
    return !state.visitorsChecked ? 'Não consultado' : state.visitorsOnline ? 'Conectado' : 'Sem resposta';
  }
  function deviceData(id) {
    if (id === 'equipe') return state.local;
    if (id === 'mestre') return state.master.diagnostico_mestre || state.master.ping;
    return state.visitors.versao || state.visitors.ping;
  }
  function makeGroup(group) {
    const root = $(group + '-content');
    root.innerHTML = '<article id="'+group+'-details" class="card device-details"></article>';
  }
  function renderGroup(group) {
    if (!$(group+'-content').firstElementChild) makeGroup(group);
    renderDevice(group,group);
  }
  function dataRows(id) {
    const d = deviceData(id), no = 'Não disponível';
    if (id === 'equipe') return [['Firmware',d?.versao_firmware || no],['CPU',d?.cpu_valida ? format(value(d,'cpu_total_decimos_percentual',10),'%',1) : no],['Temperatura interna',d?.temperatura_valida ? format(value(d,'temperatura_centesimos_c',100),'°C',1) : no],['Wi-Fi',d ? d.wifi_ssid + ' · ' + d.clientes_wifi + ' cliente(s)' : no],['UART RX',d ? String(d.uart_recepcoes_ok) : no],['Tempo ligado',d ? age(d.tempo_ativo_ms) : no],['Heap interno livre',d ? bytes(d.heap_interno_livre) : no],['PSRAM livre',d?.psram_total ? bytes(d.psram_livre) : no],['Último reinício',d ? 'Código ' + d.motivo_reinicio : no]];
    if (id === 'mestre') {const diag=state.master.diagnostico_mestre,version=state.master.versao,ping=state.master.ping,sync=state.master.sincronizacao;return [['Firmware',version?.firmware || no],['CPU',diag?.cpu_valida ? format(value(diag,'cpu_total_decimos_percentual',10),'%',1) : no],['Temperatura interna',diag?.temperatura_valida ? format(value(diag,'temperatura_centesimos_c',100),'°C',1) : no],['Tempo ligado',ping ? age(ping.tempo_ativo_ms) : no],['Heap livre',diag ? bytes(diag.heap_livre) : no],['PSRAM',diag?.psram_disponivel ? bytes(diag.psram_livre) : no],['Último reinício',diag ? 'Código ' + diag.motivo_ultimo_reset : no],['Sincronização Google',sync ? sync.em_andamento ? 'Em andamento' : sync.sincronizacoes_finalizadas ? sync.ultimo_resultado_sincronizacao===0 ? 'Concluída' : 'Falhou' : 'Pronta' : no],['Arquivos pendentes no SD',sync ? String(sync.arquivos_pendentes) : no]];}
    const ping=state.visitors.ping,version=state.visitors.versao;return [['Firmware',version?.firmware || no],['Tempo ligado',ping && !ping.error ? age(ping.tempo_ativo_ms) : no],['Capacidades',version?.capacidades !== undefined ? '0x'+version.capacidades.toString(16).toUpperCase() : no]];
  }
  function renderDevice(group,id) {
    const root=$(group+'-details'),d=devices[id],status=deviceStatus(id),rows=dataRows(id),rtt=id==='mestre'?state.master.ping?.rtt_ms:id==='visitantes'?state.visitors.ping?.rtt_ms:null;
    const link=id==='equipe'?'Navegador → ESP da equipe':id==='mestre'?'Equipe → mestre, UART':'Equipe → mestre → visitantes';
    const rate=id==='equipe'||id==='mestre' ? state.local&&state.local.valido ? format(value(state.local,'taxa_pacotes_centesimos_hz',100),'Hz',2) : 'Não disponível' : 'Não disponível';
    const signature = id;
    if (root.dataset.signature !== signature) {
      root.dataset.signature=signature;
      const communication=id==='visitantes'
        ? '<div><dt>Ligação</dt><dd>'+link+'</dd></div><div><dt>Tempo da consulta</dt><dd id="device-rtt-'+group+'">Não disponível</dd></div><div><dt>Estado do enlace</dt><dd id="device-link-'+group+'">Aguardando consulta</dd></div>'
        : '<div><dt>Ligação</dt><dd>'+link+'</dd></div><div><dt>Tempo da consulta</dt><dd id="device-rtt-'+group+'">Não disponível</dd></div><div><dt>Taxa de telemetria mestre → equipe</dt><dd id="device-rate-'+group+'">Não disponível</dd></div><div><dt>Perda estimada</dt><dd id="device-loss-'+group+'">Não disponível</dd></div>';
      root.innerHTML='<div class="device-title"><div><h2>'+d.name+'</h2><p>'+d.id+' · ESP32-S3 · '+d.role+' · '+d.function+'</p></div><span id="device-status-'+group+'" class="badge muted"></span></div><div class="device-section"><h3>Dados e sistema</h3><dl id="device-rows-'+group+'"></dl></div><div class="device-section"><h3>Comunicação</h3><dl>'+communication+'</dl><div class="device-chart-head"><div><h3>Histórico do dispositivo</h3><p id="device-chart-label-'+group+'"></p></div><label>Visualizar<select id="device-chart-select-'+group+'" aria-label="Dados do gráfico de '+d.name+'"></select></label></div><div class="device-chart"><canvas id="device-chart-'+group+'" role="img"></canvas></div></div>';
      const selector=$('device-chart-select-'+group);
      deviceChartOptions[id].forEach(option=>{const item=document.createElement('option');item.value=option.key;item.textContent=option.label;selector.append(item);});
      selector.value=state.deviceSignal[id];
      selector.addEventListener('change',()=>{state.deviceSignal[id]=selector.value;renderDevice(group,id);});
    }
    badge('device-status-'+group,status,status==='Conectado'?'good':status==='Sem resposta'?'bad':'muted');
    const dl=$('device-rows-'+group);
    if(dl.children.length!==rows.length){dl.replaceChildren();rows.forEach(([label])=>{const div=document.createElement('div'),dt=document.createElement('dt'),dd=document.createElement('dd');dt.textContent=label;div.append(dt,dd);dl.append(div);});}
    rows.forEach(([,text],index)=>{const cell=dl.children[index].lastElementChild;if(cell.textContent!==text)cell.textContent=text;});
    set('device-rtt-'+group,Number.isFinite(rtt)?rtt+' ms':'Não disponível');
    if(id==='visitantes')set('device-link-'+group,status==='Conectado'?'Respondendo':status==='Sem resposta'?'Sem resposta':'Aguardando consulta');
    else {set('device-rate-'+group,rate);set('device-loss-'+group,state.local&&state.local.valido?format(value(state.local,'taxa_perda_centesimos_percentual',100),'%',2):'Não disponível');}
    const chart=deviceChartOptions[id].find(option=>option.key===state.deviceSignal[id]);
    set('device-chart-label-'+group,chart.label+' · '+chart.unit+' · leituras nesta página');$('device-chart-'+group).setAttribute('aria-label','Histórico de '+chart.label+' de '+d.name);requestAnimationFrame(()=>drawDeviceChart(group,id));
    badge(group+'-auto',status+' · atualização automática',status==='Conectado'?'good':status==='Sem resposta'?'bad':'muted');
  }
  const actionNames={verificar_ota:'Verificar atualizações',autorizar_mestre:'Autorizar OTA do mestre',autorizar_equipe:'Autorizar OTA da Equipe',autorizar_visitantes:'Autorizar OTA de Visitantes',autorizar_todos:'Autorizar OTA de todos',cancelar_ota:'Cancelar OTA',alternar_relatorios:'Pausar ou retomar relatórios',resumo_terminal:'Solicitar resumo no terminal',sincronizar_google:'Solicitar sincronização',zerar_percurso:'Zerar percurso',intervalo_relatorio:'Alterar intervalo do relatório',intervalo_ota:'Alterar intervalo de verificação OTA',intervalo_telemetria_ota:'Alterar telemetria durante OTA',wifi_test:'Testar nova rede Wi-Fi',wifi_conectar_salva:'Conectar rede salva',wifi_excluir_salva:'Excluir rede salva',wifi_primary:'Definir nova rede principal',wifi_restaurar_principal:'Restaurar rede principal',wifi_automatico:'Alterar conexão automática',wifi_politica:'Salvar política Wi-Fi'};
  const timeOf = time => new Date(time).toLocaleTimeString('pt-BR');
  function runMeta(run) {
    const ids=run.requests.length ? 'Solicitação '+run.requests.map(id=>'#'+id).join(', ') : 'Número da solicitação ainda indisponível';
    const response=run.responses[run.responses.length-1];
    return timeOf(run.time)+' · '+ids+(response&&Number.isFinite(response.duracao_ms)?' · Resposta em '+response.duracao_ms+' ms':'')+(response&&Number.isFinite(response.tentativas)?' · '+response.tentativas+' tentativa(s)':'')+(run.checkedAt ? ' · Estado consultado às '+timeOf(run.checkedAt) : '');
  }
  function renderMasterCommandHistory(){
    const list=$('master-command-history');if(!list)return;
    const current=state.commandRuns[0];
    if(current){
      set('command-live-name',current.name);badge('command-live-status',current.status,current.tone);set('command-live-text',current.text);set('command-live-meta',runMeta(current));
      set('command-stage-send',current.status==='Enviando'?'Em andamento':current.responses.length?'Enviado':current.status==='Preparando'?'Aguardando':'Tentado');
      set('command-stage-response',current.status==='Falhou'||current.status==='Sem resposta'?current.responses.length?'Parcial':current.status==='Falhou'?'Falha':'Sem resposta':current.responses.length?'Recebida':'Aguardando');
      set('command-stage-state',current.checkedAt?'Consultado':current.hasRefresh?'Não consultado':'Não disponível');
    }
    list.replaceChildren();
    if(!state.commandRuns.length){const li=document.createElement('li');li.textContent='Nenhuma ação enviada nesta página.';list.append(li);return;}
    state.commandRuns.forEach(run=>{
      const li=document.createElement('li'),head=document.createElement('div'),name=document.createElement('strong'),status=document.createElement('span'),detail=document.createElement('p'),meta=document.createElement('small');
      li.className='command-run';head.className='command-run-head';name.textContent=run.name;status.className='badge '+run.tone;status.textContent=run.status;detail.textContent=run.text;meta.textContent=runMeta(run);
      head.append(name,status);li.append(head,detail,meta);list.append(li);
    });
  }
  function availability(){
    const capabilities=state.master.versao?.capacidades;
    document.querySelectorAll('[data-master-action]').forEach(button=>{const mask=Number(button.dataset.cap);button.disabled=state.busy||!state.masterOnline||!Number.isFinite(capabilities)||(capabilities&mask)!==mask;});
    const sync=state.master.sincronizacao,syncButton=document.querySelector('[data-master-action="sincronizar_google"]');
    if(syncButton&&sync&&(sync.em_andamento||!sync.iniciado||!sync.cartao_disponivel))syncButton.disabled=true;
    $('run-master-query').disabled=state.busy;
    $('refresh-ota-state').disabled=state.busy;
    $('refresh-sync-state').disabled=state.busy||!state.masterOnline||(Number.isFinite(capabilities)&&(capabilities&CAP_SYNC)===0);
    const supported=Number.isFinite(capabilities)&&(capabilities&(128|256|512))!==0;
    badge('master-command-status',state.masterChecked&&!state.masterOnline?'Mestre sem resposta':supported?'Comandos disponíveis':Number.isFinite(capabilities)?'Ações não anunciadas pelo mestre':'Aguardando capacidades do mestre',state.masterChecked&&!state.masterOnline?'bad':supported?'good':'muted');
  }
  function setField(id,newValue){const field=$(id);if(field&&!field.dataset.edited&&document.activeElement!==field)field.value=String(newValue);}
  const otaMasterStates=['Desabilitado','Inicializando','Aguardando rede','Verificando','Atualizado','Aguardando autorização','Baixando','Aplicado','Cancelado','Falha'];
  const otaSatelliteStates=['Desconhecido','Indisponível','Não suportado','Atualizado','Aguardando autorização','Transferindo','Reiniciando','Concluído','Cancelado','Falha'];
  const otaTargets=['Nenhum','Todos','Mestre','Equipe','Visitantes'];
  function otaTone(code,satellite){
    if(code===9)return 'bad';
    if(satellite ? code===3||code===7 : code===4||code===7)return 'good';
    if(satellite ? code===4||code===5||code===6 : code===2||code===3||code===5||code===6)return 'warn';
    return 'muted';
  }
  function renderOtaState(data){
    const detailed=!!(data.mestre&&data.equipe&&data.visitantes);
    const general=otaMasterStates[data.estado]||'Estado desconhecido';
    const target=otaTargets[data.alvo]||'Não informado';
    set('ota-command-state','Estado geral: '+general+' · alvo ativo: '+target+'.'+(Number.isFinite(data.falhas)?' Falhas registradas: '+data.falhas+'.':''));
    const items=detailed?[
      {name:'Mestre',value:data.mestre,satellite:false},
      {name:'Equipe',value:data.equipe,satellite:true},
      {name:'Visitantes',value:data.visitantes,satellite:true}
    ]:[{name:'Estado informado pelo mestre',value:data,satellite:false}];
    const root=$('ota-targets');root.replaceChildren();
    items.forEach(item=>{
      const row=document.createElement('article'),head=document.createElement('div'),name=document.createElement('strong'),status=document.createElement('span'),versions=document.createElement('p'),progress=document.createElement('div'),fill=document.createElement('span');
      const code=Number(item.value.estado),states=item.satellite?otaSatelliteStates:otaMasterStates;
      const percent=Number(item.value.progresso_percentual),validPercent=Number.isFinite(percent)?Math.max(0,Math.min(100,percent)):0;
      row.className='ota-target';head.className='ota-target-head';name.textContent=item.name;status.className='badge '+otaTone(code,item.satellite);status.textContent=states[code]||'Estado '+code;
      versions.className='ota-versions';versions.textContent='Atual: '+(item.value.versao_atual||'não informada')+' · Disponível: '+(item.value.versao_disponivel||'não informada');
      progress.className='ota-progress';progress.setAttribute('role','progressbar');progress.setAttribute('aria-label','Progresso OTA de '+item.name);progress.setAttribute('aria-valuemin','0');progress.setAttribute('aria-valuemax','100');progress.setAttribute('aria-valuenow',String(validPercent));fill.style.width=validPercent+'%';progress.append(fill);
      const measure=document.createElement('small');measure.textContent=Number.isFinite(percent)&&(validPercent>0||code===(item.satellite?5:6)||code===7)?number(validPercent,1)+'%':'Sem transferência ativa';
      head.append(name,status);row.append(head,versions,progress,measure);root.append(row);
    });
    const checked=Date.now();
    set('ota-state-checked','Consultado às '+timeOf(checked)+(Number.isFinite(data.solicitacao)?' · solicitação #'+data.solicitacao:'')+(Number.isFinite(data.duracao_ms)?' · resposta em '+data.duracao_ms+' ms':''));
    const run=state.commandRuns.find(item=>item.followOta&&item.status!=='Falhou'&&item.status!=='Sem resposta');
    if(run&&run.responses.length){
      const progressText=items.map(item=>{const current=(item.satellite?otaSatelliteStates:otaMasterStates)[item.value.estado]||'desconhecido';return item.name+': '+current+(Number(item.value.progresso_percentual)>0?' '+number(item.value.progresso_percentual,1)+'%':'');}).join(' · ');
      const active=[2,3,6].includes(Number(data.estado))||items.some(item=>item.satellite&&[5,6].includes(Number(item.value.estado)));
      const failed=Number(data.estado)===9||items.some(item=>Number(item.value.estado)===9);
      run.checkedAt=checked;run.status=failed?'Falha observada':active?'OTA em andamento':'Estado OTA';run.tone=failed?'bad':active?'warn':'good';
      run.text='Estado atual do mestre: '+general+'. '+progressText+'. Esta leitura mostra o estado observado após a solicitação.';
      renderMasterCommandHistory();
    }
  }
  function renderSyncState(data){
    const checked=Date.now(),started=Number(data.sincronizacoes_iniciadas)||0,finished=Number(data.sincronizacoes_finalizadas)||0;
    const total=Number(data.arquivos_upload_total)||0,done=Number(data.arquivos_upload_concluidos)||0;
    const sent=Number(data.arquivo_atual_bytes_enviados)||0,size=Number(data.arquivo_atual_bytes_total)||0;
    const activeFile=!!data.em_andamento&&size>0;
    const progress=activeFile?Math.min(100,Math.max(0,sent/size*100)):0;
    let label='Pronto',kind='muted',summary='Nenhuma sincronização em andamento.';
    if(!data.iniciado||!data.cartao_disponivel){label='Indisponível';kind='bad';summary='O registro no cartão SD não está disponível no mestre.';}
    else if(data.em_andamento&&data.aguardando_confirmacao){label='Confirmando';kind='warn';summary='Arquivo enviado; aguardando a confirmação do Google.';}
    else if(data.em_andamento&&total>0){label='Enviando';kind='warn';summary='Enviando arquivo '+Math.min(done+1,total)+' de '+total+'.';}
    else if(data.em_andamento){label='Preparando';kind='warn';summary='Preparando o arquivo, a rede e a conexão para o upload.';}
    else if(started>0&&finished>=started){
      const success=data.ultimo_resultado_sincronizacao===0;
      label=success?'Concluída':'Falhou';kind=success?'good':'bad';
      summary=success?(total===0?'Nenhum arquivo pendente na última execução.':'Última sincronização concluída; '+data.arquivos_ultima_sincronizacao+' arquivo(s) confirmados pelo Google.'):'A última sincronização terminou com erro. Os arquivos não confirmados permanecem pendentes.';
    }
    badge('sync-badge',label,kind);
    set('sync-summary',summary+' Pendentes no SD: '+(Number(data.arquivos_pendentes)||0)+'.');
    set('sync-files',done+' / '+total);
    set('sync-current',activeFile?data.arquivo_em_upload||'Preparando arquivo':'Nenhum arquivo em envio');
    set('sync-bytes',activeFile?bytes(sent)+' / '+bytes(size):'Sem transferência ativa');
    set('sync-rate',activeFile?bytes(Number(data.velocidade_upload_bytes_s)||0)+'/s · '+(data.tempo_restante_upload_s?age(data.tempo_restante_upload_s*1000):'tempo não estimado'):'—');
    const bar=$('sync-progress-fill'),track=bar.parentElement;bar.style.width=progress+'%';track.setAttribute('aria-valuenow',String(Math.round(progress)));track.setAttribute('aria-valuetext',activeFile?number(progress,1)+'% do arquivo atual':'Sem arquivo em transferência');
    set('sync-progress-label',activeFile?number(progress,1)+'% do arquivo atual':data.em_andamento?'Preparando próximo arquivo':'Sem transferência ativa');
    set('sync-last-result',finished>0?'Última execução: '+data.arquivos_ultima_sincronizacao+' arquivo(s), '+bytes(data.bytes_ultima_sincronizacao)+' em '+age(data.duracao_ultima_sincronizacao_ms)+' · '+(data.ultimo_resultado_sincronizacao===0?'sem erro':(data.ultimo_resultado_nome||'erro '+data.ultimo_resultado_sincronizacao))+'.':'Nenhuma sincronização concluída nesta inicialização.');
    set('sync-checked','Consultado às '+timeOf(checked)+(Number.isFinite(data.solicitacao)?' · solicitação #'+data.solicitacao:'')+'.');
    const run=state.commandRuns.find(item=>item.followSync&&item.responses.length&&['Preparando','Resposta recebida','Acompanhar estado','Sincronizando','Aguardando Google'].includes(item.status));
    if(!run)return;
    run.checkedAt=checked;
    if(run.syncGeneration===null)run.syncGeneration=started;
    if(started!==run.syncGeneration){updateRun(run,'Contadores alterados',started<run.syncGeneration?'Os contadores do mestre reiniciaram. O estado atual não confirma o resultado desta solicitação.':'O mestre iniciou outra sincronização. O estado atual não confirma o resultado desta solicitação.','warn');return;}
    if(data.em_andamento){updateRun(run,label==='Confirmando'?'Aguardando Google':'Sincronizando',summary+' '+done+' de '+total+' arquivo(s) confirmados.','warn');return;}
    if(finished>=run.syncGeneration&&run.syncGeneration>0){
      const success=data.ultimo_resultado_sincronizacao===0;
      updateRun(run,success?'Concluída':'Falhou',success?summary:'A sincronização terminou com '+(data.ultimo_resultado_nome||'erro '+data.ultimo_resultado_sincronizacao)+'. '+(Number(data.arquivos_pendentes)||0)+' arquivo(s) ainda pendente(s).',success?'good':'bad');
    }else updateRun(run,'Preparando','O mestre aceitou a solicitação; a tarefa ainda não informou o resultado final.','warn');
  }
  function updateMasterUI(consult,data){
    if(consult==='versao'){
      availability();
      if(Number.isFinite(data.capacidades)&&(data.capacidades&CAP_SYNC)===0){badge('sync-badge','Não suportado','muted');set('sync-summary','O firmware atual do mestre não informa o andamento da sincronização. Atualize o mestre para habilitar esse acompanhamento.');set('sync-checked','Consulta detalhada indisponível neste mestre.');}
    }
    if(consult==='configuracao'){
      set('report-state',data.relatorios_pausados?'Pausados':'Ativos');
      const group=Number($('report-group').value);
      if(Array.isArray(data.intervalos_relatorios_ms))setField('report-interval',data.intervalos_relatorios_ms[group]);
      setField('ota-interval',data.intervalo_ota_minutos);
      setField('telemetry-ota-interval',data.intervalo_telemetria_ota_ms);
      setField('wifi-auto',data.wifi_automatico?'1':'0');
      setField('wifi-attempts',data.tentativas_wifi);
      setField('wifi-attempt-time',data.tempo_tentativa_wifi_ms);
      setField('wifi-retry-interval',data.intervalo_tentativas_wifi_ms);
      setField('wifi-session-limit',data.tempo_limite_wifi_ms);
    }
    if(consult==='ota')renderOtaState(data);
    if(consult==='sincronizacao')renderSyncState(data);
    if(consult==='wifi')set('wifi-command-state',data.conectado?'Mestre conectado a '+(data.rede||'rede não identificada')+' · RSSI '+data.rssi_dbm+' dBm':data.radio_ativo?'Rádio do mestre ativo; sem conexão informada.':'Rádio do mestre inativo.');
    if(consult==='wifi'||consult==='configuracao')observeOperationalRun(consult,data);
  }
  async function loadSavedNetworks(count){
    if(state.polls.savedNetworks)return;state.polls.savedNetworks=true;
    const select=$('wifi-saved'),selected=select.value,networks=[];
    try{
      for(let index=1;index<=Math.min(5,Number(count)||0);index++){
        try{const data=await remote(()=>getJSON('/api/mestre?consulta=rede_wifi_salva&indice='+index,18000));networks.push({index,ssid:data.ssid});}catch(_){/* A posição fica indisponível. */}
      }
      select.replaceChildren();
      if(!networks.length){const option=document.createElement('option');option.value='';option.textContent='Nenhuma rede salva informada';select.append(option);}
      else networks.forEach(({index,ssid})=>{const option=document.createElement('option');option.value=String(index);option.textContent=index+' · '+ssid;select.append(option);});
      if(networks.some(item=>String(item.index)===selected))select.value=selected;
    }finally{state.polls.savedNetworks=false;}
  }
  document.querySelectorAll('#command-center input,#command-center select').forEach(field=>field.addEventListener('input',()=>{field.dataset.edited='1';}));
  $('report-group').addEventListener('change',()=>{const config=state.master.configuracao;if(config?.intervalos_relatorios_ms){$('report-interval').dataset.edited='';setField('report-interval',config.intervalos_relatorios_ms[Number($('report-group').value)]);}});
  function bytesLength(text){return new TextEncoder().encode(text).length;}
  function numberField(id){
    const input=$(id),raw=input.value.trim(),n=Number(raw),min=Number(input.min),max=Number(input.max);
    if(!raw||!Number.isInteger(n)||n<min||n>max)throw new Error('Confira '+(input.closest('label')?.textContent.trim()||id)+' ('+min+' a '+max+').');
    return String(n);
  }
  function permission(){const secret=$('wifi-permission').value;if(!secret||secret.includes('\n')||bytesLength(secret)>=108)throw new Error('Informe uma permissão administrativa válida.');return secret;}
  function preparedNetwork(){const ssid=$('wifi-ssid').value,password=$('wifi-password').value;if(!ssid||bytesLength(ssid)>32||bytesLength(password)<8||bytesLength(password)>63||ssid.includes('\n')||password.includes('\n'))throw new Error('Informe SSID de até 32 bytes e senha de 8 a 63 bytes.');return ssid+'\n'+password;}
  function savedIndex(){const index=$('wifi-saved').value;if(!/^[1-5]$/.test(index))throw new Error('Selecione uma rede salva informada pelo mestre.');return index;}
  function buildPlan(action){
    const name=actionNames[action];if(!name)throw new Error('Comando desconhecido.');
    const plan={name,action,steps:[{code:action,body:''}],refresh:[],secret:false};
    if(['verificar_ota','autorizar_mestre','autorizar_equipe','autorizar_visitantes','autorizar_todos','cancelar_ota'].includes(action))plan.refresh=['ota'];
    if(action==='alternar_relatorios')plan.refresh=['configuracao'];
    if(action==='zerar_percurso')plan.note='O percurso atualizado aparece na página Telemetria; a API não relaciona essa leitura ao comando.';
    if(action==='resumo_terminal')plan.note='O resumo é emitido no terminal físico do mestre; a API não devolve seu conteúdo.';
    if(action==='sincronizar_google')plan.refresh=['sincronizacao'];
    if(action==='intervalo_relatorio'){const group=Number($('report-group').value),interval=Number(numberField('report-interval'));plan.steps[0].body=group+'\n'+interval;plan.refresh=['configuracao'];plan.expected={field:'intervalos_relatorios_ms',index:group,value:interval,label:'Intervalo do relatório'};}
    if(action==='intervalo_ota'){const interval=Number(numberField('ota-interval'));plan.steps[0].body=String(interval);plan.refresh=['configuracao'];plan.expected={field:'intervalo_ota_minutos',value:interval,label:'Verificação OTA'};}
    if(action==='intervalo_telemetria_ota'){const interval=Number(numberField('telemetry-ota-interval'));plan.steps[0].body=String(interval);plan.refresh=['configuracao'];plan.expected={field:'intervalo_telemetria_ota_ms',value:interval,label:'Telemetria durante OTA'};}
    if(action==='wifi_test'){plan.expectedSsid=$('wifi-ssid').value;plan.steps=[{code:'wifi_preparar',body:preparedNetwork()},{code:'wifi_conectar_preparada',body:''}];if($('wifi-save').checked)plan.steps.push({code:'wifi_salvar_preparada',body:''});plan.secret=true;plan.refresh=['wifi','configuracao'];}
    if(action==='wifi_primary'){plan.expectedSsid=$('wifi-ssid').value;const network=preparedNetwork(),secret=permission();plan.steps=[{code:'wifi_preparar',body:network},{code:'wifi_alterar_principal',body:secret}];plan.secret=true;plan.refresh=['wifi','configuracao'];}
    if(action==='wifi_conectar_salva'||action==='wifi_excluir_salva'){plan.steps[0].body=savedIndex();plan.refresh=['wifi','configuracao'];if(action==='wifi_conectar_salva')plan.expectedSsid=($('wifi-saved').selectedOptions[0]?.textContent||'').split(' · ').slice(1).join(' · ');}
    if(action==='wifi_restaurar_principal'){plan.steps[0].body=permission();plan.secret=true;plan.refresh=['wifi','configuracao'];}
    if(action==='wifi_automatico'){const enabled=$('wifi-auto').value==='1';plan.steps[0].body=(enabled?'1':'0')+'\n'+permission();plan.secret=true;plan.refresh=['configuracao'];plan.expected={field:'wifi_automatico',value:enabled,label:'Conexão automática'};}
    if(action==='wifi_politica'){plan.steps[0].body=['wifi-attempts','wifi-attempt-time','wifi-retry-interval','wifi-session-limit'].map(numberField).join('\n')+'\n'+permission();plan.secret=true;plan.refresh=['configuracao'];}
    return plan;
  }
  function observedConfiguration(plan,config){
    if(!plan.expected)return null;
    const expected=plan.expected,current=expected.index===undefined?config[expected.field]:config[expected.field]?.[expected.index];
    if(current===undefined)return null;
    return {matches:current===expected.value,text:expected.label+': valor atual '+String(current)+'; solicitado '+String(expected.value)+'.'};
  }
  function observeOperationalRun(consult,data){
    const run=state.commandRuns[0];
    if(!run||!run.responses.length||!run.refreshQueries?.includes(consult)||Date.now()>run.followUntil||['Falhou','Sem resposta'].includes(run.status))return;
    run.checkedAt=Date.now();
    if(consult==='configuracao'){
      const observed=observedConfiguration(run,data);
      if(observed)updateRun(run,observed.matches?'Valor confirmado':'Aguardando valor',observed.text+' Leitura atual do mestre.',observed.matches?'good':'warn');
      else if(run.action==='alternar_relatorios')updateRun(run,'Estado consultado','Relatórios no mestre: '+(data.relatorios_pausados?'pausados':'ativos')+'.','good');
      else if(run.action==='wifi_excluir_salva')updateRun(run,'Redes consultadas','O mestre informa '+data.quantidade_redes_wifi+' rede(s) salva(s). Confira a lista atualizada antes de considerar a exclusão concluída.','warn');
      else if(run.action==='wifi_politica')updateRun(run,'Política consultada','Política atual: '+data.tentativas_wifi+' tentativa(s), '+age(data.tempo_tentativa_wifi_ms)+' por tentativa, intervalo de '+age(data.intervalo_tentativas_wifi_ms)+'.','good');
      else if(!run.refreshQueries.includes('wifi'))updateRun(run,'Estado consultado','Configuração atual do mestre consultada. Os valores do formulário refletem a última leitura.','good');
    }else if(consult==='wifi'){
      const connected=!!data.conectado,matched=connected&&run.expectedSsid&&data.rede===run.expectedSsid;
      const status=matched?'Rede observada':connected&&run.expectedSsid?'Aguardando rede':connected?'Wi-Fi consultado':'Aguardando conexão';
      const detail=connected?'Mestre conectado a '+(data.rede||'rede não identificada')+'.':'O mestre ainda não informa conexão ativa.';
      updateRun(run,status,detail+(run.expectedSsid&&!matched?' Rede solicitada: '+run.expectedSsid+'.':''),matched?'good':'warn');
    }
  }
  function observeOdometerRun(data){
    const run=state.commandRuns[0];
    if(!run||run.action!=='zerar_percurso'||!run.responseAt||run.status==='Percurso observado'||!Number.isFinite(data.percurso_metros)||!Number.isFinite(data.idade_ms)||Date.now()-data.idade_ms<=run.responseAt)return;
    run.checkedAt=Date.now();
    updateRun(run,'Percurso observado','Leitura posterior à resposta do mestre: '+number(data.percurso_metros/1000,2)+' km no percurso atual.','good');
  }
  function updateRun(run,status,text,tone='muted'){
    run.status=status;run.text=text;run.tone=tone;renderMasterCommandHistory();
  }
  async function postMaster(code,body,run){
    commandEvent('MESTRE',code,'Enviando');
    const timeout=['wifi_conectar_preparada','wifi_conectar_salva','wifi_alterar_principal'].includes(code)?200000:20000;
    try{const result=await remote(async()=>{const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),timeout);try{const response=await fetch('/api/mestre?consulta='+encodeURIComponent(code),{method:'POST',headers:{'Content-Type':'text/plain; charset=utf-8'},body,cache:'no-store',signal:controller.signal});const data=await response.json();if(!response.ok||!data.ok){const error=new Error(data.erro||'Comando recusado');error.requestId=data.solicitacao;throw error;}return data;}finally{clearTimeout(timer);}});if(Number.isFinite(result.solicitacao))run.requests.push(result.solicitacao);if(run.followSync&&Number.isFinite(result.sincronizacao_id))run.syncGeneration=result.sincronizacao_id;run.responses.push({duracao_ms:result.duracao_ms,tentativas:result.tentativas});run.responseAt=Date.now();commandEvent('MESTRE',code,'Recebido',result);return result;}
    catch(error){if(Number.isFinite(error.requestId))run.requests.push(error.requestId);commandEvent('MESTRE',code,error.name==='AbortError'?'Tempo esgotado':'Falhou',null,error.message);throw error;}
  }
  async function executePlan(plan){
    const run={name:plan.name,action:plan.action,time:Date.now(),requests:[],responses:[],step:0,status:'Preparando',text:'Preparando o envio ao mestre.',tone:'muted',checkedAt:0,hasRefresh:plan.refresh.length>0||plan.action==='zerar_percurso',refreshQueries:plan.refresh,expected:plan.expected,expectedSsid:plan.expectedSsid,followUntil:Date.now()+300000,followOta:plan.refresh.includes('ota'),followSync:plan.refresh.includes('sincronizacao'),syncGeneration:null};
    state.commandRuns.unshift(run);state.commandRuns.length=Math.min(state.commandRuns.length,12);renderMasterCommandHistory();
    state.busy=true;availability();
    try{
      for(let index=0;index<plan.steps.length;index++){
        run.step=index+1;
        updateRun(run,'Enviando','Enviando etapa '+(index+1)+' de '+plan.steps.length+' ao mestre.','warn');
        await postMaster(plan.steps[index].code,plan.steps[index].body,run);
        updateRun(run,'Resposta recebida','O mestre respondeu à etapa '+(index+1)+' de '+plan.steps.length+'. A execução posterior ainda pode estar em andamento.','warn');
      }
      run.followUntil=Date.now()+120000;
      let checked=0;const observed={};
      for(const query of plan.refresh){const result=await masterQuery(query);if(result){checked++;observed[query]=result;run.checkedAt=Date.now();}if(query==='configuracao'&&result)await loadSavedNetworks(result.quantidade_redes_wifi);}
      if(plan.refresh.length&&!checked){
        const unsupported=run.followSync&&Number.isFinite(state.master.versao?.capacidades)&&(state.master.versao.capacidades&CAP_SYNC)===0;
        updateRun(run,unsupported?'Sem acompanhamento':'Acompanhar estado',unsupported?'O mestre aceitou a ação, mas seu firmware não oferece o andamento detalhado. Atualize o mestre para acompanhar esta sincronização.':'O mestre respondeu ao comando, mas a consulta do estado não retornou. A atualização automática tentará novamente.','warn');
        message(plan.name+': resposta recebida; '+(unsupported?'o mestre não oferece consulta detalhada.':'não foi possível consultar o estado atualizado.'),unsupported?'':'error');
      }
      else if(!run.followSync&&!run.followOta){
        const otaObserved=run.followOta&&state.master.ota&&checked?' Último estado observado: '+(otaMasterStates[state.master.ota.estado]||'desconhecido')+'.':'';
        const configObserved=observed.configuracao?observedConfiguration(plan,observed.configuracao):null;
        if(configObserved)updateRun(run,configObserved.matches?'Valor observado':'Valor divergente',configObserved.text+' A resposta inicial confirma o aceite; esta leitura mostra a configuração atual.',configObserved.matches?'good':'warn');
        else if(plan.steps[0].code==='alternar_relatorios'&&observed.configuracao)updateRun(run,'Estado consultado','Relatórios no mestre: '+(observed.configuracao.relatorios_pausados?'pausados':'ativos')+'. A consulta mostra o estado atual após a solicitação.','good');
        else if(observed.wifi)updateRun(run,'Wi-Fi consultado',observed.wifi.conectado?'Mestre conectado a '+(observed.wifi.rede||'uma rede não identificada')+'.':'O mestre ainda não informa conexão Wi-Fi ativa. Consulte novamente após a operação.','warn');
        else updateRun(run,run.followOta?'Acompanhar OTA':'Resposta recebida',run.followOta?'O mestre respondeu ao comando.'+otaObserved+' Confira o estado de cada dispositivo abaixo para acompanhar a conclusão.':plan.note|| (checked?'O mestre respondeu ao comando e o estado foi consultado. Confira os dados atuais antes de considerar a operação concluída.':'O mestre respondeu ao comando. A API não informa a conclusão posterior desta ação.'),run.followOta?'warn':checked?'good':'warn');
        message(plan.name+': resposta recebida pelo mestre.'+(checked?' Estado consultado.':''),'success');
      }
      else message(plan.name+': resposta recebida. O andamento aparece no painel de '+(run.followSync?'sincronização':'OTA')+'.','success');
      if(plan.secret){$('wifi-password').value='';$('wifi-permission').value='';}
    }catch(error){const timeout=error.name==='AbortError',reason=timeout?'O tempo de resposta esgotou':error.message;updateRun(run,timeout?'Sem resposta':'Falhou','Etapa '+run.step+' de '+plan.steps.length+': '+reason+'. A etapa não teve confirmação de sucesso. Confira o estado antes de repetir a ação.',timeout?'warn':'bad');message(plan.name+': '+reason+'. Confira o estado antes de repetir a ação.','error');}
    finally{state.busy=false;availability();}
  }
  document.querySelectorAll('[data-master-action]').forEach(button=>button.addEventListener('click',()=>{
    if(state.busy||button.disabled)return;
    try{state.pending=buildPlan(button.dataset.masterAction);$('confirm-title').textContent=state.pending.name;$('confirm-text').textContent='Enviar '+state.pending.steps.length+' solicitação(ões) ao mestre? A resposta confirma o recebimento; operações como OTA e Wi-Fi podem continuar depois.';$('confirm-dialog').showModal();}
    catch(error){message(error.message,'error');}
  }));
  $('confirm-dialog').addEventListener('close',()=>{const plan=state.pending;state.pending=null;if($('confirm-dialog').returnValue==='confirm'&&plan)executePlan(plan);});
  $('run-master-query').addEventListener('click',async()=>{
    if(state.busy)return;state.busy=true;availability();
    const query=$('master-query').value;commandEvent('MESTRE',query,'Enviando');
    try{const result=await masterQuery(query,false);if(!result)throw new Error('Sem resposta do mestre');
      const payload=Object.fromEntries(Object.entries(result).filter(([key])=>!['ok','consulta','comando','solicitacao','resultado','tentativas','duracao_ms'].includes(key)));
      const view=$('master-query-result');view.textContent=query+' · solicitação #'+result.solicitacao+' · '+result.duracao_ms+' ms\n'+JSON.stringify(payload,null,2);view.className='command-result success';
    }catch(error){const view=$('master-query-result');view.textContent=query+' · '+error.message;view.className='command-result error';}
    finally{state.busy=false;availability();}
  });
  $('refresh-ota-state').addEventListener('click',async()=>{
    if(state.busy)return;state.busy=true;availability();
    try{if(!await masterQuery('ota'))set('ota-state-checked','Não foi possível consultar agora. O estado exibido acima é o último recebido.');}
    finally{state.busy=false;availability();}
  });
  $('refresh-sync-state').addEventListener('click',async()=>{
    if(state.busy)return;state.busy=true;availability();
    try{if(!await masterQuery('sincronizacao'))set('sync-checked','Não foi possível consultar agora. Os dados acima são os últimos recebidos.');}
    finally{state.busy=false;availability();}
  });
  $('export-csv').addEventListener('click',()=>{const rows=['horario_local;origem;velocidade_kmh;aceleracao_ms2;potencia_w;corrente_a;tensao_v'];state.samples.filter(s=>!s.gap).forEach(s=>rows.push([new Date(s.time).toISOString(),s.simulated?'simulado':'mestre',s.speed,s.accel,s.power,s.current,s.voltage].map(v=>v===null?'':String(v)).join(';')));const url=URL.createObjectURL(new Blob([rows.join('\n')],{type:'text/csv;charset=utf-8'}));const a=document.createElement('a');a.href=url;a.download='nexus-telemetria-'+new Date().toISOString().slice(0,19).replace(/:/g,'-')+'.csv';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);});

  function setupCanvas(canvas) {
    const rect=canvas.getBoundingClientRect(),ratio=Math.min(devicePixelRatio||1,2),width=Math.max(1,Math.round(rect.width*ratio)),height=Math.max(1,Math.round(rect.height*ratio));
    if(canvas.width!==width||canvas.height!==height){canvas.width=width;canvas.height=height;}
    const ctx=canvas.getContext('2d');ctx.setTransform(ratio,0,0,ratio,0,0);ctx.clearRect(0,0,rect.width,rect.height);return {ctx,w:rect.width,h:rect.height};
  }
  const chartInfo={speed:{label:'Velocidade',unit:'km/h',color:'#8BC4DF'},accel:{label:'Aceleração',unit:'m/s²',color:'#EBC17F'},power:{label:'Potência',unit:'W',color:'#8BC4DF'},current:{label:'Corrente',unit:'A',color:'#8BC4DF'},voltage:{label:'Tensão',unit:'V',color:'#8BC4DF'}};
  function chartRange(key,values){
    if(key==='speed')return [0,30];
    if(key==='accel'){const limit=Math.max(2,Math.ceil(Math.max(0,...values.map(Math.abs))*1.15*2)/2);return [-limit,limit];}
    if(!values.length)return [0,1];
    let min=Math.min(...values),max=Math.max(...values);
    if(key==='power'||key==='rtt')min=Math.min(0,min);
    const spread=Math.max(1,(max-min)*.18);min-=spread;max+=spread;
    const step=Math.pow(10,Math.floor(Math.log10((max-min)/4)));
    return [key==='rtt'?0:Math.floor(min/step)*step,Math.ceil(max/step)*step];
  }
  function plot(canvasId,keys){
    const canvas=$(canvasId);if(!canvas||!canvas.getBoundingClientRect().width)return;
    const {ctx,w,h}=setupCanvas(canvas),left=49,right=w-14,bottom=h-26,start=Date.now()-state.range*1000,end=start+state.range*1000;
    const samples=state.samples.filter(s=>s.time>=start&&s.time<=end);
    const x=time=>left+(time-start)/(end-start)*(right-left);
    const panels=keys.length===2?[{key:'speed',top:12,bottom:Math.round(h*.55)},{key:'accel',top:Math.round(h*.64),bottom:bottom}]:[{key:keys[0],top:12,bottom}];
    ctx.font='10px Inter, Segoe UI, sans-serif';ctx.lineWidth=1;
    for(let i=0;i<=2;i++){
      const gx=left+(right-left)*i/2;
      ctx.strokeStyle='#344B5C';ctx.beginPath();ctx.moveTo(gx,12);ctx.lineTo(gx,bottom);ctx.stroke();
      ctx.fillStyle='#BACBD5';ctx.textAlign=i===0?'left':i===2?'right':'center';
      ctx.fillText(new Date(start+(end-start)*i/2).toLocaleTimeString('pt-BR',{hour:'2-digit',minute:'2-digit',second:'2-digit'}),gx,h-7);
    }
    panels.forEach(panel=>{
      const info=chartInfo[panel.key],values=samples.filter(s=>!s.gap&&Number.isFinite(s[panel.key])).map(s=>s[panel.key]);
      const [min,max]=chartRange(panel.key,values),y=v=>Math.max(panel.top,Math.min(panel.bottom,panel.bottom-(v-min)/(max-min)*(panel.bottom-panel.top)));
      for(let i=0;i<=2;i++){const gy=panel.top+(panel.bottom-panel.top)*i/2;ctx.strokeStyle='#344B5C';ctx.beginPath();ctx.moveTo(left,gy);ctx.lineTo(right,gy);ctx.stroke();ctx.fillStyle='#BACBD5';ctx.textAlign='right';ctx.fillText(number(max-(max-min)*i/2,Math.abs(max-min)<10?1:0),left-7,gy+3);}
      if(min<0&&max>0){ctx.strokeStyle='#52637A';ctx.setLineDash([4,4]);ctx.beginPath();ctx.moveTo(left,y(0));ctx.lineTo(right,y(0));ctx.stroke();ctx.setLineDash([]);}
      ctx.fillStyle=info.color;ctx.textAlign='left';ctx.font='600 10px Inter, Segoe UI, sans-serif';ctx.fillText(info.label+' · '+info.unit,left+7,panel.top+12);ctx.font='10px Inter, Segoe UI, sans-serif';
      ctx.strokeStyle=info.color;ctx.lineWidth=2.2;ctx.lineJoin='round';ctx.beginPath();let open=false,last=null;
      samples.forEach(s=>{const v=s[panel.key];if(s.gap||!Number.isFinite(v)){open=false;return;}const px=x(s.time),py=y(v);if(open)ctx.lineTo(px,py);else ctx.moveTo(px,py);open=true;last={x:px,y:py};});ctx.stroke();
      if(last){ctx.fillStyle=info.color;ctx.beginPath();ctx.arc(last.x,last.y,3.5,0,Math.PI*2);ctx.fill();}
    });
    ctx.setLineDash([3,4]);ctx.strokeStyle='#A98264';ctx.lineWidth=1;state.events.filter(e=>e.time>=start&&e.time<=end).forEach(e=>{const px=x(e.time);ctx.beginPath();ctx.moveTo(px,12);ctx.lineTo(px,bottom);ctx.stroke();});ctx.setLineDash([]);
    const hover=state.hover?.[canvasId];if(hover&&hover.time>=start){const px=x(hover.time);ctx.strokeStyle='#9BBCE8';ctx.setLineDash([3,3]);ctx.beginPath();ctx.moveTo(px,12);ctx.lineTo(px,bottom);ctx.stroke();ctx.setLineDash([]);}
    if(!samples.some(s=>keys.some(key=>Number.isFinite(s[key])))){ctx.fillStyle='#BACBD5';ctx.textAlign='center';ctx.font='12px Inter, Segoe UI, sans-serif';ctx.fillText(canvasId==='electric-chart'&&!state.telemetry?.simulado?'Sem leitura elétrica no firmware real':'Aguardando amostras disponíveis',w/2,h/2);}
  }
  function drawCharts(){plot('motion-chart',['speed','accel']);plot('electric-chart',[state.signal]);set('electric-caption',chartInfo[state.signal].label+' em '+chartInfo[state.signal].unit);}
  function drawDeviceChart(group,id){
    const canvas=$('device-chart-'+group);if(!canvas||state.tab!==group||state.selected[group]!==id||!canvas.getBoundingClientRect().width)return;
    const option=deviceChartOptions[id].find(item=>item.key===state.deviceSignal[id]);
    const key=option.key,history=state.deviceSamples[id].filter(sample=>Object.prototype.hasOwnProperty.call(sample,key));
    const values=history.map(sample=>sample[key]).filter(Number.isFinite);
    const {ctx,w,h}=setupCanvas(canvas),left=44,right=w-12,top=12,bottom=h-24;
    const [min,max]=['cpu','core0','core1','availability'].includes(key)?[0,100]:chartRange(key,values);
    ctx.font='10px Inter, Segoe UI, sans-serif';ctx.textAlign='right';
    for(let i=0;i<=2;i++){const y=top+(bottom-top)*i/2;ctx.strokeStyle='#344B5C';ctx.beginPath();ctx.moveTo(left,y);ctx.lineTo(right,y);ctx.stroke();ctx.fillStyle='#BACBD5';ctx.fillText(number(max-(max-min)*i/2,0),left-7,y+3);}
    if(!values.length){ctx.fillStyle='#BACBD5';ctx.textAlign='center';ctx.fillText('Aguardando '+option.label.toLowerCase(),w/2,h/2);return;}
    const start=history[0].time,end=Math.max(start+1,history[history.length-1].time),x=t=>left+(t-start)/(end-start)*(right-left),y=v=>bottom-(v-min)/(max-min)*(bottom-top);
    ctx.save();ctx.beginPath();ctx.rect(left,top,right-left,bottom-top);ctx.clip();
    ctx.strokeStyle=option.color;ctx.lineWidth=2.2;ctx.lineJoin='round';ctx.beginPath();let open=false,last=null;
    history.forEach(s=>{const reading=s[key];if(!Number.isFinite(reading)){open=false;return;}const px=x(s.time),py=y(reading);if(open)ctx.lineTo(px,py);else ctx.moveTo(px,py);open=true;last={x:px,y:py};});ctx.stroke();
    if(last){ctx.fillStyle=option.color;ctx.beginPath();ctx.arc(last.x,last.y,3.5,0,Math.PI*2);ctx.fill();}
    ctx.restore();
    ctx.fillStyle='#BACBD5';ctx.textAlign='left';ctx.fillText(new Date(start).toLocaleTimeString('pt-BR'),left,h-6);ctx.textAlign='right';ctx.fillText('agora',right,h-6);
  }
  function chartTooltip(canvasId,tipId,keys){
    const canvas=$(canvasId),tip=$(tipId);
    canvas.addEventListener('mousemove',e=>{
      const rect=canvas.getBoundingClientRect(),px=e.clientX-rect.left,start=Date.now()-state.range*1000,ts=start+(px-49)/Math.max(1,rect.width-63)*state.range*1000;
      const list=state.samples.filter(s=>!s.gap&&Math.abs(s.time-ts)<state.range*1000/12);
      if(!list.length){tip.hidden=true;state.hover={};drawCharts();return;}
      const sample=list.reduce((a,b)=>Math.abs(a.time-ts)<Math.abs(b.time-ts)?a:b),chosen=canvasId==='electric-chart'?[state.signal]:keys;
      tip.textContent=new Date(sample.time).toLocaleTimeString('pt-BR')+' · '+chosen.map(k=>chartInfo[k].label+': '+(Number.isFinite(sample[k])?number(sample[k],k==='speed'?1:2)+' '+chartInfo[k].unit:'Não disponível')).join(' · ');
      tip.hidden=false;tip.style.left=Math.min(Math.max(8,px+12),Math.max(8,rect.width-tip.offsetWidth-8))+'px';tip.style.top='18px';
      state.hover={[canvasId]:sample};drawCharts();
    });
    canvas.addEventListener('mouseleave',()=>{tip.hidden=true;state.hover={};drawCharts();});
  }
  chartTooltip('motion-chart','motion-tip',['speed','accel']);chartTooltip('electric-chart','electric-tip',['power','current','voltage']);
  addEventListener('resize',()=>{drawCharts();if(state.tab!=='telemetria')drawDeviceChart(state.tab,state.selected[state.tab]);});
  async function pollCommandStates(force=false){
    if(document.hidden||state.busy||state.polls.commandCycle||state.masterChecked&&!state.masterOnline)return;
    state.polls.commandCycle=true;
    try{
      const now=Date.now();
      if(force||!state.lastCommandPoll||now-state.lastCommandPoll>=state.commandRefreshMs){
        if(!await masterQuery('ota'))set('ota-state-checked','Consulta automática sem resposta. O estado acima pode estar desatualizado.');
        if(state.busy)return;
        const capabilities=state.master.versao?.capacidades;
        if(Number.isFinite(capabilities)&&(capabilities&CAP_SYNC)!==0){
          if(!await masterQuery('sincronizacao'))set('sync-checked','Consulta automática sem resposta. O estado acima pode estar desatualizado.');
        }else if(Number.isFinite(capabilities)){
          badge('sync-badge','Não suportado','muted');
          set('sync-summary','O firmware atual do mestre não informa o andamento da sincronização. Atualize o mestre para habilitar esse acompanhamento.');
        }
        state.lastCommandPoll=Date.now();
      }
      if(state.busy)return;
      const run=state.commandRuns[0],activeAux=run&&Date.now()<run.followUntil&&run.refreshQueries?.some(query=>query==='wifi'||query==='configuracao');
      const auxInterval=activeAux?state.commandRefreshMs:30000;
      if(force||!state.lastAuxPoll||Date.now()-state.lastAuxPoll>=auxInterval){
        await masterQuery('wifi');
        if(state.busy)return;
        const config=await masterQuery('configuracao');
        if(config&&(!state.lastNetworksPoll||Date.now()-state.lastNetworksPoll>=60000)){
          await loadSavedNetworks(config.quantidade_redes_wifi);
          state.lastNetworksPoll=Date.now();
        }
        state.lastAuxPoll=Date.now();
      }
    }finally{state.polls.commandCycle=false;}
  }
  async function pollRemote(){
    if(state.polls.remoteCycle || state.busy)return;
    state.polls.remoteCycle=true;
    try{
      if(!await masterQuery('ping')){
        set('ota-state-checked','Mestre sem resposta. O estado acima é a última leitura recebida.');
        set('sync-checked','Mestre sem resposta. O estado acima é a última leitura recebida.');
        return;
      }
      if(state.busy)return;
      await masterQuery('diagnostico_mestre');
      if(state.busy)return;
      await visitorsQuery('ping');
      const now=Date.now();
      if(!state.lastVersionPoll || now-state.lastVersionPoll>(state.master.versao?60000:15000)){
        await masterQuery('versao');
        await visitorsQuery('versao');
        state.lastVersionPoll=Date.now();
      }
    }finally{state.polls.remoteCycle=false;}
  }
  makeGroup('equipe');makeGroup('visitantes');showTab(location.hash.slice(1));renderEvents();renderMasterCommandHistory();availability();updateStatus();
  pollTelemetry();pollLocal();pollRemote();pollCommandStates();
  setInterval(pollTelemetry,1000);setInterval(pollLocal,3000);setInterval(pollRemote,10000);
  setInterval(pollCommandStates,1000);
  setInterval(()=>{updateStatus();if(state.tab==='telemetria')drawCharts();},1000);
  document.addEventListener('visibilitychange',()=>{if(!document.hidden){state.lastCommandPoll=0;state.lastAuxPoll=0;pollTelemetry();pollLocal();pollRemote();pollCommandStates();}});
})();
