// RouteCam .360 reframe transcode -- Y-plane composer.
//
// Sibling of compose_nv12_to_y.slang for an RGB source: samples the
// reframed EAC view (RGBA8, R'G'B' sRGB-encoded -- the EAC shader's
// output_kind=1 path), alpha-blends the RGBA8 ImGui overlay in R'G'B'
// space, then converts once to BT.709 limited-range Y.
//
// Output: R8, the Y component for one encoded picture.
//
// Bindings (descriptor set 0) -- same layout as compose_nv12_to_y so
// the caller can share plumbing:
//   binding 0 = source RGB view texture (RGBA8)
//   binding 1 = overlay RGBA8 texture (dest UV remapped via
//               u_overlay_rect)
//   binding 2 = sampler (LINEAR, ClampToEdge)
//   binding 3 = uniform buffer (shared 48-byte ComposeUbo layout;
//               the rotation rows are unused -- the reframed view is
//               already upright)

[[vk::binding(0, 0)]] Texture2D u_rgb_view;
[[vk::binding(1, 0)]] Texture2D u_overlay_color;
[[vk::binding(2, 0)]] SamplerState u_sampler;

[[vk::binding(3, 0)]]
cbuffer ComposeUbo {
  float4 u_uv_rotation_row0;  // unused
  float4 u_uv_rotation_row1;  // unused
  float4 u_overlay_rect;
};

struct VertexOutput {
  float4 position : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertex_main(uint vid : SV_VertexID) {
  static const float2 kQuadUv[6] = {
    float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0),
    float2(1.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
  };
  VertexOutput o;
  o.uv = kQuadUv[vid];
  o.position = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
  return o;
}

// BT.709 limited-range RGB -> Y (R'G'B' in [0,1] -> Y in [0,1]).
float RgbToYBt709Limited(float3 rgb) {
  return (16.0 + 65.738 * rgb.r + 129.057 * rgb.g + 25.064 * rgb.b) / 256.0;
}

[shader("fragment")]
float fragment_main(VertexOutput input) : SV_Target {
  float3 rgb = u_rgb_view.Sample(u_sampler, input.uv).rgb;

  float2 rect_min = u_overlay_rect.xy;
  float2 rect_max = u_overlay_rect.zw;
  float2 ov_uv = (input.uv - rect_min) / (rect_max - rect_min);
  if (all(ov_uv >= 0.0) && all(ov_uv <= 1.0)) {
    float4 overlay = u_overlay_color.Sample(u_sampler, ov_uv);
    rgb = lerp(rgb, overlay.rgb, overlay.a);
  }
  return RgbToYBt709Limited(rgb);
}
