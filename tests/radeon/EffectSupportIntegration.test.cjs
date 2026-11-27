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
        'gpuPresetName', 'reconcileGpu']) {
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
