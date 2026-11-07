const assert = require('node:assert/strict');
async function run(api) {
    const {check, launch, processes, resources, value, until, key, type, text, waitText, window, command} = api;
    const baseline = await resources();
    const row = async (pid, expected) => until(async () => (await text(pid)).split('\n').includes(expected), expected);
    async function complete(pid, input) {
        await type(input);
        await until(async()=>/gsh:\/[^\n]* \$$/.test(await text(pid)), 'new Shell prompt');
    }
    let first, second;
    await check('G3 Shell builtins, quoting, empty argv and syntax errors', async () => {
        first = await launch('term', ''); await waitText(first, 'SINO user Shell');
        await waitText(first, 'gsh:/ $');
        const before = await text(first); await key('backspace');
        assert.equal(await text(first), before, 'empty backspace preserves prompt');
        await type('help\n'); await row(first, 'builtins: help pwd cd history status exit');
        await type('args "" "a b" c\\ d\n');
        await row(first, 'argc=4'); await row(first, 'arg[1]=0:');
        await row(first, 'arg[2]=3:a b'); await row(first, 'arg[3]=3:c d');
        await type('echo "broken\n'); await row(first, 'syntax error');
        await type('unknown\n'); await row(first, 'unknown command: unknown');
        await complete(first, 'echo recovered\n'); await row(first, 'recovered');
        await type('history\n'); await until(async()=>(await text(first)).split('\n').some(row=>/^\d+ echo recovered$/.test(row)), 'stored history');
    });
    await check('G3 per-session cwd, root confinement and Chinese argv', async () => {
        await type('cd Documents\n'); await waitText(first, 'gsh:/Documents $');
        await type('pwd\n'); await row(first, '/Documents');
        await type('cd ../../outside\n'); await row(first, 'cd: outside grant or path too long');
        await type('cd /\n'); await type('pwd\n'); await row(first, '/');
        await type('args '); await key('ctrl','spc'); await type('nihao'); await key('spc');
        await key('ctrl','spc'); await key('ret'); await row(first, 'arg[1]=6:你好');
        second = await launch('term', ''); await waitText(second, 'gsh:/ $');
        await type('cd Documents\n'); await waitText(second, 'gsh:/Documents $');
        await key('alt','tab'); await type('pwd\n'); await row(first, '/');
        assert(!(await text(first)).endsWith('gsh:/Documents $'));
    });
    await check('G3 typed exit status, worker fault, cancellation and recovery', async () => {
        await complete(first, 'exitcode 7\n'); await type('status\n');
        await row(first, 'status code=7 reason=1 start=0');
        await complete(first, 'exitcode -7\n'); await type('status\n');
        await row(first, 'status code=-7 reason=1 start=0');
        const faults = await value('process_faults');
        await type('fault\n'); await until(async () => await value('process_faults') === faults + 1, 'G3 worker fault');
        await row(first, 'task ended reason=2'); await type('status\n');
        await row(first, 'status code=134 reason=2 start=0');
        await type('spin\n'); await row(first, 'worker waiting'); await key('ctrl','c');
        await row(first, 'task ended reason=3'); await type('status\n');
        await row(first, 'status code=130 reason=3 start=0');
        await complete(first, 'echo live\n'); await row(first, 'live');
        await type('exit 23\n'); await row(first, 'Task exited 23'); await key('ctrl','w');
        await until(async () => await window(first) < 0, 'first shell closes');
        await key('ctrl','d'); await row(second, 'Task exited 0'); await key('ctrl','w');
        await until(async () => (await processes()).length === 0, 'Shell resources reaped');
        assert.deepEqual(await resources(), baseline);
    });
    await check('G3 startup exhaustion reports a final error and accepts another command', async () => {
        first=await launch('term','');await waitText(first,'gsh:/ $');
        const blockers=[];
        try {
            for(let i=0;i<6;i++)blockers.push(await launch('spin'));
            await complete(first,'echo blocked\n');
            await row(first,'start failed -8');await type('status\n');
            await row(first,'status code=126 reason=0 start=-8');
        } finally { for(const pid of blockers)await command('kill '+pid); }
        await until(async()=>(await processes()).length===2,'blockers reaped');
        await complete(first,'echo available\n');await row(first,'available');
        for(let i=0;i<20;i++){await type('pwd\n');await until(async()=>(await text(first)).endsWith('gsh:/ $'),'builtin prompt');}
        await type('history\n');
        await until(async()=>(await text(first)).split('\n').filter(row=>/^\d+ /.test(row)).length===16,'bounded history');
        await key('ctrl','w');await until(async()=>(await processes()).length===0,'exhaustion recovery closes');
        assert.deepEqual(await resources(),baseline);
    });
    await check('G3 Ctrl+C discards a prompt line and long canonical input stays bounded', async () => {
        first = await launch('term', ''); await waitText(first, 'gsh:/ $');
        await type('notacommand'); await key('ctrl','c');
        await type('echo reset\n'); await row(first, 'reset');
        assert(!(await text(first)).includes('unknown command: notacommand'));
        await type('echo ' + 'x'.repeat(120) + '\n');
        await until(async () => (await text(first)).includes('x'.repeat(80)), 'wrapped long command');
        await key('ctrl','w'); await until(async () => (await processes()).length === 0, 'partial input close');
        assert.deepEqual(await resources(), baseline);
        assert.equal(await command('echo g3compatible'), 'g3compatible\n');
    });
}
module.exports = {run};
