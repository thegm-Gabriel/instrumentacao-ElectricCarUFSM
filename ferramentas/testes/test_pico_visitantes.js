// O pico só aparece após queda significativa e o risco permanece dentro do arco.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const web = path.join(__dirname, '..', '..', 'firmware', 'visitantes-s3', 'web');
const source = fs.readFileSync(path.join(web, 'app.js'), 'utf8');
const html = fs.readFileSync(path.join(web, 'index.html'), 'utf8');
const start = source.indexOf('  const speedPeak =');
const end = source.indexOf('\n  function setStatus(', start);
assert(start >= 0 && end > start);

let now = 100, visible = false, labelVisible = false, timerId = 0;
const timers = new Map(), line = {}, label = {textContent:''};
label.classList = {add: () => {labelVisible = true;}, remove: () => {labelVisible = false;}};
const marker = {
  classList:{add: () => {visible = true;}, remove: () => {visible = false;}},
  querySelectorAll: () => [{setAttribute: (name, value) => {line[name] = value;}}],
  getBoundingClientRect: () => ({width:320}),
};
const context = vm.createContext({
  speedGaugeMax:35,
  $: id => id === 'speed-peak-marker' ? marker : label,
  format: value => value.toFixed(1),
  finite: value => typeof value === 'number' && Number.isFinite(value),
  performance:{now:() => now},
  setTimeout: callback => {timers.set(++timerId, callback); return timerId;},
  clearTimeout: id => timers.delete(id),
});
vm.runInContext(source.slice(start, end) + '\nglobalThis.trackPeak=trackSpeedPeak;', context);

function sample(time, speed) {now=time;context.trackPeak(speed);}
sample(100,20);
sample(600,23);
sample(1100,22.6);
assert.equal(visible,false);
sample(1600,20.8);
assert.equal(visible,true);
assert.equal(labelVisible,true);
assert.equal(label.textContent,'Pico 23.0 km/h');
assert(Math.abs(Math.hypot(Number(line.x1)-160,Number(line.y1)-165)-113.5)<.1);
assert(Math.abs(Math.hypot(Number(line.x2)-160,Number(line.y2)-165)-119)<.1);
assert.match(html,/id="speed-arc"[^>]*>.*id="speed-peak-marker"/);

const [fade,expire]=[...timers.values()];
fade();
assert.equal(visible,true);
assert.equal(labelVisible,false);
expire();
assert.equal(visible,false);
sample(2100,18);
assert.equal(visible,false);
console.log('Pico recente no arco e expiração: OK');
