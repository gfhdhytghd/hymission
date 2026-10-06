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
uniform vec2 uWidths;
uniform float uMaxRadius;
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
    float alpha = min(l, r);
    if (alpha >= 1.0) {
        fragColor = sampleLayer(uTexture, p);
        mirrorColor = uHasMirror ? sampleLayer(uMirror, p) : fragColor;
        return;
    }
    float width = l < r ? uWidths.x : uWidths.y;
    float radius = min(uMaxRadius, width * 0.25) * (1.0 - alpha);
    const float weights[5] = float[5](0.06136, 0.24477, 0.38774, 0.24477, 0.06136);
    vec4 color = vec4(0.0);
    vec4 mirror = vec4(0.0);
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 5; ++x) {
            vec2 point = p + vec2(x - 2, y - 2) * (radius * 0.5);
            color += sampleLayer(uTexture, point) * weights[x] * weights[y];
            if (uHasMirror)
                mirror += sampleLayer(uMirror, point) * weights[x] * weights[y];
        }
    fragColor = color * alpha;
    mirrorColor = uHasMirror ? mirror * alpha : fragColor;
}
)";
} // namespace hymission::stage
