const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const host = require('../../../tools/host-tools');
const hostRoot = path.resolve(__dirname, '..');
async function infrastructure({directory, command, readJSON}) {
    const folder = path.join(directory, 'infrastructure');
    fs.mkdirSync(folder);
    await command(process.execPath, [path.join(hostRoot, 'test-src32-refactor.js')],
        path.join(folder, 'output.txt'), {SINO_REFACTOR_REPORT_DIR:folder});
    const result = readJSON(path.join(folder, 'src32-refactor-report.json'));
    assert.equal(result.goalSatisfied, true);
    assert(result.checks.length >= 41 && result.checks.every(c => c.result === 'PASS'));
    const model = path.join(folder, 'terminal-model.exe');
    await command(host.tool('g++'), [path.join(hostRoot, 'test-terminal-model.cpp'),
        '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-missing-field-initializers', '-o', model],
        path.join(folder, 'terminal-model-build.txt'));
    const modelOutput = path.join(folder, 'terminal-model-output.txt');
    await command(model, [], modelOutput);
    assert.equal(fs.readFileSync(modelOutput, 'utf8').trim(), 'TERMINAL MODEL PASS');
    const parser=path.join(folder,'gsh-parser.exe');
    await command(host.tool('g++'),[path.join(hostRoot,'test-gsh.cpp'),'-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-o',parser],path.join(folder,'gsh-build.txt'));
    const parserOutput=path.join(folder,'gsh-output.txt');
    await command(parser,[],parserOutput);
    assert.equal(fs.readFileSync(parserOutput,'utf8').trim(),'GSH PARSER PASS');
    const fileText=path.join(folder,'file-text.exe');
    await command(host.tool('g++'),[path.join(hostRoot,'test-file-text.cpp'),'-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-o',fileText],path.join(folder,'file-text-build.txt'));
    const fileTextOutput=path.join(folder,'file-text-output.txt');
    await command(fileText,[],fileTextOutput);
    assert.equal(fs.readFileSync(fileTextOutput,'utf8').trim(),'FILE TEXT PASS');
    const scan=path.join(folder,'text-scan.exe');
    await command(host.tool('g++'),[path.join(hostRoot,'test-text-scan.cpp'),'-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-o',scan],path.join(folder,'text-scan-build.txt'));
    const scanOutput=path.join(folder,'text-scan-output.txt');await command(scan,[],scanOutput);
    assert.equal(fs.readFileSync(scanOutput,'utf8').trim(),'TEXT SCAN PASS');
    const editor=path.join(folder,'line-edit.exe');
    await command(host.tool('g++'),[path.join(hostRoot,'test-line-edit.cpp'),'-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-o',editor],path.join(folder,'line-edit-build.txt'));
    const editOutput=path.join(folder,'line-edit-output.txt');await command(editor,[],editOutput);assert.equal(fs.readFileSync(editOutput,'utf8').trim(),'LINE EDIT PASS');
    for(const [name,marker] of [['line-model','LINE MODEL PASS']]){
      const exe=path.join(folder,name+'.exe');await command(host.tool('g++'),[path.join(hostRoot,'test-'+name+'.cpp'),'-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-missing-field-initializers','-o',exe],path.join(folder,name+'-build.txt'));
      const output=path.join(folder,name+'-output.txt');await command(exe,[],output);assert.equal(fs.readFileSync(output,'utf8').trim(),marker);
    }
    const caseIds = result.checks.map(item => item.id).concat(['model.terminal', 'model.gsh-parser',
        'model.file-text', 'model.text-scan', 'model.line-edit', 'model.line-model']);
    return {checks:caseIds.length, caseIds, modelOutput, parserOutput, fileTextOutput, scanOutput,
        report:path.join(folder, 'src32-refactor-report.json')};
}
async function rtc({directory, command}) {
    const exe = path.join(directory, 'rtc-test.exe');
    await command(host.tool('g++'), [path.join(hostRoot, 'test-rtc.cpp'), '-std=c++17',
        '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', '-o', exe], path.join(directory, 'rtc-build.txt'));
    const output = path.join(directory, 'rtc-output.txt');
    await command(exe, [], output);
    const match = fs.readFileSync(output, 'utf8').match(/RTC DRIVER PASS: (\d+) assertions/);
    assert(match && Number(match[1]) >= 40, 'actual RTC driver assertions');
    return {checks:1, caseIds:['rtc.driver'], assertions:Number(match[1]), output};
}
module.exports = {infrastructure, rtc};
