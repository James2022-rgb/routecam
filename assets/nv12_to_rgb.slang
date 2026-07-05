// Full-screen NV12 -> RGB blit for the Video_MP4Demux playground.
//
// Source plane formats depend on the bit depth of the HEVC stream:
//   - 8-bit Main: plane 0 = R8 luma, plane 1 = R8G8 chroma 4:2:0
//   - 10-bit Main10: plane 0 = R16 luma, plane 1 = R16G16 chroma 4:2:0,
//     with the 10-bit code stored in the upper 10 bits of each
//     16-bit element (the lower 6 bits are zero -- the canonical
//     `R10X6` packing).
// The LINEAR sampler upsamples chroma so each fragment gets a
// co-located Cb/Cr.
//
// Two input pipelines (selected by `u_io_kinds.x`):
//   0 = SDR BT.709 limited-range 8-bit (HVC1 / x264 / typical web).
//       Inverse matrix to gamma-encoded R'G'B', linearized via the
//       sRGB EOTF (close enough to BT.709 EOTF at 8-bit precision).
//   1 = HDR10 BT.2020 NCL limited-range 10-bit, PQ-encoded.
//       Inverse matrix to PQ-encoded R'G'B', linearized via the
//       SMPTE ST.2084 EOTF. Result is linear BT.2020 with values
//       in [0,1] meaning [0, 10000] nits.
//
// Three output paths (selected by `u_io_kinds.y`):
//   0 = sRGB swapchain (`*_SRGB`): write linear (HW does sRGB OETF).
//   1 = SDR UNORM swapchain (`*_UNORM`): apply sRGB OETF in shader.
//   2 = HDR10 ST.2084 swapchain (`A2*10G10R10_UNORM_PACK32`): output
//       PQ-encoded BT.2020. SDR input is gamut-converted from BT.709
//       to BT.2020 and scaled to `kSdrWhiteNits` first; HDR10 input
//       is already in linear BT.2020 nit space.
//
// HDR input + SDR output runs the input through a per-channel ACES
// Narkowicz tonemap (in BT.2020 linear, scaled so SDR diffuse white
// = 1.0) and then a linear BT.2020 -> BT.709 gamut conversion with
// out-of-gamut clipping. The HDR mastering peak is hardcoded to
// `kHdrPeakNits` for now (= 1000 nits, matching typical phone HDR10
// captures); content brighter than that just clips.
//
// Bindings (descriptor set 0):
//   binding 0 = Y plane texture (R8 / R16)
//   binding 1 = CbCr plane texture (R8G8 / R16G16)
//   binding 2 = sampler (LINEAR, ClampToEdge)
//   binding 3 = uniform buffer (source-UV rotation + I/O kinds)

[[vk::binding(0, 0)]] Texture2D u_y_plane;
[[vk::binding(1, 0)]] Texture2D u_cbcr_plane;
[[vk::binding(2, 0)]] SamplerState u_sampler;

// `u_uv_rotation` is the 2x2 matrix applied to (uv - 0.5) before
// sampling, so the 4 cardinal display rotations decoded from the MP4
// `tkhd` matrix become picking the right source pixel: identity for
// 0 deg, [[-1,0],[0,-1]] for 180 deg, etc. Each row padded to 16 bytes
// to match std140-ish UBO layout (Slang/SPIR-V emit float2x2 as two
// vec2 columns each occupying 16 bytes when in a `cbuffer`).
//
// `u_io_kinds.x` is the input pipeline (0 = SDR BT.709 8-bit,
// 1 = HDR10 BT.2020 10-bit). `u_io_kinds.y` is the output path
// (0 = linear into sRGB swapchain, 1 = sRGB OETF, 2 = HDR10 PQ).
//
// `u_quad_scale.xy` shrinks the full-screen quad in clip space to
// preserve the displayed video's aspect ratio against arbitrary
// swapchain dimensions. Driven CPU-side from the video's coded
// extent (with the tkhd rotation factored in) and the swapchain
// extent. Region outside the scaled quad shows the render pass's
// clear value (black) -- letterbox / pillarbox.
//
// `u_hdr_meta.x` is the HDR mastering peak in nits (read from the
// MP4 `mdcv` / `clli` boxes; falls back to the file's MaxCLL or
// to a 1000-nit default when absent). Used as the L_white in the
// HDR -> SDR tonemap.
[[vk::binding(3, 0)]]
cbuffer NV12Ubo {
  float4 u_uv_rotation_row0;
  float4 u_uv_rotation_row1;
  float4 u_io_kinds;
  float4 u_quad_scale;
  float4 u_hdr_meta;
};

struct VertexOutput {
  float4 position : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
};

// Quad from gl_VertexIndex (no vertex buffer needed). Drawn as two
// triangles -- vs the classic one-triangle-with-clip-space-clip trick
// which slants its hypotenuse through the viewport and produces an
// uneven mask once `u_quad_scale.xy` shrinks the geometry below 1.
//   vid 0,1,2 = (0,0), (1,0), (0,1)
//   vid 3,4,5 = (1,0), (1,1), (0,1)
[shader("vertex")]
VertexOutput vertex_main(uint vid : SV_VertexID) {
  static const float2 kQuadUv[6] = {
    float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0),
    float2(1.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
  };
  VertexOutput o;
  o.uv = kQuadUv[vid];
  // The quad spans clip space `[-1, +1] * u_quad_scale.xy`; the
  // remaining margin shows the render pass clear color
  // (= letterbox / pillarbox).
  o.position = float4((o.uv * 2.0 - 1.0) * u_quad_scale.xy, 0.0, 1.0);
  return o;
}

// IEC 61966-2-1 sRGB EOTF: gamma-encoded value in [0,1] -> linear in [0,1].
float SrgbEotf(float v) {
  return (v <= 0.04045) ? (v / 12.92)
                        : pow((v + 0.055) / 1.055, 2.4);
}
float3 SrgbEotf3(float3 v) {
  return float3(SrgbEotf(v.r), SrgbEotf(v.g), SrgbEotf(v.b));
}

// IEC 61966-2-1 sRGB OETF: linear in [0,1] -> gamma-encoded in [0,1].
float SrgbOetf(float l) {
  return (l <= 0.0031308) ? (12.92 * l)
                          : (1.055 * pow(l, 1.0 / 2.4) - 0.055);
}
float3 SrgbOetf3(float3 l) {
  return float3(SrgbOetf(l.r), SrgbOetf(l.g), SrgbOetf(l.b));
}

// ITU-R BT.2087 / BT.2020 NCL primaries conversion, linear-light.
float3 LinearBt709ToBt2020(float3 v) {
  return float3(
    0.6274 * v.r + 0.3293 * v.g + 0.0433 * v.b,
    0.0691 * v.r + 0.9195 * v.g + 0.0114 * v.b,
    0.0164 * v.r + 0.0880 * v.g + 0.8956 * v.b);
}

// Inverse of the above (BT.2020 NCL -> BT.709), linear-light. Some
// resulting channels may go negative for colors outside the BT.709
// gamut; the caller should saturate as the final step.
float3 LinearBt2020ToBt709(float3 v) {
  return float3(
     1.6605 * v.r - 0.5876 * v.g - 0.0728 * v.b,
    -0.1246 * v.r + 1.1329 * v.g - 0.0083 * v.b,
    -0.0182 * v.r - 0.1006 * v.g + 1.1187 * v.b);
}

// Per-channel extended Reinhard tone map with a configurable
// white-point. Maps `x = L_white` exactly to 1.0 and asymptotes to
// 1.0 above; values below L_white compress smoothly without losing
// shadow detail. Saturate is just safety for tiny floating-point
// overshoot.
float3 ReinhardWithWhitepoint(float3 x, float L_white) {
  float L2 = L_white * L_white;
  return saturate(x * (1.0 + x / L2) / (1.0 + x));
}

// HDR mastering peak (in cd/m^2) is supplied per-video via the UBO
// (see u_hdr_meta below); content brighter than that gets clipped
// by the tonemap.

// SMPTE ST.2084 inverse EOTF (= "PQ OETF" for encoding scene/display
// linear nits into the 10-bit PQ signal). Input is normalized so that
// 1.0 == 10000 nits.
float PqOetf(float L_norm) {
  const float m1 = 0.1593017578125;
  const float m2 = 78.84375;
  const float c1 = 0.8359375;
  const float c2 = 18.8515625;
  const float c3 = 18.6875;
  float Lp = pow(max(L_norm, 0.0), m1);
  return pow(max((c1 + c2 * Lp) / (1.0 + c3 * Lp), 0.0), m2);
}
float3 PqOetf3(float3 v) {
  return float3(PqOetf(v.r), PqOetf(v.g), PqOetf(v.b));
}

// SMPTE ST.2084 EOTF: PQ-encoded value in [0,1] -> normalized linear
// nits in [0,1] (1.0 == 10000 nits).
float PqEotf(float pq) {
  const float m1 = 0.1593017578125;
  const float m2 = 78.84375;
  const float c1 = 0.8359375;
  const float c2 = 18.8515625;
  const float c3 = 18.6875;
  float Np = pow(max(pq, 0.0), 1.0 / m2);
  float num = max(Np - c1, 0.0);
  float den = c2 - c3 * Np;
  return pow(num / max(den, 1e-9), 1.0 / m1);
}
float3 PqEotf3(float3 v) {
  return float3(PqEotf(v.r), PqEotf(v.g), PqEotf(v.b));
}

// Reference brightness for "SDR diffuse white" when remapping an SDR
// signal into an HDR10 swapchain. 100 nits is the traditional SDR
// reference; bump if SDR-in-HDR ends up too dim on the target display.
static const float kSdrWhiteNits = 100.0;

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target {
  // Rotate the source-UV around the texture center. For axis-aligned
  // 90/180/270 the matrix never moves the unit square outside [0,1].
  float2 centered = input.uv - 0.5;
  float2 src_uv   = float2(
    u_uv_rotation_row0.x * centered.x + u_uv_rotation_row0.y * centered.y,
    u_uv_rotation_row1.x * centered.x + u_uv_rotation_row1.y * centered.y) + 0.5;

  // Raw samples. R8/R8G8 yields [0,1] from [0,255]; R16/R16G16 (with
  // 10-bit data left-shifted into upper bits) yields [0, ~1] from
  // [0, 1023*64=65472]. The off-by-one-LSB compared to true 10-bit
  // normalization (0/1023..1023/1023) is negligible.
  float  y_s   = u_y_plane.Sample(u_sampler, src_uv).r;
  float2 cbcr  = u_cbcr_plane.Sample(u_sampler, src_uv).rg;

  // input_kind: 0 = SDR BT.709 8-bit, 1 = HDR10 BT.2020 10-bit.
  // output_kind: 0 = linear into sRGB swapchain, 1 = sRGB OETF, 2 = HDR10 PQ.
  const bool is_hdr10_input = (u_io_kinds.x > 0.5);
  const int  output_kind    = (int)(u_io_kinds.y + 0.5);

  // Decode YCbCr -> linear RGB in the source's color space.
  // SDR path:  rgb_linear is BT.709 linear, 1.0 == SDR diffuse white.
  // HDR10 path: rgb_linear is BT.2020 linear, in [0,1] = [0, 10000] nits.
  float3 rgb_linear;
  if (!is_hdr10_input) {
    // BT.709 limited 8-bit: Y in [16,235], Cb/Cr in [16,240].
    float yn = (y_s      - 16.0  / 255.0) * (255.0 / 219.0);
    float cb = (cbcr.r   - 128.0 / 255.0) * (255.0 / 224.0);
    float cr = (cbcr.g   - 128.0 / 255.0) * (255.0 / 224.0);
    float3 rgb_encoded = saturate(float3(
      yn               + 1.5748 * cr,
      yn - 0.1873 * cb - 0.4681 * cr,
      yn + 1.8556 * cb));
    rgb_linear = SrgbEotf3(rgb_encoded);
  } else {
    // BT.2020 NCL limited 10-bit: Y in [64,940]/1023, CbCr in [64,960]/1023.
    // BT.2020 NCL uses Kr=0.2627, Kb=0.0593, Kg=0.678.
    float yn = (y_s    - 64.0  / 1023.0) * (1023.0 / 876.0);
    float cb = (cbcr.r - 512.0 / 1023.0) * (1023.0 / 896.0);
    float cr = (cbcr.g - 512.0 / 1023.0) * (1023.0 / 896.0);
    float3 rgb_pq_encoded = saturate(float3(
      yn               + 1.4747 * cr,
      yn - 0.1646 * cb - 0.5714 * cr,
      yn + 1.8814 * cb));
    rgb_linear = PqEotf3(rgb_pq_encoded);
  }

  // Encode to swapchain. Four input/output combinations:
  //   SDR in,  HDR out:  BT.709 linear -> BT.2020 + 100-nit-scaled PQ.
  //   HDR in,  HDR out:  passthrough (already linear BT.2020 nits).
  //   SDR in,  SDR out:  identity (just pick OETF for the format).
  //   HDR in,  SDR out:  per-channel ACES tonemap in BT.2020, then
  //                      gamut-convert to BT.709 and clip.
  float3 sdr_linear_bt709;
  if (is_hdr10_input) {
    // HDR10 -> SDR tonemap. Scale PQ-norm into "SDR diffuse white = 1.0"
    // multiples (so content brighter than SDR white reads as > 1), then
    // per-channel Reinhard with whitepoint == hdr_peak_nits / kSdrWhiteNits
    // so the mastering peak lands exactly at 1.0. Then BT.2020 -> BT.709
    // and clip out-of-gamut channels.
    float3 hdr_in_sdr_units = rgb_linear * (10000.0 / kSdrWhiteNits);
    float hdr_peak_nits = u_hdr_meta.x;
    float L_white = hdr_peak_nits / kSdrWhiteNits;
    float3 tonemapped_bt2020 = ReinhardWithWhitepoint(hdr_in_sdr_units, L_white);
    sdr_linear_bt709 = saturate(LinearBt2020ToBt709(tonemapped_bt2020));
  } else {
    // SDR input is already linear BT.709, 1.0 = SDR diffuse white.
    sdr_linear_bt709 = rgb_linear;
  }

  float3 rgb_out;
  if (output_kind == 2) {
    // HDR10 PQ output. SDR input -> gamut + 100-nit scale + PQ. HDR10
    // input is already linear BT.2020 in [0,1]=10000 nits, just PQ-encode.
    float3 linear_bt2020_norm = is_hdr10_input
      ? rgb_linear
      : LinearBt709ToBt2020(rgb_linear) * (kSdrWhiteNits / 10000.0);
    rgb_out = PqOetf3(linear_bt2020_norm);
  } else if (output_kind == 1) {
    // SDR `*_UNORM`: shader applies sRGB OETF.
    rgb_out = SrgbOetf3(sdr_linear_bt709);
  } else {
    // SDR `*_SRGB`: write linear, HW does sRGB OETF on store.
    rgb_out = sdr_linear_bt709;
  }

  return float4(rgb_out, 1.0);
}
