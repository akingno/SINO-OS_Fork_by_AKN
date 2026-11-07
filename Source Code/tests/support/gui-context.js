const assert = require('node:assert/strict');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
async function createGui(api) {
    const {qmp, value, until, memory, symbols, command, processes, resources} = api;
    const baseline = await resources();
    async function key(...codes) {
        const epoch = await value('gui_key_epoch');
        await qmp.call('send-key', {keys:codes.map(data => ({type:'qcode', data})), 'hold-time':20});
        await until(async () => await value('gui_key_epoch') !== epoch, 'desktop consumes key');
        await sleep(30);
    }
    async function type(text) {
        const plain={'\n':'ret',' ':'spc',"'":'apostrophe','\\':'backslash','/':'slash','.':'dot','-':'minus','|':'backslash'};
        const shifted={'"':'apostrophe','|':'backslash','>':'dot','<':'comma',':':'semicolon'};
        for (const c of text) {
            if(shifted[c]) await key('shift',shifted[c]);
            else if(/[A-Z]/.test(c)) await key('shift',c.toLowerCase());
            else await key(plain[c]||c);
        }
    }
    async function window(pid) {
        const owners = await memory(symbols.user_owner, 32);
        for (let i = 0; i < 8; i++) if (owners.readUInt32LE(i * 4) === pid) return i;
        return -1;
    }
    async function text(pid) {
        const id = await window(pid);
        if (id < 0) return '';
        await qmp.call('stop');
        try {
            const ptr = (await memory(symbols.user_scene + id * 4, 4)).readUInt32LE();
            if (!ptr) return '';
            const data = await memory(ptr, 4 + 256 * 104);
            const count = data.readUInt32LE();
            assert(count <= 256, 'bounded scene');
            const rows = new Map();
            for (let n = 0; n < count; n++) {
                const c = data.subarray(4 + n * 104, 4 + (n + 1) * 104);
                if (c.readUInt32LE() !== 9) continue;
                const y = c.readInt32LE(8);
                rows.set(y, (rows.get(y) || '') + c.subarray(24,104).toString('utf8').split('\0')[0]);
            }
            return [...rows.values()].map(row => row.trimEnd()).join('\n');
        } finally { await qmp.call('cont'); }
    }
    async function waitText(pid, fragment) {
        await until(async () => (await text(pid)).includes(fragment), 'terminal text ' + fragment);
    }
    async function close(pid) {
        await command('kill ' + pid);
        await until(async () => (await processes()).length === 0,
            'frontend and deadline-bound probe reaped');
        assert.deepEqual(await resources(), baseline);
    }
    return {...api, key, type, window, text, waitText, close};
}
module.exports = {createGui};
