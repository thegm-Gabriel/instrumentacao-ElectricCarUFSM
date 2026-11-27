(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const number = (value, digits = 1) => Number.isFinite(value) ? value.toLocaleString('pt-BR', {minimumFractionDigits: digits, maximumFractionDigits: digits}) : 'Não disponível';
  const value = (data, key, divisor = 1) => data && Number.isFinite(data[key]) ? data[key] / divisor : null;
  const format = (n, unit, digits = 1) => n === null ? 'Não disponível' : number(n, digits) + ' ' + unit;
  const age = ms => ms === null ? 'Não disponível' : ms < 1000 ? ms + ' ms' : ms < 60000 ? number(ms / 1000, 1) + ' s' : number(ms / 60000, 1) + ' min';
  const bytes = n => n === null ? 'Não disponível' : n >= 1048576 ? number(n / 1048576, 1) + ' MiB' : number(n / 1024, 1) + ' KiB';
  const set = (id, text) => { const el = $(id); if (el && el.textContent !== text) el.textContent = text; };
  const badge = (id, text, name) => { const el = $(id); if (el) { el.textContent = text; el.className = 'badge ' + name; } };
  const messageTypes = {
    info: {label:'Aviso', icon:'i', duration:5000},
    success: {label:'Concluído', icon:'✓', duration:6500},
    warning: {label:'Atenção', icon:'!', duration:8000},
    error: {label:'Erro', icon:'×', duration:10000},
    emergency: {label:'Parada remota', icon:'!', duration:12000}
  };
  function message(text, kind = 'info', onDetails) {
    if(!Object.hasOwn(messageTypes,kind))kind='info';
    const type=messageTypes[kind];
    let host=$('global-toast-stack');
    if(!host){
      host=document.createElement('div');
      host.id='global-toast-stack';
      host.className='global-toast-stack';
      document.body.append(host);
    }
    for(const previous of [...host.children]){
      if(previous.dataset.message===text||(kind!=='info'&&previous.dataset.tone==='info')){
        previous.remove();
      }
    }
    const toast=document.createElement('div');
    const symbol=document.createElement('span');
    const body=document.createElement('span');
    const label=document.createElement('strong');
    const close=document.createElement('button');
    toast.className='global-toast message-state '+kind;
    toast.dataset.tone=kind;
    toast.dataset.message=text;
    toast.setAttribute('role',kind==='error'||kind==='emergency'?'alert':'status');
    symbol.className='global-toast-symbol';
    symbol.textContent=type.icon;
    symbol.setAttribute('aria-hidden','true');
    body.className='global-toast-text';
    label.className='message-label';
    label.textContent=type.label;
    body.append(label,document.createTextNode(' '+text));
    close.type='button';
    close.className='global-toast-close';
    close.textContent='×';
    close.setAttribute('aria-label','Fechar mensagem');
    toast.append(symbol,body,close);
    host.prepend(toast);
    while(host.children.length>2)host.lastElementChild.remove();
    let timer;
    const dismiss=()=>{
      clearTimeout(timer);
      toast.classList.add('is-leaving');
      setTimeout(()=>toast.remove(),220);
    };
    const schedule=()=>{
      clearTimeout(timer);
      timer=setTimeout(dismiss,type.duration);
    };
    if(onDetails){
      const details=document.createElement('button');
      details.type='button';
      details.className='global-toast-details';
      details.textContent='Ver diagnóstico';
      details.addEventListener('click',()=>{dismiss();onDetails();});
      body.append(' ',details);
    }
    close.addEventListener('click',dismiss);
    toast.addEventListener('mouseenter',()=>clearTimeout(timer));
    toast.addEventListener('mouseleave',schedule);
    toast.addEventListener('focusin',()=>clearTimeout(timer));
    toast.addEventListener('focusout',schedule);
    schedule();
    return toast;
  }
  function controlNotice(text,kind='info'){
    const notice=$('remote-state');
    notice.dataset.tone=kind;
    set('remote-state',text);
  }
  const state = { tab: 'telemetria', range: 30, signal: 'power', signalSecondary: 'current', signalTertiary: 'none', telemetry: null, local: null, localAt: 0, master: {}, visitors: {}, browserOnline: false, localChecked: false, lastHttp: 0, masterOnline: false, masterChecked: false, visitorsOnline: false, visitorsChecked: false, lastSample: 0, lastSampleAgeMs: 0, lastSequence: null, samples: [], deviceSamples: {mestre:[],equipe:[],visitantes:[]}, deviceSignal:{mestre:'cpu',equipe:'cpu',visitantes:'rtt'}, deviceSignalSecondary:{mestre:'heap',equipe:'heap',visitantes:'availability'}, events: [], commandEvents: [], commandRuns: [], selected: {mestre:'mestre',equipe:'equipe', visitantes:'visitantes'}, busy: false, energyWh: 0, distanceStart: null, pending: null, polls: {}, telemetryRefreshMs: 250, commandRefreshMs: 2000 };
  const devices = {
    mestre: {group:'mestre', name:'ESP32 mestre', id:'MESTRE', role:'Mestre', function:'Aquisição e telemetria do veículo'},
    equipe: {group:'equipe', name:'ESP32 da equipe', id:'EQUIPE', role:'Satélite / gateway', function:'Recepção UART, Wi-Fi e painel web'},
    visitantes: {group:'visitantes', name:'ESP32 de visitantes', id:'VISITANTES', role:'Satélite', function:'Painel e recepção de telemetria'}
  };
  const deviceChartOptions={
    mestre:[{key:'cpu',label:'CPU total',unit:'%',tone:'primary'},{key:'core0',label:'CPU núcleo 0',unit:'%',tone:'primary'},{key:'core1',label:'CPU núcleo 1',unit:'%',tone:'primary'},{key:'heap',label:'Memória interna livre',unit:'KiB',tone:'memory'},{key:'temp',label:'Temperatura interna',unit:'°C',tone:'secondary'},{key:'rtt',label:'Tempo de resposta',unit:'ms',tone:'failure'}],
    equipe:[{key:'cpu',label:'CPU total',unit:'%',tone:'primary'},{key:'core0',label:'CPU núcleo 0',unit:'%',tone:'primary'},{key:'heap',label:'Memória interna livre',unit:'KiB',tone:'memory'},{key:'temp',label:'Temperatura interna',unit:'°C',tone:'secondary'},{key:'rate',label:'Taxa de telemetria',unit:'Hz',tone:'secondary'}],
    visitantes:[{key:'rtt',label:'Tempo de resposta',unit:'ms',tone:'primary'},{key:'availability',label:'Disponibilidade',unit:'%',tone:'memory'},{key:'cpu',label:'CPU total',unit:'%',tone:'secondary'},{key:'core0',label:'CPU núcleo 0',unit:'%',tone:'primary'},{key:'core1',label:'CPU núcleo 1',unit:'%',tone:'primary'},{key:'heap',label:'Memória interna livre',unit:'KiB',tone:'memory'},{key:'temp',label:'Temperatura interna',unit:'°C',tone:'failure'}]
  };
  function selectedDeviceCharts(id){
    const charts=[state.deviceSignal[id],state.deviceSignalSecondary[id]].map(key=>deviceChartOptions[id].find(option=>option.key===key));
    if(charts[0].tone===charts[1].tone){
      charts[1]={...charts[1],tone:charts[0].tone==='secondary'?'primary':'secondary'};
    }
    return charts;
  }
  const CAP_SYNC = 1 << 11;
  const CAP_HEALTH = 1 << 2;
  const CAP_SIMULATOR = 1 << 12;
  const CAP_CONTROL = 1 << 13;
  const CAP_SENSORS = 1 << 14;
  const CAP_TERMINAL_LOGS = 1 << 16;
  const CAP_MASTER_TASKS = 1 << 17;
  const CAP_REVERSE_PERMISSION = 1 << 18;
  const CAP_ISOLATED_DAC = 1 << 20;
  const CAP_DEVELOPER_SETTINGS = 1 << 21;
  const ARM_ISOLATED_DAC = 3;
  const refreshChoices = [1000,500,333,250];
  try {const saved=Number(localStorage.getItem('nexus-telemetry-refresh-ms'));if(refreshChoices.includes(saved))state.telemetryRefreshMs=saved;} catch (_) { /* O painel funciona sem armazenamento local. */ }
  $('command-refresh-rate').value=String(state.telemetryRefreshMs);
  $('command-refresh-rate').addEventListener('change',event=>{
    const selected=Number(event.target.value);if(!refreshChoices.includes(selected))return;
    state.telemetryRefreshMs=selected;
    clearTimeout(state.telemetryTimer);
    try {localStorage.setItem('nexus-telemetry-refresh-ms',String(selected));} catch (_) { /* Preferência válida apenas nesta sessão. */ }
    pollTelemetry();
  });
  const marchas = ['Estacionado','Neutro','Frente','Ré'];
  const navigation = $('tab-telemetria').closest('.tabs');
  const pageTabs = [...navigation.querySelectorAll('.tab')];
  const deviceTabList = $('device-tab-list');
  const deviceTabs = [...deviceTabList.querySelectorAll('.tab')];
  const mainTabs = pageTabs.filter(tab => !deviceTabList.contains(tab));
  const deviceTabsToggle = $('device-tabs-toggle');
  const compactTabLayout = matchMedia('(max-width: 760px)');

  function setDeviceMenu(open, focus = false) {
    const compact = compactTabLayout.matches;
    const expanded = compact && open;
    const active = pageTabs.find(tab => tab.dataset.tab === state.tab);
    const deviceSelected = deviceTabList.contains(active);
    const insideMenu = deviceTabList.contains(document.activeElement);
    const deviceStop = deviceSelected ? active : deviceTabs[0];
    const pageStop = compact && deviceSelected ? mainTabs[0] : active;
    deviceTabList.classList.toggle('is-open', expanded);
    deviceTabsToggle.setAttribute('aria-expanded', String(expanded));
    deviceTabsToggle.classList.toggle('is-active', compact && deviceSelected);
    pageTabs.forEach(tab => {
      tab.tabIndex = (tab === pageStop || (expanded && tab === deviceStop)) ? 0 : -1;
    });
    if (expanded && focus) deviceStop.focus({preventScroll:true});
    if (compact && !expanded && insideMenu) deviceTabsToggle.focus({preventScroll:true});
  }

  function showTab(tab, historyMode = 'replace') {
    if (!['telemetria','comandos','controle','mestre','equipe','visitantes','sobre'].includes(tab)) tab = 'telemetria';
    if (tab === 'visitantes' && state.tab !== tab) state.lastLinkPoll = 0;
    state.tab = tab;
    document.querySelectorAll('.page').forEach(el => el.classList.toggle('active', el.id === tab));
    pageTabs.forEach(el => { const active = el.dataset.tab === tab; el.classList.toggle('active', active); el.setAttribute('aria-selected', String(active)); });
    setDeviceMenu(false);
    if (location.hash !== '#' + tab) history[historyMode+'State'](null, '', '#' + tab);
    if (tab === 'telemetria') requestAnimationFrame(drawCharts);
    else if (tab === 'mestre') renderDevice('mestre','mestre');
    else if (tab === 'equipe' || tab === 'visitantes') renderGroup(tab);
  }
  pageTabs.forEach(el => el.addEventListener('click', () => showTab(el.dataset.tab, 'push')));
  deviceTabsToggle.addEventListener('click', () => setDeviceMenu(!deviceTabList.classList.contains('is-open'), true));
  compactTabLayout.addEventListener?.('change', () => setDeviceMenu(false));
  document.addEventListener('pointerdown', event => {
    if (deviceTabList.classList.contains('is-open') && !navigation.contains(event.target)) setDeviceMenu(false);
  });
  // Os cartões de acompanhamento conservam seus IDs e lógica de atualização.
  function foldCard(id,label){
    const content=$(id),panel=document.createElement('details'),heading=document.createElement('summary');
    panel.className='card fold-panel';heading.textContent=label;
    content.before(panel);panel.append(heading,content);
  }
  $('master-query').closest('details').id='master-queries';
  [['command-overview','Última ação'],['sync-card','Sincronização com Google'],['simulator-card','Telemetria simulada']].forEach(([id,label])=>foldCard(id,label));
  const localNetworks=$('wifi-local'),localHeading=localNetworks.previousElementSibling;
  foldCard('wifi-local','Redes locais · Equipe e Visitantes');
  localHeading.remove();
  const historyCard=document.querySelector('.command-history-card');historyCard.id='command-history-card';
  foldCard('command-history-card','Histórico de ações');
  function revealSection(target){
    if(target.matches('details'))target.open=true;
    for(let parent=target.parentElement;parent;parent=parent.parentElement)if(parent.matches('details'))parent.open=true;
    const destination=target.closest('.fold-panel')||target;
    destination.scrollIntoView({behavior:matchMedia('(prefers-reduced-motion: reduce)').matches?'auto':'smooth',block:'start'});
    target.setAttribute('tabindex','-1');target.focus({preventScroll:true});
  }
  document.querySelectorAll('[data-scroll-to]').forEach(button=>button.addEventListener('click',()=>{
    revealSection($(button.dataset.scrollTo));
    document.querySelectorAll('[data-scroll-to]').forEach(item=>item.classList.toggle('active',item===button));
  }));
  document.querySelectorAll('[data-open-clients]').forEach(button=>button.addEventListener('click',()=>{showTab('comandos','push');requestAnimationFrame(()=>revealSection($('wifi-clients-'+button.dataset.openClients)));}));
  navigation.addEventListener('keydown', event => {
    if (event.key === 'Escape' && deviceTabList.classList.contains('is-open')) {
      event.preventDefault();
      setDeviceMenu(false);
      return;
    }
    const inMenu = compactTabLayout.matches && deviceTabList.contains(document.activeElement);
    const tabs = compactTabLayout.matches ? (inMenu ? deviceTabs : mainTabs) : pageTabs;
    const index = tabs.indexOf(document.activeElement);
    if(index<0)return;
    const next=event.key==='ArrowRight'?tabs[(index+1)%tabs.length]:event.key==='ArrowLeft'?tabs[(index+tabs.length-1)%tabs.length]:event.key==='Home'?tabs[0]:event.key==='End'?tabs[tabs.length-1]:null;
    if(next){event.preventDefault();if(!inMenu)showTab(next.dataset.tab, 'push');next.focus();}
  });
  addEventListener('hashchange', () => showTab(location.hash.slice(1)));
  addEventListener('popstate', () => showTab(location.hash.slice(1)));
  document.querySelectorAll('.range').forEach(el => el.addEventListener('click', () => { state.range = Number(el.dataset.range); document.querySelectorAll('.range').forEach(b => b.classList.toggle('active', b === el)); drawCharts(); }));
  const electricSlots=[['electric-signal','signal'],['electric-signal-2','signalSecondary'],['electric-signal-3','signalTertiary']];
  function selectedElectricKeys(){return [state.signal,state.signalSecondary,state.signalTertiary].filter(key=>key!=='none');}
  electricSlots.forEach(([id,key])=>$(id).addEventListener('change',event=>{
    const next=event.target.value,previous=state[key],other=electricSlots.find(([otherId,otherKey])=>otherId!==id&&state[otherKey]===next);
    if(other&&previous==='none'){event.target.value=previous;return;}
    state[key]=next;
    if(other){state[other[1]]=previous;$(other[0]).value=previous;}
    drawCharts();
  }));
  const speedPeak={last:null,lastAt:0,sequence:null,source:null,candidate:0,candidateAt:0,trough:0,armed:true,cooldownUntil:0,value:null,until:0,timer:null};
  const PEAK_HOLD_MS=1400;
  const speedScale={max:30,belowSince:null};
  const telemetryMotion={items:{},period:250,arrivedAt:0,tickScale:null,slowingUntil:0};
  const CURRENT_HIGHLIGHT_A=7.5,CURRENT_VISUAL_MAX_A=35;
  const reducedMotion=()=>matchMedia('(prefers-reduced-motion: reduce)').matches;
  function animateTelemetry(id,target,digits=1,onFrame=()=>{},suffix=''){
    const old=telemetryMotion.items[id];
    if(old&&old.target===target&&old.suffix===suffix)return;
    if(old?.frame)cancelAnimationFrame(old.frame);
    if(target===null){delete telemetryMotion.items[id];set(id,'—');onFrame(null);return;}
    const start=old&&Number.isFinite(old.current)?old.current:target;
    const item={current:start,target,frame:0,suffix};telemetryMotion.items[id]=item;
    const duration=Math.max(230,Math.min(730,telemetryMotion.period*1.55));
    const began=performance.now();
    const paint=now=>{
      const progress=document.hidden||reducedMotion()?1:Math.min(1,(now-began)/duration);
      item.current=start+(target-start)*Math.sin(progress*Math.PI/2);
      set(id,number(item.current,digits)+suffix);onFrame(item.current);
      if(progress<1)item.frame=requestAnimationFrame(paint);
      else item.frame=0;
    };
    paint(began);
  }
  function renderSpeedTicks(scale){
    const group=$('speed-ticks');
    if(!group||telemetryMotion.tickScale===scale)return;
    telemetryMotion.tickScale=scale;group.replaceChildren();
    const svgNS='http://www.w3.org/2000/svg';
    for(let speed=5;speed<scale;speed+=5){
      const major=speed%10===0,angle=Math.PI*(1-speed/scale);
      const point=radius=>[160+radius*Math.cos(angle),158-radius*Math.sin(angle)];
      const inner=point(134),outer=point(major?145:141),label=point(154);
      const mark=document.createElementNS(svgNS,'line');
      mark.setAttribute('x1',inner[0].toFixed(2));mark.setAttribute('y1',inner[1].toFixed(2));mark.setAttribute('x2',outer[0].toFixed(2));mark.setAttribute('y2',outer[1].toFixed(2));
      mark.setAttribute('class',major?'speed-tick major':'speed-tick');group.append(mark);
      const text=document.createElementNS(svgNS,'text');text.setAttribute('x',label[0].toFixed(2));text.setAttribute('y',label[1].toFixed(2));text.setAttribute('class',major?'speed-tick-label major':'speed-tick-label');text.textContent=String(speed);group.append(text);
    }
  }
  function scaleForSpeed(speed,heldPeak,now){
    const required=Math.max(0,speed||0,heldPeak||0);
    if(required>speedScale.max+0.15){
      speedScale.max=Math.max(30,Math.ceil(required/5)*5);
      speedScale.belowSince=null;
    }else if(speedScale.max>30&&required<=speedScale.max-7){
      if(speedScale.belowSince===null)speedScale.belowSince=now;
      else if(now-speedScale.belowSince>=4000){
        speedScale.max=Math.max(30,Math.ceil((required+1.5)/5)*5);
        speedScale.belowSince=null;
      }
    }else speedScale.belowSince=null;
    return speedScale.max;
  }
  function trackSpeedPeak(speed,source,sequence){
    const now=performance.now();
    if(speed===null||speedPeak.last===null||now-speedPeak.lastAt>2500||speedPeak.source!==source){
      clearTimeout(speedPeak.timer);
      Object.assign(speedPeak,{last:speed,lastAt:now,sequence,source,candidate:speed||0,candidateAt:now,trough:speed||0,armed:true,cooldownUntil:0,value:null,until:0,timer:null});
      return;
    }
    if(sequence!==null&&sequence!==undefined&&sequence===speedPeak.sequence)return;
    const elapsed=now-speedPeak.lastAt,dropRate=(speedPeak.last-speed)*1000/Math.max(100,elapsed);
    if(speedPeak.armed){
      if(now-speedPeak.candidateAt>3000){speedPeak.candidate=speedPeak.last;speedPeak.candidateAt=speedPeak.lastAt;}
      if(speed>speedPeak.candidate){speedPeak.candidate=speed;speedPeak.candidateAt=now;}
      else if(speedPeak.candidate>=5&&speedPeak.candidate-speed>=1.5&&dropRate>=3.5&&now>=speedPeak.cooldownUntil){
        speedPeak.value=speedPeak.candidate;
        speedPeak.until=now+PEAK_HOLD_MS;
        speedPeak.cooldownUntil=now+4000;
        speedPeak.armed=false;
        speedPeak.trough=speed;
        clearTimeout(speedPeak.timer);
        speedPeak.timer=setTimeout(()=>{speedPeak.until=0;updateArc(false);},PEAK_HOLD_MS);
      }
    }else{
      speedPeak.trough=Math.min(speedPeak.trough,speed);
      if(now>=speedPeak.cooldownUntil&&speed-speedPeak.trough>=2){speedPeak.armed=true;speedPeak.candidate=speed;speedPeak.candidateAt=now;}
    }
    if(speedPeak.until&&speed>=speedPeak.value){speedPeak.until=0;clearTimeout(speedPeak.timer);}
    speedPeak.last=speed;speedPeak.lastAt=now;speedPeak.sequence=sequence;
  }
  function updateArc(recordSample=true) {
    const d=state.telemetry&&state.telemetry.valido?state.telemetry:null;
    const speed=d?value(d,'velocidade_centesimos_kmh',100):null;
    if(recordSample)trackSpeedPeak(speed,d?.simulado,d?.sequencia);
    const peakVisible=speedPeak.value!==null&&speedPeak.until>performance.now()&&speed!==null;
    const scale=scaleForSpeed(speed,peakVisible?speedPeak.value:null,performance.now());
    renderSpeedTicks(scale);
    const displayedSpeed=telemetryMotion.items.speed?.current??speed;
    $('speed-fill').style.strokeDasharray=(Math.max(0,Math.min(100,(displayedSpeed||0)/scale*100)))+' 100';
    set('speed-scale-max',String(scale));
    const marker=$('speed-peak-marker');marker.classList.toggle('is-visible',peakVisible);
    const peakLabel=$('speed-peak-label');
    if(peakLabel){
      if(peakVisible){clearTimeout(telemetryMotion.peakLabelTimer);peakLabel.textContent='Pico '+number(speedPeak.value,1)+' km/h';peakLabel.classList.add('is-visible');}
      else if(peakLabel.classList.contains('is-visible')){peakLabel.classList.remove('is-visible');telemetryMotion.peakLabelTimer=setTimeout(()=>{if(!peakLabel.classList.contains('is-visible'))peakLabel.textContent='';},300);}
    }
    if(peakVisible){
      const angle=Math.PI*(1-Math.min(1,speedPeak.value/scale));
      // Um risco radial curto, contido na espessura do arco, registra o pico antes da queda.
      const point=radius=>[(160+radius*Math.cos(angle)).toFixed(2),(158-radius*Math.sin(angle)).toFixed(2)];
      const from=point(117),to=point(125);
      marker.querySelectorAll('line').forEach(line=>{line.setAttribute('x1',from[0]);line.setAttribute('y1',from[1]);line.setAttribute('x2',to[0]);line.setAttribute('y2',to[1]);});
    }
    $('speed-fill').closest('svg').setAttribute('aria-label','Velocidade em escala de 0 a '+scale+' km/h'+(peakVisible?'. Velocidade antes da desaceleração: '+number(speedPeak.value,1)+' km/h.':''));
  }
  const powerPeak={last:null,candidate:0,value:null,until:0,cooldownUntil:0,timer:null,minimum:120,minimumFall:55,markerId:'power-peak-marker',labelId:'power-peak-label',unit:' W',digits:0};
  const currentPeak={last:null,candidate:0,value:null,until:0,cooldownUntil:0,timer:null,minimum:2,minimumFall:.8,markerId:'current-peak-marker',labelId:'current-peak-label',unit:' A',digits:2,highlight:CURRENT_HIGHLIGHT_A};
  const POWER_VISUAL_MAX_W=1000;
  function recordMeterPeak(peak,reading){
    const now=performance.now();
    if(reading===null){clearTimeout(peak.timer);Object.assign(peak,{last:null,candidate:0,value:null,until:0,cooldownUntil:0,timer:null});$(peak.markerId).classList.remove('is-visible');$(peak.labelId).classList.remove('is-visible');return;}
    if(peak.last===null){peak.last=reading;peak.candidate=reading;return;}
    if(Math.sign(reading)===Math.sign(peak.candidate)&&Math.abs(reading)>Math.abs(peak.candidate))peak.candidate=reading;
    const fall=Math.abs(peak.candidate)-Math.abs(reading);
    if(Math.abs(peak.candidate)>=peak.minimum&&fall>=Math.max(peak.minimumFall,Math.abs(peak.candidate)*.25)&&now>=peak.cooldownUntil){
      peak.value=peak.candidate;peak.until=now+1000;peak.cooldownUntil=now+2500;
      peak.candidate=reading;clearTimeout(peak.timer);
      const label=$(peak.labelId);label.textContent='Pico · '+number(peak.value,peak.digits)+peak.unit;
      label.classList.toggle('is-regen',peak.value<0);
      label.classList.toggle('is-elevated',!!peak.highlight&&Math.abs(peak.value)>peak.highlight);
      label.classList.add('is-visible');
      peak.timer=setTimeout(()=>{peak.until=0;$(peak.markerId).classList.remove('is-visible');label.classList.remove('is-visible');},1000);
    }
    if(Math.sign(reading)!==Math.sign(peak.candidate))peak.candidate=reading;
    peak.last=reading;
  }
  function renderPowerRail(power){
    const rail=$('power-rail');if(!rail)return;
    rail.hidden=power===null;
    const card=$('power-card'),strength=power===null?0:Math.min(1,Math.abs(power)/POWER_VISUAL_MAX_W);
    card.style.setProperty('--power-intensity',Math.round(strength*100)+'%');
    card.style.setProperty('--power-glow',(strength*.3).toFixed(3));
    if(power===null)return;
    const fraction=Math.min(1,Math.abs(power)/POWER_VISUAL_MAX_W);
    $('power-rail-fill').style.setProperty('--fill-level',(fraction*100)+'%');
    const peak=$('power-peak-marker'),visible=powerPeak.value!==null&&powerPeak.until>performance.now();
    peak.classList.toggle('is-visible',visible);
    if(visible)peak.style.left=Math.min(99,Math.max(1,Math.abs(powerPeak.value)/POWER_VISUAL_MAX_W*100))+'%';
    rail.setAttribute('aria-label','Potência elétrica: '+number(power,0)+' W; barra de 0 a '+POWER_VISUAL_MAX_W+' W'+(power<0?' em regeneração':''));
    $('power-card').classList.toggle('is-regen',power<0);
  }

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
    const elapsed = state.lastSample ? Math.max(0,Date.now()-state.lastSample)+state.lastSampleAgeMs : null;
    const stale = !state.telemetry || !state.telemetry.valido || elapsed === null || elapsed >= 2000;
    document.querySelectorAll('.speed-card,.metric-card,.quick-strip,.chart-card,.detail-card').forEach(el => el.classList.toggle('stale', stale));
    if (state.telemetry) badge('data-badge', stale ? 'Dados desatualizados' : state.telemetry.simulado ? 'Dados simulados' : 'Dados do mestre', stale || state.telemetry.simulado ? 'warn' : 'good');
    renderCriticalTelemetry(stale);
    renderHealth();
  }
  function renderCriticalTelemetry(stale){
    const d=stale?null:state.telemetry,sim=!!d?.simulado;
    const electrical=!!d&&(sim||!!(d.estado_mpu&128));
    badge('critical-freshness',!state.telemetry?'Aguardando telemetria':stale?'Leitura desatualizada':sim?'Leitura simulada':'Leitura do mestre',stale?'warn':sim?'warn':'good');
    set('critical-speed',d?format(value(d,'velocidade_centesimos_kmh',100),'',1).trim():'—');
    set('critical-soc',format(batteryCharge(d),'%',1));
    set('critical-throttle',format(value(d,'acelerador_decimos_percentual',10),'%',1));
    set('critical-brake',sim?format(value(d,'freio_decimos_percentual',10),'%',1):'Não disponível');
    set('critical-current',electrical?format(value(d,'corrente_centesimos_a',100),'A',2):'Não disponível');
    set('critical-voltage',electrical?format(value(d,'tensao_pacote_mv',1000),'V',3):'Não disponível');
    set('critical-power',electrical?format(value(d,'potencia_w'),'W',0):'Não disponível');
  }
  // A ordem das causas segue os bits de CausaSaudeSistema no mestre.
  const healthCauses = [
    {label:'Memória crítica', tab:'mestre', target:'health-diagnostics-memory'},
    {label:'Pilha de tarefa baixa', short:'Pilha baixa', tab:'comandos', target:'master-tasks'},
    {label:'Sensor', tab:'comandos', target:'sensor-command'},
    {label:'I²C', tab:'comandos', target:'sensor-bus'},
    {label:'UART', tab:'comandos', target:'master-queries', query:'uart'},
    {label:'Supervisão de tarefas', short:'Supervisão', tab:'comandos', target:'master-tasks'},
    {label:'Equipe sem resposta', short:'Equipe', tab:'equipe', target:'title-equipe'},
    {label:'Visitantes sem resposta', short:'Visitantes', tab:'visitantes', target:'title-visitantes'},
    {label:'Wi-Fi fraco', tab:'comandos', target:'wifi-master'},
    {label:'Falha OTA recente', short:'OTA', tab:'comandos', target:'ota-command'},
    {label:'Falha ao salvar o odômetro na NVS', short:'NVS do odômetro', tab:'comandos', target:'sensor-command'},
    {label:'Memória baixa', tab:'mestre', target:'health-diagnostics-memory'},
    {label:'Divergência entre encoders', short:'Encoders divergentes', tab:'comandos', target:'sensor-command'},
    {label:'Velocidade indisponível no odômetro', short:'Odômetro', tab:'comandos', target:'sensor-command'},
    {label:'DAC do acelerador indisponível', short:'DAC indisponível', tab:'comandos', target:'sensor-command'},
    {label:'Falha nas saídas de tração', short:'Tração', tab:'controle', target:'remote-state'},
    {label:'Leitura do pedal indisponível', short:'Pedal indisponível', tab:'comandos', target:'sensor-command'},
    {label:'MPU6050 ainda sem calibração', short:'MPU sem calibração', tab:'comandos', target:'sensor-command'},
  ];
  const encoderDivergenceMask = 1 << 12;
  const odometerUnavailableMask = 1 << 13;
  const healthFreshMs = 30000;

  function getSystemHealth() {
    const health = state.master.saude;
    const level = health?.estado;
    const receivedAt = state.lastHealthAt;
    const known = Number.isInteger(level) && level >= 0 && level <= 3;
    const offline = state.masterChecked && !state.masterOnline;
    const fresh = state.masterOnline && known && Date.now() - receivedAt < healthFreshMs;
    const labels = ['Normal', 'Recuperando', 'Degradada', 'Falha'];
    const tones = ['success', 'info', 'warning', 'error'];
    let label = 'Aguardando';
    if (offline) label = 'Mestre offline';
    else if (fresh) label = labels[level];
    else if (known) label = 'Desatualizada';
    else if (health) label = 'Estado desconhecido';

    const mask = value(health, 'causas_ativas');
    const causes = healthCauses.filter((_, index) => (mask & (1 << index)) !== 0);
    const unknown = (mask >>> healthCauses.length) << healthCauses.length;
    if (unknown) causes.push({label:'Outras causas (0x' + (unknown >>> 0).toString(16).toUpperCase() + ')', short:'Outras causas'});
    return {health, level, fresh, offline, label, tone: fresh ? tones[level] : offline ? 'error' : 'info', causes, mask, receivedAt};
  }

  function healthAge(receivedAt) {
    const seconds = Math.max(0, Math.floor((Date.now() - receivedAt) / 1000));
    if (seconds < 1) return 'agora';
    if (seconds < 60) return 'há ' + seconds + ' s';
    if (seconds < 3600) return 'há ' + Math.floor(seconds / 60) + ' min';
    return 'há ' + Math.floor(seconds / 3600) + ' h';
  }

  function healthCauseSummary(causes, compact = false) {
    if (!causes.length) return '';
    const first = causes[0];
    const label = compact && first.short ? first.short : first.label;
    return label + (causes.length > 1 ? ' +' + (causes.length - 1) : '');
  }

  let lastHealthAlert='', healthToast;
  function renderHealth(){
    const healthState = getSystemHealth();
    const {level,fresh,offline,causes,mask,label,tone}=healthState;
    const shortcut=$('health-shortcut');
    if(shortcut.dataset.tone!==tone)shortcut.dataset.tone=tone;
    const hasFault=fresh&&(level>=2||(level===1&&mask));
    const encoderWarning = fresh && (mask & (encoderDivergenceMask | odometerUnavailableMask));
    $('sensor-encoder-warning').hidden = !encoderWarning;
    set('sensor-encoder-warning', encoderWarning
      ? (mask & encoderDivergenceMask
          ? 'Divergência recente entre os encoders esquerdo e direito. Confira as leituras e a calibração das rodas.'
          : 'O Mestre está sem uma leitura válida de velocidade dos encoders.')
      : '');
    const causeLabel = hasFault ? healthCauseSummary(causes, true) : '';
    set('health-status', label + (causeLabel ? ' · ' + causeLabel : ''));
    shortcut.title = hasFault && causes.length
      ? 'Diagnóstico do Mestre · ' + causes.map(cause => cause.label).join(', ')
      : 'Abrir diagnóstico do Mestre';
    const alertKey=offline?'offline':hasFault?`${level}/${mask}`:'';
    if(alertKey!==lastHealthAlert){
      healthToast?.remove();
      healthToast=null;
      lastHealthAlert=alertKey;
      if(alertKey){
        const text=offline?'Mestre sem resposta':['','Mestre em recuperação','Operação degradada','Falha no Mestre'][level];
        const detail=offline?'':(mask&encoderDivergenceMask)
          ? 'Divergência entre encoders'+(causes.length>1?' · mais '+(causes.length-1)+' aviso(s)':'')
          : healthCauseSummary(causes);
        healthToast=message(text+(detail?' · '+detail:'')+'.',offline||level===3?'error':'warning',openHealthDiagnostics);
      }
    }
    renderDeviceSystemHealth(healthState);
  }
  function showUnconfirmedControl(text, detail) {
    state.controlUnconfirmed = true;
    renderControlSensors(null);
    controlNotice(text, 'warning');
    badge('actuation-status', 'Atuação não confirmada', 'muted');
    badge('vehicle-arm-badge', 'Estado do carro não confirmado', 'muted');
    const lastReading = state.master.controle_remoto ? 'A última leitura pode estar desatualizada.' : 'Nenhuma leitura confirmada.';
    set('remote-checked', detail || lastReading);
  }

  async function refreshControlState() {
    if (!await masterQuery('controle_remoto')) {
      showUnconfirmedControl('Mestre sem resposta. Estado não confirmado.');
      availability();
    }
  }

  function commandCardOpen(id) {
    return state.tab === 'comandos' && $(id)?.closest('details')?.open === true;
  }

  function liveReadingsAvailable() {
    return !document.hidden && !state.busy && !state.stopInFlight && remotePending === 0;
  }

  async function refreshDeveloperSettings() {
    if (!await masterQuery('ajustes_desenvolvedor')) {
      state.developerUnconfirmed = true;
      set('developer-status', 'Sem resposta do Mestre.');
      set('developer-limiter-state', 'Limites em uso sem confirmação.');
      availability();
    }
  }

  function developerQueryAvailable(capabilities) {
    return !!state.master.ajustes_desenvolvedor || !Number.isFinite(capabilities) || (capabilities & CAP_DEVELOPER_SETTINGS) !== 0;
  }

  function visibleReadings(tab) {
    const queries = [];
    const capabilities = state.master.versao?.capacidades;
    if (state.masterOnline) {
      if ((tab === 'controle' || tab === 'telemetria' || commandCardOpen('sensor-command')) &&
          (!Number.isFinite(capabilities) || (capabilities & CAP_CONTROL) !== 0 || state.master.controle_remoto)) {
        queries.push(refreshControlState);
      }
      if (!Number.isFinite(capabilities) || (capabilities & CAP_HEALTH) !== 0) queries.push(() => masterQuery('saude'));
      if (tab === 'mestre') queries.push(() => masterQuery('diagnostico_mestre'));
      if (commandCardOpen('sensor-command') && (capabilities & CAP_SENSORS) !== 0) {
        queries.push(async () => {
          if (!await masterQuery('sensores')) set('sensor-status', 'Sem resposta · dados anteriores.');
        });
      }
      if (commandCardOpen('developer-command')) {
        if (developerDevice === 'mestre' && developerQueryAvailable(capabilities)) queries.push(refreshDeveloperSettings);
        else if (developerDevice !== 'mestre') {
          const target=developerDevice;
          queries.push(() => refreshRadioSettings(target));
        }
      }
      if (commandCardOpen('ota-command') || (tab === 'controle' && state.master.controle_remoto?.ota_em_andamento))
        queries.push(async ()=>{
          if(!await masterQuery('ota'))set('ota-state-checked','Sem resposta. O estado exibido é o último recebido.');
        });
      if (commandCardOpen('sync-card') && (capabilities & CAP_SYNC) !== 0) queries.push(() => masterQuery('sincronizacao'));
      if (commandCardOpen('simulator-card') && (capabilities & CAP_SIMULATOR) !== 0) queries.push(() => masterQuery('simulador'));
      if (commandCardOpen('wifi-master')) queries.push(() => masterQuery('wifi'));
    }
    if (tab === 'equipe') queries.push(pollLocal);
    if (tab === 'visitantes' && state.visitorsOnline) queries.push(() => visitorsQuery('diagnostico'));
    return queries;
  }

  async function pollVisibleReadings() {
    if (state.polls.liveCycle || !liveReadingsAvailable()) return;
    const tab = state.tab;
    const queries = visibleReadings(tab);
    if (!queries.length) return;
    state.polls.liveCycle = true;
    const started = performance.now(), first = (state.nextVisibleReading || 0) % queries.length;
    try {
      // Retoma da próxima leitura se a janela acabar antes de concluir o ciclo.
      for (let count = 0; count < queries.length && state.tab === tab && liveReadingsAvailable(); count++) {
        if (count > 0 && performance.now()-started >= state.telemetryRefreshMs) break;
        const index = (first+count) % queries.length;
        state.nextVisibleReading = index+1;
        await queries[index]();
      }
    } finally {
      state.polls.liveCycle = false;
    }
  }

  $('refresh-remote-control').addEventListener('click', refreshControlState);
  function openHealthDiagnostics() {
    showTab('mestre', 'push');
    const diagnostics = $('health-diagnostics');
    diagnostics.open = true;
    renderMasterHealthDiagnostics(getSystemHealth());
    diagnostics.querySelector('summary').focus({preventScroll: true});
    diagnostics.scrollIntoView({block: 'nearest'});
  }
  $('health-shortcut').addEventListener('click',openHealthDiagnostics);
  function openHealthCause(cause) {
    if (state.tab !== cause.tab) showTab(cause.tab, 'push');
    if (cause.query) $('master-query').value = cause.query;
    requestAnimationFrame(() => {
      const target = $(cause.target);
      const destination = target && !target.closest('[hidden]') ? target : $('health-diagnostics');
      if (destination) revealSection(destination);
      if (cause.query) $('run-master-query').click();
    });
  }
  async function getJSON(url, timeout = 7000) {
    const controller = new AbortController(); const timer = setTimeout(() => controller.abort(), timeout);
    try { const response = await fetch(url, {cache:'no-store', signal:controller.signal}); state.lastHttp = Date.now(); state.browserOnline = true; const body = await response.json(); if (!response.ok || body.ok === false) throw new Error(body.erro || 'Resposta ' + response.status); return body; }
    finally { clearTimeout(timer); }
  }
  let remoteQueue = Promise.resolve();
  let remotePending = 0;
  function remote(task) {
    remotePending++;
    const next = remoteQueue.then(task).finally(() => { remotePending--; });
    remoteQueue = next.catch(() => {});
    return next;
  }
  function appendSample(d) {
    const now = Date.now();
    if (state.lastSequence === d.sequencia) return;
    if(telemetryMotion.arrivedAt){const elapsed=now-telemetryMotion.arrivedAt;if(elapsed>=120&&elapsed<=2500)telemetryMotion.period=Math.round(telemetryMotion.period*.7+elapsed*.3);}
    telemetryMotion.arrivedAt=now;
    if (state.samples.length && now - state.samples[state.samples.length - 1].time > 3000) state.samples.push({time:now - 1, gap:true});
    const electrical=d.simulado || !!(d.estado_mpu & 128);
    const ina=electrical&&!d.simulado&&!!(d.estado_ina&1);
    const sample = {time:now, simulated:d.simulado, speed:value(d,'velocidade_centesimos_kmh',100), accel:d.simulado || (d.estado_mpu & 1) ? value(d,'aceleracao_milesimos_ms2',1000) : null, power:electrical ? value(d,'potencia_w') : null, current:electrical ? value(d,'corrente_centesimos_a',100) : null, voltage:electrical ? value(d,'tensao_pacote_mv',1000) : null,
      soc:batteryCharge(d),autonomy:d.simulado?value(d,'autonomia_decimos_km',10):null,
      shunt:ina?value(d,'ina_shunt_microvolts',1000):null,inaTemp:ina?value(d,'ina_temperatura_centesimos_c',100):null,
      inaEnergy:ina&&(d.estado_ina&2)?value(d,'ina_energia_mwh',1000):null,inaCharge:ina&&(d.estado_ina&4)?value(d,'ina_carga_mah',1000):null};
    const previousMotion=state.samples.length?state.samples[state.samples.length-1]:null;
    if(previousMotion&&!previousMotion.gap&&Number.isFinite(previousMotion.speed)&&Number.isFinite(sample.speed)){
      const drop=previousMotion.speed-sample.speed;
      if(drop>=.25&&drop/Math.max(.12,(now-previousMotion.time)/1000)>=.9)telemetryMotion.slowingUntil=now+850;
      else if(sample.speed>previousMotion.speed+.2)telemetryMotion.slowingUntil=0;
    }
    clearTimeout(telemetryMotion.slowingTimer);
    $('speed-card').classList.toggle('is-slowing',telemetryMotion.slowingUntil>now);
    if(telemetryMotion.slowingUntil>now)telemetryMotion.slowingTimer=setTimeout(()=>$('speed-card').classList.remove('is-slowing'),telemetryMotion.slowingUntil-now);
    recordMeterPeak(powerPeak,sample.power);
    recordMeterPeak(currentPeak,sample.current);
    if (state.samples.length && !state.samples[state.samples.length-1].gap) {
      const previous = state.samples[state.samples.length-1];
      const hours = Math.min((now - previous.time) / 3600000, 2 / 3600);
      if (previous.power !== null && sample.power !== null) state.energyWh += (previous.power + sample.power) / 2 * hours;
    }
    state.samples.push(sample); state.samples = state.samples.filter(s => now - s.time <= 610000);
    state.lastSequence = d.sequencia; state.lastSample = now; state.lastSampleAgeMs = Number.isFinite(d.idade_ms) ? Math.max(0,d.idade_ms) : 0;
    if (state.distanceStart === null && Number.isFinite(d.percurso_metros)) state.distanceStart = d.percurso_metros;
    $('export-csv').disabled = false;
    drawCharts();
  }
  function deviceSample(id, measurements) { const history=state.deviceSamples[id]; history.push({time:Date.now(),...measurements}); if(history.length>180)history.shift(); if(state.tab===devices[id].group) drawDeviceChart(devices[id].group,id); }
  const batteryWarnings = [
    { below: 10, level: 'critical', label: 'Crítico', icon: '☠️' },
    { below: 30, level: 'low', label: 'Bateria baixa', icon: '🪫' },
    { below: 50, level: 'attention', label: 'Atenção', icon: '⚠️' }
  ];
  function batteryCharge(data) {
    return data && (data.simulado || (data.estado_ina & 8))
      ? value(data,'carga_decimos_percentual',10) : null;
  }
  function renderBatteryCharge(charge) {
    const warning = charge === null ? null : batteryWarnings.find(item => charge < item.below);
    $('soc-card').dataset.chargeLevel = charge === null ? 'unavailable' : warning?.level ?? 'normal';
    const notice = $('soc-notice');
    set('soc-notice-label', warning?.label ?? '');
    set('soc-notice-icon', warning?.icon ?? '');
    notice.hidden = !warning;
    animateTelemetry('soc', charge, 2, reading => {
      $('soc-bar').style.width = Math.max(0, Math.min(100, reading ?? 0)) + '%';
    });
  }
  function renderTelemetry(d) {
    const available = !!(d && d.valido);
    if (!available) return;
    const electrical=d.simulado || !!(d.estado_mpu & 128);
    const speed = value(d,'velocidade_centesimos_kmh',100), accel = d.simulado || (d.estado_mpu & 1) ? value(d,'aceleracao_milesimos_ms2',1000) : null, soc = batteryCharge(d), power = electrical ? value(d,'potencia_w') : null, current = electrical ? value(d,'corrente_centesimos_a',100) : null;
    animateTelemetry('speed',speed,1,()=>updateArc(false));
    animateTelemetry('mpu-accel',accel,2,reading=>{$('mpu-accel-row').classList.toggle('is-negative',reading!==null&&reading<-.04);},' m/s²');
    renderBatteryCharge(soc);
    const powerKw=power!==null&&Math.abs(power)>=1000;
    if(telemetryMotion.powerUnit!==undefined&&telemetryMotion.powerUnit!==powerKw){const oldPower=telemetryMotion.items.power;if(oldPower?.frame)cancelAnimationFrame(oldPower.frame);delete telemetryMotion.items.power;}
    telemetryMotion.powerUnit=powerKw;
    set('power-unit',powerKw?'kW':'W');
    animateTelemetry('power',powerKw?power/1000:power,powerKw?2:0,current=>renderPowerRail(current===null?null:powerKw?current*1000:current));
    if(power===null)$('power-card').classList.remove('is-regen');
    animateTelemetry('current-primary',current,2,reading=>{
      const magnitude=Math.abs(reading||0),card=$('current-card');
      const intensity=Math.min(1,Math.max(0,(magnitude-CURRENT_HIGHLIGHT_A)/(CURRENT_VISUAL_MAX_A-CURRENT_HIGHLIGHT_A)));
      card.classList.toggle('is-elevated',reading!==null&&magnitude>CURRENT_HIGHLIGHT_A);
      card.classList.toggle('is-regen',reading!==null&&reading<-.5&&magnitude<=CURRENT_HIGHLIGHT_A);
      card.style.setProperty('--current-red',Math.round(intensity*100)+'%');
      card.style.setProperty('--current-heat',(intensity*.34).toFixed(3));
      $('current-meter-fill').style.setProperty('--fill-level',Math.min(100,magnitude/CURRENT_VISUAL_MAX_A*100)+'%');
      const peak=$('current-peak-marker'),visible=currentPeak.value!==null&&currentPeak.until>performance.now()&&reading!==null;
      peak.classList.toggle('is-visible',visible);
      if(visible)peak.style.left=Math.min(99,Math.max(1,Math.abs(currentPeak.value)/CURRENT_VISUAL_MAX_A*100))+'%';
    },' A');
    set('gear', d.simulado ? marchas[d.marcha] || 'Não disponível' : 'Não disponível'); set('distance', format(value(d,'percurso_metros',1000),'km',2)); set('master-time', age(value(d,'tempo_mestre_ms'))); set('source', d.simulado ? 'Dados simulados' : 'Mestre');
    set('voltage', format(electrical ? value(d,'tensao_pacote_mv',1000) : null,'V',3)); set('current', format(current,'A',2)); set('power-detail',format(power,'W',2)); set('soc-detail', format(soc,'%',2));
    set('range-estimate',format(d.simulado?value(d,'autonomia_decimos_km',10):null,'km',2));
    const distanceKm = state.distanceStart === null ? null : (d.percurso_metros - state.distanceStart)/1000;
    set('consumption', electrical && distanceKm !== null && distanceKm >= 0.1 && state.energyWh >= 0 ? format(state.energyWh/distanceKm,'Wh/km',2) : 'Não disponível');
    const ina=electrical&&!d.simulado&&!!(d.estado_ina&1);
    badge('ina-badge',ina?'Leitura do INA228':d.simulado?'Simulação':'Sem leitura',ina?'good':d.simulado?'warn':'muted');
    set('ina-shunt',ina?format(value(d,'ina_shunt_microvolts',1000),'mV',3):'Não disponível');
    set('ina-power',ina?format(power,'W',2):'Não disponível');
    set('ina-temp',ina?format(value(d,'ina_temperatura_centesimos_c',100),'°C',2):'Não disponível');
    set('ina-energy',ina&&(d.estado_ina&2)?format(value(d,'ina_energia_mwh',1000),'Wh',2):'Não disponível');
    set('ina-charge',ina&&(d.estado_ina&4)?format(value(d,'ina_carga_mah',1000),'Ah',3):'Não disponível');
    const throttle = value(d,'acelerador_decimos_percentual',10), brake = d.simulado ? value(d,'freio_decimos_percentual',10) : null;
    animateTelemetry('throttle-arc-value',throttle,0,current=>{$('throttle-fill').style.setProperty('--fill-level',Math.max(0,Math.min(100,current||0))+'%');},'%');
    animateTelemetry('brake-arc-value',brake,0,current=>{$('brake-fill').style.setProperty('--fill-level',Math.max(0,Math.min(100,current||0))+'%');},'%');
    set('throttle', format(throttle,'%',1)); set('brake',format(brake,'%',1)); $('throttle-bar').style.width = Math.max(0,Math.min(100,throttle || 0))+'%'; $('brake-bar').style.width = Math.max(0,Math.min(100,brake || 0))+'%';
    const mpu = d.estado_mpu || 0; set('mpu', d.simulado ? 'Dados simulados' : mpu & 1 ? (mpu & 2 ? 'Ativo · calibrado' : 'Ativo · sem calibração') : 'Inativo'); set('mpu-temp', mpu & 1 ? format(value(d,'temperatura_mpu_centesimos_c',100),'°C',1) : 'Não disponível');
    set('packet-rate',format(value(d,'taxa_pacotes_centesimos_hz',100),'Hz',2)); set('sample-age',age(value(d,'idade_ms'))); set('packet-loss',format(value(d,'taxa_perda_centesimos_percentual',100),'%',2)); set('crc-fails',Number.isFinite(d.falhas_crc) ? String(d.falhas_crc) : 'Não disponível');
    badge('link-badge', d.idade_ms < 2000 ? 'Atual' : 'Desatualizado', d.idade_ms < 2000 ? 'good' : 'warn');
    updateArc();
  }
  async function pollTelemetry() {
    if (document.hidden || state.polls.telemetry) return;
    state.polls.telemetry = true;
    const started = performance.now();
    let failed = false;
    try {
      void pollRemote();
      void pollCommandStates();
      void pollVisibleReadings();
      const d = await getJSON('/api/telemetria'); state.browserOnline = true; state.telemetry = d;
      if (d.valido && d.idade_ms < 2000) { const first = state.lastSequence === null; appendSample(d); if (first) event(d.simulado ? 'Telemetria simulada recebida.' : 'Telemetria recebida do mestre.'); renderTelemetry(d); observeOdometerRun(d); }
      else if (state.lastSequence !== null && (!state.samples.length || !state.samples[state.samples.length-1].gap)) { state.samples.push({time:Date.now(),gap:true}); event('Telemetria desatualizada.','warn'); drawCharts(); }
    } catch (error) { failed = true; state.browserOnline = Date.now() - state.lastHttp < 5000; if (state.lastSequence !== null && (!state.samples.length || !state.samples[state.samples.length-1].gap)) {state.samples.push({time:Date.now(),gap:true}); event('Conexão com a equipe interrompida.','warn'); drawCharts();} }
    finally {
      state.polls.telemetry = false; updateStatus();
      clearTimeout(state.telemetryTimer);
      if(!document.hidden)state.telemetryTimer=setTimeout(pollTelemetry,failed?Math.max(500,state.telemetryRefreshMs):Math.max(0,state.telemetryRefreshMs-(performance.now()-started)));
    }
  }
  async function pollLocal() {
    if (state.polls.local) return; state.polls.local = true;
    try { state.local = await getJSON('/api/diagnostico'); state.localAt=Date.now(); state.browserOnline = true; deviceSample('equipe',{cpu:state.local.cpu_valida?value(state.local,'cpu_total_decimos_percentual',10):null,core0:state.local.cpu_valida?value(state.local,'cpu_nucleo_0_decimos_percentual',10):null,heap:value(state.local,'heap_interno_livre',1024),temp:state.local.temperatura_valida?value(state.local,'temperatura_centesimos_c',100):null,rate:value(state.local,'taxa_pacotes_centesimos_hz',100)}); }
    catch (_) { state.browserOnline = Date.now() - state.lastHttp < 5000; deviceSample('equipe',{cpu:null,core0:null,heap:null,temp:null,rate:null}); }
    finally {state.localChecked=true;state.polls.local = false; set('equipe-client-count',state.local&&Number.isFinite(state.local.clientes_wifi)?String(state.local.clientes_wifi):'—');updateStatus(); renderGroup('equipe');}
  }
  async function masterQuery(consult, quiet = true) {
    const key = 'master-' + consult; if (state.polls[key]) return state.polls[key];
    const pending=(async()=>{
      try { const d = await remote(() => getJSON('/api/mestre?consulta=' + encodeURIComponent(consult), 18000)); const restored=consult==='ping'&&!state.masterOnline;state.master[consult] = d; state.masterOnline = true; if(consult==='saude')state.lastHealthAt=Date.now(); if(consult==='diagnostico_mestre')state.masterDiagnosisAt=Date.now(); if(restored){state.lastCommandPoll=0;state.lastAuxPoll=0;}updateMasterUI(consult,d); if(consult==='ping')deviceSample('mestre',{rtt:value(d,'rtt_ms')});if(consult==='diagnostico_mestre')deviceSample('mestre',{cpu:d.cpu_valida?value(d,'cpu_total_decimos_percentual',10):null,core0:d.cpu_valida?value(d,'cpu_nucleo_0_decimos_percentual',10):null,core1:d.cpu_valida?value(d,'cpu_nucleo_1_decimos_percentual',10):null,heap:value(d,'heap_interno_livre',1024),temp:d.temperatura_valida?value(d,'temperatura_centesimos_c',100):null}); if (!quiet) commandEvent('MESTRE', consult, 'Executado', d); return d; }
      catch (error) {
        if (consult === 'ping') {
          state.masterOnline = false;
          showUnconfirmedControl('Mestre sem resposta. Estado não confirmado.');
          deviceSample('mestre',{rtt:null,cpu:null,core0:null,core1:null,heap:null,temp:null});
        }
        if (!quiet) commandEvent('MESTRE', consult, error.name === 'AbortError' ? 'Tempo esgotado' : 'Falhou', null, error.message);
        return null;
      }
      finally {
        if (consult === 'ping') state.masterChecked = true;
        updateStatus();
        availability();
        if (state.tab === 'mestre' || (consult !== 'controle_remoto' && consult !== 'sensores')) renderDevice('mestre','mestre');
      }
    })();
    state.polls[key]=pending;
    try{return await pending;}finally{state.polls[key]=null;}
  }
  async function visitorsQuery(consult, quiet = true) {
    const key = 'visitor-' + consult; if (state.polls[key]) return null; state.polls[key] = true;
    const name = 'satelite_visitantes_' + consult;
    try {const d = await remote(() => getJSON('/api/mestre?consulta=' + name, 18000)); state.visitors[consult] = d; state.visitorsOnline = true; if(consult==='diagnostico')state.visitorsDiagnosisAt=Date.now(); if(consult==='ping')deviceSample('visitantes',{rtt:value(d,'rtt_ms'),availability:100});if(consult==='diagnostico')deviceSample('visitantes',{cpu:d.cpu_valida?value(d,'cpu_total_decimos_percentual',10):null,core0:d.cpu_valida?value(d,'cpu_nucleo_0_decimos_percentual',10):null,core1:d.cpu_valida?value(d,'cpu_nucleo_1_decimos_percentual',10):null,heap:value(d,'heap_interno_livre',1024),temp:d.temperatura_valida?value(d,'temperatura_centesimos_c',100):null}); if (!quiet) commandEvent('VISITANTES', consult, 'Executado', d); return d;}
    catch (error) {if (consult === 'ping') {state.visitorsOnline = false; state.visitors.ping = {error:true};deviceSample('visitantes',{rtt:null,availability:0});}if(consult==='diagnostico'){state.visitors.diagnostico=null;deviceSample('visitantes',{cpu:null,core0:null,core1:null,heap:null,temp:null});} if (!quiet) commandEvent('VISITANTES', consult, error.name === 'AbortError' ? 'Tempo esgotado' : 'Falhou', null, error.message); return null;}
    finally {if(consult==='ping')state.visitorsChecked=true;state.polls[key] = false;set('visitantes-client-count',state.visitorsOnline&&Number.isFinite(state.visitors.diagnostico?.clientes_wifi)?String(state.visitors.diagnostico.clientes_wifi):'—');updateStatus(); renderGroup('visitantes');}
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
  function renderGroup(group) {
    const host = $(group + '-content');
    if (!host.firstElementChild) {
      const article = document.createElement('article');
      article.id = group + '-details';
      article.className = 'device-dashboard';
      host.append(article);
    }
    renderDevice(group, group);
  }

  function deviceFacts(id, diagnosis) {
    const missing = 'Não disponível';
    const local = id === 'equipe';
    const master = id === 'mestre';
    const source = master ? state.master : state.visitors;
    const uptime = diagnosis?.tempo_ativo_ms ?? source.ping?.tempo_ativo_ms;
    const reset = diagnosis?.motivo_reinicio ?? diagnosis?.motivo_ultimo_reset;
    const core = index => diagnosis?.cpu_valida
      ? format(value(diagnosis, 'cpu_nucleo_' + index + '_decimos_percentual', 10), '%', 1)
      : missing;
    const rows = [
      ['Firmware', (local ? diagnosis?.versao_firmware : source.versao?.firmware) || missing],
      ['Tempo ligado', Number.isFinite(uptime) ? age(uptime) : missing],
      ['CPU núcleo 0', core(0)],
      ['CPU núcleo 1', core(1)],
      ['PSRAM livre', diagnosis?.psram_disponivel || diagnosis?.psram_total ? bytes(diagnosis.psram_livre) : missing],
      ['Último reinício', Number.isFinite(reset) ? 'Código ' + reset : missing]
    ];

    if (master) {
      const sync = state.master.sincronizacao;
      let syncStatus = missing;
      if (sync) {
        syncStatus = 'Pronta';
        if (sync.em_andamento) syncStatus = 'Em andamento';
        else if (sync.sincronizacoes_finalizadas) syncStatus = sync.ultimo_resultado_sincronizacao === 0 ? 'Concluída' : 'Falhou';
      }
      rows.push(
        ['Heap livre (total)', bytes(value(diagnosis, 'heap_livre'))],
        ['Sincronização Google', syncStatus],
        ['Arquivos pendentes no SD', sync?.arquivos_pendentes ?? missing]
      );
    } else if (!local) {
      rows.push(
        ['Heap mínimo', bytes(value(diagnosis, 'heap_interno_minimo'))],
        ['Maior bloco interno', bytes(value(diagnosis, 'heap_interno_maior_bloco'))],
        ['Tarefas ativas', diagnosis?.tarefas_ativas ?? missing]
      );
    }
    return rows;
  }

  function deviceLinkFacts(id, diagnosis) {
    const missing = 'Não disponível';
    const local = id === 'equipe';
    const visitors = id === 'visitantes';
    const ping = visitors ? state.visitors.ping : state.master.ping;
    const rtt = local ? null : ping?.rtt_ms;
    const route = local ? 'Navegador → equipe' : visitors ? 'Equipe → mestre → visitantes' : 'Equipe → mestre (UART)';
    const rows = [['Trajeto', route]];
    if (!local) rows.push(['Tempo de resposta', Number.isFinite(rtt) ? rtt + ' ms' : missing]);
    if (!visitors) {
      const telemetry = state.local?.valido ? state.local : null;
      rows.push(
        ['Taxa de telemetria', telemetry ? format(value(telemetry, 'taxa_pacotes_centesimos_hz', 100), 'Hz', 2) : missing],
        ['Perda estimada', telemetry ? format(value(telemetry, 'taxa_perda_centesimos_percentual', 100), '%', 2) : missing]
      );
    }
    if (local) {
      rows.push(['Wi-Fi', diagnosis?.wifi_ssid || missing], ['UART RX', diagnosis?.uart_recepcoes_ok ?? missing]);
    }
    if (local || visitors) rows.push(['Clientes Wi-Fi', diagnosis?.clientes_wifi ?? missing]);
    return rows;
  }

  function renderDeviceFacts(list, rows) {
    rows.forEach(([label, text], index) => {
      if (!list.children[index]) {
        const row = document.createElement('div');
        const term = document.createElement('dt');
        term.textContent = label;
        row.append(term, document.createElement('dd'));
        list.append(row);
      }
      const cell = list.children[index].lastElementChild;
      if (cell.textContent !== String(text)) cell.textContent = String(text);
    });
  }

  function deviceHealthMarkup(group) {
    const tag = group === 'mestre' ? 'summary' : 'div';
    const summary = `
      <${tag} class="device-health-band message-state" id="device-health-${group}" data-tone="info">
        <span class="device-health-mark" aria-hidden="true"></span>
        <span class="device-health-copy"><span>Saúde do sistema</span><strong id="device-health-status-${group}">Aguardando</strong></span>
        <small id="device-health-note-${group}"></small>
      </${tag}>`;
    if (tag === 'div') return summary;
    return `
      <details id="health-diagnostics" class="health-diagnostics">
        ${summary}
        <div class="health-diagnostics-body">
          <section aria-labelledby="health-diagnostics-causes-title">
            <h2 id="health-diagnostics-causes-title">Causas ativas</h2>
            <p id="health-diagnostics-empty">Aguardando leitura de saúde.</p>
            <ul id="health-diagnostics-causes"></ul>
          </section>
          <p id="health-diagnostics-age"></p>
          <div class="device-facts-grid health-diagnostics-grid">
            <section hidden><h2>Memória e tarefas</h2><dl id="health-diagnostics-memory"></dl></section>
            <section hidden><h2>Ocorrências acumuladas</h2><dl id="health-diagnostics-counters"></dl></section>
          </div>
        </div>
      </details>`;
  }

  function readHealthFields(data, fields, format) {
    return fields.flatMap(([label, field]) => {
      const reading = value(data, field);
      return reading === null ? [] : [[label, format(reading)]];
    });
  }

  function renderHealthFields(id, rows) {
    const list = $(id);
    list.parentElement.hidden = !rows.length;
    const signature = rows.map(([label]) => label).join('|');
    if (list.dataset.fields !== signature) {
      list.replaceChildren();
      list.dataset.fields = signature;
    }
    renderDeviceFacts(list, rows);
  }

  function renderMasterHealthDiagnostics({health, fresh, causes, mask, receivedAt}) {
    const panel = $('health-diagnostics');
    if (!panel?.open) return;

    set('health-diagnostics-causes-title', fresh || !receivedAt ? 'Causas ativas' : 'Últimas causas registradas');
    const empty = $('health-diagnostics-empty');
    empty.hidden = causes.length > 0;
    set('health-diagnostics-empty', mask === null
      ? receivedAt ? 'Causas indisponíveis nesta leitura.' : 'Aguardando leitura de saúde.'
      : fresh ? 'Nenhuma causa ativa.' : 'Nenhuma causa na última leitura.');

    const list = $('health-diagnostics-causes');
    const causeSignature = causes.map(cause => cause.label).join('|');
    if (list.dataset.causes !== causeSignature) {
      list.dataset.causes = causeSignature;
      list.replaceChildren(...causes.map(cause => {
        const item = document.createElement('li');
        if (cause.target) {
          const button = document.createElement('button');
          button.type = 'button';
          button.className = 'health-cause-link';
          button.textContent = cause.label;
          button.addEventListener('click', () => openHealthCause(cause));
          item.append(button);
        } else item.textContent = cause.label;
        return item;
      }));
    }

    const diagnosis = state.master.diagnostico_mestre;
    const diagnosisAt = state.masterDiagnosisAt;
    const stale = !state.masterOnline || Date.now() - diagnosisAt >= healthFreshMs;
    set('health-diagnostics-age', diagnosisAt
      ? (stale ? 'Últimos detalhes técnicos · ' : 'Detalhes técnicos recebidos ') + healthAge(diagnosisAt)
      : 'Aguardando detalhes técnicos.');

    const memory = readHealthFields(diagnosis, [
      ['Heap interno livre', 'heap_interno_livre'],
      ['Heap interno mínimo', 'heap_interno_minimo'],
      ['Maior bloco interno livre', 'heap_interno_maior_bloco']
    ], bytes);
    if (diagnosis?.psram_disponivel) {
      memory.push(...readHealthFields(diagnosis, [
        ['PSRAM livre', 'psram_livre'],
        ['Maior bloco livre na PSRAM', 'psram_maior_bloco']
      ], bytes));
    }
    if (!diagnosis) memory.push(...readHealthFields(health, [['Heap livre (total)', 'heap_livre']], bytes));
    if (diagnosis?.pilha_valida && Number.isFinite(diagnosis.menor_pilha_livre_bytes)) {
      memory.push(['Menor folga de pilha', number(diagnosis.menor_pilha_livre_bytes, 0) + ' B']);
      if (diagnosis.tarefa_menor_pilha) memory.push(['Tarefa com menor folga', diagnosis.tarefa_menor_pilha]);
    }
    memory.push(...readHealthFields(diagnosis, [['Tarefas ativas', 'tarefas_ativas']], n => number(n, 0)));
    renderHealthFields('health-diagnostics-memory', memory);

    renderHealthFields('health-diagnostics-counters', [
      ...readHealthFields(health, [['Total informado', 'erros_historicos']], n => number(n, 0)),
      ...readHealthFields(diagnosis, [
        ['Erros de sensor', 'erros_sensores'],
        ['Erros I²C', 'erros_i2c'],
        ['Erros UART', 'erros_uart'],
        ['Descartes UART', 'descartes_uart'],
        ['Falhas OTA', 'falhas_ota'],
        ['Falhas de supervisão', 'falhas_supervisao']
      ], n => number(n, 0))
    ]);
  }

  function mountDevice(root, group, id) {
    const name = devices[id].name;
    root.innerHTML = `
      ${deviceHealthMarkup(group)}
      <dl class="device-metrics">
        <div><dt>CPU</dt><dd id="device-glance-cpu-${group}">—</dd></div>
        <div><dt>Heap interno livre</dt><dd id="device-glance-heap-${group}">—</dd></div>
        <div><dt>Temperatura</dt><dd id="device-glance-temp-${group}">—</dd></div>
      </dl>
      <p class="device-read-time" id="device-read-time-${group}"></p>
      <section class="device-history-section" aria-label="Histórico de ${name}">
        <div class="device-chart-head">
          <h2>Histórico</h2>
          <div class="device-chart-controls">
            <label>Eixo esquerdo<select id="device-chart-select-${group}"></select></label>
            <label>Eixo direito<select id="device-chart-select2-${group}"></select></label>
          </div>
        </div>
        <div id="device-chart-legend-${group}" class="device-chart-legend"></div>
        <div class="device-chart"><canvas id="device-chart-${group}" role="img"></canvas></div>
      </section>
      <details class="device-facts">
        <summary>Detalhes do ESP</summary>
        <div class="device-facts-grid">
          <section><h2>Sistema</h2><dl id="device-rows-${group}"></dl></section>
          <section><h2>Enlace</h2><dl id="device-link-rows-${group}"></dl></section>
        </div>
      </details>`;
    root.querySelector('.device-facts').open = matchMedia('(min-width: 900px)').matches;
    root.querySelector('.health-diagnostics')?.addEventListener('toggle', () => renderMasterHealthDiagnostics(getSystemHealth()));

    const selects = [$('device-chart-select-' + group), $('device-chart-select2-' + group)];
    const signals = [state.deviceSignal, state.deviceSignalSecondary];
    selects.forEach((select, index) => {
      deviceChartOptions[id].forEach(option => {
        const item = document.createElement('option');
        item.value = option.key;
        item.textContent = option.key === 'heap' ? 'Memória livre' : option.key === 'temp' ? 'Temperatura' : option.label;
        item.title = option.label;
        select.append(item);
      });
      select.value = signals[index][id];
      select.addEventListener('change', () => {
        const previous = signals[index][id];
        signals[index][id] = select.value;
        const other = 1 - index;
        if (select.value === signals[other][id]) {
          signals[other][id] = previous;
          selects[other].value = previous;
        }
        renderDevice(group, id);
      });
    });
  }

  function renderDeviceSystemHealth(healthState = getSystemHealth()) {
    const {label, tone, causes, fresh, receivedAt} = healthState;
    const causeNote = fresh ? healthCauseSummary(causes) : '';
    for (const group of Object.keys(devices)) {
      const band = $('device-health-' + group);
      if (!band) continue;
      band.dataset.tone = tone;
      set('device-health-status-' + group, label);
      const note = group === 'mestre' && receivedAt
        ? 'Lido ' + healthAge(receivedAt) : causeNote;
      set('device-health-note-' + group, note);
    }
    renderMasterHealthDiagnostics(healthState);
  }

  function renderDevice(group, id) {
    const root = $(group + '-details');
    const status = deviceStatus(id);
    const tone = status === 'Conectado' ? 'success' : status === 'Sem resposta' ? 'error' : 'info';
    const online = tone === 'success';
    const diagnosis = id === 'mestre' ? state.master.diagnostico_mestre
      : id === 'equipe' ? state.local : state.visitors.diagnostico;
    const readAt = id === 'mestre' ? state.masterDiagnosisAt : id === 'equipe' ? state.localAt : state.visitorsDiagnosisAt;
    const link = $(group + '-auto');

    if (!root.firstElementChild) mountDevice(root, group, id);
    link.dataset.tone = tone;
    set(group + '-auto', status === 'Não consultado' ? 'Aguardando enlace' : status);
    renderDeviceSystemHealth();
    set('device-glance-cpu-' + group, diagnosis?.cpu_valida ? format(value(diagnosis, 'cpu_total_decimos_percentual', 10), '%', 1) : '—');
    set('device-glance-heap-' + group, diagnosis && Number.isFinite(diagnosis.heap_interno_livre) ? bytes(diagnosis.heap_interno_livre) : '—');
    set('device-glance-temp-' + group, diagnosis?.temperatura_valida ? format(value(diagnosis, 'temperatura_centesimos_c', 100), '°C', 1) : '—');
    root.querySelector('.device-metrics').classList.toggle('stale', !online && Boolean(readAt));
    set('device-read-time-' + group, readAt ? (online ? 'Diagnóstico às ' : 'Último diagnóstico às ') + new Date(readAt).toLocaleTimeString('pt-BR') : 'Aguardando diagnóstico');
    renderDeviceFacts($('device-rows-' + group), deviceFacts(id, diagnosis));
    renderDeviceFacts($('device-link-rows-' + group), deviceLinkFacts(id, diagnosis));

    const charts = selectedDeviceCharts(id);
    const legend = $('device-chart-legend-' + group);
    const signature = charts.map(chart => chart.key).join(':');
    if (legend.dataset.signals !== signature) {
      legend.dataset.signals = signature;
      legend.replaceChildren();
      charts.forEach((chart, index) => {
        const item = document.createElement('span');
        const mark = document.createElement('i');
        mark.className = 'device-legend-key';
        mark.style.setProperty('--series-color', 'var(--chart-' + chart.tone + ')');
        item.append(mark, document.createTextNode((index ? 'Dir.' : 'Esq.') + ' · ' + chart.label + ' (' + chart.unit + ')'));
        legend.append(item);
      });
    }
    $('device-chart-' + group).setAttribute('aria-label', 'Histórico de ' + charts.map(chart => chart.label + ' em ' + chart.unit).join(' e ') + ' de ' + devices[id].name);
    if (state.tab === group) requestAnimationFrame(() => drawDeviceChart(group, id));
  }
  const actionNames={velocidade_maxima:'Definir velocidade máxima',parada_remota:'Parada de emergência',verificar_ota:'Verificar atualizações',autorizar_mestre:'Autorizar OTA do mestre',autorizar_equipe:'Autorizar OTA da Equipe',autorizar_visitantes:'Autorizar OTA de Visitantes',autorizar_todos:'Autorizar OTA de todos',cancelar_ota:'Cancelar OTA',alternar_relatorios:'Pausar ou retomar relatórios',resumo_terminal:'Solicitar resumo no terminal',sincronizar_google:'Solicitar sincronização',zerar_percurso:'Zerar percurso',intervalo_relatorio:'Alterar intervalo do relatório',intervalo_ota:'Alterar intervalo de verificação OTA',intervalo_telemetria_ota:'Alterar telemetria durante OTA',simulador_telemetria:'Alterar simulador de telemetria',wifi_test:'Testar nova rede Wi-Fi',wifi_conectar_salva:'Conectar rede salva',wifi_excluir_salva:'Excluir rede salva',wifi_primary:'Definir rede principal',wifi_restaurar_principal:'Restaurar rede principal',wifi_automatico:'Alterar conexão automática',wifi_politica:'Salvar política Wi-Fi'};
  const timeOf = time => new Date(time).toLocaleTimeString('pt-BR');
  Object.assign(actionNames,{limite_marcha:'Definir marcha máxima',velocidade_maxima:'Salvar velocidade máxima',limite_acelerador:'Salvar acelerador máximo',liberar_parada_remota:'Armar',ajuste_desenvolvedor:'Salvar alterações'});
  const controlFields = {
    velocidade_maxima: ['remote-speed',100,35,'velocidade_maxima_centesimos_kmh'],
    limite_acelerador: ['remote-throttle-max',10,100,'acelerador_maximo_decimos_percentual']
  };
  // IDs e limites de protocolo em ajustes_desenvolvedor.h. A escala converte o inteiro em unidade de tela.
  const developerGroups = [
    ['Pedal e DAC', [
      [0,'Tensão no início do curso do pedal','mV',1,100,2500],
      [1,'Tensão com o pedal no fim do curso','mV',1,200,3000],
      [2,'Tensão para reconhecer o pedal solto','mV',1,101,2800],
      [3,'Tolerância de tensão fora do curso do pedal','mV',1,0,400],
      [4,'Zona morta do pedal','%',10,0,300],
      [27,'Ganho do pedal','×',1000,500,2000],
      [5,'Tensão do DAC em repouso','mV',1,0,2500],
      [6,'Tensão do DAC a 100%','mV',1,100,5000],
      [14,'Intervalo de reenvio da tensão ao DAC','ms',1,20,1000],
      [15,'Intervalo para tentar reconectar o DAC','ms',1,100,30000]
    ]],
    ['Armamento', [
      [30,'Marcha máxima','',1,1,3],
      [7,'Tempo com pedal solto para armar','ms',1,250,3000],
      [12,'Velocidade máxima para armar','km/h',100,0,100],
      [13,'Espera antes de rearmar','ms',1,5000,30000]
    ]],
    ['Limitador de velocidade', [
      [29,'Velocidade máxima','km/h',100,0,3500],
      [28,'Acelerador máximo','%',10,0,1000],
      [9,'Faixa de redução progressiva','km/h',100,1,1000],
      [10,'Margem para retomar após o corte','km/h',100,1,500],
      [11,'Leituras consecutivas para retomar','',1,1,20],
      [8,'Rampa de subida do acelerador','p.p./s',10,10,1000]
    ]],
    ['Rodas e encoders', [
      [16,'Diâmetro da roda esquerda','mm',1,50,2000],
      [17,'Diâmetro da roda direita','mm',1,50,2000],
      [18,'Voltas do encoder por volta da roda esquerda','×',1000,50,100000],
      [19,'Voltas do encoder por volta da roda direita','×',1000,50,100000],
      [20,'Fator de correção da roda esquerda','×',1000,500,2000],
      [21,'Fator de correção da roda direita','×',1000,500,2000],
      [22,'Encoder esquerdo','pulsos/giro',1,1,100000],
      [23,'Encoder direito','pulsos/giro',1,1,100000]
    ]],
    ['Sensores elétricos e inerciais', [
      [24,'Fator de tensão do INA228','×',1000,100,10000],
      [25,'Fator de corrente do INA228','×',1000,100,10000],
      [26,'Fator de aceleração do MPU','×',1000,500,2000]
    ]],
    ['Bateria', [
      [31,'Baterias em série','',1,1,5],
      [32,'Tensão por bateria a 0%','V',1000,9000,14000],
      [33,'Tensão por bateria a 100%','V',1000,10000,15000],
      [34,'Tempo parado para estimar a carga','s',1000,100,120000]
    ]],
    ['Sinalização', [
      [35,'Intensidade máxima do buzzer','%',1,0,100],
      [36,'LED integrado do Mestre','',1,0,1]
    ]]
  ];
  const developerSettings = new Map(developerGroups.flatMap(([,rows])=>rows.map(row=>[row[0],row])));
  const developerDefaults = [500,2500,650,200,50,1000,5000,500,250,300,30,2,50,5000,20,5000,
    1200,1200,5000,5000,1000,1000,600,600,1000,1000,1000,1000,1000,3000,3,
    4,11200,13000,1000,65,1];
  const developerHelp = [
    'Tensão de referência no início do curso do pedal.',
    'Tensão de referência no fim do curso do pedal.',
    'A tensão deve ficar abaixo deste valor para o Mestre considerar o pedal solto.',
    'Tolerância fora da faixa calibrada. Se a leitura passar dessa margem, o Mestre rejeita a amostra.',
    'Parte inicial do curso que continua pedindo zero de aceleração.',
    'Tensão de repouso do DAC com o carro armado. Com a tração bloqueada, a referência vai a 0 V.',
    'Tensão do DAC correspondente a 100% de aceleração permitida.',
    'Por quanto tempo o pedal deve ficar solto antes de autorizar o armamento.',
    'Maior: o acelerador sobe mais rápido. Menor: sobe mais suave. A redução continua imediata.',
    'Maior: reduz o teto do acelerador mais cedo. Menor: reduz mais perto do limite. O Mestre limita a faixa a um terço da velocidade máxima.',
    'Maior: o carro precisa perder mais velocidade para voltar a acelerar. Menor: libera mais perto do limite. A margem fica limitada à metade da faixa de redução.',
    'Mais leituras: demora mais para liberar após o corte e tolera melhor oscilações. Menos: libera mais rápido. Conta só amostras novas do odômetro, a cada 20 ms.',
    'Velocidade máxima medida em que o Mestre aceita armar.',
    'Tempo mínimo entre a parada de emergência e um novo armamento.',
    'Intervalo para reenviar a referência com a saída habilitada. O padrão de 20 ms corresponde a 50 Hz.',
    'Intervalo entre tentativas de localizar o DAC quando ele fica indisponível.',
    'Meça de uma borda à outra passando pelo centro. Se mediu a volta do pneu, divida por π.',
    'Meça de uma borda à outra passando pelo centro. Se mediu a volta do pneu, divida por π.',
    'Quantas voltas o encoder esquerdo dá a cada volta da roda esquerda.',
    'Quantas voltas o encoder direito dá a cada volta da roda direita.',
    'Multiplica a velocidade e a distância calculadas para a roda esquerda.',
    'Multiplica a velocidade e a distância calculadas para a roda direita.',
    'Quantidade de pulsos por volta do eixo do encoder esquerdo.',
    'Quantidade de pulsos por volta do eixo do encoder direito.',
    'Multiplica a tensão do INA228 usada na telemetria e no cálculo de potência e energia.',
    'Multiplica a corrente do INA228 usada na telemetria e no cálculo de potência, energia e carga.',
    'Multiplica a aceleração longitudinal filtrada do MPU6050.',
    'Multiplica o pedido do pedal depois da zona morta. Os limites de aceleração continuam valendo.',
    'Maior: permite mais acelerador. Menor: reduz o máximo permitido, inclusive longe do limite de velocidade. Zero bloqueia a tração.',
    'Maior: corta o pedido de aceleração mais tarde. Menor: corta mais cedo. Zero bloqueia a tração. O Mestre usa a maior velocidade válida das rodas.',
    'Marcha mais alta permitida pelo Mestre quando o controle de marcha está habilitado.',
    'Multiplica as tensões por bateria para obter os limites do banco.',
    'Abaixo deste limite por bateria, a estimativa de carga chega a 0%.',
    'Acima deste limite por bateria, a estimativa de carga chega a 100%.',
    'O odômetro deve indicar repouso contínuo por este tempo. Em movimento, o painel mantém a última estimativa.',
    'Limita a intensidade dos avisos sonoros. Zero silencia o buzzer.',
    'Liga ou desliga o RGB da placa do Mestre, no GPIO 48. Os LEDs externos e o buzzer continuam sinalizando.'
  ];
  const developerGears = ['', 'L', 'M', 'H'];
  const developerLedOptions = ['Desligado', 'Ligado'];
  function developerDisplay(id, value, scale, unit) {
    if(id===30)return developerGears[value];
    if(id===36)return developerLedOptions[value];
    return format(value/scale,unit,Math.log10(scale)).trim();
  }
  $('developer-tabs').innerHTML=developerGroups.map(([group],index)=>`
    <button type="button" role="tab" id="developer-tab-${index}" aria-controls="developer-group-${index}"
      aria-selected="${index===0}" tabindex="${index===0?0:-1}">${group}</button>`).join('');
  $('developer-settings').innerHTML=developerGroups.map(([group,rows],index)=>`
    <div id="developer-group-${index}" class="developer-rows" role="tabpanel"
      aria-labelledby="developer-tab-${index}" ${index===0?'':'hidden'}>
      ${group==='Armamento'?'<p class="developer-startup">Parada de emergência na partida <strong>Inativa por padrão</strong>. O carro inicia desarmado.</p>':''}
      ${group==='Limitador de velocidade'?'<p id="developer-limiter-state" class="developer-startup" role="status">Aguardando ajustes do Mestre.</p>':''}
      ${rows.map(([id,label,unit,scale,min,max])=>`<div class="developer-row">
        <div class="developer-name">
          <label for="developer-value-${id}">${label}${unit?' ('+unit+')':''}
            <small>Padrão ${developerDisplay(id, developerDefaults[id], scale, unit)}</small></label>
          <details class="developer-help">
            <summary aria-label="Ajuda sobre ${label}">?</summary>
            <p>${developerHelp[id]} <small>Gravado na NVS do Mestre.</small></p>
          </details>
        </div>
        ${id===30 ? '<select id="developer-value-30">'+developerGears.slice(1).map((gear,index)=>'<option value="'+(index+1)+'">'+gear+'</option>').join('')+'</select>' :
          id===36 ? '<select id="developer-value-36" disabled><option value="" disabled selected>Indisponível</option>'+developerLedOptions.map((label,index)=>'<option value="'+index+'">'+label+'</option>').join('')+'</select>' :
          `<input id="developer-value-${id}" type="number" min="${min/scale}" max="${max/scale}" step="${1/scale}">`}
      </div>`).join('')}
    </div>`).join('');
  $('developer-save').dataset.cap=String(CAP_DEVELOPER_SETTINGS);
  const developerInputs=[...$('developer-settings').querySelectorAll('input, select')];
  function bindSettingsTabs(id, onSelect) {
    const tabs=[...$(id).querySelectorAll('[role="tab"]')];
    const show=index=>{
      tabs.forEach((tab,i)=>{
        tab.setAttribute('aria-selected',String(i===index));
        tab.tabIndex=i===index?0:-1;
        $(tab.getAttribute('aria-controls')).hidden=i!==index;
      });
      onSelect(tabs[index]);
    };
    tabs.forEach((tab,index)=>{
      tab.addEventListener('click',()=>show(index));
      tab.addEventListener('keydown',event=>{
        const count=tabs.length;
        const targets={ArrowLeft:(index+count-1)%count,ArrowRight:(index+1)%count,Home:0,End:count-1};
        if (!Object.hasOwn(targets,event.key)) return;
        event.preventDefault();
        const next=targets[event.key];
        show(next);
        tabs[next].focus();
      });
    });
    return show;
  }
  const showDeveloperGroup=bindSettingsTabs('developer-tabs',()=>{});
  let developerDevice='mestre';
  const radioSettings={};
  for(const target of ['equipe','visitantes']){
    const panel=$('developer-panel-'+target);
    panel.innerHTML=`<p class="command-field-status" role="status">Aguardando o satélite.</p>
      <fieldset class="developer-fields" aria-label="Wi-Fi do satélite" disabled>
        <div class="command-fields developer-radio-fields">
          <label>Canal Wi-Fi<input type="number" min="1" max="13" step="1" required></label>
          <label>Largura do canal<select><option value="20">20 MHz</option><option value="40">40 MHz</option></select></label>
          <label>Potência máxima (dBm)<input type="number" min="0" max="20" step="0.25" required></label>
          <label>Máximo de conexões<input type="number" min="1" max="10" step="1" required></label>
        </div>
        <p class="command-field-status">0 dBm neste campo usa o padrão do rádio. Os ajustes entram em vigor ao reiniciar o satélite.</p>
        <div class="wifi-step-actions">
          <button type="button" class="button" data-master-action="salvar_radio_satelite" data-radio-target="${target}" disabled>Salvar Wi-Fi</button>
        </div>
      </fieldset>
      <p><button type="button" class="button secondary developer-radio-clients">Gerenciar conexões</button></p>`;
    panel.querySelector('.developer-radio-clients').addEventListener('click',()=>revealSection($('wifi-local')));
    radioSettings[target]={
      query:'satelite_'+target+'_radio',
      status:panel.querySelector('[role="status"]'),
      fieldset:panel.querySelector('fieldset'),
      fields:[...panel.querySelectorAll('input, select')],
      confirmed:false,
    };
  }
  bindSettingsTabs('developer-devices',tab=>{
    developerDevice=tab.id.slice('developer-device-'.length);
    void pollVisibleReadings();
  });
  Object.assign(actionNames,{wifi_restaurar_politica:'Restaurar política Wi-Fi',logs_terminal:'Configurar logs do terminal',sensor_estado:'Alterar aquisição de sensores',salvar_radio_satelite:'Salvar Wi-Fi'});
  async function refreshRadioSettings(target) {
    const radio=radioSettings[target];
    if(!await masterQuery(radio.query)){
      radio.confirmed=false;
      radio.status.textContent='Ajustes de Wi-Fi indisponíveis. Confira o enlace e o firmware do satélite e do Mestre.';
      availability();
    }
  }
  function renderRadioSettings(radio,data) {
    radio.confirmed=Array.isArray(data.salvo)&&data.salvo.length===4&&Array.isArray(data.ativo)&&data.ativo.length===4;
    if(!radio.confirmed){radio.status.textContent='O satélite não informou os parâmetros de Wi-Fi.';return;}
    const [channel,band,power,clients]=data.ativo;
    const [first,,,last]=radio.fields;
    first.min=String(data.canal_minimo);
    first.max=String(data.canal_maximo);
    last.max=String(data.capacidade_clientes);
    radio.fields.forEach((field,index)=>{
      if(!field.dataset.edited&&document.activeElement!==field)
        field.value=String(index===2?data.salvo[index]/4:data.salvo[index]);
    });
    const pending=data.reinicio_pendente?' · Perfil salvo. Reinicie o satélite para aplicar.':'';
    radio.status.textContent='Em uso · Canal '+channel+' · '+band+' MHz · '+format(power/4,'dBm',2)+' · Até '+clients+' conexões'+pending;
  }
  function runMeta(run) {
    const ids=run.requests.length ? 'Solicitação '+run.requests.map(id=>'#'+id).join(', ') : 'Número da solicitação ainda indisponível';
    const response=run.responses[run.responses.length-1];
    return timeOf(run.time)+' · '+ids+(response&&Number.isFinite(response.duracao_ms)?' · Resposta em '+response.duracao_ms+' ms':'')+(response&&Number.isFinite(response.tentativas)?' · '+response.tentativas+' tentativa(s)':'')+(run.checkedAt ? ' · Estado consultado às '+timeOf(run.checkedAt) : '');
  }
  function renderMasterCommandHistory(){
    const list=$('master-command-history');if(!list)return;
    const current=state.commandRuns[0];
    if(current){
      $('command-overview').dataset.tone=current.tone;
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
  const simulatorSourceButtons=document.querySelectorAll('[data-source-value]');
  const reversePermissionButtons=document.querySelectorAll('[data-master-action="permitir_re"]');
  const automaticRearmMs=30000;
  const minimumRearmMs=5000;
  function isolatedDacAvailable(control=state.master.controle_remoto) {
    return control?.dac_isolado_habilitado ?? ((state.master.versao?.capacidades & CAP_ISOLATED_DAC) !== 0);
  }
  function needsArming(control) {
    return control?.parada_solicitada || control?.armamento_inicial_pendente;
  }
  function armingBlock(control) {
    if (state.stopInFlight) return 'Confirmando a parada…';
    if (!needsArming(control) || !state.masterOnline) return '';
    if (state.controlUnconfirmed) return 'Aguardando confirmação do estado do controle.';
    if (control.ota_em_andamento) {
      const ota = state.master.ota?.mestre;
      if (masterOtaValidationActive(ota?.estado))
        return (ota.estado===10?'Validando firmware':'Firmware em observação')+
          ' · '+number(ota.progresso_percentual,0)+'% desta etapa.';
      return 'Aguarde a atualização e a validação do firmware.';
    }
    if (control.partida_concluida === false) return 'Aguarde a inicialização do Mestre.';
    const waitMs = control.rearme_restante_ms ?? Math.max(0,(control.parada_restante_ms||0)-(automaticRearmMs-minimumRearmMs));
    if (waitMs > 0) return 'Aguarde '+Math.ceil(waitMs/1000)+' s para armar.';
    const isolated=isolatedDacAvailable(control);
    if (control.atuacao_habilitada === false && !isolated) return 'Saída do acelerador desabilitada no Mestre.';
    if (control.bloqueios & (1 << 7)) return 'Verifique as saídas de tração.';
    if (control.bloqueios & (1 << 3)) return 'Selecione sensores reais para armar.';
    if (control.bloqueios & (1 << 2)) {
      const health = getSystemHealth();
      if (health.fresh && (health.mask & encoderDivergenceMask)) return 'Verifique a divergência entre os encoders.';
      if (health.fresh && (health.mask & odometerUnavailableMask)) return 'Aguardando uma leitura válida dos encoders.';
      return 'Aguardando os sensores de movimento.';
    }
    if (control.faixa_marcha_fisica === 0 && !isolated) return 'Selecione uma marcha na chave do veículo.';
    if (control.reversao_solicitada && control.reversao_permitida === false) return 'A ré está bloqueada. Selecione Frente na chave.';
    if (control.velocidade_maxima_centesimos_kmh === 0 || control.acelerador_maximo_decimos_percentual === 0) {
      return 'Defina limites de velocidade e acelerador acima de zero.';
    }
    if ((control.bloqueios & (1 << 4)) || control.velocidade_medida_centesimos_kmh >= control.velocidade_maxima_centesimos_kmh) {
      return 'Aguarde o veículo parar.';
    }
    if (control.pedal_disponivel === false || control.amostra_pedal_restante_ms === 0) return 'Aguardando a leitura do pedal.';
    if (control.acelerador_lido_decimos_percentual > 0 || (control.bloqueios & (1 << 6))) return 'Mantenha o pedal do acelerador solto.';
    if (control.dac_disponivel === false) return 'DAC do acelerador indisponível.';
    if (control.dac_zero_confirmado === false) return 'Aguardando a confirmação de 0 V no DAC.';
    return '';
  }
  function availability(){
    const capabilities=state.master.versao?.capacidades;
    const control=state.master.controle_remoto;
    const developer=state.master.ajustes_desenvolvedor;
    const developerEditable=state.masterOnline&&!state.developerUnconfirmed&&developer?.edicao_permitida===true;
    const reverseSupported=(capabilities&CAP_REVERSE_PERMISSION)!==0&&typeof control?.reversao_permitida==='boolean';
    for(const button of reversePermissionButtons){
      const selected=reverseSupported&&(button.dataset.controlValue==='1')===control.reversao_permitida;
      button.setAttribute('aria-pressed',String(selected));
    }
    set('reverse-permission-state',reverseSupported?control.reversao_permitida?'Ré permitida.':'Ré bloqueada.':Number.isFinite(capabilities)&&(capabilities&CAP_REVERSE_PERMISSION)===0?'Controle de ré indisponível neste firmware.':'Aguardando o Mestre.');
    document.querySelectorAll('[data-master-action]').forEach(button=>{
      const action=button.dataset.masterAction,mask=Number(button.dataset.cap);
      const supported=(capabilities&mask)===mask||(mask===CAP_CONTROL&&!!control)||(mask===CAP_DEVELOPER_SETTINGS&&!!developer);
      button.disabled=action==='parada_remota'?(!state.browserOnline||!!state.stopInFlight):state.busy||!state.masterOnline||!supported;
      if(action==='sensor_estado'&&(!state.master.sensores?.controle_permitido||!button.dataset.sensorValue))button.disabled=true;
      if(action==='logs_terminal'&&!state.master.logs_terminal)button.disabled=true;
      if(action==='permitir_re'&&!reverseSupported)button.disabled=true;
      if(action==='ajuste_desenvolvedor'&&(!developerEditable||!developerInputs.some(field=>field.dataset.edited)))button.disabled=true;
      if(action==='salvar_radio_satelite'&&!radioSettings[button.dataset.radioTarget].confirmed)button.disabled=true;
      if(action.startsWith('autorizar_')&&masterOtaValidationActive(state.master.ota?.mestre?.estado))button.disabled=true;
    });
    $('developer-fields').disabled=!developerEditable||state.busy;
    for(const radio of Object.values(radioSettings))
      radio.fieldset.disabled=state.busy||!state.masterOnline||!radio.confirmed;
    $('refresh-sensors').disabled=state.busy||!state.masterOnline||(capabilities&CAP_SENSORS)===0;
    $('refresh-terminal-logs').disabled=state.busy||!state.masterOnline||(capabilities&CAP_TERMINAL_LOGS)===0;
    const rearm=document.querySelector('[data-master-action="liberar_parada_remota"]');
    const waitText=armingBlock(control);
    const wait=$('remote-rearm-wait');
    wait.hidden=!waitText;
    set('remote-rearm-wait',waitText);
    if(rearm){
      rearm.disabled ||= !!waitText||!needsArming(control);
      rearm.title=waitText||'Solicitar armamento ao Mestre';
    }
    const sync=state.master.sincronizacao,syncButton=document.querySelector('[data-master-action="sincronizar_google"]');
    if(syncButton){
      if(sync&&(sync.em_andamento||!sync.iniciado||!sync.cartao_disponivel))syncButton.disabled=true;
      syncButton.title=!state.masterOnline?'Mestre sem resposta':sync?.em_andamento?'Sincronização já em andamento':sync&&(!sync.iniciado||!sync.cartao_disponivel)?'Cartão SD ou registro indisponível no mestre':'Solicitar envio dos arquivos pendentes ao Google';
    }
    $('run-master-query').disabled=state.busy;
    $('refresh-ota-state').disabled=state.busy;
    $('refresh-sync-state').disabled=state.busy||!state.masterOnline||(Number.isFinite(capabilities)&&(capabilities&CAP_SYNC)===0);
    $('refresh-simulator-state').disabled=state.busy||!state.masterOnline||(Number.isFinite(capabilities)&&(capabilities&CAP_SIMULATOR)===0);
    $('refresh-wifi-state').disabled=state.busy||!state.masterOnline;
    const supported=Number.isFinite(capabilities)&&(capabilities&(128|256|512))!==0;
    badge('master-command-status',state.masterChecked&&!state.masterOnline?'Mestre sem resposta':supported?'Comandos disponíveis':Number.isFinite(capabilities)?'Ações não anunciadas pelo mestre':'Aguardando resposta do mestre',state.masterChecked&&!state.masterOnline?'bad':supported?'good':'muted');
  }
  function setField(id,newValue){const field=$(id);if(field&&!field.dataset.edited&&document.activeElement!==field)field.value=String(newValue);}
  const sensors=[
    ['ADC','Pedal · GPIO5'],
    ['MPU6050','Aceleração, rotação e temperatura'],
    ['Encoders','Pulsos e velocidade por roda'],
    ['INA228','Tensão, corrente e diagnóstico'],
    ['Temperatura do ESP','Sensor interno do Mestre']
  ];
  const sensorNames=sensors.map(([name])=>name);
  $('sensor-list').insertAdjacentHTML('beforeend',sensors.map(([name,description],id)=>`
    <li class="sensor-card">
      <details>
        <summary><span><strong>${name}</strong><small id="sensor-glance-${id}">${description}</small></span><span id="sensor-state-${id}" class="badge muted">Sem consulta</span></summary>
        <dl id="sensor-facts-${id}" class="sensor-facts" hidden></dl>
      </details>
      <button type="button" class="button secondary" data-master-action="sensor_estado" data-sensor-id="${id}" data-cap="16384" disabled>Pausar</button>
    </li>`).join(''));
  const terminalSources=['Wi-Fi','Rádio Wi-Fi','OTA','Transporte OTA','Google e SD','Telemetria','Sensores','Comunicação UART','Comandos','Supervisão','Sinalização','Relatórios','Cartão SD','Odômetro','Aceleração','I²C','Simulador','Terminal'];
  const terminalLevels=['Silenciado','Erros','Avisos','Informações'];
  function renderSensors(data){
    const ina=data.ina228,i2c=data.i2c;
    const encoder=reading=>number(reading?.contagem,0)+' contagens · '+format(value(reading,'velocidade_kmh'),'km/h',2);
    const axes=reading=>Array.isArray(reading)?reading.map((item,index)=>'XYZ'[index]+' '+number(item,0)).join(' · '):'Não disponível';
    const hex=reading=>Number.isInteger(reading)?'0x'+(reading>>>0).toString(16).toUpperCase():'Não disponível';
    const readings=[
      [['Valor bruto',number(data.adc_bruto,0)]],
      [['Aceleração bruta',axes(data.acelerometro_bruto)],['Rotação bruta',axes(data.giroscopio_bruto)],['Temperatura',format(value(data,'temperatura_mpu_c'),'°C')]],
      [['Roda esquerda',encoder(data.esquerda)],['Roda direita',encoder(data.direita)]],
      [['Barramento',format(value(ina,'tensao_v'),'V',3)],['Corrente',format(value(ina,'corrente_a'),'A',3)],['Tensão no shunt',format(value(ina,'queda_shunt_mv'),'mV',3)],['Temperatura',format(value(ina,'temperatura_c'),'°C')],['Registro de diagnóstico',hex(ina?.diagnostico)]],
      [['Temperatura interna',format(value(data,'temperatura_esp_c'),'°C')]]
    ];
    const highlights=[readings[0][0],readings[1][2],readings[2][0],readings[3][0],readings[4][0]];
    sensorNames.forEach((name,index)=>{
      const active=(data.ativos&(1<<index))!==0;
      const valid=active&&(data.validos&(1<<index))!==0;
      const button=document.querySelector('[data-sensor-id="'+index+'"]');
      const facts=$('sensor-facts-'+index);
      badge('sensor-state-'+index,active?(valid?'Lendo':'Sem leitura'):'Pausado',active?(valid?'good':'warn'):'muted');
      set('sensor-glance-'+index,valid?highlights[index].join(' · '):sensors[index][1]);
      facts.hidden=!valid;
      if(valid)renderDeviceFacts(facts,readings[index]);
      button.dataset.sensorValue=active?'0':'1';
      button.textContent=active?'Pausar':'Retomar';
      button.setAttribute('aria-label',button.textContent+' '+name);
    });
    set('sensor-status','Consultado às '+timeOf(Date.now())+(data.controle_permitido?'':' · Somente leitura com atuação física habilitada'));
    set('sensor-i2c',i2c?number(i2c.erros,0)+' erros':'Sem diagnóstico');
    const busFacts=$('sensor-i2c-facts');
    busFacts.hidden=!i2c;
    if(i2c)renderDeviceFacts(busFacts,[['Operações bem-sucedidas',number(i2c.operacoes_ok,0)],['Erros',number(i2c.erros,0)],['Sondagens bem-sucedidas',number(i2c.sondagens_ok,0)],['Endereços ausentes',number(i2c.enderecos_ausentes,0)],['Último erro',i2c.ultimo_erro===0?'Nenhum':hex(i2c.ultimo_erro)]]);
  }
  const controlGears = ['Nenhuma', 'L', 'M', 'H'];
  function renderControlSensors(control) {
    const selectorsAvailable = control?.partida_concluida === true && control.atuacao_habilitada === true;
    const gear = selectorsAvailable ? controlGears[control.faixa_marcha_fisica] : null;
    const selectedReverse = selectorsAvailable ? control.reversao_solicitada : null;
    const reverseCommand = selectorsAvailable ? control.reversao_acionar_solicitado : null;
    set('sensor-gear-selected', gear ?? 'Não disponível');
    set('sensor-direction-selected', typeof selectedReverse === 'boolean' ? (selectedReverse ? 'Ré' : 'Frente') : 'Não disponível');
    set('sensor-reverse-command', typeof reverseCommand === 'boolean' ? (reverseCommand ? 'Acionado' : 'Inativo') : 'Não disponível');

    const pedal = control?.pedal_disponivel === true ? value(control, 'tensao_pedal_mv', 1000) : null;
    const dacAvailable = control?.dac_disponivel === true;
    const reference = dacAvailable ? value(control, 'tensao_dac_mv', 1000) : null;
    const readings = [];
    if (pedal !== null) readings.push('Pedal ' + format(pedal, 'V', 3));
    if (reference !== null) readings.push('DAC ref. ' + format(reference, 'V', 3));
    set('sensor-throttle-glance', readings.join(' · ') || 'ADC do pedal · MCP4725');
    badge('sensor-throttle-state', ['Sem leitura', 'Leitura parcial', 'Lendo'][readings.length],
          ['muted', 'warn', 'good'][readings.length]);
    set('sensor-pedal-voltage', format(pedal, 'V', 3));
    set('sensor-pedal-position', format(pedal === null ? null : value(control, 'acelerador_lido_decimos_percentual', 10), '%'));
    set('sensor-pedal-allowed', format(value(control, 'acelerador_permitido_decimos_percentual', 10), '%'));
    set('sensor-dac-state', dacAvailable ? 'Disponível' : control?.dac_disponivel === false ? 'Indisponível' : 'Sem leitura');
    set('sensor-dac-reference', format(reference, 'V', 3));
    const zero = dacAvailable ? control.dac_zero_confirmado : null;
    set('sensor-dac-zero', typeof zero === 'boolean' ? zero ? 'Sim' : 'Não' : 'Não disponível');
  }

  function limiterSettingsSummary(values) {
    const limit=values?.[29], throttle=values?.[28], range=values?.[9], margin=values?.[10], samples=values?.[11];
    if(![limit,throttle,range,margin,samples].every(Number.isInteger))return 'Aguardando ajustes do Mestre.';
    if(limit===0||throttle===0)return 'Tração bloqueada por limite em zero.';
    // Reproduz os limites efetivos de LimitadorVelocidade::calcular_teto, em centésimos de km/h.
    const effectiveRange=Math.min(range,Math.max(1,Math.floor(limit/3)));
    const effectiveMargin=Math.min(margin,Math.max(1,Math.floor(effectiveRange/2)));
    const speed=value=>format(Math.max(0,value)/100,'km/h',2);
    return 'Em uso · Redução a partir de '+speed(limit-effectiveRange)+
      ' · Corte em '+speed(limit)+' · Retomada após '+samples+
      (samples===1?' leitura':' leituras')+' a '+speed(limit-effectiveMargin)+' ou menos.';
  }
  function renderDeveloperSettings(data) {
    state.developerUnconfirmed = !Array.isArray(data.valores);
    for (const [id,,,scale] of developerSettings.values()) {
      const disponivel=Number.isInteger(data.valores?.[id]);
      const field=$('developer-value-'+id);
      field.disabled=!disponivel;
      if (disponivel) setField(field.id, data.valores[id]/scale);
      else if(id===36){
        field.value='';
        delete field.dataset.edited;
      }
    }
    set('developer-status', state.developerUnconfirmed ? 'Valores indisponíveis.' :
      data.edicao_permitida ? 'Edição disponível' : 'Desarme o veículo e aguarde o fim da OTA para editar.');
    set('developer-limiter-state',limiterSettingsSummary(data.valores));
  }
  function showTerminalLogOptions(){
    const type=Number($('terminal-log-type').value),source=Number($('terminal-log-source').value);
    const data=state.master.logs_terminal;
    $('terminal-log-source-field').hidden=type!==3;
    const options=type===1?['Restaurar níveis','Apenas avisos NEXUS']:type===2?['Desativadas','Ativadas']:terminalLevels;
    const current=type===1?Number(data?.apenas_avisos_projeto):type===2?Number(data?.cores):type===3?data?.fontes?.find(item=>item.id===source)?.nivel:data?.nivel_global;
    $('terminal-log-value').replaceChildren(...options.map((label,index)=>new Option(label,index,false,index===current)));
  }
  function renderTerminalLogs(data){
    set('terminal-log-status',(terminalLevels[data.nivel_global]??'—')+' · '+(data.apenas_avisos_projeto?'Perfil NEXUS ativo':'Perfil normal')+' · Cores '+(data.cores?'ativas':'desativadas'));
    const rows=(data.fontes||[]).map(source=>{
      const li=document.createElement('li'),name=document.createElement('strong'),level=document.createElement('span');
      name.textContent=terminalSources[source.id]??('Fonte '+source.id);
      level.textContent=terminalLevels[source.nivel_efetivo]??'—';
      li.append(name,level);
      return li;
    });
    $('terminal-log-levels').replaceChildren(...rows);
    showTerminalLogOptions();
  }
  $('terminal-log-source').replaceChildren(...terminalSources.map((name,index)=>new Option(name,index)));
  $('terminal-log-type').addEventListener('change',showTerminalLogOptions);
  $('terminal-log-source').addEventListener('change',showTerminalLogOptions);
  showTerminalLogOptions();
  for(const [panel,button,query,cap,status] of [
    ['sensor-command','refresh-sensors','sensores',CAP_SENSORS,'sensor-status'],
    ['terminal-logs','refresh-terminal-logs','logs_terminal',CAP_TERMINAL_LOGS,'terminal-log-status']
  ]){
    const refresh=async()=>{
      const capabilities=state.master.versao?.capacidades??(await masterQuery('versao'))?.capacidades;
      if(!Number.isFinite(capabilities)){set(status,'Mestre sem resposta.');return;}
      if((capabilities&cap)===0){set(status,'Atualize o firmware do mestre para usar.');return;}
      if(!await masterQuery(query)){
        delete state.master[query];
        set(status,'Sem resposta · dados anteriores.');
        availability();
      }
      if(query==='sensores'&&commandCardOpen('sensor-command')&&!state.stopInFlight&&(capabilities&CAP_CONTROL)!==0)await refreshControlState();
    };
    $(button).addEventListener('click',refresh);
    $(panel).addEventListener('toggle',()=>{if($(panel).open)refresh();});
  }
  const taskStates=['Executando','Pronta','Bloqueada','Suspensa','Excluída'];
  const taskPageSize=5;
  const taskBatchSize=3;
  let taskPage,taskPageLoading=false;
  async function readMasterTaskPage(start){
    // Cada resposta UART traz até três tarefas. A página web usa uma só coleta.
    const readBatch=async offset=>{
      const data=await getJSON('/api/mestre?consulta=tarefas_mestre&inicio='+offset,18000);
      if(!data.valido)throw Error('Coleta de tarefas indisponível.');
      return data;
    };
    for(let attempt=0;attempt<2;attempt++){
      const initialOffset=Math.floor(start/taskBatchSize)*taskBatchSize;
      const first=await readBatch(initialOffset);
      const lastStart=Math.floor(Math.max(0,first.total-1)/taskPageSize)*taskPageSize;
      const pageStart=Math.min(start,lastStart);
      const pageEnd=Math.min(pageStart+taskPageSize,first.total);
      const tasks=[];
      for(let offset=Math.floor(pageStart/taskBatchSize)*taskBatchSize;offset<pageEnd;offset+=taskBatchSize){
        const batch=offset===initialOffset?first:await readBatch(offset);
        if(batch.coletado_em_ms!==first.coletado_em_ms||batch.total!==first.total)break;
        tasks.push(...batch.tarefas.slice(Math.max(0,pageStart-offset),pageEnd-offset));
      }
      if(tasks.length===pageEnd-pageStart)return {inicio:pageStart,total:first.total,tarefas:tasks};
    }
    throw Error('A coleta mudou durante a consulta. Tente novamente.');
  }
  async function loadMasterTasks(start=0){
    if(taskPageLoading)return;
    taskPageLoading=true;
    const first=$('master-task-first'),previous=$('master-task-previous'),next=$('master-task-next');
    first.disabled=previous.disabled=next.disabled=true;
    set('master-task-status','Consultando tarefas…');
    try{
      const capabilities=state.master.versao?.capacidades??(await masterQuery('versao'))?.capacidades;
      if(!Number.isFinite(capabilities))throw Error('Mestre sem resposta.');
      if((capabilities&CAP_MASTER_TASKS)===0)throw Error('Atualize o firmware do mestre para consultar tarefas.');
      const data=await remote(()=>readMasterTaskPage(start));
      const rows=data.tarefas.map(task=>{
        const row=document.createElement('li'),info=document.createElement('div'),name=document.createElement('strong'),detail=document.createElement('p');
        name.textContent=task.nome;
        detail.textContent=[taskStates[task.estado]??'Estado indeterminado','Prioridade '+task.prioridade,Number.isInteger(task.nucleo)&&task.nucleo>=0?'Núcleo '+task.nucleo:'Núcleo livre',task.cpu_valida?'CPU '+number(task.cpu_decimos/10)+'%':'CPU —','Pilha livre '+number(task.pilha_minima_livre_bytes,0)+' B de '+number(task.pilha_reservada_bytes,0)+' B'].join(' · ');
        info.append(name,detail);row.append(info);return row;
      });
      $('master-task-list').replaceChildren(...rows);
      set('master-task-status',rows.length?(data.inicio+1)+'–'+(data.inicio+rows.length)+' de '+data.total+' tarefas':'Nenhuma tarefa.');
      taskPage=data;
    }catch(error){
      const reason=error.name==='AbortError'?'Consulta de tarefas expirou.':error.message;
      set('master-task-status',reason+(taskPage?' Última página consultada mantida.':''));
    }finally{
      taskPageLoading=false;
      first.disabled=false;
      previous.disabled=!taskPage||taskPage.inicio===0;
      next.disabled=!taskPage||taskPage.inicio+taskPage.tarefas.length>=taskPage.total;
    }
  }
  $('master-task-first').addEventListener('click',()=>loadMasterTasks());
  $('master-task-previous').addEventListener('click',()=>loadMasterTasks(Math.max(0,taskPage.inicio-taskPageSize)));
  $('master-task-next').addEventListener('click',()=>loadMasterTasks(taskPage.inicio+taskPageSize));
  $('master-tasks').addEventListener('toggle',()=>{if($('master-tasks').open&&!taskPage)loadMasterTasks();});
  const otaMasterStates=['Desabilitado','Inicializando','Aguardando rede','Verificando','Atualizado','Aguardando autorização','Baixando','Aplicado','Cancelado','Falha','Validando firmware','Em observação','Firmware validado','Falha na validação','Voltou à versão anterior'];
  const otaSatelliteStates=['Desconhecido','Indisponível','Não suportado','Atualizado','Aguardando autorização','Transferindo','Reiniciando','Concluído','Cancelado','Falha','Em observação','Versão em execução','Voltou à versão anterior'];
  const otaTargets=['Nenhum','Todos','Mestre','Equipe','Visitantes'];
  function masterOtaValidationActive(code){return code===10||code===11;}
  function otaTone(code,satellite){
    if(code===9||(satellite?code===12:code===13||code===14))return 'bad';
    if(satellite ? code===3||code===7 : code===4||code===7||code===12)return 'good';
    if(satellite ? code===4||code===5||code===6||code===10 : code===2||code===3||code===5||code===6||masterOtaValidationActive(code))return 'warn';
    return 'muted';
  }
  function renderOtaState(data){
    const detailed=!!(data.mestre&&data.equipe&&data.visitantes);
    const general=otaMasterStates[data.estado]||'Estado desconhecido';
    const target=otaTargets[data.alvo]||'Não informado';
    set('ota-command-state','Estado geral: '+general+' · alvo ativo: '+target+'.'+(Number.isFinite(data.falhas)?' Falhas registradas: '+data.falhas+'.':''));
    const validation=$('ota-master-validation'), masterCode=data.mestre?.estado;
    const notices={
      10:['warning','Mestre validando o novo firmware. O armamento fica bloqueado até o fim da observação.'],
      11:['warning','Firmware confirmado. O Mestre ainda está em observação e mantém a tração desarmada.'],
      12:['success','Validação pós-OTA concluída. O armamento manual já pode ser solicitado quando as condições de segurança forem atendidas.'],
      13:['error','O Mestre falhou ao validar o firmware. A tração continua desarmada.'],
      14:['error','O Mestre voltou à versão anterior após a atualização. Confira a versão em uso.'],
    };
    const notice=notices[masterCode];
    validation.hidden=!notice;
    if(notice){validation.dataset.tone=notice[0];set('ota-master-validation',notice[1]);}
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
      const measure=document.createElement('small');
      measure.textContent=!item.satellite&&masterOtaValidationActive(code)
        ? number(validPercent,0)+'% desta etapa'
        : Number.isFinite(percent)&&(validPercent>0||code===(item.satellite?5:6)||code===7)?number(validPercent,1)+'%':'Sem transferência ativa';
      head.append(name,status);row.append(head,versions,progress,measure);root.append(row);
    });
    const checked=Date.now();
    set('ota-state-checked','Consultado às '+timeOf(checked)+(Number.isFinite(data.solicitacao)?' · solicitação #'+data.solicitacao:'')+(Number.isFinite(data.duracao_ms)?' · resposta em '+data.duracao_ms+' ms':''));
    const run=state.commandRuns.find(item=>item.followOta&&item.status!=='Falhou'&&item.status!=='Sem resposta');
    if(run&&run.responses.length){
      const progressText=items.map(item=>{const current=(item.satellite?otaSatelliteStates:otaMasterStates)[item.value.estado]||'desconhecido';return item.name+': '+current+(Number(item.value.progresso_percentual)>0?' '+number(item.value.progresso_percentual,1)+'%':'');}).join(' · ');
      const active=masterOtaValidationActive(masterCode)||[2,3,6].includes(Number(data.estado))||items.some(item=>item.satellite&&[5,6,10].includes(Number(item.value.estado)));
      const failed=masterCode===13||masterCode===14||Number(data.estado)===9||items.some(item=>Number(item.value.estado)===9||(item.satellite&&Number(item.value.estado)===12));
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
    if(consult==='sensores')renderSensors(data);
    if(consult==='logs_terminal')renderTerminalLogs(data);
    if(consult==='ajustes_desenvolvedor')renderDeveloperSettings(data);
    for(const radio of Object.values(radioSettings))if(consult===radio.query)renderRadioSettings(radio,data);
    if(consult==='versao'){
      availability();
      if(!developerQueryAvailable(data.capacidades))set('developer-status','Recurso indisponível neste firmware.');
      if(Number.isFinite(data.capacidades)&&(data.capacidades&CAP_SYNC)===0){badge('sync-badge','Não suportado','muted');set('sync-summary','O firmware atual do mestre não informa o andamento da sincronização. Atualize o mestre para habilitar esse acompanhamento.');set('sync-checked','Consulta detalhada indisponível neste mestre.');}
    }
    if(consult==='configuracao'){
      set('report-state',data.relatorios_pausados?'Pausados':'Ativos');
      document.querySelector('[data-master-action="alternar_relatorios"]').textContent=data.relatorios_pausados?'Retomar relatórios':'Pausar relatórios';
      const group=Number($('report-group').value);
      if(Array.isArray(data.intervalos_relatorios_ms))setField('report-interval',data.intervalos_relatorios_ms[group]);
      setField('ota-interval',data.intervalo_ota_minutos);
      setField('telemetry-ota-interval',data.intervalo_telemetria_ota_ms);
      setField('wifi-auto',data.wifi_automatico?'1':'0');
      set('wifi-primary-current',data.rede_principal?'Principal atual: '+data.rede_principal+(data.principal_alterada?' · definida pelo usuário':' · padrão do firmware'):'Nenhuma rede principal configurada.');
      setField('wifi-attempts',data.tentativas_wifi);
      setField('wifi-attempt-time',data.tempo_tentativa_wifi_ms/1000);
      setField('wifi-retry-interval',data.intervalo_tentativas_wifi_ms/1000);
      setField('wifi-session-limit',data.tempo_limite_wifi_ms/1000);
    }
    if(consult==='ota')renderOtaState(data);
    if(consult==='sincronizacao')renderSyncState(data);
    if(consult==='controle_remoto'){
      state.controlUnconfirmed=false;
      renderControlSensors(data);
      const isolated=isolatedDacAvailable(data);
      const armed=(data.atuacao_habilitada===true||isolated)&&!needsArming(data)&&!data.corte_por_rele_solicitado&&!data.ota_em_andamento&&data.partida_concluida!==false;
      const controlStatus=data.ota_em_andamento?'Atualização ou validação de firmware':data.partida_concluida===false?'Inicializando Mestre':data.parada_solicitada?'Parada ativa':data.armamento_inicial_pendente?'Carro desarmado':armed?(isolated?'DAC armado':'Carro armado'):'Controle bloqueado';
      controlNotice(controlStatus, data.parada_solicitada?'emergency':armed?'info':'warning');
      const actuationLabel=(data.dac_disponivel?'DAC disponível':'DAC indisponível')+' · '+(data.atuacao_habilitada?'Tração disponível':'Tração desabilitada');
      badge('actuation-status',actuationLabel,data.atuacao_habilitada?'good':'muted');
      const armLabel=armed&&data.estado_operacao===5?'Limitando velocidade':controlStatus;
      badge('vehicle-arm-badge',armLabel,armed?'good':data.parada_solicitada||data.ota_em_andamento?'bad':'warn');
      set('remote-gear-physical',controlGears[data.faixa_marcha_fisica]||'Desconhecida');
      set('remote-gear-current',data.corte_por_rele_solicitado?'Bloqueada':controlGears[data.faixa_marcha_solicitada]||'Desconhecida');
      set('remote-gear-limit-current',controlGears[data.faixa_marcha_maxima]||'Desconhecida');
      set('remote-reverse-current',data.reversao_solicitada?'Ré':'Frente');
      set('remote-speed-current',format(value(data,'velocidade_maxima_centesimos_kmh',100),'km/h',2));
      set('remote-throttle-max-current',format(value(data,'acelerador_maximo_decimos_percentual',10),'%',1));
      for (const [id,scale,,key] of Object.values(controlFields)) {
        const reading=value(data,key,scale);
        if(reading!==null)setField(id,reading);
      }
      set('remote-stop-current',data.parada_solicitada?'Ativa'+(data.parada_restante_ms?' · '+Math.ceil(data.parada_restante_ms/1000)+' s':''):'Inativa'+(data.corte_por_rele_solicitado?' · corte mantido':''));
      document.querySelector('.remote-stop-reading')?.classList.toggle('is-stopped',!!data.parada_solicitada);
      document.querySelectorAll('[data-master-action="limite_marcha"]').forEach(button=>
        button.setAttribute('aria-pressed',String(Number(button.dataset.controlValue)===data.faixa_marcha_maxima)));
      set('remote-checked','Estado consultado às '+timeOf(Date.now())+' · revisão '+data.revisao);
    }
    if(consult==='simulador'){
      simulatorSourceButtons.forEach(button=>button.setAttribute('aria-pressed',String((button.dataset.sourceValue==='1')===data.ativo)));
      badge('simulator-badge',data.ativo?'Simulação ativa':'Sensores reais',data.ativo?'warn':'good');
      set('simulator-state',(data.ativo?'Telemetria simulada ativa':'Telemetria real ativa')+' · padrão do firmware: '+(data.padrao_firmware?'simulação':'sensores reais')+' · '+(Number(data.amostras_geradas)||0)+' amostras simuladas nesta inicialização.');
      set('simulator-checked','Estado confirmado no mestre às '+new Date().toLocaleTimeString('pt-BR')+'. A alteração via web é temporária e se perde ao reiniciar.');
      observeOperationalRun(consult,data);
    }
    if(consult==='wifi')set('wifi-command-state',data.conectado?'Rede externa ativa: '+(data.rede||'rede não identificada')+' · sinal '+data.rssi_dbm+' dBm':data.radio_ativo?'Wi-Fi externo ligado; ainda sem conexão confirmada.':'Wi-Fi externo desligado. Isso é normal fora das sessões de OTA e Google.');
    if(consult==='wifi'||consult==='configuracao')observeOperationalRun(consult,data);
  }
  async function loadSavedNetworks(count){
    while(state.polls.savedNetworks){
      if(state.polls.savedNetworks.count===count)return state.polls.savedNetworks.promise;
      await state.polls.savedNetworks.promise;
    }
    const work=(async()=>{
    const select=$('wifi-saved'),selected=select.value,networks=[];
      for(let index=1;index<=Math.min(5,Number(count)||0);index++){
        try{const data=await remote(()=>getJSON('/api/mestre?consulta=rede_wifi_salva&indice='+index,18000));networks.push({index,ssid:data.ssid});}catch(_){/* A posição fica indisponível. */}
      }
      select.replaceChildren();
      if(!networks.length){const option=document.createElement('option');option.value='';option.textContent='Nenhuma rede salva informada';select.append(option);}
      else networks.forEach(({index,ssid})=>{const option=document.createElement('option');option.value=String(index);option.textContent=index+' · '+ssid;select.append(option);});
      if(networks.some(item=>String(item.index)===selected))select.value=selected;
      return networks;
    })();
    state.polls.savedNetworks={count,promise:work};
    try{return await work;}finally{if(state.polls.savedNetworks?.promise===work)state.polls.savedNetworks=null;}
  }
  document.querySelectorAll('#command-center input,#command-center select,#remote-control input').forEach(field=>field.addEventListener('input',()=>{
    if(field.id==='developer-permission')return;
    field.dataset.edited='1';
    if(field.closest('#developer-settings'))availability();
  }));
  $('wifi-primary-source').addEventListener('change',event=>set('wifi-primary-help',event.target.value==='saved'?'Escolha uma opção em Redes salvas.':'Preencha os dados em Nova rede.'));
  $('report-group').addEventListener('change',()=>{const config=state.master.configuracao;if(config?.intervalos_relatorios_ms){$('report-interval').dataset.edited='';setField('report-interval',config.intervalos_relatorios_ms[Number($('report-group').value)]);}});
  function bytesLength(text){return new TextEncoder().encode(text).length;}
  function numberField(id){
    const input=$(id),raw=input.value.trim(),n=Number(raw),min=Number(input.min),max=Number(input.max);
    if(!raw||!Number.isInteger(n)||n<min||n>max)throw new Error('Confira '+(input.closest('label')?.textContent.trim()||id)+' ('+min+' a '+max+').');
    return String(n);
  }
  function wifiSecondsField(id){
    const input=$(id),raw=input.value.trim(),seconds=Number(raw),min=Number(input.min),max=Number(input.max);
    if(!raw||!Number.isFinite(seconds)||seconds<min||seconds>max)throw new Error('Confira '+(input.closest('label')?.textContent.trim()||id)+' ('+min+' a '+max+' s).');
    return String(Math.round(seconds*1000));
  }
  function permission(id='wifi-permission'){const field=$(id),secret=field.value;if(!secret||/[\r\n]/.test(secret)||bytesLength(secret)>=108){field.closest('details').open=true;field.focus();throw new Error('Informe a senha de permissão.');}return secret;}
  function preparedNetwork(){const ssid=$('wifi-ssid').value,password=$('wifi-password').value,length=bytesLength(password);if(!ssid||bytesLength(ssid)>32||(length!==0&&length<8)||length>63||/[\r\n]/.test(ssid)||/[\r\n]/.test(password))throw new Error('Informe SSID de até 32 bytes; deixe a senha vazia para rede aberta ou use 8 a 63 bytes.');return ssid+'\n'+password;}
  function savedIndex(){const index=$('wifi-saved').value;if(!/^[1-5]$/.test(index))throw new Error('Selecione uma rede salva informada pelo mestre.');return index;}
  function developerDrafts() {
    const current=state.master.ajustes_desenvolvedor?.valores;
    if(!Array.isArray(current))throw Error('Aguarde a leitura dos ajustes do Mestre.');
    const drafts=[];
    for(const [id,label,,scale,min,max] of developerSettings.values()){
      const field=$('developer-value-'+id);
      if(!field.dataset.edited)continue;
      const raw=field.value.trim(),scaled=Number(raw)*scale,value=Math.round(scaled);
      if(!raw||!Number.isFinite(scaled)||value<min||value>max||Math.abs(scaled-value)>1e-6){
        const group=developerGroups.findIndex(([,rows])=>rows.some(([setting])=>setting===id));
        showDeveloperGroup(group);
        field.focus();
        throw Error('Confira o valor de “'+label+'”.');
      }
      if(value!==current[id])drafts.push({index:id,id:field.id,raw,value});
      else delete field.dataset.edited;
    }
    if(!drafts.length){availability();throw Error('Nenhum valor foi alterado.');}

    // O Mestre valida cada gravação. Aplique os limites em uma ordem que mantenha
    // a calibração consistente enquanto o lote é enviado.
    const valid=values=>{
      const [start,end,released,margin,,dacMin,dacMax]=values;
      const batteries=values[31],empty=values[32],full=values[33];
      const packValid=batteries===undefined||(empty<full&&batteries*full<=65535);
      return margin<=start&&start<end&&released<=end&&released>start-margin&&dacMin<dacMax&&packValid;
    };
    const final=current.slice();
    for(const draft of drafts)final[draft.index]=draft.value;
    if(!valid(final))throw Error('Confira os limites do pedal, do DAC e da bateria.');
    const ordered=[],next=current.slice();
    while(drafts.length){
      const candidate=drafts.findIndex(draft=>{
        const trial=next.slice();
        trial[draft.index]=draft.value;
        return valid(trial);
      });
      if(candidate<0)throw Error('Esses limites precisam de um valor intermediário válido. Ajuste os valores em etapas.');
      const [draft]=drafts.splice(candidate,1);
      next[draft.index]=draft.value;
      ordered.push(draft);
    }
    return ordered;
  }
  function buildPlan(action,button){
    const name=action==='permitir_re'?(button.dataset.controlValue==='1'?'Permitir ré':'Bloquear ré'):actionNames[action];
    if(!name)throw new Error('Comando desconhecido.');
    const plan={name,action,steps:[{code:action,body:''}],refresh:[],secret:false};
    if(['verificar_ota','autorizar_mestre','autorizar_equipe','autorizar_visitantes','autorizar_todos','cancelar_ota'].includes(action))plan.refresh=['ota'];
    if(action==='alternar_relatorios')plan.refresh=['configuracao'];
    if(action==='zerar_percurso')plan.note='O percurso atualizado aparece na página Telemetria; a API não relaciona essa leitura ao comando.';
    if(action==='resumo_terminal')plan.note='Resumo solicitado no serial do mestre.';
    if(action==='sensor_estado'){
      const id=Number(button.dataset.sensorId),enabled=button.dataset.sensorValue==='1';
      plan.name=(enabled?'Retomar ':'Pausar ')+(id===255?'todos os sensores':sensorNames[id]);
      plan.steps=[{code:'sensores',body:id+'\n'+Number(enabled)}];
      plan.refresh=['sensores'];plan.expectedSensor={mask:id===255?(1<<sensorNames.length)-1:1<<id,enabled};
    }
    if(action==='logs_terminal'){
      const type=Number($('terminal-log-type').value),source=type===3?Number($('terminal-log-source').value):0,level=Number($('terminal-log-value').value);
      plan.steps[0].body=[type,level,source,permission('terminal-log-permission')].join('\n');
      plan.refresh=['logs_terminal'];plan.expectedLog={type,source,level};
    }
    if(action==='ajuste_desenvolvedor'){
      plan.developerDrafts=developerDrafts();
      const password=permission('developer-permission');
      plan.steps=plan.developerDrafts.map(({index,value})=>({code:action,body:[index,value,password].join('\n')}));
      plan.prompt=plan.steps.length===1?'Gravar 1 alteração no Mestre?':'Gravar '+plan.steps.length+' alterações no Mestre?';
      plan.refresh=['ajustes_desenvolvedor'];
      plan.secret=true;
    }
    if(action==='salvar_radio_satelite'){
      const target=button.dataset.radioTarget,radio=radioSettings[target];
      const invalid=radio.fields.find(field=>!field.checkValidity());
      if(invalid){invalid.reportValidity();throw Error('Confira os valores do Wi-Fi.');}
      const [channel,band,power,clients]=radio.fields.map(field=>Number(field.value));
      if(power!==0&&power<2)throw Error('Use 0 para o padrão ou uma potência a partir de 2 dBm.');
      const profile=[channel,band,power*4,clients];
      plan.name=target==='equipe'?'Salvar Wi-Fi da Equipe':'Salvar Wi-Fi de Visitantes';
      plan.prompt='O perfil será aplicado na próxima vez que esse satélite iniciar.';
      plan.radio={target,profile};
      plan.steps=[{code:radio.query,body:[...profile,permission('developer-permission')].join('\n')}];
      plan.secret=true;
    }
    if(action==='sincronizar_google')plan.refresh=['sincronizacao'];
    if(action==='permitir_re'){
      const enabled=button.dataset.controlValue==='1';
      plan.steps[0].body=String(Number(enabled));
      plan.refresh=['controle_remoto'];
      plan.expected={field:'reversao_permitida',value:enabled};
      plan.prompt=enabled?'A ré ficará disponível para seleção.':'Se houver uma ré solicitada, o Mestre vai desarmar a tração.';
    }
    if(['limite_marcha','velocidade_maxima','limite_acelerador','parada_remota','liberar_parada_remota'].includes(action)){
      let value,field;
      if(controlFields[action]){
        const [id,scale,max,key]=controlFields[action],raw=$(id).value.trim(),n=Number(raw);
        if(!raw||!Number.isFinite(n)||n<0||n>max||Math.abs(n*scale-Math.round(n*scale))>1e-6)throw Error('Confira a faixa e a precisão do valor informado.');
        value=Math.round(n*scale);
        field=key;
        plan.input={id,raw};
      }else if(action==='limite_marcha'){
        value=Number(button.dataset.controlValue);
        if(!Number.isInteger(value)||value<1||value>3)throw Error('Selecione uma marcha válida.');
        field='faixa_marcha_maxima';
      }else{
        value=action==='parada_remota'?1:isolatedDacAvailable()?ARM_ISOLATED_DAC:0;
        field=action==='liberar_parada_remota'&&state.master.controle_remoto?.armamento_inicial_pendente
          ? 'armamento_inicial_pendente' : 'parada_solicitada';
        plan.steps[0].code='parada_remota';
        if(action==='liberar_parada_remota')plan.prompt='Confirme que o veículo está parado e o pedal do acelerador está solto.';
      }
      plan.steps[0].body=String(value);plan.refresh=['controle_remoto'];
      plan.expected={field,value:action==='liberar_parada_remota'?false:action==='parada_remota'?true:value};
      plan.note='O painel consulta o estado após enviar.';
    }
    if(action==='simulador_telemetria'){
      const enabled=button.dataset.sourceValue==='1';
      plan.name=enabled?'Ativar telemetria simulada':'Usar sensores reais';
      plan.steps[0].body=String(Number(enabled));
      plan.refresh=['simulador'];
      plan.expected={field:'ativo',value:enabled,label:'Fonte da telemetria'};
    }
    if(action==='intervalo_relatorio'){const group=Number($('report-group').value),interval=Number(numberField('report-interval'));plan.steps[0].body=group+'\n'+interval;plan.refresh=['configuracao'];plan.expected={field:'intervalos_relatorios_ms',index:group,value:interval,label:'Intervalo do relatório'};}
    if(action==='intervalo_ota'){const interval=Number(numberField('ota-interval'));plan.steps[0].body=String(interval);plan.refresh=['configuracao'];plan.expected={field:'intervalo_ota_minutos',value:interval,label:'Verificação OTA'};}
    if(action==='intervalo_telemetria_ota'){const interval=Number(numberField('telemetry-ota-interval'));plan.steps[0].body=String(interval);plan.refresh=['configuracao'];plan.expected={field:'intervalo_telemetria_ota_ms',value:interval,label:'Telemetria durante OTA'};}
    if(action==='wifi_test'){plan.expectedSsid=$('wifi-ssid').value;plan.steps=[{code:'wifi_preparar',body:preparedNetwork()},{code:'wifi_conectar_preparada',body:''}];plan.saveNetwork=$('wifi-save').checked;if(plan.saveNetwork)plan.steps.push({code:'wifi_salvar_preparada',body:''});plan.secret=true;plan.refresh=['wifi','configuracao'];}
    if(action==='wifi_primary'){
      if($('wifi-primary-source').value==='saved'){
        const index=savedIndex();plan.expectedSsid=($('wifi-saved').selectedOptions[0]?.textContent||'').split(' · ').slice(1).join(' · ');
        plan.steps=[{code:'wifi_principal_salva',body:index+'\n'+permission()}];
      }else{
        plan.expectedSsid=$('wifi-ssid').value;
        plan.steps=[{code:'wifi_preparar',body:preparedNetwork()},{code:'wifi_alterar_principal',body:permission()}];
      }
      plan.secret=true;plan.refresh=['configuracao'];
    }
    if(action==='wifi_conectar_salva'||action==='wifi_excluir_salva'){plan.steps[0].body=savedIndex();plan.refresh=['wifi','configuracao'];plan.expectedSsid=($('wifi-saved').selectedOptions[0]?.textContent||'').split(' · ').slice(1).join(' · ');}
    if(action==='wifi_restaurar_principal'){plan.steps[0].body=permission();plan.secret=true;plan.refresh=['configuracao'];}
    if(action==='wifi_automatico'){const enabled=$('wifi-auto').value==='1';plan.steps[0].body=(enabled?'1':'0')+'\n'+permission();plan.secret=true;plan.refresh=['configuracao'];plan.expected={field:'wifi_automatico',value:enabled,label:'Conexão automática'};}
    if(action==='wifi_politica'){const values=[numberField('wifi-attempts'),wifiSecondsField('wifi-attempt-time'),wifiSecondsField('wifi-retry-interval'),wifiSecondsField('wifi-session-limit')];plan.steps[0].body=values.join('\n')+'\n'+permission();plan.expectedPolicy=values.map(Number);plan.secret=true;plan.refresh=['configuracao'];}
    if(action==='wifi_restaurar_politica'){plan.steps[0].body=permission();plan.secret=true;plan.refresh=['configuracao'];}
    return plan;
  }
  function observedConfiguration(plan,config){
    if(plan.expectedPolicy){const keys=['tentativas_wifi','tempo_tentativa_wifi_ms','intervalo_tentativas_wifi_ms','tempo_limite_wifi_ms'];const matches=keys.every((key,index)=>Number(config[key])===plan.expectedPolicy[index]);return {matches,text:matches?'Política Wi-Fi confirmada nos quatro parâmetros do mestre.':'A política consultada difere dos valores solicitados; confira tentativas e tempos.'};}
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
      if(['wifi_test','wifi_conectar_salva','wifi_excluir_salva','wifi_primary','wifi_restaurar_principal'].includes(run.action))return;
      const observed=observedConfiguration(run,data);
      if(observed)updateRun(run,observed.matches?'Valor confirmado':'Aguardando valor',observed.text+' Leitura atual do mestre.',observed.matches?'good':'warn');
      else if(run.action==='alternar_relatorios')updateRun(run,'Estado consultado','Relatórios no mestre: '+(data.relatorios_pausados?'pausados':'ativos')+'.','good');
      else if(run.action==='wifi_excluir_salva')updateRun(run,'Redes consultadas','O mestre informa '+data.quantidade_redes_wifi+' rede(s) salva(s). Confira a lista atualizada antes de considerar a exclusão concluída.','warn');
      else if(run.action==='wifi_politica')updateRun(run,'Política consultada','Política atual: '+data.tentativas_wifi+' tentativa(s), '+age(data.tempo_tentativa_wifi_ms)+' por tentativa, intervalo de '+age(data.intervalo_tentativas_wifi_ms)+'.','good');
      else if(!run.refreshQueries.includes('wifi'))updateRun(run,'Estado consultado','Configuração atual do mestre consultada. Os valores do formulário refletem a última leitura.','good');
    }else if(consult==='simulador'){
      const enabled=!!data.ativo,matches=enabled===run.expected?.value;
      updateRun(run,matches?'Estado confirmado':'Aguardando estado','O mestre informa telemetria '+(enabled?'simulada':'real')+'. Padrão após reiniciar: '+(data.padrao_firmware?'simulada':'real')+'.',matches?'good':'warn');
    }else if(consult==='wifi'){
      if(run.action.startsWith('wifi_'))return;
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
  async function sendStopNow(){
    if(state.stopInFlight)return;
    state.stopInFlight=true;availability();
    const run={name:'Parada de emergência',action:'parada_remota',time:Date.now(),requests:[],responses:[],step:1,status:'Enviando',text:'Comando enviado ao mestre; aguardando resposta.',tone:'warn',checkedAt:0,hasRefresh:true,refreshQueries:['controle_remoto'],followUntil:Date.now()+30000};
    state.commandRuns.unshift(run);state.commandRuns.length=Math.min(state.commandRuns.length,12);renderMasterCommandHistory();
    message('Enviando parada…');
    const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),20000);
    try{
      const response=await fetch('/api/mestre?consulta=parada_emergencia',{method:'POST',headers:{'Content-Type':'text/plain; charset=utf-8'},body:'1',cache:'no-store',signal:controller.signal});
      const result=await response.json();
      if(!response.ok||!result.ok)throw Error(result.erro||'Pedido recusado');
      if(Number.isFinite(result.solicitacao))run.requests.push(result.solicitacao);
      run.responses.push({duracao_ms:result.duracao_ms,tentativas:result.tentativas});
      updateRun(run,'Resposta recebida','Consultando a parada no mestre.','warn');
      message('Parada recebida. Consultando estado…');
      try{
        const current=await getJSON('/api/mestre?consulta=controle_remoto',7000);
        state.master.controle_remoto=current;updateMasterUI('controle_remoto',current);run.checkedAt=Date.now();
        const stopped=current.parada_solicitada;
        updateRun(run,stopped?'Parada ativa':'Estado divergente',stopped?'Acelerador lógico em zero. Confira a parada física.':'Parada sem confirmação.',stopped?'good':'warn');
        message(stopped?'Acelerador lógico em zero. Confira a parada física.':'O estado consultado difere do comando.',stopped?'emergency':'error');
      }catch(_){updateRun(run,'Estado não confirmado','O mestre respondeu, mas a consulta posterior não confirmou o estado.','warn');message('Parada recebida. Estado sem confirmação.','warning');}
    }catch(error){updateRun(run,error.name==='AbortError'?'Sem resposta':'Falhou',error.name==='AbortError'?'Prazo de resposta esgotado. Estado da parada desconhecido.':error.message,'bad');message('Parada sem confirmação: '+(error.name==='AbortError'?'tempo esgotado':error.message),'error');}
    finally{clearTimeout(timer);state.stopInFlight=false;availability();}
  }
  async function postMaster(code,body,run){
    commandEvent('MESTRE',code,'Enviando');
    const timeout=['wifi_conectar_preparada','wifi_conectar_salva','wifi_alterar_principal','wifi_principal_salva'].includes(code)?200000:20000;
    try{const result=await remote(async()=>{const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),timeout);try{const response=await fetch('/api/mestre?consulta='+encodeURIComponent(code),{method:'POST',headers:{'Content-Type':'text/plain; charset=utf-8'},body,cache:'no-store',signal:controller.signal});const data=await response.json();if(!response.ok||!data.ok){const error=new Error(data.erro||'Comando recusado');error.requestId=data.solicitacao;throw error;}return data;}finally{clearTimeout(timer);}});if(Number.isFinite(result.solicitacao))run.requests.push(result.solicitacao);if(run.followSync&&Number.isFinite(result.sincronizacao_id))run.syncGeneration=result.sincronizacao_id;run.responses.push({duracao_ms:result.duracao_ms,tentativas:result.tentativas});run.responseAt=Date.now();commandEvent('MESTRE',code,'Recebido',result);return result;}
    catch(error){if(Number.isFinite(error.requestId))run.requests.push(error.requestId);commandEvent('MESTRE',code,error.name==='AbortError'?'Tempo esgotado':'Falhou',null,error.message);throw error;}
  }
  function clearConfirmedDraft(input) {
    if(!input)return;
    const field=$(input.id);
    if(field.value.trim()===input.raw)delete field.dataset.edited;
  }
  function confirmDeveloperDrafts(drafts,data) {
    const confirmed=drafts.filter(draft=>data?.valores?.[draft.index]===draft.value);
    for(const draft of confirmed)clearConfirmedDraft(draft);
    return confirmed.length;
  }
  async function executePlan(plan){
    const run={name:plan.name,action:plan.action,time:Date.now(),requests:[],responses:[],step:0,status:'Preparando',text:'Preparando o envio ao mestre.',tone:'muted',checkedAt:0,hasRefresh:plan.refresh.length>0||plan.action==='zerar_percurso',refreshQueries:plan.refresh,expected:plan.expected,expectedSsid:plan.expectedSsid,followUntil:Date.now()+300000,followOta:plan.refresh.includes('ota'),followSync:plan.refresh.includes('sincronizacao'),syncGeneration:null};
    state.commandRuns.unshift(run);state.commandRuns.length=Math.min(state.commandRuns.length,12);renderMasterCommandHistory();
    state.busy=true;availability();
    message(plan.name+'…');
    try{
      let response;
      for(let index=0;index<plan.steps.length;index++){
        run.step=index+1;
        updateRun(run,'Enviando','Enviando etapa '+(index+1)+' de '+plan.steps.length+' ao mestre.','warn');
        response=await postMaster(plan.steps[index].code,plan.steps[index].body,run);
        updateRun(run,'Resposta recebida','O mestre respondeu à etapa '+(index+1)+' de '+plan.steps.length+'. A execução posterior ainda pode estar em andamento.','warn');
      }
      if(plan.radio){
        const {target,profile}=plan.radio,radio=radioSettings[target];
        if(!profile.every((value,index)=>response.salvo?.[index]===value))
          throw Error('O satélite informou um perfil diferente do solicitado');
        for(const field of radio.fields)delete field.dataset.edited;
        state.master[radio.query]=response;
        renderRadioSettings(radio,response);
        const detail=response.reinicio_pendente?'Wi-Fi salvo. Reinicie o satélite para aplicar.':'Perfil Wi-Fi confirmado.';
        updateRun(run,'Wi-Fi salvo',detail,'good');
        message(detail,'success');
        return;
      }
      run.followUntil=Date.now()+120000;
      if(plan.action==='wifi_restaurar_politica')for(const id of ['wifi-attempts','wifi-attempt-time','wifi-retry-interval','wifi-session-limit'])delete $(id).dataset.edited;
      if(plan.action==='logs_terminal')$('terminal-log-permission').value='';
      let checked=0;const observed={};
      for(const query of plan.refresh){const result=await masterQuery(query);if(result){checked++;observed[query]=result;run.checkedAt=Date.now();}if(query==='configuracao'&&result)observed.savedNetworks=await loadSavedNetworks(result.quantidade_redes_wifi);}
      if(plan.action==='wifi_test'||plan.action==='wifi_conectar_salva'){
        const saved=plan.saveNetwork&&observed.savedNetworks?.some(network=>network.ssid===plan.expectedSsid);
        const detail=plan.action==='wifi_conectar_salva'?' Rede já estava na lista salva.':plan.saveNetwork?(saved?' Rede encontrada na lista salva do mestre.':' O mestre aceitou o salvamento, mas a lista não confirmou a rede; atualize o estado para conferir.'):' Rede não salva, conforme solicitado.';
        updateRun(run,saved?'Rede salva e confirmada':plan.saveNetwork?'Salvamento sem confirmação':'Conexão validada','O mestre confirmou o teste de conexão com '+plan.expectedSsid+'. O rádio foi desligado após o teste.'+detail,saved||!plan.saveNetwork?'good':'warn');
        message(plan.name+': conexão validada.'+(saved?' Rede salva e confirmada.':plan.saveNetwork?' Confira a lista de redes salvas.':''),saved||!plan.saveNetwork?'success':'');
        if(plan.action==='wifi_test')$('wifi-password').value='';
        return;
      }
      if(plan.action==='wifi_excluir_salva'){
        const removed=observed.savedNetworks?.every(network=>network.ssid!==plan.expectedSsid);
        updateRun(run,removed?'Exclusão confirmada':'Exclusão sem confirmação',removed?'A rede '+plan.expectedSsid+' não aparece mais na lista salva do mestre.':'O mestre aceitou a exclusão, mas a lista atual não confirmou a remoção.',removed?'good':'warn');
        message(plan.name+': '+(removed?'rede removida da lista.':'confira a lista de redes salvas.'),removed?'success':'');
        return;
      }
      if(plan.action==='wifi_primary'||plan.action==='wifi_restaurar_principal'){
        const config=observed.configuracao,matched=config&&(plan.action==='wifi_primary'?config.principal_alterada&&config.rede_principal===plan.expectedSsid:!config.principal_alterada);
        updateRun(run,matched?'Rede principal confirmada':'Alteração aceita',matched?'O mestre informa '+config.rede_principal+' como principal. A conexão foi testada antes da gravação; o rádio voltou ao repouso.':'O mestre aceitou a alteração, mas a consulta posterior não confirmou a rede principal. Atualize o estado para conferir.',matched?'good':'warn');
        message(plan.name+': '+(matched?'rede principal confirmada.':'confira o estado da rede principal.'),matched?'success':'error');
        $('wifi-password').value='';$('wifi-permission').value='';
        return;
      }
      if(plan.refresh.length&&!checked){
        const unsupported=run.followSync&&Number.isFinite(state.master.versao?.capacidades)&&(state.master.versao.capacidades&CAP_SYNC)===0;
        updateRun(run,unsupported?'Sem acompanhamento':'Acompanhar estado',unsupported?'O mestre aceitou a ação, mas seu firmware não oferece o andamento detalhado. Atualize o mestre para acompanhar esta sincronização.':'O mestre respondeu ao comando, mas a consulta do estado não retornou. A atualização automática tentará novamente.','warn');
        message('Comando recebido. Estado sem confirmação.','warning');
      }
      else if(!run.followSync&&!run.followOta){
        const configObserved=observed.configuracao?observedConfiguration(plan,observed.configuracao):null;
        if(configObserved){
          updateRun(run,configObserved.matches?'Valor observado':'Valor divergente',configObserved.text+' A resposta inicial confirma o aceite; esta leitura mostra a configuração atual.',configObserved.matches?'good':'warn');
          message(plan.name+': '+(configObserved.matches?'configuração confirmada no mestre.':'comando recebido, mas o valor consultado difere.'),configObserved.matches?'success':'error');
        }else if(observed.controle_remoto){
          const matches=observed.controle_remoto[plan.expected.field]===plan.expected.value;
          if(matches)clearConfirmedDraft(plan.input);
          updateRun(run,matches?'Comando confirmado':'Estado divergente',matches?'Valor confirmado no mestre.':'O mestre informou outro estado.',matches?'good':'warn');
          if(matches&&plan.action==='liberar_parada_remota')document.querySelectorAll('.global-toast.emergency').forEach(toast=>toast.remove());
          message(plan.name+': '+(matches?'estado confirmado no mestre.':'o mestre informou outro estado.'),matches?'success':'error');
        }else if(observed.ajustes_desenvolvedor){
          const saved=confirmDeveloperDrafts(plan.developerDrafts,observed.ajustes_desenvolvedor);
          const matches=saved===plan.developerDrafts.length;
          const detail=matches?'Alterações salvas no Mestre.':saved+' de '+plan.developerDrafts.length+' alterações confirmadas. As demais continuam no formulário.';
          updateRun(run,matches?'Ajustes salvos':'Confirmação parcial',detail,matches?'good':'warn');
          message(detail,matches?'success':'warning');
        }else if(observed.simulador){
          const matches=observed.simulador.ativo===plan.expected?.value;
          updateRun(run,matches?'Estado confirmado':'Estado divergente','O mestre informa telemetria '+(observed.simulador.ativo?'simulada':'real')+'. O padrão após reiniciar é '+(observed.simulador.padrao_firmware?'simulado':'real')+'.',matches?'good':'warn');
          message(plan.name+': '+(matches?'estado confirmado no mestre.':'o mestre informou outro estado.'),matches?'success':'error');
        }else if(observed.sensores||observed.logs_terminal){
          const sensor=plan.expectedSensor,log=plan.expectedLog,data=observed.logs_terminal;
          const actualLog=log&&data?[data.nivel_global,Number(data.apenas_avisos_projeto),Number(data.cores),data.fontes?.find(item=>item.id===log.source)?.nivel][log.type]:null;
          const matches=sensor?(observed.sensores.ativos&sensor.mask)===(sensor.enabled?sensor.mask:0):actualLog===log.level;
          const detail=matches?'Estado confirmado no mestre.':'O mestre informou outro estado.';
          updateRun(run,matches?'Confirmado':'Estado divergente',detail,matches?'good':'warn');
          message(detail,matches?'success':'warning');
        }else if(plan.action==='wifi_restaurar_politica'&&observed.configuracao){
          updateRun(run,'Política consultada','Valores atualizados no formulário.', 'good');
          message('Política padrão restaurada.','success');
        }else if(plan.steps[0].code==='alternar_relatorios'&&observed.configuracao){
          updateRun(run,'Estado consultado','Relatórios no mestre: '+(observed.configuracao.relatorios_pausados?'pausados':'ativos')+'. A consulta mostra o estado atual após a solicitação.','good');
          message('Relatórios '+(observed.configuracao.relatorios_pausados?'pausados':'ativos')+' no mestre.','success');
        }else if(observed.wifi){
          updateRun(run,'Wi-Fi consultado',observed.wifi.conectado?'Mestre conectado a '+(observed.wifi.rede||'uma rede não identificada')+'.':'O mestre ainda não informa conexão Wi-Fi ativa. Consulte novamente após a operação.','warn');
          message(plan.name+': comando recebido. Confira o estado do Wi-Fi.');
        }else{
          updateRun(run,'Resposta recebida',plan.note||(checked?'O mestre respondeu; confira o estado atual.':'O mestre respondeu, mas ainda não há confirmação da conclusão.'),checked?'good':'warn');
          message(plan.name+': comando recebido'+(checked?'; estado consultado.':'; confirmação pendente.'));
        }
      }
      else message(plan.name+': comando recebido. Acompanhe o estado de '+(run.followSync?'sincronização':'OTA')+' no painel.');
      if(plan.secret){
        if(['wifi_test','wifi_primary'].includes(plan.action))$('wifi-password').value='';
        if(['wifi_primary','wifi_restaurar_principal','wifi_automatico','wifi_politica','wifi_restaurar_politica'].includes(plan.action))$('wifi-permission').value='';
      }
    }catch(error){
      const timeout=error.name==='AbortError',reason=timeout?'O tempo de resposta esgotou':error.message;
      if(plan.developerDrafts){
        const observed=await masterQuery('ajustes_desenvolvedor');
        const saved=confirmDeveloperDrafts(plan.developerDrafts,observed);
        const detail=reason+'. '+(observed?saved+' de '+plan.developerDrafts.length+' alterações confirmadas.':'A consulta dos valores salvos ficou pendente.');
        updateRun(run,saved?'Confirmação parcial':'Falhou',detail,saved||timeout?'warn':'bad');
        message(detail,saved||timeout?'warning':'error');
      }else{
        updateRun(run,timeout?'Sem resposta':'Falhou','Etapa '+run.step+' de '+plan.steps.length+': '+reason+'. A etapa não teve confirmação de sucesso. Confira o estado antes de repetir a ação.',timeout?'warn':'bad');
        message(reason+'. Confira o estado antes de repetir.',timeout?'warning':'error');
      }
    }
    finally{state.busy=false;availability();}
  }
  const criticalConfirmationActions=new Set([
    'permitir_re','liberar_parada_remota','zerar_percurso','ajuste_desenvolvedor','salvar_radio_satelite',
    'autorizar_mestre','autorizar_equipe','autorizar_visitantes','autorizar_todos'
  ]);
  const armingConfirmationMinMs=3000;
  const confirmationStorageKey='nexus-command-confirmation-seconds';
  const confirmationSeconds=$('command-confirmation-seconds');
  let confirmationDelayMs=3000;
  try {
    const stored=localStorage.getItem(confirmationStorageKey);
    const seconds=Number(stored);
    if(stored!==null&&Number.isInteger(seconds)&&seconds>=0&&seconds<=Number(confirmationSeconds.max))
      confirmationDelayMs=seconds*1000;
  } catch (_) { /* Use o padrão se o armazenamento estiver indisponível. */ }
  confirmationSeconds.value=String(confirmationDelayMs/1000);
  $('confirmation-preferences').addEventListener('submit',event=>{
    event.preventDefault();
    if(!confirmationSeconds.reportValidity())return;
    const seconds=confirmationSeconds.valueAsNumber;
    confirmationDelayMs=seconds*1000;
    let status=seconds===0?'Contagem desligada.':'Espera definida em '+seconds+' s.';
    try { localStorage.setItem(confirmationStorageKey,String(seconds)); }
    catch (_) { status+=' Vale enquanto esta página estiver aberta.'; }
    set('command-confirmation-status',status);
    $('command-confirmation-status').hidden=false;
  });
  const confirmDialog=$('confirm-dialog');
  const confirmButton=$('confirm-action');
  const confirmWait=$('confirm-wait');
  let confirmAfter=0,confirmTimer;
  function commandConfirmationDelayMs(action) {
    if(action==='liberar_parada_remota')return Math.max(armingConfirmationMinMs,confirmationDelayMs);
    return criticalConfirmationActions.has(action)?confirmationDelayMs:0;
  }
  function showCommandConfirmation(plan,destructive){
    clearTimeout(confirmTimer);
    const delay=commandConfirmationDelayMs(plan.action);
    set('confirm-title',plan.name);
    set('confirm-text',plan.prompt||'Enviar ao mestre?');
    confirmButton.className='button'+(destructive?' danger':'');
    confirmButton.textContent=criticalConfirmationActions.has(plan.action)||destructive?'Confirmar':'Enviar comando';
    confirmDialog.returnValue='';
    confirmAfter=performance.now()+delay;
    const update=()=>{
      const remaining=Math.max(0,confirmAfter-performance.now());
      confirmButton.disabled=remaining>0;
      confirmWait.hidden=delay===0;
      set('confirm-wait',remaining>0?'Confirmação disponível em '+Math.ceil(remaining/1000)+' s.':'Você já pode confirmar.');
      if(remaining>0)confirmTimer=setTimeout(update,Math.min(1000,remaining));
    };
    update();
    confirmDialog.showModal();
  }
  document.querySelectorAll('[data-master-action]').forEach(button=>button.addEventListener('click',()=>{
    if(button.dataset.masterAction==='parada_remota'){sendStopNow();return;}
    if(state.busy||button.disabled)return;
    if(button.getAttribute('aria-pressed')==='true')return;
    const action=button.dataset.masterAction;
    const wifiAction=action.startsWith('wifi_');
    if(wifiAction)$('wifi-form-error').hidden=true;
    try{
      const plan=buildPlan(action,button);
      if(plan.refresh.includes('controle_remoto')&&!criticalConfirmationActions.has(action)){executePlan(plan);return;}
      state.pending=plan;
      const destructive=['zerar_percurso','wifi_excluir_salva','wifi_primary','wifi_restaurar_principal','wifi_restaurar_politica','cancelar_ota'].includes(action)||button.dataset.sensorValue==='0';
      showCommandConfirmation(plan,destructive);
    }catch(error){
      if(wifiAction){$('wifi-form-error').textContent=error.message;$('wifi-form-error').hidden=false;$('wifi-form-error').scrollIntoView({block:'nearest'});}
      else message(error.message,'error');
    }
  }));
  confirmDialog.querySelector('form').addEventListener('submit',event=>{
    if(event.submitter?.value==='confirm'&&performance.now()<confirmAfter)event.preventDefault();
  });
  confirmDialog.addEventListener('close',()=>{
    const plan=state.pending;
    state.pending=null;
    clearTimeout(confirmTimer);
    if(confirmDialog.returnValue==='confirm'&&plan&&performance.now()>=confirmAfter)executePlan(plan);
  });
  const queryMetadata = new Set(['ok', 'consulta', 'comando', 'solicitacao', 'resultado', 'tentativas', 'duracao_ms', 'capacidades']);
  const queryPriority = {
    saude: ['estado', 'causas_ativas', 'heap_livre', 'tempo_ativo_ms', 'erros_historicos'],
    telemetria: ['velocidade_centesimos_kmh', 'tensao_pacote_mv', 'corrente_centesimos_a', 'potencia_w',
      'carga_decimos_percentual', 'autonomia_decimos_km', 'percurso_metros', 'simulado'],
    diagnostico_mestre: ['firmware', 'tempo_ativo_ms', 'cpu_total_decimos_percentual', 'temperatura_centesimos_c',
      'heap_interno_livre', 'menor_pilha_livre_bytes', 'tarefa_menor_pilha', 'tarefas_ativas'],
  };
  const queryFieldLabels = {
    tempo_ativo_ms:'Tempo ativo', rtt_ms:'Latência do enlace', versao_firmware:'Firmware',
    tensao_pacote_mv:'Tensão da bateria', carga_decimos_percentual:'Carga da bateria', percurso_metros:'Odômetro',
    autonomia_decimos_km:'Autonomia restante', rssi_dbm:'Sinal Wi-Fi', falhas_crc:'Falhas de CRC',
    recepcoes_ok:'Pacotes recebidos', uart_recepcoes_ok:'Pacotes UART recebidos', rejeicoes_crc:'Rejeições por CRC',
    erros_historicos:'Ocorrências acumuladas', cpu_total_decimos_percentual:'CPU total',
    heap_livre:'Memória livre', heap_interno_livre:'Memória interna livre', menor_pilha_livre_bytes:'Menor folga de pilha',
    tarefa_menor_pilha:'Tarefa com menor folga', simulado:'Dados simulados', equipe_online:'Equipe conectada',
    visitantes_online:'Visitantes conectados', rede:'Rede Wi-Fi',
  };
  const queryUnits = [
    [/_centesimos_kmh$/, 100, 'km/h', 1], [/_milesimos_ms2$/, 1000, 'm/s²', 2],
    [/_decimos_percentual$/, 10, '%', 1], [/_decimos_km$/, 10, 'km', 1],
    [/_centesimos_a$/, 100, 'A', 2], [/_centesimos_c$/, 100, '°C', 1],
    [/_mv$/, 1000, 'V', 3], [/_w$/, 1, 'W', 0], [/_dbm$/, 1, 'dBm', 0],
    [/_ms$/, 1, 'ms', 0], [/_minutos$/, 1, 'min', 0],
  ];

  function queryReading(query, key, reading) {
    if (query === 'saude' && key === 'estado') {
      return ['Saúde do sistema', ['Normal', 'Recuperando', 'Degradada', 'Falha'][reading] ?? String(reading)];
    }
    if (query === 'saude' && key === 'causas_ativas') {
      return ['Causas ativas', getSystemHealth().causes.map(cause => cause.label).join(', ') || 'Nenhuma'];
    }

    const unit = queryUnits.find(([suffix]) => suffix.test(key));
    const words = (unit ? key.replace(unit[0], '') : key).replaceAll('_', ' ');
    const label = queryFieldLabels[key] || words.charAt(0).toUpperCase() + words.slice(1);
    if (typeof reading === 'boolean') return [label, reading ? 'Sim' : 'Não'];
    if (!Number.isFinite(reading)) return [label, reading || 'Não disponível'];
    if (key === 'tempo_ativo_ms') return [label, age(reading)];
    if (key === 'percurso_metros') return [label, format(reading / 1000, 'km', 2)];
    if (key.startsWith('heap_') || key.startsWith('psram_') || key.endsWith('_bytes')) return [label, bytes(reading)];
    if (unit) return [label, format(reading / unit[1], unit[2], unit[3])];
    return [label, number(reading, Number.isInteger(reading) ? 0 : 2)];
  }

  function showMasterQueryResult(query, label, result, error) {
    const view = $('master-query-result');
    const fields = $('master-query-fields');
    const raw = $('master-query-raw');
    view.className = 'command-result' + (result ? ' success' : error ? ' error' : '');
    fields.replaceChildren();
    fields.hidden = true;
    raw.hidden = !result;
    raw.open = false;
    set('master-query-json', result ? JSON.stringify(result, null, 2) : '');
    if (!result) {
      set('master-query-status', error ? label + ' · ' + error : 'Consultando ' + label.toLowerCase() + '…');
      return;
    }

    const title = [label];
    if (Number.isFinite(result.solicitacao)) title.push('solicitação #' + result.solicitacao);
    if (Number.isFinite(result.duracao_ms)) title.push(format(result.duracao_ms, 'ms', 0));
    set('master-query-status', title.join(' · '));
    const candidates = queryPriority[query] || Object.keys(result).filter(key => !queryMetadata.has(key));
    const rows = candidates.filter(key => ['number', 'string', 'boolean'].includes(typeof result[key]))
      .slice(0, 8).map(key => queryReading(query, key, result[key]));
    renderDeviceFacts(fields, rows);
    fields.hidden = !rows.length;
  }

  $('run-master-query').addEventListener('click', async () => {
    if (state.busy) return;
    state.busy = true;
    availability();
    const select = $('master-query');
    const query = select.value, label = select.selectedOptions[0].textContent;
    showMasterQueryResult(query, label);
    commandEvent('MESTRE', query, 'Enviando');
    try {
      const result = await masterQuery(query, false);
      if (!result) throw new Error('Sem resposta do mestre');
      showMasterQueryResult(query, label, result);
    } catch (error) {
      showMasterQueryResult(query, label, null, error.message);
    } finally {
      state.busy = false;
      availability();
    }
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
  $('refresh-simulator-state').addEventListener('click',async()=>{
    if(state.busy)return;state.busy=true;availability();
    try{if(!await masterQuery('simulador'))set('simulator-checked','Não foi possível consultar agora. O estado exibido é o último recebido.');}
    finally{state.busy=false;availability();}
  });
  $('refresh-wifi-state').addEventListener('click',async()=>{
    if(state.busy)return;state.busy=true;availability();
    try{
      if(!await masterQuery('wifi')){set('wifi-command-state','Sem resposta do mestre. O estado da rede não pôde ser confirmado agora.');return;}
      const config=await masterQuery('configuracao');
      if(config)await loadSavedNetworks(config.quantidade_redes_wifi);
    }finally{state.busy=false;availability();}
  });
  function formatCsvTimestamp(time) {
    const offset = -new Date(time).getTimezoneOffset();
    const local = new Date(time + offset * 60000).toISOString().slice(0, -1);
    const absolute = Math.abs(offset);
    const hours = String(Math.floor(absolute / 60)).padStart(2, '0');
    const minutes = String(absolute % 60).padStart(2, '0');
    return local + (offset < 0 ? '-' : '+') + hours + ':' + minutes;
  }

  $('export-csv').addEventListener('click', () => {
    const rows = ['horario_local;origem;velocidade_kmh;aceleracao_ms2;potencia_w;corrente_a;tensao_v'];
    for (const sample of state.samples.filter(point => !point.gap)) {
      rows.push([formatCsvTimestamp(sample.time), sample.simulated ? 'simulado' : 'mestre',
        sample.speed, sample.accel, sample.power, sample.current, sample.voltage].join(';'));
    }
    const url = URL.createObjectURL(new Blob([rows.join('\n')], {type:'text/csv;charset=utf-8'}));
    const link = document.createElement('a');
    link.href = url;
    link.download = 'nexus-telemetria-' + new Date().toISOString().slice(0, 19).replace(/:/g, '-') + '.csv';
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  });

  function setupCanvas(canvas) {
    const rect=canvas.getBoundingClientRect(),ratio=Math.min(devicePixelRatio||1,2),width=Math.max(1,Math.round(rect.width*ratio)),height=Math.max(1,Math.round(rect.height*ratio));
    if(canvas.width!==width||canvas.height!==height){canvas.width=width;canvas.height=height;}
    const ctx=canvas.getContext('2d');ctx.setTransform(ratio,0,0,ratio,0,0);ctx.clearRect(0,0,rect.width,rect.height);return {ctx,w:rect.width,h:rect.height};
  }
  const themeRoot = document.documentElement;
  const themeToggle = $('theme-toggle');
  const chartTones = ['primary', 'secondary', 'tertiary'];
  let chartTheme;

  function syncTheme() {
    const light = themeRoot.dataset.theme === 'light';
    themeToggle.setAttribute('aria-pressed', String(light));
    themeToggle.title = light ? 'Ativar tema escuro' : 'Ativar tema claro';
    const style = getComputedStyle(themeRoot);
    document.querySelector('meta[name="theme-color"]').content = style.getPropertyValue('--nav').trim();
    const names = [...chartTones, 'memory', 'failure', 'fill', 'grid', 'axis', 'label', 'event', 'cursor', 'marker-edge'];
    chartTheme = Object.fromEntries(names.map(name => [name, style.getPropertyValue('--chart-' + name).trim()]));
  }
  syncTheme();
  themeToggle.addEventListener('click', () => {
    const theme = themeRoot.dataset.theme === 'dark' ? 'light' : 'dark';
    themeRoot.dataset.theme = theme;
    try {
      localStorage.setItem(themeRoot.dataset.themeStorage, theme);
    } catch (_) {
      // A troca de tema continua válida na página aberta.
    }
    syncTheme();
    drawCharts();
    if (state.tab in state.selected) drawDeviceChart(state.tab, state.selected[state.tab]);
  });
  const chartInfo = {
    speed:{label:'Velocidade',unit:'km/h',digits:1},
    accel:{label:'Aceleração',unit:'m/s²',digits:2},
    power:{label:'Potência',unit:'W',digits:2},
    current:{label:'Corrente',unit:'A',digits:2},
    voltage:{label:'Tensão',unit:'V',digits:3},
    soc:{label:'SoC',unit:'%',digits:2},
    autonomy:{label:'Autonomia · simulada',unit:'km',digits:2},
    shunt:{label:'Tensão no shunt · INA228',unit:'mV',digits:3},
    inaTemp:{label:'Temperatura · INA228',unit:'°C',digits:2},
    inaEnergy:{label:'Energia acumulada · INA228',unit:'Wh',digits:2},
    inaCharge:{label:'Carga acumulada · INA228',unit:'Ah',digits:3},
  };
  function chartRange(key,values){
    if(key==='speed')return [0,Math.max(30,Math.ceil(Math.max(0,...values)*1.08/5)*5)];
    if(key==='accel'){const limit=Math.max(2,Math.ceil(Math.max(0,...values.map(Math.abs))*1.15*2)/2);return [-limit,limit];}
    if(key==='soc')return [0,100];
    if(!values.length)return [0,1];
    let min=Math.min(...values),max=Math.max(...values);
    if(key==='power'||key==='inaEnergy'||key==='rtt')min=Math.min(0,min);
    const spread=Math.max(1,(max-min)*.18);min-=spread;max+=spread;
    const step=Math.pow(10,Math.floor(Math.log10((max-min)/4)));
    return [key==='rtt'?0:Math.floor(min/step)*step,Math.ceil(max/step)*step];
  }
  const chartFrame=(w,h,expanded=false)=>{
    const side=expanded?46:58,top=expanded?20:26,bottomMargin=expanded?24:29;
    return {left:side,right:Math.max(side+1,w-side),top,bottom:Math.max(top+1,h-bottomMargin)};
  };
  function drawChartGrid(ctx, {left, right, top, bottom}, start, end, height) {
    ctx.font = '10px Segoe UI, sans-serif';
    ctx.lineWidth = 1;
    for (let i = 0; i <= 3; i++) {
      const y = top + (bottom - top) * i / 3;
      ctx.strokeStyle = i === 3 ? chartTheme.axis : chartTheme.grid;
      ctx.beginPath();
      ctx.moveTo(left, y);
      ctx.lineTo(right, y);
      ctx.stroke();
    }
    for (let i = 0; i <= 2; i++) {
      const x = left + (right - left) * i / 2;
      ctx.strokeStyle = chartTheme.grid;
      ctx.beginPath();
      ctx.moveTo(x, top);
      ctx.lineTo(x, bottom);
      ctx.stroke();
      ctx.fillStyle = chartTheme.label;
      ctx.textAlign = ['left', 'center', 'right'][i];
      const time = new Date(start + (end - start) * i / 2);
      ctx.fillText(time.toLocaleTimeString('pt-BR', {hour:'2-digit', minute:'2-digit', second:'2-digit'}), x, height - 7);
    }
  }
  function plot(canvasId,keys){
    const canvas=$(canvasId);if(!canvas||!canvas.getBoundingClientRect().width)return;
    const {ctx,w,h}=setupCanvas(canvas),frame=chartFrame(w,h,true),{left,right,top,bottom}=frame;
    const end=Date.now(),start=end-state.range*1000,samples=state.samples.filter(s=>s.time>=start&&s.time<=end);
    const x=time=>left+(time-start)/(end-start)*(right-left);
    const series=keys.map((key,index)=>{const info=chartInfo[key],values=samples.filter(s=>!s.gap&&Number.isFinite(s[key])).map(s=>s[key]);return {...info,key,color:chartTheme[chartTones[index]],available:values.length>0,range:chartRange(key,values)};});
    drawChartGrid(ctx, frame, start, end, h);
    series.slice(0,2).forEach((axis,index)=>{
      const [min,max]=axis.range;
      ctx.fillStyle=axis.color;ctx.font='600 10px Segoe UI, sans-serif';ctx.textAlign=index?'right':'left';ctx.fillText(axis.unit,index?right:left,15);
      ctx.font='10px Segoe UI, sans-serif';ctx.textAlign=index?'left':'right';
      for(let i=0;i<=3;i++){const gy=top+(bottom-top)*i/3,reading=max-(max-min)*i/3;ctx.fillText(axis.available?number(reading,Math.abs(max-min)<10?1:0):'—',index?right+7:left-7,gy+3);}
    });
    ctx.save();ctx.beginPath();ctx.rect(left,top,right-left,bottom-top);ctx.clip();
    ctx.setLineDash([3,5]);ctx.strokeStyle=chartTheme.event;ctx.lineWidth=1;
    state.events.filter(e=>e.time>=start&&e.time<=end).forEach(e=>{const px=x(e.time);ctx.beginPath();ctx.moveTo(px,top);ctx.lineTo(px,bottom);ctx.stroke();});ctx.setLineDash([]);
    series.forEach((axis, index) => {
      const [min, max] = axis.range;
      const y = v => bottom - (v - min) / (max - min) * (bottom - top);
      ctx.strokeStyle = axis.color;
      ctx.lineWidth = index === 2 ? 1.9 : 2.2;
      ctx.lineJoin = 'round';
      ctx.lineCap = 'round';
      ctx.setLineDash(index === 2 ? [6, 4] : []);
      let first = null, last = null, latest = null;
      const finishSegment = () => {
        if (!last) return;
        ctx.stroke();
        if (index === 0 && first.x !== last.x) {
          ctx.lineTo(last.x, bottom);
          ctx.lineTo(first.x, bottom);
          ctx.closePath();
          ctx.fillStyle = chartTheme.fill;
          ctx.fill();
        }
        first = last = null;
      };
      for (const sample of samples) {
        const reading = sample[axis.key];
        if (sample.gap || !Number.isFinite(reading)) {
          finishSegment();
          continue;
        }
        const point = {x:x(sample.time), y:y(reading)};
        if (first) ctx.lineTo(point.x, point.y);
        else {
          ctx.beginPath();
          ctx.moveTo(point.x, point.y);
          first = point;
        }
        last = latest = point;
      }
      finishSegment();
      ctx.setLineDash([]);
      if (latest) {
        ctx.fillStyle = axis.color;
        ctx.beginPath();
        ctx.arc(latest.x, latest.y, 3.3, 0, Math.PI * 2);
        ctx.fill();
      }
    });
    const hover=state.hover?.[canvasId];if(hover&&hover.time>=start&&hover.time<=end){
      const px=x(hover.time);ctx.strokeStyle=chartTheme.cursor;ctx.lineWidth=1;ctx.setLineDash([3,3]);ctx.beginPath();ctx.moveTo(px,top);ctx.lineTo(px,bottom);ctx.stroke();ctx.setLineDash([]);
      series.forEach(axis=>{if(!Number.isFinite(hover[axis.key]))return;const [min,max]=axis.range,py=bottom-(hover[axis.key]-min)/(max-min)*(bottom-top);ctx.fillStyle=axis.color;ctx.strokeStyle=chartTheme['marker-edge'];ctx.lineWidth=2;ctx.beginPath();ctx.arc(px,py,5,0,Math.PI*2);ctx.fill();ctx.stroke();});
    }
    ctx.restore();
    if(!samples.some(s=>keys.some(key=>Number.isFinite(s[key])))){ctx.fillStyle=chartTheme.label;ctx.textAlign='center';ctx.font='12px Segoe UI, sans-serif';ctx.fillText(canvasId==='electric-chart'?'Aguardando medição elétrica':'Aguardando amostras disponíveis',w/2,h/2);}
  }
  function drawCharts(){
    plot('motion-chart',['speed','accel']);plot('electric-chart',selectedElectricKeys());
    const primary=chartInfo[state.signal],secondary=chartInfo[state.signalSecondary];
    const extra=state.signalTertiary==='none'?null:chartInfo[state.signalTertiary];
    set('electric-caption','À esquerda, '+primary.label.toLowerCase()+' ('+primary.unit+'); à direita, '+secondary.label.toLowerCase()+' ('+secondary.unit+').'+(extra?' Curva tracejada com escala própria; toque ou passe o cursor para ler o valor.':''));
    [["electric-legend-primary",primary],["electric-legend-secondary",secondary]].forEach(([id,info])=>{
      const legend=$(id),mark=legend.querySelector('.legend-key');
      if(legend.lastChild?.textContent!==info.label)legend.replaceChildren(mark,document.createTextNode(info.label));
    });
    const third=$('electric-legend-tertiary');third.hidden=!extra;
    if(extra){
      set('electric-tertiary-label',extra.label);
    }
  }
  function drawDeviceChart(group,id){
    const canvas=$('device-chart-'+group);if(!canvas||state.tab!==group||state.selected[group]!==id||!canvas.getBoundingClientRect().width)return;
    const keys=[state.deviceSignal[id],state.deviceSignalSecondary[id]],history=state.deviceSamples[id].filter(sample=>keys.some(key=>Object.prototype.hasOwnProperty.call(sample,key)));
    const {ctx,w,h}=setupCanvas(canvas),frame=chartFrame(w,h),{left,right,top,bottom}=frame,end=Date.now(),start=history.length>1?Math.min(history[0].time,end-1000):end-60000;
    const x=t=>left+(t-start)/(end-start)*(right-left);
    const series=selectedDeviceCharts(id).map(option=>{const key=option.key,values=history.filter(sample=>Object.prototype.hasOwnProperty.call(sample,key)).map(sample=>sample[key]).filter(Number.isFinite);return {...option,color:chartTheme[option.tone],values,range:['cpu','core0','core1','availability'].includes(key)?[0,100]:chartRange(key,values)};});
    drawChartGrid(ctx, frame, start, end, h);
    series.forEach((axis,index)=>{const [min,max]=axis.range;ctx.fillStyle=axis.color;ctx.font='600 10px Segoe UI, sans-serif';ctx.textAlign=index?'right':'left';ctx.fillText(axis.unit,index?right:left,15);ctx.font='10px Segoe UI, sans-serif';ctx.textAlign=index?'left':'right';for(let i=0;i<=3;i++){const gy=top+(bottom-top)*i/3;ctx.fillText(axis.values.length?number(max-(max-min)*i/3,Math.abs(max-min)<10?1:0):'—',index?right+7:left-7,gy+3);}});
    ctx.save();ctx.beginPath();ctx.rect(left,top,right-left,bottom-top);ctx.clip();
    series.forEach(axis=>{const [min,max]=axis.range,y=v=>bottom-(v-min)/(max-min)*(bottom-top);ctx.strokeStyle=axis.color;ctx.lineWidth=2.2;ctx.lineJoin='round';ctx.lineCap='round';ctx.beginPath();let open=false,last=null;history.filter(sample=>Object.prototype.hasOwnProperty.call(sample,axis.key)).forEach(sample=>{const reading=sample[axis.key];if(!Number.isFinite(reading)){open=false;return;}const px=x(sample.time),py=y(reading);if(open)ctx.lineTo(px,py);else ctx.moveTo(px,py);open=true;last={x:px,y:py};});ctx.stroke();if(last){ctx.fillStyle=axis.color;ctx.beginPath();ctx.arc(last.x,last.y,3.3,0,Math.PI*2);ctx.fill();}});
    ctx.restore();
    if(!series.some(axis=>axis.values.length)){ctx.fillStyle=chartTheme.label;ctx.textAlign='center';ctx.font='12px Segoe UI, sans-serif';ctx.fillText('Aguardando leituras do dispositivo',w/2,h/2);}
  }
  function chartTooltip(canvasId,tipId,keys){
    const canvas=$(canvasId),tip=$(tipId);
    const move=e=>{
      const rect=canvas.getBoundingClientRect(),px=e.clientX-rect.left,start=Date.now()-state.range*1000,frame=chartFrame(rect.width,rect.height,true),ts=start+(px-frame.left)/Math.max(1,frame.right-frame.left)*state.range*1000;
      const list=state.samples.filter(s=>!s.gap&&Math.abs(s.time-ts)<state.range*1000/12);
      if(!list.length){tip.hidden=true;state.hover={};drawCharts();return;}
      const sample=list.reduce((a,b)=>Math.abs(a.time-ts)<Math.abs(b.time-ts)?a:b),chosen=canvasId==='electric-chart'?selectedElectricKeys():keys;
      const heading=document.createElement('strong');heading.className='chart-tip-time';heading.textContent=new Date(sample.time).toLocaleTimeString('pt-BR');
      const rows=chosen.map((key,index)=>{const info=chartInfo[key],row=document.createElement('span'),mark=document.createElement('i'),label=document.createElement('span'),reading=document.createElement('b');row.className='chart-tip-reading';mark.className='chart-tip-key';mark.style.backgroundColor='var(--chart-'+chartTones[index]+')';label.textContent=info.label;reading.textContent=Number.isFinite(sample[key])?number(sample[key],info.digits)+' '+info.unit:'Não disponível';row.append(mark,label,reading);return row;});
      tip.replaceChildren(heading,...rows);
      tip.hidden=false;tip.style.left=Math.min(Math.max(8,px+12),Math.max(8,rect.width-tip.offsetWidth-8))+'px';tip.style.top='18px';
      state.hover={[canvasId]:sample};drawCharts();
    };
    canvas.addEventListener('pointermove',move);
    canvas.addEventListener('pointerdown',move);
    const leave=()=>{tip.hidden=true;state.hover={};drawCharts();};
    canvas.addEventListener('pointerleave',leave);
    canvas.addEventListener('pointercancel',leave);
  }
  chartTooltip('motion-chart','motion-tip',['speed','accel']);chartTooltip('electric-chart','electric-tip',['power','current','voltage']);
  addEventListener('resize',()=>{drawCharts();if(state.tab==='mestre'||state.tab==='equipe'||state.tab==='visitantes')drawDeviceChart(state.tab,state.selected[state.tab]);});
  const liveCommandCards = {ota:'ota-command',sincronizacao:'sync-card',wifi:'wifi-master'};
  async function pollCommandStates(){
    if(!liveReadingsAvailable()||!state.masterOnline||state.polls.commandCycle)return;
    const now=Date.now(),run=state.commandRuns[0];
    const followup=run?.responseAt&&now<run.followUntil&&now-(state.lastCommandPoll||0)>=state.commandRefreshMs;
    const queries=followup ? run.refreshQueries.filter(query=>query==='configuracao'||
      (liveCommandCards[query]&&!commandCardOpen(liveCommandCards[query]))) : [];
    const wifiOpen=commandCardOpen('wifi-master');
    const settingsVisible=wifiOpen||commandCardOpen('operation-command');
    if(settingsVisible&&now-(state.lastAuxPoll||0)>=30000&&!queries.includes('configuracao'))queries.push('configuracao');
    const readNetworks=wifiOpen&&now-(state.lastNetworksPoll||0)>=60000;
    if(!queries.length&&!readNetworks)return;
    state.polls.commandCycle=true;
    try{
      for(const query of queries){
        if(!liveReadingsAvailable())break;
        await masterQuery(query);
        if(query==='configuracao')state.lastAuxPoll=Date.now();
      }
      if(followup)state.lastCommandPoll=Date.now();
      if(readNetworks&&liveReadingsAvailable()){
        const config=state.master.configuracao;
        if(config){
          await loadSavedNetworks(config.quantidade_redes_wifi);
          state.lastNetworksPoll=Date.now();
        }
      }
    }finally{state.polls.commandCycle=false;}
  }
  async function pollRemote(){
    if(!liveReadingsAvailable()||state.polls.remoteCycle)return;
    const now=Date.now();
    if(now-(state.lastLinkPoll||0)<10000)return;
    state.lastLinkPoll=now;
    state.polls.remoteCycle=true;
    try{
      if(!await masterQuery('ping')){
        set('ota-state-checked','Mestre sem resposta. O estado acima é a última leitura recebida.');
        set('sync-checked','Mestre sem resposta. O estado acima é a última leitura recebida.');
        return;
      }
      if(!liveReadingsAvailable())return;
      const atualizarVersao=!state.lastVersionPoll || now-state.lastVersionPoll>(state.master.versao?60000:15000);
      if(atualizarVersao){
        await masterQuery('versao');
        state.lastVersionPoll=Date.now();
      }
      if(!liveReadingsAvailable()||(state.tab!=='visitantes'&&!commandCardOpen('wifi-local')))return;
      if(await visitorsQuery('ping')&&liveReadingsAvailable()&&(!state.visitors.versao||atualizarVersao))await visitorsQuery('versao');
    }finally{state.polls.remoteCycle=false;}
  }
  const wifiPanels={equipe:{},visitantes:{}};
  function setupWifiPanel(group){
    const panel=$('wifi-clients-'+group),label=group==='equipe'?'Equipe':'Visitantes';
    panel.innerHTML='<div class="wifi-clients-head"><div><h2>Rede da '+label+'</h2><p>Celulares e outros dispositivos conectados ao painel deste ESP.</p></div><div class="wifi-clients-actions"><span class="wifi-clients-summary">Aguardando consulta</span><button type="button" class="button secondary wifi-clients-refresh">Atualizar</button></div></div><form class="wifi-client-settings"><label>Tempo máximo por conexão (min)<input type="number" min="0" max="1440" step="1" value="15" aria-label="Tempo máximo de conexão na rede '+label+'"></label><button type="submit" class="button">Salvar tempo</button><p>0 deixa a conexão sem limite. Um dispositivo desconectado pode entrar novamente.</p></form><p class="wifi-client-feedback" role="status" hidden></p><div class="wifi-client-list"><p class="wifi-client-empty">Aguardando lista de dispositivos.</p></div><p class="command-live-meta wifi-client-checked">Nenhuma consulta concluída.</p>';
    panel.querySelector('.wifi-clients-refresh').addEventListener('click',()=>pollWifiClients(group,true));
    const form=panel.querySelector('form');
    form.addEventListener('submit',async event=>{
      event.preventDefault();const minutes=Number(form.querySelector('input').value);
      if(!Number.isInteger(minutes)||minutes<0||minutes>1440)return;
      const button=form.querySelector('button');button.disabled=true;
      try{await wifiCommand(group,'limite',minutes);wifiFeedback(group,'Tempo máximo salvo: '+(minutes?minutes+' min por conexão':'sem limite')+'.','good');await pollWifiClients(group,true);}
      catch(error){wifiFeedback(group,'Não foi possível salvar o tempo: '+error.message,'bad');}
      finally{button.disabled=false;}
    });
  }
  function wifiFeedback(group,text,kind){const el=$('wifi-clients-'+group).querySelector('.wifi-client-feedback');el.textContent=text;el.className='wifi-client-feedback '+kind;el.hidden=false;}
  async function wifiCommand(group,action,value){
    const local=group==='equipe',url=local?'/api/wifi/'+(action==='limite'?'limite':'desconectar'):'/api/mestre?consulta=satelite_visitantes_'+(action==='limite'?'limite':'kick');
    const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),18000);
    const body=local?JSON.stringify(action==='limite'?{minutos:value}:{mac:value}):String(value);
    const run=async()=>{const response=await fetch(url,{method:'POST',headers:{'Content-Type':local?'application/json':'text/plain; charset=utf-8'},body,cache:'no-store',signal:controller.signal});const data=await response.json();if(!response.ok||data.ok===false)throw new Error(data.erro||'Solicitação recusada');return data;};
    try{return await (local?run():remote(run));}finally{clearTimeout(timer);}
  }
  async function pollWifiClients(group,force=false){
    const view=wifiPanels[group];if(view.busy||document.hidden&&!force)return;view.busy=true;
    const panel=$('wifi-clients-'+group),list=panel.querySelector('.wifi-client-list');
    try{
      let data;
      if(group==='equipe')data=await getJSON('/api/wifi/clientes',7000);
      else {data=await remote(()=>getJSON('/api/mestre?consulta=satelite_visitantes_clientes&inicio=0',18000));if(Number(data.quantidade_total)>data.clientes.length){const rest=await remote(()=>getJSON('/api/mestre?consulta=satelite_visitantes_clientes&inicio=5',18000));data.clientes.push(...rest.clientes);}}
      view.data=data;view.checked=Date.now();
      const count=Number(data.quantidade_total??data.quantidade??data.clientes.length);
      set(group+'-client-count',String(count));
      panel.querySelector('.wifi-clients-summary').textContent=count+' conectado(s)';
      const field=panel.querySelector('.wifi-client-settings input');if(document.activeElement!==field)field.value=String(data.limite_sessao_minutos??15);
      list.replaceChildren();
      if(!data.clientes.length){const empty=document.createElement('p');empty.className='wifi-client-empty';empty.textContent='Nenhum dispositivo conectado agora.';list.append(empty);}
      data.clientes.forEach(client=>{
        const row=document.createElement('div'),mac=document.createElement('strong'),duration=document.createElement('span'),button=document.createElement('button');
        row.className='wifi-client-row';mac.textContent=client.mac;duration.dataset.connectedMs=String(Number(client.conectado_ms)||0);duration.dataset.checkedAt=String(view.checked);duration.textContent='Conectado há '+age(Number(client.conectado_ms)||0);
        button.type='button';button.className='button danger';button.textContent='Desconectar';button.setAttribute('aria-label','Desconectar '+client.mac);
        button.addEventListener('click',async()=>{if(!confirm('Desconectar '+client.mac+' da rede '+(group==='equipe'?'Equipe':'Visitantes')+'?'))return;button.disabled=true;try{await wifiCommand(group,'kick',client.mac);wifiFeedback(group,'Desconexão solicitada para '+client.mac+'.','good');await pollWifiClients(group,true);}catch(error){wifiFeedback(group,'Resposta não confirmada para '+client.mac+': '+error.message+'. Consulte a lista novamente.','bad');}finally{button.disabled=false;}});
        row.append(mac,duration,button);list.append(row);
      });
      panel.querySelector('.wifi-client-checked').textContent='Consultado às '+new Date(view.checked).toLocaleTimeString('pt-BR')+'. O limite é por conexão, não é o prazo do endereço IP.';
    }catch(error){panel.querySelector('.wifi-client-checked').textContent='Não foi possível consultar a rede: '+error.message+'. Os dados anteriores podem estar desatualizados.';}
    finally{view.busy=false;}
  }
  setupWifiPanel('equipe');setupWifiPanel('visitantes');
  function pollVisibleWifiClients(){
    if(!commandCardOpen('wifi-local')||!liveReadingsAvailable())return;
    void pollWifiClients('equipe');
    if(state.masterOnline)void pollWifiClients('visitantes');
  }
  $('wifi-local').closest('details').addEventListener('toggle',pollVisibleWifiClients);
  setInterval(pollVisibleWifiClients,10000);
  setInterval(()=>{document.querySelectorAll('.wifi-client-row span[data-connected-ms]').forEach(el=>{el.textContent='Conectado há '+age(Number(el.dataset.connectedMs)+Date.now()-Number(el.dataset.checkedAt));});},1000);
  showTab(location.hash.slice(1));renderEvents();renderMasterCommandHistory();availability();updateStatus();
  pollTelemetry();pollVisibleWifiClients();
  document.addEventListener('visibilitychange',()=>{if(!document.hidden){state.lastLinkPoll=0;state.lastCommandPoll=0;state.lastAuxPoll=0;pollTelemetry();pollVisibleWifiClients();}});
})();
