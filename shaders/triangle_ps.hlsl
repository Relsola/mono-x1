Texture2D texture0 : register(t0);
SamplerState sampler0 : register(s0);

// 输入必须与顶点着色器输出的语义匹配
struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

float4 main(VertexOutput input) : SV_TARGET
{
    // UV 在矩形内部插值；Sample 按该 UV 从 texture0 读出当前像素的颜色
    return texture0.Sample(sampler0, input.uv);
}
