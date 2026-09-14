#include "main.h"
#include "d3d9_recovery.h"
#include "state_shadow.h"

extern IDirect3DTexture9* white_pixel = nullptr;
extern D3DPRESENT_PARAMETERS* present_params;

D3DCAPS9 d3d_caps;

// 8.0 的 D3DX 是动态 LoadLibrary，runner 的加载器 (0x49A254) 用字符串 "\D3DX8.dll" 拼路径。
// 改指这个插件字符串，runner 就会加载 D3DX9_43.dll（插件随扩展分发）。
static const char d3dx9_dll_name[] = "\\D3DX9_43.dll";

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

// [2026-09-14] 防御完整化: D3D8 语义允许 pSourceRectsArray=NULL(整面)与
// pDestPointsArray=NULL(目标=源左上角), 且 cRects 可 >1 —— 逐矩形全部搬运。
// IDA 已验证 runner 三个站点恒 cRects=1 且两数组非空, 此处为纵深防御。
HRESULT WINAPI CopyRects(IDirect3DDevice9* dev, IDirect3DSurface9* pSourceSurface,
    CONST RECT* pSourceRectsArray, UINT cRects, IDirect3DSurface9* pDestinationSurface,
    CONST POINT* pDestPointsArray)
{
    if (cRects == 0 || !pSourceSurface || !pDestinationSurface)
        return D3DERR_INVALIDCALL;

    // 源矩形缺省 = 整个源表面
    RECT srcFull{};
    if (!pSourceRectsArray)
    {
        D3DSURFACE_DESC d{};
        if (FAILED(pSourceSurface->GetDesc(&d))) return D3DERR_INVALIDCALL;
        srcFull.right = (LONG)d.Width;
        srcFull.bottom = (LONG)d.Height;
        pSourceRectsArray = &srcFull;
    }

    // AddDirtyRect 不需要: D3D9 表面无法反向取父纹理, 目标多为 default-pool 表面, 正确性无碍。
    HRESULT hr = S_OK;
    for (UINT i = 0; i < cRects; ++i)
    {
        const RECT& src = pSourceRectsArray[i];
        POINT dstPt{ 0, 0 };
        if (pDestPointsArray)
            dstPt = pDestPointsArray[i];
        RECT dstRect;
        dstRect.left = dstPt.x;
        dstRect.top = dstPt.y;
        dstRect.right = dstRect.left + (src.right - src.left);
        dstRect.bottom = dstRect.top + (src.bottom - src.top);
        hr = D3DXLoadSurfaceFromSurface(pDestinationSurface, nullptr, &dstRect,
            pSourceSurface, nullptr, &src, D3DX_FILTER_NONE, 0);
        if (FAILED(hr)) return hr;
    }
    return hr;
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

static HRESULT gm80_compile_fake_ffp_vs(IDirect3DDevice9* dev, const char* hlsl,
    size_t len, IDirect3DVertexShader9** out)
{
    ID3DXBuffer *code = nullptr, *errs = nullptr;
    HRESULT hr = D3DXCompileShader(hlsl,
        (UINT)len, nullptr, nullptr, "main", "vs_3_0", 0,
        &code, &errs, nullptr);
    if (FAILED(hr))
    {
        if (errs) errs->Release();
        return hr;
    }

    hr = dev->CreateVertexShader((DWORD*)code->GetBufferPointer(), out);
    code->Release();
    return hr;
}

static HRESULT gm80_ensure_fake_ffp_vs(IDirect3DDevice9* dev)
{
    if (gm80_fake_ffp_vs) return S_OK;
    return gm80_compile_fake_ffp_vs(dev, gm80_fake_ffp_hlsl,
        sizeof(gm80_fake_ffp_hlsl) - 1, &gm80_fake_ffp_vs);
}

// [2026-08-26] 形状签名仿固定管线 VS: 输入仅 POSITION+COLOR0(精确匹配引擎 shape 布局,
// 16 字节 stride 无 UV)。输出仍声明 TEXCOORD0=(0,0), 保证 ps_3_0 的 dcl_texcoord v0
// 拿到定义良好的值 —— 复用 2d 签名 VS 配 shape 声明会读未定义输入寄存器
// (ps-only 下形状异常根因之一; 根因之二是空采样器采样为黑, 见 SetTexture_wrap)。
static IDirect3DVertexShader9* gm80_fake_ffp_vs_shape = nullptr;
static const char gm80_fake_ffp_shape_hlsl[] =
    "float4x4 uWVP : register(c0);"       "\n"
    "struct VS_IN {"                      "\n"
    "    float4 pos: POSITION;"           "\n"
    "    float4 color: COLOR0;"           "\n"
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
    "    o.uv = float2(0, 0);"            "\n"
    "    return o;"                       "\n"
    "}"                                   "\n";

static HRESULT gm80_ensure_fake_ffp_vs_shape(IDirect3DDevice9* dev)
{
    if (gm80_fake_ffp_vs_shape) return S_OK;
    return gm80_compile_fake_ffp_vs(dev, gm80_fake_ffp_shape_hlsl,
        sizeof(gm80_fake_ffp_shape_hlsl) - 1, &gm80_fake_ffp_vs_shape);
}

// 刷新仿固定管线 VS 的 WVP 常量。钩子在 DrawPrimitiveUP 前触发,
// 此刻引擎本绘制所需的 SetTransform 已全部完成, 当前值必为所需值。
// [2026-09-14 修复③] 三矩阵经影子表读取(零设备调用); 影子未就绪退回真 GetTransform。
static HRESULT gm80_update_fake_ffp_wvp(IDirect3DDevice9* dev)
{
    D3DXMATRIX world, view, proj, wvp;
    if (!shadow::copy_xf(D3DTS_WORLD, (D3DMATRIX*)&world))
        dev->GetTransform(D3DTS_WORLD, &world);
    if (!shadow::copy_xf(D3DTS_VIEW, (D3DMATRIX*)&view))
        dev->GetTransform(D3DTS_VIEW, &view);
    if (!shadow::copy_xf(D3DTS_PROJECTION, (D3DMATRIX*)&proj))
        dev->GetTransform(D3DTS_PROJECTION, &proj);

    D3DXMatrixMultiply(&wvp, &world, &view);
    D3DXMatrixMultiply(&wvp, &wvp, &proj);

    return dev->SetVertexShaderConstantF(0, &wvp._11, 4);
}

// 绑仿固定管线 VS: 声明按引擎 FVF 选 + WVP 常量 + VS。uWVP 是唯一 uniform → 编译器分配 c0-c3。
// [2026-08-26] VS 按 FVF 签名分流: shape 布局(无 TEXCOORD0)配 shape 签名 VS;
// 2d/3d 声明含 TEXCOORD0(@16/@28)共用 2d 签名 VS(3d 的 NORMAL/COLOR1 不读即合法)。
static HRESULT gm80_bind_fake_ffp(IDirect3DDevice9* dev, DWORD fvf)
{
    IDirect3DVertexDeclaration9** pdecl = nullptr;
    const D3DVERTEXELEMENT9* elems = nullptr;
    bool shape_sig = false;

    if (fvf == (D3DFVF_XYZ | D3DFVF_DIFFUSE))
    {
        pdecl = &gm80_decl_shape;
        elems = gm80_elems_shape;
        shape_sig = true;
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

    HRESULT hr = shape_sig ? gm80_ensure_fake_ffp_vs_shape(dev)
                           : gm80_ensure_fake_ffp_vs(dev);
    if (FAILED(hr)) return hr;

    hr = gm80_ensure_decl(dev, pdecl, elems);
    if (FAILED(hr)) return hr;

    hr = gm80_update_fake_ffp_wvp(dev);
    if (FAILED(hr)) return hr;

    hr = dev->SetVertexShader(shape_sig ? gm80_fake_ffp_vs_shape : gm80_fake_ffp_vs);
    if (FAILED(hr)) return hr;

    return dev->SetVertexDeclaration(*pdecl);
}

// FFP VS 判定: 引擎绘制时设备上可能绑着 FFP VS: 本插件的仿固定管线 VS, 或插件经 
// gmdx9_register_ffp_vs 注册的 FFP VS。SetVertexShader 钩子识别后, 每次引擎
// 绘制前刷新 WVP 到当前投影。两套 FFP VS 共用 c0-c3 + mul(uWVP,pos) 约定, 
// 刷新函数通用。用户自定义 VS 不在此列(其常量自管)。
static bool is_passthrough_vs(IDirect3DVertexShader9* vs)
{
    if (vs == gm80_fake_ffp_vs || vs == gm80_fake_ffp_vs_shape)
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
    // [2026-09-14 二批] 引擎每绘制的 SetVertexShader(FVF) 调用点包装 —— 兼任 flush:
    // 本包装不落 vtable 92(内部转 SetVertexDeclaration/SetFVF/整绑), 需在此先刷挂起批。
    gmdx9_fire_flush();
    // [2026-09-14 修复③] 当前 VS/PS 经影子表借读(零设备调用, 无 AddRef, 不得
    // Release); 影子未就绪(理论不可达: 本包装只在渲染期执行)退回真 Get + 计数归还。
    IDirect3DVertexShader9* vs = nullptr;
    bool release_vs = false;
    if (!shadow::borrow_vs((void**)&vs))
    {
        if (SUCCEEDED(dev->GetVertexShader(&vs)) && vs != nullptr)
            release_vs = true;
        else
            vs = nullptr;
    }
    if (vs != nullptr)
    {
        // 自定义 VS 已绑定: 引擎的 FVF 重置 → 声明切换, VS 保持。
        // (真 Get 路径注意: GetVertexShader 会给返回对象加引用(RecoveryTest R8
        // 实证), 所有出口必须 Release, 否则引擎每次绘制的 FVF 重置都泄漏一个引用。)
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
        {
            if (release_vs) vs->Release();
            return D3DERR_INVALIDCALL; // 未知 FVF + 自定义 VS: 引擎忽略返回值, 保持上次声明
        }

        if (FAILED(hr))
        {
            if (release_vs) vs->Release();
            return hr;
        }
        // [2026-08-08] 若当前 VS 是本钩子绑的仿固定管线 VS(ps-only 场景), 每绘制刷一次 WVP
        // (投影可能已变, 如换视图/d3d_set_projection)。用户自定义 VS 不在此列(其常量自管)。
        // [2026-08-09] 注册模式: 同样识别插件注册的透传 VS(如 GMGraphic 的 s_passthrough_vs),
        // 引擎绘制时设备上可能是它绑的(ps-only shader), WVP 需在引擎 SetVertexShader 时刻刷新
        // 到当前投影(surface_set_target 重设后)。
        if (is_passthrough_vs(vs))
        {
            // [2026-08-26] 双向签名重绑: shape 布局要 shape 签名 VS(uv≡(0,0)),
            // 2d/3d 要 2d 签名 VS。只换声明不换 VS 的后果: shape 绘制读未定义 uv
            // (黑屏/花屏根因之一); 反向则精灵全采 uv=(0,0) 变纯色。两个透传 VS 均
            // FFP 等价(uWVP@c0 + mul(uWVP,pos)), 互换安全; 用户自定义 VS 不在识别列。
            bool want_shape = (fvf == (D3DFVF_XYZ | D3DFVF_DIFFUSE));
            bool have_shape = (vs == gm80_fake_ffp_vs_shape);
            if (want_shape != have_shape)
            {
                if (release_vs) vs->Release();
                return gm80_bind_fake_ffp(dev, fvf);   // 整绑匹配签名的透传 VS + 声明 + WVP
            }
            hr = gm80_update_fake_ffp_wvp(dev);
            if (FAILED(hr))
            {
                if (release_vs) vs->Release();
                return hr;
            }
        }
        if (release_vs) vs->Release();
        return dev->SetVertexDeclaration(decl);
    }
    // 仿固定管线 VS 兜底: 无自定义 VS 但自定义 PS 激活(ps-only)时绑定喂 v0/v1;
    // 实测 ps_3_0 仍全透明 → GMGraphic 已回退 ps_2_0, 本分支留作 vs_3_0 透传 VS 实验。
    // [2026-09-14 修复③] PS 判定同走影子(真 Get 路径注意 GetPixelShader 加引用)。
    void* ps = nullptr;
    if (!shadow::borrow_ps(&ps))
    {
        IDirect3DPixelShader9* got = nullptr;
        if (SUCCEEDED(dev->GetPixelShader(&got)) && got != nullptr)
        {
            ps = got;
            got->Release();
        }
    }
    if (ps != nullptr)
        return gm80_bind_fake_ffp(dev, fvf);
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


// CheckDeviceMultiSampleType 接管
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

// ---- [2026-08-26] SetTexture 白像素兜底(设备 vtable 槽 65 = 0x104) ----
// ps-only 场景下引擎/GMGraphic 会把 stage0 置 NULL(无纹理图元): 固定管线对空采样器
// 透传顶点色, 可编程 PS 的 tex2D 返回黑 → 形状全黑(ps-only 黑块根因之二)。
// 钩子在 PS 激活时把 stage0 的 NULL 换成 1x1 白像素: 任意 UV 都采到白 →
// tex2D(s0)*color 类 PS 在无纹理图元上退化为纯顶点色(FFP 观感)。
// 真实纹理绑定与非 PS 绘制原样透传, 纹理路径零改动。

static HRESULT(WINAPI* real_set_texture)(
    IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*) = nullptr;
static IDirect3DDevice9* g_white_pixel_dev = nullptr;

// 懒创建 1x1 白像素(MANAGED 池跨设备 Reset 存活); 设备重建后按设备失配自动重建。
// 用 D3DXCreateTexture+LockRect 而非内嵌 TGA: 保证 RGBA 全 1, 无文件格式歧义
// (PS 会读到纹理 alpha, 与 FFP 只用顶点 alpha 不同, 必须确保不透明)。
static void ensure_white_pixel(IDirect3DDevice9* dev)
{
    if (white_pixel && g_white_pixel_dev == dev) return;
    if (white_pixel)
    {
        white_pixel->Release();
        white_pixel = nullptr;
    }
    IDirect3DTexture9* t = nullptr;
    if (FAILED(D3DXCreateTexture(dev, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t)))
        return;   // 设备丢失期创建失败 → 本调用退回透传, 下次再试
    D3DLOCKED_RECT lr;
    if (FAILED(t->LockRect(0, &lr, nullptr, 0)))
    {
        t->Release();
        return;
    }
    *(DWORD*)lr.pBits = 0xFFFFFFFF;
    t->UnlockRect(0);
    white_pixel = t;
    g_white_pixel_dev = dev;
}

HRESULT WINAPI SetTexture_wrap(
    IDirect3DDevice9* dev, DWORD Stage, IDirect3DBaseTexture9* pTexture)
{
    // [2026-09-14 二批] 兼任 flush 钩(槽 65): 纹理绑定是批继承态, 引擎逐绘制的
    // SetTexture 原本让位于 DrawUP 钩冲刷, 现提前到此处 —— 净冲刷次数不变。
    gmdx9_fire_flush();
    IDirect3DBaseTexture9* bound = pTexture;
    if (Stage == 0 && pTexture == nullptr)
    {
        // [2026-09-14 修复③] PS 判定走影子借读(真 Get 路径须 Release, 见 R8 实证)。
        void* ps = nullptr;
        bool has_ps = shadow::borrow_ps(&ps);
        if (!has_ps)
        {
            IDirect3DPixelShader9* got = nullptr;
            if (SUCCEEDED(dev->GetPixelShader(&got)) && got != nullptr)
            {
                ps = got;
                got->Release();
            }
        }
        if (ps != nullptr)
        {
            ensure_white_pixel(dev);
            if (white_pixel)
            {
                bound = white_pixel;   // 影子必须记实际落设备的对象(白像素替换)
                HRESULT hr = real_set_texture(dev, 0, white_pixel);
                shadow::update_tex(dev, Stage, bound, hr);
                return hr;
            }
        }
    }
    HRESULT hr = real_set_texture(dev, Stage, bound);
    shadow::update_tex(dev, Stage, bound, hr);
    return hr;
}

// DLL 卸载时恢复 vt[65](dllmain DLL_PROCESS_DETACH 调用)。
// [2026-09-14] 并入 gmdx9_restore_device_hooks(全钩子组恢复), 本函数删除。

// ---- [2026-09-14] 合批 flush 钩子(自动 force_draw_to_screen) ----
// GMGraphic 等插件把绘制攒在自己的顶点缓冲延迟提交, 与引擎原生绘制的即时提交混用
// 时顺序会断(rTitle 入场幕帘被末尾 flush 盖掉即实例)。插件经 gmdx9_register_flush_
// callback 注册 flush 入口后, 下列设备调用发生前自动先刷挂起批。
//
// 不变式(字面闭合): 任何提交像素、写入纹理内容、读取像素或改变绘制状态的设备调用
// 都先冲刷。批内容按批打开时刻的状态渲染, 所以状态写入不切段 = 批内后段内容被
// 冻结成旧值(地图图标丢 d3d_transform、psDissolution uniform 被 SDF 常量污染即实例)。
//
//   提交/内容族  DrawPrimitive / DrawIndexedPrimitive(含各自 UP 变体) / Clear /
//                ProcessVertices / DrawRectPatch / DrawTriPatch / ColorFill /
//                StretchRect / UpdateSurface / UpdateTexture /
//                GetRenderTargetData / GetFrontBufferData(读回=截图)
//   目标族       SetRenderTarget / SetDepthStencilSurface
//                (批画给旧目标; EndScene 帧尾兜底, 批不过夜)
//   状态族       SetTransform / MultiplyTransform / SetViewport / SetScissorRect /
//                SetRenderState / SetTextureStageState / SetSamplerState /
//                SetVertexDeclaration / SetFVF / SetVertexShader(92) /
//                SetTexture(65) / SetStreamSource(±Freq) / SetIndices /
//                SetVertexShaderConstantF/I/B / SetPixelShaderConstantF/I/B /
//                SetMaterial / SetLight / LightEnable / SetClipPlane /
//                SetSoftwareVertexProcessing
//
// 引擎路径覆盖: 8.0 的 D3D8→D3D9 槽位重映射后, 引擎状态调用直通设备 vtable,
// 状态族钩子天然捕获; 每绘制的 SetVertexShader(FVF) 走调用点包装(下方
// SetVertexShader), 其内的 SetVertexDeclaration/SetFVF/SetVertexShader 落 vtable
// 被钩 —— 三连(SetTexture→SetVertexShader→DrawUP)的冲刷从 DrawUP 提前到
// SetTexture, 净冲刷次数不变。空批两比较早退; flush 侧 dssnap 状态恢复的
// 数十次设备写由 g_flush_active 挡住, 不递归。
//
// 不钩清单(契约, 非"游戏够用"而是按性质排除):
//   Get*/Create*/ValidateDevice/CreateQuery   只读与资源创建, 无状态无顺序
//   Present              GM8 每帧必经 EndScene(42), 传递覆盖
//   Reset                设备丢失重建, 批随设备灭, flush 无意义(recovery 路径另处理)
//   SetGammaRamp         扫描输出期整帧套用, 与绘制顺序无交互
//   SetCursorProperties/Position/ShowCursor   系统光标覆盖层, 非管线状态
//   SetPaletteEntries/SetCurrentTexturePalette   P8 调色板纹理, 本层不创建
//   SetNPatchMode        只影响补面细分, 而补面绘制(DrawRect/TriPatch)自身已钩
//   DeletePatch          资源销毁, 非状态非绘制
//   StateBlock 三方法    录制/Apply 走 state-block 对象 vtable, 设备钩天然盲区;
//                        本层不使用 state block, 使用者自负
// 重入纪律: gmdx9_fire_flush 执行期间 g_flush_active 为真, 本组钩子直接透传 —— 回调
// 自己的 DrawPrimitiveUP、状态守卫恢复触发的 SetRenderTarget 都不会递归。

static HRESULT(WINAPI* real_draw_primitive_up)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT) = nullptr;
static HRESULT(WINAPI* real_draw_primitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT) = nullptr;
static HRESULT(WINAPI* real_draw_indexed_primitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) = nullptr;
static HRESULT(WINAPI* real_draw_indexed_primitive_up)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT) = nullptr;
static HRESULT(WINAPI* real_clear)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD) = nullptr;
static HRESULT(WINAPI* real_set_render_target_vt)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*) = nullptr;
static HRESULT(WINAPI* real_set_depth_stencil)(IDirect3DDevice9*, IDirect3DSurface9*) = nullptr;
static HRESULT(WINAPI* real_end_scene)(IDirect3DDevice9*) = nullptr;

HRESULT WINAPI DrawPrimitiveUP_hook(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type,
    UINT count, const void* data, UINT stride)
{
    gmdx9_fire_flush();
    return real_draw_primitive_up(dev, type, count, data, stride);
}

HRESULT WINAPI DrawPrimitive_hook(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type,
    UINT count, UINT start)
{
    gmdx9_fire_flush();
    return real_draw_primitive(dev, type, count, start);
}

HRESULT WINAPI DrawIndexedPrimitive_hook(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type,
    INT base_vertex, UINT min_index, UINT num_vertices, UINT start_index, UINT prim_count)
{
    gmdx9_fire_flush();
    return real_draw_indexed_primitive(dev, type, base_vertex, min_index, num_vertices,
        start_index, prim_count);
}

HRESULT WINAPI DrawIndexedPrimitiveUP_hook(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type,
    UINT min_index, UINT num_indices, UINT prim_count, const void* index_data,
    D3DFORMAT index_format, const void* data, UINT stride)
{
    gmdx9_fire_flush();
    return real_draw_indexed_primitive_up(dev, type, min_index, num_indices, prim_count,
        index_data, index_format, data, stride);
}

HRESULT WINAPI Clear_hook(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects,
    DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    gmdx9_fire_flush();
    return real_clear(dev, count, rects, flags, color, z, stencil);
}

// 名带 _vt: 与上方引擎调用点包装 SetRenderTarget(0x7C 站点重定向)区分。
// 本钩在设备 vtable 槽 37, 连 surface_set_target 之外的直接调用也一并覆盖。
HRESULT WINAPI SetRenderTarget_vt(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* surface)
{
    gmdx9_fire_flush();
    return real_set_render_target_vt(dev, index, surface);
}

HRESULT WINAPI SetDepthStencilSurface_hook(IDirect3DDevice9* dev, IDirect3DSurface9* surface)
{
    gmdx9_fire_flush();
    return real_set_depth_stencil(dev, surface);
}

HRESULT WINAPI EndScene_hook(IDirect3DDevice9* dev)
{
    gmdx9_fire_flush();
    return real_end_scene(dev);
}

// ---- [2026-09-14 二批] 状态/内容族 flush 钩子 ----
// 全部同构: gmdx9_fire_flush() 后透传。槽位号由 d3d9.h 全表 119 方法枚举核定
// (与上方全部既有锚点吻合)。_vt 后缀 = 与既有引擎调用点翻译函数(D3D8 旧签名)
// 撞名, vtable 版加缀区分。

// 提交/内容族: 纹理内容更新与像素读写也参与顺序 —— 先画的批应见旧内容,
// 截图(GetFrontBufferData/GetRenderTargetData)必须包含已画批。
static HRESULT(WINAPI* real_update_surface)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*) = nullptr;
static HRESULT(WINAPI* real_update_texture)(IDirect3DDevice9*, IDirect3DBaseTexture9*, IDirect3DBaseTexture9*) = nullptr;
static HRESULT(WINAPI* real_get_render_target_data)(IDirect3DDevice9*, IDirect3DSurface9*, IDirect3DSurface9*) = nullptr;
static HRESULT(WINAPI* real_get_front_buffer_data)(IDirect3DDevice9*, UINT, IDirect3DSurface9*) = nullptr;
static HRESULT(WINAPI* real_stretch_rect)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE) = nullptr;
static HRESULT(WINAPI* real_color_fill)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, D3DCOLOR) = nullptr;
static HRESULT(WINAPI* real_process_vertices)(IDirect3DDevice9*, UINT, UINT, UINT, IDirect3DVertexBuffer9*, IDirect3DVertexDeclaration9*, DWORD) = nullptr;
static HRESULT(WINAPI* real_draw_rect_patch)(IDirect3DDevice9*, UINT, const float*, const D3DRECTPATCH_INFO*) = nullptr;
static HRESULT(WINAPI* real_draw_tri_patch)(IDirect3DDevice9*, UINT, const float*, const D3DTRIPATCH_INFO*) = nullptr;

HRESULT WINAPI UpdateSurface_hook(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* src_rect, IDirect3DSurface9* dst, const RECT* dst_rect)
{ gmdx9_fire_flush(); return real_update_surface(dev, src, src_rect, dst, dst_rect); }
HRESULT WINAPI UpdateTexture_hook(IDirect3DDevice9* dev, IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst)
{ gmdx9_fire_flush(); return real_update_texture(dev, src, dst); }
HRESULT WINAPI GetRenderTargetData_hook(IDirect3DDevice9* dev, IDirect3DSurface9* rt, IDirect3DSurface9* dst)
{ gmdx9_fire_flush(); return real_get_render_target_data(dev, rt, dst); }
HRESULT WINAPI GetFrontBufferData_hook(IDirect3DDevice9* dev, UINT swap_chain, IDirect3DSurface9* dst)
{ gmdx9_fire_flush(); return real_get_front_buffer_data(dev, swap_chain, dst); }
HRESULT WINAPI StretchRect_hook(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* src_rect, IDirect3DSurface9* dst, const RECT* dst_rect, D3DTEXTUREFILTERTYPE filter)
{ gmdx9_fire_flush(); return real_stretch_rect(dev, src, src_rect, dst, dst_rect, filter); }
HRESULT WINAPI ColorFill_hook(IDirect3DDevice9* dev, IDirect3DSurface9* surface, const RECT* rect, D3DCOLOR color)
{ gmdx9_fire_flush(); return real_color_fill(dev, surface, rect, color); }
HRESULT WINAPI ProcessVertices_hook(IDirect3DDevice9* dev, UINT src_start, UINT dest_index, UINT vertex_count, IDirect3DVertexBuffer9* dest_buffer, IDirect3DVertexDeclaration9* decl, DWORD flags)
{ gmdx9_fire_flush(); return real_process_vertices(dev, src_start, dest_index, vertex_count, dest_buffer, decl, flags); }
HRESULT WINAPI DrawRectPatch_hook(IDirect3DDevice9* dev, UINT handle, const float* num_segs, const D3DRECTPATCH_INFO* info)
{ gmdx9_fire_flush(); return real_draw_rect_patch(dev, handle, num_segs, info); }
HRESULT WINAPI DrawTriPatch_hook(IDirect3DDevice9* dev, UINT handle, const float* num_segs, const D3DTRIPATCH_INFO* info)
{ gmdx9_fire_flush(); return real_draw_tri_patch(dev, handle, num_segs, info); }

// 变换/视口/剪裁族: d3d_transform_*(引擎写 WORLD)、视图正交/视口切换。
static HRESULT(WINAPI* real_set_transform)(IDirect3DDevice9*, DWORD, const D3DMATRIX*) = nullptr;
static HRESULT(WINAPI* real_multiply_transform)(IDirect3DDevice9*, DWORD, const D3DMATRIX*) = nullptr;
static HRESULT(WINAPI* real_set_viewport_vt)(IDirect3DDevice9*, const D3DVIEWPORT9*) = nullptr;
static HRESULT(WINAPI* real_set_scissor_rect)(IDirect3DDevice9*, const RECT*) = nullptr;

HRESULT WINAPI SetTransform_hook(IDirect3DDevice9* dev, DWORD state, const D3DMATRIX* matrix)
{
    gmdx9_fire_flush();
    HRESULT hr = real_set_transform(dev, state, matrix);
    shadow::update_xf(dev, state, matrix, hr);   // [修复③] 变换族记账
    return hr;
}
HRESULT WINAPI MultiplyTransform_hook(IDirect3DDevice9* dev, DWORD state, const D3DMATRIX* matrix)
{
    gmdx9_fire_flush();
    HRESULT hr = real_multiply_transform(dev, state, matrix);
    shadow::update_xf_mul(dev, state, matrix, hr);   // [修复③] 影子原地右乘(语义见 .cpp)
    return hr;
}
// 名带 _vt: 引擎调用点包装 SetViewport_inj(D3D8 旧签名)已存在, 最终落本钩。
HRESULT WINAPI SetViewport_vt(IDirect3DDevice9* dev, const D3DVIEWPORT9* viewport)
{
    gmdx9_fire_flush();
    HRESULT hr = real_set_viewport_vt(dev, viewport);
    shadow::update_vp(dev, viewport, hr);   // [修复③]
    return hr;
}
HRESULT WINAPI SetScissorRect_hook(IDirect3DDevice9* dev, const RECT* rect)
{ gmdx9_fire_flush(); return real_set_scissor_rect(dev, rect); }

// 状态族: 渲染状态/采样/纹理阶段 + 着色器与常量 + 顶点格式与流 + 灯光材质。
// [2026-09-14 修复③] 影子表覆盖族的记账; 常量/流/灯光族不记账(见 state_shadow.h)。
static HRESULT(WINAPI* real_set_render_state)(IDirect3DDevice9*, DWORD, DWORD) = nullptr;
static HRESULT(WINAPI* real_set_texture_stage_state)(IDirect3DDevice9*, DWORD, DWORD, DWORD) = nullptr;
static HRESULT(WINAPI* real_set_sampler_state)(IDirect3DDevice9*, DWORD, DWORD, DWORD) = nullptr;
static HRESULT(WINAPI* real_set_vertex_declaration)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*) = nullptr;
static HRESULT(WINAPI* real_set_fvf)(IDirect3DDevice9*, DWORD) = nullptr;
static HRESULT(WINAPI* real_set_vertex_shader_vt)(IDirect3DDevice9*, IDirect3DVertexShader9*) = nullptr;
static HRESULT(WINAPI* real_set_pixel_shader)(IDirect3DDevice9*, IDirect3DPixelShader9*) = nullptr;
static HRESULT(WINAPI* real_set_vs_constant_f)(IDirect3DDevice9*, UINT, const float*, UINT) = nullptr;
static HRESULT(WINAPI* real_set_vs_constant_i)(IDirect3DDevice9*, UINT, const int*, UINT) = nullptr;
static HRESULT(WINAPI* real_set_vs_constant_b)(IDirect3DDevice9*, UINT, const BOOL*, UINT) = nullptr;
static HRESULT(WINAPI* real_set_ps_constant_f)(IDirect3DDevice9*, UINT, const float*, UINT) = nullptr;
static HRESULT(WINAPI* real_set_ps_constant_i)(IDirect3DDevice9*, UINT, const int*, UINT) = nullptr;
static HRESULT(WINAPI* real_set_ps_constant_b)(IDirect3DDevice9*, UINT, const BOOL*, UINT) = nullptr;
static HRESULT(WINAPI* real_set_stream_source_vt)(IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT) = nullptr;
static HRESULT(WINAPI* real_set_stream_source_freq)(IDirect3DDevice9*, UINT, UINT) = nullptr;
static HRESULT(WINAPI* real_set_indices)(IDirect3DDevice9*, IDirect3DIndexBuffer9*) = nullptr;
static HRESULT(WINAPI* real_set_material)(IDirect3DDevice9*, const D3DMATERIAL9*) = nullptr;
static HRESULT(WINAPI* real_set_light)(IDirect3DDevice9*, DWORD, const D3DLIGHT9*) = nullptr;
static HRESULT(WINAPI* real_light_enable)(IDirect3DDevice9*, DWORD, BOOL) = nullptr;
static HRESULT(WINAPI* real_set_clip_plane)(IDirect3DDevice9*, DWORD, const float*) = nullptr;
static HRESULT(WINAPI* real_set_sw_vertex_processing)(IDirect3DDevice9*, BOOL) = nullptr;

HRESULT WINAPI SetRenderState_hook(IDirect3DDevice9* dev, DWORD state, DWORD value)
{
    gmdx9_fire_flush();
    HRESULT hr = real_set_render_state(dev, state, value);
    shadow::update_rs(dev, state, value, hr);   // [修复③]
    return hr;
}
HRESULT WINAPI SetTextureStageState_hook(IDirect3DDevice9* dev, DWORD stage, DWORD type, DWORD value)
{
    gmdx9_fire_flush();
    HRESULT hr = real_set_texture_stage_state(dev, stage, type, value);
    shadow::update_tss(dev, stage, type, value, hr);   // [修复③]
    return hr;
}
HRESULT WINAPI SetSamplerState_hook(IDirect3DDevice9* dev, DWORD sampler, DWORD type, DWORD value)
{
    gmdx9_fire_flush();
    HRESULT hr = real_set_sampler_state(dev, sampler, type, value);
    shadow::update_samp(dev, sampler, type, value, hr);   // [修复③]
    return hr;
}
HRESULT WINAPI SetVertexDeclaration_hook(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl)
{
    gmdx9_fire_flush();
    HRESULT hr = real_set_vertex_declaration(dev, decl);
    shadow::update_decl(dev, decl, hr);   // [修复③]
    return hr;
}
HRESULT WINAPI SetFVF_hook(IDirect3DDevice9* dev, DWORD fvf)
{
    gmdx9_fire_flush();
    HRESULT hr = real_set_fvf(dev, fvf);
    shadow::update_fvf(dev, fvf, hr);   // [修复③]
    return hr;
}
// 名带 _vt: 引擎调用点包装 SetVertexShader(FVF 旧签名)已存在, 本钩覆盖 vtable 直呼。
HRESULT WINAPI SetVertexShader_vt(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs)
{
    gmdx9_fire_flush();
    HRESULT hr = real_set_vertex_shader_vt(dev, vs);
    shadow::update_vs(dev, vs, hr);   // [修复③]
    return hr;
}
HRESULT WINAPI SetPixelShader_hook(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps)
{
    gmdx9_fire_flush();
    HRESULT hr = real_set_pixel_shader(dev, ps);
    shadow::update_ps(dev, ps, hr);   // [修复③]
    return hr;
}
HRESULT WINAPI SetVertexShaderConstantF_hook(IDirect3DDevice9* dev, UINT start, const float* data, UINT count)
{ gmdx9_fire_flush(); return real_set_vs_constant_f(dev, start, data, count); }
HRESULT WINAPI SetVertexShaderConstantI_hook(IDirect3DDevice9* dev, UINT start, const int* data, UINT count)
{ gmdx9_fire_flush(); return real_set_vs_constant_i(dev, start, data, count); }
HRESULT WINAPI SetVertexShaderConstantB_hook(IDirect3DDevice9* dev, UINT start, const BOOL* data, UINT count)
{ gmdx9_fire_flush(); return real_set_vs_constant_b(dev, start, data, count); }
HRESULT WINAPI SetPixelShaderConstantF_hook(IDirect3DDevice9* dev, UINT start, const float* data, UINT count)
{ gmdx9_fire_flush(); return real_set_ps_constant_f(dev, start, data, count); }
HRESULT WINAPI SetPixelShaderConstantI_hook(IDirect3DDevice9* dev, UINT start, const int* data, UINT count)
{ gmdx9_fire_flush(); return real_set_ps_constant_i(dev, start, data, count); }
HRESULT WINAPI SetPixelShaderConstantB_hook(IDirect3DDevice9* dev, UINT start, const BOOL* data, UINT count)
{ gmdx9_fire_flush(); return real_set_ps_constant_b(dev, start, data, count); }
// 名带 _vt: D3D8→9 翻译函数 SetStreamSource(4 参旧签名)已存在, 本钩为 5 参 vtable 版。
HRESULT WINAPI SetStreamSource_vt(IDirect3DDevice9* dev, UINT number, IDirect3DVertexBuffer9* buffer, UINT offset, UINT stride)
{ gmdx9_fire_flush(); return real_set_stream_source_vt(dev, number, buffer, offset, stride); }
HRESULT WINAPI SetStreamSourceFreq_hook(IDirect3DDevice9* dev, UINT number, UINT divider)
{ gmdx9_fire_flush(); return real_set_stream_source_freq(dev, number, divider); }
HRESULT WINAPI SetIndices_hook(IDirect3DDevice9* dev, IDirect3DIndexBuffer9* indices)
{ gmdx9_fire_flush(); return real_set_indices(dev, indices); }
HRESULT WINAPI SetMaterial_hook(IDirect3DDevice9* dev, const D3DMATERIAL9* material)
{ gmdx9_fire_flush(); return real_set_material(dev, material); }
HRESULT WINAPI SetLight_hook(IDirect3DDevice9* dev, DWORD index, const D3DLIGHT9* light)
{ gmdx9_fire_flush(); return real_set_light(dev, index, light); }
HRESULT WINAPI LightEnable_hook(IDirect3DDevice9* dev, DWORD index, BOOL enable)
{ gmdx9_fire_flush(); return real_light_enable(dev, index, enable); }
HRESULT WINAPI SetClipPlane_hook(IDirect3DDevice9* dev, DWORD index, const float* plane)
{ gmdx9_fire_flush(); return real_set_clip_plane(dev, index, plane); }
HRESULT WINAPI SetSoftwareVertexProcessing_hook(IDirect3DDevice9* dev, BOOL software)
{ gmdx9_fire_flush(); return real_set_sw_vertex_processing(dev, software); }

// 钩子表: D3D9 设备 vtable 槽位号。已验证锚点(与既有补丁注释一致): Present=17(0x44)、
// BeginScene=41(0xA4)、EndScene=42(0xA8)、Clear=43(0xAC)、SetTransform=44(0xB0)、
// SetTexture=65(0x104)、DrawPrimitiveUP=83(0x14C); SetRenderTarget=37(0x94)/
// SetDepthStencilSurface=39(0x9C)/DrawPrimitive=81(0x144) 按同段连续序反推;
// DrawIndexedPrimitive=82(0x148)/DrawIndexedPrimitiveUP=84(0x150) 由 d3d9.h 全表
// 119 方法枚举核定(与上述全部锚点吻合)。SetVertexShader=92(0x170), 补丁表里的
// 0x130 是它的 D3D8 源槽, 勿当 D3D9 槽位引用。
// [2026-09-14 二批] 二批全部新增槽位(30-35/44/46/47/49/51/53/55/57/67/69/75/77/85/
// 87/89/92/94/96/98/100/102/104/107/109/111/113/115/116)由同一次全表枚举直接读出,
// 与上述锚点同源。
struct VtHook
{
    int slot;
    void* wrap;
    void** real;
};

static VtHook g_vt_hooks[] = {
    { 65, (void*)&SetTexture_wrap,             (void**)&real_set_texture },
    { 83, (void*)&DrawPrimitiveUP_hook,        (void**)&real_draw_primitive_up },
    { 81, (void*)&DrawPrimitive_hook,          (void**)&real_draw_primitive },
    { 82, (void*)&DrawIndexedPrimitive_hook,   (void**)&real_draw_indexed_primitive },
    { 84, (void*)&DrawIndexedPrimitiveUP_hook, (void**)&real_draw_indexed_primitive_up },
    { 43, (void*)&Clear_hook,                  (void**)&real_clear },
    { 37, (void*)&SetRenderTarget_vt,          (void**)&real_set_render_target_vt },
    { 39, (void*)&SetDepthStencilSurface_hook, (void**)&real_set_depth_stencil },
    { 42, (void*)&EndScene_hook,               (void**)&real_end_scene },
    // 提交/内容族
    { 30, (void*)&UpdateSurface_hook,          (void**)&real_update_surface },
    { 31, (void*)&UpdateTexture_hook,          (void**)&real_update_texture },
    { 32, (void*)&GetRenderTargetData_hook,    (void**)&real_get_render_target_data },
    { 33, (void*)&GetFrontBufferData_hook,     (void**)&real_get_front_buffer_data },
    { 34, (void*)&StretchRect_hook,            (void**)&real_stretch_rect },
    { 35, (void*)&ColorFill_hook,              (void**)&real_color_fill },
    { 85, (void*)&ProcessVertices_hook,        (void**)&real_process_vertices },
    { 115, (void*)&DrawRectPatch_hook,         (void**)&real_draw_rect_patch },
    { 116, (void*)&DrawTriPatch_hook,          (void**)&real_draw_tri_patch },
    // 变换/视口/剪裁族
    { 44, (void*)&SetTransform_hook,           (void**)&real_set_transform },
    { 46, (void*)&MultiplyTransform_hook,      (void**)&real_multiply_transform },
    { 47, (void*)&SetViewport_vt,              (void**)&real_set_viewport_vt },
    { 75, (void*)&SetScissorRect_hook,         (void**)&real_set_scissor_rect },
    // 状态族
    { 57, (void*)&SetRenderState_hook,         (void**)&real_set_render_state },
    { 67, (void*)&SetTextureStageState_hook,   (void**)&real_set_texture_stage_state },
    { 69, (void*)&SetSamplerState_hook,        (void**)&real_set_sampler_state },
    { 87, (void*)&SetVertexDeclaration_hook,   (void**)&real_set_vertex_declaration },
    { 89, (void*)&SetFVF_hook,                 (void**)&real_set_fvf },
    { 92, (void*)&SetVertexShader_vt,          (void**)&real_set_vertex_shader_vt },
    { 107, (void*)&SetPixelShader_hook,        (void**)&real_set_pixel_shader },
    { 94, (void*)&SetVertexShaderConstantF_hook, (void**)&real_set_vs_constant_f },
    { 96, (void*)&SetVertexShaderConstantI_hook, (void**)&real_set_vs_constant_i },
    { 98, (void*)&SetVertexShaderConstantB_hook, (void**)&real_set_vs_constant_b },
    { 109, (void*)&SetPixelShaderConstantF_hook, (void**)&real_set_ps_constant_f },
    { 111, (void*)&SetPixelShaderConstantI_hook, (void**)&real_set_ps_constant_i },
    { 113, (void*)&SetPixelShaderConstantB_hook, (void**)&real_set_ps_constant_b },
    { 100, (void*)&SetStreamSource_vt,         (void**)&real_set_stream_source_vt },
    { 102, (void*)&SetStreamSourceFreq_hook,   (void**)&real_set_stream_source_freq },
    { 104, (void*)&SetIndices_hook,            (void**)&real_set_indices },
    { 49, (void*)&SetMaterial_hook,            (void**)&real_set_material },
    { 51, (void*)&SetLight_hook,               (void**)&real_set_light },
    { 53, (void*)&LightEnable_hook,            (void**)&real_light_enable },
    { 55, (void*)&SetClipPlane_hook,           (void**)&real_set_clip_plane },
    { 77, (void*)&SetSoftwareVertexProcessing_hook, (void**)&real_set_sw_vertex_processing },
};

// 设备 vtable 钩子组安装: CreateDevice 成功块与 recovery 重建路径共用。
// 真指针只需首次保存 —— 所有设备对象共享同一驱动实现地址(同 SetTexture 钩子先例)。
// [2026-09-14 修复③] 安装完成后影子表全量播种(Get* 一次), 此后由 Set* 钩维护。
bool gmdx9_install_render_hooks(IDirect3DDevice9* dev)
{
    __try
    {
        void** vt = *(void***)dev;
        for (int i = 0; i < (int)(sizeof(g_vt_hooks) / sizeof(g_vt_hooks[0])); i++)
        {
            if (*g_vt_hooks[i].real == nullptr)
                *g_vt_hooks[i].real = vt[g_vt_hooks[i].slot];
            DWORD oldp;
            if (VirtualProtect(&vt[g_vt_hooks[i].slot], sizeof(void*),
                    PAGE_EXECUTE_READWRITE, &oldp))
            {
                vt[g_vt_hooks[i].slot] = g_vt_hooks[i].wrap;
                VirtualProtect(&vt[g_vt_hooks[i].slot], sizeof(void*), oldp, &oldp);
            }
        }
        shadow::seed_from_device(dev);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

// DLL 卸载时恢复全部设备 vtable 钩子(原 SetTexture 恢复扩展, 2026-09-14)。
void gm80_restore_device_hooks(void)
{
    __try
    {
        IDirect3DDevice9* dev = Device;   // runner 全局 0x58d388
        if (!dev) return;
        void** vt = *(void***)dev;
        for (int i = 0; i < (int)(sizeof(g_vt_hooks) / sizeof(g_vt_hooks[0])); i++)
        {
            if (*g_vt_hooks[i].real == nullptr) continue;
            if (vt[g_vt_hooks[i].slot] != g_vt_hooks[i].wrap) continue; // 被别处改过 → 不碰
            DWORD oldp;
            if (VirtualProtect(&vt[g_vt_hooks[i].slot], sizeof(void*),
                    PAGE_EXECUTE_READWRITE, &oldp))
            {
                vt[g_vt_hooks[i].slot] = *g_vt_hooks[i].real;
                VirtualProtect(&vt[g_vt_hooks[i].slot], sizeof(void*), oldp, &oldp);
            }
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
    // [2026-09-14] gm82dx9 的 FPU 控制字舞蹈与数学 trampoline 已整体移除:
    // 8.0 的 CreateDevice 带 D3DCREATE_FPU_PRESERVE(0x22), 实测 precision=1,
    // D3D9 不改写控制字(§10), 原 trampoline 服务的 8.1 场景不存在于 8.0。

    present_params = pPresentationParameters;

    // 分辨率可调修复: 保持 runner 传入的 backbuffer 尺寸(显示器), 不缩到首个房间视图
    // (8.0 换分辨率不调 Reset, backbuffer 恒不变; 缩了会被夹住, 可见区永远卡在初始尺寸)。

    D3DPRESENT_PARAMETERS pp9;
    memset(&pp9, 0, sizeof(pp9));
    DWORD runner_ms = D3DMULTISAMPLE_NONE;
    if (present_params)
    {
        pp9.BackBufferWidth = present_params->BackBufferWidth;
        pp9.BackBufferHeight = present_params->BackBufferHeight;
        pp9.BackBufferFormat = present_params->BackBufferFormat;
        pp9.BackBufferCount = present_params->BackBufferCount
            ? present_params->BackBufferCount
            : 1;
        // [2026-09-14] MSAA 尊重 runner 传入值。注意布局: runner 传的是 D3D8 结构
        // (无 0x14 MultiSampleQuality, 之后全部错位) —— MultiSampleType 两代都在
        // 0x10 可直读。IDA 证实 8.0 两个 pp 构造点(sub_4A1DA0 / INNER_display_set_size)
        // 都先 52 字节清零且从不写 0x10, 此处恒得 NONE —— 保留透传仅为纵深防御。
        runner_ms = present_params->MultiSampleType;
        if (runner_ms > D3DMULTISAMPLE_16_SAMPLES)
            runner_ms = D3DMULTISAMPLE_NONE;
        pp9.MultiSampleType = (D3DMULTISAMPLE_TYPE)runner_ms;
    }
    // [2026-09-14 三批] 呈现节奏恒 IMMEDIATE(窗口化), 不透传 runner 的 interval 字段。
    // 原因: IDA 证实 runner 窗口化 pp 恒写 interval=0(DEFAULT) + SwapEffect=COPY_VSYNC(3)
    // (sub_4A1DA0@0x4a1e93 与 INNER_display_set_size@0x4a2287, 两处均无条件) ——
    // set_synchronization 根本不进窗口化 pp, GM8 的同步开关在 D3D8 语义里依附于
    // COPY_VSYNC 交换效果, Win10 DWM 下近似无效(Present 从不阻塞 CPU)。而 D3D9 的
    // COPY+DEFAULT 是真阻塞的垂直同步(每帧 Present 等 vblank/DWM 合成才返回),
    // 透传 0 等于替游戏强制开 sync —— 曾实测由此 set_synchronization(false) 被无视。
    // 故窗口化恒 IMMEDIATE(gm82dx9 同款选择)。垂直同步由 runner 自带机制落实:
    // INNER_screen_refresh(每帧 Present 前读同步标志 0x58D3A0, 非零则 DirectDraw
    // WaitForVerticalBlank 软等待, 0x4a2840→0x4a2842) —— 该路径不经过 d3d8.dll,
    // 本插件的 D3D8→D3D9 移植不触及; IMMEDIATE 恰使 Present 落在回扫等待之后。
    pp9.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    // COPY 交换效果不支持多重采样, 请求 MSAA 时换 DISCARD(D3D9 硬性要求)。
    pp9.SwapEffect = (pp9.MultiSampleType != D3DMULTISAMPLE_NONE)
        ? D3DSWAPEFFECT_DISCARD
        : D3DSWAPEFFECT_COPY;
    pp9.hDeviceWindow = hFocusWindow; // 不读 runner 的字段(错位垃圾); 焦点窗口已验证有效
    pp9.Windowed = TRUE;
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
    // (pp9.Windowed 恒 TRUE, 无全屏失败回退分支; 窗口化是 §12 定案)

    if (SUCCEEDED(res) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface)
    {
        // 设备丢失恢复核心(d3d9_recovery.cpp): 保存干净 pp9/创建参数 + 安装 Reset 钩子
        gmdx9_recovery_on_device_created(*ppReturnedDeviceInterface, &pp9,
            Adapter, DeviceType, hFocusWindow, bf_used);
        // [2026-08-26] SetTexture 白像素兜底钩子(vt[65]) + [2026-09-14] flush 六槽钩子:
        // 每个新设备对象都要重装; recovery 重建路径的 gmdx9_install_device_hooks 同调。
        gmdx9_install_render_hooks(*ppReturnedDeviceInterface);
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

// [2026-09-14] gm82dx9 的 FPU 控制字 trampoline(sqrt/ln/log/arcsin/.../power)与
// D3DXMatrix 投影矩阵注入函数已整体移除: 8.0 的 CreateDevice 带 FPU_PRESERVE,
// D3D9 不改写控制字(§10 实测 precision=1), 这些 trampoline 服务的 8.1 场景在
// 8.0 不存在, 且从未被本工程的补丁表引用。

// [2026-08-26] 旧 white_pixel_tga/create_white_pixel/SetNullTexture 已删除:
// 白像素改由 ensure_white_pixel 惰性创建(见 CreateDevice 上方), 置空兜底走
// 设备 vtable 槽 65 的 SetTexture_wrap, 不再依赖逐路由点区分 NULL/非 NULL。

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

    // SetTexture(0,NULL) 白像素兜底不走路由点补丁: 已在设备 vtable 槽 65(0x104) 装
    // SetTexture_wrap(CreateDevice 成功块安装), 覆盖引擎与外部 DLL(GMGraphic)的全部置空调用。

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
