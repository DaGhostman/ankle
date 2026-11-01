// Lightning trail: a jagged bolt arcs from where the cursor was to
// where it is, crackling as it fades.
//
// Written for ankle, to go with lightning-cursor.glsl (from ghostty-cursor-fx); same
// license as ankle (MIT). The lightning.ini preset chains it before the cursor
// effect. A trail plays for DURATION seconds each time the cursor moves at
// least MIN_CELLS cells, so it works with custom-shader-animation=auto.

#define DURATION   0.3
#define MIN_CELLS  2.0
#define JAGGED     0.6    // how far the bolt strays, in character heights

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

float jag(float x, float seed) {
    float i = floor(x), f = fract(x);
    return mix(hash(i * 7.31 + seed), hash((i + 1.0) * 7.31 + seed), f) * 2.0 - 1.0;
}

float bolt(vec2 p, vec2 a, vec2 b, float cellH, float seed, float width) {
    vec2 ab = b - a;
    float len = length(ab);
    vec2 dir = ab / len, n = vec2(-dir.y, dir.x);
    float along = dot(p - a, dir) / len;
    if (along < 0.0 || along > 1.0)
        return 0.0;
    float x = along * len / cellH * 1.4;
    float off = (jag(x, seed) * 0.7 + jag(x * 3.0, seed + 3.0) * 0.3)
              * JAGGED * cellH * sin(3.14159 * along);
    float d = abs(dot(p - a, n) - off);
    return smoothstep(width, width * 0.3, d) + exp(-d / (cellH * 0.25)) * 0.5;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    fragColor = texture(iChannel0, fragCoord / iResolution.xy);
    vec2 a, b; float cellH;
    float t = trailTime(a, b, cellH);
    if (t < 0.0) return;

    // The bolt re-strikes a few times, a new path each time
    float strike = floor(iTime * 24.0);
    float mainBolt = bolt(fragCoord, a, b, cellH, strike, cellH * 0.06);
    float fork = bolt(fragCoord, a, b, cellH, strike + 17.0, cellH * 0.035) * 0.5;

    float k = (1.0 - t) * (0.6 + 0.4 * hash(strike));
    vec3 c = iCurrentCursorColor.rgb;
    vec3 core = mix(c, vec3(1.0), 0.75);
    fragColor.rgb += (core * mainBolt + c * fork) * k;
}
