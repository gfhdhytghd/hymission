#pragma once

namespace hymission::stage {
inline constexpr char edgeVertexShader[] = R"(#version 300 es
void main() {
    vec2 p = vec2(float((gl_VertexID & 1) * 2), float((gl_VertexID & 2)));
    gl_Position = vec4(p - 1.0, 0.0, 1.0);
}
)";

// Input is a premultiplied, transparent window layer in framebuffer orientation.
// The two affine axes map framebuffer pixels to untransformed monitor pixels.
// Never sample beyond the hard clip, even when the Gaussian footprint crosses it.
inline constexpr char edgeFragmentShader[] = R"(#version 300 es
precision highp float;
uniform sampler2D uTexture;
uniform sampler2D uMirror;
uniform bool uHasMirror;
uniform vec2 uSize;
uniform vec3 uAxisX;
uniform vec3 uAxisY;
uniform vec4 uViewport;
uniform vec4 uSample;
uniform vec4 uSampleFB;
uniform vec4 uWidths; // left, right, top, bottom
uniform float uMaxRadius;
uniform int uPass; // 0: horizontal, 1: vertical and opacity
layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 mirrorColor;
vec2 monitorPoint(vec2 p) {
    return vec2(dot(uAxisX, vec3(p, 1.0)), dot(uAxisY, vec3(p, 1.0)));
}
bool inside(vec2 p, vec4 b) {
    return all(greaterThanEqual(p, b.xy)) && all(lessThan(p, b.xy + b.zw));
}
vec4 sampleLayer(sampler2D layer, vec2 p) {
    vec2 m = monitorPoint(p);
    if (!inside(m, uViewport) || !inside(m, uSample))
        return vec4(0.0);
    // Bilinear filtering may otherwise fetch a stale texel just outside the
    // cleared sample rectangle. Clamp to the centers of its interior texels.
    p = clamp(p, uSampleFB.xy + vec2(0.5), uSampleFB.xy + uSampleFB.zw - vec2(0.5));
    return texture(layer, p / uSize);
}
void main() {
    vec2 p = gl_FragCoord.xy;
    vec2 m = monitorPoint(p);
    if (!inside(m, uViewport)) {
        fragColor = vec4(0.0);
        mirrorColor = vec4(0.0);
        return;
    }
    float l = uWidths.x > 0.0 ? smoothstep(0.0, uWidths.x, m.x - uViewport.x) : 1.0;
    float r = uWidths.y > 0.0 ? smoothstep(0.0, uWidths.y, uViewport.x + uViewport.z - m.x) : 1.0;
    float t = uWidths.z > 0.0 ? smoothstep(0.0, uWidths.z, m.y - uViewport.y) : 1.0;
    float b = uWidths.w > 0.0 ? smoothstep(0.0, uWidths.w, uViewport.y + uViewport.w - m.y) : 1.0;
    // Compress opacity into the outer half-band; retain zero at the clip
    // boundary to avoid a hard line alongside the unchanged blur ramp.
    float fadeL = uWidths.x > 0.0 ? smoothstep(0.0, uWidths.x * 0.5, m.x - uViewport.x) : 1.0;
    float fadeR = uWidths.y > 0.0 ? smoothstep(0.0, uWidths.y * 0.5, uViewport.x + uViewport.z - m.x) : 1.0;
    float fadeT = uWidths.z > 0.0 ? smoothstep(0.0, uWidths.z * 0.5, m.y - uViewport.y) : 1.0;
    float fadeB = uWidths.w > 0.0 ? smoothstep(0.0, uWidths.w * 0.5, uViewport.y + uViewport.w - m.y) : 1.0;
    float alpha = uPass == 0 ? 1.0 : min(min(fadeL, fadeR), min(fadeT, fadeB));
    vec4 radii = min(vec4(uMaxRadius), uWidths * 0.5) * (vec4(1.0) - vec4(l, r, t, b));
    float radius = max(max(radii.x, radii.y), max(radii.z, radii.w));
    vec4 color = sampleLayer(uTexture, p);
    vec4 mirror = uHasMirror ? sampleLayer(uMirror, p) : color;
    if (radius > 0.01) {
        // Full-resolution separable Gaussian, with adjacent texels combined
        // using bilinear filtering. Offsets never grow into a sparse grid.
        // Combine edge strengths with max so corners have a continuous blur
        // field. Both passes use monitor axes even on rotated/flipped outputs.
        vec2 direction = uPass == 0 ? uAxisX.xy : uAxisY.xy;
        float sigma = max(radius / 3.0, 0.01);
        float total = 1.0;
        for (int i = 1; i <= 32; i += 2) {
            float a = float(i);
            if (a > ceil(radius)) break;
            float b = a + 1.0;
            float wa = exp(-0.5 * a * a / (sigma * sigma));
            float wb = b <= ceil(radius) ? exp(-0.5 * b * b / (sigma * sigma)) : 0.0;
            float weight = wa + wb;
            if (weight < 0.00001) continue;
            vec2 offset = direction * (a + wb / weight);
            color += (sampleLayer(uTexture, p - offset) + sampleLayer(uTexture, p + offset)) * weight;
            if (uHasMirror)
                mirror += (sampleLayer(uMirror, p - offset) + sampleLayer(uMirror, p + offset)) * weight;
            total += 2.0 * weight;
        }
        color /= total;
        mirror /= total;
    }
    fragColor = color * alpha;
    mirrorColor = uHasMirror ? mirror * alpha : fragColor;
}
)";
} // namespace hymission::stage
