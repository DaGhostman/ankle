// Cyber trail: the cursor's path is drawn in small pixels that
// dissolve, block by block, like a digital afterimage.
//
// Written for ankle, to go with cyber-cursor.glsl (from ghostty-cursor-fx); same
// license as ankle (MIT). The cyber.ini preset chains it before the cursor
// effect. A trail plays for DURATION seconds each time the cursor moves at
// least MIN_CELLS cells, so it works with custom-shader-animation=auto.

#define DURATION   0.45
#define MIN_CELLS  2.0
#define PIXEL      0.2    // block size, in character heights

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

    float q = max(floor(cellH * PIXEL), 2.0);
    vec2 id = floor(fragCoord / q);
    vec2 centre = (id + 0.5) * q;

    float h;
    float d = segment(centre, a, b, h);
    float w = cellH * 0.5 * (0.35 + 0.65 * h);
    if (d > w) return;

    // Each block blinks out at its own moment; the old end goes first
    float seed = id.x * 13.1 + id.y * 71.7 + floor(iTime * 20.0);
    float life = hash(seed) * 0.6 + h * 0.4;
    if (life < t) return;

    vec2 inside = fract(fragCoord / q);
    float frame = step(0.12, inside.x) * step(0.12, inside.y);   // gaps between blocks
    vec3 c = iCurrentCursorColor.rgb;
    vec3 col = mix(c * 0.35, c, frame) * (0.7 + 0.3 * hash(seed + 5.0));
    fragColor.rgb = mix(fragColor.rgb, col, 0.75 * (1.0 - t * 0.5));
}
