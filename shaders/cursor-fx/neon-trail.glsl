// Neon trail: the tube is dragged across the screen, leaving a glowing
// streak of light that flickers out.
//
// Written for ankle, to go with neon-cursor.glsl (from ghostty-cursor-fx); same
// license as ankle (MIT). The neon.ini preset chains it before the cursor
// effect. A trail plays for DURATION seconds each time the cursor moves at
// least MIN_CELLS cells, so it works with custom-shader-animation=auto.

#define DURATION   0.45
#define MIN_CELLS  2.0
#define GLOW       0.9

float hash(float n) { return fract(sin(n) * 43758.5453); }

// Cursor rectangles are (left, top, width, height), top-left in y-up pixels
vec2 cursorCenter(vec4 r) { return vec2(r.x + r.z * 0.5, r.y - r.w * 0.5); }

// Distance from p to the segment a-b; h is where along it (0 at a, 1 at b)
float segment(vec2 p, vec2 a, vec2 b, out float h) {
    vec2 pa = p - a, ba = b - a;
    h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-4), 0.0, 1.0);
    return length(pa - ba * h);
}

// How far into the trail we are (0 to 1), or -1 when there is none
float trailTime(out vec2 a, out vec2 b, out float cellH) {
    a = cursorCenter(iPreviousCursor);
    b = cursorCenter(iCurrentCursor);
    cellH = iCurrentCursor.w;
    float cellW = cellH * 0.5;
    float t = (iTime - iTimeCursorChange) / DURATION;
    if (iCursorVisible == 0 || iPreviousCursor.w <= 0.0 || t >= 1.0 || t < 0.0
        || length(b - a) < MIN_CELLS * cellW)
        return -1.0;
    return t;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    fragColor = texture(iChannel0, fragCoord / iResolution.xy);
    vec2 a, b; float cellH;
    float t = trailTime(a, b, cellH);
    if (t < 0.0) return;

    vec2 tail = mix(a, b, 1.0 - pow(1.0 - t, 3.0));
    float h;
    float d = segment(fragCoord, tail, b, h);

    float core = smoothstep(cellH * 0.11, cellH * 0.05, d);
    float halo = exp(-d / (cellH * 0.4));
    float along = smoothstep(0.0, 0.7, h);
    float flicker = 0.75 + 0.25 * step(0.35, hash(floor(iTime * 30.0)));
    float k = along * (1.0 - t) * flicker;

    vec3 c = iCurrentCursorColor.rgb;
    fragColor.rgb += (c * halo * GLOW + mix(c, vec3(1.0), 0.6) * core) * k;
}
