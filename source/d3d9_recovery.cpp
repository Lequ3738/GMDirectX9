#include "d3d9_recovery.h"
#include "main.h"
#include "state_shadow.h"
#include <string.h>

static D3DPRESENT_PARAMETERS
    g_pp9; // 设备创建时的 pp9 副本(与 CreateDevice 完全一致 → Reset 必成功)
static HRESULT(WINAPI* real_reset)(
    IDirect3DDevice9*, D3DPRESENT_PARAMETERS*) = nullptr; // 原始 D3D9 Reset(vt[16], 覆盖前保存)
static UINT g_adapter = 0;
static D3DDEVTYPE g_devtype = D3DDEVTYPE_HAL;
static HWND g_focuswin = nullptr;
static DWORD g_bf = 0;
static ULONGLONG g_last_recreate = 0;
static const ULONGLONG RECREATE_COOLDOWN_MS = 2000;
// 注: 不保存旧 IDirect3D9 复用 —— 适配器状态也可能损坏, 重建时连同 D3D 对象一起新建。

#ifdef GMDX9_RECOVERY_TEST
// ---- 测试构建: 桩存储替代 runner 全局(测试进程内 0x58d388 无映射) ----
static IDirect3DDevice9* t_pub_dev = nullptr;
static IDirect3D9* t_pub_d3d9 = nullptr;
static IDirect3DDevice9* recovery_get_runner_device(void) { return t_pub_dev; }
static void recovery_publish_runner_device(IDirect3DDevice9* dev, IDirect3D9* d3d9)
{
    t_pub_dev = dev;
    t_pub_d3d9 = d3d9;
}
#else
// ---- 生产构建: 直接读写 runner 全局 ----
static IDirect3DDevice9* recovery_get_runner_device(void)
{
    return *(IDirect3DDevice9**)0x58d388;
}
static void recovery_publish_runner_device(IDirect3DDevice9* dev, IDirect3D9* d3d9)
{
    // runner + GMGraphic 都从 0x58d388 取设备; 接口全局(0x58d38C)一并更新
    *(IDirect3DDevice9**)0x58d388 = dev;
    *(IDirect3D9**)0x58d38C = d3d9;
}
#endif

void gmdx9_recovery_on_device_created(IDirect3DDevice9* dev,
    const D3DPRESENT_PARAMETERS* pp9,
    UINT adapter, D3DDEVTYPE devtype, HWND focuswin, DWORD behaviorflags)
{
    g_pp9 = *pp9;
    g_adapter = adapter;
    g_devtype = devtype;
    g_focuswin = focuswin;
    g_bf = behaviorflags;
    __try
    {
        void** vt = *(void***)dev;
        DWORD oldp;
        if (!real_reset)
        {
            // 必须在覆盖前从 vt[16] 取真 Reset(2026-08-16 曾误按 D3D8 布局从 vt[14] 取,
            // 实际存成 GetSwapChain → 真 Reset 从未执行、一切失败都落到整设备重建)。
            real_reset = (HRESULT(WINAPI*)(IDirect3DDevice9*,
                D3DPRESENT_PARAMETERS*))vt[16];
            if (VirtualProtect(&vt[16], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldp))
            {
                vt[16] = (void*)&ResetDevice; // runner 的 Reset 调用点已补丁改指 [eax+40h]
                VirtualProtect(&vt[16], sizeof(void*), oldp, &oldp);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static HRESULT gmdx9_recreate_device(void); // 定义在文件后部; 仅 Reset 硬崩溃(SEH)时兜底调用

HRESULT WINAPI ResetDevice(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pParams)
{
    (void)pParams;
    if (!real_reset)
        return S_OK; // 钩子未安装: 维持 no-op

    HRESULT tcl = dev->TestCooperativeLevel();
    if (tcl == D3DERR_DEVICELOST)
        return D3DERR_DEVICELOST;
    if (!IsWindow(g_pp9.hDeviceWindow))
        return D3DERR_DEVICELOST; // 创建时窗口已失效 → 无法安全 Reset, 等下帧

    // [2026-09-14] Reset 前回调: D3D9 要求 Reset 时进程内无未释放的 DEFAULT 池资源
    // (否则 INVALIDCALL, 设备永久卡死) —— 外部 DLL(GMGraphic)的 DEFAULT 资源必须
    // 先释放。pre 幂等: Reset 失败后 runner 每帧重试, 已释放即空操作; 失败不调 post。
    gmdx9_fire_reset_pre();

    __try
    {
        HRESULT hr = real_reset(dev, &g_pp9);
        if (SUCCEEDED(hr))
        {
            // [2026-09-14 修复③] Reset 后设备状态回到出厂默认 —— 影子表全量重播,
            // 否则账面停留在 Reset 前的旧值, 后续读账全部失真。
            shadow::seed_from_device(dev);
            gmdx9_fire_reset_post(false);   // 同设备 Reset: 仅 DEFAULT 资源需重建
        }
        return hr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // 真 Reset 内部硬崩溃：整体重建兜底(post(recreated=true) 在重建成功后触发)
        gmdx9_recreate_device();
        return D3DERR_DEVICELOST;
    }
}

void gm80_restore_reset_hook(void)
{
    __try
    {
        IDirect3DDevice9* dev = recovery_get_runner_device();
        if (!dev || !real_reset) return;
        void** vt = *(void***)dev;
        if (!vt) return;
        DWORD oldp;
        if (vt[16] != (void*)&ResetDevice) return; // 钩子已被别处改过 → 不碰
        if (VirtualProtect(&vt[16], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldp))
        {
            vt[16] = (void*)real_reset;
            VirtualProtect(&vt[16], sizeof(void*), oldp, &oldp);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // 设备已释放/不可读 → 忽略(进程/游戏正在结束)
    }
}

// ============ 设备重建 ============
static void gmdx9_install_device_hooks(IDirect3DDevice9* dev)
{
    __try
    {
        void** vt = *(void***)dev;
        DWORD oldp;
        if (real_reset)
        {
            if (VirtualProtect(&vt[16], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldp))
            {
                vt[16] = (void*)&ResetDevice; // runner 调用点已改指 [eax+40h]
                VirtualProtect(&vt[16], sizeof(void*), oldp, &oldp);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    // [2026-09-14] 重建设备同样要装 SetTexture 白像素 + flush 钩子组
    // (原先重建路径无钩, 白像素兜底静默失效; 现与 CreateDevice 共用安装器)。
    gmdx9_install_render_hooks(dev);
}

static HRESULT gmdx9_recreate_device(void)
{
    ULONGLONG now = GetTickCount64();
    if (now - g_last_recreate < RECREATE_COOLDOWN_MS)
        return E_FAIL; // 冷却中(创建失败也每 2s 重试)
    g_last_recreate = now;
    __try
    {
        IDirect3DDevice9* old = recovery_get_runner_device();
        if (old && old != (IDirect3DDevice9*)-1)
            old->Release(); // 旧设备(丢失态)整体释放

        // 适配器状态也可能损坏(CreateDevice 内部数组遍历 AV) →
        // 连同 IDirect3D9 接口一起重建。
        typedef IDirect3D9*(WINAPI* Direct3DCreate9_t)(UINT);
        HMODULE d3d9dll = GetModuleHandleA("d3d9.dll");
        Direct3DCreate9_t create9 = (Direct3DCreate9_t)GetProcAddress(d3d9dll, "Direct3DCreate9");
        IDirect3D9* nd3d9 = create9 ? create9(D3D_SDK_VERSION) : nullptr;
        if (!nd3d9)
            return E_FAIL;
        IDirect3DDevice9* nd = nullptr;
        HRESULT hr = nd3d9->CreateDevice(
            g_adapter, g_devtype, g_focuswin, g_bf, &g_pp9, &nd);
        if (FAILED(hr))
        {
            nd3d9->Release();
            return hr;
        }
        recovery_publish_runner_device(nd, nd3d9);
        gmdx9_install_device_hooks(nd);
        // [2026-09-14] 整设备重建成功: 旧设备上的一切对象(含 MANAGED/着色器/声明)
        // 已消亡, 通知注册方全量重建。旧设备已在上面 Release, DEFAULT 资源随之不存在,
        // 新设备干净无需 pre。
        gmdx9_fire_reset_post(true);
        return S_OK;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return E_FAIL;
    }
}

#ifdef GMDX9_RECOVERY_TEST
// ---- 测试专用注入点实现 ----
void gmdx9_test_clear_state(void)
{
    real_reset = nullptr;
    memset(&g_pp9, 0, sizeof(g_pp9));
    g_adapter = 0;
    g_devtype = D3DDEVTYPE_HAL;
    g_focuswin = nullptr;
    g_bf = 0;
    g_last_recreate = 0;
    t_pub_dev = nullptr;
    t_pub_d3d9 = nullptr;
}

void gmdx9_test_null_real_reset(void)
{
    real_reset = nullptr;
}

void gmdx9_test_publish(IDirect3DDevice9* dev, IDirect3D9* d3d9)
{
    recovery_publish_runner_device(dev, d3d9);
}

IDirect3DDevice9* gmdx9_test_published_dev(void)
{
    return t_pub_dev;
}

HRESULT gmdx9_test_force_recreate(void)
{
    return gmdx9_recreate_device();
}
#endif
