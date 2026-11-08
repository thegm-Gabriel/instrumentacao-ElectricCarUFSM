// Regressão do marcador: desaceleração real, uma amostra por sequência e visibilidade.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const web = path.join(__dirname, '..', '..', 'firmware', 'equipe-s3', 'web');
const js = fs.readFileSync(path.join(web, 'app.js'), 'utf8');
const css = fs.readFileSync(path.join(web, 'layout.css'), 'utf8');
const html = fs.readFileSync(path.join(web, 'index.html'), 'utf8');
const start = js.indexOf('  const speedPeak=');
const end = js.indexOf('\n  function event(', start);
assert(start >= 0 && end > start);

let now = 100;
let visible = false;
let markerLine = {};
const elements = {
  'speed-fill': {style: {}, closest: () => ({setAttribute: () => {}})},
  'speed-peak-marker': {
    classList: {toggle: (_name, value) => { visible = value; }},
    querySelectorAll: () => [{setAttribute: (name, value) => { markerLine[name] = value; }}]
  },
  'throttle-fill': {style: {setProperty: () => {}}},
  'brake-fill': {style: {setProperty: () => {}}}
};
const state = {telemetry: null};
const context = vm.createContext({
  state,
  $: id => elements[id],
  set: () => {},
  value: (data, key, divisor = 1) => Number.isFinite(data[key]) ? data[key] / divisor : null,
  number: value => String(value),
  performance: {now: () => now},
  setTimeout: () => 1,
  clearTimeout: () => {}
});
vm.runInContext(js.slice(start, end) + '\nglobalThis.updateArcTest=updateArc;', context);

function sample(ms, speed, sequence) {
  now = ms;
  state.telemetry = {
    valido: true, simulado: true, sequencia: sequence,
    velocidade_centesimos_kmh: Math.round(speed * 100)
  };
  context.updateArcTest();
}

sample(100, 20, 1);
sample(350, 23, 2);
sample(600, 22.7, 3);
assert.equal(visible, false);
sample(850, 21, 4);
assert.equal(visible, true);
const innerRadius = Math.hypot(Number(markerLine.x1) - 160, Number(markerLine.y1) - 158);
const outerRadius = Math.hypot(Number(markerLine.x2) - 160, Number(markerLine.y2) - 158);
assert(Math.abs(innerRadius - 117) < .1);
assert(Math.abs(outerRadius - 125) < .1);
now = 1851;
context.updateArcTest(false);
assert.equal(visible, true);
now = 2251;
context.updateArcTest(false);
assert.equal(visible, false);

assert.match(css, /\.is-visible \.peak-mark\{animation:speed-peak-reveal/);
const inactiveStyle = css.match(/\.speed-arc \.peak-mark\{[^}]*\}/);
assert(inactiveStyle);
assert.doesNotMatch(inactiveStyle[0], /animation:/);
assert.match(css, /prefers-reduced-motion:reduce[^}]*\.speed-arc \.is-visible \.peak-mark/);
assert.match(html, /class="peak-mark"\s*\/>/);
assert.doesNotMatch(html, /id="speed-peak-marker"[^>]*display:none/);
console.log('Marcador de desaceleração visível e temporário: OK');
