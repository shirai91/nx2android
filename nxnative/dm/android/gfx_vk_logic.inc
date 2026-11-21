// Native logic operations from nxnative Android parity e6c73832b / 1f01e28c.
static int logic_format(VkFormat f) {
    if (clear_color_type(f)==CLEAR_UINT) return 1;
    switch (f) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16:
        case VK_FORMAT_B5G5R5A1_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16: return 1;
        default: return 0;
    }
}
// Parent output-mask handling already removes unwritten attachments. Preserve
// the donor's eligibility, blend precedence and unsupported-state COPY policy.
static void color_logic(pkey *k) {
    int eligible=0, other=0;
    for (unsigned i=0;i<k->ncol;i++) {
        if (clear_color_type(k->cfmt[i])!=CLEAR_FLOAT) k->blend_en &= ~(1u<<i);
        if (!k->cfmt[i] || !k->wmask[i]) continue;
        if (logic_format(k->cfmt[i])) eligible=1; else other=1;
    }
    if (!S.sh[1] || !eligible || k->logic_op==NVN_LOGIC_OP_COPY) return;
    if (other || k->alpha) {
        VKRED_ONCE(115,"logic operation on mixed formats or with alpha testing uses COPY"); return;
    }
    k->blend_en=0;
    k->logic_en=V.has_logic_op;
    if (!k->logic_en) VKRED_ONCE(115,"device lacks logicOp; drawing color with COPY");
}
