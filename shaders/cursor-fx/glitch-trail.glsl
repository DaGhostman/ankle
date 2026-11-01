// Glitch trail: broken copies of the cursor stutter along its path,
// split into red and blue, and snap out of existence.
//
// Written for ankle, to go with glitch-cursor.glsl (from ghostty-cursor-fx); same
// license as ankle (MIT). The glitch.ini preset chains it before the cursor
// effect. A trail plays for DURATION seconds each time the cursor moves at
// least MIN_CELLS cells, so it works with custom-shader-animation=auto.

#define DURATION   0.35
#define MIN_CELLS  2.0
#define GHOSTS     6

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

float inRect(vec2 p, vec2 centre, vec2 size) {
    vec2 d = abs(p - centre) - size * 0.5;
    return step(max(d.x, d.y), 0.0);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    fragColor = texture(iChannel0, fragCoord / iResolution.xy);
    vec2 a, b; float cellH;
    float t = trailTime(a, b, cellH);
    if (t < 0.0) return;

    vec2 size = vec2(max(iCurrentCursor.z, cellH * 0.5), cellH);
    float step_ = floor(iTime * 30.0);
    vec3 add = vec3(0.0);

    for (int i = 0; i < GHOSTS; i++) {
        float fi = float(i);
        float at = (fi + 0.5) / float(GHOSTS);
        // Ghosts nearer the old spot vanish first
        if (t > at * 0.9 + 0.1) continue;
        if (hash(fi * 3.7 + step_) < 0.25) continue;      // stutter

        vec2 p = mix(a, b, at);
        p.x += (hash(fi + step_ * 1.3) - 0.5) * cellH * 0.6;
        p.y += (hash(fi * 2.0 + step_) - 0.5) * cellH * 0.2;
        vec2 s = size * vec2(0.6 + 0.8 * hash(fi * 9.1 + step_), 1.0);

        float split = cellH * 0.12;
        add.r += inRect(fragCoord, p - vec2(split, 0.0), s);
        add.g += inRect(fragCoord, p, s) * 0.5;
        add.b += inRect(fragCoord, p + vec2(split, 0.0), s);
    }

    // Scanline gaps, so the ghosts look torn
    float rows = step(0.35, fract(fragCoord.y / 3.0));
    vec3 c = iCurrentCursorColor.rgb;
    fragColor.rgb += clamp(add, 0.0, 1.0) * mix(vec3(1.0), c, 0.4) * rows * (1.0 - t) * 1.1;
}
