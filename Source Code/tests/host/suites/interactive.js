const assert = require('node:assert/strict');

async function run(api) {
    const {check, resources, guestLog, value, launch, until, processes} = api;
    let baseline = await resources();
    await check('interactive launch and broker contracts', async () => {
        const offset=guestLog().length, faults=await value('process_faults');
        await launch('brokerqa','suite');
        await until(()=>guestLog().slice(offset).includes('INTERACTIVE SUITE PASS'),'G2 suite',30000);
        await until(async()=>(await processes()).length===0,'G2 jobs reaped');
        assert.equal(await value('process_faults'),faults);
        assert.deepEqual(await resources(), baseline);
    });
}
module.exports = {run};
