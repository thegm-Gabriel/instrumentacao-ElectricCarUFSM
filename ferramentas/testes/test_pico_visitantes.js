// O pico só aparece após queda significativa e o risco usa a escala de velocidade.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const web = path.join(__dirname, '..', '..', 'firmware', 'visitantes-s3', 'web');
const source = fs.readFileSync(path.join(web, 'app.js'), 'utf8');
const html = fs.readFileSync(path.join(web, 'index.html'), 'utf8');
const start = source.indexOf('  function hidePeak(');
const end = source.indexOf('\n  function setStatus(', start);
assert(start >= 0 && end > start);

let now = 100, visible = false, timerId = 0;
const timers = new Map(), markerStyle = {};
const marker = {
  classList:{add: () => {visible = true;}, remove: () => {visible = false;}, toggle: () => {}},
  style:{setProperty: (name, value) => {markerStyle[name] = value;}},
  getBoundingClientRect: () => ({width:320}),
};
const context = vm.createContext({
  speedScaleMax:35,
  $: id => {
    assert.equal(id, 'speed-peak-marker');
    return marker;
  },
  finite: value => typeof value === 'number' && Number.isFinite(value),
  performance:{now:() => now},
  setTimeout: (callback, delay) => {timers.set(++timerId, {callback, delay}); return timerId;},
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
assert(Math.abs(parseFloat(markerStyle['--peak-position']) - 23 / 35 * 100) < 0.01);
assert.equal(markerStyle['--peak-duration'], '1600ms');
assert.match(html,/<div class="speed-track">[^]*?id="speed-peak-marker"/);
assert.doesNotMatch(html, /speed-peak-label/);

assert.equal(timers.size, 1);
const [expiration] = timers.values();
assert.equal(expiration.delay, 1600);
expiration.callback();
assert.equal(visible,false);
sample(2100,18);
assert.equal(visible,false);
console.log('Pico recente na escala e expiração: OK');
