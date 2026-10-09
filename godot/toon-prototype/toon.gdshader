shader_type spatial;
render_mode unshaded, cull_back;

uniform vec4 base_color : source_color = vec4(0.22, 0.47, 0.86, 1.0);
uniform vec3 light_direction = vec3(-0.38, 0.72, 0.58);
uniform float shade_strength : hint_range(0.0, 0.5) = 0.22;
uniform bool use_texture = false;
uniform bool use_vertex_color = false;
uniform vec3 shadow_tint = vec3(0.83, 0.86, 1.0);
uniform sampler2D base_texture : source_color, filter_linear_mipmap;
varying vec3 world_normal;

vec3 linear_vertex_to_display(vec3 value) {
    return mix(value * 12.92, 1.055 * pow(max(value, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055,
        step(vec3(0.0031308), value));
}

void vertex() {
    world_normal = normalize(MODEL_NORMAL_MATRIX * NORMAL);
}

void fragment() {
    float light_value = dot(normalize(world_normal), normalize(light_direction));
    float lit = smoothstep(0.08, 0.12, light_value);
    vec3 color = base_color.rgb;
    if (use_texture) {
        color *= texture(base_texture, UV).rgb;
    }
    if (use_vertex_color) {
        // glTF COLOR_0 is linear; Compatibility ALBEDO is in display space.
        color *= OUTPUT_IS_SRGB ? linear_vertex_to_display(COLOR.rgb) : COLOR.rgb;
    }
    // A fixed two-band illustration palette, with a narrow antialiased boundary.
    ALBEDO = color * mix((1.0 - shade_strength) * shadow_tint, vec3(1.0), lit);
}
