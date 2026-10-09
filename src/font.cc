#include "font.h"

#include <string.h> // memset / strlen

// ============================================================================
// 字形表：ASCII 显示子集（A-Z / 0-9 / 空格 / 少量标点）
//
// 直接写成 ASCII 图案而不是十六进制字节，是为了**能被人读出来**：改字形就是改这几行 '#'，
// 抄错一行也看得出来。每行必须正好 5 个字符 —— 这条有 static_assert 兜着（见文件末尾）。
// 小写不单独画：5×7 里画小写会挤成一团，而且地图界面这种全大写文本完全够用，
// 所以 font_glyph_index 把小写映射到大写。
// ============================================================================

struct FontGlyphSource
{
    char codepoint;
    const char *rows[FONT_GLYPH_HEIGHT]; // '#' = 实心像素，其余 = 空
};

internal constexpr FontGlyphSource FONT_GLYPHS[] = {
    { ' ', { "     ", "     ", "     ", "     ", "     ", "     ", "     " } },
    { 'A', { " ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #" } },
    { 'B', { "#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### " } },
    { 'C', { " ####", "#    ", "#    ", "#    ", "#    ", "#    ", " ####" } },
    { 'D', { "#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### " } },
    { 'E', { "#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####" } },
    { 'F', { "#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    " } },
    { 'G', { " ####", "#    ", "#    ", "#  ##", "#   #", "#   #", " ####" } },
    { 'H', { "#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #" } },
    { 'I', { "#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "#####" } },
    { 'J', { "  ###", "   # ", "   # ", "   # ", "   # ", "#  # ", " ##  " } },
    { 'K', { "#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #" } },
    { 'L', { "#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####" } },
    { 'M', { "#   #", "## ##", "# # #", "#   #", "#   #", "#   #", "#   #" } },
    { 'N', { "#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #" } },
    { 'O', { " ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### " } },
    { 'P', { "#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    " } },
    { 'Q', { " ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #" } },
    { 'R', { "#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #" } },
    { 'S', { " ####", "#    ", "#    ", " ### ", "    #", "    #", "#### " } },
    { 'T', { "#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  " } },
    { 'U', { "#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### " } },
    { 'V', { "#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  " } },
    { 'W', { "#   #", "#   #", "#   #", "#   #", "# # #", "## ##", "#   #" } },
    { 'X', { "#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #" } },
    { 'Y', { "#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  " } },
    { 'Z', { "#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####" } },
    { '0', { " ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### " } },
    { '1', { "  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### " } },
    { '2', { " ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####" } },
    { '3', { "#####", "   # ", "  #  ", "   # ", "    #", "#   #", " ### " } },
    { '4', { "   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # " } },
    { '5', { "#####", "#    ", "#### ", "    #", "    #", "#   #", " ### " } },
    { '6', { "  ## ", " #   ", "#    ", "#### ", "#   #", "#   #", " ### " } },
    { '7', { "#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   " } },
    { '8', { " ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### " } },
    { '9', { " ### ", "#   #", "#   #", " ####", "    #", "   # ", " ##  " } },
    { '.', { "     ", "     ", "     ", "     ", "     ", "     ", "  #  " } },
    { ',', { "     ", "     ", "     ", "     ", "     ", "  #  ", " #   " } },
    { '-', { "     ", "     ", "     ", " ### ", "     ", "     ", "     " } },
    { ':', { "     ", "  #  ", "     ", "     ", "     ", "  #  ", "     " } },
    { '/', { "    #", "    #", "   # ", "  #  ", " #   ", "#    ", "#    " } },
    { '\'', { "  #  ", "  #  ", "     ", "     ", "     ", "     ", "     " } },
    { '!', { "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "     ", "  #  " } },
    { '?', { " ### ", "#   #", "    #", "   # ", "  #  ", "     ", "  #  " } },
};

internal constexpr u32 FONT_GLYPH_COUNT = (u32)array_size(FONT_GLYPHS);

// 图集尺寸：每行 16 个字形，向上取整
internal constexpr u32 FONT_ATLAS_ROWS = (FONT_GLYPH_COUNT + FONT_ATLAS_COLUMNS - 1) / FONT_ATLAS_COLUMNS;
internal constexpr u32 FONT_ATLAS_WIDTH = FONT_ATLAS_COLUMNS * FONT_CELL_WIDTH;
internal constexpr u32 FONT_ATLAS_HEIGHT = FONT_ATLAS_ROWS * FONT_CELL_HEIGHT;

internal constexpr u32 font_cstr_length(const char *text)
{
    u32 length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

// 每行必须正好 5 个字符：抄错一行（多一个空格、少一个 '#'）在这里就是编译错误，
// 而不是画出来之后肉眼看半天才发现某个字歪了
internal constexpr bool font_rows_are_valid()
{
    for (u32 glyph = 0; glyph < FONT_GLYPH_COUNT; ++glyph) {
        for (u32 row = 0; row < FONT_GLYPH_HEIGHT; ++row) {
            if (font_cstr_length(FONT_GLYPHS[glyph].rows[row]) != FONT_GLYPH_WIDTH) {
                return false;
            }
        }
    }
    return true;
}

static_assert(font_rows_are_valid(), "字形表的每一行都必须是 5 个字符（'#' = 实心，其余 = 空）");

// ASCII → 字形下标，**编译期建表**（运行期查表 O(1)，不用每帧扫 45 个字形的名字）。
// 小写在大写那一行上多写一格，于是「小写映射成大写」这件事是数据本身，不需要额外分支。
// 用结构体包一层是因为 constexpr 函数不能按值返回裸数组。
struct FontAsciiMap
{
    i8 glyph[128];
};

internal constexpr FontAsciiMap font_build_ascii_map()
{
    FontAsciiMap map = {};
    for (u32 i = 0; i < 128; ++i) {
        map.glyph[i] = -1;
    }
    for (u32 i = 0; i < FONT_GLYPH_COUNT; ++i) {
        // 码点按**无符号字节**取：表里全是 ASCII，但写成 char（MSVC 上有符号）时
        // `codepoint >= 0` 恒真、`map.glyph[codepoint]` 也是 clang 的 -Wchar-subscripts，
        // 而且一旦表里出现非 ASCII 字节就会变成负下标
        u32 codepoint = (u8)FONT_GLYPHS[i].codepoint;
        if (codepoint < 128) {
            map.glyph[codepoint] = (i8)i;
        }
        if (codepoint >= 'A' && codepoint <= 'Z') {
            map.glyph[codepoint - 'A' + 'a'] = (i8)i;
        }
    }
    return map;
}

inline constexpr FontAsciiMap FONT_ASCII_MAP = font_build_ascii_map();

u32 font_glyph_count()
{
    return FONT_GLYPH_COUNT;
}

u32 font_atlas_size(u32 *width, u32 *height)
{
    *width = FONT_ATLAS_WIDTH;
    *height = FONT_ATLAS_HEIGHT;
    return FONT_ATLAS_WIDTH * FONT_ATLAS_HEIGHT;
}

void font_build_atlas(u32 *pixels, u32 width, u32 height)
{
    // 先整片透明（文字是白色实心 + tint 着色，所以底必须是透明而不是黑色）
    memset(pixels, 0, (u64)width * height * sizeof(u32));

    for (u32 glyph = 0; glyph < FONT_GLYPH_COUNT; ++glyph) {
        u32 cell_x = (glyph % FONT_ATLAS_COLUMNS) * FONT_CELL_WIDTH;
        u32 cell_y = (glyph / FONT_ATLAS_COLUMNS) * FONT_CELL_HEIGHT;
        for (u32 row = 0; row < FONT_GLYPH_HEIGHT; ++row) {
            const char *line = FONT_GLYPHS[glyph].rows[row];
            for (u32 col = 0; col < FONT_GLYPH_WIDTH; ++col) {
                if (line[col] != '#') {
                    continue;
                }
                pixels[(cell_y + row) * width + cell_x + col] = 0xFFFFFFFFu;
            }
        }
    }
}

int font_glyph_index(u32 codepoint)
{
    if (codepoint >= 128) {
        return -1;
    }
    return FONT_ASCII_MAP.glyph[codepoint];
}

void font_glyph_uv(u32 glyph, f32 *offset_x, f32 *offset_y, f32 *scale_x, f32 *scale_y)
{
    assert(glyph < FONT_GLYPH_COUNT);
    u32 cell_x = (glyph % FONT_ATLAS_COLUMNS) * FONT_CELL_WIDTH;
    u32 cell_y = (glyph / FONT_ATLAS_COLUMNS) * FONT_CELL_HEIGHT;

    // 采样的是字形那 5×7 块（不含右侧/下方的留白），换算成渲染层的「偏移 + 缩放」写法
    *offset_x = (f32)cell_x / (f32)FONT_ATLAS_WIDTH;
    *offset_y = (f32)cell_y / (f32)FONT_ATLAS_HEIGHT;
    *scale_x = (f32)FONT_GLYPH_WIDTH / (f32)FONT_ATLAS_WIDTH;
    *scale_y = (f32)FONT_GLYPH_HEIGHT / (f32)FONT_ATLAS_HEIGHT;
}

u32 font_text_width(const char *text)
{
    return (u32)strlen(text) * FONT_CELL_WIDTH;
}
