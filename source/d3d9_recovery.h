#pragma once
// ============ 设备丢失恢复核心(2026-08-22 从 inject.cpp 拆出) ============
// 机制对齐 GM8.0 runner 原生协议(PORTING_NOTES §20):
//   Present 失败 → runner INNER_display_set_size(释放全部表面+现构造参数)
//                → ResetDevice(本模块; 挂在 D3D9 设备 vtable 槽 16/0x40)。
// 与 runner 的耦合仅剩两处全局读写(0x58d388/0x58d38C), 收敛于 recovery_get/
// publish_runner_device —— 测试构建(GMDX9_RECOVERY_TEST)下替换为桩存储,
// 使本模块可脱离 GameMaker 单测(tests/recovery_test.cpp)。

#include <windows.h>
#include <d3d9.h>

// 真设备创建成功后由 inject.cpp 的 CreateDevice 包装调用:
// 保存干净 pp9 与创建参数(供整设备重建复用), 安装 Reset 钩子。
// 必须在覆盖前从 vt[16] 保存真 Reset —— D3D9 官方布局:
// vt[14]=GetSwapChain / vt[15]=GetNumberOfSwapChains /
// vt[16](0x40)=Reset / vt[17](0x44)=Present, 切勿按 D3D8 布局取槽。
void gmdx9_recovery_on_device_created(IDirect3DDevice9* dev,
    const D3DPRESENT_PARAMETERS* pp9,
    UINT adapter, D3DDEVTYPE devtype, HWND focuswin, DWORD behaviorflags);

// Reset vtable 槽包装(语义见 .cpp); 同时是写入设备 vtable 的钩子目标地址。
HRESULT WINAPI ResetDevice(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pParams);

// DLL 卸载时恢复 vt[16] 为真 Reset(dllmain DLL_PROCESS_DETACH 调用)。
void gm80_restore_reset_hook(void);

#ifdef GMDX9_RECOVERY_TEST
// ---- 仅测试构建存在 ----
void       gmdx9_test_clear_state(void);      // 清空全部静态状态(含发布桩/冷却时间戳)
void       gmdx9_test_publish(IDirect3DDevice9* dev, IDirect3D9* d3d9);
void       gmdx9_test_null_real_reset(void);  // 模拟"钩子未安装"分支
IDirect3DDevice9* gmdx9_test_published_dev(void);
HRESULT    gmdx9_test_force_recreate(void);   // 直接触发重建路径(绕过 SEH)
#endif
