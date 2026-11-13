/* Current memory contracts. Each guest uses a private data image. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {run} = require('../../tools/shell-test');
const {RunSession} = require('../../tools/run-channel');
const vm = require('../../tools/vm-config');

const memoryCases = require('../support/memory-cases.json');
const {assertCaseIds} = require('../support/regression-plan');
const executed = [];
const readJSON = file => JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

function bootFile(directory, name) {
    const iso = fs.readFileSync(path.join(directory, 'SINO.iso'));
    const manifest = readJSON(path.join(directory, 'boot-manifest.json'));
    const entry = manifest.files.find(file => file.name === name);
    assert(entry, 'missing boot file ' + name);
    const catalog = iso.readUInt32LE(17 * 2048 + 71) * 2048;
    const image = iso.subarray(iso.readUInt32LE(catalog + 40) * 2048);
    const sectorBytes = image.readUInt16LE(11), clusterBytes = image[13] * sectorBytes;
    const start = (image.readUInt16LE(14) + image[16] * image.readUInt16LE(22) +
        Math.ceil(image.readUInt16LE(17) * 32 / sectorBytes)) * sectorBytes;
    return Buffer.concat(entry.clusters.map(cluster => {
        const offset = start + (cluster - 2) * clusterBytes;
        return image.subarray(offset, offset + clusterBytes);
    })).subarray(0, entry.bytes);
}

async function boot(source, label, memoryMiB, verify, seedDisk) {
    const directory = fs.mkdtempSync(path.join(source, label + '-'));
    const iso = path.join(directory, 'SINO.iso');
    fs.copyFileSync(path.join(source, 'SINO.iso'), iso, fs.constants.COPYFILE_EXCL);
    const build = {...readJSON(path.join(source, 'build.json')), iso};
    fs.writeFileSync(path.join(directory, 'build.json'), JSON.stringify(build));
    fs.writeFileSync(path.join(directory, 'shell-job.json'), JSON.stringify({timeoutMs:5000}));
    const code = await run(directory, {memoryMiB, seedDisk, execute: async context => {
        const {channel, report, guestLog} = context;
        let seq = 0;
        async function command(text, status = 0) {
            const reply = await channel.request(++seq, text, 5000);
            assert.equal(reply.status, status, text + ': ' + reply.output.toString());
            assert(!reply.overflow, text);
            return reply.output.toString();
        }
        async function until(fn) {
            const deadline = Date.now() + 10000;
            while (Date.now() < deadline) {
                const failure = guestLog().match(/KERNEL PANIC[^\n]*|SELFTEST FAIL:[^\n]*/);
                assert(!failure, failure?.[0]);
                if (await fn()) return;
                await sleep(25);
            }
            assert.fail('Guest condition timed out.');
        }
        function free(out) {
            const match = out.match(/\/ (\d+) pages/);
            assert(match, 'page count');
            return Number(match[1]);
        }
        const baseline = free(await command('mem'));
        async function check(id, names, fn) {
            assert(memoryCases[mode].some(item => item.id === id), 'Unregistered memory case');
            assert(!executed.some(item => item.id === id), 'Repeated memory case');
            const items = [{id, name:names[0], conditions:names, result:'FAIL'}];
            executed.push(...items);
            report.cases.push(...items);
            try {
                await fn();
                for (const item of items) {
                    item.result = 'PASS';
                    console.log('PASS ' + item.name);
                }
            } catch (error) {
                for (const item of items) item.error = error.message;
                throw error;
            }
        }
        async function probe(cmd, marker, retained = 0, observe) {
            const offset = guestLog().length;
            const out = await command(cmd + '; ps; mem');
            const pid = Number(out.match(/Started user process (\d+)/)?.[1]);
            assert(pid, out);
            const log = () => guestLog().slice(offset);
            await until(() => marker.test(log()));
            if (observe) await observe({pid, log});
            await until(async () => free(await command('mem')) === baseline - retained);
            assert.doesNotMatch(await command('ps'), new RegExp('^' + pid + '\\s', 'm'));
        }
        await verify({channel, guestLog, command, until, free, baseline, check, probe, qmp:context.qmp, directory, report});
    }});
    if (code) throw Error('Failed gate: ' + directory);
    return path.join(directory, 'shell-data.img');
}

async function geometry(qmp, directory, log, baseline) {
    const dump = path.join(directory, 'e820.bin');
    await qmp.hmp(`pmemsave 0x4f00 1024 "${dump.replaceAll('\\', '/')}"`);
    const bytes = fs.readFileSync(dump), count = bytes.readUInt16LE(0);
    assert(count > 0 && count <= 32, 'E820 count');
    const ranges = [], pages = new Uint8Array(0x100000), top = 0x100000000n;
    let endPFN = 0;
    for (let i = 0; i < count; i++) {
        const offset = 0x100 + i * 24, base = bytes.readBigUInt64LE(offset);
        const length = bytes.readBigUInt64LE(offset + 8), type = bytes.readUInt32LE(offset + 16);
        if (!(bytes.readUInt32LE(offset + 20) & 1) || base >= top || !length) continue;
        const end = base + length < top ? base + length : top;
        const first = Number((base + (type === 1 ? 4095n : 0n)) / 4096n);
        const last = Number((end + (type === 1 ? 0n : 4095n)) / 4096n);
        if (first >= last) continue;
        ranges.push({first, last, type});
        if (type === 1) { pages.fill(1, first, last); endPFN = Math.max(endPFN, last); }
    }
    for (const range of ranges) if (range.type !== 1) pages.fill(0, range.first, range.last);
    const extendedPages = pages.subarray(64 * 1024 * 1024 / 4096).reduce((sum, n) => sum + n, 0);
    const actualEnd = Number(log.match(/nr_pages=(0x[0-9a-f]+)/i)?.[1]);
    const actualPublished = Number(log.match(/PAGE extended pages=(0x[0-9a-f]+)/i)?.[1]);
    assert.equal(actualEnd, endPFN, 'physical limit follows E820 below 4 GiB');
    assert.equal(actualPublished, extendedPages, 'reserved overlaps and MMIO holes stay unpublished');
    assert.match(log, /Direct mapping: \[0, 0x40000000\)/i);
    assert(endPFN > 0x40000 && baseline > 0x40000, 'real pages above the direct map are available');
    assert(baseline < endPFN, 'reserved and kernel-owned pages are not free');
    return {e820Entries:count, endPFN, extendedPages, freePages:baseline};
}

async function imageContracts(source) {
    const seed = await boot(source, 'images-4096', vm.memoryMiB, async ({baseline, free, check, probe, channel, command, guestLog, qmp, directory, report}) => {
        await check('memory.images.4-gib-e820-pool-and-single-core-identity', ['4 GiB single-core VM publishes only eligible E820 pages below 4 GiB'], async () => {
            report.memoryGeometry = await geometry(qmp, directory, guestLog(), baseline);
        });
        await check('memory.images.targeted-image-oom-rolls-back', ['partial SNX image allocation failure restores all resources'], async () => {
            const session = new RunSession(channel);
            await session.hello();
            const offset = guestLog().length;
            await assert.rejects(session.start('/boot/memv2.snx', ['oom-image']), e => e.guestStatus === -2);
            assert.match(guestLog().slice(offset), /MM PROBE injected stage=0x00000001/i);
            assert.equal(free(await command('mem')), baseline);
            assert.doesNotMatch(await command('ps'), /memv2/i);
        });
        await check('memory.images.targeted-table-oom-rolls-back', ['data page is reclaimed when its new page table allocation fails'], async () => {
            const offset = guestLog().length;
            await command('run -s /boot/poltest.snx paging-c-oom', 1);
            assert.match(guestLog().slice(offset), /MM PROBE injected stage=0x00000002[\s\S]*PAGING probe table OOM rollback passed/i);
            assert.equal(free(await command('mem')), baseline);
        });
        await check('memory.images.multi-pde-mappings-recover-after-injected-failure', ['multi-PDE mapping succeeds after the failed launch is reclaimed'], () =>
            probe('run -s /boot/poltest.snx paging-c', /PAGING C WINDOW PASS/));
        await check('memory.images.current-v1-binary-preserves-argv-and-typed-completion', ['current v1 binary preserves argv and typed completion'], async () => {
            const current = bootFile(source, 'CLITEST.SNX');
            assert.equal(current.readUInt32LE(4), 1);
            const session = new RunSession(channel);
            await session.hello();
            const result = await session.run('/boot/clitest.snx', ['args', '', 'old']);
            assert.equal(result.reason, 1);
            assert.equal(result.code, 0);
            assert.deepEqual(result.stdout, Buffer.from([3,0,0,0,4,0,0,0,97,114,103,115,0,0,0,0,3,0,0,0,111,108,100]));
            assert.equal(free(await command('mem')), baseline);
        });
        await check('memory.images.v1-startup-guards-and-thread-stack-reuse-remain-valid', ['v1 startup guards and thread stack reuse remain valid'], () =>
            probe('run -s /boot/poltest.snx paging-a', /PAGING STRICT A PASS/));
        await check('memory.images.v2-startup-strings-and-high-argument-stack-protection', ['v2 startup strings and high argument/stack protection',
            'wide zeroed BSS crosses the old image and dynamic boundaries',
            'v2 thread entry policy and relocated worker stack',
            '13 MiB anonymous heap skips the image and verifies every byte',
            'large workload allocates real high pages and returns to baseline'], async () => {
            const header = bootFile(source, 'MEMV2.SNX');
            assert.equal(header.readUInt32LE(4), 2);
            assert(header.readUInt32LE(20) > 15 * 1024 * 1024);
            await probe('run -s /boot/memv2.snx wide', /SNX V2 LARGE HEAP READY/, 0, async ({log}) => {
                const allocated = Number(guestLog().match(/PAGE first high allocation=(0x[0-9a-f]+)/i)?.[1]);
                assert(allocated >= 1024 * 1024 * 1024);
                assert(baseline - free(await command('mem')) > 7000, 'wide image and heap not resident together');
            });
            assert.match(guestLog(), /SNX V2 PASS/);
        });
        await check('memory.images.strict-file-pipe-and-lazy-cross-page-buffer-roundtrip', ['strict file, pipe and lazy cross-page buffer roundtrip'], () =>
            probe('run /boot/fstest.snx anon-parent', /ANON I\/O PASS[\s\S]*ANON PARENT PASS/));
        await check('memory.images.real-headless-gsh-runs-a-strict-worker-and-restores-its-prompt', ['real headless gsh runs a strict worker and restores its prompt'], () =>
            probe('run /boot/brokerqa.snx shell-smoke', /SHELL PASS/));
        await check('memory.images.private-disk-commits-binary-bytes-on-the-4-gib-image', ['private disk commits binary bytes on the 4 GiB image'], () =>
            probe('run /boot/fstest.snx memory-save', /MEMORY SAVE PASS/, 1));
    });
    await boot(source, 'images-cold', vm.memoryMiB, async ({check, probe}) =>
        check('memory.images.cold-boot-on-the-same-iso-recovers-the-exact-binary-bytes', ['cold boot on the same ISO recovers the exact binary bytes'], () =>
            probe('run /boot/fstest.snx memory-read', /MEMORY READ PASS/)), seed);
}

async function anonymousContracts(source) {
    await boot(source, 'anonymous', vm.memoryMiB, async ({check, probe, command, free, baseline}) => {
        await check('memory.anonymous.strict-feature-discovery', ['strict feature discovery', 'zero, overflow and reserved map arguments',
            'rounded aligned independent regions', 'zero-filled pages and independent writes',
            'malformed and interior unmap leave data intact', 'released address and double-unmap rejected',
            'first-fit reuse starts zeroed', '32-region quota and slot reuse', 'exit releases outstanding regions'], () =>
            probe('run -s /boot/fstest.snx anon-map', /ANON MAPPING PASS/));
        await check('memory.anonymous.exact-16-mib-quota-and-one-byte-excess', ['exact 16 MiB quota and one-byte excess', 'unaligned and external unmap rejected',
            'last byte of the exact region is accessible', 'full unmap returns quota and space'], () =>
            probe('run -s /boot/fstest.snx anon-d-quota', /ANON D QUOTA PASS/));
        await check('memory.anonymous.strict-granted-cross-page-file-i-o', ['strict granted cross-page file I/O', 'anonymous pipe I/O and teardown'], () =>
            probe('run /boot/fstest.snx anon-parent', /ANON I\/O PASS[\s\S]*ANON PARENT PASS/));
        await check('memory.anonymous.released-address-faults-and-exit-reclaims-the-remaining-region', ['released address faults and exit reclaims the remaining region'], () =>
            probe('run -s /boot/fstest.snx anon-fault', /ANON EXPECT RELEASED PAGE FAULT[\s\S]*USER fault contained pid=.*vector=0x0000000e/i));
        await check('memory.anonymous.physical-oom-leaves-output-intact', ['injected anonymous allocation failure leaves output intact', 'injected anonymous allocation failure preserves pending pipe bytes'], () =>
            probe('run -s /boot/fstest.snx anon-oom', /MM PROBE injected stage=0x00000003[\s\S]*ANON OOM PASS/i));
        await check('memory.anonymous.injected-first-touch-oom-is-contained', ['first-touch anonymous allocation failure faults only the target process'], () =>
            probe('run -s /boot/fstest.snx anon-fault-oom', /ANON EXPECT FIRST-TOUCH OOM FAULT[\s\S]*USER fault contained pid=.*vector=0x0000000e/i));
        await check('memory.anonymous.writable-probe-copy-is-not-injected', ['a writable copy with the same argument cannot enable memory injection'], async () => {
            await command('cp /boot/fstest.snx /home/user/probe-copy.snx');
            const retained = baseline - free(await command('mem'));
            try {
                await probe('run -s /home/user/probe-copy.snx anon-oom', /ANON UNINJECTED PASS/, retained,
                    async ({log}) => assert.doesNotMatch(log(), /MM PROBE injected/));
            } finally { await command('rm /home/user/probe-copy.snx'); }
            assert.equal(free(await command('mem')), baseline);
        });
    });
}

const [mode, source] = process.argv.slice(2);
if (!Object.hasOwn(memoryCases, mode) || process.argv.length !== 4) {
    console.error('Usage: node test-memory.js images|anonymous <ShellBridge build directory>|--list');
    process.exitCode = 2;
} else if (source === '--list') {
    console.log(mode + ': ' + memoryCases[mode].length + ' cases');
    for (const item of memoryCases[mode]) console.log(item.id + '  ' + item.name);
} else {
    (mode === 'images' ? imageContracts(path.resolve(source)) : anonymousContracts(path.resolve(source)))
        .then(() => {
            assertCaseIds(executed, memoryCases[mode]);
            fs.writeFileSync(path.join(path.resolve(source), 'memory-' + mode + '-report.json'),
                JSON.stringify({result:'PASS', cases:executed}, null, 2));
            console.log('PASS memory ' + mode + ': ' + executed.length + ' unique cases');
        }).catch(error => { console.error(error); process.exitCode = 1; });
}
