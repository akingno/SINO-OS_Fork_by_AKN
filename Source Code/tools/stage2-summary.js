const suite = require('../tests/host/stage2-summary');
if (require.main === module) suite.cli();
module.exports = suite;
