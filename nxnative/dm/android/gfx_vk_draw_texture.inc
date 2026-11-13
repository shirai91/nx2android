// Existing Android-parity DrawTexture (1134e0e75), adapted to nx2android replay.
static VkRect2D draw_texture_rect(const NVNdrawTextureRegion *d, VkExtent2D size) {
    double left = fmax(0, fmin(d->x0, d->x1)), right = fmin(size.width, fmax(d->x0, d->x1));
    double top = fmax(0, fmin(d->y0, d->y1)), bottom = fmin(size.height, fmax(d->y0, d->y1));
    if (S.sc_set) {
        left = fmax(left, S.sc.x); right = fmin(right, (double)S.sc.x + S.sc.w);
        top = fmax(top, S.sc.y); bottom = fmin(bottom, (double)S.sc.y + S.sc.h);
    }
    if (!(left < right && top < bottom)) return (VkRect2D){0};
    int x = (int)ceil(left - .5), y = (int)ceil(top - .5);
    return (VkRect2D){{x,y},{(uint32_t)((int)ceil(right - .5) - x),(uint32_t)((int)ceil(bottom - .5) - y)}};
}
