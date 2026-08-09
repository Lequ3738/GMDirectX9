#pragma once
// [GM80] 测试日志模块(结构对齐 GMSave)。GM80_LOG=0 时整体编译掉, GM_WRITE 退回裸 WriteProcessMemory。
// 写 %TEMP%\gm82dx9_port.log(追加) + OutputDebugStringA(DbgView 实时看)。
#include <windows.h>

#ifndef GM80_LOG
#define GM80_LOG 0   // [GM80] 插件收尾: 关掉日志
#endif

#if GM80_LOG
void gm_log(const char *fmt, ...);
int gm_readback(const char *what, void *addr, unsigned expect, int size);
extern int g_patch_failures;
// 写失败检测: gm80_apply_patches 内所有 WriteProcessMemory 经此宏(引用局部 proc), 失败即计数+报错。
#define GM_WRITE(addr, buf, len) do { \
    if (!WriteProcessMemory(proc, (void*)(addr), (buf), (len), nullptr)) { \
        g_patch_failures++; \
        gm_log("  !! WriteProcessMemory FAILED @0x%X (err=%u)", (unsigned)(size_t)(addr), (unsigned)GetLastError()); \
    } \
} while (0)
#else
#define gm_log(...) ((void)0)
#define GM_WRITE(addr, buf, len) WriteProcessMemory(proc, (void*)(addr), (buf), (len), nullptr)
#endif
