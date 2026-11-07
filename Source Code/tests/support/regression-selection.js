const assert = require('node:assert/strict');
const profiles = require('./regression-suites');

function select(value) {
    assert.equal(typeof value, 'string', 'A regression selection is required.');
    const selected = value.split(',').map(name => name.trim());
    assert(selected.length && selected.every(name => Object.hasOwn(profiles, name)),
        'Unknown regression selection: ' + value);
    return [...new Set(selected.flatMap(name => profiles[name]))];
}
module.exports = {select};
