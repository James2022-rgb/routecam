// GoPro encode + overlay PoC -- Y-plane composer.
//
// Samples the decoded source Y plane (R8) with a 2x2 UV rotation
// (to bake the source's tkhd display rotation into the encoded
// pixels), then alpha-blends an RGBA8 ImGui-rendered overlay
// directly in YCbCr space (no roundtrip through linear-RGB; the
// RGB->Y conversion is the BT.709 limited-range Kr/Kg/Kb matrix).
//
// Output: R8, the Y component for one encoded picture.
//
// Bindings (descriptor set 0):
//   binding 0 = source Y plane texture (R8)
//   binding 1 = overlay RGBA8 texture, in dest-picture-rect layout
//               (u_overlay_rect picks out the sub-region of the
//               dest picture to remap into [0,1] over the overlay)
//   binding 2 = sampler (LINEAR, ClampToEdge)
//   binding 3 = uniform buffer

[[vk::binding(0, 0)]] Texture2D u_y_plane;
[[vk::binding(1, 0)]] Texture2D u_overlay_color;
[[vk::binding(2, 0)]] SamplerState u_sampler;

[[vk::binding(3, 0)]]
cbuffer ComposeUbo {
  // 2x2 matrix applied to (dest_uv - 0.5) to get source UV.
  float4 u_uv_rotation_row0;
  float4 u_uv_rotation_row1;
  // u_overlay_rect.xy = overlay top-left in normalised dest UV;
  // .zw = overlay bottom-right. Sampled UV inside this rect maps
  // to [0,1] across the overlay texture; outside the rect, the
  // overlay sample is skipped and the picture passes through.
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

// BT.709 limited-range RGB -> Y (R'G'B' in [0,1] sRGB-encoded -> Y in [0,1]).
// Treats ImGui's RGBA8 as sRGB-encoded; no gamma-decode is applied (close
// enough for an alpha-blended overlay, where the eye is forgiving).
float RgbToYBt709Limited(float3 rgb) {
  return (16.0 + 65.738 * rgb.r + 129.057 * rgb.g + 25.064 * rgb.b) / 256.0;
}

[shader("fragment")]
float fragment_main(VertexOutput input) : SV_Target {
  float2 centered = input.uv - 0.5;
  float2 src_uv = float2(
    u_uv_rotation_row0.x * centered.x + u_uv_rotation_row0.y * centered.y,
    u_uv_rotation_row1.x * centered.x + u_uv_rotation_row1.y * centered.y
  ) + 0.5;
  float y_src = u_y_plane.Sample(u_sampler, src_uv).r;

  float2 rect_min = u_overlay_rect.xy;
  float2 rect_max = u_overlay_rect.zw;
  float2 ov_uv = (input.uv - rect_min) / (rect_max - rect_min);
  if (any(ov_uv < 0.0) || any(ov_uv > 1.0)) return y_src;

  float4 overlay = u_overlay_color.Sample(u_sampler, ov_uv);
  if (overlay.a <= 0.0) return y_src;
  float y_overlay = RgbToYBt709Limited(overlay.rgb);
  return lerp(y_src, y_overlay, overlay.a);
}
