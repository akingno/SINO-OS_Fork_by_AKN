const fs=require('node:fs'),path=require('node:path'),{execFileSync:run}=require('node:child_process');
const root=path.resolve(__dirname,'..'),out=path.join(root,'build32','user'),tc=path.dirname(require('./host-tools').tool('gcc'));fs.mkdirSync(out,{recursive:true});
const flags=['-m32','-march=i386','-std=c11','-Os','-ffreestanding','-fno-builtin','-fno-pic','-fno-pie','-fno-stack-protector','-fno-asynchronous-unwind-tables','-mno-stack-arg-probe','-mno-sse','-mno-mmx','-msoft-float','-mpreferred-stack-boundary=2','-mincoming-stack-boundary=2','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-misleading-indentation'];
function command(exe,args){run(exe,args,{cwd:root,stdio:'inherit'});}
command(path.join(root,'tools/nasm-3.02/nasm.exe'),['-f','win32','user/entry.asm','-o',path.join(out,'entry.o')]);
command(path.join(tc,'gcc.exe'),[...flags,'-c','user/runtime.c','-o',path.join(out,'runtime.o')]);
// Each row names the output, source path, MODE and optional SNX ABI.
const apps = [
 ['writer', 'user/write_tool.c', 0],
 ['tview', 'user/tview.c', 0],
 ['rdtest', 'tests/guest/read_tests.c', 0],
 ['gsh', 'user/gsh.c', 0],
 ['gcmd', 'user/gcmd.c', 0],
 ['brokerqa', 'tests/guest/interactive_tests.c', 0],
 ['term', 'user/terminal_frontend.c', 0],
 ['termqa', 'user/terminal_frontend.c', 1],
 ['termtest', 'tests/guest/terminal_tests.c', 0],
 ['pipetest', 'tests/guest/pipe_tests.c', 0],
 ['fstest', 'tests/guest/file_tests.c', 0],
 ['clitest', 'tests/guest/cli_tests.c', 0],
 ['comptest', 'tests/guest/completion_tests.c', 0],
 ['poltest', 'tests/guest/policy_tests.c', 0],
 ['probe', 'tests/guest/probe.c', 0],
 ['fault', 'tests/guest/probe.c', 1],
 ['iofault', 'tests/guest/probe.c', 2],
 ['spin', 'tests/guest/probe.c', 3],
 ['udfault', 'tests/guest/probe.c', 4],
 ['hello', 'user/hello.c', 0],
 ['setup', 'user/setup.c', 0],
 ['apps', 'user/apps.c', 0],
 ['instest', 'tests/guest/installer_tests.c', 0],
 ['nettest', 'tests/guest/nettest.c', 0],
 ['bstest', 'tests/guest/basic_tests.c', 0],
 ['basicrun', 'user/basicrun.c', 0],
 ['ide', 'user/ide.c', 0],
 ['files', 'user/files.c', 0],
 ['editor', 'user/editor.c', 0],
 ['memv2', 'tests/guest/memory_v2_test.c', 0, 2]
];
for(const [name,source,mode,abi=1] of apps){
 // The frontend and strict probe share one boot image. Strip unused runtime
 // sections only for this new program to fit the existing FAT12 image budget.
 const appFlags=(name==='term'||name==='termqa')?[...flags,'-ffunction-sections','-fno-ident']:flags;
 const runtime=path.join(out,(name==='term'||name==='termqa')?name+'-runtime.o':'runtime.o');
 if(name==='term'||name==='termqa')command(path.join(tc,'gcc.exe'),[...appFlags,'-c','user/runtime.c','-o',runtime]);
 command(path.join(tc,'gcc.exe'),[...appFlags,'-I','user',`-DMODE=${mode}`,`-DSINO_SNX_ABI=${abi}`,'-c',source,'-o',path.join(out,`${name}.o`)]);
 const extra=[];if(name==='termqa'){const obj=path.join(out,name+'-probe.o');command(path.join(tc,'gcc.exe'),[...appFlags,'-I','user','-c','tests/guest/terminal_probe.c','-o',obj]);extra.push(obj);}
 if(name==='setup'||name==='instest'){const obj=path.join(out,`${name}-service.o`);command(path.join(tc,'gcc.exe'),[...flags,'-c','user/install_service.c','-o',obj]);extra.push(obj);}
 if(name==='bstest'||name==='basicrun'||name==='ide'){for(const module of (name==='bstest'?['basic','netlib']:['basic','netlib','forms'])){const obj=path.join(out,`${name}-${module}.o`);command(path.join(tc,'gcc.exe'),[...flags,'-c',`user/${module}.c`,'-o',obj]);extra.push(obj);}}
 if(name==='nettest'){const obj=path.join(out,`${name}-netlib.o`);command(path.join(tc,'gcc.exe'),[...flags,'-c','user/netlib.c','-o',obj]);extra.push(obj);}
 const exe=path.join(out,`${name}.exe`),bin=path.join(out,`${name}.snx`);
 command(path.join(tc,'ld.exe'),['-m','i386pe','--image-base','0x3fff0000','--disable-reloc-section','--disable-dynamicbase','--no-insert-timestamp',...((name==='term'||name==='termqa')?['--gc-sections']:[]),`--defsym=snx_abi=${abi}`,'-T','user/linker.ld',path.join(out,'entry.o'),runtime,path.join(out,`${name}.o`),...extra,'-o',exe]);
 command(path.join(tc,'objcopy.exe'),['-O','binary',exe,bin]);
 const symbols={};for(const line of run(path.join(tc,'nm.exe'),['-n',exe],{encoding:'utf8'}).split('\n')){const m=line.match(/^([0-9a-f]+) [A-Za-z] (\w+)\s*$/);if(m)symbols[m[2]]=parseInt(m[1],16)-0x40000000;}
 for(const key of ['_user_start','___user_data','___user_end'])if(!Number.isInteger(symbols[key])||symbols[key]<0)throw Error('Missing/invalid native link symbol '+key);
 const bytes=fs.readFileSync(bin);bytes.writeUInt32LE(bytes.length,8);bytes.writeUInt32LE(symbols._user_start,12);bytes.writeUInt32LE(symbols.___user_data,16);bytes.writeUInt32LE(symbols.___user_end,20);
 if(bytes.readUInt32LE(4)!==abi||bytes.length>65536||symbols.___user_end>(abi===2?0x1000000:0x30000))throw Error('Native image exceeds resource limits');fs.writeFileSync(bin,bytes);console.log(`Native ${name}.snx: ${bytes.length} bytes, memory ${symbols.___user_end}`);
}


run(process.execPath,[path.join(root,"tools/package.js"),path.join(root,"packages/hello.json"),path.join(out,"hello.sxp")],{stdio:"inherit"});

run(process.execPath,[path.join(root,"tools/package.js"),path.join(root,"packages/sysbasic.json"),path.join(out,"sysbasic.sxp")],{stdio:"inherit"});

for(const name of ["files","editor"])run(process.execPath,[path.join(root,"tools/package.js"),path.join(root,`packages/${name}.json`),path.join(out,`${name}.sxp`)],{stdio:"inherit"});
