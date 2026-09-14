#pragma once
#include "main.h"

// ============================================================================
// [2026-09-14 桥梁期修复③] 设备状态影子表
// 43 槽设备钩同步维护的内存状态账本: 引擎/插件的每笔 Set* 成功后记账, 此后一切
// "需要知道当前设备状态"的场合(SetVertexShader 包装、GMGraphic 批自愈、统一流
// 捕获器)读影子, 不再调用设备的 Get*(消除运行时开销与 COM AddRef/Release 往返)。
//
// 覆盖闭包(路线图 §3.2): 渲染状态(索引 0-255)/TSS(stage0-7, type 0-63)/采样器
// (0-15, type 0-15)/纹理(0-15)/变换族(D3DTS_WORLDMATRIX(0-255)/VIEW/PROJECTION
// = 索引 0-257)/视口/FVF/顶点声明/VS/PS。
// 明确不记账(钩照装, 账不记): 常量寄存器、材质/灯光、剪裁面/剪裁矩形、流与
// 索引缓冲、渲染目标、StateBlock 三方法(契约盲区, 见 inject.cpp 不钩清单)。
//
// 播种: 设备创建成功(钩安装处)与每次 Reset/重建成功后 seed_from_device 全量
// Get 一次, 此后只靠 Set* 钩维护。影子里的纹理/着色器/声明指针是记账副本,
// 不 AddRef —— 生命周期由设备自身的绑定引用保证(对象被解绑前设备持引用,
// 解绑那笔 Set* 本身就会刷新影子)。
//
// 对账调试模式(环境变量 GMDX9_SHADOW_AUDIT=1, 首笔记账时惰性读取): 每笔记账
// 后即时真 Get 逐项比对, 差异写工作目录 gmdx9_shadow_audit.log(封顶 2000 行)
// —— 验证期跑全场景零差异后转常规关闭(默认关, 每笔记账纯内存写)。
// 线程约定: 设备状态写入只发生在引擎渲染主线程, 影子无锁。
// ============================================================================

namespace shadow
{
    // 播种/重播(设备创建成功与 Reset 成功路径调用)。Get 失败的项标记无效。
    void seed_from_device(IDirect3DDevice9* dev);
    bool live();

    // Set* 钩记账口(hr 失败不记账)。dev 仅供对账模式的真 Get 使用。
    void update_rs  (IDirect3DDevice9* dev, DWORD state, DWORD value, HRESULT hr);
    void update_tss (IDirect3DDevice9* dev, DWORD stage, DWORD type, DWORD value, HRESULT hr);
    void update_samp(IDirect3DDevice9* dev, DWORD sampler, DWORD type, DWORD value, HRESULT hr);
    void update_tex (IDirect3DDevice9* dev, DWORD stage, void* tex, HRESULT hr);
    void update_xf  (IDirect3DDevice9* dev, DWORD state, const D3DMATRIX* m, HRESULT hr);
    void update_xf_mul(IDirect3DDevice9* dev, DWORD state, const D3DMATRIX* m, HRESULT hr);
    void update_vp  (IDirect3DDevice9* dev, const D3DVIEWPORT9* vp, HRESULT hr);
    void update_fvf (IDirect3DDevice9* dev, DWORD fvf, HRESULT hr);
    void update_decl(IDirect3DDevice9* dev, void* decl, HRESULT hr);
    void update_vs  (IDirect3DDevice9* dev, void* vs, HRESULT hr);
    void update_ps  (IDirect3DDevice9* dev, void* ps, HRESULT hr);

    // 借读(不 AddRef, 调用方不得 Release; 未播种返回 false)。
    bool borrow_vs(void** out);
    bool borrow_ps(void** out);
    bool copy_xf(DWORD state, D3DMATRIX* out);
}

// ---- 对外读口(DLL 间契约; GMGraphic 与统一流捕获器经 GetProcAddress 解析) ----
// 单导出返回 v1 结构指针, 只增不改 —— 新字段一律尾部追加并递增 version。
struct gmdx9_shadow_api_v1
{
    DWORD size;      // = sizeof(gmdx9_shadow_api_v1)
    DWORD version;   // = 1
    // 数值类返回 false = 该项无效(未播种/Get 曾失败); 指针类 nullptr 既可能是
    // "合法的未绑定"也可能是未就绪 —— 调用方先探 live()。
    bool  (__cdecl* live)();
    bool  (__cdecl* get_rs)(DWORD state, DWORD* out);
    bool  (__cdecl* get_tss)(DWORD stage, DWORD type, DWORD* out);
    bool  (__cdecl* get_sampler)(DWORD sampler, DWORD type, DWORD* out);
    void* (__cdecl* get_texture)(DWORD sampler);
    bool  (__cdecl* get_xf)(DWORD state, void* out4x4);
    bool  (__cdecl* get_vp)(void* out);            // D3DVIEWPORT9*
    bool  (__cdecl* get_fvf)(DWORD* out);
    void* (__cdecl* get_decl)();
    void* (__cdecl* get_vs)();
    void* (__cdecl* get_ps)();
};

extern "C" __declspec(dllexport) const gmdx9_shadow_api_v1* __cdecl gmdx9_shadow_api();
