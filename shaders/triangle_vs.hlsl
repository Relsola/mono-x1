// 每次 Draw 共用的变换参数
// row_major 和 Matrix4x4 行布局一致；后三项与 C++ 的 TransformConstants 逐字段对齐
cbuffer TransformConstants : register(b0)
{
    row_major float4x4 model;
    float2 uv_scale;  // 纹理平铺次数（1,1 = 不重复）
    float2 uv_offset; // UV 子矩形的左上角（取多帧条带的一帧时用，否则为 0）
    float4 tint;      // 与采样结果相乘（灰度贴图着色用，1,1,1,1 = 原样输出）
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
    // UV 先乘次数再平移：平铺（uv_offset 恒为 0）与取多帧条带的一帧（乘 1/count）
    // 都是这一个式子，所以两条路共用同一段着色器代码
    output.uv = uv * uv_scale + uv_offset;
    return output;
}
