#pragma once

// ============================================================================
// Windows SDK 的唯一入口
//
// 任何地方要包含 <windows.h>，都改成包含本文件
//
// 原因有两个：
//   1. 两个开关宏必须在 <windows.h> 之前定义才生效，散在各文件里迟早会漏：
//        WIN32_LEAN_AND_MEAN 少拉 DDE / RPC / Winsock1 / OLE / Shell / CommDlg 等一大堆声明
//        NOMINMAX            防止 min / max 宏污染（本工程自己有 MIN / MAX 模板）
//   2. 保证每个编译单元看到的 Windows 声明集一致
//
// 注意：本头只能包含在 .cc 里。include/ 下的公共头不许包含平台头，
// 需要窗口句柄的地方一律用 void *（见 input.h / renderer.h 的做法）。
//
// 不要在这里加 NOGDI / NOUSER / NOSOUND 之类的宏：本工程要创建窗口、画光标、
// 用 GetSystemMetrics，拿掉这些分组会直接编译不过。
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
