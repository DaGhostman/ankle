// Amber monitor: a monochrome phosphor terminal from the early 80s.
//
// Every colour is mapped to the brightness of an amber phosphor, with a
// soft glow, light scanlines and a vignette. Change PHOSPHOR for a green
// (P1) or white (P4) screen instead.
//
// Does not need animation; the default custom-shader-animation is fine:
//
//   [main]
//   custom-shader=/usr/share/ankle/shaders/terminal/amber-monitor.glsl
//
// Written for ankle; same license as ankle (MIT).

#define PHOSPHOR   vec3(1.00, 0.66, 0.16)   // amber
//#define PHOSPHOR vec3(0.25, 1.00, 0.35)   // green
//#define PHOSPHOR vec3(0.85, 0.92, 1.00)   // white
#define DARK_GLOW  0.035                    // unlit phosphor
#define GLOW       0.45
#define SCANLINES  0.18

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 px = 1.0 / iResolution.xy;
    vec2 uv = fragCoord * px;

    // How lit a pixel is: 0 on the background, 1 on normal text. Works
    // the same on light and dark colour schemes.
    float bg = luma(iBackgroundColor);
    float span = max(abs(luma(iForegroundColor) - bg), 0.2);

    float l = clamp(abs(luma(texture(iChannel0, uv).rgb) - bg) / span, 0.0, 1.3);

    float halo = 0.0;
    for (int i = 0; i < 12; i++) {
        float a = float(i) * 0.5235988;
        vec2 d = vec2(cos(a), sin(a));
        vec3 c = texture(iChannel0, uv + d * 3.0 * px).rgb;
        halo += clamp(abs(luma(c) - bg) / span, 0.0, 1.3);
    }
    halo /= 12.0;

    float lit = pow(l, 0.85) + halo * GLOW;
    vec3 col = PHOSPHOR * lit + PHOSPHOR * DARK_GLOW;

    float scan = 0.5 + 0.5 * cos(fragCoord.y * 6.2831853 / 2.0);
    col *= 1.0 - SCANLINES * scan;

    vec2 v = uv * (1.0 - uv.yx);
    col *= pow(v.x * v.y * 16.0, 0.12);

    fragColor = vec4(col, 1.0);
}
