#pragma once

#define GMREAL extern "C" __declspec(dllexport) double __cdecl
#define GMSTR extern "C" __declspec(dllexport) char* __cdecl

#define create_c_function(rettype, name, addr, ...)                                      \
    rettype (*name)(__VA_ARGS__) = (rettype(*)(__VA_ARGS__))addr;

#define _USE_MATH_DEFINES

#include <stdio.h>
#include <math.h>
#include <windows.h>
#include <versionhelpers.h>
#include <array>
#include <map>

#include "d3dx9.h"

typedef int(__cdecl* DLL_FUNC)();
typedef struct
{
    int is_string;
    int padding;
    double real;
    char* string;
    int padding2;
} GMVAL;

extern IDirect3DDevice9** d3d9_device;
#define Device (*d3d9_device)

extern D3DPRESENT_PARAMETERS* present_params;

// [GM80] dllmain.cpp ↔ inject.cpp 共享
extern HINSTANCE my_handle;
bool gm80_apply_patches(void);
void gm80_restore_reset_hook(void); // DLL 卸载时恢复 vtable Reset 钩子(2026-08-06)
void gm80_restore_device_hooks(void); // DLL 卸载时恢复 vtable 钩子组: SetTexture(白像素)+flush 全族(2026-09-14 二批扩至提交/状态/内容 43 槽)
bool gmdx9_install_render_hooks(IDirect3DDevice9* dev); // 设备 vtable 钩子组安装(CreateDevice 与 recovery 重建路径共用)
void gmdx9_fire_flush(void); // flush 钩子入口(提交/目标/状态族): 依次调用注册的插件 flush 回调(patch_support)
void gmdx9_fire_reset_pre(void);   // 设备 Reset 前回调(注册口 gmdx9_register_reset_callback, 见 patch_support)
void gmdx9_fire_reset_post(bool recreated); // 设备 Reset/重建成功后回调(recreated=true = 整设备重建)

HRESULT WINAPI SetVertexShader(IDirect3DDevice9* dev, DWORD fvf);

// FFP VS 注册槽查询(patch_support 定义, inject.cpp 的 SetVertexShader 钩子使用)
int  gmdx9_ffp_vs_count(void);
void** gmdx9_ffp_vs_slot(int i);
