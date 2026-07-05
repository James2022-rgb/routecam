// RouteCam .360 reframe transcode -- CbCr-plane composer.
//
// Sibling of compose_rgb_to_y.slang. Same overlay blend in R'G'B'
// space, runs at half luma resolution (4:2:0), outputs interleaved
// Cb/Cr in BT.709 limited range.
//
// Bindings (descriptor set 0): identical to compose_rgb_to_y.

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

// BT.709 limited-range RGB -> CbCr (R'G'B' in [0,1] -> Cb,Cr in [0,1]).
float2 RgbToCbCrBt709Limited(float3 rgb) {
  float cb = (128.0 + (-37.945 * rgb.r - 74.494 * rgb.g + 112.439 * rgb.b)) / 256.0;
  float cr = (128.0 + ( 112.439 * rgb.r - 94.154 * rgb.g -  18.285 * rgb.b)) / 256.0;
  return float2(cb, cr);
}

[shader("fragment")]
float2 fragment_main(VertexOutput input) : SV_Target {
  float3 rgb = u_rgb_view.Sample(u_sampler, input.uv).rgb;

  float2 rect_min = u_overlay_rect.xy;
  float2 rect_max = u_overlay_rect.zw;
  float2 ov_uv = (input.uv - rect_min) / (rect_max - rect_min);
  if (all(ov_uv >= 0.0) && all(ov_uv <= 1.0)) {
    float4 overlay = u_overlay_color.Sample(u_sampler, ov_uv);
    rgb = lerp(rgb, overlay.rgb, overlay.a);
  }
  return RgbToCbCrBt709Limited(rgb);
}
