async function run({check, native}) {
    await check('cold boot preserves published file bytes', () =>
        native('fstest', 'reboot', 'FILESYS REBOOT PASS'));
}
module.exports = {run};
