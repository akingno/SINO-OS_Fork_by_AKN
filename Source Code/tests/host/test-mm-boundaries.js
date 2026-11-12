const fs = require('node:fs'), path = require('node:path'), assert = require('node:assert/strict');
const {execFileSync} = require('node:child_process');
const root = path.resolve(__dirname, '../..');
const {tool} = require(path.join(root, 'tools/host-tools'));
const names = ['align-overflow','usable-inward','reserved-outward','e820-wide-and-invalid',
  'reserved-overlap-priority','uaccess-address-space','uaccess-permissions-and-bounds',
  'validation-before-allocation','partial-population-oom','string-and-copy-roundtrip'];
if (process.argv.length === 3 && process.argv[2] === '--list') {
  console.log(names.map(n => 'mm.boundaries.' + n).join('\n') + '\n10 cases');
} else {
  assert.equal(process.argv.length, 2, 'Only --list is accepted');
  const runs = path.join(root, '../Build Output/dev/runs');
  fs.mkdirSync(runs, {recursive:true});
  const directory = fs.mkdtempSync(path.join(runs, 'mm-boundaries-'));
  const exe = path.join(directory, 'mm-boundaries.exe');
  const report = {result:'FAIL', cases:[]};
  try {
    const compile = execFileSync(tool('gcc'), ['-fno-builtin','-Wno-int-to-pointer-cast','-Wno-pointer-to-int-cast','-std=c11','-O2','-Wall','-Wextra','-Werror',
      path.join(__dirname,'test-mm-boundaries.c'),path.join(root,'src32/mm/uaccess.c'),'-o',exe], {encoding:'utf8',windowsHide:true,timeout:30000,stdio:['ignore','pipe','pipe']});
    fs.writeFileSync(path.join(directory,'build.log'),compile);
    const output = execFileSync(exe, [], {encoding:'utf8',windowsHide:true,timeout:10000});
    fs.writeFileSync(path.join(directory,'output.log'),output);
    const ids = output.trim().split(/\r?\n/).map(line => {assert(line.startsWith('PASS '));return line.slice(5);});
    assert.deepEqual(ids, names.map(n => 'mm.boundaries.' + n));
    report.cases = ids.map(id => ({id,result:'PASS'}));report.result = 'PASS';
    console.log('PASS mm boundaries: ' + ids.length + ' cases');
  } catch (e) {report.error=e.stack;fs.writeFileSync(path.join(directory,'failure.log'),String(e.stderr || e.stack));console.error('FAIL mm boundaries; see failure.log');process.exitCode=1;}
  finally {fs.writeFileSync(path.join(directory,'mm-boundaries-report.json'),JSON.stringify(report,null,2));console.log('Report: '+directory);}
}
