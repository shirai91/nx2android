// nxnative 1f01e28c: donor compaction with existing nx2android treg_at selection.
static int table_reserve(vk_resource_table *table,uint32_t length,uint32_t handles) {
    if (table->map_capacity<length+2) {
        uint32_t *map=realloc(table->map,(size_t)(length+2)*sizeof *map);
        if (!map) return 0;
        table->map=map;table->map_capacity=length+2;
    }
    if (table->handle_capacity<handles) {
        uint64_t *values=realloc(table->handles,(size_t)handles*sizeof *values);
        if (!values) return 0;
        table->handles=values;table->handle_capacity=handles;
    }
    return 1;
}
static uint64_t pool_hash(const void *p,size_t n) { const uint8_t *b=p; uint64_t h=0xcbf29ce484222325ull; while(n--) h=(h^*b++)*0x100000001b3ull; return h; }
// The pool's guest ID space can be sparse and much larger than the driver's
// descriptor budget. Deduplicate equivalent registered resources and retain
// only a compact live descriptor array plus a logical-index map.
static uint64_t pool_registration_hash(int sampler,unsigned id) {
    if (sampler) return pool_hash(&g_sreg[id],sizeof g_sreg[id]);
    const treg_ver *texture=treg_at((int)id,nvn_replaying_job());
    uint64_t hash=pool_hash(&texture->t,sizeof texture->t) ^ texture->has_v;
    if (texture->has_v) hash^=pool_hash(&texture->v,sizeof texture->v);
    return hash;
}
static int same_pool_registration(int sampler,unsigned a,unsigned b) {
    if (sampler) return !memcmp(&g_sreg[a],&g_sreg[b],sizeof g_sreg[a]);
    const treg_ver *first=treg_at((int)a,nvn_replaying_job()),*second=treg_at((int)b,nvn_replaying_job());
    return !memcmp(&first->t,&second->t,sizeof first->t) && first->has_v==second->has_v &&
        (!first->has_v || !memcmp(&first->v,&second->v,sizeof first->v));
}
int vk_resource_pool_build(vk_resource_table *table,int sampler,int base) {
    if (base<0) return 0;
    uint32_t logical_count=sampler ? 1u<<12 : 1u<<20;
    uint64_t job=nvn_replaying_job();
    pthread_mutex_lock(&g_rmu);
    uint64_t generation=sampler ? g_samp_gen : g_tex_gen;
    if (table->pool_kind==sampler+1 && table->pool_base==base && table->registration_generation==generation && table->replay_job==job) {
        pthread_mutex_unlock(&g_rmu);return 1;
    }
    int available=(sampler ? g_nsreg : g_ntreg)-base;
    uint32_t length=available>0 ? (uint32_t)available : 1;
    if (length>logical_count) length=logical_count;
    uint32_t live=0;
    for (uint32_t i=0;i<length;i++) {
        uint64_t id=(uint64_t)(unsigned)base+i;
        if (id && id<(unsigned)(sampler ? g_nsreg : g_ntreg) &&
            (sampler ? g_sreg_ok[id] : id<=0xfffff && treg_at((int)id,job)->ok)) live++;
    }
    if (!table_reserve(table,length,live+1)) { pthread_mutex_unlock(&g_rmu);return 0; }
    uint32_t buckets=16;
    while (buckets<2*live) buckets*=2;
    uint32_t *slots=calloc(buckets,sizeof *slots);
    if (!slots) { pthread_mutex_unlock(&g_rmu);return 0; }
    table->count=1;table->handles[0]=0;table->map[0]=length;table->map[length+1]=0;
    for (uint32_t i=0;i<length;i++) {
        uint64_t id=(uint64_t)(unsigned)base+i;
        uint32_t slot=0;
        if (id && id<(unsigned)(sampler ? g_nsreg : g_ntreg) &&
            (sampler ? g_sreg_ok[id] : id<=0xfffff && treg_at((int)id,job)->ok)) {
            uint32_t bucket=(uint32_t)pool_registration_hash(sampler,(unsigned)id)&(buckets-1);
            while (slots[bucket]) {
                uint64_t registered=table->handles[slots[bucket]];
                unsigned other=(unsigned)(sampler ? registered>>20 : registered);
                if (same_pool_registration(sampler,(unsigned)id,other)) break;
                bucket=(bucket+1)&(buckets-1);
            }
            if (!slots[bucket]) {
                slots[bucket]=table->count++;
                table->handles[slots[bucket]]=sampler ? id<<20 : id;
            }
            slot=slots[bucket];
        }
        table->map[1+i]=slot;
    }
    free(slots);
    table->pool_kind=sampler+1;table->pool_base=base;table->registration_generation=generation;table->replay_job=job;
    pthread_mutex_unlock(&g_rmu);return 1;
}
