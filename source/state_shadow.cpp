#include "state_shadow.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

// ============================================================================
// 影子存储。valid 位区分"从未见过"(播种时 Get 失败/引擎不用的枚举)与真实值,
// 读口对无效项返回 false, 对账模式对无效项跳过比对。
// ============================================================================
namespace
{
    DWORD  s_rs[256];        bool s_rs_valid[256];
    DWORD  s_tss[8][64];     bool s_tss_valid[8][64];
    DWORD  s_samp[16][16];   bool s_samp_valid[16][16];
    void*  s_tex[16];
    D3DMATRIX s_xf[258];     bool s_xf_valid[258];   // 0-255 world, 256 VIEW, 257 PROJECTION
    D3DVIEWPORT9 s_vp;       bool s_vp_valid;
    DWORD  s_fvf;            bool s_fvf_valid;
    void*  s_decl;           bool s_decl_valid;
    void*  s_vs;             bool s_vs_valid;
    void*  s_ps;             bool s_ps_valid;
    bool   s_live = false;

    void invalidate_all()
    {
        memset(s_rs_valid, 0, sizeof(s_rs_valid));
        memset(s_tss_valid, 0, sizeof(s_tss_valid));
        memset(s_samp_valid, 0, sizeof(s_samp_valid));
        memset(s_tex, 0, sizeof(s_tex));
        memset(s_xf_valid, 0, sizeof(s_xf_valid));
        s_vp_valid = s_fvf_valid = s_decl_valid = s_vs_valid = s_ps_valid = false;
        s_decl = s_vs = s_ps = nullptr;
    }

    // ---- 对账调试模式 ----
    enum AuditState { AuditUninit = 0, AuditOff, AuditOn };
    AuditState s_audit = AuditUninit;
    FILE* s_log = nullptr;
    unsigned s_log_lines = 0;

    bool audit_on()
    {
        if (s_audit == AuditUninit)
        {
            char buf[2] = { 0 };
            DWORD n = GetEnvironmentVariableA("GMDX9_SHADOW_AUDIT", buf, 2);
            s_audit = (n > 0 && buf[0] != '0') ? AuditOn : AuditOff;
        }
        return s_audit == AuditOn;
    }

    void log_mismatch(const char* fmt, ...)
    {
        if (s_log_lines > 2000) return;
        if (!s_log)
        {
            s_log = std::fopen("gmdx9_shadow_audit.log", "a");
            if (!s_log) return;
            std::fprintf(s_log, "==== GMDirectX9 设备状态影子对账 ====\n");
        }
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(s_log, fmt, ap);
        va_end(ap);
        std::fputc('\n', s_log);
        std::fflush(s_log);
        ++s_log_lines;
        OutputDebugStringA("gmdx9: shadow audit mismatch (gmdx9_shadow_audit.log)\n");
    }

    // 各状态族的对账: 记账值 vs 真 Get(AddRef 的对象即时 Release)。
    void audit_rs(IDirect3DDevice9* dev, DWORD state, DWORD value)
    {
        DWORD got = 0;
        if (SUCCEEDED(dev->GetRenderState((D3DRENDERSTATETYPE)state, &got)) && got != value)
            log_mismatch("SetRenderState(%lu): shadow=%lu device=%lu",
                (unsigned long)state, (unsigned long)value, (unsigned long)got);
    }
    void audit_tss(IDirect3DDevice9* dev, DWORD stage, DWORD type, DWORD value)
    {
        DWORD got = 0;
        if (SUCCEEDED(dev->GetTextureStageState(stage, (D3DTEXTURESTAGESTATETYPE)type, &got))
            && got != value)
            log_mismatch("SetTSS(stage %lu, type %lu): shadow=%lu device=%lu",
                (unsigned long)stage, (unsigned long)type,
                (unsigned long)value, (unsigned long)got);
    }
    void audit_samp(IDirect3DDevice9* dev, DWORD sampler, DWORD type, DWORD value)
    {
        DWORD got = 0;
        if (SUCCEEDED(dev->GetSamplerState(sampler, (D3DSAMPLERSTATETYPE)type, &got))
            && got != value)
            log_mismatch("SetSampler(%lu, type %lu): shadow=%lu device=%lu",
                (unsigned long)sampler, (unsigned long)type,
                (unsigned long)value, (unsigned long)got);
    }
    void audit_tex(IDirect3DDevice9* dev, DWORD stage, void* tex)
    {
        IDirect3DBaseTexture9* got = nullptr;
        if (SUCCEEDED(dev->GetTexture(stage, &got)))
        {
            if ((void*)got != tex)
                log_mismatch("SetTexture(stage %lu): shadow=%p device=%p",
                    (unsigned long)stage, tex, (void*)got);
            if (got) got->Release();
        }
    }
    void audit_xf(IDirect3DDevice9* dev, DWORD state, const D3DMATRIX* m)
    {
        D3DMATRIX got;
        if (SUCCEEDED(dev->GetTransform((D3DTRANSFORMSTATETYPE)state, &got))
            && memcmp(&got, m, sizeof(got)) != 0)
            log_mismatch("SetTransform(%lu): shadow != device", (unsigned long)state);
    }
    void audit_vp(IDirect3DDevice9* dev, const D3DVIEWPORT9* vp)
    {
        D3DVIEWPORT9 got;
        if (SUCCEEDED(dev->GetViewport(&got)) && memcmp(&got, vp, sizeof(got)) != 0)
            log_mismatch("SetViewport: shadow != device");
    }
    void audit_fvf(IDirect3DDevice9* dev, DWORD fvf)
    {
        DWORD got = 0;
        if (SUCCEEDED(dev->GetFVF(&got)) && got != fvf)
            log_mismatch("SetFVF: shadow=%lu device=%lu",
                (unsigned long)fvf, (unsigned long)got);
    }
    void audit_decl(IDirect3DDevice9* dev, void* decl)
    {
        IDirect3DVertexDeclaration9* got = nullptr;
        if (SUCCEEDED(dev->GetVertexDeclaration(&got)))
        {
            if ((void*)got != decl)
                log_mismatch("SetVertexDeclaration: shadow=%p device=%p", decl, (void*)got);
            if (got) got->Release();
        }
    }
    void audit_vs(IDirect3DDevice9* dev, void* vs)
    {
        IDirect3DVertexShader9* got = nullptr;
        if (SUCCEEDED(dev->GetVertexShader(&got)))
        {
            if ((void*)got != vs)
                log_mismatch("SetVertexShader: shadow=%p device=%p", vs, (void*)got);
            if (got) got->Release();
        }
    }
    void audit_ps(IDirect3DDevice9* dev, void* ps)
    {
        IDirect3DPixelShader9* got = nullptr;
        if (SUCCEEDED(dev->GetPixelShader(&got)))
        {
            if ((void*)got != ps)
                log_mismatch("SetPixelShader: shadow=%p device=%p", ps, (void*)got);
            if (got) got->Release();
        }
    }
}

namespace shadow
{

// 播种主体(真 Get 全量; 由 seed_from_device 的 SEH 包裹调用)。
static void seed_from_device_impl(IDirect3DDevice9* dev);

void seed_from_device(IDirect3DDevice9* dev)
{
    if (!dev) return;
    invalidate_all();
    // SEH 自护: 设备边际态(或测试桩)上真 Get 可能崩溃 —— 只降级为"影子未就绪",
    // 绝不改变调用方(ResetDevice)的返回路径(Reset 本身已成功)。
    __try
    {
        seed_from_device_impl(dev);
        s_live = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        invalidate_all();
        s_live = false;
    }
}

// 播种主体(真 Get 全量; 由 seed_from_device 的 SEH 包裹调用)。
static void seed_from_device_impl(IDirect3DDevice9* dev)
{
    DWORD v = 0;
    for (int i = 0; i < 256; ++i)
        if (SUCCEEDED(dev->GetRenderState((D3DRENDERSTATETYPE)i, &v)))
        {
            s_rs[i] = v;
            s_rs_valid[i] = true;
        }
    for (int st = 0; st < 8; ++st)
        for (int t = 0; t < 64; ++t)
            if (SUCCEEDED(dev->GetTextureStageState(
                    st, (D3DTEXTURESTAGESTATETYPE)t, &v)))
            {
                s_tss[st][t] = v;
                s_tss_valid[st][t] = true;
            }
    for (int sm = 0; sm < 16; ++sm)
        for (int t = 0; t < 16; ++t)
            if (SUCCEEDED(dev->GetSamplerState(sm, (D3DSAMPLERSTATETYPE)t, &v)))
            {
                s_samp[sm][t] = v;
                s_samp_valid[sm][t] = true;
            }
    for (int i = 0; i < 16; ++i)
        dev->GetTexture(i, (IDirect3DBaseTexture9**)&s_tex[i]);   // 失败保持 null
    for (int i = 0; i < 258; ++i)
        if (SUCCEEDED(dev->GetTransform((D3DTRANSFORMSTATETYPE)i, &s_xf[i])))
            s_xf_valid[i] = true;
    s_vp_valid = SUCCEEDED(dev->GetViewport(&s_vp));
    s_fvf_valid = SUCCEEDED(dev->GetFVF(&s_fvf));
    s_decl_valid = SUCCEEDED(dev->GetVertexDeclaration(
        (IDirect3DVertexDeclaration9**)&s_decl));
    s_vs_valid = SUCCEEDED(dev->GetVertexShader((IDirect3DVertexShader9**)&s_vs));
    s_ps_valid = SUCCEEDED(dev->GetPixelShader((IDirect3DPixelShader9**)&s_ps));
}

bool live() { return s_live; }

void update_rs(IDirect3DDevice9* dev, DWORD state, DWORD value, HRESULT hr)
{
    if (FAILED(hr) || state >= 256) return;
    s_rs[state] = value;
    s_rs_valid[state] = true;
    if (audit_on()) audit_rs(dev, state, value);
}

void update_tss(IDirect3DDevice9* dev, DWORD stage, DWORD type, DWORD value, HRESULT hr)
{
    if (FAILED(hr) || stage >= 8 || type >= 64) return;
    s_tss[stage][type] = value;
    s_tss_valid[stage][type] = true;
    if (audit_on()) audit_tss(dev, stage, type, value);
}

void update_samp(IDirect3DDevice9* dev, DWORD sampler, DWORD type, DWORD value, HRESULT hr)
{
    if (FAILED(hr) || sampler >= 16 || type >= 16) return;
    s_samp[sampler][type] = value;
    s_samp_valid[sampler][type] = true;
    if (audit_on()) audit_samp(dev, sampler, type, value);
}

void update_tex(IDirect3DDevice9* dev, DWORD stage, void* tex, HRESULT hr)
{
    if (FAILED(hr) || stage >= 16) return;
    s_tex[stage] = tex;
    if (audit_on()) audit_tex(dev, stage, tex);
}

void update_xf(IDirect3DDevice9* dev, DWORD state, const D3DMATRIX* m, HRESULT hr)
{
    if (FAILED(hr) || state >= 258) return;
    s_xf[state] = *m;
    s_xf_valid[state] = true;
    if (audit_on()) audit_xf(dev, state, m);
}

void update_xf_mul(IDirect3DDevice9* dev, DWORD state, const D3DMATRIX* m, HRESULT hr)
{
    // D3D9 语义: 现矩阵 * 给定矩阵(MultiplyTransform, D3DXMatrixMultiply 同序)。
    // 记账发生在真调用之后, 影子里仍是现值, 原地右乘即可。
    if (FAILED(hr) || state >= 258 || !s_xf_valid[state]) return;
    D3DMATRIX cur = s_xf[state];
    D3DMATRIX& out = s_xf[state];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float acc = 0;
            for (int k = 0; k < 4; ++k)
                acc += cur.m[r][k] * m->m[k][c];
            out.m[r][c] = acc;
        }
    if (audit_on()) audit_xf(dev, state, &out);
}

void update_vp(IDirect3DDevice9* dev, const D3DVIEWPORT9* vp, HRESULT hr)
{
    if (FAILED(hr) || !vp) return;
    s_vp = *vp;
    s_vp_valid = true;
    if (audit_on()) audit_vp(dev, vp);
}

void update_fvf(IDirect3DDevice9* dev, DWORD fvf, HRESULT hr)
{
    if (FAILED(hr)) return;
    s_fvf = fvf;
    s_fvf_valid = true;
    if (audit_on()) audit_fvf(dev, fvf);
}

void update_decl(IDirect3DDevice9* dev, void* decl, HRESULT hr)
{
    if (FAILED(hr)) return;
    s_decl = decl;
    s_decl_valid = true;
    if (audit_on()) audit_decl(dev, decl);
}

void update_vs(IDirect3DDevice9* dev, void* vs, HRESULT hr)
{
    if (FAILED(hr)) return;
    s_vs = vs;
    s_vs_valid = true;
    if (audit_on()) audit_vs(dev, vs);
}

void update_ps(IDirect3DDevice9* dev, void* ps, HRESULT hr)
{
    if (FAILED(hr)) return;
    s_ps = ps;
    s_ps_valid = true;
    if (audit_on()) audit_ps(dev, ps);
}

bool borrow_vs(void** out)
{
    if (!s_live || !s_vs_valid) return false;
    *out = s_vs;
    return true;
}

bool borrow_ps(void** out)
{
    if (!s_live || !s_ps_valid) return false;
    *out = s_ps;
    return true;
}

bool copy_xf(DWORD state, D3DMATRIX* out)
{
    if (!s_live || state >= 258 || !s_xf_valid[state]) return false;
    *out = s_xf[state];
    return true;
}

} // namespace shadow

// ---- 对外读口 ----
namespace
{
    bool __cdecl api_live() { return shadow::live(); }
    bool __cdecl api_get_rs(DWORD s, DWORD* o)
    {
        if (!shadow::live() || s >= 256 || !s_rs_valid[s]) return false;
        *o = s_rs[s];
        return true;
    }
    bool __cdecl api_get_tss(DWORD st, DWORD t, DWORD* o)
    {
        if (!shadow::live() || st >= 8 || t >= 64 || !s_tss_valid[st][t]) return false;
        *o = s_tss[st][t];
        return true;
    }
    bool __cdecl api_get_sampler(DWORD sm, DWORD t, DWORD* o)
    {
        if (!shadow::live() || sm >= 16 || t >= 16 || !s_samp_valid[sm][t]) return false;
        *o = s_samp[sm][t];
        return true;
    }
    void* __cdecl api_get_texture(DWORD sm)
    { return (shadow::live() && sm < 16) ? s_tex[sm] : nullptr; }
    bool __cdecl api_get_xf(DWORD st, void* o)
    {
        if (!o || !shadow::copy_xf(st, (D3DMATRIX*)o)) return false;
        return true;
    }
    bool __cdecl api_get_vp(void* o)
    {
        if (!shadow::live() || !s_vp_valid || !o) return false;
        *(D3DVIEWPORT9*)o = s_vp;
        return true;
    }
    bool __cdecl api_get_fvf(DWORD* o)
    {
        if (!shadow::live() || !s_fvf_valid || !o) return false;
        *o = s_fvf;
        return true;
    }
    void* __cdecl api_get_decl()
    { return (shadow::live() && s_decl_valid) ? s_decl : nullptr; }
    void* __cdecl api_get_vs()
    { return (shadow::live() && s_vs_valid) ? s_vs : nullptr; }
    void* __cdecl api_get_ps()
    { return (shadow::live() && s_ps_valid) ? s_ps : nullptr; }

    const gmdx9_shadow_api_v1 s_shadow_api = {
        sizeof(gmdx9_shadow_api_v1),
        1,
        &api_live,
        &api_get_rs,
        &api_get_tss,
        &api_get_sampler,
        &api_get_texture,
        &api_get_xf,
        &api_get_vp,
        &api_get_fvf,
        &api_get_decl,
        &api_get_vs,
        &api_get_ps,
    };
}

extern "C" const gmdx9_shadow_api_v1* __cdecl gmdx9_shadow_api()
{
    return &s_shadow_api;
}
