/* Private image/installation test. No daily or physical disk is opened. */
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const {execFileSync, spawnSync} = require('node:child_process');
const {VM, root, sleep} = require('./vm-test');
const {waitFor} = require('./calculator-test');
const layout = require('./boot-layout');
const vm = new VM('boot-expansion');
const checks = [];
const pass = name => { checks.push(name); console.log('PASS ' + name); };
(async () => {
    try {
        const image = Buffer.from(fs.readFileSync(path.join(root, 'sino.img')));
        assert.equal(image.length, 2949120);
        assert.equal(image.readUInt16LE(19), layout.BOOT_SECTORS);
        assert.equal(image[13], 2);
        assert.equal(image.readUInt16LE(24), 36);
        pass('expanded BPB and image size');
        const reserved = image.readUInt16LE(14), fatBytes = image.readUInt16LE(22) * 512;
        const directory = (reserved * 512 + fatBytes * 2);
        const data = directory + image.readUInt16LE(17) * 32;
        const clusterBytes = image[13] * 512;
        // A fragmented file spans a cluster boundary and reaches the last full
        // data cluster, beyond the old image. Both whole/range readers use it.
        const occupied = JSON.parse(fs.readFileSync(path.join(root, 'build32/manifest-normal.json'))).files.flatMap(f => f.clusters);
        const first = Math.max(Math.ceil((1474560 - data) / clusterBytes) + 2, Math.max(...occupied) + 1);
        const last = Math.floor((image.length - data) / clusterBytes) + 1;
        const content = Buffer.from('a'.repeat(1023) + '\nEXPANDED TAIL PASS\n');
        const setFat = (cluster, value) => {
            for (let copy = 0; copy < 2; copy++) {
                const n = reserved * 512 + copy * fatBytes + cluster + (cluster >> 1);
                if (cluster & 1) { image[n] = (image[n] & 15) | ((value & 15) << 4); image[n+1] = value >> 4; }
                else { image[n] = value & 255; image[n+1] = (image[n+1] & 240) | (value >> 8); }
            }
        };
        setFat(first, last); setFat(last, 0xfff);
        content.copy(image, data + (first - 2) * clusterBytes, 0, clusterBytes);
        content.copy(image, data + (last - 2) * clusterBytes, clusterBytes);
        let entry = directory;
        while (image[entry]) entry += 32;
        image.write('EXTENT  TXT', entry, 'ascii'); image[entry+11] = 32;
        image.writeUInt16LE(first, entry+26); image.writeUInt32LE(content.length, entry+28);
        const payload = path.join(vm.run, 'expanded.img'), iso = path.join(vm.run, 'expanded.iso');
        fs.writeFileSync(payload, image);
        execFileSync(process.execPath, [path.join(root, 'build-iso.js'), payload, iso], {stdio:'pipe'});
        const optical = fs.readFileSync(iso);
        const catalog = optical.readUInt32LE(17 * 2048 + 71) * 2048;
        assert.equal(optical[catalog+33], 3);
        assert(optical.subarray(24*2048, 24*2048+image.length).equals(image));
        pass('dynamic ISO layout preserves the full expanded payload');
        for (const [name, bytes] of [['truncated', image.subarray(0,1474560)], ['bpb', Buffer.from(image)]]) {
            if (name === 'bpb') bytes.writeUInt16LE(2880, 19);
            const file = path.join(vm.run, name+'.img'); fs.writeFileSync(file, bytes);
            assert.notEqual(spawnSync(process.execPath, [path.join(root,'build-iso.js'),file,path.join(vm.run,name+'.iso')], {stdio:'pipe'}).status, 0);
        }
        pass('truncated and mismatched geometry images rejected');
        // paths is shared with VM, so this selects only the private fixture.
        require('./paths').iso = iso;
        await vm.start({fresh:true, diskBytes:128*1024*1024});
        assert((await vm.memory(0x200000,image.length)).equals(image));
        pass('CD boot preloads every byte including the last data cluster');
        async function at(x,y) {
            const s=await vm.state(), w=s.windows.find(w=>w.id===s.focus);
            assert(w && w.app===8); await vm.click(w.x+x,w.y+y);
        }
        async function next() { const s=await vm.state(),w=s.windows.find(w=>w.id===s.focus); await at(w.w-92,w.h-39); }
        await next(); await next(); await next();
        const s=await vm.state(),w=s.windows.find(w=>w.id===s.focus);
        await at(609,w.h-96); await next(); await next();
        await waitFor(async()=>await vm.value('install_complete')===1,'expanded installation',30000);
        await vm.stop();
        const disk=fs.readFileSync(path.join(vm.run,'data.img')), base=disk.readUInt32LE(454);
        const installed=Buffer.from(image); installed.writeUInt32LE(base,28); installed[36]=128;
        assert(disk.subarray(base*512,base*512+image.length).equals(installed));
        pass('installer copies and verifies all 5760 sectors');
        await vm.start({hardBoot:true});
        assert((await vm.memory(0x200000,installed.length)).equals(installed));
        assert.equal(await vm.value('storage_ready'),1);
        pass('installed HDD boots with optical media disconnected and full RAM image');
        await vm.key('f2'); await vm.type('cat /boot/extent.txt\n');
        const terminal = (await vm.state()).windows.find(w => w.title === 'SINO Shell');
        assert(terminal);
        const {terminalText} = require('./test-terminal-replacement');
        await waitFor(async()=> (await terminalText(vm,terminal.id,false)).includes('EXPANDED TAIL PASS'),
            'read fragmented tail file');
        pass('filesystem reads fragmented file beyond old capacity on installed HDD');
        await vm.type('run /boot/probe.snx /boot-extent\n');
        await waitFor(async()=>vm.log().includes('BOOT EXTENT RANGE PASS'),'cross cluster range API');
        assert(!vm.log().includes('USER PROBE FAIL'));
        pass('file object range API reads exact bytes across fragmented clusters');

    } catch(error) { console.error(error.message); process.exitCode=1; }
    finally {
        await vm.stop();
        fs.writeFileSync(path.join(vm.run,'expansion-report.json'), JSON.stringify({result:process.exitCode?'FAIL':'PASS',checks,vmStopped:!vm.child||vm.child.exitCode!==null||vm.child.signalCode!==null},null,2));
        console.log(`${process.exitCode?'FAIL':'PASS'} boot expansion: ${checks.length} checks; private VM stopped`);
    }
})();
