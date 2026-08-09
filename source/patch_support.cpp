// GMDirectX9 纯 patch 版(2026-08-06 重构): 无 GML 导出, 只在 DllMain 里把 GM8.0 runner 的
// D3D8 渲染后端替换为 D3D9。此文件仅保留补丁核心(inject.cpp)依赖的共享符号定义。
#include "gm82dx9.h"
#include "gm_log.h"

D3DPRESENT_PARAMETERS* present_params;

// runner_display_reset = 0x4a2228 = INNER_display_set_size
create_c_function(void, runner_display_reset, 0x4a2228);

// d3d9_device = 0x58d388 (GMAPI GMDIRECT3DINFO.direct3dDevice, 原 8.1 为 0x6886a8)
IDirect3DDevice9** d3d9_device = (IDirect3DDevice9**)0x58d388;

// [GM80] GM8 扩展加载器只 LoadLibrary 注册了函数的 DLL → 必须提供此最小导出作为加载触发。
// 非 GML 功能, 返回 1 仅标记插件已加载(gej 里注册为 hidden)。
extern "C" __declspec(dllexport) int __cdecl gm82dx9_loaded(void) {
    return 1;
}
