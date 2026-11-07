async function run({check, native}) {
    await check('cold boot preserves process probe binary bytes', () =>
        native('probe', '/contracts-read', 'CONTRACT PERSISTENCE PASS'));
}
module.exports = {run};
