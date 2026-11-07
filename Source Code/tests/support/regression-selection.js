const assert = require('node:assert/strict');
const profiles = require('./regression-suites');

function select(value) {
    assert.equal(typeof value, 'string', 'A regression selection is required.');
    const selected = value.split(',').map(name => name.trim());
    assert(selected.length && selected.every(name => Object.hasOwn(profiles, name)),
        'Unknown regression selection: ' + value);
    const names = selected.flatMap(name => profiles[name]);
    assert.equal(new Set(names).size, names.length,
        'Selections overlap; choose each functional suite once.');
    return names;
}
module.exports = {select};
