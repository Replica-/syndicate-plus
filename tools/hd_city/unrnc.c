#include <stdio.h>
#include <stdlib.h>
#include "rnc.h"
int main(int c, char **v){ FILE*f=fopen(v[1],"rb"); static unsigned char in[8<<20]; size_t n=fread(in,1,sizeof in,f); fclose(f);
 size_t s=rnc_size(in,n); unsigned char*o; if(!s){o=in;s=n;} else {o=malloc(s); s=rnc_unpack(in,n,o,s);} f=fopen(v[2],"wb"); fwrite(o,1,s,f); fclose(f); printf("%zu\n",s); return 0;}
