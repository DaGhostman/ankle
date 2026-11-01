// Aurora backdrop: slow northern lights drifting behind the text.
//
// Only the background colour is replaced; text, selections and
// coloured cells stay as they are.
//
//   [main]
//   custom-shader=/usr/share/ankle/shaders/terminal/aurora-backdrop.glsl
//   custom-shader-animation=true
//
// Written for ankle; same license as ankle (MIT).

#define INTENSITY  0.22
#define SPEED      0.08

float wave(vec2 p, float t) {
    return sin(p.x * 3.0 + t) * 0.5
         + sin(p.x * 5.3 - t * 1.3 + p.y * 2.0) * 0.3
         + sin(p.x * 9.1 + t * 0.7) * 0.2;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    vec3 c = texture(iChannel0, uv).rgb;

    // How much this pixel is plain background
    float bg = 1.0 - smoothstep(0.015, 0.06, distance(c, iBackgroundColor));

    float t = iTime * SPEED * 6.2831853;
    vec2 p = vec2(uv.x * iResolution.x / iResolution.y, uv.y);

    vec3 aurora = vec3(0.0);
    for (int i = 0; i < 3; i++) {
        float fi = float(i);
        float center = 0.55 + 0.12 * fi + 0.08 * wave(p * 0.7 + fi, t + fi * 2.1);
        float curtain = exp(-pow((uv.y - center) * (5.0 + fi * 2.0), 2.0));
        float rays = 0.6 + 0.4 * sin(p.x * 40.0 + wave(p, t) * 4.0 + fi);
        vec3 tint = mix(vec3(0.1, 0.9, 0.6), vec3(0.5, 0.3, 1.0), fi / 2.0);
        aurora += tint * curtain * rays;
    }

    // Fade out toward the bottom, where the prompt usually is
    aurora *= smoothstep(0.0, 0.6, uv.y);

    // Dark themes get light added, light themes get it tinted in
    bool light = dot(iBackgroundColor, vec3(0.333)) > 0.5;
    vec3 lit = light ? c * mix(vec3(1.0), aurora * 0.6 + 0.6, INTENSITY * 2.0)
                     : c + aurora * INTENSITY;

    fragColor = vec4(mix(c, lit, bg), 1.0);
}
