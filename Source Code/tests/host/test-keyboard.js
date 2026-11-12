const {execFileSync}=require('node:child_process'),path=require('node:path'),fs=require('node:fs'),{tool}=require('../../tools/host-tools');
const root=path.resolve(__dirname,'../..'),out=path.join(root,'build32/test-keyboard.exe');fs.mkdirSync(path.dirname(out),{recursive:true});
execFileSync(tool('g++'),['-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-missing-field-initializers',path.join(__dirname,'test-keyboard.cpp'),path.join(root,'src32/keyboard.cpp'),'-o',out],{stdio:'inherit'});
execFileSync(out,[],{stdio:'inherit'});
