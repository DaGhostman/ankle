// Katana trail: one fast, curved slash of steel from the old spot to
// the new one, with a thin edge of colour.
//
// Written for ankle, to go with katana-cursor.glsl (from ghostty-cursor-fx); same
// license as ankle (MIT). The katana.ini preset chains it before the cursor
// effect. A trail plays for DURATION seconds each time the cursor moves at
// least MIN_CELLS cells, so it works with custom-shader-animation=auto.

#define DURATION   0.22
#define MIN_CELLS  2.0
#define CURVE      0.45   // how much the slash bows, in character heights

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

    vec2 ab = b - a;
    float len = length(ab);
    vec2 dir = ab / len, n = vec2(-dir.y, dir.x);
    if (n.y < 0.0) n = -n;                       // always bow upward

    float along = dot(fragCoord - a, dir) / len;
    float tailAt = 1.0 - pow(1.0 - t, 2.0);       // the tail chases the blade
    if (along < tailAt || along > 1.0) return;

    float bow = CURVE * cellH * sin(3.14159 * along);
    float d = dot(fragCoord - a, n) - bow;

    // A crescent: razor thin at the tail, widest near the blade
    float h = (along - tailAt) / max(1.0 - tailAt, 1e-3);
    float w = cellH * 0.22 * h;
    float blade = smoothstep(w, w * 0.2, abs(d));
    float edge = exp(-abs(d - w * 0.6) / (cellH * 0.05)) * h;

    float k = 1.0 - t * t;
    vec3 steel = vec3(0.92, 0.95, 1.0);
    vec3 c = iCurrentCursorColor.rgb;
    fragColor.rgb = mix(fragColor.rgb, steel, blade * k * 0.85);
    fragColor.rgb += c * edge * k;
}
