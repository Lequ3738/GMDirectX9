// TssProbe —— 实测本机 D3D9 运行时对"枚举间隙"TSS 类型(13/14/16/17)的真实处理。
// 背景: GMGraphic 共享代码按 D3D8 枚举发 TSS(这套 d3d8.h: ADDRESSU=13/ADDRESSV=14/
// BORDERCOLOR=16/MAGFILTER=17/MINFILTER=18), 而 D3D9 的 TSS 枚举在 11(TEXCOORDINDEX)
// 与 22(BUMPENVLSCALE) 之间是空隙, 地址/过滤全部走 D3DSAMP。本探针回答:
//   1) SetTextureStageState(0, 13/14/16/17, v) 返回成功还是 INVALIDCALL?
//   2) 若成功, 值落在哪个内部状态(Get 对照 TEXCOORDINDEX=11 / BUMPENVL*=22/23 /
//      TEXTURETRANSFORMFLAGS=24 / 各 D3DSAMP)?
//   3) D3D9 正宗的 TSS 25/26(D3D8 头没有)与 D3DSAMP 1/2 的读写对称性。
#include <windows.h>
#include <d3d9.h>
#include <cstdio>

#pragma comment(lib, "d3d9.lib")

static const char* HR(HRESULT hr)
{
    switch (hr)
    {
        case D3D_OK: return "S_OK";
        case D3DERR_INVALIDCALL: return "INVALIDCALL";
        default: return "OTHER";
    }
}

int main()
{
    HWND hwnd = CreateWindowA("STATIC", "tss", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
        nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("no d3d9\n"); return 1; }
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &dev);
    if (FAILED(hr))
    {
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &dev);
        if (FAILED(hr)) { printf("no device (%s)\n", HR(hr)); return 1; }
        printf("[device] SWVP\n");
    }
    else printf("[device] HWVP\n");

    DWORD v = 0;
    // 基线: 默认值
    dev->GetTextureStageState(0, (D3DTEXTURESTAGESTATETYPE)11, &v);
    printf("[base ] TEXCOORDINDEX(11)      = %lu\n", (unsigned long)v);
    dev->GetTextureStageState(0, (D3DTEXTURESTAGESTATETYPE)24, &v);
    printf("[base ] TEXTURETRANSFORM(24)   = %lu\n", (unsigned long)v);
    dev->GetSamplerState(0, D3DSAMP_ADDRESSU, &v);
    printf("[base ] SAMP_ADDRESSU(1)       = %lu\n", (unsigned long)v);
    dev->GetSamplerState(0, D3DSAMP_MAGFILTER, &v);
    printf("[base ] SAMP_MAGFILTER(5)      = %lu\n", (unsigned long)v);

    // GMGraphic 共享代码实际发出的写(这套 d3d8.h 枚举值)
    struct { const char* name; DWORD type; DWORD val; } writes[] = {
        { "SetTSS(13)=CLAMP(3)  [d3d8 ADDRESSU]", 13, 3 },
        { "SetTSS(14)=CLAMP(3)  [d3d8 ADDRESSV]", 14, 3 },
        { "SetTSS(16)=POINT(1)  [d3d8 BORDERCOLOR]", 16, 1 },
        { "SetTSS(17)=LINEAR(2) [d3d8 MAGFILTER]", 17, 2 },
        { "SetTSS(18)=LINEAR(2) [d3d8 MINFILTER]", 18, 2 },
    };
    for (auto& w : writes)
    {
        HRESULT r = dev->SetTextureStageState(0, (D3DTEXTURESTAGESTATETYPE)w.type, w.val);
        printf("[write] %s -> %s\n", w.name, HR(r));
    }

    // 落点核对
    dev->GetTextureStageState(0, (D3DTEXTURESTAGESTATETYPE)11, &v);
    printf("[after] TEXCOORDINDEX(11)      = %lu\n", (unsigned long)v);
    dev->GetTextureStageState(0, (D3DTEXTURESTAGESTATETYPE)22, &v);
    printf("[after] BUMPENVLSCALE(22)      = %lu\n", (unsigned long)v);
    dev->GetTextureStageState(0, (D3DTEXTURESTAGESTATETYPE)23, &v);
    printf("[after] BUMPENVLOFFSET(23)     = %lu\n", (unsigned long)v);
    dev->GetTextureStageState(0, (D3DTEXTURESTAGESTATETYPE)24, &v);
    printf("[after] TEXTURETRANSFORM(24)   = %lu\n", (unsigned long)v);
    dev->GetSamplerState(0, D3DSAMP_ADDRESSU, &v);
    printf("[after] SAMP_ADDRESSU(1)       = %lu\n", (unsigned long)v);
    dev->GetSamplerState(0, D3DSAMP_ADDRESSV, &v);
    printf("[after] SAMP_ADDRESSV(2)       = %lu\n", (unsigned long)v);
    dev->GetSamplerState(0, D3DSAMP_MAGFILTER, &v);
    printf("[after] SAMP_MAGFILTER(5)      = %lu\n", (unsigned long)v);
    dev->GetSamplerState(0, D3DSAMP_MINFILTER, &v);
    printf("[after] SAMP_MINFILTER(6)      = %lu\n", (unsigned long)v);

    // D3D9 正宗 TSS 地址态(25/26)是否可写可读
    HRESULT r25 = dev->SetTextureStageState(0, (D3DTEXTURESTAGESTATETYPE)25, 3);
    DWORD g25 = 0xdeadbeef;
    dev->GetTextureStageState(0, (D3DTEXTURESTAGESTATETYPE)25, &g25);
    printf("[probe] SetTSS(25)=3 -> %s ; GetTSS(25) = %lu ; SAMP_ADDRESSU now = ",
        HR(r25), (unsigned long)g25);
    dev->GetSamplerState(0, D3DSAMP_ADDRESSU, &v);
    printf("%lu\n", (unsigned long)v);

    // D3DSAMP 正宗写
    HRESULT rs = dev->SetSamplerState(0, D3DSAMP_ADDRESSU, 3);
    dev->GetSamplerState(0, D3DSAMP_ADDRESSU, &v);
    printf("[probe] SetSAMP(1)=3 -> %s ; readback = %lu\n", HR(rs), (unsigned long)v);

    dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    return 0;
}
