// tests/recovery_test.cpp —— 设备丢失恢复核心独立测试(无 GameMaker、不打任何补丁)
//
// 层1 假设备决策表: 手搓 vtable 冒充 IDirect3DDevice9, 脚本化 TestCooperativeLevel/
//     Release 行为, 断言 ResetDevice 的诚实语义(LOST 不碰硬件 / 透传真 Reset /
//     SEH 兜底重建 / 冷却 / 卸载恢复)。
// 层2 真 D3D9 集成: 真窗口+真设备走生产安装路径(gmdx9_recovery_on_device_created),
//     验证保存的真 Reset 可用、强制丢设备后经钩子诚实恢复。
//
// 运行输出 PASS/FAIL 清单, 进程退出码 = 失败数。

#ifndef GMDX9_RECOVERY_TEST // 工程里已定义, 防宏重定义警告
#define GMDX9_RECOVERY_TEST
#endif
#include "../source/d3d9_recovery.h"
#include "d3dx9.h"
#include <stdio.h>
#include <string.h>

// patch_support.cpp 的 reset 回调注册口(extern "C" dllexport; 测试直接链接声明)
extern "C" int __cdecl gmdx9_register_reset_callback(void (*pre)(void), void (*post)(bool));

// inject.cpp 的设备 vtable 钩子组安装在测试构建里为桩(recreate 路径引用; 假设备
// 不走真实 vtable 钩子组, 真 D3D9 部分也不依赖设备钩子 —— 只验恢复核心语义)。
bool gmdx9_install_render_hooks(IDirect3DDevice9*) { return true; }

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, name)                                                    \
    do                                                                       \
    {                                                                        \
        if (cond)                                                            \
        {                                                                    \
            printf("  [PASS] %s\n", name);                                   \
            ++g_pass;                                                        \
        }                                                                    \
        else                                                                 \
        {                                                                    \
            printf("  [FAIL] %s  (%s:%d)\n", name, __FILE__, __LINE__);      \
            ++g_fail;                                                        \
        }                                                                    \
    } while (0)

// ---------------- 假设备: 手搓 vtable(仅实现被调用的槽位) ----------------
// 槽位布局按 D3D9 官方: [2]=Release, [3]=TestCooperativeLevel, [16]=Reset
static void* g_fake_vtbl[18];
static void* g_fake_obj[1] = {(void*)g_fake_vtbl};
static IDirect3DDevice9* FakeDev(void) { return (IDirect3DDevice9*)g_fake_obj; }

static int g_tcl_calls = 0, g_rst_calls = 0, g_rel_calls = 0;
static HRESULT g_tcl_val = S_OK, g_rst_val = S_OK;
static BOOL g_rst_av = FALSE;

// 注意: 经 vtable 调用的 COM 方法会压入 this(x86 stdcall 由被调方清栈),
// 桩必须声明匹配的 this 形参, 否则每调用一次 ESP 漂移 4 字节 → 栈失衡崩溃。
static HRESULT __stdcall fake_tcl(IDirect3DDevice9*) { ++g_tcl_calls; return g_tcl_val; }
static ULONG __stdcall fake_release(IDirect3DDevice9*) { ++g_rel_calls; return 1; }
static HRESULT WINAPI fake_reset(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*)
{
    ++g_rst_calls;
    if (g_rst_av) *(volatile int*)0 = 0; // 注入硬崩溃 → 触发 SEH 兜底
    return g_rst_val;
}

static void reset_counters(void)
{
    g_tcl_calls = g_rst_calls = g_rel_calls = 0;
    g_tcl_val = S_OK;
    g_rst_val = S_OK;
    g_rst_av = FALSE;
}

static void arm_fake(void) // 装好假设备三件套
{
    for (auto& s : g_fake_vtbl) s = nullptr;
    g_fake_vtbl[2] = (void*)&fake_release;
    g_fake_vtbl[3] = (void*)&fake_tcl;
    g_fake_vtbl[16] = (void*)&fake_reset;
}

static HWND MakeWindow(const char* name)
{
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.lpszClassName = name;
    RegisterClassA(&wc);
    return CreateWindowExA(0, name, "recovery_test", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 320, 240, nullptr, nullptr,
        GetModuleHandleA(nullptr), nullptr);
}

// 生产安装路径的便捷封装(假设备版): 参数齐全, 供 recreate 使用
static void install_via_production(HWND wnd, HRESULT rst_val, BOOL av)
{
    arm_fake();
    g_tcl_val = S_OK;
    g_rst_val = rst_val;
    g_rst_av = av;
    D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth = 640;
    pp.BackBufferHeight = 480;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.Windowed = TRUE;
    pp.hDeviceWindow = wnd;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    gmdx9_recovery_on_device_created(FakeDev(), &pp, 0, D3DDEVTYPE_HAL, wnd, 0x42);
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0); // 不缓冲: 崩溃也能看到已打印进度
    printf("=== GMDirectX9 设备丢失恢复核心 测试 ===\n");
    HWND wnd = MakeWindow("gm82rec_main");

    // ---------- 层 1: 决策表 ----------
    printf("-- 层1 ResetDevice 决策表 --\n");

    // T1 LOST: 诚实上报, 不碰硬件
    gmdx9_test_clear_state();
    reset_counters();
    install_via_production(wnd, S_OK, FALSE);
    g_tcl_val = D3DERR_DEVICELOST;
    HRESULT hr = ResetDevice(FakeDev(), nullptr);
    CHECK(hr == D3DERR_DEVICELOST, "T1 DEVICELOST 原样上报");
    CHECK(g_rst_calls == 0, "T1 LOST 时真 Reset 零调用");

    // T2/T3 NOTRESET: 透传真 Reset 的真实返回值
    g_tcl_val = D3DERR_DEVICENOTRESET;
    g_rst_val = S_OK;
    hr = ResetDevice(FakeDev(), nullptr);
    CHECK(hr == S_OK && g_rst_calls == 1, "T2 NOTRESET→真Reset 成功透传");
    g_rst_val = E_FAIL;
    hr = ResetDevice(FakeDev(), nullptr);
    CHECK(hr == E_FAIL, "T3 真 Reset 失败码原样透传(交还 runner 重试)");

    // T4 S_OK(换分辨率等合法场景): 同样走真 Reset
    g_tcl_val = S_OK;
    g_rst_val = S_OK;
    hr = ResetDevice(FakeDev(), nullptr);
    CHECK(hr == S_OK && g_rst_calls == 3, "T4 S_OK 分支也执行真 Reset");

    // T5 钩子未安装: no-op 不崩
    gmdx9_test_clear_state();
    gmdx9_test_null_real_reset();
    reset_counters();
    g_tcl_val = D3DERR_DEVICENOTRESET;
    hr = ResetDevice(FakeDev(), nullptr);
    CHECK(hr == S_OK && g_rst_calls == 0, "T5 未安装钩子时安全 no-op");

    // T6 创建窗口已失效: 保护性拒绝
    gmdx9_test_clear_state();
    reset_counters();
    install_via_production(nullptr, S_OK, FALSE); // hDeviceWindow=null → IsWindow 失败
    g_tcl_val = S_OK;
    hr = ResetDevice(FakeDev(), nullptr);
    CHECK(hr == D3DERR_DEVICELOST && g_rst_calls == 0, "T6 无效窗口保护性拒绝");

    // T7 真 Reset 硬崩溃 → SEH 兜底 → 整设备重建(含真 D3D9 重建!)
    gmdx9_test_clear_state();
    reset_counters();
    install_via_production(wnd, S_OK, TRUE); // real_reset 一调用即 AV
    gmdx9_test_publish(FakeDev(), nullptr);  // 模拟 runner 全局持有旧设备
    hr = ResetDevice(FakeDev(), nullptr);
    CHECK(hr == D3DERR_DEVICELOST, "T7a SEH 兜底后按丢失上报");
    CHECK(g_rel_calls >= 1, "T7b 旧设备已被释放");
    IDirect3DDevice9* pub = gmdx9_test_published_dev();
    CHECK(pub != nullptr && pub != FakeDev(), "T7c 已发布新建设备");
    if (pub)
    {
        void** pvt = *(void***)pub;
        CHECK(pvt[16] == (void*)&ResetDevice, "T7d 新设备已重装 Reset 钩子");
    }

    // T8 重建冷却: 连续触发被拦
    hr = gmdx9_test_force_recreate();
    CHECK(hr == E_FAIL, "T8 重建冷却期内拒绝重复重建");

    // T9 卸载恢复: vt[16] 还原为 real_reset, 且幂等
    if (pub)
    {
        void** pvt = *(void***)pub;
        gm80_restore_reset_hook();
        CHECK(pvt[16] != (void*)&ResetDevice, "T9a 卸载后 vt[16] 不再指向包装");
        gm80_restore_reset_hook(); // 幂等, 不崩
        CHECK(true, "T9b 幂等调用安全");
        pub->Release(); // 回收 T7 重建产生的真设备(其 vt 已还原为安全指针)
        pub = nullptr;
    }

    // ---------- 层 1b: 设备 Reset 前后回调时序 ----------
    printf("-- 层1b Reset 前后回调 --\n");
    {
        // 具名函数保证重复注册传的是同一对指针(lambda 每个都是新函数, 测不了去重)
        static int pre_calls = 0, post_calls = 0;
        static int post_recreated_flags = 0;   // bit0: 见过 recreated=false; bit1: 见过 true
        struct Cbs
        {
            static void pre(void) { ++pre_calls; }
            static void post(bool recreated) { ++post_calls; post_recreated_flags |= recreated ? 2 : 1; }
            static void pre_dup(void) { ++pre_calls; }             // 不同指针(去重测试的第二对)
            static void post_dup(bool) { ++post_calls; }
        };
        reset_counters();
        gmdx9_test_clear_state();
        install_via_production(wnd, S_OK, FALSE);
        int rc0 = gmdx9_register_reset_callback(&Cbs::pre, &Cbs::post);
        CHECK(rc0 == 0, "T10a reset 回调注册成功");

        // T10 成功 Reset: pre 先于 post, post 携带 recreated=false
        pre_calls = post_calls = post_recreated_flags = 0;
        g_tcl_val = S_OK;
        g_rst_val = S_OK;
        hr = ResetDevice(FakeDev(), nullptr);
        CHECK(hr == S_OK && pre_calls == 1 && post_calls == 1, "T10b 成功 Reset: pre/post 各一次");
        CHECK((post_recreated_flags & 1) && !(post_recreated_flags & 2),
            "T10c 同设备 Reset 的 post 收到 recreated=false");

        // T11 失败 Reset: pre 触发但不 post(资源保持已释放态等重试)
        g_rst_val = E_FAIL;
        hr = ResetDevice(FakeDev(), nullptr);
        CHECK(hr == E_FAIL && pre_calls == 2 && post_calls == 1, "T11 失败 Reset: 只 pre 不 post");

        // T12 LOST 早退: 不触发 pre(未到 Reset 环节)
        g_tcl_val = D3DERR_DEVICELOST;
        hr = ResetDevice(FakeDev(), nullptr);
        CHECK(hr == D3DERR_DEVICELOST && pre_calls == 2 && post_calls == 1,
            "T12 LOST 早退: pre/post 均不触发");

        // T13 重复注册去重: 同一对回调二次注册不产生双触发
        rc0 = gmdx9_register_reset_callback(&Cbs::pre, &Cbs::post);
        CHECK(rc0 == 0, "T13a 同一对回调二次注册返回 0");
        g_tcl_val = S_OK;
        g_rst_val = S_OK;
        hr = ResetDevice(FakeDev(), nullptr);
        CHECK(hr == S_OK && pre_calls == 3 && post_calls == 2, "T13b 去重后仍单次触发");
    }
    // ---------- 层 2: 真 D3D9 集成 ----------
    printf("-- 层2 真 D3D9 集成 --\n");
    gmdx9_test_clear_state();
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    CHECK(d3d != nullptr, "R0 Direct3DCreate9 可用");
    if (d3d)
    {
        D3DPRESENT_PARAMETERS pp = {};
        pp.BackBufferWidth = 640;
        pp.BackBufferHeight = 480;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.SwapEffect = D3DSWAPEFFECT_COPY;
        pp.Windowed = TRUE;
        pp.hDeviceWindow = wnd;
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D24S8;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        IDirect3DDevice9* dev = nullptr;
        HRESULT cr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &dev);
        CHECK(cr == D3D_OK && dev, "R1 真设备创建");
        if (dev)
        {
            void** vt = *(void***)dev;
            void* orig16 = vt[16];
            void* slot14 = vt[14]; // D3D9 = GetSwapChain(旧 bug 误存的对象)
            gmdx9_recovery_on_device_created(dev, &pp,
                D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE);
            CHECK(vt[16] == (void*)&ResetDevice, "R2 钩子已安装到 vt[16]");
            CHECK(slot14 != orig16, "R3 vt[14]≠vt[16](旧 bug 取错对象的反证)");

            typedef HRESULT(WINAPI* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF204080, 1.0f, 0);

            D3DPRESENT_PARAMETERS junk = {}; // 钩子必须无视它, 用创建时的干净参数
            HRESULT rr = ((Reset_t)vt[16])(dev, &junk);
            CHECK(rr == D3D_OK, "R4 经钩子的真 Reset 成功(TCL=S_OK)");
            HRESULT pr = dev->Present(nullptr, nullptr, nullptr, nullptr);
            CHECK(pr == D3D_OK, "R5 Reset 后设备仍可渲染");

            // R6 强制丢设备: 同适配器再建独占全屏设备(经典手法; FSO 模拟环境可能不复现)
            HWND wnd2 = MakeWindow("gm82rec_fs");
            D3DPRESENT_PARAMETERS pp2 = {};
            pp2.BackBufferWidth = 640;
            pp2.BackBufferHeight = 480;
            pp2.BackBufferFormat = D3DFMT_X8R8G8B8;
            pp2.SwapEffect = D3DSWAPEFFECT_FLIP;
            pp2.Windowed = FALSE;
            pp2.hDeviceWindow = wnd2;
            IDirect3DDevice9* dev2 = nullptr;
            HRESULT cr2 = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd2,
                D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp2, &dev2);
            bool saw_lost = false;
            if (cr2 == D3D_OK && dev2)
            {
                for (int i = 0; i < 60; ++i) // ≤3s 观察
                {
                    if (dev->TestCooperativeLevel() != S_OK) { saw_lost = true; break; }
                    Sleep(50);
                }
            }
            if (!saw_lost)
            {
                printf("  [SKIP] R6 本机独占全屏未使窗口设备丢失(Win10+ FSO 模拟属预期)\n");
            }
            else
            {
                CHECK(dev->TestCooperativeLevel() == D3DERR_DEVICELOST,
                    "R6a 设备确实进入 LOST");
                HRESULT rl = ((Reset_t)vt[16])(dev, &junk);
                CHECK(rl == D3DERR_DEVICELOST, "R6b LOST 态下钩子诚实上报, 不碰硬件");
                if (dev2) { dev2->Release(); dev2 = nullptr; } // 移除元凶
                bool recovered = false;
                for (int i = 0; i < 100; ++i) // ≤10s 等可复位并自愈
                {
                    Sleep(100);
                    if (((Reset_t)vt[16])(dev, &junk) == D3D_OK &&
                        dev->TestCooperativeLevel() == S_OK)
                    {
                        recovered = true;
                        break;
                    }
                }
                CHECK(recovered, "R6c 经钩子自愈成功(原生循环语义)");
                if (recovered)
                {
                    CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr) == D3D_OK,
                        "R6d 自愈后渲染正常");
                }
            }

            // R7 卸载恢复(真设备): 发布→restore→vt[16] 还原为真 Reset
            gmdx9_test_publish(dev, d3d);
            gm80_restore_reset_hook();
            CHECK(vt[16] == orig16, "R7 卸载后 vt[16] 恢复为原始真 Reset");
            gmdx9_test_clear_state();

            // R8 GetVertexShader 引用语义(决定 inject.cpp SetVertexShader 钩子的 Release 纪律):
            // 基线 c0 = 创建引用(1) + AddRef 返回; Set 不应加引用; Get 每次调用加一引用
            // 则 c1 == c0 + 1。(GMGraphic 全部 Guard 按"Get 带 AddRef"写且实机不崩, 本用例实证。)
            {
                const char* vs_hlsl =
                    "float4 main(float4 p : POSITION) : POSITION { return p; }";
                ID3DXBuffer* code = nullptr;
                HRESULT hc = D3DXCompileShader(vs_hlsl, (UINT)strlen(vs_hlsl),
                    nullptr, nullptr, "main", "vs_1_1", 0, &code, nullptr, nullptr);
                CHECK(hc == D3D_OK && code, "R8a 最小 vs_1_1 编译成功");
                if (code)
                {
                    IDirect3DVertexShader9* vs = nullptr;
                    HRESULT hv = dev->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &vs);
                    code->Release();
                    CHECK(hv == D3D_OK && vs, "R8b CreateVertexShader 成功");
                    if (vs)
                    {
                        ULONG c0 = vs->AddRef(); vs->Release();   // ≈1(创建)+1
                        dev->SetVertexShader(vs);
                        ULONG cS = vs->AddRef(); vs->Release();
                        CHECK(cS == c0, "R8c SetVertexShader 不加引用");
                        IDirect3DVertexShader9* got = nullptr;
                        HRESULT hg = dev->GetVertexShader(&got);
                        ULONG c1 = vs->AddRef(); vs->Release();
                        CHECK(hg == D3D_OK && got == vs, "R8d GetVertexShader 返回绑定对象");
                        bool get_addrefs = (c1 == c0 + 1);
                        CHECK(get_addrefs, "R8e GetVertexShader 每次 Get 加一引用");
                        // 归零清理: 解绑 + 创建引用 + (若 Get 加引用)Get 引用
                        dev->SetVertexShader(nullptr);
                        vs->Release();
                        if (get_addrefs && got) got->Release();
                    }
                }
            }

            if (dev2) dev2->Release();
            dev->Release();
        }
        d3d->Release();
    }
    DestroyWindow(wnd);

    printf("=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail;
}
