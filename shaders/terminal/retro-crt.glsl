// Retro CRT: a curved, slightly worn-out tube.
//
// Barrel distortion, scanlines, an aperture grille, a bit of glow and
// colour fringing, flicker, noise, a dark vignette, and every few
// seconds a bright interference line that rolls down the screen.
//
// The rolling line, flicker and noise need the shader to keep running:
//
//   [main]
//   custom-shader=/usr/share/ankle/shaders/terminal/retro-crt.glsl
//   custom-shader-animation=always
//
// Written for ankle; same license as ankle (MIT).

#define CURVATURE       0.045   // barrel distortion (0 = flat)
#define SCANLINE_PX     3.0     // scanline period, in pixels
#define SCANLINE_DEPTH  0.30    // how dark the gaps between scanlines are
#define MASK_DEPTH      0.12    // aperture grille strength
#define GLOW            0.35    // halo around bright text
#define FRINGE_PX       0.9     // red/blue offset, in pixels
#define ROLL_EVERY      7.0     // seconds between rolling lines
#define ROLL_DURATION   2.2     // seconds a line takes to cross the screen
#define NOISE           0.035
#define FLICKER         0.015

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

vec2 barrel(vec2 uv) {
    vec2 c = uv * 2.0 - 1.0;
    c *= 1.0 + CURVATURE * dot(c, c);
    return c * 0.5 + 0.5;
}

vec3 sample_tube(vec2 uv, vec2 px) {
    vec3 col;
    col.r = texture(iChannel0, uv + vec2(FRINGE_PX, 0.0) * px).r;
    col.g = texture(iChannel0, uv).g;
    col.b = texture(iChannel0, uv - vec2(FRINGE_PX, 0.0) * px).b;
    return col;
}

vec3 glow(vec2 uv, vec2 px) {
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 8; i++) {
        float a = float(i) * 0.785398;
        vec2 d = vec2(cos(a), sin(a));
        sum += texture(iChannel0, uv + d * 2.5 * px).rgb;
        sum += texture(iChannel0, uv + d * 5.0 * px).rgb * 0.5;
    }
    return sum / 12.0;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 px = 1.0 / iResolution.xy;
    vec2 uv = barrel(fragCoord * px);

    // Rolling interference line, top to bottom
    float t = mod(iTime, ROLL_EVERY);
    float roll_pos = 1.15 - (t / ROLL_DURATION) * 1.3;
    float roll = t < ROLL_DURATION
        ? exp(-pow((uv.y - roll_pos) * 28.0, 2.0)) : 0.0;

    // The line tugs the picture sideways as it passes
    uv.x += roll * 0.006 * sin(uv.y * 400.0 + iTime * 40.0);

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 col = sample_tube(uv, px);
    col += glow(uv, px) * GLOW;

    // Scanlines, following the curved picture
    float scan = 0.5 + 0.5 * cos(uv.y * iResolution.y * 6.2831853 / SCANLINE_PX);
    col *= 1.0 - SCANLINE_DEPTH * scan;

    // Aperture grille
    int column = int(mod(fragCoord.x, 3.0));
    vec3 mask = vec3(1.0 - MASK_DEPTH);
    mask[column] = 1.0;
    col *= mask;

    // Interference line, plus a faint slow hum bar
    col += vec3(0.75, 0.85, 0.8) * roll * 0.32;
    col *= 1.0 + 0.035 * sin((uv.y + iTime * 0.12) * 6.2831853);

    // Flicker and noise
    col *= 1.0 + FLICKER * sin(iTime * 110.0);
    col += (hash(fragCoord + fract(iTime) * 100.0) - 0.5) * NOISE;

    // Vignette and rounded corners
    vec2 v = uv * (1.0 - uv.yx);
    col *= pow(v.x * v.y * 16.0, 0.18);
    vec2 corner = abs(uv * 2.0 - 1.0);
    float edge = length(max(corner - 0.96, 0.0)) / 0.04;
    col *= 1.0 - smoothstep(0.7, 1.0, edge);

    // Small boost to make up for the scanlines and mask
    fragColor = vec4(col * 1.18, 1.0);
}
