#include "main.h"

extern IDirect3DTexture9* white_pixel = nullptr;
extern D3DPRESENT_PARAMETERS* present_params;

D3DCAPS9 d3d_caps;
D3DPRESENT_PARAMETERS d3d_parameters = {
    .BackBufferCount = 1,
    .MultiSampleType = D3DMULTISAMPLE_NONE,
    .SwapEffect = D3DSWAPEFFECT_COPY,
    .Windowed = 1,
    .EnableAutoDepthStencil = TRUE,
    .AutoDepthStencilFormat = D3DFMT_D24S8,
    .PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE,
};

// 8.0 的 D3DX 是动态 LoadLibrary，runner 的加载器 (0x49A254) 用字符串 "\D3DX8.dll" 拼路径。
// 改指这个插件字符串，runner 就会加载 D3DX9_43.dll（插件随扩展分发）。
static const char d3dx9_dll_name[] = "\\D3DX9_43.dll";

HRESULT WINAPI CheckDeviceMultiSampleType(IDirect3D9* d3d, UINT Adapter,
    D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed,
    D3DMULTISAMPLE_TYPE MultiSampleType)
{
    return d3d->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat,
        Windowed, MultiSampleType, nullptr);
}

HRESULT WINAPI GetDisplayMode(IDirect3DDevice9* dev, D3DDISPLAYMODE* pMode)
{
    return dev->GetDisplayMode(0, pMode);
}

HRESULT WINAPI CreateImageSurface(IDirect3DDevice9* dev, UINT Width, UINT Height,
    D3DFORMAT Format, IDirect3DSurface9** ppSurface)
{
    return dev->CreateOffscreenPlainSurface(Width, Height, Format, D3DPOOL_SCRATCH,
        ppSurface, nullptr);
}

HRESULT WINAPI GetBackBuffer(IDirect3DDevice9* dev, UINT BackBuffer,
    D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer)
{
    return dev->GetBackBuffer(0, BackBuffer, Type, ppBackBuffer);
}

HRESULT WINAPI CreateVertexBuffer(IDirect3DDevice9* dev, UINT Length, DWORD Usage,
    DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer)
{
    return dev->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, nullptr);
}

HRESULT WINAPI SetStreamSource(IDirect3DDevice9* dev, UINT StreamNumber,
    IDirect3DVertexBuffer9* pStreamData, UINT Stride)
{
    return dev->SetStreamSource(StreamNumber, pStreamData, 0, Stride);
}

HRESULT WINAPI CreateDepthStencilSurface(IDirect3DDevice9* dev, UINT Width, UINT Height,
    D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface9** ppSurface)
{
    return dev->CreateDepthStencilSurface(Width, Height, Format, MultiSample, 0, FALSE,
        ppSurface, nullptr);
}

HRESULT WINAPI SetRenderTarget(IDirect3DDevice9* dev, IDirect3DSurface9* pRenderTarget,
    IDirect3DSurface9* pNewZStencil)
{
    HRESULT hr = dev->SetRenderTarget(0, pRenderTarget);
    if (SUCCEEDED(hr)) hr = dev->SetDepthStencilSurface(pNewZStencil);
    return hr;
}

HRESULT WINAPI GetRenderTarget(IDirect3DDevice9* dev, IDirect3DSurface9** ppRenderTarget)
{
    return dev->GetRenderTarget(0, ppRenderTarget);
}

HRESULT WINAPI CopyRects(IDirect3DDevice9* dev, IDirect3DSurface9* pSourceSurface,
    CONST RECT* pSourceRectsArray, UINT cRects, IDirect3DSurface9* pDestinationSurface,
    CONST POINT* pDestPointsArray)
{
    RECT destRect;
    destRect.left = pDestPointsArray->x;
    destRect.top = pDestPointsArray->y;
    destRect.right = destRect.left + (pSourceRectsArray->right - pSourceRectsArray->left);
    destRect.bottom = destRect.top + (pSourceRectsArray->bottom - pSourceRectsArray->top);
    // AddDirtyRect 不需要: D3D9 表面无法反向取父纹理, 目标多为 default-pool 表面, 正确性无碍。
    HRESULT hr = D3DXLoadSurfaceFromSurface(pDestinationSurface, nullptr, &destRect,
        pSourceSurface, nullptr, pSourceRectsArray, D3DX_FILTER_NONE, 0);
    return hr;
}

HRESULT WINAPI D3DXGetErrorStringA(HRESULT hr, LPSTR pBuffer, UINT BufferLen)
{
    const wchar_t* wstr = DXGetErrorStringW(hr);
    WideCharToMultiByte(CP_ACP, 0, wstr, -1, pBuffer, BufferLen, nullptr, nullptr);
    return S_OK;
}

HRESULT WINAPI screen_refresh(IDirect3DDevice9* dev, const RECT* pSourceRect,
    const RECT* pDestRect, HWND hDestOverride, const RGNDATA* pDirtyRegion)
{
    dev->EndScene();
    auto res = dev->Present(pSourceRect, pDestRect, hDestOverride, pDirtyRegion);
    dev->BeginScene();
    return res;
}

void WINAPI regain_device()
{
    // force exclusive fullscreen off
    d3d_parameters.Windowed = TRUE;
    d3d_parameters.FullScreen_RefreshRateInHz = 0;
    (*runner_display_reset)();
}

// SetVertexShader 包装: runner 每绘制 SetVertexShader(FVF)。有自定义 VS 时
// FVF 重置翻译成 SetVertexDeclaration 保持 VS 绑定, 否则透传 SetFVF。

// 引擎三种 FVF 顶点布局: shape 16B(pos+color) / 2d 24B(+uv) / 3d 36B(pos+normal+color+uv)。
static const D3DVERTEXELEMENT9 gm80_elems_shape[] = {
    {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
    {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
    D3DDECL_END()
};
static const D3DVERTEXELEMENT9 gm80_elems_2d[] = {
    {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
    {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
    {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
    D3DDECL_END()
};
static const D3DVERTEXELEMENT9 gm80_elems_3d[] = {
    {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
    {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
    {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
    {0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
    D3DDECL_END()
};
static IDirect3DVertexDeclaration9* gm80_decl_shape = nullptr;
static IDirect3DVertexDeclaration9* gm80_decl_2d = nullptr;
static IDirect3DVertexDeclaration9* gm80_decl_3d = nullptr;

static HRESULT gm80_ensure_decl(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9** out,
    const D3DVERTEXELEMENT9* elems)
{
    if (*out == nullptr)
    {
        HRESULT hr = dev->CreateVertexDeclaration(elems, out);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

// 仿固定管线 VS: 在 SM3.0 标准中，PS 的输入必须由 VS 输出，不能直接用 ps-only shader。
// 此 VS 仅做 WVP 变换, 透传 color/uv。
static IDirect3DVertexShader9* gm80_fake_ffp_vs = nullptr;
static const char gm80_fake_ffp_hlsl[] =
    "float4x4 uWVP : register(c0);"       "\n"
    "struct VS_IN {"                      "\n"
    "    float4 pos: POSITION;"           "\n"
    "    float4 color: COLOR0;"           "\n"
    "    float2 uv: TEXCOORD0;"           "\n"
    "};"                                  "\n"
    "struct VS_OUT {"                     "\n"
    "    float4 pos: POSITION;"           "\n"
    "    float4 color: COLOR0;"           "\n"
    "    float2 uv: TEXCOORD0;"           "\n"
    "};"                                  "\n"
    "VS_OUT main(VS_IN v) {"              "\n"
    "    VS_OUT o;"                       "\n"
    "    o.pos = mul(uWVP, v.pos);"       "\n"
    "    o.color = v.color;"              "\n"
    "    o.uv = v.uv;"                    "\n"
    "    return o;"                       "\n"
    "}"                                   "\n";

static HRESULT gm80_ensure_fake_ffp_vs(IDirect3DDevice9* dev)
{
    if (gm80_fake_ffp_vs) return S_OK;

    ID3DXBuffer *code = nullptr, *errs = nullptr;
    HRESULT hr = D3DXCompileShader(gm80_fake_ffp_hlsl,
        (UINT)sizeof(gm80_fake_ffp_hlsl) - 1, nullptr, nullptr, "main", "vs_3_0", 0,
        &code, &errs, nullptr);
    if (FAILED(hr))
    {
        if (errs) errs->Release();
        return hr;
    }

    hr = dev->CreateVertexShader((DWORD*)code->GetBufferPointer(), &gm80_fake_ffp_vs);
    code->Release();
    return hr;
}

// 刷新仿固定管线 VS 的 WVP 常量。钩子在 DrawPrimitiveUP 前触发,
// 此刻引擎本绘制所需的 SetTransform 已全部完成, GetTransform 必为当前值。
static HRESULT gm80_update_fake_ffp_wvp(IDirect3DDevice9* dev)
{
    D3DXMATRIX world, view, proj, wvp;
    dev->GetTransform(D3DTS_WORLD, &world);
    dev->GetTransform(D3DTS_VIEW, &view);
    dev->GetTransform(D3DTS_PROJECTION, &proj);

    D3DXMatrixMultiply(&wvp, &world, &view);
    D3DXMatrixMultiply(&wvp, &wvp, &proj);

    return dev->SetVertexShaderConstantF(0, &wvp._11, 4);
}

// 绑仿固定管线 VS: 声明按引擎 FVF 选 + WVP 常量 + VS。uWVP 是唯一 uniform → 编译器分配 c0-c3。
static HRESULT gm80_bind_fake_ffp(IDirect3DDevice9* dev, DWORD fvf)
{
    IDirect3DVertexDeclaration9** pdecl = nullptr;
    const D3DVERTEXELEMENT9* elems = nullptr;

    if (fvf == (D3DFVF_XYZ | D3DFVF_DIFFUSE))
    {
        pdecl = &gm80_decl_shape;
        elems = gm80_elems_shape;
    }
    else if (fvf == (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1))
    {
        pdecl = &gm80_decl_2d;
        elems = gm80_elems_2d;
    }
    else if (fvf == (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1))
    {
        pdecl = &gm80_decl_3d;
        elems = gm80_elems_3d;
    }
    else
        return D3DERR_INVALIDCALL;

    HRESULT hr = gm80_ensure_fake_ffp_vs(dev);
    if (FAILED(hr)) return hr;

    hr = gm80_ensure_decl(dev, pdecl, elems);
    if (FAILED(hr)) return hr;

    hr = gm80_update_fake_ffp_wvp(dev);
    if (FAILED(hr)) return hr;

    hr = dev->SetVertexShader(gm80_fake_ffp_vs);
    if (FAILED(hr)) return hr;

    return dev->SetVertexDeclaration(*pdecl);
}

// FFP VS 判定: 引擎绘制时设备上可能绑着 FFP VS: 本插件的仿固定管线 VS, 或插件经 
// gmdx9_register_ffp_vs 注册的 FFP VS。SetVertexShader 钩子识别后, 每次引擎
// 绘制前刷新 WVP 到当前投影。两套 FFP VS 共用 c0-c3 + mul(uWVP,pos) 约定, 
// 刷新函数通用。用户自定义 VS 不在此列(其常量自管)。
static bool is_passthrough_vs(IDirect3DVertexShader9* vs)
{
    if (vs == gm80_fake_ffp_vs)
        return true;
    for (int i = 0; i < gmdx9_ffp_vs_count(); i++)
    {
        void** slot = gmdx9_ffp_vs_slot(i);
        if (slot && *(IDirect3DVertexShader9**)slot == vs)
            return true;
    }
    return false;
}

HRESULT WINAPI SetVertexShader(IDirect3DDevice9* dev, DWORD fvf)
{
    IDirect3DVertexShader9* vs = nullptr;
    if (SUCCEEDED(dev->GetVertexShader(&vs)) && vs != nullptr)
    {
        // 自定义 VS 已绑定: 引擎的 FVF 重置 → 声明切换, VS 保持。
        // 注: D3D9 的 GetVertexShader 不 AddRef 返回对象, 无需 Release。
        HRESULT hr;
        IDirect3DVertexDeclaration9* decl = nullptr;
        if (fvf == (D3DFVF_XYZ | D3DFVF_DIFFUSE))
        {
            hr = gm80_ensure_decl(dev, &gm80_decl_shape, gm80_elems_shape);
            decl = gm80_decl_shape;
        }
        else if (fvf == (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1))
        {
            hr = gm80_ensure_decl(dev, &gm80_decl_2d, gm80_elems_2d);
            decl = gm80_decl_2d;
        }
        else if (fvf == (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1))
        {
            hr = gm80_ensure_decl(dev, &gm80_decl_3d, gm80_elems_3d);
            decl = gm80_decl_3d;
        }
        else
            return D3DERR_INVALIDCALL; // 未知 FVF + 自定义 VS: 引擎忽略返回值, 保持上次声明
        
        if (FAILED(hr)) return hr;
        // [2026-08-08] 若当前 VS 是本钩子绑的仿固定管线 VS(ps-only 场景), 每绘制刷一次 WVP
        // (投影可能已变, 如换视图/d3d_set_projection)。用户自定义 VS 不在此列(其常量自管)。
        // [2026-08-09] 注册模式: 同样识别插件注册的透传 VS(如 GMGraphic 的 s_passthrough_vs),
        // 引擎绘制时设备上可能是它绑的(ps-only shader), WVP 需在引擎 SetVertexShader 时刻刷新
        // 到当前投影(surface_set_target 重设后)。
        if (is_passthrough_vs(vs))
        {
            hr = gm80_update_fake_ffp_wvp(dev);
            if (FAILED(hr)) return hr;
        }
        return dev->SetVertexDeclaration(decl);
    }
    // 仿固定管线 VS 兜底: 无自定义 VS 但自定义 PS 激活(ps-only)时绑定喂 v0/v1;
    // 实测 ps_3_0 仍全透明 → GMGraphic 已回退 ps_2_0, 本分支留作 vs_3_0 透传 VS 实验。
    IDirect3DPixelShader9* ps = nullptr;
    if (SUCCEEDED(dev->GetPixelShader(&ps)) && ps != nullptr)
    {
        return gm80_bind_fake_ffp(dev, fvf);
    }
    return dev->SetFVF(fvf);
}

// SetViewport 接管: D3D9 viewport 超出 render target 只裁剪不收缩 → 表面渲染只捕左上角。
// 把 D3D_SetViewport 调用点(0x4a2432)重定向到本函数, 钳到当前 render target 尺寸。
HRESULT WINAPI SetViewport_inj(IDirect3DDevice9* dev, D3DVIEWPORT9* vp)
{
    IDirect3DSurface9* rt = nullptr;
    if (SUCCEEDED(dev->GetRenderTarget(0, &rt)))
    {
        D3DSURFACE_DESC desc;
        if (SUCCEEDED(rt->GetDesc(&desc)))
        {
            LONG x = vp->X, y = vp->Y;
            LONG w = (LONG)vp->Width, h = (LONG)vp->Height;
            if (x < 0)
            {
                w += x;
                x = 0;
            }
            if (y < 0)
            {
                h += y;
                y = 0;
            }
            if ((LONG)desc.Width - x < w) w = (LONG)desc.Width - x;
            if ((LONG)desc.Height - y < h) h = (LONG)desc.Height - y;
            if (w < 0) w = 0;
            if (h < 0) h = 0;
            vp->X = x;
            vp->Y = y;
            vp->Width = (DWORD)w;
            vp->Height = (DWORD)h;
        }
        rt->Release();
    }
    return dev->SetViewport(vp);
}

short old_cw = 0;
short new_cw = 0;

// Reset 接管: 8.0 runner 传 D3D8 布局 present params 给 Reset → D3D9 字段错位必失败。
// CreateDevice 成功后把 vtable 槽 0x40 改指 ResetDevice 包装, 用创建时的干净 pp9 副本调真 Reset。
static D3DPRESENT_PARAMETERS
    g_pp9; // 设备创建时的 pp9 副本(与 CreateDevice 完全一致 → Reset 必成功)
static HWND g_window = nullptr; // CreateDevice 传入的有效窗口
static HRESULT(WINAPI* real_reset)(
    IDirect3DDevice9*, D3DPRESENT_PARAMETERS*) = nullptr; // 原始 D3D9 Reset

// CheckDeviceMultiSampleType 接管: D3D9 比 D3D8 多第 6 参 pQualityLevels, 8.0 只传 5 参
// → D3D9 写栈垃圾。D3D 对象 vtable 槽 0x2C → 包装(补 &quality)。
static HRESULT(WINAPI* real_check_ms)(IDirect3D9*, UINT, D3DDEVTYPE, D3DFORMAT, BOOL,
    D3DMULTISAMPLE_TYPE, DWORD*) = nullptr;

HRESULT WINAPI CheckDeviceMultiSampleType_wrap(IDirect3D9* d3d9, UINT Adapter,
    D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed,
    D3DMULTISAMPLE_TYPE MultiSampleType)
{
    DWORD quality = 0;
    if (real_check_ms)
        return real_check_ms(d3d9, Adapter, DeviceType, SurfaceFormat, Windowed,
            MultiSampleType, &quality);
    return D3DERR_INVALIDCALL;
}

HRESULT WINAPI ResetDevice(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pParams)
{
    // 设备丢失恢复: Present 失败 → INNER_display_set_size → Reset。TestCooperativeLevel 判定:
    // NOTRESET=真 Reset(必成功, g_pp9 与 CreateDevice 一致); S_OK/LOST=no-op(避免每帧真 Reset 黑屏)。
    (void)pParams;
    HRESULT tcl = dev->TestCooperativeLevel();
    if (tcl == D3DERR_DEVICENOTRESET && real_reset)
        return real_reset(dev, &g_pp9);

    return S_OK;
}

// DLL 卸载安全: DLL 卸载而设备仍活着时, runner 再调 Reset 会跳未映射内存。
// DllMain(DETACH) 恢复 vtable 槽 0x40(仅当仍是我们的钩子), __try/__except 兜底设备已释放。
void gm80_restore_reset_hook(void)
{
    __try
    {
        IDirect3DDevice9* dev = *(IDirect3DDevice9**)0x58d388;
        if (!dev || !real_reset) return;
        void** vt = *(void***)dev;
        if (!vt) return;
        if (vt[16] != (void*)&ResetDevice) return; // 钩子已被别处改过 → 不碰
        DWORD oldp;
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

HRESULT WINAPI CreateDevice(IDirect3D9* d3d9, UINT Adapter, D3DDEVTYPE DeviceType,
    HWND hFocusWindow, DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9** ppReturnedDeviceInterface)
{
    // _control87 doesn't seem to let us reset the control word, so we're going asm
    if (old_cw == 0)
    {
        _asm fnstcw[old_cw];
        new_cw = old_cw | 0x3f;
        _asm fldcw[new_cw];
    }

    present_params = pPresentationParameters;

    // 分辨率可调修复: 保持 runner 传入的 backbuffer 尺寸(显示器), 不缩到首个房间视图
    // (8.0 换分辨率不调 Reset, backbuffer 恒不变; 缩了会被夹住, 可见区永远卡在初始尺寸)。

    // runner 栈上构造 D3D8 布局 present params → D3D9 字段错位。只采用开头 4 字段(布局一致), 其余干净重建 pp9。
    D3DPRESENT_PARAMETERS pp9;
    memset(&pp9, 0, sizeof(pp9));
    if (present_params)
    {
        pp9.BackBufferWidth = present_params->BackBufferWidth;
        pp9.BackBufferHeight = present_params->BackBufferHeight;
        pp9.BackBufferFormat = present_params->BackBufferFormat;
        pp9.BackBufferCount = present_params->BackBufferCount
            ? present_params->BackBufferCount
            : 1;
    }
    pp9.SwapEffect = D3DSWAPEFFECT_COPY;
    pp9.hDeviceWindow = hFocusWindow; // 不读 runner 的字段(错位垃圾); 焦点窗口已验证有效
    // GM8 只支持无边框全屏(2026-08-06 确认): "全屏"= runner 把窗口放大到显示尺寸,
    // 设备始终 windowed, Present 自动拉伸到窗口。绝不能设 Windowed=FALSE(D3D9 独占全屏 ≠ GM8 行为)。
    pp9.Windowed = TRUE;
    pp9.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    // 深度缓冲(2026-08-06, 对齐 gm82dx9 d3d_parameters): 启用 auto-depth-stencil,
    // 3D 游戏的 d3d_set_hidden(z-test)/d3d_clear_depth 依赖它。D24S8 不支持则回退 D16。
    pp9.EnableAutoDepthStencil = TRUE;
    pp9.AutoDepthStencilFormat = D3DFMT_D24S8;
    if (d3d9->CheckDeviceFormat(Adapter, DeviceType, pp9.BackBufferFormat,
            D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D24S8) != D3D_OK)
    {
        pp9.AutoDepthStencilFormat = D3DFMT_D16;
    }

    // 硬件 VP: 把 runner 的 0x22(SWVP|FPU_PRESERVE) 换成 0x42(HWVP), 失败则回退原始 flags。
    DWORD bf_orig = BehaviorFlags;
    DWORD bf_hw = (bf_orig & ~D3DCREATE_SOFTWARE_VERTEXPROCESSING) |
        D3DCREATE_HARDWARE_VERTEXPROCESSING;
    DWORD bf_used = bf_hw;

    auto res = d3d9->CreateDevice(
        Adapter, DeviceType, hFocusWindow, bf_hw, &pp9, ppReturnedDeviceInterface);
    if (FAILED(res) && bf_hw != bf_orig)
    {
        res = d3d9->CreateDevice(
            Adapter, DeviceType, hFocusWindow, bf_orig, &pp9, ppReturnedDeviceInterface);
        bf_used = SUCCEEDED(res) ? bf_orig : bf_hw;
    }
    // 全屏失败(D3DERR_NOTAVAILABLE 常见于基本显示适配器/远程桌面不支持全屏) → 回退窗口模式
    if (FAILED(res) && !pp9.Windowed)
    {
        pp9.Windowed = TRUE;
        if (!pp9.BackBufferWidth) pp9.BackBufferWidth = 640;
        if (!pp9.BackBufferHeight) pp9.BackBufferHeight = 480;
        res = d3d9->CreateDevice(
            Adapter, DeviceType, hFocusWindow, bf_used, &pp9, ppReturnedDeviceInterface);
    }

    // Reset 接管(2026-08-05): 设备创建成功后把 vtable 槽 0x40(Reset)重定向到 ResetDevice 包装。
    // 8.0 runner 的 Reset 调用(传 D3D8 布局 present params)经此包装用创建时的干净 pp9 调真实 Reset。
    if (SUCCEEDED(res) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface)
    {
        g_window = hFocusWindow;
        g_pp9 = pp9;
        if (!real_reset)
        {
            IDirect3DDevice9* dev = *ppReturnedDeviceInterface;
            void** vt = *(void***)dev;
            real_reset = (HRESULT(WINAPI*)(
                IDirect3DDevice9*, D3DPRESENT_PARAMETERS*))vt[16]; // 槽 0x40 = Reset
            DWORD oldp;
            if (VirtualProtect(&vt[16], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldp))
            {
                vt[16] = (void*)&ResetDevice;
                VirtualProtect(&vt[16], sizeof(void*), oldp, &oldp);
            }
        }
        // CheckDeviceMultiSampleType 接管: D3D 对象 vtable 槽 0x2C → 包装(补 pQualityLevels)。
        if (!real_check_ms)
        {
            void** d3d9_vt = *(void***)d3d9;
            real_check_ms = (HRESULT(WINAPI*)(IDirect3D9*, UINT, D3DDEVTYPE, D3DFORMAT,
                BOOL, D3DMULTISAMPLE_TYPE, DWORD*))d3d9_vt[11]; // 槽 0x2C/4 = 11
            DWORD oldp2;
            if (VirtualProtect(
                    &d3d9_vt[11], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldp2))
            {
                d3d9_vt[11] = (void*)&CheckDeviceMultiSampleType_wrap;
                VirtualProtect(&d3d9_vt[11], sizeof(void*), oldp2, &oldp2);
            }
        }
    }
    return res;
}

#define CW_INJ_START(name)                        \
    __declspec(naked) void name##_inj() { __asm { \
        __asm fldcw [old_cw]
#define CW_INJ_END() \
        __asm fldcw [new_cw] \
        __asm ret 0xc        \
        }                    \
    }
#define CW_INJ_ENDD()        \
        __asm fldcw [new_cw] \
        __asm ret 0x8        \
        }                    \
    }
#define CW_INJ_END2()            \
            __asm fldcw [new_cw] \
            __asm ret 0x18       \
        }                        \
    }


CW_INJ_START(sqrt)
    _asm fld tbyte ptr [esp + 4]
    _asm fsqrt
CW_INJ_END()

CW_INJ_START(ln)
    fld tbyte ptr [esp + 4]
    fldln2
    fxch
    fyl2x
CW_INJ_END()

CW_INJ_START(log2)
    fld1
    fld tbyte ptr [esp + 4]
    fyl2x
CW_INJ_END()

CW_INJ_START(log10)
    fldlg2
    fld tbyte ptr [esp + 4]
    fyl2x
CW_INJ_END()

CW_INJ_START(arcsin)
    fld qword ptr [esp + 4]
    fld1
    fadd st(0), st(1)
    fld1
    fsub st(0), st(2)
    fmulp st(1), st(0)
    fsqrt
    fpatan
CW_INJ_ENDD()

CW_INJ_START(arccos)
    fld qword ptr [esp + 4]
    fld1
    fadd st(0), st(1)
    fld1
    fsub st(0), st(2)
    fmulp st(1), st(0)
    fsqrt
    fxch
    fpatan
CW_INJ_ENDD()

CW_INJ_START(arctan)
    fld tbyte ptr [esp + 4]
    fld1
    fpatan
CW_INJ_END()

CW_INJ_START(arctan2)
    fld tbyte ptr [esp + 0x10]
    fld tbyte ptr [esp + 4]
    fpatan
CW_INJ_END2()

CW_INJ_START(logn)
    fld1
    fld tbyte ptr [esp + 4]
    fyl2x
    fld1
    fld tbyte ptr [esp + 0x10]
    fyl2x
    fdivp st(1), st(0)
CW_INJ_END2()

CW_INJ_START(exp)
    __asm push dword ptr [esp + 0xc]
    __asm push dword ptr [esp + 0xc]
    __asm push dword ptr [esp + 0xc]
    __asm mov eax, 0x404844
    __asm call eax
CW_INJ_END()

CW_INJ_START(power)
    __asm push dword ptr [esp + 0x10]
    __asm push dword ptr [esp + 0x10]
    __asm push dword ptr [esp + 0x10]
    __asm push dword ptr [esp + 0x10]
    __asm mov eax, 0x4103d8
    __asm call eax
    __asm fldcw [new_cw]
    __asm ret 0x10
}}

typedef D3DXMATRIX*(__stdcall* d3dx_matrix_func)(
    D3DXMATRIX* pOut, FLOAT w, FLOAT h, FLOAT zn, FLOAT zf);

d3dx_matrix_func D3DXMatrixPerspectiveLH_ptr, D3DXMatrixOrthoLH_ptr;

D3DXMATRIX* __stdcall D3DXMatrixPerspectiveLH_inj(
    D3DXMATRIX* pOut, FLOAT w, FLOAT h, FLOAT zn, FLOAT zf)
{
    _asm { fldcw [old_cw] }
    D3DXMatrixPerspectiveLH_ptr(pOut, w, h, zn, zf);
    _asm { fldcw [new_cw] }
}

D3DXMATRIX* __stdcall D3DXMatrixOrthoLH_inj(
    D3DXMATRIX* pOut, FLOAT w, FLOAT h, FLOAT zn, FLOAT zf)
{
    _asm { fldcw [old_cw] }
    D3DXMatrixOrthoLH_ptr(pOut, w, h, zn, zf);
    _asm { fldcw [new_cw] }
}

uint8_t white_pixel_tga[] = {
    0, 0, 2, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 1, 0, 1, 0,
    32, 0, 255, 255, 255, 255
};
void create_white_pixel()
{
    D3DXCreateTextureFromFileInMemory(Device, white_pixel_tga, 22, &white_pixel);
}

HRESULT WINAPI SetNullTexture(
    IDirect3DDevice9* dev, DWORD Stage, IDirect3DBaseTexture9* pTexture)
{
    return dev->SetTexture(0, white_pixel);
}

// 由 dllmain.cpp 的 DllMain 在 DLL_PROCESS_ATTACH 时调用。
bool gm80_apply_patches(void)
{
    HANDLE proc = GetCurrentProcess();

    void* ptr;
    uint16_t offset;

    // SDK 版本: 0x4a1e13 push 0x20(32)。必须 5 字节 `68 20 00 00 00` 与原 push 同长;
    // 2 字节 `6A 20` 会留下 3 字节被解码成 `add [eax],al` → 写 0x20 崩溃。
    {
        uint8_t push32[] = {0x68, 0x20, 0x00, 0x00, 0x00};
        WriteProcessMemory(proc, (void*)(0x4a1e13), push32, 5, nullptr);
    }

    // Direct3DCreate8→9：8.0 的 D3DCreate@0x484df4 内 `call sub_484DEC`(导入thunk) @0x484dff，
    // 把 rel32 改指 Direct3DCreate9（基址 0x400000，0x484dff+5=0x484e04）
    {
        ptr = (char*)(&Direct3DCreate9) - (0x484e04);
        WriteProcessMemory(proc, (void*)(0x484e00), &ptr, 4, nullptr);
    }

    // present-params 已接管: 8.0 的 present params 在栈上构造(无全局可重定向) → CreateDevice
    // 包装内重建干净 D3D9 pp9。

    // D3DCAPS 接管: D3DCAPS9 比 D3DCAPS8 大, runner 的 caps 缓冲会溢出 → 改指插件 d3d_caps。
    // 两个 push offset unk_6C7244 站点: 0x4a1f3e、0x4a2309。
    ptr = &d3d_caps;
    WriteProcessMemory(proc, (void*)(0x4a1f3e + 1), &ptr, 4, nullptr);
    WriteProcessMemory(proc, (void*)(0x4a2309 + 1), &ptr, 4, nullptr);

    // CheckDeviceMultiSampleType 已接管: 0x4a50ef 调 D3D 对象槽 0x2C, CreateDevice hook 里
    // 改指 wrap(补 &quality)。0x4a50fa 槽 0x20 = GetAdapterDisplayMode(签名相同, 无需接管)。

    // CreateDevice：8.0 sub_4A1DA0 的 CreateDevice 调用（call@0x4a1f1f，槽 0x3C），
    // 整段重定向到插件 CreateDevice 包装（含 present_params 捕获 + FPU 控制字）
#define PATCH(a)                                                                         \
    offset = 0xe8;                                                                       \
    WriteProcessMemory(proc, (void*)(a), &offset, 1, nullptr);                           \
    ptr = ((char*)(&CreateDevice) - (a + 5));                                            \
    WriteProcessMemory(proc, (void*)(a + 1), &ptr, 4, nullptr);

    PATCH(0x4a1f1d) // CreateDevice 重试调用 (sub_4A1DA0, call@0x4a1f1f)
    PATCH(0x4a1ee5) // CreateDevice 第一次尝试 (sub_4A1DA0, call@0x4a1ee7 槽 0x3C)
    // 第一个 CreateDevice(0x4a1ee7)实为槽 0x3C GetAdapterMonitor, 假成功不创建设备 → 必须一并重定向。
#undef PATCH

    // D3DX 接管: 把加载器 DLL 名字符串(\D3DX8.dll)改指插件 \D3DX9_43.dll
    ptr = (void*)d3dx9_dll_name;
    WriteProcessMemory(proc, (void*)(0x49a27b + 1), &ptr, 4, nullptr);

    // 写 14 个全局为 D3DX9_43.dll 导出指针。
    {
        HMODULE d3dx9 = GetModuleHandleA("D3DX9_43.dll");
        if (!d3dx9) d3dx9 = LoadLibraryA("D3DX9_43.dll");
        static const char* const d3dx9_names[14] = {
            "D3DXMatrixScaling", "D3DXMatrixTranslation", "D3DXMatrixRotationX",
            "D3DXMatrixRotationY", "D3DXMatrixRotationZ", "D3DXMatrixRotationAxis",
            "D3DXMatrixMultiply", "D3DXMatrixLookAtLH", "D3DXMatrixPerspectiveFovLH",
            "D3DXMatrixPerspectiveLH", "D3DXMatrixOrthoLH",
            "D3DXCheckTextureRequirements", "D3DXCreateTexture", "D3DXLoadSurfaceFromMemory",
        };
        void** dx = (void**)0x593868;
        for (int i = 0; i < 14; i++)
        {
            void* fn = d3dx9 ? (void*)GetProcAddress(d3dx9, d3dx9_names[i]) : nullptr;
            WriteProcessMemory(proc, (void*)(0x593868 + i * 4), &fn, 4, nullptr);
        }
    }

    // wrapper 重定向
    // 用 "E8 rel32" 替换 "mov eax,[eax]" 和 "call [eax+sz3]" 5字节；
    // sz6 站点用 "90 E8 rel32" 6字节等长替换。

    // SetRenderTarget (0x7C, sz3)
#define PATCH(a)                                                                         \
    offset = 0xe8;                                                                       \
    WriteProcessMemory(proc, (void*)(a), &offset, 1, nullptr);                           \
    ptr = ((char*)(&SetRenderTarget) - (a + 5));                                         \
    WriteProcessMemory(proc, (void*)(a + 1), &ptr, 4, nullptr);

    PATCH(0x4a0edb)
    PATCH(0x4a0f99)
#undef PATCH

    // CopyRects (0x70, sz3)
#define PATCH(a)                                                                         \
    offset = 0xe8;                                                                       \
    WriteProcessMemory(proc, (void*)(a), &offset, 1, nullptr);                           \
    ptr = ((char*)(&CopyRects) - (a + 5));                                               \
    WriteProcessMemory(proc, (void*)(a + 1), &ptr, 4, nullptr);

    PATCH(0x4a143f)
    PATCH(0x4a173d)
    PATCH(0x4a29a5)
#undef PATCH

    // GetDisplayMode (0x20, sz3)
#define PATCH(a)                                                                         \
    offset = 0xe8;                                                                       \
    WriteProcessMemory(proc, (void*)(a), &offset, 1, nullptr);                           \
    ptr = ((char*)(&GetDisplayMode) - (a + 5));                                          \
    WriteProcessMemory(proc, (void*)(a + 1), &ptr, 4, nullptr);

    PATCH(0x4a28f4)
#undef PATCH

    // CreateImageSurface (0x6C, sz3)
#define PATCH(a)                                                                         \
    offset = 0xe8;                                                                       \
    WriteProcessMemory(proc, (void*)(a), &offset, 1, nullptr);                           \
    ptr = ((char*)(&CreateImageSurface) - (a + 5));                                      \
    WriteProcessMemory(proc, (void*)(a + 1), &ptr, 4, nullptr);

    PATCH(0x4a13d0)
    PATCH(0x4a2922)
#undef PATCH

    // GetBackBuffer (0x40, sz3)
#define PATCH(a)                                                                         \
    offset = 0xe8;                                                                       \
    WriteProcessMemory(proc, (void*)(a), &offset, 1, nullptr);                           \
    ptr = ((char*)(&GetBackBuffer) - (a + 5));                                           \
    WriteProcessMemory(proc, (void*)(a + 1), &ptr, 4, nullptr);

    PATCH(0x4a294e)
#undef PATCH

    // screen_refresh 整段重定向不需要: 帧管线已用槽位补丁覆盖(BeginScene/EndScene/Present)。

    // GetRenderTarget (0x80, sz6)
#define PATCH(a)                                                                         \
    offset = 0xe890;                                                                     \
    WriteProcessMemory(proc, (void*)(a), &offset, 2, nullptr);                           \
    ptr = ((char*)(&GetRenderTarget) - (a + 6));                                         \
    WriteProcessMemory(proc, (void*)(a + 2), &ptr, 4, nullptr);

    PATCH(0x4a0eb7)
#undef PATCH

    // SetTexture(0, NULL)→SetNullTexture：8.0 的 SetTexture 站点已统一补到 0x104；
    // 是否区分"置 NULL"站点需要逐个确认参数，暂不启用 SetNullTexture 包装。

    // white pixel 初始化: 8.0 无 D3DXCreateTextureFromFileInMemoryEx 调用点, 已删除。

#define PATCH_SIMPLE(a, off)                                                             \
    offset = off;                                                                        \
    WriteProcessMemory(proc, (void*)(a + 2), &offset, 1, nullptr)

#define PATCH_BYTE(a, off)                                                               \
    offset = off;                                                                        \
    WriteProcessMemory(proc, (void*)(a + 1), &offset, 1, nullptr)

#define PATCH_DOUBLE(a, off)                                                             \
    offset = off;                                                                        \
    WriteProcessMemory(proc, (void*)(a + 2), &offset, 2, nullptr)

    // D3D8→D3D9 vtable 槽位重映射: 
    // sz6 = FF 90 disp32(写于 +2); sz3 = FF 5? disp8(写 1 字节于 +2)。
    
    // Reset (0x38→0x40, sz3)
    PATCH_SIMPLE(0x4a22ce, 0x40);
    PATCH_SIMPLE(0x4a22f6, 0x40);

    // Clear (0x90→0xAC, sz6)
    PATCH_SIMPLE(0x4a1f62, 0xac);
    PATCH_SIMPLE(0x4a232d, 0xac);
    PATCH_SIMPLE(0x49cb0b, 0xac);
    PATCH_SIMPLE(0x49cb40, 0xac);
    PATCH_SIMPLE(0x49e76a, 0xac);

    // SetViewport —— 重定向到 SetViewport_inj:
    // D3D9 的 viewport 不随 render target 收缩, 需手动钳到 render target 尺寸。
#define PATCH(a)                                                                         \
    offset = 0xe890;                                                                     \
    WriteProcessMemory(proc, (void*)(a), &offset, 2, nullptr);                           \
    ptr = ((char*)(&SetViewport_inj) - (a + 6));                                         \
    WriteProcessMemory(proc, (void*)(a + 2), &ptr, 4, nullptr)

    PATCH(0x4a2432);
#undef PATCH

    // SetMaterial：8.0 扫描未发现 0xA8 站点（8.1 有 0x56475e→0xc4），无需补丁。

    // SetLight (0xB0→0xCC, sz6)
    PATCH_SIMPLE(0x49f3eb, 0xcc);
    PATCH_SIMPLE(0x49f4dd, 0xcc);

    // LightEnable (0xB8→0xD4, sz6)
    PATCH_SIMPLE(0x49f50d, 0xd4);

    // SetTransform (0x94→0xB0, sz6)
    PATCH_SIMPLE(0x4a2555, 0xb0);
    PATCH_SIMPLE(0x4a259a, 0xb0);
    PATCH_SIMPLE(0x4a2685, 0xb0);
    PATCH_SIMPLE(0x4a26c6, 0xb0);
    PATCH_SIMPLE(0x49e8af, 0xb0);
    PATCH_SIMPLE(0x49e96b, 0xb0);
    PATCH_SIMPLE(0x49e9c2, 0xb0);
    PATCH_SIMPLE(0x49ead6, 0xb0);
    PATCH_SIMPLE(0x49eb19, 0xb0);
    PATCH_SIMPLE(0x49ec2b, 0xb0);
    PATCH_SIMPLE(0x49ec72, 0xb0);
    PATCH_SIMPLE(0x49ecae, 0xb0);
    PATCH_SIMPLE(0x49ecfc, 0xb0);
    PATCH_SIMPLE(0x49ed4c, 0xb0);
    PATCH_SIMPLE(0x49ed98, 0xb0);
    PATCH_SIMPLE(0x49edf4, 0xb0);
    PATCH_SIMPLE(0x49ee50, 0xb0);
    PATCH_SIMPLE(0x49eed6, 0xb0);
    PATCH_SIMPLE(0x49ef66, 0xb0);
    PATCH_SIMPLE(0x49efe6, 0xb0);
    PATCH_SIMPLE(0x49f062, 0xb0);
    PATCH_SIMPLE(0x49f0ee, 0xb0);
    PATCH_SIMPLE(0x49f17a, 0xb0);
    PATCH_SIMPLE(0x49f239, 0xb0);
    PATCH_SIMPLE(0x49f2dd, 0xb0);
    PATCH_SIMPLE(0x49f319, 0xb0);

    // GetTransform (0x98→0xB4, sz6)
    PATCH_SIMPLE(0x49ef38, 0xb4);
    PATCH_SIMPLE(0x49efb8, 0xb4);
    PATCH_SIMPLE(0x49f034, 0xb4);
    PATCH_SIMPLE(0x49f0c0, 0xb4);
    PATCH_SIMPLE(0x49f14c, 0xb4);
    PATCH_SIMPLE(0x49f20b, 0xb4);
    PATCH_SIMPLE(0x49f2a6, 0xb4);

    // SetRenderState (0xC8→0xE4, sz6)
    PATCH_SIMPLE(0x4a18a5, 0xe4);
    PATCH_SIMPLE(0x4a18b7, 0xe4);
    PATCH_SIMPLE(0x4a18c9, 0xe4);
    PATCH_SIMPLE(0x4a18db, 0xe4);
    PATCH_SIMPLE(0x4a18ef, 0xe4);
    PATCH_SIMPLE(0x4a1901, 0xe4);
    PATCH_SIMPLE(0x4a1925, 0xe4);
    PATCH_SIMPLE(0x4a193b, 0xe4);
    PATCH_SIMPLE(0x4a195e, 0xe4);
    PATCH_SIMPLE(0x4a1971, 0xe4);
    PATCH_SIMPLE(0x4a19b2, 0xe4);
    PATCH_SIMPLE(0x4a19cc, 0xe4);
    PATCH_SIMPLE(0x4a19ed, 0xe4);
    PATCH_SIMPLE(0x4a1a0b, 0xe4);
    PATCH_SIMPLE(0x4a1a40, 0xe4);
    PATCH_SIMPLE(0x4a1a58, 0xe4);
    PATCH_SIMPLE(0x4a1a70, 0xe4);
    PATCH_SIMPLE(0x4a1a84, 0xe4);
    PATCH_SIMPLE(0x4a1af7, 0xe4);
    PATCH_SIMPLE(0x4a1b0d, 0xe4);
    PATCH_SIMPLE(0x4a1b37, 0xe4);
    PATCH_SIMPLE(0x4a1b4b, 0xe4);
    PATCH_SIMPLE(0x4a1d15, 0xe4);

    // SetSamplerState (0xFC→0x114, sz6): 
    // sampler 站点需同时把 state 常量 D3DTSS_* 改成 D3DSAMP_*。
    PATCH_DOUBLE(0x4a1b79, 0x114); // MAGFILTER
    PATCH_DOUBLE(0x4a1b8d, 0x114); // MINFILTER
    PATCH_DOUBLE(0x4a1ba3, 0x114); // MAGFILTER
    PATCH_DOUBLE(0x4a1bb7, 0x114); // MINFILTER
    PATCH_DOUBLE(0x4a1ca5, 0x114); // ADDRESSU
    PATCH_DOUBLE(0x4a1cb9, 0x114); // ADDRESSV
    PATCH_DOUBLE(0x4a1ccf, 0x114); // ADDRESSU
    PATCH_DOUBLE(0x4a1ce3, 0x114); // ADDRESSV
    PATCH_DOUBLE(0x4a36e9, 0x114); // ADDRESSU
    PATCH_DOUBLE(0x4a36ff, 0x114); // ADDRESSV
    PATCH_DOUBLE(0x4a3849, 0x114); // ADDRESSU
    PATCH_DOUBLE(0x4a385f, 0x114); // ADDRESSV
    PATCH_DOUBLE(0x4a39b2, 0x114); // ADDRESSU
    PATCH_DOUBLE(0x4a39c8, 0x114); // ADDRESSV
    PATCH_DOUBLE(0x4a46a4, 0x114); // ADDRESSU
    PATCH_DOUBLE(0x4a46ba, 0x114); // ADDRESSV

    // sampler state 常量：D3DTSS_ADDRESSU(0x0D)→D3DSAMP_ADDRESSU(1)、ADDRESSV(0x0E)→2、
    // MAGFILTER(0x10)→5、MINFILTER(0x11)→6（写 push imm8 的 +1 字节）
    PATCH_BYTE(0x4a1b6d, D3DSAMP_MAGFILTER);
    PATCH_BYTE(0x4a1b97, D3DSAMP_MAGFILTER);
    PATCH_BYTE(0x4a1b81, D3DSAMP_MINFILTER);
    PATCH_BYTE(0x4a1bab, D3DSAMP_MINFILTER);
    PATCH_BYTE(0x4a1c99, D3DSAMP_ADDRESSU);
    PATCH_BYTE(0x4a1cc3, D3DSAMP_ADDRESSU);
    PATCH_BYTE(0x4a36db, D3DSAMP_ADDRESSU);
    PATCH_BYTE(0x4a383b, D3DSAMP_ADDRESSU);
    PATCH_BYTE(0x4a39a4, D3DSAMP_ADDRESSU);
    PATCH_BYTE(0x4a4696, D3DSAMP_ADDRESSU);
    PATCH_BYTE(0x4a1cad, D3DSAMP_ADDRESSV);
    PATCH_BYTE(0x4a1cd7, D3DSAMP_ADDRESSV);
    PATCH_BYTE(0x4a36f1, D3DSAMP_ADDRESSV);
    PATCH_BYTE(0x4a3851, D3DSAMP_ADDRESSV);
    PATCH_BYTE(0x4a39ba, D3DSAMP_ADDRESSV);
    PATCH_BYTE(0x4a46ac, D3DSAMP_ADDRESSV);

    // SetTextureStageState (0xFC→0x10C, sz6)
    // blending 的 COLOROP/COLORARG(1-6)，state 不变
    PATCH_DOUBLE(0x4a1be9, 0x10c);
    PATCH_DOUBLE(0x4a1bfd, 0x10c);
    PATCH_DOUBLE(0x4a1c11, 0x10c);
    PATCH_DOUBLE(0x4a1c25, 0x10c);
    PATCH_DOUBLE(0x4a1c39, 0x10c);
    PATCH_DOUBLE(0x4a1c4d, 0x10c);
    PATCH_DOUBLE(0x4a1c63, 0x10c);
    PATCH_DOUBLE(0x4a1c77, 0x10c);

    // BeginScene (0x88→0xA4, sz6)
    // EndScene (0x8C→0xA8, sz6)
    // Present (0x3C→0x44, sz3)
    PATCH_SIMPLE(0x4a26fc, 0xa4); // BeginScene
    PATCH_SIMPLE(0x4a2723, 0xa8); // EndScene
    PATCH_SIMPLE(0x4a27ab, 0x44); // Present
    PATCH_SIMPLE(0x4a2861, 0x44); // Present

    // 8.1 的 screen_refresh 重定向(0x6200c2)在 8.0 不需要, 帧管线已用槽位补丁覆盖。

    // SetTexture (0xF4→0x104, sz6)。8.0 未区分 NULL/非 NULL 站点, 暂统一补到 0x104(真实纹理)。
    PATCH_DOUBLE(0x49cb89, 0x104);
    PATCH_DOUBLE(0x49cc0e, 0x104);
    PATCH_DOUBLE(0x49cca0, 0x104);
    PATCH_DOUBLE(0x49cdf5, 0x104);
    PATCH_DOUBLE(0x49ceae, 0x104);
    PATCH_DOUBLE(0x49d043, 0x104);
    PATCH_DOUBLE(0x49d101, 0x104);
    PATCH_DOUBLE(0x49d20e, 0x104);
    PATCH_DOUBLE(0x49d3c7, 0x104);
    PATCH_DOUBLE(0x49d5ca, 0x104);
    PATCH_DOUBLE(0x49d7c2, 0x104);
    PATCH_DOUBLE(0x49dc8c, 0x104);
    PATCH_DOUBLE(0x4a359b, 0x104);
    PATCH_DOUBLE(0x4a35b1, 0x104);
    PATCH_DOUBLE(0x4a3715, 0x104);
    PATCH_DOUBLE(0x4a3875, 0x104);
    PATCH_DOUBLE(0x4a39de, 0x104);
    PATCH_DOUBLE(0x4a46d9, 0x104);

    // DrawPrimitive(0x118→0x144)：8.0 全模块扫描未发现 0x118 站点（8.1 有 0x568b87 等 3 处），
    // 8.0 只用 DrawPrimitiveUP(0x120)，无需补 DrawPrimitive。

    // DrawPrimitiveUP (0x120→0x14C, sz6)
    PATCH_DOUBLE(0x49cbb6, 0x14c);
    PATCH_DOUBLE(0x49cc3b, 0x14c);
    PATCH_DOUBLE(0x49ccc9, 0x14c);
    PATCH_DOUBLE(0x49ce1e, 0x14c);
    PATCH_DOUBLE(0x49ced7, 0x14c);
    PATCH_DOUBLE(0x49d06c, 0x14c);
    PATCH_DOUBLE(0x49d137, 0x14c);
    PATCH_DOUBLE(0x49d150, 0x14c);
    PATCH_DOUBLE(0x49d246, 0x14c);
    PATCH_DOUBLE(0x49d25f, 0x14c);
    PATCH_DOUBLE(0x49d3fd, 0x14c);
    PATCH_DOUBLE(0x49d416, 0x14c);
    PATCH_DOUBLE(0x49d602, 0x14c);
    PATCH_DOUBLE(0x49d61b, 0x14c);
    PATCH_DOUBLE(0x49d810, 0x14c);
    PATCH_DOUBLE(0x49d831, 0x14c);
    PATCH_DOUBLE(0x49dcdd, 0x14c);
    PATCH_DOUBLE(0x49dd01, 0x14c);
    PATCH_DOUBLE(0x49e1bf, 0x14c);
    PATCH_DOUBLE(0x49fb97, 0x14c);
    PATCH_DOUBLE(0x4a3741, 0x14c);
    PATCH_DOUBLE(0x4a38a1, 0x14c);
    PATCH_DOUBLE(0x4a3a0a, 0x14c);
    PATCH_DOUBLE(0x4a4919, 0x14c);

    // SetVertexShader (0x130→包装，e890 6字节重定向，sz6 原指令等长替换)
#define PATCH(a)                                                                         \
    offset = 0xe890;                                                                     \
    WriteProcessMemory(proc, (void*)(a), &offset, 2, nullptr);                           \
    ptr = ((char*)(&SetVertexShader) - (a + 6));                                         \
    WriteProcessMemory(proc, (void*)(a + 2), &ptr, 4, nullptr);

    PATCH(0x49cb9b);
    PATCH(0x49cc20);
    PATCH(0x49ccb2);
    PATCH(0x49ce07);
    PATCH(0x49cec0);
    PATCH(0x49d055);
    PATCH(0x49d113);
    PATCH(0x49d220);
    PATCH(0x49d3d9);
    PATCH(0x49d5dc);
    PATCH(0x49d7d4);
    PATCH(0x49dc9e);
    PATCH(0x49e1a6);
    PATCH(0x49fb7e);
    PATCH(0x4a372a);
    PATCH(0x4a388a);
    PATCH(0x4a39f3);
    PATCH(0x4a46ee);
#undef PATCH

    // GetSurfaceLevel（纹理接口 0x3C→0x48, sz3）
    PATCH_SIMPLE(0x4a316f, 0x48);
    PATCH_SIMPLE(0x4a35ed, 0x48);

    // GetDepthStencilSurface(0x84→0xa0)：8.0 全模块扫描未发现 0x84 站点（8.1 有 0x56b741）。
    // 8.0 深度缓冲经 off_58FC14 间接指针创建，无独立 GetDepthStencilSurface 调用，暂无需补丁。

    // LockRect (0x24→0x34, sz3) / UnlockRect (0x28→0x38, sz3)
    PATCH_SIMPLE(0x4a1466, 0x34); // LockRect
    PATCH_SIMPLE(0x4a2a40, 0x34); // LockRect
    PATCH_SIMPLE(0x4a3545, 0x34); // LockRect
    PATCH_SIMPLE(0x4a14d1, 0x38); // UnlockRect
    PATCH_SIMPLE(0x4a2a95, 0x38); // UnlockRect

    // D3DX 接管已实现: 8.0 动态加载(sub_49A254 填 0x593868–0x59389c), 已在 DllMain。

#define PATCH(addr, func)                                                                \
    ptr = (char*)(&func) - (addr + 5);                                                   \
    WriteProcessMemory(proc, (void*)(addr + 1), &ptr, 4, nullptr);

    // 设备丢失恢复 = ResetDevice 真 Reset; 卸载安全 = gm80_restore_reset_hook() 恢复 vtable。
    // 8.1 原版裸汇编挂钩(0x620012/0x5795c5)在 8.0 不安装。

    // 数学 FPU trampoline 不需要: CreateDevice 带 D3DCREATE_FPU_PRESERVE 生效, 实测 precision=1。

    // 投影矩阵 D3DX 注入不需要: 8.0 FPU 已实测不受 D3D9 影响(precision=1)。

    FlushInstructionCache(proc, nullptr, 0);

    return true;
}
