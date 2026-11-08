(() => {
  'use strict';

  const $ = id => document.getElementById(id);
  const format = (value, digits = 1) => value.toLocaleString('pt-BR', {minimumFractionDigits:digits, maximumFractionDigits:digits});
  const finite = value => typeof value === 'number' && Number.isFinite(value);
  const speedGaugeMax = 35;
  const consumptionMinMeters = 20;
  function updatePedal(id, value) {
    const valid = finite(value) && value >= 0 && value <= 100;
    animateValue(id + '-value', valid ? value : null, 0, '', '%', current => {
      $(id + '-fill').style.setProperty('--fill-level', Math.max(0, Math.min(100, current || 0)) + '%');
    });
  }
  const state = {
    lastData:null, lastSampleAt:0, firstSampleAt:0, lastSequence:null,
    lastArrivalAt:0, observedPeriodMs:500,
    previousSpeedForTone:null, speedToneTimer:null,
    samples:[], gapOpen:false, rangeMinutes:1, chartSignal:'power-current',
    firstDistance:null, previousEnergy:null, energyWh:null,
    consumptionWh:0, consumptionMeters:0, powerRailLimit:500,
    chartLimits:{}, hovering:{}, fetching:false,
  };

  function setValue(id, text, unit = '') {
    const el = $(id);
    if (el.textContent !== text) el.textContent = text;
    el.classList.toggle('empty', text === 'Sem leitura');
    if (id === 'speed-value') el.parentElement.classList.toggle('no-reading', text === 'Sem leitura');
    const unitEl = $(id.replace('-value', '-unit'));
    if (unitEl && unitEl.textContent !== unit) unitEl.textContent = unit;
  }

  const animatedValues = {};
  const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)');
  function animateValue(id, numeric, digits = 1, unit = '', suffix = '', onFrame = () => {}) {
    const previous = animatedValues[id];
    if (previous?.frame) cancelAnimationFrame(previous.frame);
    if (!finite(numeric)) {
      delete animatedValues[id];
      setValue(id, 'Sem leitura');
      onFrame(null);
      return;
    }
    const slot = {current: previous?.current ?? numeric, unit, suffix, frame: null};
    animatedValues[id] = slot;
    const render = current => {slot.current = current; setValue(id, format(current, digits) + suffix, unit); onFrame(current);};
    if (!previous || previous.unit !== unit || previous.suffix !== suffix || reducedMotion.matches || document.hidden || Math.abs(slot.current - numeric) < .5 / Math.pow(10, digits)) {
      render(numeric);
      return;
    }
    const start = slot.current, started = performance.now();
    const duration = Math.max(450, Math.min(850, state.observedPeriodMs * 1.15));
    function step(now) {
      const progress = Math.min(1, (now - started) / duration);
      const eased = Math.sin(progress * Math.PI / 2);
      render(start + (numeric - start) * eased);
      if (progress < 1) slot.frame = requestAnimationFrame(step);
      else slot.frame = null;
    }
    slot.frame = requestAnimationFrame(step);
  }
  function clearAnimatedValues() {
    Object.keys(animatedValues).forEach(id => {if (animatedValues[id].frame) cancelAnimationFrame(animatedValues[id].frame); delete animatedValues[id];});
  }

  const speedPeak = {last:null,lastAt:0,candidate:0,candidateAt:0,armed:true,trough:0,cooldownUntil:0,timer:null,labelTimer:null};
  function hideSpeedPeak() {
    clearTimeout(speedPeak.timer);
    clearTimeout(speedPeak.labelTimer);
    speedPeak.timer=null;speedPeak.labelTimer=null;
    $('speed-peak-marker').classList.remove('is-visible');
    $('speed-peak-label').classList.remove('visible');
  }
  function resetSpeedPeak() {
    hideSpeedPeak();
    Object.assign(speedPeak,{last:null,lastAt:0,candidate:0,candidateAt:0,armed:true,trough:0,cooldownUntil:0});
  }
  function showSpeedPeak(value) {
    const angle=Math.PI*(1-Math.min(1,value/speedGaugeMax));
    const point=radius=>[(160+radius*Math.cos(angle)).toFixed(2),(165-radius*Math.sin(angle)).toFixed(2)];
    const from=point(113.5),to=point(119);
    const marker=$('speed-peak-marker');
    marker.querySelectorAll('line').forEach(line=>{
      line.setAttribute('x1',from[0]);line.setAttribute('y1',from[1]);
      line.setAttribute('x2',to[0]);line.setAttribute('y2',to[1]);
    });
    marker.classList.remove('is-visible');
    void marker.getBoundingClientRect();
    marker.classList.add('is-visible');
    $('speed-peak-label').textContent='Pico '+format(value,1)+' km/h';
    $('speed-peak-label').classList.add('visible');
    clearTimeout(speedPeak.timer);
    clearTimeout(speedPeak.labelTimer);
    speedPeak.labelTimer=setTimeout(()=>$('speed-peak-label').classList.remove('visible'),1050);
    speedPeak.timer=setTimeout(hideSpeedPeak,1600);
  }
  function trackSpeedPeak(speed) {
    const now=performance.now();
    if(!finite(speed)){resetSpeedPeak();return;}
    if(speedPeak.last===null||now-speedPeak.lastAt>2500){
      resetSpeedPeak();
      Object.assign(speedPeak,{last:speed,lastAt:now,candidate:speed,candidateAt:now,trough:speed});
      return;
    }
    const dropRate=(speedPeak.last-speed)*1000/Math.max(150,now-speedPeak.lastAt);
    if(speedPeak.armed){
      if(now-speedPeak.candidateAt>3500){speedPeak.candidate=speedPeak.last;speedPeak.candidateAt=speedPeak.lastAt;}
      if(speed>speedPeak.candidate){speedPeak.candidate=speed;speedPeak.candidateAt=now;}
      else if(speedPeak.candidate>=5&&speedPeak.candidate-speed>=1.4&&dropRate>=2.3&&now>=speedPeak.cooldownUntil){
        showSpeedPeak(speedPeak.candidate);
        speedPeak.armed=false;speedPeak.trough=speed;speedPeak.cooldownUntil=now+3800;
      }
    }else{
      speedPeak.trough=Math.min(speedPeak.trough,speed);
      if(now>=speedPeak.cooldownUntil&&speed-speedPeak.trough>=2){speedPeak.armed=true;speedPeak.candidate=speed;speedPeak.candidateAt=now;}
    }
    speedPeak.last=speed;speedPeak.lastAt=now;
  }

  const powerPeak={candidate:null,candidateAt:0,lastAt:0,cooldownUntil:0,timer:null};
  function hidePowerPeak() {
    clearTimeout(powerPeak.timer);
    powerPeak.timer=null;
    $('power-peak-marker').classList.remove('is-visible');
  }
  function resetPowerPeak() {
    hidePowerPeak();
    Object.assign(powerPeak,{candidate:null,candidateAt:0,lastAt:0,cooldownUntil:0});
  }
  function showPowerPeak(value) {
    const marker=$('power-peak-marker');
    marker.style.setProperty('--power-peak-position',(50+Math.max(-1,Math.min(1,value/state.powerRailLimit))*50)+'%');
    marker.classList.toggle('is-regen',value<0);
    marker.classList.remove('is-visible');
    void marker.getBoundingClientRect();
    marker.classList.add('is-visible');
    clearTimeout(powerPeak.timer);
    powerPeak.timer=setTimeout(hidePowerPeak,1550);
  }
  function trackPowerPeak(value) {
    const now=performance.now();
    if(!finite(value)){resetPowerPeak();return;}
    if(powerPeak.candidate===null||now-powerPeak.lastAt>2500){
      resetPowerPeak();
      powerPeak.candidate=value;powerPeak.candidateAt=now;powerPeak.lastAt=now;
      return;
    }
    const candidate=powerPeak.candidate;
    const signChanged=Math.sign(value)!==Math.sign(candidate)&&Math.abs(value)>20;
    const fell=Math.abs(candidate)-Math.abs(value)>=Math.max(55,Math.abs(candidate)*.25);
    if(Math.abs(candidate)>=120&&(fell||signChanged)&&now>=powerPeak.cooldownUntil){
      showPowerPeak(candidate);
      powerPeak.cooldownUntil=now+3500;
      powerPeak.candidate=value;powerPeak.candidateAt=now;
    }else if(signChanged||Math.abs(value)>Math.abs(candidate)||now-powerPeak.candidateAt>3500){
      powerPeak.candidate=value;powerPeak.candidateAt=now;
    }
    powerPeak.lastAt=now;
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
    clearAnimatedValues();
    resetSpeedPeak();
    resetPowerPeak();
    clearTimeout(state.speedToneTimer);
    state.speedToneTimer=null;
    state.previousSpeedForTone=null;
    $('speed-card').classList.remove('is-slowing');
    state.samples = [];
    state.gapOpen = false;
    state.firstSampleAt = 0;
    state.firstDistance = null;
    state.previousEnergy = null;
    state.energyWh = null;
    state.consumptionWh = 0;
    state.consumptionMeters = 0;
    state.chartLimits = {};
    state.lastArrivalAt = 0;
    state.observedPeriodMs = 500;
    state.powerRailLimit = 500;
  }

  function addGap() {
    if (state.gapOpen || !state.samples.length) return;
    state.samples.push({time:Date.now(), speed:null, power:null, accel:null, battery:null, current:null, voltage:null});
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

    const previousSpeed=state.previousSpeedForTone;
    if(finite(speed)&&finite(previousSpeed)&&previousSpeed-speed>=.18){
      $('speed-card').classList.add('is-slowing');
      clearTimeout(state.speedToneTimer);
      state.speedToneTimer=setTimeout(()=>$('speed-card').classList.remove('is-slowing'),950);
    }else if(!finite(speed)||!finite(previousSpeed)||speed-previousSpeed>=.12||speed<.5){
      clearTimeout(state.speedToneTimer);
      state.speedToneTimer=null;
      $('speed-card').classList.remove('is-slowing');
    }
    state.previousSpeedForTone=speed;
    trackSpeedPeak(speed);
    animateValue('speed-value', speed, 1, '', '', current => {
      const fraction = current === null ? 0 : Math.min(1, Math.max(0, current / speedGaugeMax));
      $('speed-arc').style.strokeDasharray = (fraction * 100) + ' 101';
      $('speed-arc').style.opacity = fraction === 0 ? '0' : String(Math.min(1, fraction * 70));
    });
    $('motion-state').textContent = speed === null ? 'Aguardando dados' : speed < 0.5 ? 'Parado' : 'Em movimento';
    $('speed-source').textContent = simulated ? 'Leitura simulada' : 'Leitura do veículo';
    updatePedal('throttle', readNumber(data, 'acelerador_decimos_percentual', 10));
    updatePedal('brake', readNumber(data, 'freio_decimos_percentual', 10));

    animateValue('battery-value', charge, 1, charge === null ? '' : '%', '', current => {
      $('battery-fill').style.width = current === null ? '0%' : Math.max(0, Math.min(100, current)) + '%';
    });
    $('battery-hint').textContent = simulated ? 'Carga estimada pela simulação.' : 'Sem medição validada no modo real.';

    const powerUnit = power === null ? '' : Math.abs(power) >= 1000 ? 'kW' : 'W';
    animateValue('power-value', power === null ? null : powerUnit === 'kW' ? power / 1000 : power, powerUnit === 'kW' ? 2 : 0, powerUnit,'',current=>{
      const watts=current===null?null:powerUnit==='kW'?current*1000:current;
      $('power-hint').textContent=watts===null?'Potência elétrica sem leitura no modo real.':watts<-.5?'Simulação · devolução de energia ao sistema.':watts>.5?'Simulação · consumo elétrico, não potência nas rodas.':'Simulação · potência próxima de zero.';
    });
    const rail=$('power-rail');
    $('power-motion-card').hidden=power===null;
    document.querySelector('.primary-stack').classList.toggle('has-power',power!==null);
    if(power!==null){
      const limit=niceCeiling(Math.abs(power)*1.12,500);
      if(limit>state.powerRailLimit){state.powerRailLimit=limit;hidePowerPeak();}
      const scaleText=state.powerRailLimit>=1000?format(state.powerRailLimit/1000,1)+' kW':format(state.powerRailLimit,0)+' W';
      $('power-rail-negative-limit').textContent='−'+scaleText;
      $('power-rail-positive-limit').textContent=scaleText;
    }
    trackPowerPeak(power);
    animateValue('power-rail-value',power===null?null:powerUnit==='kW'?power/1000:power,powerUnit==='kW'?2:0,'',' '+powerUnit,current=>{
      const watts=current===null?0:powerUnit==='kW'?current*1000:current;
      const extent=Math.min(100,Math.abs(watts)/state.powerRailLimit*100)+'%';
      $('power-rail-consume').style.setProperty('--power-positive',watts>0?extent:'0%');
      $('power-rail-regen').style.setProperty('--power-negative',watts<0?extent:'0%');
      const flow=watts<-.5?'Regeneração':watts>.5?'Consumo':'Sem fluxo';
      $('power-flow').textContent=flow;
      $('power-motion-card').classList.toggle('is-regen',watts<-.5);
      rail.setAttribute('aria-label',flow+' · '+format(Math.abs(watts),0)+' watts');
    });

    animateValue('range-value', range, 1, range === null ? '' : 'km');
    $('range-hint').textContent = range === null ? 'Estimativa não disponível.' : simulated ? 'Autonomia simulada.' : 'Estimativa informada pelo veículo.';
    const consumption = power !== null && state.consumptionMeters >= consumptionMinMeters
      ? state.consumptionWh / (state.consumptionMeters / 1000) : null;
    animateValue('consumption-value', consumption, 1, consumption === null ? '' : 'Wh/km');
    $('consumption-hint').textContent = power === null ? 'Aguardando leituras de energia.' : consumption === null
      ? 'Aguardando ' + consumptionMinMeters + ' m com energia e percurso.'
      : 'Trechos acompanhados nesta página · sem descontar regeneração.';

    animateValue('accel-value', accel, 2, accel === null ? '' : 'm/s²','',current=>{
      $('accel-card').classList.toggle('is-negative',current!==null&&current<-.005);
      $('accel-hint').textContent=current===null?'Aguardando leitura válida do sensor.':simulated?'Dados simulados.':current>.15?'Acelerando · leitura do sensor.':current<-.15?'Desacelerando · leitura do sensor.':'Velocidade estável · leitura do sensor.';
    });

    const distance = readNumber(data, 'percurso_metros');
    animateValue('trip-value', distance === null ? null : distance / 1000, 2, distance === null ? '' : 'km');
    $('trip-hint').textContent = simulated ? 'Percurso simulado.' : 'Percurso registrado pelo veículo.';
    const hasElectricData = charge !== null || power !== null;
    $('energy-live-grid').hidden = !hasElectricData && range === null;
    $('energy-unavailable').hidden = hasElectricData || range !== null;
    document.querySelectorAll('.electric-detail').forEach(card => {card.hidden = !hasElectricData;});
    document.querySelector('#details-title').closest('.section').querySelector('.detail-grid').classList.toggle('compact',!hasElectricData);
    $('electric-chart-card').hidden = !hasElectricData;
    $('visitor-chart-grid').classList.toggle('single', !hasElectricData);
    if (!hasElectricData) $('energy-unavailable').textContent = 'O veículo ainda não envia leituras de bateria e potência.';

    animateValue('voltage-value', voltage === null || voltage <= 0 ? null : voltage, 2, '', ' V');
    $('voltage-hint').textContent = simulated ? 'Valor simulado.' : 'Sem medição no modo real.';
    animateValue('current-value', current, 2, '', ' A');
    $('current-hint').textContent = simulated ? 'Valor simulado.' : 'Sem medição no modo real.';
    animateValue('energy-value', state.energyWh, 1, '', ' Wh');
    if (distance !== null) {
      if (state.firstDistance === null || distance < state.firstDistance) state.firstDistance = distance;
      animateValue('distance-value', Math.max(0, distance - state.firstDistance) / 1000, 2, '', ' km');
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
    if (state.lastArrivalAt) {
      const period = now - state.lastArrivalAt;
      if (period >= 200 && period <= 1800) state.observedPeriodMs = state.observedPeriodMs * .75 + period * .25;
    }
    state.lastArrivalAt = now;
    if (!state.firstSampleAt) state.firstSampleAt = now;
    const power = data.simulado ? readNumber(data, 'potencia_w') : null;
    accumulateEnergy(power, distance, now);
    state.samples.push({time:now, speed:readNumber(data, 'velocidade_centesimos_kmh', 100), power,accel:data.aceleracao_disponivel?readNumber(data,'aceleracao_milesimos_ms2',1000):null,battery:data.simulado?readNumber(data,'carga_decimos_percentual',10):null,current:data.simulado?readNumber(data,'corrente_centesimos_a',100):null,voltage:data.simulado?readNumber(data,'tensao_pacote_mv',1000):null});
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
    speed:{label:'Velocidade',unit:'km/h',digits:1,axisDigits:0,color:'#63d9f2'},
    accel:{label:'Aceleração',unit:'m/s²',digits:2,axisDigits:1,color:'#f4bf70'},
    power:{label:'Potência',unit:'W',digits:0,axisDigits:0,color:'#78d7ae'},
    current:{label:'Corrente',unit:'A',digits:2,axisDigits:1,color:'#f4bf70'},
    voltage:{label:'Tensão',unit:'V',digits:1,axisDigits:0,color:'#9bbcf3'},
    battery:{label:'Carga',unit:'%',digits:1,axisDigits:0,color:'#9bbcf3'}
  };
  const chartPairs={
    'power-current':['power','current'],
    'voltage-current':['voltage','current'],
    'power-voltage':['power','voltage'],
    'battery-power':['battery','power']
  };
  function chartLimit(key, points) {
    const numeric=points.map(point=>point[key]).filter(finite);
    const highest=numeric.length?Math.max(0,...numeric):0;
    const lowest=numeric.length?Math.min(0,...numeric):0;
    if(key==='speed')return {min:0,max:Math.max(30,Math.ceil(highest*1.08/5)*5)};
    if(key==='battery')return {min:0,max:100};
    if(key==='accel'){
      const peak=niceCeiling(Math.max(Math.abs(highest),Math.abs(lowest))*1.12,1);
      return {min:-peak,max:peak};
    }
    const base=key==='voltage'?10:1;
    return {min:lowest<0?-niceCeiling(Math.abs(lowest)*1.12,base):0,max:niceCeiling(highest*1.12,base)};
  }
  function drawChart(slot) {
    const keys=slot==='speed'?['speed','accel']:chartPairs[state.chartSignal];
    const setup=chartSetup($(slot+'-chart'));
    if(!setup)return;
    const {ctx,width:w,height:h}=setup;
    const plot={left:53,right:w-50,top:27,bottom:h-27};
    const end=Date.now(),start=end-state.rangeMinutes*60000;
    const points=state.samples.filter(point=>point.time>=start&&point.time<=end);
    const limits=keys.map(key=>chartLimit(key,points));
    const x=time=>plot.left+(time-start)/(end-start)*(plot.right-plot.left);
    const y=(value,index)=>Math.max(plot.top,Math.min(plot.bottom,plot.bottom-(value-limits[index].min)/(limits[index].max-limits[index].min)*(plot.bottom-plot.top)));
    $(slot+'-empty').hidden=points.some(point=>keys.some(key=>finite(point[key])));
    ctx.font='11px Inter, Segoe UI, sans-serif';ctx.lineWidth=1;
    keys.forEach((key,index)=>{
      const series=chartSeries[key];
      ctx.fillStyle=series.color;ctx.textAlign=index?'right':'left';
      ctx.fillText(series.unit,index?w-2:2,14);
    });
    for(let index=0;index<=4;index++){
      const yy=plot.top+(plot.bottom-plot.top)*index/4;
      ctx.strokeStyle='#385365';ctx.beginPath();ctx.moveTo(plot.left,yy);ctx.lineTo(plot.right,yy);ctx.stroke();
      keys.forEach((key,axis)=>{
        const series=chartSeries[key],limit=limits[axis];
        const value=limit.max-(limit.max-limit.min)*index/4;
        ctx.fillStyle=series.color;ctx.textAlign=axis?'left':'right';
        ctx.fillText(format(value,series.axisDigits),axis?plot.right+7:plot.left-7,yy+4);
      });
    }
    for(let index=0;index<=2;index++){
      const xx=plot.left+(plot.right-plot.left)*index/2;
      ctx.strokeStyle='#2b4354';ctx.beginPath();ctx.moveTo(xx,plot.top);ctx.lineTo(xx,plot.bottom);ctx.stroke();
      ctx.fillStyle='#b7cbd4';ctx.textAlign=index===0?'left':index===2?'right':'center';
      ctx.fillText(new Date(start+(end-start)*index/2).toLocaleTimeString('pt-BR',{hour:'2-digit',minute:'2-digit'}),xx,h-7);
    }
    ctx.save();ctx.beginPath();ctx.rect(plot.left,plot.top,plot.right-plot.left,plot.bottom-plot.top);ctx.clip();
    keys.forEach((key,index)=>{
      const series=chartSeries[key];
      ctx.strokeStyle=series.color;ctx.lineWidth=2.4;ctx.lineJoin='round';ctx.lineCap='round';
      let open=false,count=0,only=null;
      for(const point of points){
        if(!finite(point[key])){if(open)ctx.stroke();open=false;continue;}
        count++;only=point;
        if(!open){ctx.beginPath();ctx.moveTo(x(point.time),y(point[key],index));open=true;}
        else ctx.lineTo(x(point.time),y(point[key],index));
      }
      if(open)ctx.stroke();
      const hover=state.hovering[slot];
      const dot=hover&&finite(hover[key])&&hover.time>=start&&hover.time<=end?hover:count===1?only:null;
      if(dot){ctx.fillStyle=series.color;ctx.beginPath();ctx.arc(x(dot.time),y(dot[key],index),hover===dot?4:3,0,Math.PI*2);ctx.fill();}
    });
    ctx.restore();
    state[slot+'ChartGeometry']={start,end,plot,x,y,keys};
  }

  function drawCharts() {
    const keys=chartPairs[state.chartSignal];
    $('electric-chart-legend').innerHTML=keys.map(key=>'<span style="--series-color:'+chartSeries[key].color+'">'+chartSeries[key].label+' · '+chartSeries[key].unit+'</span>').join('');
    $('power-chart').setAttribute('aria-label','Gráfico de '+keys.map(key=>chartSeries[key].label.toLowerCase()).join(' e ')+' ao longo do tempo');
    drawChart('speed');drawChart('power');
  }

  function showTooltip(slot,event) {
    const geometry=state[slot+'ChartGeometry'];
    if(!geometry)return;
    const canvas=$(slot+'-chart'),rect=canvas.getBoundingClientRect();
    const px=Math.max(geometry.plot.left,Math.min(geometry.plot.right,event.clientX-rect.left));
    const time=geometry.start+(px-geometry.plot.left)/(geometry.plot.right-geometry.plot.left)*(geometry.end-geometry.start);
    const points=state.samples.filter(point=>geometry.keys.some(key=>finite(point[key]))&&point.time>=geometry.start&&point.time<=geometry.end);
    if(!points.length)return;
    const nearest=points.reduce((best,point)=>Math.abs(point.time-time)<Math.abs(best.time-time)?point:best);
    const tip=$(slot+'-tip');
    tip.textContent=new Date(nearest.time).toLocaleTimeString('pt-BR')+'\n'+geometry.keys.map(key=>{
      const series=chartSeries[key];
      return series.label+': '+(finite(nearest[key])?format(nearest[key],series.digits)+' '+series.unit:'sem leitura');
    }).join('\n');
    tip.hidden=false;
    tip.style.left=Math.max(4,Math.min(rect.width-204,geometry.x(nearest.time)-75))+'px';
    tip.style.top='32px';
    state.hovering[slot]=nearest;
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
  const infoDisclosures=[...document.querySelectorAll('.info-disclosure')];
  infoDisclosures.forEach(item=>item.addEventListener('toggle',()=>{
    if(item.open)infoDisclosures.forEach(other=>{if(other!==item)other.open=false;});
  }));
  document.addEventListener('pointerdown',event=>{
    if(!event.target.closest('.info-disclosure'))infoDisclosures.forEach(item=>{item.open=false;});
  });
  document.addEventListener('keydown',event=>{
    if(event.key==='Escape')infoDisclosures.forEach(item=>{item.open=false;});
  });
  window.addEventListener('resize', drawCharts);
  document.addEventListener('visibilitychange', () => {if (!document.hidden) poll();});
  updateAge();
  drawCharts();
  poll();
  setInterval(poll, 500);
  setInterval(() => {updateAge();drawCharts();}, 1000);
})();
