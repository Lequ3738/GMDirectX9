// GMDirectX9 纯 patch 版(2026-08-06 重构): 无 GML 导出, 只在 DllMain 里把 GM8.0 runner 的
// D3D8 渲染后端替换为 D3D9。此文件保留补丁核心(inject.cpp)依赖的共享符号定义
// 与对外注册口(flush / FFP VS / 设备 Reset 前后回调)。
#include "main.h"

D3DPRESENT_PARAMETERS* present_params;

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
// 挂 flush 入口, inject.cpp 的设备钩子在下列动作发生前调用:
//   提交/内容族(绘制提交、Clear、纹理内容更新、ColorFill、StretchRect、读回截图)、
//   目标族(SetRenderTarget/SetDepthStencilSurface)、
//   状态族(变换/视口/剪裁/渲染状态/采样/着色器与常量/顶点格式与流/灯光材质)。
// [2026-09-14 二批] 由"只钩提交动作+状态快照免疫"扩为字面闭合: 批内容按批打开
// 时刻的状态渲染, 状态写入不切段会把批内后段冻结成旧值 —— 因此全部状态写入槽
// 设钩切段; 状态快照(dssnap)降为纵深防御(防 state block 等钩外路径)。
// 不钩清单与理由见 inject.cpp flush 钩子注释块(Get*/Present 传递/Reset/GammaRamp/
// 光标/调色板/NPatch/DeletePatch; StateBlock 三方法走对象 vtable = 机制盲区)。
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

// ---- 设备 Reset 前后回调(2026-09-14, 设备丢失恢复配套) ----
// 背景: D3D9 的 Reset 要求进程内不存在未释放的 D3DPOOL_DEFAULT 资源(否则
// INVALIDCALL), 且 Reset 成功后 DEFAULT 资源内容失效。外部 DLL(GMGraphic 的 gpart
// RT 状态纹理等)持有的 DEFAULT 资源会卡死 runner 的原生自愈循环。
// 契约(由 d3d9_recovery.cpp 的 ResetDevice/recreate 路径调用):
//   pre  : real Reset 之前触发 —— 释放 DEFAULT 资源、丢弃依赖它们的挂起状态。
//          必须幂等: Reset 失败后 runner 每帧重试, pre 会再次触发, 已释放即空操作。
//   post : Reset 成功后触发(recreated=false), 或整设备重建成功后触发(recreated=true)。
//          recreated=true 时旧设备上的一切对象(含 MANAGED/着色器/声明)均已消亡,
//          注册方需全量重建; false 时仅需重建 DEFAULT 资源(MANAGED 自动存活)。
// pre 触发过而 Reset 未成功时不调 post —— 注册方保持在"已释放"态等下一次机会。
static void (*g_reset_pre_cbs[4])(void) = {};
static void (*g_reset_post_cbs[4])(bool) = {};
static int  g_reset_cb_count = 0;   // pre/post 成对注册, 单计数器

extern "C" __declspec(dllexport) int __cdecl gmdx9_register_reset_callback(
    void (*pre)(void), void (*post)(bool))
{
    if (!pre || !post) return -1;
    for (int i = 0; i < g_reset_cb_count; i++)
        if (g_reset_pre_cbs[i] == pre && g_reset_post_cbs[i] == post)
            return 0;   // 已注册
    if (g_reset_cb_count >= 4) return -1;
    g_reset_pre_cbs[g_reset_cb_count] = pre;
    g_reset_post_cbs[g_reset_cb_count] = post;
    g_reset_cb_count++;
    return 0;
}

void gmdx9_fire_reset_pre(void)
{
    for (int i = 0; i < g_reset_cb_count; i++)
        g_reset_pre_cbs[i]();
}

void gmdx9_fire_reset_post(bool recreated)
{
    for (int i = 0; i < g_reset_cb_count; i++)
        g_reset_post_cbs[i](recreated);
}
