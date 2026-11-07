const assert = require('node:assert/strict');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
async function run({check, launch, job, guestLog, until}) {
        await check('intentional guest failure must reject the gate', async () => {
            await launch('probe', '/contracts-fail');
            // Log is written by several syscalls; wait for the complete reason
            // before asking the normal failure detector to reject it.
            const deadline = Date.now() + job.timeoutMs;
            while (!guestLog().includes('USER PROBE FAIL: intentional core runner failure\n')) {
                assert(!guestLog().includes('KERNEL PANIC'), 'panic is not the expected probe failure');
                assert(Date.now() < deadline, 'intentional failure marker timed out');
                await sleep(25);
            }
            await until(() => false, 'intentional probe cannot pass');
        });
        throw Error('The intentional failure was incorrectly accepted.');
}
module.exports = {run};
