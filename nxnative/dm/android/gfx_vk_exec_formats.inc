// ---------------- formats and enums ----------------
static VkFormat vertex_format(int f, int want_int, int want_sint) {
    static const uint16_t rgb[] = {VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16_UNORM,
        VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT};
    static const uint16_t rgb_scaled[] = {VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT};
    VkFormat v;
    if (f >= 0x19 && f <= 0x24) v = (VkFormat)rgb[f - 0x19];
    else if (f >= 0x5D && f <= 0x62) v = (VkFormat)rgb_scaled[f - 0x5D];
    else { const vkfmt_t *e = vk_fmt(f); v = e ? e->vk : VK_FORMAT_UNDEFINED; }
    if (want_int) {   // the shader declared an integer input: the attribute must be an integer format of the same layout
        switch (v) {
        case VK_FORMAT_R32_SFLOAT: return want_sint ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT;
        case VK_FORMAT_R32G32_SFLOAT: return want_sint ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT;
        case VK_FORMAT_R32G32B32_SFLOAT: return want_sint ? VK_FORMAT_R32G32B32_SINT : VK_FORMAT_R32G32B32_UINT;
        case VK_FORMAT_R32G32B32A32_SFLOAT: return want_sint ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_USCALED: return want_sint ? VK_FORMAT_R8G8B8A8_SINT : VK_FORMAT_R8G8B8A8_UINT;
        case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_USCALED: return want_sint ? VK_FORMAT_R16G16_SINT : VK_FORMAT_R16G16_UINT;
        case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_USCALED: return want_sint ? VK_FORMAT_R16G16B16A16_SINT : VK_FORMAT_R16G16B16A16_UINT;
        default: return v;
        }
    }
    return v;
}
// Numeric kind of integer fetch formats: preserve the format's extension before bitcasting in SPIR-V.
static int vertex_integer_kind(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32A32_UINT: return 2;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_SINT: return 1;
    default: return 0;
    }
}
static const VkBlendFactor BF[0x65] = {
    [1] = VK_BLEND_FACTOR_ZERO, [2] = VK_BLEND_FACTOR_ONE, [3] = VK_BLEND_FACTOR_SRC_COLOR, [4] = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, [5] = VK_BLEND_FACTOR_SRC_ALPHA,
    [6] = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, [7] = VK_BLEND_FACTOR_DST_ALPHA, [8] = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, [9] = VK_BLEND_FACTOR_DST_COLOR,
    [10] = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, [11] = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, [0x10] = VK_BLEND_FACTOR_SRC1_COLOR, [0x11] = VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR,
    [0x12] = VK_BLEND_FACTOR_SRC1_ALPHA, [0x13] = VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, [0x61] = VK_BLEND_FACTOR_CONSTANT_COLOR, [0x62] = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
    [0x63] = VK_BLEND_FACTOR_CONSTANT_ALPHA, [0x64] = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA};
static VkBlendFactor bfac(unsigned f) { return f < 0x65 && (f == 1 || BF[f]) ? BF[f] : VK_BLEND_FACTOR_ONE; }
static VkBlendOp bop(unsigned e) { return e >= 1 && e <= 5 ? (VkBlendOp)(e - 1) : VK_BLEND_OP_ADD; }
static VkCompareOp cmpop(unsigned f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }
static VkStencilOp sop(unsigned o) { return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP; }
static VkPrimitiveTopology topo(int p) {
    switch (p) { case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST; case 2: case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
                 case 5: case 8: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; case 6: case 9: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
                 case 0xA: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; case 0xB: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
                 case 0xC: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; case 0xD: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY; case 0xE: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
                 default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; }
}

