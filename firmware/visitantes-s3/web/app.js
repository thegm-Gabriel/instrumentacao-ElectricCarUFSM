(() => {
  'use strict';

  const $ = id => document.getElementById(id);
  const format = (value, digits = 1) => value.toLocaleString('pt-BR', {minimumFractionDigits:digits, maximumFractionDigits:digits});
  const finite = value => typeof value === 'number' && Number.isFinite(value);
  const speedGaugeMax = 35;
  const consumptionMinMeters = 20;
  const speedColors = [[69,216,191],[234,213,104],[255,159,83],[255,98,115]];
  const throttleColors = [[69,216,191],[166,222,111],[246,197,92]];
  const brakeColors = [[246,205,127],[255,153,86],[255,98,115]];

  function colorAt(colors, fraction) {
    const position = Math.max(0, Math.min(1, fraction)) * (colors.length - 1);
    const index = Math.min(colors.length - 2, Math.floor(position));
    const weight = position - index;
    return 'rgb(' + colors[index].map((channel, i) => Math.round(channel + (colors[index + 1][i] - channel) * weight)).join(',') + ')';
  }

  function updatePedal(id, value, colors, reverse) {
    const valid = finite(value) && value >= 0 && value <= 100;
    setValue(id + '-value', valid ? format(value, 0) + '%' : 'Sem leitura');
    const fill = $(id + '-fill');
    const fraction = valid ? value / 100 : 0;
    fill.style.width = (fraction * 100) + '%';
    fill.style.background = 'linear-gradient(to ' + (reverse ? 'left' : 'right') + ',' + colorAt(colors, 0) + ',' + colorAt(colors, fraction) + ')';
  }
  const state = {
    lastData:null, lastSampleAt:0, firstSampleAt:0, lastSequence:null,
    samples:[], gapOpen:false, rangeMinutes:1, chartSignal:'accel',
    firstDistance:null, previousEnergy:null, energyWh:null,
    consumptionWh:0, consumptionMeters:0,
    chartLimits:{}, hovering:{}, fetching:false,
  };

  function setValue(id, text, unit = '') {
    const el = $(id);
    el.textContent = text;
    el.classList.toggle('empty', text === 'Sem leitura');
    const unitEl = $(id.replace('-value', '-unit'));
    if (unitEl) unitEl.textContent = unit;
  }

  function setStatus(kind, label) {
    const status = document.querySelector('.live-status');
    if (status.className !== 'live-status ' + kind) status.className = 'live-status ' + kind;
    if ($('status-label').textContent !== label) $('status-label').textContent = label;
    document.body.dataset.feed = kind;
  }

  function elapsedText(ms) {
    if (ms < 1000) return 'agora';
    if (ms < 60000) return 'há ' + Math.floor(ms / 1000) + ' s';
    return 'há ' + Math.floor(ms / 60000) + ' min';
  }

  function updateAge() {
    $('last-update').textContent = state.lastSampleAt ? 'Última leitura ' + elapsedText(Date.now() - state.lastSampleAt) : 'Sem leitura';
    if (state.firstSampleAt) {const elapsed=Date.now()-state.firstSampleAt;setValue('session-value',elapsed<60000?format(Math.floor(elapsed/1000),0)+' s':format(elapsed/60000,1)+' min');}
    if (state.lastSampleAt && Date.now() - state.lastSampleAt > 2500 && !state.gapOpen) addGap();
  }

  function resetSession() {
    state.samples = [];
    state.gapOpen = false;
    state.firstSampleAt = 0;
    state.firstDistance = null;
    state.previousEnergy = null;
    state.energyWh = null;
    state.consumptionWh = 0;
    state.consumptionMeters = 0;
    state.chartLimits = {};
  }

  function addGap() {
    if (state.gapOpen || !state.samples.length) return;
    state.samples.push({time:Date.now(), speed:null, power:null, accel:null, battery:null});
    state.gapOpen = true;
    state.previousEnergy = null;
    drawCharts();
  }

  function readNumber(data, key, divisor = 1) {
    return finite(data[key]) ? data[key] / divisor : null;
  }

  function updateReadings(data) {
    const simulated = data.simulado === true;
    const speed = readNumber(data, 'velocidade_centesimos_kmh', 100);
    const power = simulated ? readNumber(data, 'potencia_w') : null;
    const accel = data.aceleracao_disponivel ? readNumber(data, 'aceleracao_milesimos_ms2', 1000) : null;
    const charge = simulated ? readNumber(data, 'carga_decimos_percentual', 10) : null;
    const voltage = simulated ? readNumber(data, 'tensao_pacote_mv', 1000) : null;
    const current = simulated ? readNumber(data, 'corrente_centesimos_a', 100) : null;
    const range = readNumber(data, 'autonomia_decimos_km', 10);

    setValue('speed-value', speed === null ? 'Sem leitura' : format(speed, 1));
    $('motion-state').textContent = speed === null ? 'Aguardando dados' : speed < 0.5 ? 'Parado' : 'Em movimento';
    $('speed-source').textContent = simulated ? 'Leitura simulada' : 'Leitura do veículo';
    const fraction = speed === null ? 0 : Math.min(1, Math.max(0, speed / speedGaugeMax));
    $('speed-arc').style.strokeDasharray = (fraction * 100) + ' 100';
    $('speed-tone-start').setAttribute('stop-color', colorAt(speedColors, fraction * 0.65));
    $('speed-tone-end').setAttribute('stop-color', colorAt(speedColors, fraction));
    $('speed-marker').toggleAttribute('hidden', speed === null);
    $('speed-marker').style.transform = 'rotate(' + (fraction * 180) + 'deg)';
    updatePedal('throttle', readNumber(data, 'acelerador_decimos_percentual', 10), throttleColors, false);
    updatePedal('brake', readNumber(data, 'freio_decimos_percentual', 10), brakeColors, true);

    setValue('battery-value', charge === null ? 'Sem leitura' : format(charge, 1), charge === null ? '' : '%');
    $('battery-fill').style.width = charge === null ? '0%' : Math.max(0, Math.min(100, charge)) + '%';
    $('battery-hint').textContent = simulated ? 'Carga estimada pela simulação.' : 'Sem medição validada no modo real.';

    const powerUnit = power === null ? '' : Math.abs(power) >= 1000 ? 'kW' : 'W';
    setValue('power-value', power === null ? 'Sem leitura' : format(powerUnit === 'kW' ? power / 1000 : power, powerUnit === 'kW' ? 2 : 0), powerUnit);
    $('power-hint').textContent = power === null ? 'Potência elétrica sem leitura no modo real.' : power < 0 ? 'Simulação · devolução de energia ao sistema.' : 'Simulação · consumo elétrico, não potência nas rodas.';

    setValue('range-value', range === null ? 'Sem leitura' : format(range, 1), range === null ? '' : 'km');
    $('range-hint').textContent = range === null ? 'Estimativa não disponível.' : simulated ? 'Autonomia simulada.' : 'Estimativa informada pelo veículo.';
    const consumption = power !== null && state.consumptionMeters >= consumptionMinMeters
      ? state.consumptionWh / (state.consumptionMeters / 1000) : null;
    setValue('consumption-value', consumption === null ? 'Sem leitura' : format(consumption, 1), consumption === null ? '' : 'Wh/km');
    $('consumption-hint').textContent = power === null ? 'Aguardando leituras de energia.' : consumption === null
      ? 'Aguardando ' + consumptionMinMeters + ' m com energia e percurso.'
      : 'Trechos acompanhados nesta página · sem descontar regeneração.';

    setValue('accel-value', accel === null ? 'Sem leitura' : format(accel, 2), accel === null ? '' : 'm/s²');
    $('accel-hint').textContent = accel === null ? 'Aguardando leitura válida do sensor.' : simulated ? 'Dados simulados.' : accel > 0.15 ? 'Acelerando · leitura do sensor.' : accel < -0.15 ? 'Desacelerando · leitura do sensor.' : 'Velocidade estável · leitura do sensor.';

    const distance = readNumber(data, 'percurso_metros');
    setValue('trip-value', distance === null ? 'Sem leitura' : format(distance / 1000, 2), distance === null ? '' : 'km');
    $('trip-hint').textContent = simulated ? 'Percurso simulado.' : 'Percurso registrado pelo veículo.';
    const hasElectricData = charge !== null || power !== null;
    $('energy-live-grid').hidden = !hasElectricData && range === null;
    $('energy-unavailable').hidden = hasElectricData || range !== null;
    document.querySelectorAll('.electric-detail').forEach(card => {card.hidden = !hasElectricData;});
    document.querySelector('#details-title').closest('.section').querySelector('.detail-grid').classList.toggle('compact',!hasElectricData);
    ['power','battery'].forEach(key => {$('visitor-chart-signal').querySelector('option[value="'+key+'"]').disabled = !hasElectricData;});
    if (!hasElectricData && ['power','battery'].includes(state.chartSignal)) {state.chartSignal='accel';$('visitor-chart-signal').value='accel';}
    if (!hasElectricData) $('energy-unavailable').textContent = 'O veículo ainda não envia leituras de bateria e potência.';

    setValue('voltage-value', voltage === null || voltage <= 0 ? 'Sem leitura' : format(voltage, 2) + ' V');
    $('voltage-hint').textContent = simulated ? 'Valor simulado.' : 'Sem medição no modo real.';
    setValue('current-value', current === null ? 'Sem leitura' : format(current, 2) + ' A');
    $('current-hint').textContent = simulated ? 'Valor simulado.' : 'Sem medição no modo real.';
    setValue('energy-value', state.energyWh === null ? 'Sem leitura' : format(state.energyWh, 1) + ' Wh');
    if (distance !== null) {
      if (state.firstDistance === null || distance < state.firstDistance) state.firstDistance = distance;
      setValue('distance-value', format(Math.max(0, distance - state.firstDistance) / 1000, 2) + ' km');
    }
    $('data-note').hidden = !simulated;
    if (simulated) $('data-note').textContent = 'Demonstração · Os valores apresentados são simulados.';
  }

  // Energia e distância do consumo médio devem cobrir os mesmos intervalos.
  function accumulateEnergy(power, distance, now) {
    if (power !== null) {
      if (state.previousEnergy && now > state.previousEnergy.time && now - state.previousEnergy.time <= 3000) {
        const hours = (now - state.previousEnergy.time) / 3600000;
        const before = state.previousEnergy.power;
        let consumed = 0;
        if (before >= 0 && power >= 0) consumed = (before + power) / 2;
        else if (before > 0) consumed = before * before / (2 * (before - power));
        else if (power > 0) consumed = power * power / (2 * (power - before));
        state.energyWh = (state.energyWh || 0) + consumed * hours;
        if (finite(distance) && finite(state.previousEnergy.distance) && distance >= state.previousEnergy.distance) {
          state.consumptionWh += consumed * hours;
          state.consumptionMeters += distance - state.previousEnergy.distance;
        }
      }
      state.previousEnergy = {time:now, power, distance};
    } else state.previousEnergy = null;
  }

  function acceptSample(data) {
    const distance = readNumber(data, 'percurso_metros');
    const previousDistance = state.lastData ? readNumber(state.lastData, 'percurso_metros') : null;
    if (state.lastData && (state.lastData.simulado !== data.simulado ||
        (distance !== null && previousDistance !== null && distance < previousDistance))) resetSession();
    const now = Date.now();
    if (!state.firstSampleAt) state.firstSampleAt = now;
    const power = data.simulado ? readNumber(data, 'potencia_w') : null;
    accumulateEnergy(power, distance, now);
    state.samples.push({time:now, speed:readNumber(data, 'velocidade_centesimos_kmh', 100), power,accel:data.aceleracao_disponivel?readNumber(data,'aceleracao_milesimos_ms2',1000):null,battery:data.simulado?readNumber(data,'carga_decimos_percentual',10):null});
    state.samples = state.samples.filter(sample => now - sample.time <= 610000);
    state.gapOpen = false;
    state.lastData = data;
    state.lastSampleAt = now - Math.min(Math.max(data.idade_ms, 0), 2000);
    state.lastSequence = data.sequencia;
    updateReadings(data);
    drawCharts();
  }

  async function poll() {
    if (state.fetching) return;
    state.fetching = true;
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 3500);
    try {
      const response = await fetch('/api/telemetria', {cache:'no-store', signal:controller.signal});
      if (!response.ok) throw new Error('Sem resposta');
      const data = await response.json();
      if (data.valido === true && finite(data.idade_ms) && data.idade_ms < 2000) {
        if (data.sequencia !== state.lastSequence) acceptSample(data);
        setStatus(data.simulado ? 'demo' : 'live', data.simulado ? 'Simulação ativa' : 'Leituras atuais');
      } else {
        setStatus('stale', 'Dados desatualizados');
        addGap();
      }
    } catch (_) {
      setStatus('offline', 'Reconectando');
      addGap();
    } finally {
      clearTimeout(timer);
      state.fetching = false;
      updateAge();
    }
  }

  function chartSetup(canvas) {
    const rect = canvas.getBoundingClientRect();
    if (!rect.width || !rect.height) return null;
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const width = Math.round(rect.width * dpr), height = Math.round(rect.height * dpr);
    if (canvas.width !== width || canvas.height !== height) {canvas.width = width; canvas.height = height;}
    const ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, rect.width, rect.height);
    return {ctx, width:rect.width, height:rect.height};
  }

  function niceCeiling(value, minimum) {
    if (!value || value < minimum) return minimum;
    const step = Math.pow(10, Math.floor(Math.log10(value / 4)));
    return Math.ceil(value / step) * step;
  }

  const chartSeries={
    speed:{label:'Velocidade',unit:'km/h',digits:1,color:'#60A5FA',empty:'Aguardando leituras de velocidade.'},
    accel:{label:'Aceleração',unit:'m/s²',digits:2,color:'#E9B66A',empty:'Aguardando leituras de aceleração.'},
    power:{label:'Potência elétrica',unit:'W',digits:0,color:'#79C9A3',empty:'Potência disponível apenas quando informada pela simulação.'},
    battery:{label:'Carga da bateria',unit:'%',digits:1,color:'#9AB8E5',empty:'Carga disponível apenas quando informada pela simulação.'}
  };
  function drawChart(slot) {
    const key=slot==='speed'?'speed':state.chartSignal,series=chartSeries[key];
    const setup = chartSetup($(slot + '-chart'));
    if (!setup) return;
    const {ctx, width:w, height:h} = setup;
    const plot = {left:46, right:w - 13, top:13, bottom:h - 28};
    const end = Date.now(), start = end - state.rangeMinutes * 60000;
    const points = state.samples.filter(sample => sample.time >= start && sample.time <= end);
    const numeric = points.map(point => point[key]).filter(finite);
    $(slot + '-empty').hidden = numeric.length > 0;
    const highest = numeric.length ? Math.max(0, ...numeric) : 0;
    const lowest = numeric.length ? Math.min(0, ...numeric) : 0;
    let limit = state.chartLimits[key];
    if(key==='speed'){
      limit={min:0,max:Math.max(30,Math.ceil(highest*1.08/5)*5)};
      $('speed-chart-scale').textContent='Escala de 0 a '+limit.max+' km/h · ajustada às leituras visíveis';
      $('speed-chart').setAttribute('aria-label','Gráfico da velocidade ao longo do tempo, escala de 0 a '+limit.max+' quilômetros por hora');
    }
    else if(key==='battery')limit={min:0,max:100};
    else if(key==='accel'){
      const peak=niceCeiling(Math.max(Math.abs(highest),Math.abs(lowest))*1.12,1);
      limit={min:Math.min(limit ? limit.min : -1,-peak),max:Math.max(limit ? limit.max : 1,peak)};
    }else{
      if(!limit)limit={min:0,max:100};
      limit.max=Math.max(limit.max,niceCeiling(highest*1.12,100));
      if(lowest<0)limit.min=Math.min(limit.min,-niceCeiling(Math.abs(lowest)*1.12,100));
    }
    state.chartLimits[key] = limit;
    const x = time => plot.left + (time - start) / (end - start) * (plot.right - plot.left);
    const y = value => Math.max(plot.top,Math.min(plot.bottom,plot.bottom - (value - limit.min) / (limit.max - limit.min) * (plot.bottom - plot.top)));
    ctx.font = '11px Inter, Segoe UI, sans-serif';
    ctx.lineWidth = 1;
    for (let index = 0; index <= 4; index++) {
      const yy = plot.top + (plot.bottom - plot.top) * index / 4;
      ctx.strokeStyle = '#2B3E59'; ctx.beginPath(); ctx.moveTo(plot.left, yy); ctx.lineTo(plot.right, yy); ctx.stroke();
      ctx.fillStyle = '#A9B7CC'; ctx.textAlign = 'right';
      ctx.fillText(format(limit.max - (limit.max - limit.min) * index / 4, key==='accel'?1:0), plot.left - 7, yy + 4);
    }
    for (let index = 0; index <= 2; index++) {
      const xx = plot.left + (plot.right - plot.left) * index / 2;
      ctx.strokeStyle = '#22354D'; ctx.beginPath(); ctx.moveTo(xx, plot.top); ctx.lineTo(xx, plot.bottom); ctx.stroke();
      ctx.fillStyle = '#A9B7CC'; ctx.textAlign = index === 0 ? 'left' : index === 2 ? 'right' : 'center';
      ctx.fillText(new Date(start + (end - start) * index / 2).toLocaleTimeString('pt-BR', {hour:'2-digit', minute:'2-digit'}), xx, h - 8);
    }
    ctx.strokeStyle = series.color; ctx.lineWidth = 2.3; ctx.lineJoin = 'round'; ctx.lineCap = 'round';
    ctx.save();ctx.beginPath();ctx.rect(plot.left,plot.top,plot.right-plot.left,plot.bottom-plot.top);ctx.clip();
    let open = false;
    for (const point of points) {
      if (!finite(point[key])) {if (open) ctx.stroke(); open = false; continue;}
      if (!open) {ctx.beginPath(); ctx.moveTo(x(point.time), y(point[key])); open = true;}
      else ctx.lineTo(x(point.time), y(point[key]));
    }
    if (open) ctx.stroke();
    if (numeric.length === 1) {const point = points.find(item => finite(item[key]));ctx.fillStyle = series.color;ctx.beginPath();ctx.arc(x(point.time), y(point[key]), 3, 0, Math.PI * 2);ctx.fill();}
    const hover = state.hovering[slot];
    if (hover && finite(hover[key]) && hover.time >= start && hover.time <= end) {ctx.fillStyle = series.color;ctx.beginPath();ctx.arc(x(hover.time), y(hover[key]), 4, 0, Math.PI * 2);ctx.fill();}
    ctx.restore();
    state[slot + 'ChartGeometry'] = {start,end,plot,x,y,key};
  }

  function drawCharts() {
    const series=chartSeries[state.chartSignal];
    $('secondary-chart-title').textContent=series.label;
    $('secondary-chart-unit').textContent=series.unit;
    $('power-empty').textContent=series.empty;
    $('power-chart').setAttribute('aria-label','Gráfico de '+series.label.toLowerCase()+' ao longo do tempo');
    drawChart('speed');drawChart('power');
  }

  function showTooltip(slot, event) {
    const geometry = state[slot + 'ChartGeometry'];
    if (!geometry) return;
    const key=geometry.key,series=chartSeries[key];
    const canvas = $(slot + '-chart'), rect = canvas.getBoundingClientRect();
    const px = Math.max(geometry.plot.left, Math.min(geometry.plot.right, event.clientX - rect.left));
    const time = geometry.start + (px - geometry.plot.left) / (geometry.plot.right - geometry.plot.left) * (geometry.end - geometry.start);
    const points = state.samples.filter(point => finite(point[key]) && point.time >= geometry.start && point.time <= geometry.end);
    if (!points.length) return;
    const nearest = points.reduce((best, point) => Math.abs(point.time - time) < Math.abs(best.time - time) ? point : best);
    const tip = $(slot + '-tip');
    tip.textContent = new Date(nearest.time).toLocaleTimeString('pt-BR') + ' · ' + format(nearest[key],series.digits) + ' ' + series.unit;
    tip.hidden = false;
    tip.style.left = Math.max(4, Math.min(rect.width - 190, geometry.x(nearest.time) - 82)) + 'px';
    tip.style.top = Math.max(4, geometry.y(nearest[key]) - 40) + 'px';
    state.hovering[slot] = nearest;
    drawChart(slot);
  }

  ['speed','power'].forEach(key => {
    const canvas = $(key + '-chart');
    canvas.addEventListener('pointermove', event => showTooltip(key, event));
    let touchTimer;
    canvas.addEventListener('pointerdown', event => {
      showTooltip(key, event);
      if (event.pointerType === 'touch') {
        clearTimeout(touchTimer);
        touchTimer = setTimeout(() => {$(key + '-tip').hidden = true;state.hovering[key] = null;drawChart(key);}, 4000);
      }
    });
    canvas.addEventListener('pointerleave', event => {
      if (event.pointerType === 'touch') return;
      $(key + '-tip').hidden = true;state.hovering[key] = null;drawChart(key);
    });
  });
  document.querySelectorAll('.range').forEach(button => button.addEventListener('click', () => {
    state.rangeMinutes = Number(button.dataset.minutes);
    state.chartLimits = {};
    document.querySelectorAll('.range').forEach(other => {const active = other === button;other.classList.toggle('active', active);other.setAttribute('aria-pressed', String(active));});
    drawCharts();
  }));
  $('visitor-chart-signal').addEventListener('change',event=>{state.chartSignal=event.target.value;state.hovering.power=null;$('power-tip').hidden=true;drawCharts();});
  window.addEventListener('resize', drawCharts);
  document.addEventListener('visibilitychange', () => {if (!document.hidden) poll();});
  updateAge();
  drawCharts();
  poll();
  setInterval(poll, 500);
  setInterval(() => {updateAge();drawCharts();}, 1000);
})();
