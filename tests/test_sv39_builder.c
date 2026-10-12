#include "../src/sv39_builder.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct sv39_page_table pool[8], before[8];
static struct sv39_builder b;
static unsigned checks;
#define CHECK(x) do { assert(x); ++checks; } while (0)
static void init(size_t n) { CHECK(sv39_builder_init(&b,pool,n,0x81000000)==SV39_OK); }
static void reject(uint64_t va,uint64_t pa,uint64_t f,enum sv39_result code)
{
    struct sv39_builder old=b;
    memcpy(before,pool,sizeof(pool));
    CHECK(sv39_map4k(&b,va,pa,f)==code);
    CHECK(memcmp(before,pool,sizeof(pool))==0);
    CHECK(memcmp(&old,&b,sizeof(b))==0);
}
static void reject2m(uint64_t va,uint64_t pa,uint64_t f,enum sv39_result code)
{
    struct sv39_builder old=b;
    memcpy(before,pool,sizeof(pool));
    CHECK(sv39_map2m(&b,va,pa,f)==code);
    CHECK(memcmp(before,pool,sizeof(pool))==0);
    CHECK(memcmp(&old,&b,sizeof(b))==0);
}
/* Independent literal decoder, not production macros or production traversal. */
static void mapping(uint64_t va,uint64_t pa,uint64_t flags)
{
    size_t t=0;
    for(int l=2;l>0;--l) {
        uint64_t e=pool[t].entries[(va>>(12+9*l))&511];
        CHECK((e&0xffc00000000003ffULL)==1);
        uint64_t table_pa=(e>>10)*4096;
        CHECK(table_pa>=0x81000000 && table_pa<0x81000000+b.used*4096);
        t=(table_pa-0x81000000)/4096;
    }
    uint64_t e=pool[t].entries[(va>>12)&511];
    CHECK(e==((pa/4096)*1024|flags|1));
    CHECK(((e>>10)&0xfffffffffffULL)*4096==pa);
}
int main(void)
{
    CHECK(sizeof(pool[0])==4096 && _Alignof(struct sv39_page_table)==4096);
    CHECK(((uintptr_t)pool&4095)==0); init(8);
    CHECK(sv39_map4k(&b,0,0,2)==SV39_OK); CHECK(b.used==3); mapping(0,0,2);
    CHECK(sv39_map4k(&b,4096,0x81234000,0xd6)==SV39_OK); CHECK(b.used==3); mapping(4096,0x81234000,0xd6);
    uint64_t va=(3ULL<<30)|(5ULL<<21)|(7ULL<<12);
    CHECK(sv39_map4k(&b,va,0xabcdef12345000ULL,8)==SV39_OK); mapping(va,0xabcdef12345000ULL,8);
    CHECK(pool[0].entries[3]==((0x81003000ULL>>12)<<10|1));
    CHECK(pool[3].entries[5]==((0x81004000ULL>>12)<<10|1));
    CHECK(pool[4].entries[7]==((0xabcdef12345000ULL>>12)<<10|9));
    CHECK(sv39_map4k(&b,0xfffffffffffff000ULL,0xfffffffffff000ULL,0xee)==SV39_OK);
    mapping(0xfffffffffffff000ULL,0xfffffffffff000ULL,0xee);
    CHECK(sv39_map4k(&b,0xffffffc000000000ULL,0x1000,2)==SV39_POOL_FULL);
    reject(0,0,2,SV39_CONFLICT); reject(0,4096,2,SV39_CONFLICT);
    reject(0,0,8,SV39_CONFLICT);
    reject(0x4000000000ULL,0,2,SV39_BAD_VA);
    reject(0xffffff8000000000ULL,0,2,SV39_BAD_VA);
    reject(1,0,2,SV39_BAD_VA); reject(0x10000,1,2,SV39_BAD_PA);
    reject(0x10000,1ULL<<56,2,SV39_BAD_PA);
    uint64_t bad[]={0,1,4,12,256,1ULL<<54,1ULL<<63,0x102};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) reject(0x10000,0,bad[i],SV39_BAD_FLAGS);
    /* Exhaustion after existing L2, and with 0/1 spare pages on empty root. */
    init(3); CHECK(sv39_map4k(&b,0,0,2)==SV39_OK);
    reject(0x200000,0,2,SV39_POOL_FULL);
    init(1); reject(0,0,2,SV39_POOL_FULL);
    init(2); reject(0,0,2,SV39_POOL_FULL);
    init(8); CHECK(sv39_map4k(&b,0x3ffffff000ULL,0,2)==SV39_OK);
    mapping(0x3ffffff000ULL,0,2);
    CHECK(sv39_map4k(&b,0xffffffc000000000ULL,0x1000,2)==SV39_OK);
    mapping(0xffffffc000000000ULL,0x1000,2);
    /* All architecturally valid permission combinations, including RWX. */
    for(unsigned f=2;f<256;f+=2) {
        if (!(f&10) || ((f&4) && !(f&2))) continue;
        init(3); CHECK(sv39_map4k(&b,0,0,f)==SV39_OK); mapping(0,0,f);
    }
    init(8); for(size_t i=0;i<8;++i) for(size_t j=0;j<512;++j) CHECK(pool[i].entries[j]==0);
    CHECK(b.used==1); CHECK(sv39_map4k(&b,0,0,2)==SV39_OK);
    /* Invalid init leaves previous valid builder/pool intact. */
    struct sv39_builder old=b; memcpy(before,pool,sizeof(pool));
    CHECK(sv39_builder_init(&b,pool,0,0)==SV39_BAD_ARGUMENT);
    CHECK(sv39_builder_init(&b,pool,8,1)==SV39_BAD_ARGUMENT);
    CHECK(sv39_builder_init(&b,pool,8,0xfffffffffff000ULL)==SV39_BAD_ARGUMENT);
    CHECK(sv39_builder_init(&b,pool,SIZE_MAX,0)==SV39_BAD_ARGUMENT);
    CHECK(sv39_builder_init(&b,NULL,8,0)==SV39_BAD_ARGUMENT);
    CHECK(sv39_builder_init(NULL,pool,8,0)==SV39_BAD_ARGUMENT);
    CHECK(memcmp(&old,&b,sizeof(b))==0 && memcmp(before,pool,sizeof(pool))==0);
    CHECK(sv39_map4k(NULL,0,0,2)==SV39_BAD_ARGUMENT);
    /* Invalid edges must never become arbitrary host memory accesses. */
    pool[0].entries[0]=0x20000001; reject(0,0,2,SV39_BAD_TABLE);
    pool[0].entries[0]=1; reject(0,0,2,SV39_BAD_TABLE);
    pool[0].entries[0]=((0x81000000ULL>>12)<<10)|1; reject(0,0,2,SV39_BAD_TABLE);
    pool[0].entries[0]=0x20000003; reject(0,0,2,SV39_CONFLICT);
    pool[0].entries[0]=0x20000011; reject(0,0,2,SV39_BAD_TABLE);
    /* v1.3: 2 MiB leaves (level 1), decoded literally. */
    init(8);
    CHECK(sv39_map2m(&b,0x80400000ULL,0x80400000ULL,0xc6)==SV39_OK); CHECK(b.used==2);
    CHECK(pool[0].entries[2]==((0x81001000ULL>>12)<<10|1));
    CHECK(pool[1].entries[2]==((0x80400000ULL>>12)<<10|0xc7));
    CHECK(sv39_map2m(&b,0x80600000ULL,0x80600000ULL,0xc6)==SV39_OK); CHECK(b.used==2);
    CHECK(pool[1].entries[3]==((0x80600000ULL>>12)<<10|0xc7));
    reject2m(0x80400000ULL,0x80400000ULL,0xc6,SV39_CONFLICT);
    reject2m(0x80400000ULL,0x80a00000ULL,0x2,SV39_CONFLICT);
    reject(0x80401000ULL,0x1000,2,SV39_CONFLICT);          /* 4K inside a 2M leaf */
    CHECK(sv39_map4k(&b,0x80000000ULL,0x1000,2)==SV39_OK); CHECK(b.used==3);
    reject2m(0x80000000ULL,0x80000000ULL,2,SV39_CONFLICT);  /* slot holds a table */
    reject2m(0x80401000ULL,0,2,SV39_BAD_VA); reject2m(0x4000000000ULL,0,2,SV39_BAD_VA);
    reject2m(0x80800000ULL,0x1000,2,SV39_BAD_PA); reject2m(0x80800000ULL,1ULL<<56,2,SV39_BAD_PA);
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) reject2m(0x80800000ULL,0,bad[i],SV39_BAD_FLAGS);
    CHECK(sv39_map2m(NULL,0,0,2)==SV39_BAD_ARGUMENT);
    init(1); reject2m(0,0,2,SV39_POOL_FULL);
    init(2); CHECK(sv39_map2m(&b,0,0,2)==SV39_OK); CHECK(b.used==2);
    CHECK(sv39_map2m(&b,0x200000,0x200000,2)==SV39_OK); CHECK(b.used==2);
    init(8); pool[0].entries[0]=1; reject2m(0,0,2,SV39_BAD_TABLE);
    pool[0].entries[0]=0x20000003; reject2m(0,0,2,SV39_CONFLICT);
    pool[0].entries[0]=0x20000011; reject2m(0,0,2,SV39_BAD_TABLE);
    printf("PASS_STATIC_ONLY: %u assertions; host execution, no guest MMU\n",checks);
}
