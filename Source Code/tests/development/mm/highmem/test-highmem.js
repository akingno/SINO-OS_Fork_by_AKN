/* High-page allocation with 2 GiB RAM. Verifies 64-bit E820 parsing,
   partitioned allocator and first-high-allocation log. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {run} = require('../../../../tools/shell-test');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

const source = path.resolve(process.argv[2] || '');
if (!source || process.argv.length !== 3) {
    console.error('Usage: node test-highmem.js <ShellBridge build directory>');
    process.exitCode = 2;
} else {
    (async () => {
        const directory = fs.mkdtempSync(path.join(source, 'highmem-'));
        const iso = path.join(directory, 'SINO.iso');
        fs.copyFileSync(path.join(source, 'SINO.iso'), iso, fs.constants.COPYFILE_EXCL);
        const readJSON = f => JSON.parse(fs.readFileSync(f, 'utf8').replace(/^﻿/, ''));
        const build = {...readJSON(path.join(source, 'build.json')), iso};
        fs.writeFileSync(path.join(directory, 'build.json'), JSON.stringify(build));
        fs.writeFileSync(path.join(directory, 'shell-job.json'), JSON.stringify({timeoutMs: 5000}));
        const executed = [];

        const code = await run(directory, {memoryMiB: 2048, execute: async context => {
            const {channel, report, guestLog} = context;
            let seq = 0;
            async function command(text, status = 0) {
                const reply = await channel.request(++seq, text, 5000);
                assert.equal(reply.status, status, text + ': ' + reply.output.toString());
                return reply.output.toString();
            }
            function free(out) {
                const match = out.match(/\/ (\d+) pages/);
                assert(match, 'page count');
                return Number(match[1]);
            }
            async function check(id, fn) {
                const item = {id, name: id, result: 'FAIL'};
                executed.push(item);
                report.cases.push(item);
                try { await fn(); item.result = 'PASS'; console.log('PASS ' + id); }
                catch (e) { item.error = e.message; throw e; }
            }

            await check('highmem.nr-pages-reflects-2-gib', async () => {
                const log = guestLog();
                const match = log.match(/nr_pages=(0x[0-9a-f]+)/i);
                assert(match, 'nr_pages not logged');
                const pages = Number(match[1]);
                assert(pages > 0x70000, 'nr_pages too small for 2 GiB: 0x' + pages.toString(16));
                assert(pages <= 0x80000, 'nr_pages exceeds 2 GiB: 0x' + pages.toString(16));
            });

            await check('highmem.direct-map-capped-at-user-base', async () => {
                const log = guestLog();
                assert.match(log, /Direct mapping: \[0, 0x40000000\)/,
                    'direct_map_end should be USER_BASE for 2 GiB');
            });

            await check('highmem.extended-pages-published', async () => {
                const log = guestLog();
                const match = log.match(/PAGE extended pages=(0x[0-9a-f]+)/i);
                assert(match, 'extended pages not logged');
                const extended = Number(match[1]);
                assert(extended > 0x30000, 'too few extended pages: 0x' + extended.toString(16));
            });

            await check('highmem.first-high-allocation-above-1-gib', async () => {
                const out = await command('run -s /boot/poltest.snx paging-a');
                const log = guestLog();
                const match = log.match(/PAGE first high allocation=(0x[0-9a-f]+)/i);
                assert(match, 'first high allocation not logged');
                const addr = Number(match[1]);
                assert(addr >= 0x40000000, 'high allocation below 1 GiB: 0x' + addr.toString(16));
            });

            await check('highmem.mem-reports-2-gib-scale', async () => {
                const out = await command('mem');
                const match = out.match(/Mapped usable range: (\d+) KiB/);
                assert(match, 'mem output missing usable range');
                const kib = Number(match[1]);
                assert(kib > 1800000, 'total too small for 2 GiB: ' + kib);
                assert(kib <= 2097152, 'total exceeds 2 GiB: ' + kib);
            });
        }});

        if (code) throw Error('highmem test failed: ' + directory);
        const report = {result: 'PASS', cases: executed};
        fs.writeFileSync(path.join(directory, 'highmem-report.json'),
            JSON.stringify(report, null, 2));
        console.log('PASS highmem: ' + executed.length + ' cases');
    })().catch(error => { console.error(error); process.exitCode = 1; });
}
