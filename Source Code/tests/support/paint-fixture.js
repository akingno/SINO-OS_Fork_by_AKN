/* Private, CRC-valid disk fixtures. Never use a development/user disk here. */
const fs=require('node:fs');
const {crc32}=require('../../tools/test-volume');
function drawing(){const b=Buffer.alloc(40968);b.write('SNP1');b.writeUInt16LE(256,4);b.writeUInt16LE(160,6);for(let i=8;i<b.length;i++)b[i]=(i^(i>>>8))&15;return b;}
function seedPaintDisk(file,aliases=true){
 const records=new Map([['/home/user/Desktop',{dir:true,data:Buffer.alloc(0)}],['/home/user/Documents',{dir:true,data:Buffer.alloc(0)}],['/home/user/Trash',{dir:true,data:Buffer.alloc(0)}],['/home/user/Desktop/Sketch.snp',{dir:false,data:drawing()}]]);
 if(aliases)for(const [name,data] of [['/home/user/Documents/CAPS.SNP',drawing()],['/home/user/Desktop/Paint.app',Buffer.from('Paint')],['/home/user/Desktop/OldPaint.lnk',Buffer.from('/Applications/Paint.app\n')],['/home/user/.sino/menu/OldPaint.lnk',Buffer.from('/Applications/Paint.app\n')]])records.set(name,{dir:false,data});
 const chunks=[];for(const [name,entry] of records){const h=Buffer.alloc(160);h.write(name);h.writeUInt32LE(entry.dir?1:0,128);h.writeUInt32LE(entry.data.length,132);chunks.push(h,entry.data);}
 const payload=Buffer.concat(chunks),disk=Buffer.alloc(16*1024*1024),header=8*512;disk.write('SINODSK1');disk.writeUInt32LE(0x31534653,header);disk.writeUInt32LE(1,header+4);disk.writeUInt32LE(1,header+8);disk.writeUInt32LE(payload.length,header+12);disk.writeUInt32LE(records.size,header+16);disk.writeUInt32LE(crc32(payload),header+20);disk.writeUInt32LE(0x434f4d54,header+24);payload.copy(disk,header+512);fs.writeFileSync(file,disk);return records;
}
async function nodes(vm){const b=await vm.memory(vm.symbols.nodes,124*128),all=[];for(let i=0;i<128;i++){const p=b.subarray(i*124,(i+1)*124);if(p.readInt32LE(32))all[i]={id:i,name:p.subarray(0,32).toString().split('\0')[0],parent:p.readInt32LE(36),size:p.readUInt32LE(56),pages:Array.from({length:16},(_,j)=>p.readUInt32LE(60+j*4))};}function pathname(i){return i?pathname(all[i].parent)+'/'+all[i].name:'';}return all.filter(Boolean).map(n=>({...n,path:pathname(n.id)||'/'}));}
async function fileBytes(vm,name){const n=(await nodes(vm)).find(n=>n.path===name);if(!n)throw Error('Missing fixture: '+name);const chunks=[];for(let i=0;i<Math.ceil(n.size/4096);i++)chunks.push(await vm.memory(n.pages[i],Math.min(4096,n.size-i*4096)));return Buffer.concat(chunks);}
module.exports={drawing,seedPaintDisk,nodes,fileBytes};
