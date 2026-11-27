(() => {
  'use strict';

  const $ = id => document.getElementById(id);
  const format = (value, digits = 1) => value.toLocaleString('pt-BR', {minimumFractionDigits:digits, maximumFractionDigits:digits});
  const finite = value => typeof value === 'number' && Number.isFinite(value);
  const speedScaleMax = 35;
  const powerVisualMaxW = 1000;
  const consumptionMinMeters = 20;

  function setScaleFill(id, value, maximum = 100) {
    const percentage = value === null ? 0 : Math.min(100, Math.max(0, value / maximum * 100));
    $(id).style.setProperty('--fill-level', percentage + '%');
  }

  function updatePedal(id, value) {
    const valid = finite(value) && value >= 0 && value <= 100;
    animateValue(id + '-value', valid ? value : null, 0, '', '%', current => {
      setScaleFill(id + '-fill', current);
    });
  }
  const state = {
    lastData:null, lastSampleAt:0, lastSequence:null,
    lastArrivalAt:0, observedPeriodMs:500,
    previousSpeedForTone:null, speedToneTimer:null,
    samples:[], gapOpen:false, rangeMinutes:1, electricKeys:['power','current'],
    previousEnergy:null,
    consumptionWh:0, consumptionMeters:0,
    hovering:{}, fetching:false,
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

  function hidePeak(peak) {
    clearTimeout(peak.timer);
    peak.timer = null;
    $(peak.markerId).classList.remove('is-visible');
  }

  function showPeak(peak, percentage, regenerating = false) {
    const marker = $(peak.markerId);
    const position = Math.min(100, Math.max(0, percentage));
    marker.style.setProperty('--peak-position', position + '%');
    marker.style.setProperty('--peak-duration', peak.durationMs + 'ms');
    marker.classList.toggle('is-regen', regenerating);
    marker.classList.remove('is-visible');
    void marker.getBoundingClientRect();
    marker.classList.add('is-visible');
    clearTimeout(peak.timer);
    peak.timer = setTimeout(() => hidePeak(peak), peak.durationMs);
  }

  const speedPeak = {
    markerId:'speed-peak-marker', durationMs:1600, timer:null,
    last:null, lastAt:0, candidate:0, candidateAt:0, armed:true, trough:0, cooldownUntil:0,
  };

  function resetSpeedPeak() {
    hidePeak(speedPeak);
    Object.assign(speedPeak,{last:null,lastAt:0,candidate:0,candidateAt:0,armed:true,trough:0,cooldownUntil:0});
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
        showPeak(speedPeak, speedPeak.candidate / speedScaleMax * 100);
        speedPeak.armed=false;speedPeak.trough=speed;speedPeak.cooldownUntil=now+3800;
      }
    }else{
      speedPeak.trough=Math.min(speedPeak.trough,speed);
      if(now>=speedPeak.cooldownUntil&&speed-speedPeak.trough>=2){speedPeak.armed=true;speedPeak.candidate=speed;speedPeak.candidateAt=now;}
    }
    speedPeak.last=speed;speedPeak.lastAt=now;
  }

  const powerPeak = {
    markerId:'power-peak-marker', durationMs:1000, timer:null,
    candidate:null, candidateAt:0, lastAt:0, cooldownUntil:0,
  };

  function resetPowerPeak() {
    hidePeak(powerPeak);
    Object.assign(powerPeak,{candidate:null,candidateAt:0,lastAt:0,cooldownUntil:0});
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
    if(Math.abs(candidate)>=120&&fell&&now>=powerPeak.cooldownUntil){
      showPeak(powerPeak, Math.abs(candidate) / powerVisualMaxW * 100, candidate < 0);
      powerPeak.cooldownUntil=now+2500;
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
    const lastUpdate = $('last-update');
    lastUpdate.hidden = !state.lastSampleAt;
    if (state.lastSampleAt) {
      const age = elapsedText(Date.now() - state.lastSampleAt);
      const heldSimulation = state.lastData?.simulado === true && document.body.dataset.feed !== 'demo';
      lastUpdate.textContent = (heldSimulation ? 'Simulação · ' : 'Última leitura ') + age;
    }
    if (state.lastSampleAt && Date.now() - state.lastSampleAt > 2500 && !state.gapOpen) addGap();
    updateVehicleMotion();
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
    state.previousEnergy = null;
    state.consumptionWh = 0;
    state.consumptionMeters = 0;
    state.hovering = {};
    $('speed-tip').hidden = true;
    $('power-tip').hidden = true;
    state.lastArrivalAt = 0;
    state.observedPeriodMs = 500;
  }

  function addGap() {
    if (state.gapOpen || !state.samples.length) return;
    state.samples.push({time:Date.now()});
    state.gapOpen = true;
    state.previousEnergy = null;
    drawCharts();
  }

  function readNumber(data, key, divisor = 1) {
    return finite(data[key]) ? data[key] / divisor : null;
  }

  const trackMotion = $('vehicle-track-lines').animate?.(
    [{transform: 'translateX(0)'}, {transform: 'translateX(var(--track-step))'}],
    {duration: 540, iterations: Infinity}
  );
  trackMotion?.pause();

  function updateVehicleMotion() {
    if (!trackMotion) return;

    const feed = document.body.dataset.feed;
    const speed = state.lastData ? readNumber(state.lastData, 'velocidade_centesimos_kmh', 100) : null;
    const fresh = (feed === 'live' || feed === 'demo') && Date.now() - state.lastSampleAt < 2000;
    if (!fresh || speed === null || speed < 0.5 || reducedMotion.matches || document.hidden) {
      if (trackMotion.playState !== 'paused') trackMotion.pause();
      return;
    }

    const rate = speed / 12;
    if (trackMotion.playbackRate !== rate) {
      if (trackMotion.updatePlaybackRate) trackMotion.updatePlaybackRate(rate);
      else trackMotion.playbackRate = rate;
    }
    if (trackMotion.playState !== 'running') trackMotion.play();
  }

  function updateReadings(data, sample, distance) {
    const {speed, power, battery:charge, voltage, current, range, consumption} = sample;

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
      setScaleFill('speed-fill', current, speedScaleMax);
    });
    $('motion-state').textContent = speed === null ? 'Sem leitura' : speed < 0.5 ? 'Parado' : 'Em movimento';
    updatePedal('throttle', readNumber(data, 'acelerador_decimos_percentual', 10));
    updatePedal('brake', readNumber(data, 'freio_decimos_percentual', 10));

    animateValue('battery-value', charge, 2, charge === null ? '' : '%', '', current => {
      setScaleFill('battery-fill', current);
    });
    $('battery-hint').textContent = charge === null ? '' : 'Estimativa';

    const powerUnit = power === null ? '' : Math.abs(power) >= 1000 ? 'kW' : 'W';
    const powerScale = powerUnit === 'kW' ? 1000 : 1;
    const powerDisplay = power === null ? null : power / powerScale;
    const powerDigits = powerUnit === 'kW' ? 2 : 0;
    const powerCard = $('power-motion-card');
    const rail = $('power-rail');
    powerCard.hidden = power === null;
    trackPowerPeak(power);
    animateValue('power-rail-value', powerDisplay, powerDigits, powerUnit, '', reading => {
      const watts = reading === null ? null : reading * powerScale;
      powerCard.classList.toggle('is-regen', watts !== null && watts < 0);
      setScaleFill('power-rail-fill', Math.abs(watts ?? 0), powerVisualMaxW);
      const flow = watts === null ? '' : watts < 0 ? 'Regeneração' : watts > 0 ? 'Consumo' : 'Sem fluxo elétrico';
      $('power-flow').textContent = flow;
      rail.setAttribute('aria-label', watts === null ? 'Potência elétrica sem leitura' :
        flow + ' · ' + format(watts, 0) + ' W. Escala de 0 a ' + powerVisualMaxW + ' W.');
    });

    animateValue('range-value', range, 1, '', ' km');
    $('range-hint').textContent = range === null ? '' : 'Estimativa';
    animateValue('consumption-value', consumption, 1, '', ' Wh/km');
    $('consumption-hint').textContent = power !== null && consumption === null
      ? 'Mínimo de ' + consumptionMinMeters + ' m com dados.' : '';

    animateValue('trip-value', distance === null ? null : distance / 1000, 2, distance === null ? '' : 'km');
    const showEnergy = electricChartKeys.some(key => finite(sample[key]));
    $('energy-live-grid').hidden = !showEnergy;
    $('energy-unavailable').hidden = showEnergy;
    $('energy-unavailable').textContent = 'Medição elétrica indisponível no momento.';
    animateValue('voltage-value', voltage, 2, '', ' V');
    animateValue('current-value', current, 2, '', ' A');
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
    const electrical = data.eletrica_disponivel === true;
    const power = electrical ? readNumber(data, 'potencia_w') : null;
    accumulateEnergy(power, distance, now);
    const sample = {
      time: now,
      speed: readNumber(data, 'velocidade_centesimos_kmh', 100),
      accel: data.aceleracao_disponivel ? readNumber(data, 'aceleracao_milesimos_ms2', 1000) : null,
      voltage: electrical ? readNumber(data, 'tensao_pacote_mv', 1000) : null,
      current: electrical ? readNumber(data, 'corrente_centesimos_a', 100) : null,
      power,
      battery: readNumber(data, 'carga_decimos_percentual', 10),
      range: readNumber(data, 'autonomia_decimos_km', 10),
      consumption: power !== null && state.consumptionMeters >= consumptionMinMeters
        ? state.consumptionWh / (state.consumptionMeters / 1000) : null,
    };
    state.samples.push(sample);
    state.samples = state.samples.filter(sample => now - sample.time <= 610000);
    state.gapOpen = false;
    state.lastData = data;
    state.lastSampleAt = now - Math.min(Math.max(data.idade_ms, 0), 2000);
    state.lastSequence = data.sequencia;
    updateReadings(data, sample, distance);
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
        if (data.sequencia !== state.lastSequence || data.simulado !== state.lastData?.simulado) acceptSample(data);
        setStatus(data.simulado ? 'demo' : 'live', data.simulado ? 'Dados simulados' : 'Leituras atuais');
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

  const chartSlots = ['speed', 'power'];
  const chartColors = {};

  const chartSeries={
    speed:{label:'Velocidade',unit:'km/h',digits:1,axisDigits:0},
    accel:{label:'Aceleração',unit:'m/s²',digits:2,axisDigits:1},
    voltage:{label:'Tensão',unit:'V',digits:1,axisDigits:0},
    current:{label:'Corrente',unit:'A',digits:2,axisDigits:1},
    power:{label:'Potência',unit:'W',digits:0,axisDigits:0},
    battery:{label:'Carga da bateria',unit:'%',digits:1,axisDigits:0},
    range:{label:'Autonomia restante',unit:'km',digits:1,axisDigits:1},
    consumption:{label:'Consumo médio',unit:'Wh/km',digits:1,axisDigits:1},
  };
  const chartTones = ['primary', 'secondary'];
  const motionChartKeys = ['speed', 'accel'];
  const electricChartKeys = ['voltage', 'current', 'power', 'battery', 'range', 'consumption'];
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
    const motion = slot === 'speed';
    const keys = motion ? motionChartKeys : state.electricKeys;
    const setup=chartSetup($(slot+'-chart'));
    if(!setup)return;
    const {ctx,width:w,height:h}=setup;
    const plot={left:w<360?40:51,right:w-(w<360?41:51),top:24,bottom:h-24};
    const end=Date.now(),start=end-state.rangeMinutes*60000;
    const points=state.samples.filter(point=>point.time>=start&&point.time<=end);
    const limits=keys.map(key=>chartLimit(key,points));
    const x=time=>plot.left+(time-start)/(end-start)*(plot.right-plot.left);
    const y=(value,index)=>Math.max(plot.top,Math.min(plot.bottom,plot.bottom-(value-limits[index].min)/(limits[index].max-limits[index].min)*(plot.bottom-plot.top)));
    const available = keys.map(key => points.some(point => finite(point[key])));
    const hasPoints = available.some(Boolean);
    $(slot+'-empty').hidden = hasPoints;
    const legend = $(motion ? 'speed-chart-legend' : 'electric-chart-legend');
    Array.from(legend.children).forEach((item, index) => {
      item.querySelector('small').hidden = !hasPoints || available[index];
    });

    const palette = chartColors[slot];
    const colors = chartTones.map(tone => palette[tone]);
    ctx.font='11px system-ui, sans-serif';ctx.lineWidth=1;
    ctx.strokeStyle=palette.grid;
    keys.forEach((key,index)=>{
      const series=chartSeries[key];
      ctx.fillStyle=colors[index];ctx.textAlign=index?'right':'left';
      ctx.fillText(series.unit,index?w-2:2,14);
    });
    for(let index=0;index<=4;index++){
      const yy=plot.top+(plot.bottom-plot.top)*index/4;
      ctx.beginPath();ctx.moveTo(plot.left,yy);ctx.lineTo(plot.right,yy);ctx.stroke();
      keys.forEach((key,axis)=>{
        if (!available[axis]) return;
        const series=chartSeries[key],limit=limits[axis];
        const value=limit.max-(limit.max-limit.min)*index/4;
        ctx.fillStyle=colors[axis];ctx.textAlign=axis?'left':'right';
        const labelWidth = (axis ? w - plot.right : plot.left) - 9;
        ctx.fillText(format(value,series.axisDigits),axis?plot.right+7:plot.left-7,yy+4,labelWidth);
      });
    }
    const divisions = w < 360 ? 2 : 4;
    for (let index = 0; index <= divisions; index++) {
      const fraction = index / divisions;
      const xx = plot.left + (plot.right - plot.left) * fraction;
      ctx.beginPath();ctx.moveTo(xx,plot.top);ctx.lineTo(xx,plot.bottom);ctx.stroke();
      ctx.fillStyle=palette.axis;ctx.textAlign=index===0?'left':index===divisions?'right':'center';
      const seconds = Math.round((end - start) * (1 - fraction) / 1000);
      let label = 'Agora';
      if (seconds >= 60) label = '−' + Math.floor(seconds / 60) + ':' + String(seconds % 60).padStart(2, '0');
      else if (seconds > 0) label = '−' + seconds + ' s';
      ctx.fillText(label,xx,h-7);
    }
    ctx.save();ctx.beginPath();ctx.rect(plot.left,plot.top,plot.right-plot.left,plot.bottom-plot.top);ctx.clip();
    keys.forEach((key,index)=>{
      const color = colors[index];
      ctx.strokeStyle=color;ctx.lineWidth=index===0?3:1.9;ctx.lineJoin='round';ctx.lineCap='round';
      let first = null, last = null;
      const finishSegment = () => {
        if (!last) return;
        if (first === last) {
          ctx.fillStyle = color;
          ctx.beginPath();ctx.arc(x(last.time),y(last[key],index),2.5,0,Math.PI*2);ctx.fill();
        } else {
          ctx.stroke();
          if (index === 0) {
            ctx.lineTo(x(last.time), plot.bottom);
            ctx.lineTo(x(first.time), plot.bottom);
            ctx.closePath();
            ctx.fillStyle = palette.fill;
            ctx.fill();
          }
        }
        first = last = null;
      };
      for (const point of points) {
        if (!finite(point[key])) {
          finishSegment();
          continue;
        }
        if (first) ctx.lineTo(x(point.time), y(point[key], index));
        else {
          ctx.beginPath();ctx.moveTo(x(point.time),y(point[key],index));
          first = point;
        }
        last = point;
      }
      finishSegment();
      const hover=state.hovering[slot];
      if (hover && finite(hover[key]) && hover.time >= start && hover.time <= end) {
        ctx.fillStyle=color;ctx.beginPath();ctx.arc(x(hover.time),y(hover[key],index),4,0,Math.PI*2);ctx.fill();
      }
    });
    ctx.restore();
    state[slot+'ChartGeometry']={start,end,plot,x,y,keys};
  }

  function updateChartLegend(id, keys) {
    $(id).innerHTML = keys.map((key, index) => {
      const {label, unit} = chartSeries[key];
      return '<span style="--series-color:var(--chart-' + chartTones[index] + ')">' + label + ' · ' + unit +
        '<small hidden>· Sem leitura</small></span>';
    }).join('');
  }

  function updateElectricChartLabels() {
    const keys = state.electricKeys;
    updateChartLegend('electric-chart-legend', keys);
    $('power-chart').setAttribute('aria-label','Gráfico de '+keys.map(key=>chartSeries[key].label.toLowerCase()).join(' e ')+' ao longo do tempo');
  }

  function drawCharts() {
    chartSlots.forEach(drawChart);
  }

  const themeRoot = document.documentElement;
  const themeToggle = $('theme-toggle');
  function applyTheme(theme) {
    const light = theme === 'light';
    themeRoot.dataset.theme = theme;
    themeToggle.setAttribute('aria-pressed', String(light));
    themeToggle.title = light ? 'Ativar tema escuro' : 'Ativar tema claro';

    const style = getComputedStyle(themeRoot);
    const colors = [...chartTones, 'grid', 'axis', 'fill'];
    for (const slot of chartSlots) {
      const plotStyle = getComputedStyle($(slot + '-chart'));
      chartColors[slot] = Object.fromEntries(
        colors.map(name => [name, plotStyle.getPropertyValue('--chart-' + name).trim()])
      );
    }
    document.querySelector('meta[name="theme-color"]').content = style.getPropertyValue('--background').trim();
    drawCharts();
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

  chartSlots.forEach(key => {
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
    document.querySelectorAll('.range').forEach(other => {const active = other === button;other.classList.toggle('active', active);other.setAttribute('aria-pressed', String(active));});
    drawCharts();
  }));
  const electricSelectors = [$('visitor-chart-left'), $('visitor-chart-right')];
  electricSelectors.forEach((select, index) => {
    for (const key of electricChartKeys) select.add(new Option(chartSeries[key].label, key));
    select.value = state.electricKeys[index];
    select.addEventListener('change', () => {
      const other = 1 - index;
      if (select.value === state.electricKeys[other]) {
        state.electricKeys[other] = state.electricKeys[index];
        electricSelectors[other].value = state.electricKeys[other];
      }
      state.electricKeys[index] = select.value;
      state.hovering.power = null;
      $('power-tip').hidden = true;
      updateElectricChartLabels();
      drawChart('power');
    });
  });
  themeToggle.addEventListener('click', () => {
    const theme = themeRoot.dataset.theme === 'dark' ? 'light' : 'dark';
    applyTheme(theme);
    try {
      localStorage.setItem(themeRoot.dataset.themeStorage, theme);
    } catch {
      // O tema continua aplicado na página aberta.
    }
  });
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
  document.addEventListener('visibilitychange', () => {
    updateVehicleMotion();
    if (!document.hidden) poll();
  });
  reducedMotion.addEventListener?.('change', updateVehicleMotion);
  updateAge();
  updateChartLegend('speed-chart-legend', motionChartKeys);
  updateElectricChartLabels();
  applyTheme(themeRoot.dataset.theme);
  poll();
  setInterval(poll, 500);
  setInterval(() => {updateAge();drawCharts();}, 1000);
})();
