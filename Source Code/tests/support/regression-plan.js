const assert = require('node:assert/strict');
const catalog = require('./regression-catalog');
const {select} = require('./regression-selection');

function validateCatalog(definitions = catalog) {
    const ids = new Set();
    for (const [name, definition] of Object.entries(definitions)) {
        assert(definition.cases?.length, 'Missing cases: ' + name);
        const labels = new Set();
        for (const item of definition.cases) {
            assert(typeof item.id === 'string' && /^[a-z0-9][a-z0-9.-]+$/.test(item.id), 'Invalid case ID: ' + item.id);
            assert(!ids.has(item.id), 'Duplicate case ID: ' + item.id);
            assert(typeof item.name === 'string' && (definition.shell || !labels.has(item.name)), 'Duplicate or invalid case label: ' + name);
            ids.add(item.id); labels.add(item.name);
        }
        assert(!definition.seedFrom || definitions[definition.seedFrom]?.snapshot, 'Invalid seed owner: ' + name);
    }
}
function createPlan(selection, {full = false} = {}) {
    validateCatalog();
    const names = [], seen = new Set();
    function add(name) {
        assert(catalog[name], 'Unknown functional suite: ' + name);
        if (seen.has(name)) return;
        seen.add(name);
        if (catalog[name].seedFrom) add(catalog[name].seedFrom);
        names.push(name);
    }
    for (const name of select(selection)) add(name);
    const cases = names.flatMap(name => catalog[name].cases.map(item => ({...item, suite:name})));
    if (!full) {
        assert(cases.length <= 50, 'Plan has ' + cases.length + ' cases (limit 50). Use -List to inspect; -Full is for explicit full runs.');
        const large = names.filter(name => catalog[name].checks > 20);
        assert(!large.length, 'Suite exceeds 20 cases: ' + large.join(', ') + '. Inspect with -List; explicit full runs require -Full.');
    }
    return {names, cases, count:cases.length, full};
}
function assertCaseIds(actual, expected) {
    const ids = actual.map(item => typeof item === 'string' ? item : item.id);
    const wanted = expected.map(item => typeof item === 'string' ? item : item.id);
    assert.equal(new Set(ids).size, ids.length, 'Duplicate executed case ID');
    assert.deepEqual([...ids].sort(), [...wanted].sort(), 'Executed case IDs differ from plan');
}
module.exports = {createPlan, validateCatalog, assertCaseIds};
