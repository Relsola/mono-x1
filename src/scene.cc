#include "scene.h"

#include "sprite.h"
#include "shared/memory.h"
#include "font.h"
#include "debug/debug_vis.h"
#include "shared/mono_math.h"

// ============================================================================
// 场景装配：程序化占位贴图 + 每帧的绘制提交
//
// 这里只做两件事：把「要画什么」写成 renderer_push_* 调用，以及生成那几张还没有美术资源的
// 占位贴图（砖块 / 地刺 / 门 / 圆盘 / 加载动画 / 时停蒙版）。两者都是装配层的活 ——
// 渲染层不认识游戏概念，游戏逻辑层不认识渲染层。
// ============================================================================

// ----------------------------------------------------------------------------
// 调试盒配色（scene_init 要用它给每种标签建一张纯色贴图）
// ----------------------------------------------------------------------------

#if MONO_DEBUG_VIS
// 每种调试碰撞盒一个颜色，一眼看出语义。
// 数组不定长 + static_assert：新增标签忘了加颜色时是编译错误；
// 否则越界那一项会被零初始化成 alpha = 0 的「隐形线框」，什么都不报。
constexpr u8 DEBUG_BOX_COLORS[][4] = {
    { 0, 200, 255, 255 },   // 青：关卡实体
    { 120, 255, 120, 255 }, // 绿：单向平台
    { 255, 80, 80, 255 },   // 红：地刺
    { 255, 255, 255, 180 }, // 白：传送门矩形
    { 255, 150, 40, 255 },  // 橙：移动组件（含圆形）
    { 230, 80, 170, 255 },  // 粉：怪物伤害盒
    { 80, 230, 255, 255 },  // 青蓝：能量波
    { 255, 230, 0, 255 },   // 黄：玩家物理盒
    { 255, 0, 200, 200 },   // 洋红：地面探针（半透明色带，它是色带不是线框，理由见绘制处）
};
static_assert(array_size(DEBUG_BOX_COLORS) == DEBUG_BOX_COUNT, "DEBUG_BOX_COLORS must cover every DebugBoxTag");

// 探针 = 玩家盒整体下移 GROUND_PROBE_DEPTH（1 像素），画成线框会和玩家盒重合（看不见）：
// 它在装配层被画成玩家脚底那一条**色带**（底边不动）。厚度按**屏幕比例**给：
// 调试标记看不看得见取决于它在屏幕上占多大，而 4K 全屏下整幅关卡都在视野里、
// 玩家盒也只有 90 设备像素高 —— 固定像素厚度在那个尺度下就细得看不见了。
constexpr f32 DEBUG_PROBE_HEIGHT_RATIO = 1.0f / 180.0f; // 视口高度的 1/180
constexpr f32 DEBUG_PROBE_MIN_HEIGHT_PX = 6.0f;         // 小窗口下的下限（设备像素）
// 色带先画，否则会盖在玩家盒的黄线框上面
constexpr int DEBUG_PROBE_DRAW_ORDER = -1;
#endif

// ----------------------------------------------------------------------------
// 程序化纹理
// ----------------------------------------------------------------------------

// 可消失平台的三态外观参数：都是「同一块砖」的不同参数，所以它看起来始终像同一块地板在
// 变暗、变虚，而不是换了材质。三个数值集中在这里，调整手感/可读性只改这一处。
//   实体态 = 和普通平台同贴图；警告态（已踩上、正在倒计时）= 变暗；消失态 = 半透明。
inline constexpr f32 VANISH_DIM_BRIGHTNESS = 0.45f;
inline constexpr u8 VANISH_DIM_ALPHA = 255;
inline constexpr f32 VANISH_GHOST_BRIGHTNESS = 0.65f;
inline constexpr u8 VANISH_GHOST_ALPHA = 80;

// 程序化生成的石砖贴图（32x32），用于平台可视化（配合 WRAP 采样器平铺）。
// brightness 缩放砖块颜色、alpha 控制整体透明度（可消失平台的三态就是同一张图的三组参数）。
internal TextureHandle create_brick_texture(Renderer *renderer, f32 brightness, u8 alpha)
{
    constexpr u32 size = 32;
    u32 pixels[size * size];

    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            // 边缘与接缝深灰色，砖块表面浅石灰色
            bool is_border = (x == 0 || x == size - 1 || y == 0 || y == size - 1 || y == 16 || (y < 16 && x == 16));
            u8 base_r = is_border ? 40 : 130;
            u8 base_g = is_border ? 45 : 135;
            u8 base_b = is_border ? 55 : 145;
            u8 r = (u8)((f32)base_r * brightness);
            u8 g = (u8)((f32)base_g * brightness);
            u8 b = (u8)((f32)base_b * brightness);
            pixels[y * size + x] = ((u32)alpha << 24) | ((u32)b << 16) | ((u32)g << 8) | (u32)r;
        }
    }

    return renderer_create_texture(renderer, pixels, (int)size, (int)size);
}

// 1x1 纯色贴图（碰撞线框颜色）
internal TextureHandle create_solid_color_texture(Renderer *renderer, u8 r, u8 g, u8 b, u8 a)
{
    u32 pixel = ((u32)a << 24) | ((u32)b << 16) | ((u32)g << 8) | (u32)r;
    return renderer_create_texture(renderer, &pixel, 1, 1);
}

// 传送门的占位贴图（64x128 灰度）：逐项 tint 上色后，一对门就是同一个形状的两种颜色。
// 灰度的取法（亮边框 + 中灰门板）就是为了“乘上颜色”后还看得清结构 —— 乘色只会变暗不会变亮，
// 所以内部不能画得太黑。真正的美术资源接进来时，只要还是灰度图，这里换一张就行。
internal TextureHandle create_portal_texture(Renderer *renderer)
{
    constexpr u32 width = 64;
    constexpr u32 height = 128;
    constexpr u32 border = 4;
    u32 pixels[width * height] = {};

    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            bool is_border = (x < border || x >= width - border || y < border || y >= height - border);
            // 中缝（门缝）与横向门杨：给一个比门板更亮的细条，避免整张图只看得出一个方块
            bool is_panel = (y == height / 2) && (x >= border && x < width - border);
            u8 value = is_border ? 235 : (is_panel ? 210 : 130);
            pixels[y * width + x] = 0xFF000000u | ((u32)value << 16) | ((u32)value << 8) | (u32)value;
        }
    }

    return renderer_create_texture(renderer, pixels, (int)width, (int)height);
}

// 圆形移动组件的占位贴图（128x128 灰度 + 透明外圈）：亮边缘 + 中灰内部。
// 与门贴图同一个思路 —— 灰度乘上 tint 就能换颜色（乘色只会变暗不会变亮，所以内部不能太黑）。
// 圆外的像素 alpha = 0：它的碰撞盒是方的，靠 alpha 把画面切成圆。
internal TextureHandle create_mover_circle_texture(Renderer *renderer)
{
    constexpr u32 size = 128;
    constexpr f32 center = ((f32)size - 1.0f) * 0.5f;
    constexpr f32 radius = center - 1.0f;
    constexpr f32 rim = 6.0f;
    u32 pixels[size * size] = {};

    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            f32 dx = (f32)x - center;
            f32 dy = (f32)y - center;
            f32 distance = sqrtf(dx * dx + dy * dy);
            if (distance > radius) {
                continue;
            }
            u8 value = (distance > radius - rim) ? 240 : 150;
            pixels[y * size + x] = 0xFF000000u | ((u32)value << 16) | ((u32)value << 8) | (u32)value;
        }
    }

    return renderer_create_texture(renderer, pixels, (int)size, (int)size);
}

// 加载动画的条带（2 帧 × 64x64 横排）：两帧差 45°，交替播就是“转起来”的观感。
// 贴图本身是**白点**（不染色）：想换颜色就在绘制处改 tint，不必重新生成贴图。
internal TextureHandle create_loading_texture(Renderer *renderer)
{
    constexpr u32 frame_size = 64;
    constexpr u32 frame_count = 2;
    constexpr u32 dot_half = 6;
    constexpr f32 dot_radius = 22.0f;
    constexpr u32 width = frame_size * frame_count;
    u32 pixels[width * frame_size] = {};

    for (u32 frame = 0; frame < frame_count; ++frame) {
        f32 center = ((f32)frame_size - 1.0f) * 0.5f;
        f32 phase = ((f32)frame / (f32)frame_count) * PI * 0.5f; // 45°：四颗点转到下一个刻度
        for (u32 y = 0; y < frame_size; ++y) {
            for (u32 x = 0; x < frame_size; ++x) {
                f32 dx = (f32)x - center;
                f32 dy = (f32)y - center;
                bool filled = false;
                for (u32 dot = 0; dot < 4 && !filled; ++dot) {
                    f32 angle = phase + (f32)dot * PI * 0.5f;
                    f32 dot_x = cosf(angle) * dot_radius;
                    f32 dot_y = sinf(angle) * dot_radius;
                    filled = fabsf(dx - dot_x) <= (f32)dot_half && fabsf(dy - dot_y) <= (f32)dot_half;
                }
                if (filled) {
                    pixels[y * width + frame * frame_size + x] = 0xFFFFFFFFu;
                }
            }
        }
    }

    return renderer_create_texture(renderer, pixels, (int)width, (int)frame_size);
}

// 横向多帧条带取第 index 帧：UV 缩到 1/count、平移到 index/count（约定条带横排，见 SpriteStyle）
internal SpriteStyle sprite_strip_frame(u32 index, u32 count)
{
    SpriteStyle style = {};
    if (count > 1) {
        style.uv_scale_x = 1.0f / (f32)count;
        style.uv_offset_x = (f32)(index % count) * style.uv_scale_x;
    }
    return style;
}

// 圆形范围使用一张带 4px 柔边的半透明灰白贴图。它在世界物体之后、玩家之前绘制，
// 因而范围内除主角外的内容都会染上蒙版。
internal TextureHandle create_time_stop_texture(Renderer *renderer)
{
    constexpr u32 size = 128;
    constexpr f32 center = ((f32)size - 1.0f) * 0.5f;
    constexpr f32 radius = center - 2.0f;
    constexpr f32 soft_edge = 4.0f;
    u32 pixels[size * size] = {};

    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            f32 dx = (f32)x - center;
            f32 dy = (f32)y - center;
            f32 distance = sqrtf(dx * dx + dy * dy);
            f32 alpha = 0.0f;
            if (distance <= radius - soft_edge) {
                alpha = 72.0f;
            } else if (distance < radius) {
                alpha = (radius - distance) * (72.0f / soft_edge);
            }
            u8 a = (u8)alpha;
            pixels[y * size + x] = ((u32)a << 24) | 0x00E8E8E0u;
        }
    }

    return renderer_create_texture(renderer, pixels, (int)size, (int)size);
}

// 与蒙版共用同一半径的圆形边界。透明内区让它只承担“范围到哪里”的可视化职责。
internal TextureHandle create_time_stop_outline_texture(Renderer *renderer)
{
    constexpr u32 size = 128;
    constexpr f32 center = ((f32)size - 1.0f) * 0.5f;
    constexpr f32 radius = center - 2.0f;
    constexpr f32 thickness = 2.0f;
    u32 pixels[size * size] = {};

    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            f32 dx = (f32)x - center;
            f32 dy = (f32)y - center;
            f32 distance = sqrtf(dx * dx + dy * dy);
            f32 edge_distance = fabsf(distance - radius);
            if (edge_distance < thickness) {
                f32 alpha = (thickness - edge_distance) * (180.0f / thickness);
                u8 a = (u8)alpha;
                pixels[y * size + x] = ((u32)a << 24) | 0x00F4F4EEu;
            }
        }
    }

    return renderer_create_texture(renderer, pixels, (int)size, (int)size);
}

void scene_init(Renderer *renderer, SceneTextures *textures, GameState *game_state)
{
    // 关卡与角色自己的贴图：句柄写回 GameState（渲染层持有 GPU 资源，游戏侧只留句柄）
    game_state->backdrop.texture = renderer_create_texture(renderer, game_state->backdrop.pixels,
                                                           game_state->backdrop.width,
                                                           game_state->backdrop.height);
    for (u32 i = 0; i < game_state->player_bagdown_animation.frame_count; ++i) {
        SpriteImage *frame = &game_state->player_bagdown_animation.frames[i].image;
        frame->texture = renderer_create_texture(renderer, frame->pixels, frame->width, frame->height);
    }

    // 石砖贴图 brick
    auto spike_brick = load_sprite(L"data/base/brick.png");
    textures->platform = renderer_create_texture(renderer, spike_brick.pixels, spike_brick.width, spike_brick.height);

    // 地刺贴图
    auto spike_sprite = load_sprite(L"data/base/spike.png");
    textures->spike = renderer_create_texture(renderer, spike_sprite.pixels, spike_sprite.width, spike_sprite.height);

    // 可消失平台：三态各一张（相位索引，见 VanishPhase）
    textures->vanish[VANISH_PHASE_SOLID] = textures->platform;
    textures->vanish[VANISH_PHASE_WARNING] = create_brick_texture(renderer, VANISH_DIM_BRIGHTNESS,
                                                                  VANISH_DIM_ALPHA);
    textures->vanish[VANISH_PHASE_GONE] = create_brick_texture(renderer, VANISH_GHOST_BRIGHTNESS,
                                                               VANISH_GHOST_ALPHA);
    // 传送门的配对色：**一张灰度门贴图 + 逐项 tint**（tint 取共享调色板，与编辑器同色 = 配对）
    textures->portal = create_portal_texture(renderer);
    textures->white = create_solid_color_texture(renderer, 255, 255, 255, 255);
    textures->loading = create_loading_texture(renderer);

    // 字形图集：程序按字形表生成（白色实心 + 透明底），颜色在绘制处用 tint 上
    u32 font_atlas_w = 0;
    u32 font_atlas_h = 0;
    u32 font_atlas_pixels = font_atlas_size(&font_atlas_w, &font_atlas_h);
    u32 *font_pixels = (u32 *)arena_push(sizeof(u32) * font_atlas_pixels);
    font_build_atlas(font_pixels, font_atlas_w, font_atlas_h);
    textures->font = renderer_create_texture(renderer, font_pixels, (int)font_atlas_w, (int)font_atlas_h);

    textures->monster = create_solid_color_texture(renderer, 126, 44, 88, 255);
    // 圆形移动组件：灰度圆盘 + tint（危险色）—— 与传送门同一套做法，想换颜色只改 tint
    textures->mover_circle = create_mover_circle_texture(renderer);
    textures->projectile = create_solid_color_texture(renderer, 80, 230, 255, 255);
    textures->time_stop = create_time_stop_texture(renderer);
    textures->time_stop_outline = create_time_stop_outline_texture(renderer);

#if MONO_DEBUG_VIS
    // 每种调试盒标签一个纯色贴图，由 debug_vis 上报后统一画线框
    for (u32 tag = 0; tag < DEBUG_BOX_COUNT; ++tag) {
        const u8 *color = DEBUG_BOX_COLORS[tag];
        textures->debug_box[tag] = create_solid_color_texture(renderer, color[0], color[1], color[2], color[3]);
    }
#endif
}

// ----------------------------------------------------------------------------
// 绘制提交
// ----------------------------------------------------------------------------

// 同一层内的绘制次序（角色必须画在平台之上）
constexpr int PLATFORM_DRAW_ORDER = 0;
constexpr int TIME_STOP_DRAW_ORDER = 5;
constexpr int TIME_STOP_OUTLINE_DRAW_ORDER = 6;
constexpr int PROJECTILE_DRAW_ORDER = 7;
constexpr int PLAYER_DRAW_ORDER = 10;
// 加载动画画在最上面（它是“此刻不能操作”的唯一提示，不能被地形挡住）
constexpr int LOADING_DRAW_ORDER = 20;

// UI 的所有矩形同在 LAYER_UI 且 order 相同：排序是稳定的，所以按提交顺序画
// （压暗 → 底板 → 记号 → 选中边框），不必给每个元素单独排次序
constexpr int UI_DRAW_ORDER = 0;

// 传送门的门板半透明度：站在门里也能看清角色
constexpr f32 PORTAL_TINT_ALPHA = 110.0f / 255.0f;

// UI 语义 → 颜色与是否线框。与 DEBUG_BOX_COLORS 同一套做法：语义由 UI 层给，颜色归装配层，
// 顺序必须与 UiRectKind 一致（static_assert 卡住）。
// 同一层内 order 相同 → 排序稳定 → 按提交顺序画，所以“压暗 → 底板 → 记号 → 选中边框”不会乱
struct UiRectColor
{
    f32 r;
    f32 g;
    f32 b;
    f32 a;
    bool outline;
};

constexpr UiRectColor UI_RECT_COLORS[UI_RECT_KIND_COUNT] = {
    { 0.00f, 0.00f, 0.00f, 0.62f, false }, // UI_RECT_DIM：压暗但看得见世界（“世界已暂停”的提示）
    { 0.08f, 0.10f, 0.16f, 0.90f, false }, // UI_RECT_ITEM：未选中的底板
    { 0.16f, 0.30f, 0.50f, 0.95f, false }, // UI_RECT_ITEM_SELECTED：选中项的底板
    { 0.75f, 0.90f, 1.00f, 1.00f, true },  // UI_RECT_SELECTION_BORDER：选中项的边框
    { 1.00f, 1.00f, 1.00f, 1.00f, false }, // UI_RECT_TEXT：选中项的文字（最亮）
    { 0.72f, 0.76f, 0.82f, 1.00f, false }, // UI_RECT_TEXT_DIM：未选中项与底部提示的文字
};
static_assert(array_size(UI_RECT_COLORS) == UI_RECT_KIND_COUNT, "UI 颜色表要覆盖每一种语义");

void scene_submit(Renderer *renderer, const SceneTextures *textures, const SceneFrame *frame)
{
    GameState *game_state = frame->game_state;
    const f32 alpha = frame->alpha;

    // 背景：按关卡范围平铺
    if (game_state->backdrop.texture != TEXTURE_NONE) {
        f32 backdrop_w = (f32)game_state->backdrop.width;
        f32 backdrop_h = (f32)game_state->backdrop.height;
        int half_tiles_x = (int)ceilf(game_state->level.bounds.half_w / backdrop_w);
        int half_tiles_y = (int)ceilf(game_state->level.bounds.half_h / backdrop_h);

        // 平铺数固定（只随关卡与视口变），所以用 int 做计数器 —— 它只需要“够大且能取负”
        for (int tile_y = -half_tiles_y; tile_y <= half_tiles_y; ++tile_y) {
            for (int tile_x = -half_tiles_x; tile_x <= half_tiles_x; ++tile_x) {
                renderer_push_sprite(renderer, game_state->backdrop.texture,
                                     game_state->level.bounds.center_x + (f32)tile_x * backdrop_w,
                                     game_state->level.bounds.center_y + (f32)tile_y * backdrop_h,
                                     backdrop_w, backdrop_h,
                                     0.0f, LAYER_BACKGROUND);
            }
        }
    }

    // 平台（贴图每 32 像素重复一次）
    for (u32 i = 0; i < game_state->level.platform_count; ++i) {
        const Platform *platform = &game_state->level.platforms[i];
        renderer_push_sprite(renderer, textures->platform,
                             platform->rect.center_x, platform->rect.center_y,
                             platform->rect.half_w * 2.0f, platform->rect.half_h * 2.0f,
                             0.0f, LAYER_WORLD, 32.0f, false, PLATFORM_DRAW_ORDER);
    }

    // 地刺：每行相邻的格已经合并成一条矩形（level.cc），这里按矩形贴锯齿。
    // uv_scale 取这条矩形的高度（= 一格）而不是写死的 32：竖直方向只铺一次，
    // 所以一格高的地刺带就是**一行**锯齿（以前按 32 铺，64px 高会上下重复成两行）。
    // 精灵照整格铺，但贴图只有下半格有像素 —— 视觉半格高，伤害判定是这条整格矩形
    for (u32 i = 0; i < game_state->level.spike_count; ++i) {
        const Rect2D *spike = &game_state->level.spikes[i];
        renderer_push_sprite(renderer, textures->spike,
                             spike->center_x, spike->center_y,
                             spike->half_w * 2.0f, spike->half_h * 2.0f,
                             0.0f, LAYER_WORLD, spike->half_h * 2.0f, false, PLATFORM_DRAW_ORDER);
    }

    // 可消失平台：相位在固定步里推进（game.cc），这里只按相位选贴图
    for (u32 i = 0; i < game_state->level.vanish_platform_count; ++i) {
        const Rect2D *rect = &game_state->level.vanish_platforms[i];
        VanishPhase phase = game_state->vanish.blocks[i].phase;
        renderer_push_sprite(renderer, textures->vanish[phase],
                             rect->center_x, rect->center_y,
                             rect->half_w * 2.0f, rect->half_h * 2.0f,
                             0.0f, LAYER_WORLD, 32.0f, false, PLATFORM_DRAW_ORDER);
    }

    // 传送门：一对同色 = 互相传送。外观 = **一张灰度门贴图 + 逐项 tint**，
    // tint 取共享调色板（与编辑器同源），所以编辑器里看到的就是游戏里看到的。
    for (u32 i = 0; i < game_state->level.portal_count; ++i) {
        const LevelPortalAsset *portal = &game_state->level.portals[i];
        const LevelPortalColor *color = &LEVEL_PORTAL_PALETTE[portal->pair_id % LEVEL_PORTAL_PALETTE_COUNT];

        SpriteStyle style = { .tint_r = (f32)color->r / 255.0f,
                              .tint_g = (f32)color->g / 255.0f,
                              .tint_b = (f32)color->b / 255.0f,
                              .tint_a = PORTAL_TINT_ALPHA };

        renderer_push_sprite(renderer, textures->portal,
                             portal->rect.center_x, portal->rect.center_y,
                             portal->rect.half_w * 2.0f, portal->rect.half_h * 2.0f,
                             0.0f, LAYER_WORLD, 0.0f, false, PLATFORM_DRAW_ORDER, style);
        // 边框用纯白贴图 tint 成同色（不透明）：灰度门贴图乘色后整体偏暗，
        // 亮边框能一眼看出“这是哪一对门”。
        // order 比门板大 1 —— 描边必须压在门板之上，而这不该由纹理句柄大小决定（见 renderer.h）
        SpriteStyle outline_style = style;
        outline_style.tint_a = 1.0f;
        renderer_push_rect_outline(renderer, textures->white, portal->rect.center_x, portal->rect.center_y,
                                   portal->rect.half_w * 2.0f, portal->rect.half_h * 2.0f, LAYER_WORLD,
                                   PLATFORM_DRAW_ORDER + 1, outline_style);
    }

    // 加载动画：只在加载段出现，画在**目标门上方**（不挡角色）——
    // 帧号由相位进度推出来（game_teleport_phase_progress），所以不需要额外状态，回放也一致。
    if (game_state->teleport.phase == TELEPORT_LOADING &&
        game_state->teleport.dest_portal < game_state->level.portal_count) {
        const Rect2D *rect = &game_state->level.portals[game_state->teleport.dest_portal].rect;
        // 加载段一共切 LOADING_FRAME_SWAPS 次（4 次 ≈ 每 0.1s 一下），再对帧数取模
        constexpr u32 LOADING_FRAME_SWAPS = 4;
        constexpr u32 LOADING_STRIP_FRAMES = 2;
        f32 progress = game_teleport_phase_progress(&game_state->teleport);
        u32 strip_frame = (u32)(progress * (f32)LOADING_FRAME_SWAPS) % LOADING_STRIP_FRAMES;
        constexpr f32 LOADING_ICON_SIZE = 96.0f;
        constexpr f32 LOADING_ICON_GAP = 8.0f;

        renderer_push_sprite(renderer, textures->loading,
                             rect->center_x,
                             rect->center_y + rect->half_h + LOADING_ICON_GAP + LOADING_ICON_SIZE * 0.5f,
                             LOADING_ICON_SIZE, LOADING_ICON_SIZE,
                             0.0f, LAYER_WORLD, 0.0f, false, LOADING_DRAW_ORDER,
                             sprite_strip_frame(strip_frame, LOADING_STRIP_FRAMES));
    }

    // 移动组件：位置在固定步里推进（game.cc），这里只按上一逻辑步与当前逻辑步插值。
    // 插值用的是**矩形**而不是轴参数 t —— 端点反向的那一帧 t 会折返，插值 t 会让本体往回弹一下。
    for (u32 i = 0; i < game_state->movers.count; ++i) {
        const Mover *mover = &game_state->movers.items[i];
        const Mover *prev = (i < game_state->prev_movers.count) ? &game_state->prev_movers.items[i] : mover;
        Rect2D render_mover = mover->rect;
        render_mover.center_x = lerp(prev->rect.center_x, mover->rect.center_x, alpha);
        render_mover.center_y = lerp(prev->rect.center_y, mover->rect.center_y, alpha);
        if (mover->shape == LEVEL_MOVER_CIRCLE) {
            // 圆形：半径 = half_w，贴图本身是圆的（透明外圈），所以画成正方形 sprite
            SpriteStyle style = { .tint_r = 0.95f, .tint_g = 0.32f, .tint_b = 0.26f, .tint_a = 1.0f };
            renderer_push_sprite(renderer, textures->mover_circle,
                                 render_mover.center_x, render_mover.center_y,
                                 mover->half_w * 2.0f, mover->half_w * 2.0f,
                                 0.0f, LAYER_WORLD, 0.0f, false, PLATFORM_DRAW_ORDER, style);
        } else {
            renderer_push_sprite(renderer, textures->platform,
                                 render_mover.center_x, render_mover.center_y,
                                 render_mover.half_w * 2.0f, render_mover.half_h * 2.0f,
                                 0.0f, LAYER_WORLD, 32.0f, false, PLATFORM_DRAW_ORDER);
        }
    }

    if (game_state->monster.active) {
        Rect2D render_monster = game_state->monster.rect;
        render_monster.center_x = lerp(game_state->prev_monster.rect.center_x,
                                       game_state->monster.rect.center_x, alpha);
        render_monster.center_y = lerp(game_state->prev_monster.rect.center_y,
                                       game_state->monster.rect.center_y, alpha);
        renderer_push_sprite(renderer, textures->monster,
                             render_monster.center_x, render_monster.center_y,
                             render_monster.half_w * 2.0f, render_monster.half_h * 2.0f,
                             0.0f, LAYER_WORLD, 0.0f, false, PLATFORM_DRAW_ORDER);
    }

    for (u32 i = 0; i < MAX_PROJECTILES; ++i) {
        const Projectile *projectile = &game_state->projectiles.items[i];
        if (!projectile->active) {
            continue;
        }

        Rect2D render_projectile = projectile->rect;
        const Projectile *prev_projectile = &game_state->prev_projectiles.items[i];
        if (prev_projectile->active) {
            render_projectile.center_x = lerp(prev_projectile->rect.center_x, projectile->rect.center_x, alpha);
            render_projectile.center_y = lerp(prev_projectile->rect.center_y, projectile->rect.center_y, alpha);
        }
        renderer_push_sprite(renderer, textures->projectile,
                             render_projectile.center_x, render_projectile.center_y,
                             render_projectile.half_w * 2.0f, render_projectile.half_h * 2.0f,
                             0.0f, LAYER_WORLD, 0.0f, false, PROJECTILE_DRAW_ORDER);
    }

    if (game_state->time_stop.active) {
        renderer_push_sprite(renderer, textures->time_stop,
                             game_state->time_stop.center_x, game_state->time_stop.center_y,
                             game_state->time_stop.radius * 2.0f, game_state->time_stop.radius * 2.0f,
                             0.0f, LAYER_WORLD, 0.0f, false, TIME_STOP_DRAW_ORDER);
        renderer_push_sprite(renderer, textures->time_stop_outline,
                             game_state->time_stop.center_x, game_state->time_stop.center_y,
                             game_state->time_stop.radius * 2.0f, game_state->time_stop.radius * 2.0f,
                             0.0f, LAYER_WORLD, 0.0f, false, TIME_STOP_OUTLINE_DRAW_ORDER);
    }

#if MONO_DEBUG_VIS
    // 把所有已上报的碰撞盒统一画出来：碰撞/游戏侧不需要写任何绘制代码，
    // 而且上报的是物理盒（而非插值后的渲染盒），看到的盒就是算碰撞的盒。
    // 除探针外都是中空线框；探针另有一套画法，见下面的注释。
    for (u32 tag = 0; tag < DEBUG_BOX_COUNT; ++tag) {
        u32 box_count = 0;
        const Rect2D *boxes = debug_vis_boxes((DebugBoxTag)tag, &box_count);
        for (u32 i = 0; i < box_count; ++i) {
            const Rect2D *box = &boxes[i];
            f32 box_w = box->half_w * 2.0f;
            f32 box_h = box->half_h * 2.0f;

            if ((DebugBoxTag)tag == DEBUG_BOX_GROUND_PROBE) {
                // 探针是玩家盒整体下移 GROUND_PROBE_DEPTH（1 像素），尺寸和玩家盒一样，
                // 画成线框只会得到「黄框底下多一根洋红边」。真正有意义的是它比玩家盒
                // **多出来的那条底边**：脚底往下 1 像素那一带 —— 地面判定就发生在这里。
                // 所以当成「查询区域」画成一条色带，只把厚度抬到看得见：
                // 宽度和底边保持精确（宽度决定碰不碰得到平台，底边决定碰到没有）；
                // 加厚只往上长（长进玩家盒里，但仍是探针自己的范围），
                // 向下加厚会假装探针伸得更低，看起来就像「该判定却没判定」。
                f32 band_px = (f32)frame->client_height * DEBUG_PROBE_HEIGHT_RATIO;
                if (band_px < DEBUG_PROBE_MIN_HEIGHT_PX) {
                    band_px = DEBUG_PROBE_MIN_HEIGHT_PX;
                }
                f32 band_h = GROUND_PROBE_DEPTH;
                f32 min_band_h = band_px / frame->camera_zoom;
                if (band_h < min_band_h) {
                    band_h = min_band_h;
                }
                renderer_push_sprite(renderer, textures->debug_box[tag], box->center_x,
                                     box->center_y - box->half_h + band_h * 0.5f,
                                     box_w, band_h, 0.0f, LAYER_DEBUG, 0.0f, false,
                                     DEBUG_PROBE_DRAW_ORDER);
            } else {
                renderer_push_rect_outline(renderer, textures->debug_box[tag],
                                           box->center_x, box->center_y, box_w, box_h);
            }
        }
    }
#endif

    // 玩家（用插值后的位置画，逻辑位置保持整数步 —— 见 main.cc 的插值段）
    f32 render_player_x = lerp(game_state->prev_player_x, game_state->player_x, alpha);
    f32 render_player_y = lerp(game_state->prev_player_y, game_state->player_y, alpha);
    AnimationFrame *player_frame = get_current_animation(&game_state->player_bagdown_animation);
    renderer_push_sprite(renderer, player_frame->image.texture,
                         render_player_x, render_player_y,
                         (f32)player_frame->image.width * player_frame->image.scale,
                         (f32)player_frame->image.height * player_frame->image.scale,
                         0.0f, LAYER_WORLD, 0.0f, game_state->facing == FACE_LEFT, PLAYER_DRAW_ORDER);

    // UI 层：UI 只上报语义矩形（屏幕空间），贴图/颜色/线框全在这里翻译
    // —— 与 debug_vis 同一套分工。底板与边框用纯白贴图 + tint 上色，文字用字形图集
    {
        UiRect ui_rects[UI_MAX_RECTS] = {};
        const u32 ui_rect_count = ui_collect_rects(game_state, frame->client_width, frame->client_height,
                                                   ui_rects, UI_MAX_RECTS);
        for (u32 i = 0; i < ui_rect_count; ++i) {
            const UiRect *rect = &ui_rects[i];
            const UiRectColor *color = &UI_RECT_COLORS[rect->kind];
            SpriteStyle style = { .tint_r = color->r, .tint_g = color->g, .tint_b = color->b, .tint_a = color->a };

            // 文字：贴图换成字形图集，UV 子矩形取这一个字形 —— 就是为此加的 SpriteStyle
            TextureHandle texture = textures->white;
            if (rect->kind == UI_RECT_TEXT || rect->kind == UI_RECT_TEXT_DIM) {
                texture = textures->font;
                font_glyph_uv(rect->glyph, &style.uv_offset_x, &style.uv_offset_y, &style.uv_scale_x,
                              &style.uv_scale_y);
            }

            if (color->outline) {
                renderer_push_ui_rect_outline(renderer, texture, rect->x, rect->y, rect->w, rect->h,
                                              UI_DRAW_ORDER, style);
            } else {
                renderer_push_ui_rect(renderer, texture, rect->x, rect->y, rect->w, rect->h,
                                      UI_DRAW_ORDER, style);
            }
        }
    }
}
