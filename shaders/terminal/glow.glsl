// Glow: a soft bloom around bright text, as if the screen were lit.
//
// Static, so it costs nothing while the terminal is idle:
//
//   [main]
//   custom-shader=/usr/share/ankle/shaders/terminal/glow.glsl
//
// Written for ankle; same license as ankle (MIT).

#define STRENGTH   0.55
#define RADIUS_PX  4.0
#define THRESHOLD  0.25   // only colours brighter than this glow

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 px = 1.0 / iResolution.xy;
    vec2 uv = fragCoord * px;
    vec3 base = texture(iChannel0, uv).rgb;

    vec3 bloom = vec3(0.0);
    float total = 0.0;
    for (int ring = 1; ring <= 3; ring++) {
        float r = float(ring) * RADIUS_PX;
        float w = 1.0 / float(ring);
        for (int i = 0; i < 10; i++) {
            float a = float(i) * 0.6283185 + float(ring);
            vec3 c = texture(iChannel0, uv + vec2(cos(a), sin(a)) * r * px).rgb;
            float bright = max(c.r, max(c.g, c.b));
            bloom += c * smoothstep(THRESHOLD, 1.0, bright) * w;
            total += w;
        }
    }
    bloom /= total;

    fragColor = vec4(base + bloom * STRENGTH * 2.0, 1.0);
}
