// 每次 Draw 共用的变换参数
// row_major 和 Matrix4x4 行布局一致
cbuffer TransformConstants : register(b0)
{
    row_major float4x4 model;
};

// 顶点着色器输出。TEXCOORD 会在矩形内部由光栅化阶段插值后传给像素着色器
struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

VertexOutput main(float3 pos : POSITION, float2 uv : TEXCOORD)
{
    VertexOutput output;

    // model 内已经按 Scale -> Rotate -> Translate 合并了三种变换
    // mul(矩阵, 列向量) 得到最终的齐次坐标位置
    output.position = mul(model, float4(pos, 1.0f));
    output.uv = uv;
    return output;
}
