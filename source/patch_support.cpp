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

// ---- 绘制前 flush 回调(自动合批保序, 2026-09-14) ----
// 图集类插件(如 GMGraphic)把绘制攒在自己的顶点缓冲延迟提交, 与引擎原生绘制的即时
// 提交混用时顺序会断(游戏侧手写 force_draw_to_screen 即为此而生)。插件经本注册口
// 挂 flush 入口, inject.cpp 的八槽设备钩子在"绘制提交动作"(DrawPrimitive/
// DrawIndexedPrimitive 含 UP 变体/Clear/SetRenderTarget/SetDepthStencilSurface/
// EndScene)发生前调用 —— 顺序由机制保证, 游戏侧无需再写 flush。批对状态变更的
// 免疫由 flush 侧的状态快照(GMGraphic 端 dssnap_*)保证, 因此状态类槽位不设钩。
// 多注册者契约: 一次 fire_flush 按注册序逐个回调, 各自整批原子提交; 不同注册者
// 批间的交错提交序(A1 B1 原生 A2 B2)在任何冲刷序下都不可复现, 这是"批=原子"
// 模型的固有语义 —— 新的攒批系统应并入现有批(GMGraphic 图集批)而非自立注册者。
static void (*g_flush_cbs[8])(void) = {};
static int  g_flush_cb_count = 0;
static bool g_flush_active = false;   // 回调执行中: 钩子直接透传(回调自身的设备调用不再触发)

extern "C" __declspec(dllexport) int __cdecl gmdx9_register_flush_callback(void (*cb)(void))
{
    if (!cb) return -1;
    for (int i = 0; i < g_flush_cb_count; i++)
        if (g_flush_cbs[i] == cb) return 0;   // 已注册
    if (g_flush_cb_count >= 8) return -1;
    g_flush_cbs[g_flush_cb_count++] = cb;
    return 0;
}

// 热路径纪律: 每个引擎绘制都会路过 —— 无注册或重入时一次判空返回。
// 主线程纪律: GM8 单线程渲染, GMGraphic 的 shader worker 不绘制, 标志无需原子。
void gmdx9_fire_flush(void)
{
    if (g_flush_cb_count <= 0 || g_flush_active) return;
    g_flush_active = true;
    const int n = g_flush_cb_count;   // 快照: 冲刷途中再注册不进本遍
    for (int i = 0; i < n; i++)
        g_flush_cbs[i]();
    g_flush_active = false;
}
