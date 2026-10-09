#pragma once

#include "core.h"

// ============================================================================
// 最小点阵字体（方案 A：自制字形表 + 一张图集）
//
// 为什么是点阵而不是矢量：这是像素风画面，点阵在每个字形都按像素画死，配合渲染层的 POINT 采样
// 得到硬边、100% 可预测的画面（同一份代码在任何机器上长得一样）；矢量字在小字号要靠 hinting
// 才不糊，而且效果依赖系统字体。代价：**一个字一个尺寸，且只有表里有的字**（本表只有 ASCII，
// 小写映射成大写）。将来要中文得换/加一张含 CJK 的图集 —— 那时改的只是「字形从哪来」，
// UI 层与渲染层不需要动（它们只认「码点 → uv 子矩形 + 前进宽度」）。
//
// 三层分工（与 docs/ 里讲的一致）：
//   ① 码点 → 字形下标（font_glyph_index）
//   ② 字形下标 → 图集里的 uv 子矩形（font_glyph_uv）
//   ③ 图集 → 一整张 RGBA 贴图（font_build_atlas，由装配层上传一次）
// 字符串本身按**字节**处理：只支持 ASCII 的显示子集，非 ASCII 字节当作缺字形跳过（不崩、不乱码）。
// ============================================================================

// 字形是 5 宽 7 高；单元 6×8（右侧/下方各留 1 像素）—— 留白是为了图集里相邻字形不互相渗色
inline constexpr u32 FONT_GLYPH_WIDTH = 5;
inline constexpr u32 FONT_GLYPH_HEIGHT = 7;
inline constexpr u32 FONT_CELL_WIDTH = 6;
inline constexpr u32 FONT_CELL_HEIGHT = 8;

// 图集按每行 16 个字形排（字形数量见 font_glyph_count()）
inline constexpr u32 FONT_ATLAS_COLUMNS = 16;

// 字形总数（表在 font.cc 里）
u32 font_glyph_count();

// 图集尺寸与像素数（装配层据此分配缓冲并调 font_build_atlas）
u32 font_atlas_size(u32 *width, u32 *height);
void font_build_atlas(u32 *pixels, u32 width, u32 height);

// ASCII 码点 → 字形下标；没有对应字形时返回 -1（调用方跳过绘制、只推进游标）
int font_glyph_index(u32 codepoint);

// 字形 → 图集里的 UV 变换，**按渲染层 SpriteStyle 的约定给**（uv * scale + offset）：
// 直接写进 style.uv_offset_* / uv_scale_* 即可，不要自己去减（减错是这里最容易犯的错）。
void font_glyph_uv(u32 glyph, f32 *offset_x, f32 *offset_y, f32 *scale_x, f32 *scale_y);

// 一行文本的宽度（像素单位 = 字符单元数，含最后一个字形的右侧留白）与高度
u32 font_text_width(const char *text);
