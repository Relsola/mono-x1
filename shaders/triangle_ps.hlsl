Texture2D texture0 : register(t0);
SamplerState sampler0 : register(s0);

// 与顶点着色器同一份常量缓冲（同一个 b0），这里只用 tint
cbuffer TransformConstants : register(b0)
{
    row_major float4x4 model;
    float2 uv_scale;
    float2 uv_offset;
    float4 tint;
};

// 输入必须与顶点着色器输出的语义匹配
struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

float4 main(VertexOutput input) : SV_TARGET
{
    // UV 在矩形内部插值；Sample 按该 UV 从 texture0 读出当前像素的颜色，
    // 再乘上 tint 做逐项着色（灰度贴图 × 调色板颜色 = 那个颜色的图案）
    return texture0.Sample(sampler0, input.uv) * tint;
}
