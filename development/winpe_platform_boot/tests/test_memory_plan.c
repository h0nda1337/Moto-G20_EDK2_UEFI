/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../memory/memory_plan.h"
int main(int argc,char **argv) {
  FILE *m,*f; char path[1024], name[100]; int expected, actual, count=0;
  long n; unsigned char *b; struct mp_plan plan; unsigned int i, seed=0x12345u;
  if(argc!=2) return 2;
  (void)snprintf(path,sizeof(path),"%s/manifest.txt",argv[1]);m=fopen(path,"r");if(!m)return 2;
  while(fscanf(m,"%99s %d",name,&expected)==2) {
    (void)snprintf(path,sizeof(path),"%s/%s.dtb",argv[1],name);f=fopen(path,"rb");if(!f)return 2;
    if(fseek(f,0,SEEK_END)|| (n=ftell(f))<0 || fseek(f,0,SEEK_SET))return 2;
    b=(unsigned char*)malloc((size_t)n+1);if(!b)return 2;
    if(fread(b,1,(size_t)n,f)!=(size_t)n)return 2;
    fclose(f);memset(&plan,0xff,sizeof(plan));actual=(int)mp_make_plan(b,(size_t)n,&plan);
    if(actual!=expected || (actual==0 && (plan.total!=3118ULL*1048576ULL || plan.count!=2)) || (actual!=0 && (plan.total!=0 || plan.count!=0))) {
      fprintf(stderr,"FAIL %s expected=%d actual=%d bytes=%llu\n",name,expected,actual,(unsigned long long)plan.total);free(b);fclose(m);return 1;
    }
    if(strcmp(name,"live_dt")==0) {
      for(i=0;i<3000;++i) {
        size_t at; unsigned char old;
        seed=seed*1664525u+1013904223u;at=seed%(size_t)n;
        old=b[at];b[at]^=(unsigned char)(1u<<(seed%8u));
        (void)mp_make_plan(b,(size_t)n,&plan);b[at]=old;
      }
    }
    free(b);++count;
  }
  fclose(m);
  if(mp_make_plan(NULL,0,&plan)!=MP_ARGUMENT || mp_make_plan("",0,NULL)!=MP_ARGUMENT) return 1;
  printf("PASS %d fixtures + null arguments; 3000 mutated DTB parses under sanitizers\n",count);return 0;
}
