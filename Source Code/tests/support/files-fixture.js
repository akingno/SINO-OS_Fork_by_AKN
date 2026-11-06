/* Private disk only: exact 64-KiB binary file and an installed-menu alias. */
const fs=require('node:fs'),{crc32}=require('../../tools/test-volume');
function seedFilesDisk(file){
 const large=Buffer.from(Array.from({length:65536},(_,i)=>(i*37+(i>>>8))&255));
 const records=new Map([['/home/user/Desktop',{dir:true,data:Buffer.alloc(0)}],['/home/user/Documents',{dir:true,data:Buffer.alloc(0)}],['/home/user/Trash',{dir:true,data:Buffer.alloc(0)}],['/home/user/Documents/Large.bin',{dir:false,data:large}],['/home/user/.sino/menu/qa-calc.lnk',{dir:false,data:Buffer.from('/boot/calc.snx\nQA Calculator\n')}]]);
 const chunks=[];for(const [name,e] of records){const h=Buffer.alloc(160);h.write(name);h.writeUInt32LE(e.dir?1:0,128);h.writeUInt32LE(e.data.length,132);chunks.push(h,e.data);}
 const data=Buffer.concat(chunks),b=Buffer.alloc(16*1024*1024),at=8*512;b.write('SINODSK1');for(const [off,value] of [[0,0x31534653],[4,1],[8,1],[12,data.length],[16,records.size],[20,crc32(data)],[24,0x434f4d54]])b.writeUInt32LE(value,at+off);data.copy(b,at+512);fs.writeFileSync(file,b);return large;
}
module.exports={seedFilesDisk};
