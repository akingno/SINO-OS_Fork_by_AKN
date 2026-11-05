#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../user/text_scan.h"
int main(void) {
  TextCounts c={0};const char *s="one two\n\xe4\xbd\xa0\tthree";
  for(unsigned i=0;i<strlen(s);i++)text_count(&c,(unsigned char)s[i]);
  assert(c.bytes==17&&c.lines==1&&c.words==4);
  TextMatch m;assert(text_pattern(&m,"aba",3)==0);
  const char *p="aababa";int hits=0;
  for(unsigned i=0;i<strlen(p);i++)hits+=text_match(&m,(unsigned char)p[i]);
  assert(hits==2);assert(text_pattern(&m,"",0)==0&&text_match(&m,'x'));
  char big[257];memset(big,'a',256);assert(text_pattern(&m,big,256)<0);
  unsigned n;assert(text_unsigned("0",&n)==0&&!n);
  assert(text_unsigned("65536",&n)==0&&n==65536);
  assert(text_unsigned("-1",&n)<0&&text_unsigned("4294967296",&n)<0);
  puts("TEXT SCAN PASS");return 0;
}
