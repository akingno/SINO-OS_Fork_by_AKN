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
    const bootStart = 52 * 1024 * 1024 / 4096, bootEnd = 64 * 1024 * 1024 / 4096;
    const metadataPages = Math.ceil(endPFN * 8 / 4096);
    const metadata = log.match(/PAGE metadata base=(0x[0-9a-f]+) pages=(0x[0-9a-f]+)/i);
    assert(metadata, 'boot metadata placement');
    assert.equal(Number(metadata[2]), metadataPages, 'eight bytes of metadata per PFN');
    const metadataStart = Number(metadata[1]) / 4096;
    assert(Number.isInteger(metadataStart) && metadataStart >= bootStart &&
        metadataStart + metadataPages <= bootEnd, 'metadata fits inside the boot pool');
    assert(pages.subarray(metadataStart, metadataStart + metadataPages).every(page => page === 1),
        'metadata occupies one usable E820 span');
    assert.match(log, /Direct mapping: \[0, 0x40000000\)/i);
    const directEnd = 0x40000;
    const lowPublished = pages.subarray(bootStart, directEnd).reduce((sum, n) => sum + n, 0) - metadataPages;
    const highPublished = pages.subarray(directEnd).reduce((sum, n) => sum + n, 0);
    const publishedPages = lowPublished + highPublished;
    for (const [pool, published] of Object.entries({total:publishedPages, low:lowPublished, high:highPublished})) {
        const watermarks = log.match(new RegExp(`PAGE watermarks ${pool}=(0x[0-9a-f]+) criticalBelow=(0x[0-9a-f]+) normalAbove=(0x[0-9a-f]+)`, 'i'));
        assert(watermarks, `published watermarks for ${pool}`);
        assert.deepEqual(watermarks.slice(1).map(Number), [published, Math.ceil(published * 5 / 100),
            Math.floor(published * 15 / 100)], `${pool} watermarks include pages already allocated`);
    }
    assert(endPFN > 0x40000 && baseline > 0x40000, 'real pages above the direct map are available');
    assert(baseline < endPFN, 'reserved and kernel-owned pages are not free');
    return {e820Entries:count, endPFN, extendedPages, publishedPages, freePages:baseline};
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
            'first-fit reuse starts zeroed', '128-region quota, slot reuse and independence across bitmap words', 'exit releases outstanding regions'], () =>
            probe('run -s /boot/fstest.snx anon-map', /ANON MAPPING PASS/));
        await check('memory.anonymous.exact-byte-quota-and-one-byte-excess', ['exact 128 MiB quota and one-byte excess', 'unaligned and external unmap rejected',
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

function pressureTools({command, free, baseline, until, guestLog}) {
    async function launch(mode, marker, strict = true) {
        const offset = guestLog().length;
        const out = await command(`run ${strict ? '-s ' : ''}/boot/reclaim.snx ${mode}`);
        const pid = Number(out.match(/^Started user process (\d+)\n$/)?.[1]);
        assert(pid, out);
        await until(() => guestLog().slice(offset).includes(marker));
        return pid;
    }
    async function processes() {
        const table = await command('ps');
        return new Map([...table.matchAll(/^(\d+)\s+\d+\s+(\d+)\s+\S+$/gm)]
            .map(([, pid, pages]) => [Number(pid), Number(pages)]));
    }
    async function currentFree() { return free(await command('mem')); }
    async function override(critical, normal, expected) {
        assert.equal(await command(`mmtest-watermark ${critical} ${normal}`), `${expected}\n`);
    }
    async function reset() { assert.equal(await command('mmtest-watermark reset'), '0\n'); }
    async function cleanup() {
        await reset();
        for (const pid of (await processes()).keys()) { await command(`kill ${pid}`); }
        await until(async () => (await processes()).size === 0 && await currentFree() === baseline);
    }
    return {launch, processes, currentFree, override, reset, cleanup};
}

function poolStats(output) {
    const stats = ['Low', 'High'].map(pool => {
        const match = output.match(new RegExp(`^${pool} pool: (\\d+) free / (\\d+) published \\(watermark: (Normal|Low|Critical)\\)$`, 'm'));
        assert(match, `missing ${pool} pool statistics`);
        const [, free, published, watermark] = match;
        assert(Number(free) <= Number(published), `${pool} free pages exceed published pages`);
        return {free:Number(free), published:Number(published), watermark};
    });
    assert.equal(stats[0].free + stats[1].free, Number(output.match(/\/ (\d+) pages/)?.[1]));
    return stats;
}

const releaseFields = / low_before=(0x[\da-f]+) low_after=(0x[\da-f]+) high_before=(0x[\da-f]+) high_after=(0x[\da-f]+)/i.source;
function releasedPools(lowBefore, lowAfter, highBefore, highAfter) {
    return {low:Number(lowAfter) - Number(lowBefore), high:Number(highAfter) - Number(highBefore)};
}

function victims(log) {
    const fields = /MEM reclaim pid=(0x[\da-f]+) priority=(0x[\da-f]+) pages=(0x[\da-f]+) pass=(0x[\da-f]+) before=(0x[\da-f]+) after=(0x[\da-f]+)/i.source;
    return [...log.matchAll(new RegExp(fields + releaseFields, 'gi'))]
        .map(([, pid, score, pages, pass, before, after, ...pools]) => ({pid:Number(pid), score:Number(score),
            pages:Number(pages), pass:Number(pass), before:Number(before), after:Number(after), ...releasedPools(...pools)}));
}

function purges(log) {
    const fields = /MEM purge pid=(0x[\da-f]+) base=(0x[\da-f]+) pages=(0x[\da-f]+) pass=(0x[\da-f]+)/i.source;
    return [...log.matchAll(new RegExp(fields + releaseFields, 'gi'))]
        .map(([, pid, base, pages, pass, ...pools]) => ({pid:Number(pid), base:Number(base),
            pages:Number(pages), pass:Number(pass), ...releasedPools(...pools)}));
}

async function heapContracts(source) {
    await boot(source, 'heap', vm.memoryMiB, async context => {
        const {channel, check, guestLog, until, baseline} = context;
        const {processes, currentFree, override, reset, cleanup} = pressureTools(context);
        const session = new RunSession(channel);
        await session.hello();
        for (const item of memoryCases.heap) {
            const probeMode = item.id.split('.').pop();
            await check(item.id, [item.name, 'all pages return to baseline after exit'], async () => {
                const offset = guestLog().length;
                const log = () => guestLog().slice(offset);
                const program = '/boot/heaptest.snx';
                let result;
                if (probeMode === 'purge') {
                    const token = await session.start(program, [probeMode], Buffer.alloc(0), 1800);
                    try {
                        await until(() => /HEAP PURGE READY \d+\n/.test(log()));
                        const base = Number(log().match(/HEAP PURGE READY (\d+)/)[1]);
                        const active = await processes();
                        assert.equal(active.size, 1);
                        const [[pid, pages]] = active;
                        const before = await currentFree();
                        await override(before - 32, before + 128, 1);
                        await until(() => log().includes('HEAP PURGE DROPPED'));
                        const dropped = purges(log());
                        assert.equal(dropped.length, 1);
                        assert.equal(dropped[0].pid, pid);
                        assert.equal(dropped[0].base, base);
                        assert(dropped[0].pages > 0 && dropped[0].pages < pages);
                        assert.equal(await currentFree(), before + dropped[0].pages);
                        assert.deepEqual(await processes(), new Map([[pid, pages - dropped[0].pages]]));
                        assert.deepEqual(victims(log()), []);
                        await reset();
                        await until(async () => (result = await session.poll(token)) !== null);
                    } finally {
                        await session.release(token);
                        await cleanup();
                    }
                } else {
                    result = await session.run(program, [probeMode], Buffer.alloc(0), {timeoutTicks:1800});
                }
                assert.doesNotMatch(log(), /SELFTEST FAIL|KERNEL PANIC/);
                assert.equal(result.reason, 1);
                assert.equal(result.code, 0);
                assert.match(log(), /HEAP PASS/);
                await until(async () => await currentFree() === baseline);
            });
        }
    });
}

async function reclaimContracts(source) {
    await boot(source, 'reclaim', vm.memoryMiB, async context => {
        const {check, command, baseline, until, guestLog, channel, qmp, directory} = context;
        const {launch, processes, currentFree, override, cleanup} = pressureTools(context);

        await check('memory.reclaim.pool-and-reservation-boundaries', ['published pool, exact watermarks and anonymous reservation rules'], async () => {
            await geometry(qmp, directory, guestLog(), baseline);
            try {
                await override(baseline - 32, baseline - 1, 0);
                await override(baseline - 32, baseline, 1);
                await override(baseline, baseline + 32, 1);
                await override(baseline + 1, baseline + 32, 2);
                assert.equal(await currentFree(), baseline, 'watermark override leaves the allocator untouched');
                assert.equal(await command('mmtest-watermark reset'), '0\n');

                const offset = guestLog().length;
                await launch('low', 'RECLAIM LOW READY');
                const available = await currentFree();
                await override(available - 32, available, 1);
                await until(() => guestLog().slice(offset).includes('RECLAIM LOW PASS'));
                assert.deepEqual(victims(guestLog().slice(offset)), [], 'LOW must not kill');
            } finally { await cleanup(); }
        });

        await check('memory.reclaim.bounded-priority-and-stop-at-low', ['reclaim orders by score, resident pages and slot and stops at LOW'], async () => {
            try {
                const protectedPid = await launch('protect', 'RECLAIM HOLD READY');
                const medium = await launch('hold-large', 'RECLAIM HOLD READY', false);
                const firstSmall = await launch('hold-small', 'RECLAIM HOLD READY');
                const secondSmall = await launch('hold-small', 'RECLAIM HOLD READY');
                const before = await currentFree();
                const large = await launch('hold-large', 'RECLAIM HOLD READY');
                const available = await currentFree();
                const critical = before + 1;
                const offset = guestLog().length;
                await override(critical, critical + 256, 2);
                await until(() => victims(guestLog().slice(offset)).length >= 2);
                const reaped = victims(guestLog().slice(offset));
                assert.deepEqual(reaped.map(item => [item.pid, item.score, item.pages]), [[large, 400, 16], [firstSmall, 400, 2]]);
                assert(reaped[1].pass > reaped[0].pass, 'at most one victim per scheduler pass');
                assert.equal(reaped[0].before, available);
                assert.equal(reaped[0].after, before, 'first victim restores all owned frames');
                assert.equal(reaped[1].before, before);
                assert(reaped[1].after >= critical && reaped[1].after <= critical + 256, 'second victim restores LOW');
                for (const victim of reaped) {
                    assert(victim.low >= 9 && victim.high >= victim.pages, 'exit returns low kernel resources and high data');
                    assert.equal(victim.low + victim.high, victim.after - victim.before, 'pool deltas reconcile with the global delta');
                }
                assert.deepEqual(await processes(), new Map([[protectedPid, 24], [medium, 16], [secondSmall, 2]]));
            } finally { await cleanup(); }
        });

        await check('memory.reclaim.first-touch-retries-after-release', ['first touch resumes after real release and charges one anonymous page'], async () => {
            try {
                const victim = await launch('hold-small', 'RECLAIM HOLD READY');
                const offset = guestLog().length;
                const requester = await launch('retry', 'RECLAIM TOUCH PASS');
                const log = guestLog().slice(offset);
                assert.match(log, /injected stage=0x00000004[\s\S]*MEM reclaim pid=[^\n]+[\s\S]*RECLAIM TOUCH PASS/);
                const reaped = victims(log);
                assert.deepEqual(reaped.map(item => item.pid), [victim]);
                assert(reaped[0].after > reaped[0].before);
                assert.deepEqual(await processes(), new Map([[requester, 1]]));
            } finally { await cleanup(); }
        });

        await check('memory.reclaim.exhaustion-preserves-failure-contracts', ['no eligible victim preserves syscall OOM and contains the retried fault',
            'page table OOM rolls back the anonymous data page and skips global reclaim'], async () => {
            try {
                const offset = guestLog().length;
                const caller = await launch('copy', 'RECLAIM COPY PASS');
                await until(() => guestLog().slice(offset).includes('MEM reclaim no eligible process'));
                await sleep(150);
                const log = guestLog().slice(offset);
                assert.equal([...log.matchAll(/MEM reclaim no eligible process/g)].length, 1, 'no busy rescans');
                assert.deepEqual(await processes(), new Map([[caller, 0]]));
                assert.deepEqual(victims(log), []);
                await command(`kill ${caller}`);
                await until(async () => (await processes()).size === 0 && await currentFree() === baseline);

                const session = new RunSession(channel);
                await session.hello();
                const faultOffset = guestLog().length;
                const fault = await session.run('/boot/reclaim.snx', ['fault']);
                assert.equal(fault.reason, 2, 'exhausted requester is faulted, not killed');
                assert.equal(fault.vector, 14);
                const faultLog = guestLog().slice(faultOffset);
                assert.equal([...faultLog.matchAll(/injected stage=0x00000004/g)].length, 2, 'one original attempt and one retry');
                assert.match(faultLog, /MEM reclaim no eligible process[\s\S]*USER fault contained/);
                assert.match(faultLog, /USER anonymous data page OOM/);
                assert.deepEqual(victims(faultLog), []);

                const tableOffset = guestLog().length;
                const tableCaller = await launch('table-copy', 'RECLAIM TABLE COPY PASS');
                assert.deepEqual(await processes(), new Map([[tableCaller, 0]]), 'failed page table does not charge an anonymous page');
                await command(`kill ${tableCaller}`);
                await until(async () => (await processes()).size === 0 && await currentFree() === baseline);
                const tableFault = await session.run('/boot/reclaim.snx', ['table-fault']);
                assert.equal(tableFault.reason, 2);
                assert.equal(tableFault.vector, 14);
                const tableLog = guestLog().slice(tableOffset);
                assert.equal([...tableLog.matchAll(/injected stage=0x00000002/g)].length, 2);
                assert.match(tableLog, /USER anonymous page table OOM/);
                assert.doesNotMatch(tableLog, /MEM reclaim|USER anonymous data page OOM/);
                assert.equal(await command('echo responsive'), 'responsive\n');
            } finally { await cleanup(); }
        });

        await check('memory.reclaim.process-group-lifetime', ['managed group inheritance, deferred peer cleanup and PID generation isolation'], async () => {
            async function startGroup(mode, expectedScore) {
                const offset = guestLog().length;
                const parent = await launch(mode, 'RECLAIM GROUP PARENT ', false);
                await until(() => {
                    const log = guestLog().slice(offset);
                    return log.includes(`RECLAIM GROUP PARENT ${parent}\n`) &&
                        log.includes(`RECLAIM GROUP MEMBER ${parent}\n`);
                });
                const log = guestLog().slice(offset);
                const launches = [...log.matchAll(/PROCESS start reclaim\.snx pid=(0x[\da-f]+) reclaim=(0x[\da-f]+)/gi)];
                assert.equal(launches.length, 3, 'parent, completed child and live child');
                const [, childPid, score] = launches[2];
                assert.equal(Number(score), expectedScore);
                assert.match(log, new RegExp(`PROCESS group pid=${childPid} id=0x[\\da-f]+`, 'i'));
                return {parent, child:Number(childPid)};
            }

            try {
                const session = new RunSession(channel);
                await session.hello();
                const standalone = await session.run('/boot/reclaim.snx', ['group-id']);
                assert.equal(standalone.reason, 1);
                assert.equal(standalone.code, standalone.pid, 'host-managed run starts its own group');

                const unrelated = await launch('hold-small', 'RECLAIM HOLD READY', false);
                const unrelatedFree = await currentFree();
                const {parent, child} = await startGroup('group-parent', 300);
                assert.deepEqual(await processes(), new Map([[unrelated, 2], [parent, 1], [child, 2]]));
                const available = await currentFree();
                const offset = guestLog().length;
                await override(available + 1, available + 256, 2);
                await until(async () => (await processes()).size === 1 && await currentFree() === unrelatedFree);
                const reaped = victims(guestLog().slice(offset));
                assert.deepEqual(reaped.map(item => item.pid), [child], 'only one primary victim is selected');
                assert.equal(reaped[0].before, available);
                assert(reaped[0].after > available && reaped[0].after < unrelatedFree,
                    'the primary releases first and its peer releases in the next pass');
                assert.deepEqual(await processes(), new Map([[unrelated, 2]]));
            } finally { await cleanup(); }

            try {
                const {parent, child} = await startGroup('group-parent-max', 400);
                await command(`kill ${parent}`);
                await until(async () => !(await processes()).has(parent));
                assert.deepEqual(await processes(), new Map([[child, 2]]), 'ordinary kill does not propagate to the group');
                const replacement = await launch('hold-small', 'RECLAIM HOLD READY', false);
                assert.equal(replacement & 0x7f, parent & 0x7f);
                assert.notEqual(replacement, parent);
                const available = await currentFree();
                const offset = guestLog().length;
                await override(available + 1, available + 256, 2);
                await until(async () => !(await processes()).has(child));
                assert.deepEqual(victims(guestLog().slice(offset)).map(item => item.pid), [child]);
                assert.deepEqual(await processes(), new Map([[replacement, 2]]), 'reused PID slot is a different group');
            } finally { await cleanup(); }
        });
    });
}

async function purgeableContracts(source) {
    await boot(source, 'purgeable', vm.memoryMiB, async context => {
        const {check, probe, until, guestLog} = context;
        const {launch, processes, currentFree, override, reset, cleanup} = pressureTools(context);
        await check('memory.purgeable.abi-and-lock-limits', ['purgeable ABI, reservation ownership and lock count edges'], () =>
            probe('run -s /boot/reclaim.snx purge-abi', /PURGE ABI PASS/));

        await check('memory.purgeable.low-discard-and-rebuild', ['LOW discards resident cache, preserves neighbors and supports rebuild',
            'equal discardable candidates in one process are ordered by virtual address'], async () => {
            const offset = guestLog().length;
            const log = () => guestLog().slice(offset);
            try {
                const pid = await launch('purge-cache', 'PURGE CACHE READY');
                assert.deepEqual(await processes(), new Map([[pid, 5]]));
                const before = await currentFree();
                await override(before - 32, before + 128, 1);
                await until(() => log().includes('PURGE CACHE DROPPED'));
                assert.deepEqual(purges(log()).map(item => [item.pid, item.pages, item.low, item.high]), [[pid, 3, 0, 3]]);
                assert.equal(await currentFree(), before + 3, 'other mappings keep the shared page table alive');
                assert.deepEqual(await processes(), new Map([[pid, 2]]));
                assert.deepEqual(victims(log()), []);
                await reset();
                await until(() => log().includes('PURGE CACHE PASS'));
                await until(async () => !(await processes()).has(pid));
            } finally { await cleanup(); }

            const tieOffset = guestLog().length;
            const tieLog = () => guestLog().slice(tieOffset);
            try {
                const pid = await launch('purge-address-order', 'PURGE ADDRESS ORDER READY');
                assert.deepEqual(await processes(), new Map([[pid, 2]]));
                const before = await currentFree();
                // Reclaiming one page crosses back to NORMAL, leaving the other candidate resident.
                await override(before - 32, before, 1);
                await until(() => tieLog().includes('PURGE ADDRESS ORDER PASS'));
                assert.deepEqual(purges(tieLog()).map(item => [item.pid, item.pages]), [[pid, 1]]);
                assert.deepEqual(await processes(), new Map([[pid, 1]]));
            } finally { await cleanup(); }
        });

        await check('memory.purgeable.critical-discard-before-kill', ['CRITICAL prefers cache by priority and can satisfy a deferred fault'], async () => {
            try {
                const lower = await launch('purge-hold-large', 'PURGE HOLD READY', false);
                const higher = await launch('purge-hold', 'PURGE HOLD READY');
                const ordinary = await launch('hold-large', 'RECLAIM HOLD READY');
                const before = await currentFree();
                const offset = guestLog().length;
                await override(before + 1, before + 1, 2);
                await until(() => purges(guestLog().slice(offset)).length > 0);
                assert.deepEqual(purges(guestLog().slice(offset)).map(item => [item.pid, item.pages, item.low, item.high]), [[higher, 2, 1, 2]]);
                assert.deepEqual(victims(guestLog().slice(offset)), []);
                assert.deepEqual(await processes(), new Map([[lower, 4], [higher, 0], [ordinary, 16]]));
            } finally { await cleanup(); }

            try {
                const cache = await launch('purge-hold', 'PURGE HOLD READY');
                const offset = guestLog().length;
                const requester = await launch('retry', 'RECLAIM TOUCH PASS');
                const log = guestLog().slice(offset);
                assert.match(log, /injected stage=0x00000004[\s\S]*MEM purge pid=[^\n]+[\s\S]*RECLAIM TOUCH PASS/);
                assert.deepEqual(victims(log), []);
                assert.deepEqual(await processes(), new Map([[cache, 0], [requester, 1]]));
            } finally { await cleanup(); }
        });

        await check('memory.purgeable.insufficient-cache-falls-back', ['insufficient discardable cache falls back to process reclaim on a later pass'], async () => {
            try {
                const cache = await launch('purge-hold', 'PURGE HOLD READY');
                const ordinary = await launch('hold-large', 'RECLAIM HOLD READY');
                const before = await currentFree();
                const offset = guestLog().length;
                const log = () => guestLog().slice(offset);
                await override(before + 8, before + 256, 2);
                await until(() => victims(log()).length > 0);
                const dropped = purges(log());
                const reaped = victims(log());
                assert.deepEqual(dropped.map(item => [item.pid, item.pages]), [[cache, 2]]);
                assert.deepEqual(reaped.map(item => item.pid), [ordinary]);
                assert(dropped[0].pass < reaped[0].pass, 'purge is completed before process reclaim');
                assert(reaped[0].before > before && reaped[0].before < before + 8);
                assert.deepEqual(await processes(), new Map([[cache, 0]]));
            } finally { await cleanup(); }
        });

        await check('memory.purgeable.last-unlock-rearms-pressure', ['last unlock schedules discard again under unchanged LOW pressure'], async () => {
            const offset = guestLog().length;
            const log = () => guestLog().slice(offset);
            try {
                const pid = await launch('purge-unlock', 'PURGE UNLOCK READY');
                const available = await currentFree();
                await override(available - 32, available + 128, 1);
                await until(() => log().includes('PURGE UNLOCK PASS') && purges(log()).length >= 2);
                const dropped = purges(log());
                assert.deepEqual(dropped.map(item => [item.pid, item.pages]), [[pid, 1], [pid, 1]]);
                assert.equal(dropped[0].base, dropped[1].base);
                assert.deepEqual(victims(log()), []);
                assert.deepEqual(await processes(), new Map([[pid, 0]]));
            } finally { await cleanup(); }
        });
    });
}

async function poolContracts(source) {
    function poolTools(context) {
        const {command, baseline} = context;
        const pressure = pressureTools(context);
        async function pools() { return poolStats(await command('mem')); }
        async function setPools(criticalLow, normalLow, criticalHigh, normalHigh, expected) {
            assert.equal(await command(`mmtest-watermark ${criticalLow} ${normalLow} ${criticalHigh} ${normalHigh}`),
                expected + '\n');
        }
        async function signal() {
            // Global LOW releases the guest's reservation barrier without permitting process reclaim.
            await pressure.override(0, baseline + 32, 1);
        }
        return {...pressure, pools, setPools, signal};
    }

    await boot(source, 'pools-dual', vm.memoryMiB, async context => {
        const {check, command, until, guestLog} = context;
        const {pools, setPools, launch, reset, cleanup, signal, processes} = poolTools(context);
        await check('memory.pools.independent-watermarks', ['pool thresholds, independent transitions and atomic overrides'], async () => {
            const original = await pools();
            assert.deepEqual(poolStats(await command('free')), original);
            const [low, high] = original.map(pool => pool.free);
            assert(low > 0 && high > 0, 'both pools are published and have free pages');
            const offset = guestLog().length;
            const log = () => guestLog().slice(offset);
            try {
                await setPools(low - 1, low - 1, high + 1, high + 1, '0 2');
                await until(() => /MEM watermark high from=0x00000000 to=0x00000002/i.test(log()));
                await setPools(low - 1, low, high + 1, high + 1, '1 2');
                await until(() => /MEM watermark low from=0x00000000 to=0x00000001/i.test(log()));
                await setPools(low, low + 1, high + 1, high + 1, '1 2');
                await setPools(low + 1, low + 1, high + 1, high + 1, '2 2');
                await until(() => /MEM watermark low from=0x00000001 to=0x00000002/i.test(log()));
                await setPools(low + 1, low + 1, high, high + 1, '2 1');
                await setPools(low + 1, low + 1, high - 1, high, '2 1');
                await setPools(low + 1, low + 1, high - 1, high - 1, '2 0');
                const active = await pools();
                assert.deepEqual(active, [{...original[0], watermark:'Critical'}, original[1]]);
                assert.match(await command('mmtest-watermark 0 0 1 0', 1), /invalid pool thresholds/);
                assert.deepEqual(await pools(), active, 'invalid high thresholds must leave low untouched');
                await reset();
                assert.deepEqual(await pools(), original);
                assert.doesNotMatch(log(), /MEM watermark from=|MEM reclaim|MEM purge/);
            } finally { await cleanup(); }
        });

        await check('memory.pools.high-preferred', ['high supplies data when low is Critical'], async () => {
            const original = await pools();
            const offset = guestLog().length;
            try {
                await launch('pool-populate', 'POOL POPULATE READY');
                const [low, high] = await pools();
                assert(high.free > 0);
                await setPools(low.free + 1, low.free + 1, 0, 0, '2 0');
                await signal();
                await until(() => guestLog().slice(offset).includes('POOL POPULATE DONE'));
                assert.deepEqual(await pools(), [{...low, watermark:'Critical'}, {...high, free:high.free - 1}]);
                await reset();
                await until(() => guestLog().slice(offset).includes('POOL POPULATE UNMAPPED'));
                await until(async () => (await processes()).size === 0);
                assert.deepEqual(await pools(), original);
            } finally { await cleanup(); }
        });

        await check('memory.pools.global-admission', ['per-pool pressure leaves global reservation and reclaim decisions independent'], async () => {
            const [low] = await pools();
            const offset = guestLog().length;
            const log = () => guestLog().slice(offset);
            try {
                // Leave enough of a gap that process startup keeps the simulated low pressure.
                await setPools(low.free + 1, low.free + 1, 0, 0, '2 0');
                const pid = await launch('low', 'RECLAIM LOW READY');
                assert.deepEqual(await processes(), new Map([[pid, 0]]), 'strict process can reserve without populating');
                assert.match(log(), /MEM watermark low from=0x00000000 to=0x00000002/i);
                assert.doesNotMatch(log(), /MEM watermark from=|MEM reclaim|MEM purge/);
                await signal();
                await until(() => log().includes('RECLAIM LOW PASS'));
                assert.doesNotMatch(log(), /MEM reclaim|MEM purge/);
            } finally { await cleanup(); }
        });
    });

    await boot(source, 'pools-low-only', 1024, async context => {
        const {check, until, guestLog} = context;
        const {pools, setPools, launch, reset, cleanup, signal, processes} = poolTools(context);
        await check('memory.pools.single-pool-fallback', ['low-only fallback works at Low, denies at Critical and recovers'], async () => {
            const original = await pools();
            const emptyHigh = {free:0, published:0, watermark:'Normal'};
            assert.deepEqual(original[1], emptyHigh);
            const offset = guestLog().length;
            try {
                await launch('pool-populate', 'POOL POPULATE READY');
                const [low] = await pools();
                assert(low.free > 0);
                await setPools(low.free, low.free, 1, 1, '1 0');
                await signal();
                await until(() => guestLog().slice(offset).includes('POOL POPULATE DONE'));
                assert.deepEqual(await pools(), [{...low, free:low.free - 1, watermark:'Critical'}, emptyHigh]);
                await reset();
                await until(() => guestLog().slice(offset).includes('POOL POPULATE UNMAPPED'));
                await until(async () => (await processes()).size === 0);
                assert.deepEqual(await pools(), original);

                const deniedOffset = guestLog().length;
                await launch('pool-denied', 'POOL POPULATE READY');
                const [before] = await pools();
                assert(before.free > 0);
                await setPools(before.free + 1, before.free + 1, 1, 1, '2 0');
                await signal();
                await until(() => guestLog().slice(deniedOffset).includes('POOL POPULATE DENIED'));
                assert.deepEqual(await pools(), [{...before, watermark:'Critical'}, emptyHigh]);
                await reset();
                await until(() => guestLog().slice(deniedOffset).includes('POOL POPULATE DONE'));
                assert.deepEqual(await pools(), [{...before, free:before.free - 1}, emptyHigh]);
            } finally { await cleanup(); }
            assert.deepEqual(await pools(), original);
        });
    });
}

const [mode, source] = process.argv.slice(2);
async function vmaContracts(source) {
    await boot(source, 'vma', vm.memoryMiB, async ({check, channel, command, free, baseline, guestLog, probe}) => {
        await check('memory.vma.image-rollback', ['image VMA allocation rollback and relaunch'], async () => {
            const session = new RunSession(channel);
            await session.hello();
            const offset = guestLog().length;
            await assert.rejects(session.start('/boot/memv2.snx', ['oom-vma-image']), e => e.guestStatus === -2);
            assert.match(guestLog().slice(offset), /MM PROBE injected stage=0x00000005/i);
            assert.equal(free(await command('mem')), baseline);
            assert.doesNotMatch(await command('ps'), /memv2/i);
            await probe('run -s /boot/memv2.snx wide', /Launch VMA coverage passed[\s\S]*SNX V2 PASS/);
        });
        await check('memory.vma.stack-rollback', ['worker stack VMA allocation rollback and slot reuse'], () =>
            probe('run -s /boot/memv2.snx oom-vma-stack', /MM PROBE injected stage=0x00000005[\s\S]*VMA STACK RETRY PASS/i));
    });
}
async function protectionContracts(source) {
    await boot(source, 'mprotect', vm.memoryMiB, async ({channel, check, guestLog, command, baseline, free, until}) => {
        const session = new RunSession(channel);
        await session.hello();
        for (const item of memoryCases.mprotect) {
            const modeName = item.id.split('.').pop();
            await check(item.id, [item.name], async () => {
                const offset = guestLog().length;
                const result = await session.run('/boot/reclaim.snx', ['mp-' + modeName]);
                const log = guestLog().slice(offset);
                assert.doesNotMatch(log, /SELFTEST FAIL|KERNEL PANIC/);
                if (['cold', 'hot', 'retry-read', 'retry-write'].includes(modeName)) {
                    assert.equal(result.reason, 2);
                    assert.equal(result.vector, 14);
                    if (modeName === 'retry-write') {
                        assert.match(log, /injected stage=0x00000004[\s\S]*MP DOWNGRADED[\s\S]*USER fault contained/);
                    } else if (modeName === 'retry-read') {
                        assert.match(log, /injected stage=0x00000004[\s\S]*MP RETRIED READ/);
                    } else { assert.match(log, /MP EXPECT WRITE FAULT/); }
                } else {
                    assert.equal(result.reason, 1);
                    assert.equal(result.code, 0);
                }
                await until(async () => free(await command('mem')) === baseline);
            });
        }
    });
}
async function anonymousRangeContracts(source) {
    await boot(source, mode, vm.memoryMiB, async ({channel, check, guestLog, command, baseline, free, until}) => {
        const session = new RunSession(channel);
        await session.hello();
        for (const item of memoryCases[mode]) {
            await check(item.id, [item.name], async () => {
                const offset = guestLog().length;
                const name = item.id.split('.').pop();
                const result = await session.run('/boot/reclaim.snx', [mode + '-' + name]);
                const log = guestLog().slice(offset);
                assert.doesNotMatch(log, /SELFTEST FAIL|KERNEL PANIC/);
                if (name === 'oom') { assert.match(log, /injected stage=0x00000006/); }
                assert.equal(result.reason, 1);
                assert.equal(result.code, 0);
                await until(async () => free(await command('mem')) === baseline);
            });
        }
    });
}
const suites = {e1:anonymousRangeContracts, e2:anonymousRangeContracts, mprotect:protectionContracts, images:imageContracts, anonymous:anonymousContracts, heap:heapContracts, reclaim:reclaimContracts, purgeable:purgeableContracts, pools:poolContracts, vma:vmaContracts};
if (!Object.hasOwn(suites, mode) || process.argv.length !== 4) {
    console.error('Usage: node test-memory.js images|anonymous|heap|reclaim|purgeable|pools|vma|mprotect|e1|e2 <ShellBridge build directory>|--list');
    process.exitCode = 2;
} else if (source === '--list') {
    console.log(mode + ': ' + memoryCases[mode].length + ' cases');
    for (const item of memoryCases[mode]) console.log(item.id + '  ' + item.name);
} else {
    suites[mode](path.resolve(source))
        .then(() => {
            assertCaseIds(executed, memoryCases[mode]);
            fs.writeFileSync(path.join(path.resolve(source), 'memory-' + mode + '-report.json'),
                JSON.stringify({result:'PASS', cases:executed}, null, 2));
            console.log('PASS memory ' + mode + ': ' + executed.length + ' unique cases');
        }).catch(error => { console.error(error); process.exitCode = 1; });
}
