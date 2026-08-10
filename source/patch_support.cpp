// GMDirectX9 纯 patch 版(2026-08-06 重构): 无 GML 导出, 只在 DllMain 里把 GM8.0 runner 的
// D3D8 渲染后端替换为 D3D9。此文件仅保留补丁核心(inject.cpp)依赖的共享符号定义。
#include "main.h"

D3DPRESENT_PARAMETERS* present_params;

// runner_display_reset = 0x4a2228 = INNER_display_set_size
create_c_function(void, runner_display_reset, 0x4a2228);

// d3d9_device = 0x58d388 (GMAPI GMDIRECT3DINFO.direct3dDevice, 原 8.1 为 0x6886a8)
IDirect3DDevice9** d3d9_device = (IDirect3DDevice9**)0x58d388;

// GM8 扩展加载器只 LoadLibrary 注册了函数的 DLL，必须提供此最小导出作为加载触发。
extern "C" __declspec(dllexport) double __cdecl gmdx9_loaded(void)
{
    return 1;
}

// FFP VS 注册槽: 插件在初始化时调用本函数注册自己仿固定管线 VS 的变量地址。
// 引擎绘制前的 SetVertexShader 钩子会识别这些 VS 并刷新 c0-c3 WVP 到当前投影。
// ppvs = 插件内部 FFP VS 变量的地址(&static IDirect3DVertexShader9*)。
static void** g_ffp_vs_slots[8];
static int g_ffp_vs_slot_count = 0;

extern "C" __declspec(dllexport) int __cdecl gmdx9_register_ffp_vs(void** ppvs)
{
    if (!ppvs || g_ffp_vs_slot_count >= 8)
        return -1;
    for (int i = 0; i < g_ffp_vs_slot_count; i++)
    {
        if (g_ffp_vs_slots[i] == ppvs)
            return 0;   // 已注册
    }
    g_ffp_vs_slots[g_ffp_vs_slot_count++] = ppvs;
    return 0;
}

// 槽查询(供 inject.cpp 的 SetVertexShader 钩子判断当前 VS 是否 FFP 型)。
int gmdx9_ffp_vs_count(void)
{
    return g_ffp_vs_slot_count;
}

void** gmdx9_ffp_vs_slot(int i)
{
    return (i >= 0 && i < g_ffp_vs_slot_count) ? g_ffp_vs_slots[i] : nullptr;
}
