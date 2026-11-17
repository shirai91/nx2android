// Source: nxnative Android parity 1f01e28c; only DrawTexture adapted to an ordinary target draw.
// The same bytes can be a linear render target and an sRGB sampling object.
// Both directions must retain storage while each descriptor supplies its view.
static void check_texture_srgb_aliases_first(NVNdevice *device,NVNqueue *queue,int first,const NVNformat formats[2]) {
    enum { W=8,H=8,BYTES=W*H*4 };
    static _Alignas(16384) uint8_t storage[0x10000],commands[0x20000];
    NVNmemoryPoolBuilder pb;NVNmemoryPool pool,command_pool;
    API(nvnMemoryPoolBuilderSetDefaults)(&pb);API(nvnMemoryPoolBuilderSetDevice)(&pb,device);
    API(nvnMemoryPoolBuilderSetFlags)(&pb,NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    API(nvnMemoryPoolBuilderSetStorage)(&pb,commands,sizeof commands);assert(API(nvnMemoryPoolInitialize)(&command_pool,&pb));
    API(nvnMemoryPoolBuilderSetFlags)(&pb,NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    API(nvnMemoryPoolBuilderSetStorage)(&pb,storage,sizeof storage);assert(API(nvnMemoryPoolInitialize)(&pool,&pb));
    NVNbufferAddress address=API(nvnMemoryPoolGetBufferAddress)(&command_pool);
    NVNtextureBuilder tb;NVNtexture textures[2],output;
    API(nvnTextureBuilderSetDefaults)(&tb);API(nvnTextureBuilderSetDevice)(&tb,device);
    API(nvnTextureBuilderSetTarget)(&tb,getenv("NX2A_ALIAS_ARRAY") ? NVN_TEXTURE_TARGET_2D_ARRAY : NVN_TEXTURE_TARGET_2D);
    API(nvnTextureBuilderSetSize3D)(&tb,W,H,getenv("NX2A_ALIAS_ARRAY") ? 2 : 1);
    API(nvnTextureBuilderSetStorage)(&tb,&pool,0);
    for (int i=0;i<2;i++) {
        API(nvnTextureBuilderSetFormat)(&tb,formats[i]);
        assert(API(nvnTextureInitialize)(&textures[i],&tb));
    }
    API(nvnTextureBuilderSetStorage)(&tb,&pool,0x4000);API(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8);
    assert(API(nvnTextureInitialize)(&output,&tb));
    NVNtexturePool tp;NVNsamplerPool sp;NVNsamplerBuilder sb;
    assert(API(nvnTexturePoolInitialize)(&tp,&command_pool,0x8000,258));
    assert(API(nvnSamplerPoolInitialize)(&sp,&command_pool,0xd000,257));
    for (int i=0;i<2;i++) API(nvnTexturePoolRegisterTexture)(&tp,256+i,&textures[i],NULL);
    API(nvnSamplerBuilderSetDefaults)(&sb);API(nvnSamplerBuilderSetDevice)(&sb,device);
    API(nvnSamplerBuilderSetMinMagFilter)(&sb,NVN_MIN_FILTER_NEAREST,NVN_MAG_FILTER_NEAREST);
    API(nvnSamplerPoolRegisterSamplerBuilder)(&sp,256,&sb);
    NVNcommandBuffer cb;assert(API(nvnCommandBufferInitialize)(&cb,device));
    API(nvnCommandBufferAddCommandMemory)(&cb,&command_pool,0,0x3000);
    NVNcopyRegion full={0,0,0,W,H,1};
    int noalpha[2];
    for (int i=0;i<2;i++) noalpha[i]=formats[i]==NVN_FORMAT_RGBX8 || formats[i]==NVN_FORMAT_RGBX8_SRGB ||
        formats[i]==NVN_FORMAT_BGRX8 || formats[i]==NVN_FORMAT_BGRX8_SRGB;
    for (int phase=0;phase<4;phase++) {
        int writer=(phase+first)%2; // repeat to exercise cached views after the opposite alias writes
        // X storage's unused alpha is unspecified; use one on writes, while
        // an X reader must return one even when an A writer stored another value.
        float values[]={.25f,.5f,.75f,noalpha[writer] ? 1 : .375f};
        API(nvnCommandBufferBeginRecording)(&cb);
        API(nvnCommandBufferSetTexturePool)(&cb,&tp);API(nvnCommandBufferSetSamplerPool)(&cb,&sp);
        const NVNtexture *target[]={&textures[writer]};
        API(nvnCommandBufferSetRenderTargets)(&cb,1,target,NULL,NULL,NULL);
        API(nvnCommandBufferSetScissor)(&cb,0,0,W,H);API(nvnCommandBufferClearColor)(&cb,0,values,15);
        target[0]=&output;API(nvnCommandBufferSetRenderTargets)(&cb,1,target,NULL,NULL,NULL);
        NVNtextureHandle handle=API(nvnDeviceGetTextureHandle)(device,257-writer,256);
        API(nvnCommandBufferBindProgram)(&cb,&alias_program,3);
        API(nvnCommandBufferBindTexture)(&cb,NVN_SHADER_STAGE_FRAGMENT,0,handle);
        API(nvnCommandBufferSetViewport)(&cb,0,0,W,H);
        API(nvnCommandBufferDrawArrays)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
        API(nvnCommandBufferSetCopyRowStride)(&cb,0);API(nvnCommandBufferSetCopyImageStride)(&cb,0);
        API(nvnCommandBufferCopyTextureToBuffer)(&cb,&output,NULL,&full,address+0x4000,0);
        NVNcommandHandle list=API(nvnCommandBufferEndRecording)(&cb);
        API(nvnQueueSubmitCommands)(queue,1,&list);API(nvnQueueFinish)(queue);
        uint8_t expected[4];
        for (int ch=0;ch<4;ch++) {
            float x=ch==3 && noalpha[1-writer] ? 1 : values[ch];
            if (ch<3) {
                if (writer) x=x<=.0031308f ? x*12.92f : 1.055f*powf(x,1/2.4f)-.055f;
                else { x=roundf(x*255)/255;x=x<=.04045f ? x/12.92f : powf((x+.055f)/1.055f,2.4f); }
            }
            expected[ch]=(uint8_t)lroundf(x*255);
        }
        for (int pixel=0;pixel<W*H;pixel++) for (int ch=0;ch<4;ch++) {
            int got=commands[0x4000+4*pixel+ch];
            if (abs(got-expected[ch])>1) die("sRGB alias formats%#x/%#x first%d phase%d pixel%d channel%d: %d expected %u",formats[0],formats[1],first,phase,pixel,ch,got,expected[ch]);
        }
    }
    API(nvnCommandBufferFinalize)(&cb);API(nvnTexturePoolFinalize)(&tp);API(nvnSamplerPoolFinalize)(&sp);
    API(nvnTextureFinalize)(&output);for (int i=0;i<2;i++) API(nvnTextureFinalize)(&textures[i]);
    API(nvnMemoryPoolFinalize)(&pool);API(nvnMemoryPoolFinalize)(&command_pool);
    puts("Public linear/sRGB texture aliases preserved shared bytes and each descriptor's color conversion");
}

static void check_texture_srgb_aliases(NVNdevice *device,NVNqueue *queue) {
    static const NVNformat pairs[][2]={
        {NVN_FORMAT_RGBA8,NVN_FORMAT_RGBA8_SRGB},
        {NVN_FORMAT_RGBA8,NVN_FORMAT_RGBX8_SRGB},
        {NVN_FORMAT_RGBX8,NVN_FORMAT_RGBA8_SRGB},
        {NVN_FORMAT_RGBX8,NVN_FORMAT_RGBX8_SRGB},
        {NVN_FORMAT_BGRA8,NVN_FORMAT_BGRA8_SRGB},
        {NVN_FORMAT_BGRA8,NVN_FORMAT_BGRX8_SRGB},
        {NVN_FORMAT_BGRX8,NVN_FORMAT_BGRA8_SRGB},
        {NVN_FORMAT_BGRX8,NVN_FORMAT_BGRX8_SRGB},
    };
    for (unsigned i=0;i<sizeof pairs/sizeof *pairs;i++) for (int first=0;first<2;first++)
        check_texture_srgb_aliases_first(device,queue,first,pairs[i]);
}
