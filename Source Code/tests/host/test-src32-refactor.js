/* Pre-refactor behavior tests. --baseline reports known defects separately;
 * default strict mode fails them. No guest source is edited by this runner. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {execFileSync} = require('node:child_process');
const host = require('../../tools/host-tools');

const root = path.resolve(__dirname, '../..');
const baseline = process.argv.includes('--baseline');
const unknown = process.argv.slice(2).filter(arg => arg !== '--baseline');
assert.equal(unknown.length, 0, `Unknown arguments: ${unknown.join(' ')}`);
const run = path.resolve(process.env.SINO_REFACTOR_REPORT_DIR || path.join(root,
  '../Build Output/dev/runs', `src32-contract-${new Date().toISOString().replace(/[:.]/g, '-')}`));
fs.mkdirSync(run, {recursive: true});
const sha256 = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const known = new Set(); // Historical baseline mode can no longer excuse repaired defects.
const report = {baseline, runtimeModified: false, checks: [], sourceHashes: {}, testHashes: {},
  started: new Date().toISOString()};
const generatorPath = 'tools/build-locale.py';
report.testHashes[generatorPath] = sha256(path.join(root, generatorPath));
for (const name of ['test-src32-refactor.js', 'test-src32-storage.cpp', 'test-locale.cpp', 'test-klog.cpp']) {
  report.testHashes[`tests/host/${name}`] = sha256(path.join(__dirname, name));
}
function hashSources(folder) {
  for (const name of fs.readdirSync(path.join(root, folder)).sort()) {
    const relative = `${folder}/${name}`;
    const file = path.join(root, relative);
    if (fs.statSync(file).isDirectory()) hashSources(relative);
    else report.sourceHashes[relative] = sha256(file);
  }
}
for (const folder of ['src32', 'user', 'tests/guest']) hashSources(folder);
function check(id, ok, detail) {
  assert(!report.checks.some(c => c.id === id), `Duplicate check ${id}`);
  const result = ok ? 'PASS' : baseline && known.has(id) ? 'KNOWN-FAIL' : 'FAIL';
  report.checks.push({id, result, ...(detail === undefined ? {} : {detail})});
  console.log(`${result} ${id}`);
}
try {
  // Only replace hardware reads. All storage decisions remain original C.
  let source = fs.readFileSync(path.join(root, 'src32/storage.cpp'), 'utf8');
  const drive = /\*\(\s*volatile\s+u8\s*\*\s*\)\s*0x4f0c\b/g;
  const signature = /\*\(\s*volatile\s+u32\s*\*\s*\)\s*0x4f10\b/g;
  assert(source.match(drive) && source.match(signature), 'Review the BIOS test adapter for this source version');
  source = source.replace(drive, 'storage_test_boot_drive')
    .replace(signature, 'storage_test_boot_signature');
  fs.writeFileSync(path.join(run, 'storage-under-test.inc'), source);
  const exe = path.join(run, 'storage-contract.exe');
  execFileSync(host.tool('g++'), [path.join(__dirname, 'test-src32-storage.cpp'),
    '-I', run, '-I', path.join(root, 'src32'), '-std=c++17', '-O2',
    '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', path.join(root, 'src32/locale.cpp'), '-o', exe], {windowsHide: true, timeout: 30000});
  const output = execFileSync(exe, [], {encoding: 'utf8', windowsHide: true, timeout: 30000});
  fs.writeFileSync(path.join(run, 'storage-contract.txt'), output);
  for (const line of output.trim().split(/\r?\n/)) {
    const match = line.match(/^CHECK\t([\w.-]+)\t([01])$/);
    assert(match, `Unexpected host-test output: ${line}`);
    check(match[1], match[2] === '1');
  }
  assert(report.checks.length >= 20, 'Host-test output is incomplete');

  const klogExe=path.join(run,'klog-test.exe');
  execFileSync(host.tool('g++'),[path.join(__dirname,'test-klog.cpp'),'-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-o',klogExe],{windowsHide:true,timeout:30000});
  for(const line of execFileSync(klogExe,[],{encoding:'utf8',windowsHide:true,timeout:30000}).trim().split(/\r?\n/)) {
    const match=line.match(/^CHECK\t([\w.-]+)\t([01])$/);assert(match,line);check(match[1],match[2]==='1');
  }
  const fixture = path.join(run, 'locale-roundtrip');
  for (const folder of ['tools', 'src32']) fs.mkdirSync(path.join(fixture, folder), {recursive:true});
  fs.cpSync(path.join(root, 'resources'), path.join(fixture, 'resources'), {recursive:true});
  const generator = path.join(fixture, 'tools/build-locale.py');
  fs.copyFileSync(path.join(root, generatorPath), generator);
  execFileSync(host.python(), [generator], {windowsHide:true, timeout:30000});
  check('locale.generator-roundtrip', ['locale_keys.h', 'locale_data.inc'].every(name =>
    fs.readFileSync(path.join(root, 'src32', name)).equals(fs.readFileSync(path.join(fixture, 'src32', name)))));
  const lang = path.join(fixture, 'resources/locale/en-US.json');
  const original = fs.readFileSync(lang, 'utf8');
  const rejected = text => {
    fs.writeFileSync(lang, text);
    try {execFileSync(host.python(), [generator], {windowsHide:true, stdio:'pipe', timeout:30000}); return false;}
    catch (e) {return e.status !== undefined && e.status !== 0;}
    finally {fs.writeFileSync(lang, original);}
  };
  const missing = JSON.parse(original); delete missing[Object.keys(missing)[0]];
  check('locale.reject-missing', rejected(JSON.stringify(missing)));
  const existingKey = Object.keys(JSON.parse(original))[0];
  check('locale.reject-duplicate', rejected(original.replace('{', '{'+JSON.stringify(existingKey)+':"duplicate",')));
  const broken = JSON.parse(original); broken.VFS_ERROR_NODE_LIMIT = '{wrong}';
  check('locale.reject-parameters', rejected(JSON.stringify(broken)));
  const localeExe=path.join(run,'locale-test.exe');
  execFileSync(host.tool('g++'), [path.join(__dirname,'test-locale.cpp'),path.join(root,'src32/locale.cpp'),
    '-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-o',localeExe], {windowsHide:true,timeout:30000});
  for (const line of execFileSync(localeExe,[],{encoding:'utf8',windowsHide:true,timeout:30000}).trim().split(/\r?\n/)) {
    const match=line.match(/^CHECK\t([\w.-]+)\t([01])$/); assert(match,line); check(match[1],match[2]==='1');
  }
} catch (error) {
  console.error(error.stack || String(error));
  check('runner.execution', false, error.stack || String(error));
} finally {
  const changed = Object.entries(report.sourceHashes).filter(([name, hash]) => sha256(path.join(root, name)) !== hash);
  report.runtimeModified = changed.length > 0;
  check('runner.runtime-source-unchanged', !changed.length, changed.map(([name]) => name));
  report.finished = new Date().toISOString();
  report.summary = Object.fromEntries(['PASS', 'KNOWN-FAIL', 'FAIL'].map(result =>
    [result, report.checks.filter(c => c.result === result).length]));
  report.goalSatisfied = !report.checks.some(c => c.result !== 'PASS');
  fs.writeFileSync(path.join(run, 'src32-refactor-report.json'), JSON.stringify(report, null, 2) + '\n');
  console.log(JSON.stringify(report.summary));
  console.log(`Report: ${path.join(run, 'src32-refactor-report.json')}`);
  process.exitCode = report.summary.FAIL ? 1 : 0;
}
