const assert=require('node:assert/strict');
const {RunSession,words}=require('../../tools/run-channel');
const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function run(context,helpers) {
    const {channel,guestLog}=context;
    const {check,command,resources,value,until}=helpers;
    const baseline=await resources();
    let session=new RunSession(channel);
    await check('v2 handshake advertises bounded limits',async()=>{
        const limits=await session.hello();assert.deepEqual(limits,[40,1,2,16,1024,4096,4096,4096,65536,60000]);
    });
    await check('v2 preserves argv boundaries, empty values and UTF-8',async()=>{
        const args=['args','','a b','你好','one\ntwo'];
        const r=await session.run('/boot/clitest.snx',args);
        assert.equal(r.reason,1);assert.equal(r.code,0);assert(!r.incomplete);
        let offset=4;assert.equal(r.stdout.readUInt32LE(),args.length);
        for(const s of args){const bytes=Buffer.from(s);assert.equal(r.stdout.readUInt32LE(offset),bytes.length);offset+=4;
            assert(r.stdout.subarray(offset,offset+bytes.length).equals(bytes));offset+=bytes.length;}
        assert.equal(offset,r.stdout.length);
    });
    await check('v2 exact input/output capacity and independent binary stderr',async()=>{
        const bytes=Buffer.from(Array.from({length:4096},(_,i)=>(i*37+11)&255));
        const r=await session.run('/boot/clitest.snx',['echo'],bytes);
        assert.equal(r.reason,1);assert.equal(r.code,0);assert(r.stdout.equals(bytes));
        assert(r.stderr.equals(Buffer.from([101,0,114,10])));assert.equal(r.inputBytes,4096);
    });
    await check('v2 maximum packed arguments fit exactly',async()=>{
        const args=['args',...Array(14).fill(''),'x'.repeat(1004)];
        const r=await session.run('/boot/clitest.snx',args,Buffer.alloc(4096));assert.equal(r.code,0);assert.equal(r.stdout.readUInt32LE(),16);
    });
    await check('v2 actual negative exit and fault output are distinct',async()=>{
        const normal=await session.run('/boot/clitest.snx',['exit']);assert.equal(normal.reason,1);assert.equal(normal.code,-17);
        const before=await value('process_faults');
        const fault=await session.run('/boot/clitest.snx',['fault']);assert.equal(fault.reason,2);assert.equal(fault.vector,6);
        assert.equal(fault.stdout.toString(),'before fault');assert(fault.incomplete);assert.equal(await value('process_faults'),before+1);
    });
    await check('v2 output and stderr overflows are explicit incomplete results',async()=>{
        for(const mode of ['overflow','overflow-err']){const r=await session.run('/boot/clitest.snx',[mode]);
            assert.equal(r.reason,5);assert(r.incomplete);assert.equal(mode==='overflow'?r.stdout.length:r.stderr.length,4096);}
    });
    await check('v2 guest deadline differs from host transport timeout',async()=>{
        const r=await session.run('/boot/clitest.snx',['spin'],Buffer.alloc(0),{timeoutTicks:20});
        assert.equal(r.reason,6);assert(r.incomplete);
    });
    await check('v2 two asynchronous runs, targeted cancellation and quota',async()=>{
        const first=await session.start('/boot/clitest.snx',['spin'],Buffer.alloc(0),0);
        const second=await session.start('/boot/clitest.snx',['spin'],Buffer.alloc(0),0);
        await assert.rejects(session.start('/boot/clitest.snx',['exit']),e=>e.guestStatus===-8);
        assert.equal(await session.poll(first),null);assert.equal(await command('echo responsive'),'responsive\n');
        await session.cancel(first);let r;await until(async()=>!!(r=await session.poll(first)),'cancel result');assert.equal(r.reason,3);
        assert.equal(await session.poll(second),null);await session.release(first);await session.release(second);
        await until(async()=>JSON.stringify(await resources())===JSON.stringify(baseline),'release reclaims runs');
    });
    await check('v2 malformed descriptors reject without retaining resources',async()=>{
        for(let i=0;i<5;i++){
            const b=RunSession.encodeStart('/boot/clitest.snx',['echo'],Buffer.alloc(0),500);
            if(i===0)b.writeUInt32LE(17,4);
            if(i===1)b.writeUInt32LE(1025,8);
            if(i===2)b.writeUInt32LE(4097,12);
            if(i===3)b.writeUInt32LE(1,144);
            if(i===4)b[b.length-1]=120;
            assert.equal((await session.rpc(2,b)).status,-1);
        }
        await assert.rejects(session.start('/boot/absent.snx',[]),e=>e.guestStatus===-4);
        assert.deepEqual(await resources(),baseline);
    });
    await check('v2 replay and old session cannot operate on a new session',async()=>{
        const old=session,token=await old.start('/boot/clitest.snx',['spin'],Buffer.alloc(0),0);
        const repeated=old.id;old.id--;assert.equal((await old.rpc(6)).status,-1);assert.equal(old.id,repeated);
        session=new RunSession(channel);await session.hello();
        assert.equal((await old.rpc(4,words(token))).status,-3);
        await session.heartbeat();await until(async()=>JSON.stringify(await resources())===JSON.stringify(baseline),'session rotation cleanup');
    });
    await check('v2 framing recovers after a corrupt prefix',async()=>{
        const reply=await session.rpc(6,Buffer.alloc(0),{prefix:'garbage',fragment:true});assert.equal(reply.status,0);
        for(const [payload,declaredSize] of [[words(6),4],[Buffer.alloc(0),5341]]){
            const r=await channel.request(++session.id,payload,5000,{magic:'SCS2',declaredSize});
            assert.equal(r.status,-1);assert.equal(r.output.length,0);
        }
        const partial=Buffer.alloc(16);partial.write('SCS2');partial.writeUInt32LE(++session.id,4);
        partial.writeUInt32LE(12,8);partial.writeUInt32LE(6,12);channel.socket.write(partial);
        const began=await value('ticks');
        await until(async()=>((await value('ticks'))-began)>>>0>220,'truncated frame discarded',5000);
        assert.equal((await session.rpc(6)).status,-3,'the idle truncated session has expired');
        session=new RunSession(channel);await session.hello();
        await session.heartbeat();
        assert.deepEqual(await resources(),baseline);
    });
    await check('v2 disconnect lease cancels only its managed tasks',async item=>{
        const output=await command('run /boot/spin.snx');assert.match(output,/Started user process/);
        const independent=await resources(),beforeLog=guestLog().length;
        const began=await value('ticks');
        await session.start('/boot/clitest.snx',['spin'],Buffer.alloc(0),0);
        channel.close();
        await until(()=>guestLog().slice(beforeLog).includes('RUN BRIDGE lease expired'),'disconnect lease',5000);
        item.leaseGuestTicks=((await value('ticks'))-began)>>>0;
        assert(item.leaseGuestTicks>=200 && item.leaseGuestTicks<=275,'two-second lease plus bounded request/observation delay');
        await until(async()=>JSON.stringify(await resources())===JSON.stringify(independent),'disconnected run cleanup');
        const switches=await value('process_switches');await sleep(100);
        assert((await value('process_switches'))>switches,'unrelated user process remains runnable');
    });
}
module.exports={run};
