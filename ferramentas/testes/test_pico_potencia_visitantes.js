// Um pico relevante marca a potência anterior; a linha desaparece após um intervalo curto.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '..', '..', 'firmware', 'visitantes-s3', 'web', 'app.js'), 'utf8');
const start = source.indexOf('  const powerPeak=');
const end = source.indexOf('\n  function setStatus(', start);
assert(start >= 0 && end > start);

let now = 100, visible = false, regen = false, timerId = 0;
const timers = new Map(), styles = {};
const marker = {
  style:{setProperty:(key,value)=>{styles[key]=value;}},
  classList:{
    add:key=>{if(key==='is-visible')visible=true;},
    remove:key=>{if(key==='is-visible')visible=false;},
    toggle:(key,value)=>{if(key==='is-regen')regen=value;},
  },
  getBoundingClientRect:()=>({width:2}),
};
const context = vm.createContext({
  state:{powerRailLimit:500},
  $:()=>marker,
  finite:value=>typeof value==='number'&&Number.isFinite(value),
  performance:{now:()=>now},
  setTimeout:callback=>{timers.set(++timerId,callback);return timerId;},
  clearTimeout:id=>timers.delete(id),
});
vm.runInContext(source.slice(start,end)+'\nglobalThis.track=trackPowerPeak;',context);
function sample(time,power){now=time;context.track(power);}

sample(100,100);
sample(600,220);
sample(1100,300);
sample(1600,270);
assert.equal(visible,false);
sample(2100,180);
assert.equal(visible,true);
assert.equal(styles['--power-peak-position'],'80%');
assert.equal(regen,false);
[...timers.values()][0]();
assert.equal(visible,false);
sample(6000,-160);
sample(6500,-220);
sample(7000,-120);
assert.equal(visible,true);
assert(Math.abs(parseFloat(styles['--power-peak-position'])-28)<.01);
assert.equal(regen,true);
console.log('Picos de consumo e regeneração: OK');
