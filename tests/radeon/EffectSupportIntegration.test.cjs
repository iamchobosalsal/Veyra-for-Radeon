const {readFileSync} = require('node:fs');
const {join} = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const test = require('node:test');

// The public fork already merged the Radeon helpers. Appending the historical
// private-donor Integration.js would overwrite the code that is actually shipped.
const source = readFileSync(join(__dirname, '../../qml/Veyra/EffectSupport.js'), 'utf8')
    .replace(/^\.pragma library\s*/, '');
const support = vm.createContext({});
vm.runInContext(source, support, {filename: 'qml/Veyra/EffectSupport.js'});

test('the shipped helper defines each bridge and Radeon function once', () => {
    const names = [...source.matchAll(/^function\s+(\w+)\s*\(/gm)].map(match => match[1]);
    assert.equal(new Set(names).size, names.length, 'Production must not contain duplicate helper definitions');
    for (const name of ['status', 'available', 'reason', 'option', 'fgBackendShortLabel',
        'radeonChoices', 'radeonHint', 'radeonText', 'radeonLabel', 'radeonMultipliers',
        'radeonEffects', 'applyRadeon', 'gpuCapabilityAvailable', 'preferredNr',
        'gpuPresetName', 'gpuPresetState', 'activateGpuPreset', 'nrModelConnectionText', 'reconcileGpu']) {
        assert.equal(typeof support[name], 'function', name);
    }
});

test('grouped NR/SR/HDR/FG IDs are resolved by the production bridge mapping', () => {
    const calls = [];
    let notifyingPropertyReads = 0;
    const caps = {nr2: {available: true}, sr0: {available: true}, hdr: {available: true}, dlss: {available: true}};
    const ids = {nr: 'nr2', sr: 'sr0', 'video-hdr': 'hdr', 'frame-generation': 'dlss', sr0: 'sr0'};
    const api = {
        get effectCapabilities() { ++notifyingPropertyReads; return caps; },
        effectAvailability(id) { calls.push(id); return caps[ids[id]] || {available: false, reason: 'unsupported'}; }
    };
    for (const id of Object.keys(ids)) assert.equal(support.available(api, id), true, id);
    assert.deepEqual(calls, Object.keys(ids));
    assert.equal(notifyingPropertyReads, calls.length, 'Each binding read must track capability notifications');

    caps.hdr = {available: false, reason: 'missing runtime'};
    assert.equal(support.available(api, 'video-hdr'), false);
    assert.equal(support.reason(api, 'video-hdr'), 'missing runtime');
    assert.equal(support.available(api, 'unknown-effect'), false);
    assert.equal(support.reason(api, 'unknown-effect'), 'unsupported');
});

test('menu options preserve actual capability reasons after runtime changes', () => {
    const api = {effectCapabilities: {fsr3: {available: true, reason: ''}, fsr4: {available: false, reason: 'ML unavailable'}}};
    assert.equal(support.option(api, 'fsr3', 'FSR 3.1', 'fsr3').disabled, false);
    const ml = support.option(api, 'fsr4', 'FSR 4 ML', 'fsr4');
    assert.equal(ml.disabled, true);
    assert.equal(ml.note, 'ML unavailable');
    api.effectCapabilities.fsr3 = {available: false, reason: 'runtime removed'};
    assert.equal(support.option(api, 'fsr3', 'FSR 3.1', 'fsr3').note, 'runtime removed');
});

test('Radeon labeling retains FSR choices and never enables a disabled backend', () => {
    for (const language of ['ko', 'en']) {
        const rife = {id: 'anr', label: 'ANR FG', disabled: false};
        const fsr = {id: 'fsr3', label: 'FSR 3.1', disabled: false};
        const ml = {id: 'fsr4', label: 'FSR 4 ML', disabled: true, note: 'ML unavailable'};
        const api = {amdNrGpu: true, uiLanguage: language, fgBackendChoices: [rife, fsr, ml]};
        const choices = support.radeonChoices(api);
        assert.equal(choices[0].label, 'RIFE · Radeon');
        assert.equal(choices[0].disabled, false);
        assert.equal(choices[1], fsr, 'The FSR option must retain the bridge availability decision');
        assert.equal(choices[2], ml);
        assert.equal(rife.label, 'ANR FG', 'Rendering choices must not mutate the bridge data');
        rife.disabled = true;
        assert.equal(support.radeonChoices(api)[0].disabled, true);
        rife.disabled = false; api.amdNrGpu = false;
        const foreign = support.radeonChoices(api)[0];
        assert.equal(foreign.disabled, true);
        assert.match(foreign.note, /NVIDIA/);
    }
});

test('RIFE hints and multiplier limit do not overwrite the selected FSR provider', () => {
    const choices = [{id: '2'}, {id: '3'}, {id: '4'}];
    const api = {fgBackendName: 'anr', fgMultiplierChoices: choices, uiLanguage: 'ko', fgActive: false};
    const filtered = support.radeonMultipliers(api);
    assert.deepEqual(Array.from(filtered, option => option.id), ['2']);
    assert.equal(choices.length, 3);
    assert.match(support.radeonHint(api), /RIFE · Radeon · 2X/);
    assert.match(support.radeonHint(api), /재생 후/);
    api.fgActive = true;
    assert.match(support.radeonHint(api), /처리 중/);
    api.fgBackendName = 'fsr3'; api.fgProviderText = 'FSR 3.1 · provider 1.1.3';
    assert.equal(support.radeonHint(api), api.fgProviderText);
    assert.equal(support.radeonMultipliers(api), choices);
});

test('effect catalog and short labels describe RIFE without changing other effects', () => {
    const fsr = {id: 'fsr-fg', label: 'FSR 3.1', disabled: false};
    const rife = {id: 'anr-fg', label: 'ANR FG', disabled: true, custom: 7};
    const api = {amdNrGpu: true, effectCatalog: [rife, fsr]};
    const catalog = support.radeonEffects(api);
    assert.equal(catalog[0].label, 'RIFE · Radeon');
    assert.equal(catalog[0].disabled, true);
    assert.equal(catalog[0].custom, 7);
    assert.equal(catalog[1], fsr);
    assert.equal(support.fgBackendShortLabel({id: 'anr', label: 'ANR FG'}), 'RIFE · Radeon');
    assert.equal(support.fgBackendShortLabel({id: 'fsr3', label: 'FSR 3.1'}), 'FSR 3.1');
    assert.equal(support.radeonText('ANR FG / ANR FG'), 'RIFE · Radeon / RIFE · Radeon');
    assert.equal(support.radeonLabel('ANR FG · 2X'), 'RIFE · Radeon');
    assert.equal(support.radeonLabel('FSR 3.1'), 'FSR 3.1');
});


function presetApi() {
    return {
        amdNrGpu: true, uiLanguage: 'ko',
        nrModelAssets: {managed: true, checked: true, busy: false, smallReady: false, originalReady: false},
        effectCapabilities: {nr4: {available: false, modelAssetsMissing: true, reason: 'Original weights missing'}, anr: {available: true}},
        nrLayers: [{index: 3, runtime: 2, intensity: 1.4}],
        chain: [{type: 'sr', enabled: false}, {type: 'frame-generation', enabled: false}],
        fgBackendName: 'fsr3', fgEnabled: false, nrEnabled: false, srEnabled: false,
        calls: [],
        chooseNrModelFolder() { this.calls.push('choose'); },
        addEffect(type) { this.calls.push('add:' + type); },
        setNrLayerParameter(index, key, value) { this.calls.push([index, key, value]); }
    };
}

test('missing models connect from the main preset without changing effects, even with no Radeon layer', () => {
    for (const layers of [[], [{index: 3, runtime: 2, intensity: 1.4}]]) {
        const api = presetApi(); api.nrLayers = layers;
        const before = JSON.stringify({layers: api.nrLayers, fg: api.fgBackendName, fgOn: api.fgEnabled, nrOn: api.nrEnabled, srOn: api.srEnabled});
        const state = support.gpuPresetState(api);
        assert.equal(state.kind, 'connect'); assert.equal(state.enabled, true);
        assert.equal(state.label, 'NR 모델 연결'); assert.equal(state.reason, 'Original weights missing');
        assert.equal(support.activateGpuPreset(api), true);
        assert.deepEqual(api.calls, ['choose']);
        assert.equal(JSON.stringify({layers: api.nrLayers, fg: api.fgBackendName, fgOn: api.fgEnabled, nrOn: api.nrEnabled, srOn: api.srEnabled}), before);
    }
});

test('ready models restore the original preset action and retain per-layer strength', () => {
    const api = presetApi(); api.nrLayers[0].runtime = 4;
    api.nrModelAssets.smallReady = api.nrModelAssets.originalReady = true;
    api.effectCapabilities.nr4 = {available: true, reason: ''};
    const state = support.gpuPresetState(api);
    assert.equal(state.kind, 'apply'); assert.equal(state.enabled, true);
    assert.equal(state.label, 'Radeon NR + RIFE');
    assert.equal(support.activateGpuPreset(api), true);
    assert.deepEqual(api.calls, [[3, 'runtime', 4]]);
    assert.equal(api.nrLayers[0].intensity, 1.4);
    assert.equal(api.fgBackendName, 'anr'); assert.equal(api.fgEnabled, true);
    assert.equal(api.nrEnabled, true); assert.equal(api.srEnabled, true);
    assert.equal(api.srTargetIndex, 2); assert.equal(api.fgMultiplier, 2);
});

test('FG-only failure keeps its exact reason and cannot open a model picker or apply effects', () => {
    const api = presetApi(); api.nrModelAssets.smallReady = api.nrModelAssets.originalReady = true;
    api.effectCapabilities.nr4 = {available: true, reason: ''};
    api.effectCapabilities.anr = {available: false, reason: 'RIFE runtime is missing: provider.dll'};
    const state = support.gpuPresetState(api);
    assert.equal(state.kind, 'blocked'); assert.equal(state.enabled, false);
    assert.equal(state.reason, 'RIFE runtime is missing: provider.dll');
    assert.equal(support.activateGpuPreset(api), false);
    assert.deepEqual(api.calls, []); assert.equal(api.fgEnabled, false); assert.equal(api.nrEnabled, false);
});

test('a running model check blocks both connect and preset application without mutating settings', () => {
    for (const ready of [false, true]) {
        const api = presetApi(); api.nrModelAssets.busy = true;
        if (ready) api.effectCapabilities.nr4 = {available: true};
        const state = support.gpuPresetState(api);
        assert.equal(state.kind, 'busy'); assert.equal(state.enabled, false);
        assert.equal(support.activateGpuPreset(api), false);
        assert.deepEqual(api.calls, []); assert.equal(api.fgBackendName, 'fsr3'); assert.equal(api.nrEnabled, false);
    }
});


// Model the bridge's authoritative node list: a property switch never creates
// a missing stage. Flat Boolean mocks would hide the actual deleted-node bug.
function editableChainApi(amd, types) {
    const api = presetApi();
    api.amdNrGpu = amd;
    api.nrModelAssets.smallReady = api.nrModelAssets.originalReady = true;
    api.effectCapabilities = {nr4: {available: true}, nr2: {available: true}, anr: {available: true}, dlss: {available: true}};
    api.chain = types.map((type, index) => ({type, enabled: false, runtime: 3, intensity: 1.1 + index / 10}));
    Object.defineProperty(api, 'nrLayers', {get() {
        return this.chain.flatMap((node, index) => node.type === 'nr' ? [{...node, index}] : []);
    }});
    for (const [property, type] of [['srEnabled', 'sr'], ['fgEnabled', 'frame-generation'], ['nrEnabled', 'nr']]) {
        Object.defineProperty(api, property, {
            get() { return !!this.chain.find(node => node.type === type && node.enabled); },
            set(value) { const node = this.chain.find(node => node.type === type); if (node) node.enabled = value; }
        });
    }
    api.addEffect = function(type) {
        this.calls.push('add:' + type);
        if (this.rejectAdd === type) return -1;
        const actualType = type.endsWith('-fg') ? 'frame-generation' : type;
        const node = {type: actualType, enabled: actualType !== 'nr', runtime: amd ? 4 : 2, intensity: 1};
        const tail = this.chain.findIndex(item => item.type === 'frame-generation');
        const index = actualType !== 'frame-generation' && tail >= 0 ? tail : this.chain.length;
        this.chain.splice(index, 0, node);
        return index;
    };
    api.setNrLayerParameter = function(index, key, value) {
        this.calls.push([index, key, value]); this.chain[index][key] = value;
    };
    return api;
}

test('preset restores deleted SR/FG stages through validated additions on Radeon and NVIDIA', () => {
    for (const amd of [true, false]) for (const types of [
        ['nr', 'nr', 'nr'], ['sr', 'nr'], ['nr', 'frame-generation'], []
    ]) {
        const api = editableChainApi(amd, types);
        const strengths = api.nrLayers.map(layer => layer.intensity);
        assert.equal(support.activateGpuPreset(api), true);
        assert.equal(api.srEnabled, true); assert.equal(api.fgEnabled, true); assert.equal(api.nrEnabled, true);
        assert.equal(api.chain.filter(node => node.type === 'sr').length, 1);
        assert.equal(api.chain.filter(node => node.type === 'frame-generation').length, 1);
        assert.equal(api.chain.at(-1).type, 'frame-generation');
        assert.equal(api.fgBackendName, amd ? 'anr' : 'dlss'); assert.equal(api.fgMultiplier, 2);
        assert.equal(api.srTargetIndex, 2); assert.equal(api.nrLayers[0].runtime, amd ? 4 : 2);
        if (strengths.length) assert.deepEqual(api.nrLayers.map(layer => layer.intensity), strengths);
        const additions = api.calls.filter(call => typeof call === 'string');
        if (!types.includes('frame-generation')) assert.ok(additions.includes('add:' + (amd ? 'anr-fg' : 'dlss-fg')));
        const nodeCount = api.chain.length;
        assert.equal(support.activateGpuPreset(api), true);
        assert.equal(api.chain.length, nodeCount, 'Applying again must reuse the existing stages');
        assert.deepEqual(api.calls.filter(call => typeof call === 'string'), additions);
    }
});

test('preset propagates validated node-addition refusal instead of claiming a complete preset', () => {
    for (const rejected of ['nr', 'sr', 'anr-fg']) {
        const api = editableChainApi(true, rejected === 'nr' ? [] : ['nr']);
        api.rejectAdd = rejected;
        assert.equal(support.activateGpuPreset(api), false);
        assert.equal(api.fgEnabled, false);
        assert.equal(api.chain.some(node => node.type === (rejected === 'anr-fg' ? 'frame-generation' : rejected)), false);
        assert.equal(api.calls.filter(call => call === 'add:' + rejected).length, 1);
    }
});
