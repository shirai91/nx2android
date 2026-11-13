#version 450
layout(set=0, binding=0) uniform sampler2D sourceImage;
layout(push_constant) uniform DrawTexture {
    vec2 dstOrigin;
    vec2 srcOrigin;
    vec2 texelsPerPixel;
} region;
layout(location=0) out vec4 color;

void main() {
    vec2 texel = region.srcOrigin + (gl_FragCoord.xy - region.dstOrigin) * region.texelsPerPixel;
    color = texture(sourceImage, texel / vec2(textureSize(sourceImage, 0)));
}
