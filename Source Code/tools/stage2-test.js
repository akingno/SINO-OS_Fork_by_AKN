const suite = require('../tests/host/stage2-test');
if (require.main === module) suite.cli();
module.exports = suite;
