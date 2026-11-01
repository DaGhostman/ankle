// VHS: a tape that has been rewound a few too many times.
//
// Colour bleeding, a wobbling picture, a tracking band that drifts up
// the screen, head-switching noise along the bottom edge and, now and
// then, a burst of torn lines.
//
//   [main]
//   custom-shader=/usr/share/ankle/shaders/terminal/vhs.glsl
//   custom-shader-animation=always
//
// Written for ankle; same license as ankle (MIT).

#define WOBBLE        0.0012
#define BLEED_PX      2.5
#define TRACKING      1.0
#define GLITCH_EVERY  5.0     // seconds between glitch bursts
#define NOISE         0.05

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 px = 1.0 / iResolution.xy;
    vec2 uv = fragCoord * px;
    float line = floor(fragCoord.y);

    // Gentle wobble of the whole picture
    uv.x += WOBBLE * sin(uv.y * 14.0 + iTime * 1.7)
          + WOBBLE * 0.5 * sin(uv.y * 90.0 + iTime * 9.0);

    // Tracking band, slowly drifting up
    float band_pos = fract(iTime * 0.06);
    float band = exp(-pow((uv.y - band_pos) * 18.0, 2.0)) * TRACKING;
    uv.x += band * (hash(vec2(line, floor(iTime * 24.0))) - 0.5) * 0.02;

    // Glitch burst: random strips get torn sideways
    float burst = step(mod(iTime, GLITCH_EVERY), 0.18);
    float strip = floor(uv.y * 24.0);
    float tear = step(0.72, hash(vec2(strip, floor(iTime * 30.0))));
    uv.x += burst * tear * (hash(vec2(strip, iTime)) - 0.5) * 0.08;

    // Head-switching noise at the bottom
    float head = 1.0 - smoothstep(0.0, 0.025, uv.y);
    uv.x += head * (hash(vec2(line, iTime)) - 0.5) * 0.05;

    // Luma stays sharp, chroma bleeds sideways
    vec3 col = vec3(0.0);
    float w = 0.0;
    for (int i = -3; i <= 3; i++) {
        float k = 1.0 - abs(float(i)) / 4.0;
        col += texture(iChannel0, uv + vec2(float(i) * BLEED_PX, 0.0) * px).rgb * k;
        w += k;
    }
    col /= w;
    vec3 sharp = texture(iChannel0, uv).rgb;
    float y_sharp = dot(sharp, vec3(0.299, 0.587, 0.114));
    float y_soft = dot(col, vec3(0.299, 0.587, 0.114));
    col += y_sharp - y_soft;

    // Red and blue never quite line up
    col.r = mix(col.r, texture(iChannel0, uv + vec2(3.0, 0.0) * px).r, 0.5);
    col.b = mix(col.b, texture(iChannel0, uv - vec2(3.0, 0.0) * px).b, 0.5);

    // Washed out, slightly warm
    float y = dot(col, vec3(0.299, 0.587, 0.114));
    col = mix(vec3(y), col, 0.85) * vec3(1.03, 1.0, 0.95) + 0.02;

    // Noise, stronger in the tracking band and at the bottom
    float n = hash(fragCoord + fract(iTime * 7.0) * 100.0);
    col += (n - 0.5) * (NOISE + band * 0.25 + head * 0.4);
    col += band * 0.06;

    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
