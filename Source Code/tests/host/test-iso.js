const {shellWindow, scene, waitFor} = require('../support/ui-test');
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const {VM, root} = require('../../tools/vm-test');
const {bootPayload} = require('../support/iso-boot');

(async () => {
    const iso = fs.readFileSync(require('../../tools/paths').iso);
    const boot = fs.readFileSync(path.join(root, 'sino.img'));
    assert(bootPayload(iso).equals(boot), 'ISO embeds the current boot image');
    const vm = new VM('iso-boot', 4563);
    try {
        await vm.start({fresh:true});
        const blocks = await vm.q.call('query-block');
        assert(blocks.some(b => b.inserted && b.inserted.file.endsWith('sino.iso')));
        assert(!blocks.some(b => b.device === 'floppy0' && b.inserted));
        await vm.key('f2');
        const w = await shellWindow(vm);
        await waitFor(async () => (await scene(vm, w)).some(c => c.kind === 9), 'shell renders after CD boot');
        await vm.shot('iso-boot');
        console.log('PASS: ISO9660 catalog checksum, exact boot payload and actual CD-only boot to native apps.');
    } finally {
        await vm.stop();
    }
})().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
