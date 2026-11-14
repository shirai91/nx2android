#version 450
// Android port: Snapdragon Game Super Resolution 1.0, the edge-direction variant, for the pause menu's Super resolution (gfx_vk_present.c). One pass
// upscales the game's window texture into the window's picture rectangle and sharpens its edges: the base sample is the bilinear value under the
// destination pixel, and the 12 taps are four textureGather reads (a 2x2 block left of it, one right of it, one above and one below), weighted by a
// fast-Lanczos kernel stretched along the edge direction the taps vote for. The taps come from the Adreno texture unit's gather instruction, so the
// whole filter costs one pass and no intermediate image: nothing is written to memory between the game's frame and the swapchain.
// vp: the window texture's texel size and size (1/w, 1/h, w, h), as the shader's ViewportInfo. The sampler is LINEAR with CLAMP_TO_EDGE
// (gfx_vk_present.c sr_ready): the base sample sits between the four texels it snaps to, and the taps at the border repeat the last row or column.
//
// Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// Upstream: SnapdragonGameStudios/snapdragon-gsr, sgsr/v1/include/glsl/sgsr1_shader_mobile_edge_direction.frag (OperationMode 1, RGBA, with
// UseEdgeDirection; the uniform block is this pass's push constant). The BSD 3-clause license is beside this file, sgsr1.LICENSE.
// (the same push constant block as sr.vert: this stage reads vp only)
layout(push_constant) uniform S { vec4 vp; vec4 dst; vec4 uv; float flip; float sharp; } s;
layout(set = 0, binding = 0) uniform sampler2D ps0;
layout(location = 0) in vec4 vTex;
layout(location = 0) out vec4 outColor;

// The pause menu's Sharpness (0..1) moves the two limits the published shader fixes: the edge threshold (8/255: only real edges are
// refined) and the sharpen step (a clamped 2.0 up to the neighbours' range, then at most 23/255). At 1 the filter also refines near-flat
// texture and lets the step past the neighbours' range -- the ringing an emulator's 100% has -- which is what makes cloth and ground
// detail pop. 0 is the shader as published.
#define EdgeThreshold 8.0/255.0
#define EdgeSharpness 2.0

float fastLanczos2(float x) {
    float wA = x - 4.0;
    float wB = x * wA - wA;
    wA *= wA;
    return wB * wA;
}
vec2 weightY(float dx, float dy, float c, vec3 data) {   // data: the taps' deviation scale and the edge direction
    float std = data.x;
    vec2 dir = data.yz;
    float edgeDis = ((dx * dir.y) + (dy * dir.x));
    float x = (((dx * dx) + (dy * dy)) + ((edgeDis * edgeDis) * ((clamp(((c * c) * std), 0.0, 1.0) * 0.7) - 1.0)));
    float w = fastLanczos2(x);
    return vec2(w, w * c);
}
vec2 edgeDirection(vec4 left, vec4 right) {
    vec2 dir;
    float RxLz = (right.x + (-left.z));
    float RwLy = (right.w + (-left.y));
    vec2 delta;
    delta.x = (RxLz + RwLy);
    delta.y = (RxLz + (-RwLy));
    float lengthInv = inversesqrt((delta.x * delta.x + 3.075740e-05) + (delta.y * delta.y));
    dir.x = (delta.x * lengthInv);
    dir.y = (delta.y * lengthInv);
    return dir;
}
void main() {
    vec4 color;
    color.xyz = textureLod(ps0, vTex.xy, 0.0).xyz;
    vec2 imgCoord = ((vTex.xy * s.vp.zw) + vec2(-0.5, 0.5));
    vec2 imgCoordPixel = floor(imgCoord);
    vec2 coord = (imgCoordPixel * s.vp.xy);
    vec2 pl = (imgCoord + (-imgCoordPixel));
    vec4 left = textureGather(ps0, coord, 1);
    // a smooth area starts from the three texels of its own block; only an edge (the base differs from its neighbours) pays for the other taps
    float edgeVote = abs(left.z - left.y) + abs(color.y - left.y) + abs(color.y - left.z);
    if (edgeVote > mix(8.0, 2.0, s.sharp) / 255.0) {   // (EdgeThreshold at 0)
        coord.x += s.vp.x;
        vec4 right = textureGather(ps0, coord + vec2(s.vp.x, 0.0), 1);
        vec4 upDown;
        upDown.xy = textureGather(ps0, coord + vec2(0.0, -s.vp.y), 1).wz;
        upDown.zw = textureGather(ps0, coord + vec2(0.0, s.vp.y), 1).yx;
        float mean = (left.y + left.z + right.x + right.w) * 0.25;
        left = left - vec4(mean);
        right = right - vec4(mean);
        upDown = upDown - vec4(mean);
        color.w = color.y - mean;
        float sum = (((((abs(left.x) + abs(left.y)) + abs(left.z)) + abs(left.w)) + (((abs(right.x) + abs(right.y)) + abs(right.z)) + abs(right.w))) + (((abs(upDown.x) + abs(upDown.y)) + abs(upDown.z)) + abs(upDown.w)));
        float sumMean = 1.014185e+01 / sum;
        float std = (sumMean * sumMean);
        vec3 data = vec3(std, edgeDirection(left, right));
        vec2 aWY = weightY(pl.x, pl.y + 1.0, upDown.x, data);
        aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, data);
        aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, data);
        aWY += weightY(pl.x, pl.y - 2.0, upDown.w, data);
        aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, data);
        aWY += weightY(pl.x, pl.y - 1.0, left.y, data);
        aWY += weightY(pl.x, pl.y, left.z, data);
        aWY += weightY(pl.x + 1.0, pl.y, left.w, data);
        aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, data);
        aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, data);
        aWY += weightY(pl.x - 2.0, pl.y, right.z, data);
        aWY += weightY(pl.x - 1.0, pl.y, right.w, data);
        float finalY = aWY.y / aWY.x;
        float maxY = max(max(left.y, left.z), max(right.x, right.w));
        float minY = min(min(left.y, left.z), min(right.x, right.w));
        float es = mix(EdgeSharpness, 8.0, s.sharp);
        float sharpened = mix(clamp(es * finalY, minY, maxY), es * finalY, s.sharp * 0.35);   // past the range only as far as Sharpness asks
        float deltaY = sharpened - color.w;
        float lim = mix(23.0, 70.0, s.sharp) / 255.0;   // high-contrast input: the sharpen step stays small
        deltaY = clamp(deltaY, -lim, lim);
        color.x = clamp((color.x + deltaY), 0.0, 1.0);
        color.y = clamp((color.y + deltaY), 0.0, 1.0);
        color.z = clamp((color.z + deltaY), 0.0, 1.0);
    }
    outColor = vec4(color.xyz, 1.0);   // the window's picture is opaque: alpha is not used
}