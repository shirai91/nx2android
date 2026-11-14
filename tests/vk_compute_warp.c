// Replay existing donor Maxwell programs and compare their independent per-lane oracles.
#include "hle.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "gfx_vk.h"
#include <assert.h>
static prog_t programs[256];
int __wrap_prog_key(const prog_t *p,unsigned stage,uint8_t key[8]){if(stage)return 0;uint64_t v=p->serial;for(unsigned i=0;i<8;i++)key[7-i]=(uint8_t)(v>>(8*i));return 1;}
typedef struct{uint8_t bytes[1024];size_t used;} list_t;
static void put(list_t *l,unsigned op,const void *p,size_t n){size_t size=(sizeof(op_hdr)+n+7)&~7;assert(l->used+size<=sizeof l->bytes);op_hdr h={op,(uint32_t)size};memcpy(l->bytes+l->used,&h,sizeof h);if(n)memcpy(l->bytes+l->used+sizeof h,p,n);l->used+=size;}
#define RECORD(op,T,...) do{T value={__VA_ARGS__};put(&list,op,&value,sizeof value);}while(0)
int __wrap_main(void){
    setenv("DM_VK_NOPREWARM","1",1);mtl_init();
    _Alignas(4096) uint32_t uniform[16384]={0},output[512];
    mtl_pool_add(uniform,sizeof uniform);mtl_pool_add(output,sizeof output);
    uint64_t result=(uintptr_t)(output+64);memcpy(uniform+14,&result,8);
    FILE *cases=fopen("cases.txt","r");assert(cases);char key[17];unsigned words,number=0,total=0;
    while(fscanf(cases,"%16s %u",key,&words)==2){
        assert(number<256 && words && words<=448);char path[64];snprintf(path,sizeof path,"%s.expected",key);
        FILE *f=fopen(path,"rb");assert(f);uint32_t expected[448];assert(fread(expected,4,words,f)==words);assert(fgetc(f)==EOF);fclose(f);
        for(unsigned i=0;i<512;i++)output[i]=0xbaddecaf;
        prog_t *p=programs+number;*p=(prog_t){.nstage=1,.serial=strtoull(key,NULL,16)};
        list_t list={0};RECORD(OP_BIND_UBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)uniform,.size=sizeof uniform);
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=p,.stages=0x20);RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for(unsigned i=0;i<512;i++){uint32_t want=i>=64 && i<64+words?expected[i-64]:0xbaddecaf;if(output[i]!=want)die("warp %s word%u returned %#x expected %#x",key,i,output[i],want);}
        printf("warp %s: %u words PASS\n",key,words);fflush(stdout);number++;total+=words;
    }
    fclose(cases);assert(number);mtl_pool_del(uniform);mtl_pool_del(output);mtl_finish();printf("Compute warp: %u cases, %u expected words and surrounding guards PASS\n",number,total);return 0;
}
