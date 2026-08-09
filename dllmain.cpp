// GMDirectX9 — gm82dx9 移植到 GameMaker 8.0（DirectX8→DirectX9 渲染后端升级插件）
// DLL entry point. 结构对齐 GMSave：DllMain 放根目录，补丁主体在 GMDirectX9/inject.cpp（gm80_apply_patches）。
#include "pch.h"
#include "gm82dx9.h"
#include "gm_log.h"

HINSTANCE my_handle;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        my_handle = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
        gm_log("=== GMDirectX9 DllMain entry ===");
        gm80_apply_patches();   // 全部补丁（SDK/D3D9/D3DCAPS/D3DX/vtable 重映射）在 inject.cpp
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        // [GM80] 卸载安全: 恢复 vtable 槽 0x40(Reset), 避免卸载后跳未映射内存。
        gm80_restore_reset_hook();
        gm_log("=== GMDirectX9 DllMain detach ===");
    }
    return TRUE;
}
