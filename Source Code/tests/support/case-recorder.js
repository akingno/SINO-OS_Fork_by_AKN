const assert = require('node:assert/strict');
function createCaseRecorder(planned, report, healthy = () => {}) {
    return async function check(name, action) {
        const definition = planned?.find(item => item.name === name);
        assert(definition, 'Unregistered case: ' + name);
        assert(!report.cases.some(item => item.id === definition.id), 'Duplicate case: ' + definition.id);
        const item = {id:definition.id, name, result:'FAIL'};
        report.cases.push(item);
        const started = Date.now();
        try { await action(item); healthy(); item.result = 'PASS'; console.log('PASS ' + item.id); }
        catch (error) {item.error = error.message; throw error;}
        finally {item.milliseconds = Date.now() - started;}
    };
}
module.exports = {createCaseRecorder};
