#include "moviebuffer.h"
#include "rede.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv) {
  assert(argc==3 || argc==4);
  if(argc==4)rede_discord_ca(argv[3]);
  char url[512],line[640];setvbuf(stdout,NULL,_IOLBF,0);
  int ok=moviebuffer_url(argv[1],"Authorization: Bearer fixture\nReferer: https://fixture.invalid/\nUser-Agent: FixturePlayer",atoi(argv[2]),url,sizeof url);
  printf("URL %s\n",ok?url:"OFF");
  while(fgets(line,sizeof line,stdin)) {
    if(line[0]=='c') {
      /* c <off> <n> [url]: copia lateral do anel; confere com o padrao da origem. */
      long long off=0;int n=0;char other[512]="";
      sscanf(line+1,"%lld %d %511s",&off,&n,other);
      unsigned char *d=malloc(n>0?(size_t)n:1);assert(d);memset(d,0xEE,n>0?(size_t)n:1);
      int got=moviebuffer_copiar(*other?other:argv[1],off,d,n),ok=1;
      for(int i=0;i<got;i++) if(d[i]!=(unsigned char)((off+i)%251)) {ok=0;break;}
      printf("COPY %d %d\n",got,ok);free(d);continue;
    }
    if(line[0]!='s')break;
    DtsBufferInfo b;int active=moviebuffer_info(&b);
    printf("STATS %d %lld %lld\n",active,(long long)b.ready_bytes,(long long)b.capacity_bytes);
  }
  moviebuffer_stop();puts("STOPPED");
}
