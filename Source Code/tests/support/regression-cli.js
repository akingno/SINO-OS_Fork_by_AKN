const fs = require('node:fs');
const path = require('node:path');
async function existing(suite, args = process.argv.slice(2)) {
    try {
        const runner = require('../host/test-core');
        if (args.length === 1 && args[0] === '--list') { runner.printPlan(suite, true); return; }
        if (args.length !== 1) throw Error('Supply a ShellBridge build directory, or --list.');
        const source = path.resolve(args[0]);
        const build = JSON.parse(fs.readFileSync(path.join(source, 'build.json'), 'utf8'));
        const directory = fs.mkdtempSync(path.join(path.dirname(source), 'regression-check-'));
        const iso = path.join(directory, 'SINO.iso');
        fs.copyFileSync(build.iso, iso, fs.constants.COPYFILE_EXCL);
        fs.copyFileSync(path.join(source, 'kernel-normal.exe'), path.join(directory, 'kernel-normal.exe'));
        fs.writeFileSync(path.join(directory, 'build.json'), JSON.stringify({...build, iso}));
        process.exitCode = await runner.run(directory, suite);
    } catch (error) {console.error(error.message); process.exitCode = 1;}
}
module.exports = {existing};
