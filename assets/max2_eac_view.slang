// Mouse-look viewer for GoPro Max 2 `.360` files.
//
// The `.360` container packs the spherical scene as two HEVC tracks
// containing the 6 cube faces with GoPro's EAC (Equi-Angular Cubemap)
// projection applied per face:
//
//   Track 0 (4096 x 1344) -- [ side(1376) | center(1344) | side(1376) ]
//                            [ LEFT       | BACK         | RIGHT      ]
//   Track 1 (4096 x 1344) -- [ DOWN       | FRONT        | TOP        ]
//
// (FRONT lives in track 1 col 1 despite the trekview Max convention
// naming track 0 col 1 "FRONT" -- empirically the .360 we have stores
// the looking-forward lens in track 1. We accept the convention and
// route the d.y > 0 direction to the face we internally call id 4.)
//
// Each face is rendered onto its plane with an `atan` distortion so
// pixel-to-angle resolution stays uniform; the inverse map is
// `coord = tan(angle)`, equivalently `eac_coord = atan(raw) * 4/pi`.
//
// Per-pixel pipeline:
//   1. Build a camera-space ray from screen NDC + tan(fov/2) + aspect.
//   2. Rotate by the user's view matrix (mouse yaw + pitch on CPU side).
//   3. Pick the cube face the ray hits (largest abs component).
//   4. Project onto that face's plane, get the in-face (u, v) and apply
//      `atan * 4/pi` -- the EAC compression.
//   5. Sample that face's stored region. If the (u, v) lies inside the
//      32 px overlap region near any face boundary, also sample the
//      neighbour face's same world direction and alpha-blend so the
//      cube-face seams disappear.
//
// Reference for the EAC math: trek-view/max2sphere (`max2sphere.c`).

// Bindings ---------------------------------------------------------
//   0 = Track 0 Y plane    (R8)
//   1 = Track 0 CbCr plane (R8G8)
//   2 = Track 1 Y plane    (R8)
//   3 = Track 1 CbCr plane (R8G8)
//   4 = LINEAR sampler (clamp-to-edge)
//   5 = uniform buffer
[[vk::binding(0, 0)]] Texture2D u_track0_y;
[[vk::binding(1, 0)]] Texture2D u_track0_cbcr;
[[vk::binding(2, 0)]] Texture2D u_track1_y;
[[vk::binding(3, 0)]] Texture2D u_track1_cbcr;
[[vk::binding(4, 0)]] SamplerState u_sampler;

[[vk::binding(5, 0)]]
cbuffer ViewUbo {
  // 3x3 view rotation rows. `world_ray = R * camera_ray` with
  // `camera_ray = (ndc.x * fov_t * aspect, 1, ndc.y * fov_t)`.
  float4 u_view_rot_r0;
  float4 u_view_rot_r1;
  float4 u_view_rot_r2;
  // Camera + screen:
  //   .x = tan(fov_vertical / 2)
  //   .y = aspect (= screen_w / screen_h)
  //   .z = sign of NDC.y from VS (-1 if VS NDC y points down)
  //   .w = output_kind (0 = sRGB swapchain HW-OETF, 1 = UNORM SDR OETF)
  float4 u_view_params;
  // Per-column x left-edge in the source frame, normalised.
  float4 u_face_x_left;
  // Per-column x width in the source frame, normalised.
  float4 u_face_x_width;
};

struct VertexOutput {
  float4 position : SV_Position;
  [[vk::location(0)]] float2 ndc : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertex_main(uint vid : SV_VertexID) {
  static const float2 kTri[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0),
  };
  VertexOutput o;
  o.position = float4(kTri[vid], 0.0, 1.0);
  o.ndc      = kTri[vid];
  return o;
}

// sRGB OETF for the UNORM swapchain output path.
float SrgbOetf(float l) {
  return (l <= 0.0031308) ? (12.92 * l)
                          : (1.055 * pow(l, 1.0 / 2.4) - 0.055);
}
float3 SrgbOetf3(float3 l) {
  return float3(SrgbOetf(l.r), SrgbOetf(l.g), SrgbOetf(l.b));
}

// sRGB EOTF (gamma-encoded 8-bit -> linear).
float SrgbEotf(float v) {
  return (v <= 0.04045) ? (v / 12.92)
                        : pow((v + 0.055) / 1.055, 2.4);
}
float3 SrgbEotf3(float3 v) {
  return float3(SrgbEotf(v.r), SrgbEotf(v.g), SrgbEotf(v.b));
}

// Face id assignments. Direction convention: +x right, +y forward, +z up.
//   0 = LEFT   (track 0, col 0)
//   1 = BACK direction  (d.y < 0; sampled from track 0 col 1)
//   2 = RIGHT  (track 0, col 2)
//   3 = DOWN   (track 1, col 0, UV 90 deg CCW)
//   4 = FRONT direction (d.y > 0; sampled from track 1 col 1, anti-transpose)
//   5 = TOP    (track 1, col 2, UV 90 deg CCW)

static const float kFourDivPi = 4.0 / 3.14159265358979323846;
static const float kEps = 1e-5;

// Compute the per-face `raw` in-plane coordinate from a world direction.
// Each face's `raw = (axis_u, axis_v) / abs(normal_component)`. The
// formula is independent of whether the ray actually hits this face --
// for neighbour sampling we compute another face's raw even when the
// direction's dominant axis points elsewhere.
float2 ComputeRawForFace(int face, float3 d) {
  if (face == 0) return float2(-d.y,  d.z) / max(abs(d.x), kEps);  // LEFT
  if (face == 1) return float2( d.x,  d.z) / max(abs(d.y), kEps);  // BACK
  if (face == 2) return float2( d.y,  d.z) / max(abs(d.x), kEps);  // RIGHT
  if (face == 3) return float2( d.x, -d.y) / max(abs(d.z), kEps);  // DOWN
  if (face == 4) return float2( d.x,  d.z) / max(abs(d.y), kEps);  // FRONT
  /* face == 5 */return float2( d.x,  d.y) / max(abs(d.z), kEps);  // TOP
}

// Pick the cube face this direction hits (largest abs component).
// Returns face id + the raw in-plane coordinate for that face.
struct FaceHit {
  int    face;
  float2 raw;
};
FaceHit DirectionToFace(float3 d) {
  FaceHit r;
  float3 ad = abs(d);
  if (ad.x >= ad.y && ad.x >= ad.z) {
    r.face = (d.x < 0.0) ? 0 : 2;
  } else if (ad.y >= ad.z) {
    r.face = (d.y > 0.0) ? 4 : 1;
  } else {
    r.face = (d.z > 0.0) ? 5 : 3;
  }
  r.raw = ComputeRawForFace(r.face, d);
  return r;
}

// raw -> EAC-distorted natural face_uv (in [0, 1], v-flipped so scene
// up lands at the top of the displayed image).
float2 RawToNaturalFaceUv(float2 raw) {
  float2 eac = atan(raw) * kFourDivPi;
  return float2(eac.x * 0.5 + 0.5,
                1.0 - (eac.y * 0.5 + 0.5));
}

// Convert natural face_uv to the storage-axes face_uv (apply the
// per-face UV transform that matches how the file lays out each face's
// pixels in its track frame).
float2 NaturalToStorageFaceUv(int face, float2 nat_uv) {
  if (face == 4) {
    // FRONT (track 1 col 1): anti-transpose (1 - v, 1 - u).
    return float2(1.0 - nat_uv.y, 1.0 - nat_uv.x);
  }
  if (face >= 3) {
    // DOWN / TOP (track 1 col 0 / 2): 90 deg CCW (v, 1 - u).
    return float2(nat_uv.y, 1.0 - nat_uv.x);
  }
  return nat_uv;  // track 0 faces stored in natural axes.
}

// Sample a NV12 track at the given normalised frame UV and decode
// BT.709 limited-range to linear RGB.
float3 SampleTrack0(float2 frame_uv) {
  float  y    = u_track0_y.Sample(u_sampler, frame_uv).r;
  float2 cbcr = u_track0_cbcr.Sample(u_sampler, frame_uv).rg;
  float yn = (y      - 16.0  / 255.0) * (255.0 / 219.0);
  float cb = (cbcr.r - 128.0 / 255.0) * (255.0 / 224.0);
  float cr = (cbcr.g - 128.0 / 255.0) * (255.0 / 224.0);
  float3 enc = saturate(float3(
    yn               + 1.5748 * cr,
    yn - 0.1873 * cb - 0.4681 * cr,
    yn + 1.8556 * cb));
  return SrgbEotf3(enc);
}
float3 SampleTrack1(float2 frame_uv) {
  float  y    = u_track1_y.Sample(u_sampler, frame_uv).r;
  float2 cbcr = u_track1_cbcr.Sample(u_sampler, frame_uv).rg;
  float yn = (y      - 16.0  / 255.0) * (255.0 / 219.0);
  float cb = (cbcr.r - 128.0 / 255.0) * (255.0 / 224.0);
  float cr = (cbcr.g - 128.0 / 255.0) * (255.0 / 224.0);
  float3 enc = saturate(float3(
    yn               + 1.5748 * cr,
    yn - 0.1873 * cb - 0.4681 * cr,
    yn + 1.8556 * cb));
  return SrgbEotf3(enc);
}

// Sample one pixel from the storage frame at the given (col-relative)
// u and (frame-relative) v. `col` is 0, 1, or 2.
float3 SampleFrameAt(int face, int col, float col_u, float frame_v) {
  bool is_track1 = (face >= 3);
  float x_left  = (col == 0) ? u_face_x_left.x
                : (col == 1) ? u_face_x_left.y
                             : u_face_x_left.z;
  float x_width = (col == 0) ? u_face_x_width.x
                : (col == 1) ? u_face_x_width.y
                             : u_face_x_width.z;
  float2 frame_uv = float2(x_left + col_u * x_width, frame_v);
  return is_track1 ? SampleTrack1(frame_uv) : SampleTrack0(frame_uv);
}

// Lens-stitch blend for outer cols (LEFT/RIGHT/DOWN/TOP). GoPro Max's
// two fisheye lenses meet near the middle of each outer cube face --
// the file stores 32 px of duplicate content from each lens on either
// side of the seam. We replicate `max2sphere`'s GetColour() logic for
// these faces: the natural u in [0, 0.5 - 2*duv] stretches into the
// left half of the storage, [0.5 + 2*duv, 1] into the right half,
// and the middle slice [0.5 - 2*duv, 0.5 + 2*duv] alpha-blends the
// two storage candidates so the stitch disappears.
float3 SampleOuterColWithLensBlend(int face, float2 store_uv) {
  const float kDuv     = 32.0 / 1376.0;          // 32 px overlap, 1376 px col
  const float kThreshL = 0.5 - 2.0 * kDuv;
  const float kThreshR = 0.5 + 2.0 * kDuv;

  float u        = store_uv.x;
  float uvleft   = 2.0 * (0.5 - kDuv) * u;
  float uvright  = 2.0 * (0.5 - kDuv) * (u - 0.5) + 0.5 + kDuv;

  int col = (face == 0 || face == 3) ? 0 : 2;  // LEFT/DOWN -> col 0, RIGHT/TOP -> col 2

  if (uvleft <= kThreshL) {
    return SampleFrameAt(face, col, uvleft, store_uv.y);
  } else if (uvright >= kThreshR) {
    return SampleFrameAt(face, col, uvright, store_uv.y);
  } else {
    float3 c1     = SampleFrameAt(face, col, uvleft,  store_uv.y);
    float3 c2     = SampleFrameAt(face, col, uvright, store_uv.y);
    float  alpha  = (uvleft - kThreshL) / (2.0 * kDuv);
    return lerp(c1, c2, alpha);
  }
}

// Full sample pipeline: face id + direction -> linear RGB. Used both
// for the primary face and for neighbours during cube-edge blending.
float3 SampleFaceFromDirection(int face, float3 d) {
  float2 raw       = ComputeRawForFace(face, d);
  float2 nat_uv    = RawToNaturalFaceUv(raw);
  float2 store_uv  = NaturalToStorageFaceUv(face, nat_uv);

  // FRONT and BACK center cols have no internal lens stitch; sample
  // directly. The outer cube faces need the lens-stitch blend above.
  if (face == 1 || face == 4) {
    return SampleFrameAt(face, 1, store_uv.x, store_uv.y);
  }
  return SampleOuterColWithLensBlend(face, store_uv);
}

// For each face, the neighbour face on each of the natural face_uv's
// four edges. Encoded as a 4-int lookup keyed by face id.
//   edge 0 -> nat_uv.x = 0
//   edge 1 -> nat_uv.x = 1
//   edge 2 -> nat_uv.y = 0  (scene-up after v-flip)
//   edge 3 -> nat_uv.y = 1  (scene-down after v-flip)
//
// nat_uv.y was computed as `1 - (eac.y * 0.5 + 0.5)` so that scene up
// (eac.y > 0) maps to the top of the displayed image (nat_uv.y near 0).
// Whatever face lies in the "scene up" direction is therefore the
// y0 neighbour, not the y1 neighbour.
int NeighbourFace(int face, int edge) {
  // Hand-derived from each face's per-axis sign (with -d.y in DOWN's
  // raw.y and +d.y in TOP's raw.y, the y-edge labelling for those two
  // faces ends up inverted relative to the equatorial faces):
  //   LEFT  (0) : x0 -> FRONT(4), x1 -> BACK(1),  y0 -> TOP(5),   y1 -> DOWN(3)
  //   BACK  (1) : x0 -> LEFT(0),  x1 -> RIGHT(2), y0 -> TOP(5),   y1 -> DOWN(3)
  //   RIGHT (2) : x0 -> BACK(1),  x1 -> FRONT(4), y0 -> TOP(5),   y1 -> DOWN(3)
  //   DOWN  (3) : x0 -> LEFT(0),  x1 -> RIGHT(2), y0 -> BACK(1),  y1 -> FRONT(4)
  //   FRONT (4) : x0 -> LEFT(0),  x1 -> RIGHT(2), y0 -> TOP(5),   y1 -> DOWN(3)
  //   TOP   (5) : x0 -> LEFT(0),  x1 -> RIGHT(2), y0 -> FRONT(4), y1 -> BACK(1)
  if (face == 0) { if (edge == 0) return 4; if (edge == 1) return 1; if (edge == 2) return 5; return 3; }
  if (face == 1) { if (edge == 0) return 0; if (edge == 1) return 2; if (edge == 2) return 5; return 3; }
  if (face == 2) { if (edge == 0) return 1; if (edge == 1) return 4; if (edge == 2) return 5; return 3; }
  if (face == 3) { if (edge == 0) return 0; if (edge == 1) return 2; if (edge == 2) return 1; return 4; }
  if (face == 4) { if (edge == 0) return 0; if (edge == 1) return 2; if (edge == 2) return 5; return 3; }
  /*face == 5*/   { if (edge == 0) return 0; if (edge == 1) return 2; if (edge == 2) return 4; return 1; }
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target {
  // Camera ray. NDC.y sign flips per the device's clip-space Y direction.
  float ndc_y_sign = u_view_params.z;
  float fov_t      = u_view_params.x;
  float aspect     = u_view_params.y;
  float3 cam_ray   = float3(
    input.ndc.x * fov_t * aspect,
    1.0,
    -ndc_y_sign * input.ndc.y * fov_t);
  float3 world_ray = float3(
    dot(u_view_rot_r0.xyz, cam_ray),
    dot(u_view_rot_r1.xyz, cam_ray),
    dot(u_view_rot_r2.xyz, cam_ray));
  world_ray = normalize(world_ray);

  FaceHit f = DirectionToFace(world_ray);
  float2 nat_uv = RawToNaturalFaceUv(f.raw);

  float3 rgb_linear = SampleFaceFromDirection(f.face, world_ray);

  // Edge blend zone. The .360 stores 32 px of overlap on each face's
  // boundary inside the 1344-natural-area face; we use a slightly wider
  // zone so the alpha ramp tapers out before reaching the very edge of
  // the natural area (= cleanest visual seam).
  const float kBlendW = 48.0 / 1344.0;

  // x-axis neighbour blend. The half-weight 0.5 means at the exact
  // face edge the two faces blend 50/50; deeper inside the current
  // face the neighbour's contribution fades to zero.
  float dist_x0 = nat_uv.x;
  float dist_x1 = 1.0 - nat_uv.x;
  if (dist_x0 < kBlendW) {
    int   n     = NeighbourFace(f.face, 0);
    float3 rgb_n = SampleFaceFromDirection(n, world_ray);
    float  a    = 0.5 * (1.0 - dist_x0 / kBlendW);
    rgb_linear  = lerp(rgb_linear, rgb_n, a);
  } else if (dist_x1 < kBlendW) {
    int   n     = NeighbourFace(f.face, 1);
    float3 rgb_n = SampleFaceFromDirection(n, world_ray);
    float  a    = 0.5 * (1.0 - dist_x1 / kBlendW);
    rgb_linear  = lerp(rgb_linear, rgb_n, a);
  }

  // y-axis neighbour blend (applied on top of any x-axis blend; corner
  // regions get blended twice which is a slight over-blur but avoids
  // 3-way logic).
  float dist_y0 = nat_uv.y;
  float dist_y1 = 1.0 - nat_uv.y;
  if (dist_y0 < kBlendW) {
    int   n     = NeighbourFace(f.face, 2);
    float3 rgb_n = SampleFaceFromDirection(n, world_ray);
    float  a    = 0.5 * (1.0 - dist_y0 / kBlendW);
    rgb_linear  = lerp(rgb_linear, rgb_n, a);
  } else if (dist_y1 < kBlendW) {
    int   n     = NeighbourFace(f.face, 3);
    float3 rgb_n = SampleFaceFromDirection(n, world_ray);
    float  a    = 0.5 * (1.0 - dist_y1 / kBlendW);
    rgb_linear  = lerp(rgb_linear, rgb_n, a);
  }

  // Output encoding: 0 = sRGB swapchain (linear, HW OETF on store),
  // 1 = SDR UNORM swapchain (apply sRGB OETF here).
  int output_kind = (int)(u_view_params.w + 0.5);
  float3 rgb_out  = (output_kind == 1) ? SrgbOetf3(rgb_linear)
                                       : rgb_linear;
  return float4(rgb_out, 1.0);
}
