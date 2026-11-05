/* Stage 2 compatibility gate: text contracts, real guests and private disks. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {spawn} = require('node:child_process');
const host = require('./host-tools');
const {sources} = require('./dev-run');
const shell = require('./shell-test');
const guest = require('./stage2-guest');
const {volume} = require('./test-volume');
const sourceRoot = path.resolve(__dirname, '..');
const repoRoot = path.dirname(sourceRoot);
const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const readJSON = file => JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));
const writeJSON = (file, value) => fs.writeFileSync(file, JSON.stringify(value, null, 2) + '\n');

async function command(executable, args, output, env = {}) {
    const fd = fs.openSync(output, 'w');
    let child;
    try {
        child = spawn(executable, args, {cwd:sourceRoot, windowsHide:true,
            env:{...process.env, ...env}, stdio:['ignore', fd, fd]});
    } finally { fs.closeSync(fd); }
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => child.kill(), 120000);
        child.once('error', error => {clearTimeout(timer); reject(error);});
        child.once('exit', (code, signal) => {
            clearTimeout(timer);
            if (code === 0) resolve();
            else reject(Error(`Command failed (${code ?? signal}): ${output}`));
        });
    });
}

async function run(directory) {
    const relative = path.relative(path.join(repoRoot, 'Build Output/dev/runs'), directory);
    assert(relative && !relative.startsWith('..') && !path.isAbsolute(relative) &&
        /^stage2-[^/\\]+$/.test(relative), 'Use the repository stage2-test.ps1 entry.');
    const reportFile = path.join(directory, 'stage2-report.json');
    const names = ['infrastructure', 'rtc-driver', 'shell-transport', 'shell-commands',
        'native-contracts', 'managed-cli', 'cold-reboot', 'runner-rejects-failure'];
    const report = {version:1, result:'FAIL', startedAt:new Date().toISOString(),
        scope:'text compatibility baseline; not the complete GUI/system regression',
        excluded:['Paint', 'GUI text editor workflows', 'GUI scaling/dragging/pixel checks',
            'visual designer interactions', 'physical OS installation', 'external networking'],
        suites:names.map(name => ({name, result:'NOT-RUN'})), testHashes:{}};
    writeJSON(reportFile, report);
    let build;
    async function phase(name, action) {
        const item = report.suites.find(s => s.name === name);
        item.result = 'RUNNING';
        item.startedAt = new Date().toISOString();
        writeJSON(reportFile, report);
        console.log('RUN ' + name);
        try {
            Object.assign(item, await action());
            assert(item.checks > 0, name + ': no checks executed');
            item.result = 'PASS';
            console.log(`PASS ${name}: ${item.checks} checks`);
        } catch (error) { item.result = 'FAIL'; item.error = error.message; throw error; }
        finally { item.finishedAt = new Date().toISOString(); writeJSON(reportFile, report); }
    }
    async function shellSuite(name, job, execute, seedDisk) {
        const folder = path.join(directory, name);
        fs.mkdirSync(folder);
        const iso = path.join(folder, 'SINO.iso');
        fs.copyFileSync(build.iso, iso, fs.constants.COPYFILE_EXCL);
        writeJSON(path.join(folder, 'build.json'), {...build, iso});
        writeJSON(path.join(folder, 'shell-job.json'), {selfTest:true, timeoutMs:15000, ...job});
        const code = await shell.run(folder, {execute, seedDisk});
        const result = readJSON(path.join(folder, 'shell-report.json'));
        assert.equal(result.vmStopped, true, name + ': test VM did not stop');
        return {folder, code, result};
    }
    function passed(run, minimum) {
        assert.equal(run.code, 0, run.folder + ': runner failed');
        assert.equal(run.result.result, 'PASS');
        assert(run.result.cases.length >= minimum, run.folder + ': incomplete suite');
        assert(run.result.cases.every(c => c.result === 'PASS'));
        return {checks:run.result.cases.length, report:path.join(run.folder, 'shell-report.json')};
    }
    try {
        build = readJSON(path.join(directory, 'build.json'));
        assert.equal(sources().sha256, build.sourceSha256, 'source identity before tests');
        assert.equal(hash(build.iso), build.isoSha256, 'ISO identity before tests');
        const symbols = path.join(directory, 'kernel-normal.exe');
        const extracted = path.join(directory, 'kernel-symbol-check.bin');
        await command(host.tool('objcopy'), ['-O', 'binary', symbols, extracted],
            path.join(directory, 'symbol-check.log'));
        assert.equal(hash(extracted), build.kernelSha256, 'symbol snapshot matches built flat kernel');
        build.symbolsSha256 = hash(symbols);
        report.build = build;
        for (const file of ['stage2-test.ps1', 'test.ps1', 'run.ps1', 'Source Code/tools/stage2-test.js',
            'Source Code/tools/stage2-guest.js', 'Source Code/tools/shell-test.js']) {
            report.testHashes[file] = hash(path.join(repoRoot, file));
        }
        await phase('infrastructure', async () => {
            const folder = path.join(directory, 'infrastructure');
            fs.mkdirSync(folder);
            await command(process.execPath, [path.join(__dirname, 'test-src32-refactor.js')],
                path.join(folder, 'output.txt'), {SINO_REFACTOR_REPORT_DIR:folder});
            const result = readJSON(path.join(folder, 'src32-refactor-report.json'));
            assert.equal(result.goalSatisfied, true);
            assert(result.checks.length >= 43 && result.checks.every(c => c.result === 'PASS'));
            const model = path.join(folder, 'terminal-model.exe');
            await command(host.tool('gcc'), [path.join(__dirname, 'test-terminal-model.c'),
                '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-o', model],
                path.join(folder, 'terminal-model-build.txt'));
            const modelOutput = path.join(folder, 'terminal-model-output.txt');
            await command(model, [], modelOutput);
            assert.equal(fs.readFileSync(modelOutput, 'utf8').trim(), 'TERMINAL MODEL PASS');
            const parser=path.join(folder,'gsh-parser.exe');
            await command(host.tool('gcc'),[path.join(__dirname,'test-gsh.c'),'-std=c11','-O2','-Wall','-Wextra','-Werror','-o',parser],path.join(folder,'gsh-build.txt'));
            const parserOutput=path.join(folder,'gsh-output.txt');
            await command(parser,[],parserOutput);
            assert.equal(fs.readFileSync(parserOutput,'utf8').trim(),'GSH PARSER PASS');
            const fileText=path.join(folder,'file-text.exe');
            await command(host.tool('gcc'),[path.join(__dirname,'test-file-text.c'),'-std=c11','-O2','-Wall','-Wextra','-Werror','-o',fileText],path.join(folder,'file-text-build.txt'));
            const fileTextOutput=path.join(folder,'file-text-output.txt');
            await command(fileText,[],fileTextOutput);
            assert.equal(fs.readFileSync(fileTextOutput,'utf8').trim(),'FILE TEXT PASS');
            const scan=path.join(folder,'text-scan.exe');
            await command(host.tool('gcc'),[path.join(__dirname,'test-text-scan.c'),'-std=c11','-O2','-Wall','-Wextra','-Werror','-o',scan],path.join(folder,'text-scan-build.txt'));
            const scanOutput=path.join(folder,'text-scan-output.txt');await command(scan,[],scanOutput);
            assert.equal(fs.readFileSync(scanOutput,'utf8').trim(),'TEXT SCAN PASS');
            return {checks:result.checks.length + 4, modelOutput, parserOutput, fileTextOutput, scanOutput,
                report:path.join(folder, 'src32-refactor-report.json')};
        });
        await phase('rtc-driver', async () => {
            const exe = path.join(directory, 'rtc-test.exe');
            await command(host.tool('gcc'), [path.join(__dirname, 'test-rtc.c'), '-std=c11',
                '-O2', '-Wall', '-Wextra', '-Werror', '-o', exe], path.join(directory, 'rtc-build.txt'));
            const output = path.join(directory, 'rtc-output.txt');
            await command(exe, [], output);
            const match = fs.readFileSync(output, 'utf8').match(/RTC DRIVER PASS: (\d+) assertions/);
            assert(match && Number(match[1]) >= 40, 'actual RTC driver assertions');
            return {checks:Number(match[1]), output};
        });
        await phase('shell-transport', async () => passed(await shellSuite('transport', {suite:'transport'}), 25));
        await phase('shell-commands', async () => passed(await shellSuite('commands', {suite:'commands'}), 112));
        await phase('native-contracts', async () => {
            const run = await shellSuite('native', {suite:'stage2'}, c => guest.run(c));
            const result = passed(run, 30);
            const disk = path.join(run.folder, 'shell-data.img');
            const records = volume(disk).current.records;
            const binary = Buffer.from(Array.from({length:257}, (_, i) => (i * 37 + 11) & 255));
            assert(records.get('/home/user/stage2-persist.bin')?.data.equals(binary), 'CRC-valid binary snapshot');
            const saved = Buffer.from(Array.from({length:257}, (_, i) => (i * 19 + 7) & 255));
            assert(records.get('/home/user/stage2-file-save.bin')?.data.equals(saved), 'Stage D CRC-valid published snapshot');
            assert.equal(records.get('/home/user/stage2-marker.txt')?.data.toString(), 'stage2 durable marker\n');
            assert(records.has('/home/user/.sino/rollback/journal'), 'interrupted install journal committed');
            report.privateSeed = {path:disk, sha256:hash(disk)};
            return {...result, snapshotChecks:4, guestAssertions:run.result.cases
                .reduce((sum, c) => sum + (c.guestAssertions || 0), 0)};
        });
        await phase('managed-cli', async () => passed(await shellSuite('managed-cli',
            {suite:'stage2'}, c => guest.run(c,'cli')),12));
        await phase('cold-reboot', async () => {
            const seed = report.privateSeed;
            assert.equal(hash(seed.path), seed.sha256, 'private disk unchanged before clone');
            const run = await shellSuite('reboot', {suite:'stage2'}, c => guest.run(c, 'reboot'), seed.path);
            assert.equal(hash(seed.path), seed.sha256, 'cold boot does not modify the original private disk');
            return passed(run, 4);
        });
        await phase('runner-rejects-failure', async () => {
            const run = await shellSuite('intentional-failure', {suite:'stage2'}, c => guest.run(c, 'reject'));
            assert.equal(run.code, 1, 'intentional guest failure must return nonzero');
            assert.equal(run.result.result, 'FAIL');
            assert.equal(run.result.stage, 'commands', 'failure must occur after verified boot');
            assert.equal(run.result.cases.length, 1);
            assert.equal(run.result.cases[0].result, 'FAIL');
            assert.match(run.result.error, /USER PROBE FAIL: intentional Stage 2 runner failure/);
            return {checks:1, expectedFailure:true, report:path.join(run.folder, 'shell-report.json')};
        });
        report.result = 'PASS';
    } catch (error) { report.error = error.stack || String(error); console.error(report.error); }
    finally {
        try {
            if (build && (sources().sha256 !== build.sourceSha256 || hash(build.iso) !== build.isoSha256))
                throw Error('Source or ISO changed during Stage 2 verification.');
            for (const [name, expected] of Object.entries(report.testHashes))
                assert.equal(hash(path.join(repoRoot, name)), expected, 'test changed during run: ' + name);
        } catch (error) { report.result = 'FAIL'; report.identityError = error.message; }
        report.finishedAt = new Date().toISOString();
        report.summary = {passed:report.suites.filter(s => s.result === 'PASS').length,
            failed:report.suites.filter(s => s.result === 'FAIL').length,
            notRun:report.suites.filter(s => s.result === 'NOT-RUN').length,
            checks:report.suites.reduce((sum, s) => sum + (s.result === 'PASS' ? s.checks : 0), 0)};
        if (report.summary.passed !== names.length) report.result = 'FAIL';
        writeJSON(reportFile, report);
        console.log(`${report.result}: Stage 2 ${report.summary.passed}/${names.length} suites; ${report.summary.checks} checks`);
        console.log('Report: ' + reportFile);
    }
    return report.result === 'PASS' ? 0 : 1;
}

if (require.main === module) {
    if (process.argv.length !== 3) { console.error('Use stage2-test.ps1 from the repository root.'); process.exitCode = 1; }
    else run(path.resolve(process.argv[2])).then(code => {process.exitCode = code;})
        .catch(error => {console.error(error); process.exitCode = 1;});
}
module.exports = {run};
