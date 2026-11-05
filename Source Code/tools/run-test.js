/* One real bounded CLI execution; reports actual typed status and raw bytes. */
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const {run}=require('./shell-test');
const {RunSession}=require('./run-channel');
const readJSON=file=>JSON.parse(fs.readFileSync(file,'utf8').replace(/^\uFEFF/,''));
async function main(directory) {
    const job=readJSON(path.join(directory,'run-job.json'));
    const report={result:'FAIL',stage:'boot',job,startedAt:new Date().toISOString()};
    const code=await run(directory,{execute:async context=>{
        const item={id:1,name:'bounded user program final status',result:'FAIL'};context.report.cases.push(item);
        report.stage='run';
        try {
            const session=new RunSession(context.channel);await session.hello();
            const result=await session.run(job.program,job.arguments,Buffer.from(job.inputBase64,'base64'),{timeoutTicks:job.timeoutTicks});
            fs.writeFileSync(path.join(directory,'stdout.out'),result.stdout);
            fs.writeFileSync(path.join(directory,'stderr.out'),result.stderr);
            report.actual={...result,stdout:undefined,stderr:undefined};
            report.stdoutFile=path.join(directory,'stdout.out');report.stderrFile=path.join(directory,'stderr.out');
            assert.equal(result.reason,1,'program did not exit normally (reason '+result.reason+')');
            assert.equal(result.incomplete,false,'incomplete output');
            assert.equal(result.code,job.expectedExitCode,'actual program exit code');
            item.result='PASS';report.result='PASS';
        } catch(error) {
            report.error=error.message;report.classification=error.classification || (report.actual?'guest-result':'launch-or-transport');
            item.error=error.message;throw error;
        }
    }});
    const transport=readJSON(path.join(directory,'shell-report.json'));
    if(code || !transport.vmStopped)report.result='FAIL';
    report.vmStopped=transport.vmStopped;report.transportError=transport.error;
    report.finishedAt=new Date().toISOString();
    fs.writeFileSync(path.join(directory,'cli-report.json'),JSON.stringify(report,null,2)+'\n');
    console.log(`${report.result}: ${path.join(directory,'cli-report.json')}`);
    if(report.actual)console.log(`Guest reason ${report.actual.reason}, exit ${report.actual.code}; stdout ${report.actual.stdoutBytes} bytes, stderr ${report.actual.stderrBytes} bytes.`);
    return report.result==='PASS'?0:1;
}
if(require.main===module)main(path.resolve(process.argv[2]||'')).then(code=>{process.exitCode=code;})
    .catch(error=>{console.error(error);process.exitCode=1;});
module.exports={main};
