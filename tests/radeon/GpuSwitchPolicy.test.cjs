const {readFileSync} = require('node:fs');
const {join} = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const context = vm.createContext({});
vm.runInContext(readFileSync(join(__dirname,'../../scripts/radeon/player/Integration.js'),'utf8'),context);
function api(amd, caps) {
  return {amdNrGpu:amd,uiLanguage:'ko',effectCapabilities:Object.fromEntries(caps.map(k=>[k,{available:true}])),
    preferences:{monitorGpu:'old'},gpuMonitorChoices:[{id:''},{id:'current'}],nrLayers:[{index:2,runtime:amd?2:4,intensity:1.4}],
    fgBackendName:amd?'dlss':'anr',fgEnabled:false,changed:[],
    setPreference(k,v){this.preferences[k]=v;this.changed.push(k);},
    setNrLayerParameter(i,k,v){assert.equal(i,2);assert.equal(k,'runtime');this.nrLayers[0].runtime=v;this.changed.push(k);}
  };
}
const nv=api(false,['nr2','nr3','dlss']);context.reconcileGpu(nv);
assert.equal(nv.nrLayers[0].runtime,2);assert.equal(nv.fgBackendName,'dlss');assert.equal(nv.fgEnabled,false);
assert.equal(nv.preferences.monitorGpu,'');assert.equal(nv.nrLayers[0].intensity,1.4);
assert.equal(context.gpuPresetName(nv),'NVIDIA NR + DLSS-FG');
const amd=api(true,['nr4','anr']);context.reconcileGpu(amd);
assert.equal(amd.nrLayers[0].runtime,4);assert.equal(amd.fgBackendName,'anr');assert.equal(amd.fgEnabled,false);
const keep=api(false,['nr2','nr3','dlss','vfg']);keep.nrLayers[0].runtime=3;keep.fgBackendName='vfg';keep.fgEnabled=true;keep.preferences.monitorGpu='current';
context.reconcileGpu(keep);assert.equal(keep.changed.length,0);assert.equal(keep.fgBackendName,'vfg');assert.equal(keep.fgEnabled,true);
const missing=api(false,[]);missing.preferences.monitorGpu='';context.reconcileGpu(missing);
assert.equal(missing.changed.length,0);assert.equal(missing.fgBackendName,'anr');assert.equal(context.preferredNr(missing),-1);
for (const language of ['ko','en']) {
  nv.uiLanguage=language;nv.fgBackendChoices=[{id:'anr',disabled:false}];
  const choice=context.radeonChoices(nv)[0];assert.equal(choice.disabled,true);assert.match(choice.note,/NVIDIA/);
}
console.log('PASS: GPU migration, supported custom choices, disabled effects, missing runtime, KO/EN provider gating');
