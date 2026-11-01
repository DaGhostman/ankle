// Ember trail: the cursor sheds sparks along its path, which drift
// up, flicker and burn out.
//
// Written for ankle, to go with ember-cursor.glsl (from ghostty-cursor-fx); same
// license as ankle (MIT). The embers.ini preset chains it before the cursor
// effect. A trail plays for DURATION seconds each time the cursor moves at
// least MIN_CELLS cells, so it works with custom-shader-animation=auto.

#define DURATION   0.9
#define MIN_CELLS  2.0
#define SPARKS     40

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

    float seed = fract(iTimeCursorChange * 0.731) * 100.0;
    vec3 glow = vec3(0.0);
    for (int i = 0; i < SPARKS; i++) {
        float fi = float(i) + seed;
        float where = hash(fi * 1.7);
        float delay = where * 0.25;          // sparks near the old spot go first
        float age = clamp((t - delay) / (1.0 - delay), 0.0, 1.0);
        if (age <= 0.0 || age >= 1.0)
            continue;

        vec2 p = mix(a, b, where);
        p += (vec2(hash(fi * 3.1), hash(fi * 5.3)) - 0.5) * cellH * 0.5;
        p.y += age * cellH * (0.8 + hash(fi * 2.9) * 1.2);          // rise
        p.x += sin(age * 7.0 + fi) * cellH * 0.15;                  // sway

        float size = cellH * (0.06 + 0.07 * hash(fi * 4.7)) * (1.0 - age * 0.6);
        float d = length(fragCoord - p);
        float spark = smoothstep(size, 0.0, d) + exp(-d / (size * 2.5)) * 0.35;
        float twinkle = 0.6 + 0.4 * sin(iTime * 40.0 + fi * 13.0);

        vec3 hot = vec3(1.0, 0.85, 0.45), cool = vec3(0.9, 0.2, 0.05);
        glow += mix(hot, cool, age) * spark * twinkle * (1.0 - age);
    }
    fragColor.rgb += glow * 1.5;
}
