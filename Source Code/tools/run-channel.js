/* Bounded, versioned CLI sessions over the development-only UART transport. */
const crypto = require('node:crypto');
const assert = require('node:assert/strict');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const words = (...values) => {const b=Buffer.alloc(values.length*4);values.forEach((v,i)=>b.writeUInt32LE(v>>>0,i*4));return b;};
class RunSession {
    constructor(channel) {
        this.channel=channel;do {this.nonce=crypto.randomBytes(8);} while(this.nonce.equals(Buffer.alloc(8)));
        this.id=0;this.tokens=new Set();
    }
    async rpc(op, body=Buffer.alloc(0), options={}) {
        if (this.id === 0xffffffff) throw Error('Run request IDs exhausted.');
        const payload=Buffer.concat([words(op),this.nonce,body]);
        const reply=await this.channel.request(++this.id,payload,5000,{magic:'SCS2',...options});
        assert(!reply.overflow && reply.output.length>=8,'invalid run response');
        assert(reply.output.subarray(0,8).equals(this.nonce),'run session mismatch');
        return {status:reply.status,body:reply.output.subarray(8)};
    }
    async hello() {
        const reply=await this.rpc(1);
        assert.equal(reply.status,0,'run handshake');
        assert.equal(reply.body.length,40);assert.equal(reply.body.readUInt32LE(),40);
        this.limits=Array.from({length:10},(_,i)=>reply.body.readUInt32LE(i*4));
        return this.limits;
    }
    static encodeStart(program,args,input,timeoutTicks) {
        assert(typeof program==='string' && program.startsWith('/') && !program.includes('\0'));
        assert(Array.isArray(args) && args.length<=16);
        const path=Buffer.from(program,'utf8');assert(path.length<128);
        const parts=args.map(s=>{assert(typeof s==='string'&&!s.includes('\0'));return Buffer.from(s+'\0','utf8');});
        const bytes=Buffer.concat(parts);assert(bytes.length<=1024);
        input=Buffer.from(input);assert(input.length<=4096);
        assert(Number.isInteger(timeoutTicks)&&timeoutTicks>=0&&timeoutTicks<=60000);
        const head=Buffer.alloc(208);head.writeUInt32LE(timeoutTicks,0);head.writeUInt32LE(args.length,4);
        head.writeUInt32LE(bytes.length,8);head.writeUInt32LE(input.length,12);path.copy(head,16);
        let offset=0;parts.forEach((part,i)=>{head.writeUInt32LE(offset,144+i*4);offset+=part.length;});
        return Buffer.concat([head,bytes,input]);
    }
    async start(program,args=[],input=Buffer.alloc(0),timeoutTicks=500) {
        const reply=await this.rpc(2,RunSession.encodeStart(program,args,input,timeoutTicks));
        if(reply.status!==0){const e=Error('Run launch failed: '+reply.status);e.guestStatus=reply.status;throw e;}
        assert.equal(reply.body.length,4);const token=reply.body.readUInt32LE();assert(token>0);
        this.tokens.add(token);return token;
    }
    async poll(token) {
        const reply=await this.rpc(3,words(token));
        if(reply.status===-6){assert.equal(reply.body.length,0);return null;}
        assert.equal(reply.status,0,'run result');assert.equal(reply.body.length,48);
        const b=reply.body;assert.equal(b.readUInt32LE(),32);assert.equal(b.readUInt32LE(4),1);
        assert.equal(b.readUInt32LE(24),0);assert.equal(b.readUInt32LE(28),0);
        const result={pid:b.readUInt32LE(8),reason:b.readUInt32LE(12),code:b.readInt32LE(16),vector:b.readUInt32LE(20),
            inputBytes:b.readUInt32LE(32),stdoutBytes:b.readUInt32LE(36),stderrBytes:b.readUInt32LE(40),incomplete:!!b.readUInt32LE(44)};
        assert(result.stdoutBytes<=4096&&result.stderrBytes<=4096);return result;
    }
    async read(token,role,offset=0,capacity=4096) {
        const reply=await this.rpc(7,words(token,role,offset,capacity));
        assert.equal(reply.status,reply.body.length,'captured byte count');return reply.body;
    }
    async cancel(token) {const r=await this.rpc(4,words(token));assert.equal(r.status,0);}
    async release(token) {const r=await this.rpc(5,words(token));assert.equal(r.status,0);this.tokens.delete(token);}
    async heartbeat() {const r=await this.rpc(6);assert.equal(r.status,0);}
    async run(program,args=[],input=Buffer.alloc(0),{timeoutTicks=500,hostTimeoutMs=timeoutTicks*10+10000}={}) {
        const token=await this.start(program,args,input,timeoutTicks),deadline=Date.now()+hostTimeoutMs;
        let succeeded=false;
        try {
            let result;
            while(!(result=await this.poll(token))) {
                if(Date.now()>deadline){const e=Error('Host timed out without a confirmed guest result.');e.classification='host-timeout';throw e;}
                await sleep(40);
            }
            const stdout=await this.read(token,1),stderr=await this.read(token,2);
            assert.equal(stdout.length,result.stdoutBytes);assert.equal(stderr.length,result.stderrBytes);
            succeeded=true;return {...result,stdout,stderr};
        } finally {
            try {await this.release(token);} catch(error) {if(succeeded)throw error;}
        }
    }
}
module.exports={RunSession,words};
