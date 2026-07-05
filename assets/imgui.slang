// ImGui shader with texture sampling.
// Uses uniform buffer for view-projection, separate texture and sampler.

struct Uniforms
{
    float4x4 view_projection;
}

[[vk::binding(0, 0)]]
ConstantBuffer<Uniforms> u_uniforms;

[[vk::binding(1, 0)]]
Texture2D u_texture;

[[vk::binding(2, 0)]]
SamplerState u_sampler;

struct AssembledVertex
{
    [[vk::location(0)]] float2 position : POSITION;
    [[vk::location(1)]] float2 texcoord : TEXCOORD;
    [[vk::location(2)]] float4 color : COLOR;
}

struct VertexStageOutput
{
    float4 sv_position : SV_Position;
    float2 texcoord;
    float4 color;
}

[shader("vertex")]
VertexStageOutput vertex_main(AssembledVertex v)
{
    VertexStageOutput output;
    output.sv_position = mul(u_uniforms.view_projection, float4(v.position, 0.0, 1.0));
    output.texcoord = v.texcoord;
    output.color = v.color;
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexStageOutput input) : SV_Target
{
    float4 tex_color = u_texture.Sample(u_sampler, input.texcoord);
    return input.color * tex_color;
}
