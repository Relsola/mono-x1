#include "ui.h"

#include "font.h"
#include "game.h"
#include "shared/mono_math.h"

#include <stdio.h> // snprintf（候选项标签）

// ============================================================================
// 大地图 UI：一块全屏压暗 + 一列候选项（每项 = 某个世界的一个传送点）
//
// 候选项的文字是「世界名 + 序号」（如 `SECOND 1`），下面一行操作提示。
// 布局、颜色数值都集中在这里；装配层只按 kind 查表取贴图与颜色，
// 字形图集在装配层（main.cc）上传，这里只报「哪个字形 + 画在哪」。
// ============================================================================

// 布局数值（逻辑像素，再乘 ui_scale）
internal constexpr f32 UI_ITEM_WIDTH = 320.0f;
internal constexpr f32 UI_ITEM_HEIGHT = 88.0f;
internal constexpr f32 UI_ITEM_GAP = 16.0f;

// 列表最多占屏幕高度的比例：超过就整体压一次（见 ui_item_box）
internal constexpr f32 UI_LIST_MAX_HEIGHT_RATIO = 0.8f;

// 底部那行操作提示（全大写：字形表只有大写，小写会被映射过去，这里写大写少一层转换）
internal const char *const UI_HINT_TEXT = "ARROWS SELECT  SPACE OR CLICK GO  MAP ESC CLOSE";

// UI 尺寸统一按视口高度换算（720p → 1、4K → 3）：位图 UI 在原生尺寸附近最清晰，
// 而且同一套数值在大屏上要按比例放大才不会变成小块。
// 下限取 1.0：窗口比 720p 还矮时不再缩小（缩了就看不清了），所以小窗口下 UI 相对更大 —— 这是有意的。
internal f32 ui_scale_for(u32 client_height)
{
    return MAX(1.0f, (f32)client_height / 720.0f);
}

// 文字的尺度必须是**整数**倍：点阵字体只有在整数倍下才每个像素落在整个像素上，
// 非整数倍会让字形边缘忽胖忽瘦（与相机吸附是同一类问题）。
// 它从 ui_scale 取整而来（1080p → 1、4K → 3），所以文字与底板的尺度在大屏上一致，小屏上不一定。
internal f32 ui_glyph_scale(u32 client_height)
{
    return (f32)MAX(1u, client_height / 720);
}

// 候选项 = **所有世界的传送点**，按「世界号 → 数组顺序」排。
// 这个顺序是确定性的（与关卡数据一一对应），所以脚本能指着它写断言；
// 数量上限由 UI_MAX_ITEMS 卡（game.h 的 static_assert 保证它装得下所有传送点）。
struct UiItemRef
{
    WorldId world;
    u32 waypoint;
};

internal u32 ui_item_count(const GameState *game_state)
{
    u32 total = 0;
    for (u32 w = 0; w < WORLD_COUNT; ++w) {
        total += game_state->worlds[w].waypoint_count;
    }
    return total;
}

// 第 index 项指向哪个世界的哪个传送点；越界返回 false
internal bool ui_item_ref(const GameState *game_state, u32 index, UiItemRef *out)
{
    for (u32 w = 0; w < WORLD_COUNT; ++w) {
        u32 count = game_state->worlds[w].waypoint_count;
        if (index < count) {
            out->world = (WorldId)w;
            out->waypoint = index;
            return true;
        }
        index -= count;
    }
    return false;
}

// 某个世界的第一个传送点在列表里的下标（开图时把光标对齐到当前世界用）
internal u32 ui_first_item_of_world(const GameState *game_state, WorldId world)
{
    u32 index = 0;
    for (u32 w = 0; w < (u32)world && w < WORLD_COUNT; ++w) {
        index += game_state->worlds[w].waypoint_count;
    }
    return index;
}

// 一项在屏幕上的框（左上角 + 尺寸）。**布局只有这一处**：绘制与鼠标命中都调它 ——
// 两处各算一遍的话，「点到的」与「看到的」会在改布局时各自漂（这类 bug 只有在改布局时才暴露）。
struct UiItemBox
{
    f32 x;
    f32 y;
    f32 w;
    f32 h;
};

internal void ui_item_box(u32 index, u32 count, u32 client_width, u32 client_height, UiItemBox *out)
{
    const f32 scale = ui_scale_for(client_height);
    const f32 item_w = UI_ITEM_WIDTH * scale;
    f32 item_h = UI_ITEM_HEIGHT * scale;
    f32 gap = UI_ITEM_GAP * scale;

    // 条数多到放不下时按可用高度**整体压一次**：最多 UI_MAX_ITEMS = 8 项，
    // 4K 全屏下 8 项按原尺寸会顶出画面，而顶出去的那些既看不见也点不到。
    // 压的是列表的项高与间距 —— ui_scale（全局字号/元件尺度）与项宽不动
    const u32 gaps = (count > 0) ? (count - 1) : 0;
    f32 needed = (f32)count * item_h + (f32)gaps * gap;
    f32 available = (f32)client_height * UI_LIST_MAX_HEIGHT_RATIO;
    if (needed > available && needed > 0.0f) {
        f32 shrink = available / needed;
        item_h *= shrink;
        gap *= shrink;
    }

    f32 total_h = (f32)count * item_h + (f32)gaps * gap;
    *out = UiItemBox{ .x = ((f32)client_width - item_w) * 0.5f,
                      .y = ((f32)client_height - total_h) * 0.5f + (f32)index * (item_h + gap),
                      .w = item_w,
                      .h = item_h };
}

// 一行文本 → 一串字形矩形（每个可见字形一个）。返回新的 written。
// 容量不够就停手（不溢出）—— 文字被截断比每帧越界写强，但所以标签字符串别写长。
internal u32 ui_emit_text(const char *text, f32 x, f32 y, f32 glyph_scale, UiRectKind kind, UiRect *out,
                          u32 written, u32 capacity)
{
    f32 pen_x = x;
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        int glyph = font_glyph_index((u8)*cursor);
        if (glyph >= 0 && written < capacity) {
            out[written] = { pen_x, y, (f32)FONT_GLYPH_WIDTH * glyph_scale, (f32)FONT_GLYPH_HEIGHT * glyph_scale,
                             kind, (u32)glyph };
            ++written;
        }
        // 缺字形也要推进游标：非 ASCII 字节在那里占位，而不是把所有字挤在一起
        pen_x += (f32)FONT_CELL_WIDTH * glyph_scale;
    }
    return written;
}

// 居中画一行文本（水平居中在 [left, left + width) 内）
internal u32 ui_emit_text_centered(const char *text, f32 left, f32 width, f32 y, f32 glyph_scale,
                                   UiRectKind kind, UiRect *out, u32 written, u32 capacity)
{
    f32 text_width = (f32)font_text_width(text) * glyph_scale;
    return ui_emit_text(text, left + (width - text_width) * 0.5f, y, glyph_scale, kind, out, written, capacity);
}

bool ui_update(GameState *game_state, const GameInput *input, u32 viewport_width, u32 viewport_height)
{
    UiState *ui = &game_state->ui;
    const PlayerInput *controller = &input->player;

    if (controller->pressed[GA_MAP]) {
        ui->open = !ui->open;
        if (ui->open) {
            // 开图时把光标对齐到**当前世界的第一个传送点**：玩家最可能想传回「我刚在的地方」。
            // 那个世界一个传送点都没有（或者它在列表里已经越界）时退回到第一项
            u32 first = ui_first_item_of_world(game_state, game_state->world);
            ui->selected = (first < ui_item_count(game_state)) ? first : 0;
        }
        // 开关的这一帧不再处理其它输入（避免「一下按键同时开关 + 确认」）
        return true;
    }

    if (!ui->open) {
        return false;
    }

    const u32 count = ui_item_count(game_state);
    if (count == 0) {
        // 图开着但一个传送点都没有（还没摆）：什么都别做，只是世界暂停着
        // —— 下面几处 % count 会除零，所以这个提前退出是必须的
        return true;
    }

    // 上下左右都给同一个含义：候选项只有一列，四个方向键都能选项 ——
    // 省得玩家按了键才发现在另一对键上
    if (controller->pressed[GA_UP] || controller->pressed[GA_LEFT]) {
        ui->selected = (ui->selected + count - 1) % count;
    }
    if (controller->pressed[GA_DOWN] || controller->pressed[GA_RIGHT]) {
        ui->selected = (ui->selected + 1) % count;
    }

    // 鼠标：悬停即选项（与方向键同义），左键落在某项上 = 确认。
    // **只有真的悬停在某一项上才改选中项** —— 否则鼠标停在空白处会把选中项一直拉回第一项，
    // 手柄/键盘选好的东西会被鼠标一遛就改掉。
    bool hovering = false;
    for (u32 i = 0; i < count; ++i) {
        UiItemBox box = {};
        ui_item_box(i, count, viewport_width, viewport_height, &box);
        // 鼠标坐标与 UI 矩形是同一个空间（都是客户区像素，两个输入后端已经统一过），所以直接比
        if (input->mouse.x >= box.x && input->mouse.x < box.x + box.w && input->mouse.y >= box.y &&
            input->mouse.y < box.y + box.h) {
            ui->selected = i;
            hovering = true;
            break;
        }
    }

    if (controller->pressed[GA_JUMP] || (hovering && input->mouse.pressed[MOUSE_LEFT])) {
        UiItemRef ref = {};
        if (ui_item_ref(game_state, ui->selected, &ref)) {
            ui->open = false;
            game_teleport_to_waypoint(game_state, ref.world, ref.waypoint, viewport_width, viewport_height);
        }
    }

    // 关图这一步同样要跳过游戏逻辑：否则“按确认”的 GA_JUMP 会在同一步漏给角色，多跳一下
    return true;
}

u32 ui_collect_rects(const GameState *game_state, u32 client_width, u32 client_height,
                     UiRect *out, u32 capacity)
{
    const UiState *ui = &game_state->ui;
    if (!ui->open) {
        return 0;
    }

    const f32 glyph_scale = ui_glyph_scale(client_height);
    const f32 screen_w = (f32)client_width;
    const f32 screen_h = (f32)client_height;
    const u32 count = ui_item_count(game_state);
    // 这里的容量推演只能给个大概（标签长度可变），所以 UI_MAX_RECTS 取的是宽松值；
    // ui_emit_text 写满就停，不会溢出
    assert(count <= UI_MAX_ITEMS);

    u32 written = 0;
    // 全屏压暗：它是「世界已暂停」的唯一提示（也是本阶段唯一不出现在世界里的元素）
    out[written++] = { 0.0f, 0.0f, screen_w, screen_h, UI_RECT_DIM };

    for (u32 i = 0; i < count; ++i) {
        UiItemBox box = {};
        ui_item_box(i, count, client_width, client_height, &box);

        // 底板：选中的与未选中的只差一个 kind（颜色在装配层的表里）
        const bool selected = (i == ui->selected);
        out[written++] = { box.x, box.y, box.w, box.h,
                           selected ? UI_RECT_ITEM_SELECTED : UI_RECT_ITEM, 0 };

        // 标签 = 世界名 + 在这个世界里的序号（如 `SECOND 2`）。
        // 它是**运行时拼出来的**（世界名来自 game_world_name，序号来自数组下标），
        // 资产里没有字符串 —— 想给传送点起名字就得给格式加字段，那是另一件事
        UiItemRef ref = {};
        if (ui_item_ref(game_state, i, &ref)) {
            char label[32] = {};
            snprintf(label, sizeof(label), "%s %u", game_world_name(ref.world), ref.waypoint + 1);
            const f32 text_h = (f32)FONT_GLYPH_HEIGHT * glyph_scale;
            written = ui_emit_text_centered(label, box.x, box.w, box.y + (box.h - text_h) * 0.5f, glyph_scale,
                                            selected ? UI_RECT_TEXT : UI_RECT_TEXT_DIM, out, written, capacity);
        }

        if (selected && written < capacity) {
            out[written++] = { box.x, box.y, box.w, box.h, UI_RECT_SELECTION_BORDER, 0 };
        }
    }

    // 底部一行操作提示：世界暂停着，所以得告诉玩家怎么操作
    const f32 hint_h = (f32)FONT_GLYPH_HEIGHT * glyph_scale;
    written = ui_emit_text_centered(UI_HINT_TEXT, 0.0f, screen_w,
                                    screen_h - hint_h - (f32)FONT_CELL_HEIGHT * glyph_scale, glyph_scale,
                                    UI_RECT_TEXT_DIM, out, written, capacity);

    return written;
}
