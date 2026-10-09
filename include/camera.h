#pragma once

#include "core.h"
#include "shared/collision.h"

// 2D 摄像机：记录当前视口在游戏世界中的中心点与缩放级别
struct Camera2D
{
    f32 pos_x;       // 世界坐标 X（像素单位）
    f32 pos_y;       // 世界坐标 Y（像素单位）
    f32 zoom = 1.0f; // 视野缩放（1.0 = 原比例，>1.0 放大，<1.0 缩小拉远）
};

// ============================================================================
// 相机档位（策略的“名字”）
//
// 游戏侧只选档位（比如“这个桥段要更稳”），**不在调用点写任何数值**；
// 数值集中在 `src/camera.cc` 的 `CAMERA_PROFILES` 表里，与下表一一对应：
//   - 想加一种手感 → 这里加一行枚举 + 表里加一行数值 + `camera_profile_name` 加一个 case；
//   - 想调手感 → 只改表里的数（同档位的所有场景一起变，这是“手感可预测”的来源）。
// 档位是**策略**（跨帧不变、由场景决定），不是状态 —— 所以它不进 `GameStateSnapshot`。
enum CameraProfile : u8
{
    CAMERA_PROFILE_DEFAULT, // 常规跟随：水平/竖直同速平滑，竖直完全跟随
    CAMERA_PROFILE_PRECISE, // 精准向：竖直带死区（小跳/走路起伏不带动画面），水平同 DEFAULT
    CAMERA_PROFILE_COUNT,
};

// 档位名（日志/排查用，与枚举一一对应）
const char *camera_profile_name(CameraProfile profile);

// 相机只根据数据更新：
//   跟随 —— 平滑跟随：向目标位置指数收敛（跑得快就落在后面一点，停下后自己追上并停住）；
//   夹取 —— 目标点先夹在关卡边界内，再做插值，所以可见范围永远不会越出关卡；
//          关卡比视野小/矮的那个轴直接对齐关卡中心。
// dt <= 0 表示「直接贴合」（初始化 / 传送用：不该看到相机从旧位置滑过去）。
void camera_follow(Camera2D *camera, f32 target_x, f32 target_y,
                   const Rect2D *bounds, u32 screen_width, u32 screen_height,
                   f32 dt, CameraProfile profile);
