// Aurora trail: a soft, wavy ribbon of northern light follows the
// cursor, shifting from green to violet before it fades.
//
// Written for ankle, to go with aurora-cursor.glsl (from ghostty-cursor-fx); same
// license as ankle (MIT). The aurora.ini preset chains it before the cursor
// effect. A trail plays for DURATION seconds each time the cursor moves at
// least MIN_CELLS cells, so it works with custom-shader-animation=auto.

#define DURATION   0.6
#define MIN_CELLS  2.0
#define WIDTH      0.75   // ribbon width, in character heights

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

    float along = dot(fragCoord - a, dir) / len;
    float tailAt = 1.0 - pow(1.0 - t, 2.5);
    if (along < tailAt || along > 1.0) return;

    float x = along * len / cellH;
    float wave = sin(x * 1.3 - iTime * 9.0) * cellH * 0.15;
    float d = abs(dot(fragCoord - a, n) - wave);
    float h = (along - tailAt) / max(1.0 - tailAt, 1e-3);
    float w = WIDTH * cellH * 0.5 * (0.4 + 0.6 * h);

    float ribbon = smoothstep(w, 0.0, d);
    float rays = 0.65 + 0.35 * sin(x * 9.0 + iTime * 5.0);

    vec3 green = vec3(0.2, 1.0, 0.6), cyan = vec3(0.2, 0.75, 1.0), violet = vec3(0.65, 0.35, 1.0);
    vec3 c = h > 0.5 ? mix(cyan, green, h * 2.0 - 1.0) : mix(violet, cyan, h * 2.0);

    fragColor.rgb += c * ribbon * rays * (1.0 - t) * 0.95;
}
