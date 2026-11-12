const fs=require('node:fs'),path=require('node:path'),{execFileSync:run}=require('node:child_process');
const root=path.resolve(__dirname,'..'),out=path.join(root,'build32','user'),tc=path.dirname(require('./host-tools').tool('gcc'));fs.mkdirSync(out,{recursive:true});
const flags=['-m32','-march=i386','-std=c++17','-Os','-ffreestanding','-fno-builtin','-fno-exceptions','-fno-rtti','-fno-threadsafe-statics','-fno-pic','-fno-pie','-fno-stack-protector','-fno-asynchronous-unwind-tables','-mno-stack-arg-probe','-mno-sse','-mno-mmx','-msoft-float','-mpreferred-stack-boundary=2','-mincoming-stack-boundary=2','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-misleading-indentation','-Wno-register','-Wno-missing-field-initializers'];
function command(exe,args){run(exe,args,{cwd:root,stdio:'inherit'});}
command(path.join(root,'tools/nasm-3.02/nasm.exe'),['-f','win32','user/entry.asm','-o',path.join(out,'entry.o')]);
command(path.join(tc,'g++.exe'),[...flags,'-c','user/runtime.cpp','-o',path.join(out,'runtime.o')]);
// Each row names the output, source path, MODE and optional SNX ABI.
const apps = [
 ['conhost', 'user/console_host.cpp', 0, 2],
 ['writer', 'user/write_tool.cpp', 0, 2],
 ['tview', 'user/tview.cpp', 0, 2],
 ['rdtest', 'tests/guest/read_tests.cpp', 0],
 ['gsh', 'user/gsh.cpp', 0, 2],
 ['gcmd', 'user/gcmd.cpp', 0, 2],
 ['brokerqa', 'tests/guest/interactive_tests.cpp', 0],
 ['termtest', 'tests/guest/terminal_tests.cpp', 0],
 ['pipetest', 'tests/guest/pipe_tests.cpp', 0],
 ['fstest', 'tests/guest/file_tests.cpp', 0],
 ['clitest', 'tests/guest/cli_tests.cpp', 0],
 ['comptest', 'tests/guest/completion_tests.cpp', 0],
 ['poltest', 'tests/guest/policy_tests.cpp', 0],
 ['probe', 'tests/guest/probe.cpp', 0],
 ['fault', 'tests/guest/probe.cpp', 1],
 ['iofault', 'tests/guest/probe.cpp', 2],
 ['spin', 'tests/guest/probe.cpp', 3],
 ['udfault', 'tests/guest/probe.cpp', 4],
 ['nettest', 'tests/guest/nettest.cpp', 0],
 ['memv2', 'tests/guest/memory_v2_test.cpp', 0, 2]
];
for(const [name,source,mode,abi=1] of apps){
 command(path.join(tc,'g++.exe'),[...flags,'-I','user',`-DMODE=${mode}`,`-DSINO_SNX_ABI=${abi}`,'-c',source,'-o',path.join(out,`${name}.o`)]);
 const extra=[];
 if(name==='nettest'){const obj=path.join(out,`${name}-netlib.o`);command(path.join(tc,'g++.exe'),[...flags,'-c','user/netlib.cpp','-o',obj]);extra.push(obj);}
 const exe=path.join(out,`${name}.exe`),bin=path.join(out,`${name}.snx`);
 command(path.join(tc,'ld.exe'),['-m','i386pe','--image-base','0x3fff0000','--disable-reloc-section','--disable-dynamicbase','--no-insert-timestamp',`--defsym=snx_abi=${abi}`,'-T','user/linker.ld',path.join(out,'entry.o'),path.join(out,'runtime.o'),path.join(out,`${name}.o`),...extra,'-o',exe]);
 command(path.join(tc,'objcopy.exe'),['-O','binary',exe,bin]);
 const symbols={};for(const line of run(path.join(tc,'nm.exe'),['-n',exe],{encoding:'utf8'}).split('\n')){const m=line.match(/^([0-9a-f]+) [A-Za-z] (\w+)\s*$/);if(m)symbols[m[2]]=parseInt(m[1],16)-0x40000000;}
 for(const key of ['_user_start','___user_data','___user_end'])if(!Number.isInteger(symbols[key])||symbols[key]<0)throw Error('Missing/invalid native link symbol '+key);
 const bytes=fs.readFileSync(bin);bytes.writeUInt32LE(bytes.length,8);bytes.writeUInt32LE(symbols._user_start,12);bytes.writeUInt32LE(symbols.___user_data,16);bytes.writeUInt32LE(symbols.___user_end,20);
 if(bytes.readUInt32LE(4)!==abi||bytes.length>65536||symbols.___user_end>(abi===2?0x1000000:0x30000))throw Error('Native image exceeds resource limits');fs.writeFileSync(bin,bytes);console.log(`Native ${name}.snx: ${bytes.length} bytes, memory ${symbols.___user_end}`);
}
