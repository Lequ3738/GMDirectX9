// GMDirectX9 纯 patch 版(2026-08-06 重构)
//
// 本 DLL 不再提供任何 GML 导出函数(原 gm82dx9.cpp / gm_interface.cpp / shaders.cpp /
// transform.cpp / vertex_buffers.cpp 的 __gm82dx9_* 等已全部移除), 只做一件事:
// 在 DllMain 里把 GameMaker 8.0 runner 的 DirectX8 渲染后端替换为 DirectX9。
//
// 此文件仅保留补丁核心(inject.cpp)依赖的共享符号定义:
//   - present_params      CreateDevice 包装捕获的 runner present-params
//   - runner_display_reset 0x4a2228 = INNER_display_set_size(regain_device 用它触发 display reset)
//   - d3d9_device          0x58d388 = GMAPI GMDIRECT3DINFO.direct3dDevice
//                          GMDirectX9 打补丁后此全局即 D3D9 设备指针 —— 也是其它 DLL(如 GMGraphic)的握手点。
#include "gm82dx9.h"
#include "gm_log.h"

D3DPRESENT_PARAMETERS* present_params;

// runner_display_reset = 0x4a2228 = INNER_display_set_size
create_c_function(void, runner_display_reset, 0x4a2228);

// d3d9_device = 0x58d388 (GMAPI GMDIRECT3DINFO.direct3dDevice, 原 8.1 为 0x6886a8)
IDirect3DDevice9** d3d9_device = (IDirect3DDevice9**)0x58d388;

// [GM80] 加载触发导出(2026-08-06 实机确认): GM8 的扩展加载器(sub_518368)只对"注册了函数"的 DLL
// 做 LoadLibrary。函数表为空 → 从不加载 DLL → DllMain 不跑 → 补丁不打、不加载 d3d9.dll。
// 因此必须提供一个最小导出作为"让 GM8 把 DLL 加载进进程"的触发。它不是 GML 功能, 返回值 1 只是标记。
// gej 里注册为 hidden 函数。若游戏 GML 主动调它, 返回 1 表示插件已加载。
extern "C" __declspec(dllexport) int __cdecl gm82dx9_loaded(void) {
    return 1;
}
