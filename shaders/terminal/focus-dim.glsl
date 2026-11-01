// Focus dim: unfocused windows fade to a muted grey, and come back to
// full colour when focused. The fade only animates around focus changes,
// so it works with the default custom-shader-animation=auto.
//
//   [main]
//   custom-shader=/usr/share/ankle/shaders/terminal/focus-dim.glsl
//
// Written for ankle; same license as ankle (MIT).

#define FADE_SECONDS  0.25
#define DESATURATE    0.7
#define DIM           0.7

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    vec3 c = texture(iChannel0, uv).rgb;

    float since = clamp((iTime - iTimeFocus) / FADE_SECONDS, 0.0, 1.0);
    float k = iFocus == 1 ? 1.0 - since : since;
    k = k * k * (3.0 - 2.0 * k);

    float y = dot(c, vec3(0.299, 0.587, 0.114));
    vec3 muted = mix(c, vec3(y), DESATURATE) * DIM;

    fragColor = vec4(mix(c, muted, k), 1.0);
}
