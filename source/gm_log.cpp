#include "gm_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// [GM80] 日志实现: 写 %TEMP%\gm82dx9_port.log(追加) + OutputDebugStringA。
// 注意: DllMain/gm80_apply_patches 运行在 loader lock 下 —— 只用 kernel32 文件函数 + 纯栈上 sprintf,
// 避免 CRT fopen/fprintf(可能取 loader lock 而死锁)。sprintf_s/vsprintf_s 纯计算, 安全。
#if GM80_LOG
int g_patch_failures = 0;

void gm_log(const char *fmt, ...) {
    static char path[MAX_PATH];
    static bool path_built = false;
    char line[1024];
    int len = 0;
    if (!path_built) {
        GetTempPathA(sizeof(path), path);
        strcat_s(path, sizeof(path), "gm82dx9_port.log");
        path_built = true;
    }
    DWORD pid = GetCurrentProcessId();
    DWORD ms = GetTickCount();  // 相对开机毫秒, 仅用于排序
    len += sprintf_s(line + len, sizeof(line) - len, "[pid=%u +%u] ", pid, ms);
    va_list ap;
    va_start(ap, fmt);
    len += vsprintf_s(line + len, sizeof(line) - len, fmt, ap);
    va_end(ap);
    if (len < 0) len = 0;
    if (len > (int)sizeof(line) - 3) len = (int)sizeof(line) - 3;
    line[len++] = '\r';
    line[len++] = '\n';
    line[len] = '\0';
    OutputDebugStringA(line);
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(h, line, (DWORD)len, &written, NULL);
        CloseHandle(h);
    }
}

// 读回验证: 读 n 字节与期望值比对, 用于 gm80_apply_patches 末尾确认补丁真写进去了。
int gm_readback(const char *what, void *addr, unsigned expect, int size) {
    unsigned long long val = 0;
    SIZE_T n = 0;
    ReadProcessMemory(GetCurrentProcess(), addr, &val, (SIZE_T)size, &n);
    unsigned got = (unsigned)val;
    bool ok = (n == (SIZE_T)size && got == expect);
    gm_log("  verify %-26s @0x%p = %08X%s", what, addr, got, ok ? "" : "  <<< MISMATCH");
    return ok ? 0 : 1;
}
#endif
