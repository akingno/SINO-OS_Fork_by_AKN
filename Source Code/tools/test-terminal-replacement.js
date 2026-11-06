const suite = require('../tests/host/test-terminal-replacement');
if (require.main === module) suite.cli();
module.exports = suite;
