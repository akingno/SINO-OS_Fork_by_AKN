const fs = require('node:fs'), path = require('node:path');
const header = fs.readFileSync(path.join(__dirname, '../src32/boot-layout.h'), 'utf8');
const values = Object.fromEntries([...header.matchAll(/^#define (BOOT_\w+) (\d+)$/gm)].map(m => [m[1], Number(m[2])]));
module.exports = values;
