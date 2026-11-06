#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../user/line_edit.h"
int main(void){LineEditor e={0};
 assert(line_insert(&e,"ab",2)==1);assert(line_key(&e,331)==1);
 assert(line_insert(&e,"\xe4\xbd\xa0",3)==1);assert(!strcmp(e.text,"a你b"));
 assert(line_key(&e,8)==1&&!strcmp(e.text,"ab"));
 line_key(&e,327);line_key(&e,339);assert(!strcmp(e.text,"b"));
 line_key(&e,335);assert(line_insert(&e,"\xe5\xa5\xbd",3)==1);
 line_key(&e,331);line_key(&e,339);assert(!strcmp(e.text,"b"));
 char full[256];memset(full,'x',255);full[255]=0;line_set(&e,full);
 assert(line_insert(&e,"y",1)<0&&e.length==255&&e.cursor==255);
 assert(line_insert(&e,"\xc0\x80",2)<0);
 line_set(&e,"");assert(!line_key(&e,8)&&!line_key(&e,339));
 puts("LINE EDIT PASS");return 0;}
