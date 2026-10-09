#include "camera.h"
#include "shared/mono_math.h"

// ============================================================================
// 档位数值表：全项目相机数值的唯一来源
//
// 字段含义：
//   rate_x / rate_y  水平 / 竖直收敛速度（1/s）：每帧向目标走掉 1 - e^(-rate*dt)。
//                    9 ≈ 0.25 秒走完 90%；调大更紧跟（0 = 瞬时居中），调小更“飘”。
//                    稳态滞距 ≈ 速度 / rate：跑动 640px/s 时约 71px，满速下落 1600px/s 时约 178px。
//   deadzone_y       竖直死区半高（像素）：0 = 完全跟随（关）；> 0 时目标在这个窗口内就不推相机，
//                    小跳/走路起伏不带动画面。窗口外的跟随仍是平滑的（死区 + 边缘平滑）。
//   snap_pixels      贴近吸附（**设备像素**）：差距小于它就一次到位。指数平滑永远只能无限接近，
//                    不吸附的话相机会永远以极小速度微动（看起来就是抖）。
//                    —— 这也是“角色速度归零，相机也必须归到归零”的实现位置。
//                    按设备像素而不是世界像素定义：渲染期会把相机吸附到整数设备像素（见
//                    main.cc 的 render_camera），所以不足 1 个设备像素的差距在屏幕上完全一样，
//                    再慢慢挪只会拖长“看着不动”的尾段。实际用时除以 zoom 才是世界长度。
//
// 调法：改数 → 重编；同档位的所有用户一起变（这是可预测性的代价与好处）。
// 不要提前加没人用的档位；一个档位至少要有一个真实使用者。
// ============================================================================
struct CameraTuning
{
    f32 rate_x;
    f32 rate_y;
    f32 deadzone_y;
    f32 snap_pixels;
};

// 表**不定长**：这样「枚举加了一档、表忘记加一行」是编译错误。
// （写成 [CAMERA_PROFILE_COUNT] 的话少一行只会补一档全零参数 —— 那个档位的 rate = 0，相机永远不收敛，
//   而 default 分支让 profile_name 只回一个 "?"，一路都不会有人报错。）
global_variable constexpr CameraTuning CAMERA_PROFILES[] = {
    // rate_x  rate_y  deadzone_y  snap_pixels
    { 9.0f, 9.0f, 0.0f, 1.0f },   // DEFAULT：水平/竖直同速平滑
    { 9.0f, 9.0f, 128.0f, 1.0f }, // PRECISE：竖直死区 ±2 格，小跳不带动画面
};

static_assert(array_size(CAMERA_PROFILES) == CAMERA_PROFILE_COUNT,
              "CAMERA_PROFILES 必须覆盖每一个 CameraProfile");

const char *camera_profile_name(CameraProfile profile)
{
    switch (profile) {
    case CAMERA_PROFILE_DEFAULT:
        return "DEFAULT";
    case CAMERA_PROFILE_PRECISE:
        return "PRECISE";
    default:
        return "?";
    }
}

// 向目标平滑靠近一步
internal f32 camera_approach(f32 current, f32 target, f32 t, f32 snap_distance)
{
    f32 delta = target - current;
    if (fabsf(delta) <= snap_distance) {
        return target;
    }
    return current + delta * t;
}

// 相机只做三件事：平滑跟随目标、把目标先夹进关卡边界、按吸附规则停住。
//
// 不变量：
//   1. 目标点在夹取后的合法范围内，插值两端（上一帧相机位置与目标）都在范围内 → 结果必在范围内；
//      所以“相机不会露出关卡外”是结构性的，含初始化那一帧（game_init_asset 里以 dt=0 贴合一次）。
//   2. 角色停下后相机平滑追上，进入吸附距离内就精确落在目标上 —— 之后一动不动（速度真为 0，不漂）。
//   3. 档位只决定“怎么收敛”，不决定“跟谁”：目标与夹取对所有档位一致。
void camera_follow(Camera2D *camera, f32 target_x, f32 target_y,
                   const Rect2D *bounds, u32 screen_width, u32 screen_height,
                   f32 dt, CameraProfile profile)
{
    const CameraTuning *tuning = &CAMERA_PROFILES[profile];

    f32 desired_x = target_x;
    f32 desired_y = target_y;

    // 竖直死区（档位里 deadzone_y > 0 时启用）：目标在窗口内就保持相机不动，顶出窗口才被推着走。
    // 注意：窗口内必须把目标映成「相机当前位置」而不是目标本身 —— 否则后面的平滑又把它拉回居中，
    // 死区就退化成“没有任何效果”（这是死区 + 平滑混用时最容易写错的地方）。
    if (tuning->deadzone_y > 0.0f) {
        if (target_y > camera->pos_y + tuning->deadzone_y) {
            desired_y = target_y - tuning->deadzone_y;
        } else if (target_y < camera->pos_y - tuning->deadzone_y) {
            desired_y = target_y + tuning->deadzone_y;
        } else {
            desired_y = camera->pos_y;
        }
    }

    if (bounds) {
        f32 half_view_w = (f32)screen_width * 0.5f / camera->zoom;
        f32 half_view_h = (f32)screen_height * 0.5f / camera->zoom;

        f32 min_x = bounds->center_x - bounds->half_w;
        f32 max_x = bounds->center_x + bounds->half_w;
        if ((max_x - min_x) <= half_view_w * 2.0f) {
            desired_x = bounds->center_x;
        } else {
            desired_x = clamp(desired_x, min_x + half_view_w, max_x - half_view_w);
        }

        f32 min_y = bounds->center_y - bounds->half_h;
        f32 max_y = bounds->center_y + bounds->half_h;
        if ((max_y - min_y) <= half_view_h * 2.0f) {
            desired_y = bounds->center_y;
        } else {
            desired_y = clamp(desired_y, min_y + half_view_h, max_y - half_view_h);
        }
    }

    // dt <= 0 = 直接贴合（初始化 / 传送）；否则指数收敛（与帧率无关）。
    //
    // 注意：这里**不能**用“相机离目标多远”来猜传送 —— 那个距离里混着跟随滞距
    // （跑步稳态 ≈ 速度/rate ≈ 71px，满速下落 ≈ 178px），拿它当阈值会让相机在
    // 正常跑动/跳跃时周期性瞬移（踩过：滞距涨到 64px 就“贴合”一次，画面锯齿状卡顿）。
    // 传送由调用方用「角色这一帧实际走了多远」判断后以 dt=0 告知。
    f32 t_x = (dt > 0.0f) ? 1.0f - expf(-tuning->rate_x * dt) : 1.0f;
    f32 t_y = (dt > 0.0f) ? 1.0f - expf(-tuning->rate_y * dt) : 1.0f;

    // 吸附阈值：表里是设备像素，这里换成世界长度（1 个设备像素 = 1 / zoom 个世界像素）。
    // 渲染期会把相机吸附到整数设备像素，所以这一步决定的是“逻辑相机什么时候真的停下”。
    f32 snap_world = tuning->snap_pixels / camera->zoom;

    camera->pos_x = camera_approach(camera->pos_x, desired_x, t_x, snap_world);
    camera->pos_y = camera_approach(camera->pos_y, desired_y, t_y, snap_world);
}
