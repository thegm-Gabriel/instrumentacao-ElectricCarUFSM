// A leitura nova deve chegar suavemente ao alvo e aceitar outra amostra no caminho.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '..', '..', 'firmware', 'visitantes-s3', 'web', 'app.js'), 'utf8');
const start = source.indexOf('  const animatedValues = {}');
const end = source.indexOf('\n  function setStatus(', start);
assert(start >= 0 && end > start);

let now = 0, nextFrame = 0;
const frames = new Map(), shown = {}, media = {matches:false};
const context = vm.createContext({
  state:{observedPeriodMs:500},
  finite: value => typeof value === 'number' && Number.isFinite(value),
  format: (value, digits) => value.toFixed(digits),
  setValue: (id, text, unit) => {shown[id] = {text, unit};},
  matchMedia: () => media,
  document: {hidden:false},
  performance: {now:() => now},
  requestAnimationFrame: callback => {frames.set(++nextFrame, callback); return nextFrame;},
  cancelAnimationFrame: id => frames.delete(id),
});
vm.runInContext(source.slice(start, end) + '\nglobalThis.animateReading=animateValue;', context);

function advance(time) {
  now = time;
  const ready = [...frames.values()];
  frames.clear();
  ready.forEach(callback => callback(now));
}

context.animateReading('speed-value', 10, 1);
assert.equal(shown['speed-value'].text, '10.0');
context.animateReading('speed-value', 20, 1);
assert.equal(shown['speed-value'].text, '10.0');
advance(130);
const midway = Number(shown['speed-value'].text);
assert(midway > 10 && midway < 20);
context.animateReading('speed-value', 5, 1);
advance(260);
const reversed = Number(shown['speed-value'].text);
assert(reversed < midway && reversed > 5);
advance(720);
assert.equal(shown['speed-value'].text, '5.0');

context.animateReading('speed-value', 105, 1);
advance(870);
const early = Number(shown['speed-value'].text);
advance(1120);
const late = Number(shown['speed-value'].text);
assert(early - 5 > 105 - late, 'a leitura deve desacelerar perto do alvo');
advance(1310);
assert.equal(shown['speed-value'].text, '105.0');
context.state.observedPeriodMs = 800;
context.animateReading('speed-value', 205, 1);
advance(1800);
assert(Number(shown['speed-value'].text) < 205, 'a duração deve acompanhar leituras mais espaçadas');
advance(2200);
assert.equal(shown['speed-value'].text, '205.0');

context.animateReading('speed-value', null, 1);
assert.equal(shown['speed-value'].text, 'Sem leitura');
media.matches = true;
context.animateReading('speed-value', 3, 1);
context.animateReading('speed-value', 18, 1);
assert.equal(shown['speed-value'].text, '18.0');
assert.equal(frames.size, 0);
console.log('Animação das leituras e movimento reduzido: OK');
