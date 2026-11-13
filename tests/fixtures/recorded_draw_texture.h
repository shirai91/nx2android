// Run the same public command list and pixel oracle on Metal and Vulkan.
static void check_draw_texture(NVNdevice *device, NVNqueue *queue) {
    static _Alignas(16384) uint8_t bytes[0x10000];
    NVNmemoryPoolBuilder pb;
    NVNmemoryPool pool;
    API(nvnMemoryPoolBuilderSetDefaults)(&pb);
    API(nvnMemoryPoolBuilderSetDevice)(&pb, device);
    API(nvnMemoryPoolBuilderSetStorage)(&pb, bytes, sizeof bytes);
    API(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    assert(API(nvnMemoryPoolInitialize)(&pool, &pb));
    NVNbufferAddress addr = API(nvnMemoryPoolGetBufferAddress)(&pool);
    NVNtexture images[3];
    NVNtextureBuilder tb;
    API(nvnTextureBuilderSetDefaults)(&tb);
    API(nvnTextureBuilderSetDevice)(&tb, device);
    API(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
    API(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    for (int i = 0; i < 3; ++i) {
        API(nvnTextureBuilderSetSize2D)(&tb, i ? 8 : 4, i ? 6 : 3);
        API(nvnTextureBuilderSetLevels)(&tb, i ? 1 : 2);
        assert(API(nvnTextureBuilderGetStorageSize)(&tb) <= 0x1000);
        API(nvnTextureBuilderSetStorage)(&tb, &pool, 0x2000 + 0x1000*i);
        assert(API(nvnTextureInitialize)(&images[i], &tb));
    }
    NVNtexturePool tp;
    NVNsamplerPool sp;
    assert(API(nvnTexturePoolInitialize)(&tp, &pool, 0x6000, 257));
    assert(API(nvnSamplerPoolInitialize)(&sp, &pool, 0xa000, 258));
    API(nvnTexturePoolRegisterTexture)(&tp, 256, &images[0], NULL);
    NVNsamplerBuilder sb;
    API(nvnSamplerBuilderSetDefaults)(&sb);
    API(nvnSamplerBuilderSetDevice)(&sb, device);
    for (int linear = 0; linear < 2; ++linear) {
        API(nvnSamplerBuilderSetMinMagFilter)(&sb, linear ? NVN_MIN_FILTER_LINEAR : NVN_MIN_FILTER_NEAREST,
                                             linear ? NVN_MAG_FILTER_LINEAR : NVN_MAG_FILTER_NEAREST);
        API(nvnSamplerPoolRegisterSamplerBuilder)(&sp, 256 + linear, &sb);
    }
    const NVNtexture *targets[] = {&images[1], &images[2]};
    NVNchannelMaskState masks;
    API(nvnChannelMaskStateSetDefaults)(&masks);
    API(nvnChannelMaskStateSetChannelMask)(&masks, 0, 1, 0, 1, 0);
    API(nvnChannelMaskStateSetChannelMask)(&masks, 1, 0, 1, 0, 1);
    for (int y = 0; y < 3; ++y) for (int x = 0; x < 4; ++x) for (int c = 0; c < 4; ++c)
        bytes[0x5000 + 4*(4*y+x)+c] = 16 + 16*x + 32*y + 3*c;
    NVNcopyRegion source = {0,0,0,4,3,1}, full = {0,0,0,8,6,1};
    NVNdrawTextureRegion dst = {7.25f,5.25f,1.25f,1.25f}, src = {.75f,.5f,3.75f,2.5f};
    NVNcommandBuffer cb;
    assert(API(nvnCommandBufferInitialize)(&cb, device));
    API(nvnCommandBufferAddCommandMemory)(&cb, &pool, 0, 0x2000);
    for (int linear = 0; linear < 2; ++linear) {
        API(nvnCommandBufferBeginRecording)(&cb);
        API(nvnCommandBufferSetCopyRowStride)(&cb, 0);
        API(nvnCommandBufferSetCopyImageStride)(&cb, 0);
        API(nvnCommandBufferCopyBufferToTexture)(&cb, addr + 0x5000, &images[0], NULL, &source, 0);
        API(nvnCommandBufferSetRenderTargets)(&cb, 2, targets, NULL, NULL, NULL);
        API(nvnCommandBufferSetScissor)(&cb, 0, 0, 8, 6);
        API(nvnCommandBufferClearColor)(&cb, 0, (float[4]){0,1,0,1}, 15);
        API(nvnCommandBufferClearColor)(&cb, 1, (float[4]){1,0,1,0}, 15);
        API(nvnCommandBufferSetScissor)(&cb, 2, 0, 4, 6);
        API(nvnCommandBufferSetTexturePool)(&cb, &tp);
        API(nvnCommandBufferSetSamplerPool)(&cb, &sp);
        API(nvnCommandBufferBindChannelMaskState)(&cb, &masks);
        NVNtextureHandle handle = API(nvnDeviceGetTextureHandle)(device, 256, 256 + linear);
        API(nvnCommandBufferDrawTexture)(&cb, handle, &dst, &src);
        if (DM_GAME_RENDER_COND) {
            API(nvnCommandBufferSetRenderEnable)(&cb, 0);
            API(nvnCommandBufferDrawTexture)(&cb, handle, &(NVNdrawTextureRegion){0,0,8,6}, &src);
            API(nvnCommandBufferSetRenderEnable)(&cb, 1);
        }
        for (int i = 0; i < 2; ++i)
            API(nvnCommandBufferCopyTextureToBuffer)(&cb, targets[i], NULL, &full, addr + 0x5100 + i*256, 0);
        NVNcommandHandle list = API(nvnCommandBufferEndRecording)(&cb);
        API(nvnQueueSubmitCommands)(queue, 1, &list);
        API(nvnQueueFinish)(queue);
        for (int i = 0; i < 2; ++i) for (int y = 0; y < 6; ++y) for (int x = 0; x < 8; ++x)
            for (int c = 0; c < 4; ++c) {
                int writable = (c & 1) == i, expected = writable ? 0 : 255;
                if (writable && x >= 2 && x < 6 && y >= 1 && y < 5) {
                    float u = .75f + (7.25f - (x+.5f))*.5f, v = .5f + (5.25f - (y+.5f))*.5f;
                    expected = 16 + 3*c + (linear ? 16*(u-.5f) + 32*(v-.5f) : 16*(int)u + 32*(int)v);
                }
                int actual = bytes[0x5100 + i*256 + 4*(8*y+x) + c];
                if (abs(actual - expected) > (linear && writable ? 1 : 0))
                    die("DrawTexture filter %d target %d pixel %d,%d channel %d: got %d expected %d",
                        linear, i, x, y, c, actual, expected);
            }
    }
#ifdef DM_VK_PUBLIC_BINDINGS
    // Minification samples mip 1 while writing that same mip. Both output
    // pixels must see the pre-draw contents, even on a tiled Vulkan driver.
    NVNtextureView mip;
    API(nvnTextureViewSetDefaults)(&mip);
    API(nvnTextureViewSetLevels)(&mip, 1, 1);
    API(nvnSamplerBuilderSetMinMagFilter)(&sb, NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST, NVN_MAG_FILTER_NEAREST);
    API(nvnSamplerPoolRegisterSamplerBuilder)(&sp, 256, &sb);
    const uint8_t pair[8] = {10,20,30,40,80,90,100,110};
    memcpy(bytes + 0x5300, pair, sizeof pair);
    NVNcopyRegion two = {0,0,0,2,1,1};
    API(nvnCommandBufferBeginRecording)(&cb);
    API(nvnCommandBufferCopyBufferToTexture)(&cb, addr + 0x5300, &images[0], &mip, &two, 0);
    API(nvnCommandBufferSetRenderTargets)(&cb, 1, (const NVNtexture *[]){&images[0]},
                                         (const NVNtextureView *[]){&mip}, NULL, NULL);
    API(nvnChannelMaskStateSetDefaults)(&masks);
    API(nvnCommandBufferBindChannelMaskState)(&cb, &masks);
    API(nvnCommandBufferSetScissor)(&cb, 0, 0, 2, 1);
    API(nvnCommandBufferSetTexturePool)(&cb, &tp);
    API(nvnCommandBufferSetSamplerPool)(&cb, &sp);
    API(nvnCommandBufferDrawTexture)(&cb, API(nvnDeviceGetTextureHandle)(device,256,256),
                                     &(NVNdrawTextureRegion){0,0,2,1}, &(NVNdrawTextureRegion){4,0,0,3});
    API(nvnCommandBufferCopyTextureToBuffer)(&cb, &images[0], &mip, &two, addr + 0x5400, 0);
    NVNcommandHandle flip = API(nvnCommandBufferEndRecording)(&cb);
    API(nvnQueueSubmitCommands)(queue, 1, &flip);
    API(nvnQueueFinish)(queue);
    assert(!memcmp(bytes+0x5400,pair+4,4) && !memcmp(bytes+0x5404,pair,4));
#endif
    API(nvnCommandBufferFinalize)(&cb);
    API(nvnTexturePoolFinalize)(&tp);
    API(nvnSamplerPoolFinalize)(&sp);
    for (int i = 0; i < 3; ++i) API(nvnTextureFinalize)(&images[i]);
    API(nvnMemoryPoolFinalize)(&pool);
    puts("Public NVN DrawTexture: two masked targets, clipped and flipped fractional rectangles, nearest and linear samplers");
}
