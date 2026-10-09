#pragma once

#include "core.h"
#include "renderer.h"
#include "game.h"
#include "debug/debug_vis.h" // DEBUG_BOX_COUNT：SceneTextures 里那张调试盒贴图表要按它定长

// ============================================================================
// 场景装配：把 GameState 翻译成渲染层的绘制列表
//
// 为什么单独一层：装配层（main.cc）的 wWinMain 里塞了 250 行「场景图遍历」——
// 窗口/输入/主循环与「这一帧画什么」混在一个函数里，读哪一个都得先扒开另一个。
// 拆出来之后 main.cc 只剩「装配与主循环」，这里只剩「画什么」。
//
// 它仍是**装配层**（不是渲染层）：只调 renderer.h 的入口、只读 GameState，
// 不认识 D3D11 类型；也不认识输入与主循环。
//
// 依赖方向：scene.cc -> renderer.h + game.h + font.h + debug/debug_vis.h
// ============================================================================

// 场景用到的全部纹理句柄。
//
// 为什么收成一张表：以前它们是 wWinMain 里二十来个散落的局部变量，靠「都在同一个作用域」
// 隐式地连在一起 —— 拆成两个函数之后，它们必须显式地传递，那就顺手把「有哪些贴图」变成一处可见的数据。
// 关卡与角色自己的贴图不在这里（它们住在 GameState 里：backdrop / player_bagdown_animation）。
struct SceneTextures
{
    TextureHandle platform;
    TextureHandle spike;
    // 可消失平台的三态各一张，下标 = VanishPhase
    TextureHandle vanish[VANISH_PHASE_COUNT];
    TextureHandle portal;
    TextureHandle white;
    TextureHandle loading;
    TextureHandle font;
    TextureHandle monster;
    TextureHandle mover_circle;
    TextureHandle projectile;
    TextureHandle time_stop;
    TextureHandle time_stop_outline;
#if MONO_DEBUG_VIS
    TextureHandle debug_box[DEBUG_BOX_COUNT];
#endif
};

// 一帧的渲染输入。收成一个结构体而不是 6 个位置参数：这里的每一项都是「一帧的上下文」，
// 分开传的时候调用点是一串看不出含义的量（谁是宽谁是高、谁是 alpha）。
struct SceneFrame
{
    GameState *game_state;
    f32 alpha;       // 渲染插值系数（0 = 上一逻辑步，1 = 当前逻辑步）
    f32 camera_zoom; // 渲染相机的缩放（探针色带按设备像素给厚度，要靠它换算成世界长度）
    u32 client_width;
    u32 client_height;
};

// 上传全部贴图：关卡/角色自己的贴图（句柄写回 GameState）与程序化生成的占位贴图。
// 必须在 arena 与 scratch 就绪之后、主循环之前调一次。
void scene_init(Renderer *renderer, SceneTextures *textures, GameState *game_state);

// 提交这一帧的绘制列表（世界物件 → 调试可视化 → UI）。
// 调用前 renderer_frame_begin 必须已经设好相机；调用后由 main.cc 收尾（renderer_frame_end）。
void scene_submit(Renderer *renderer, const SceneTextures *textures, const SceneFrame *frame);
