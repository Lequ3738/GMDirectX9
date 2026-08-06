#include "gm82dx9.h"

extern IDirect3DTexture9 *white_pixel = nullptr;
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

// ============================================================================================
// GM80 移植说明（2026-08-05，已更新：vtable 重映射完成）
// --------------------------------------------------------------------------------------------
// 本文件是 gm82dx9 的注入核心（DllMain 内把 runner 的 Direct3D8 换成 Direct3D9）。
//
// ✅ 已完成（2026-08-06 收尾，全部实测通过——多个真实 GM8 游戏应用后无图形问题）：
//   - D3D8→D3D9 vtable 槽位重映射（~150 个调用点全部换为 8.0 地址，find_bytes 全二进制交叉验证通过）
//   - Direct3DCreate8→9 重定向（D3DCreate@0x484df4 的 call@0x484dff）+ D3D_SDK_VERSION（0x4a1e13 push 32）
//   - CreateDevice 包装（sub_4A1DA0 call@0x4a1f1f + 0x4a1ee7）+ room 读取（rooms 数组 0x58d4cc）
//     + 深度缓冲(EnableAutoDepthStencil D24S8/D16) + HWVP(0x22→0x42)
//   - 屏幕捕获/表面复制包装（GetDisplayMode/CreateImageSurface/GetBackBuffer/CopyRects/SetRenderTarget/GetRenderTarget）
//   - D3DCAPS 接管（GetDeviceCaps 参数改指 &d3d_caps）+ D3DX 14 全局兜底(0x593868–0x59389c)
//   - Reset 挂钩：设备丢失恢复(TestCooperativeLevel==NOTRESET 真 Reset) + 卸载安全(DllMain DETACH 恢复 vtable)
//   - CheckDeviceMultiSampleType 接管（D3D9 多第 6 参 pQualityLevels → 包装补 &quality）
//   - 数学 FPU trampoline：实测 precision=1，8.0 不需要（D3DCREATE_FPU_PRESERVE 生效）
//   - present-params：8.0 栈构造 → 包装内重建（非 8.1 的全局重定向，机制不同）
// 注：文件下方仍保留若干 8.1 专属段落的注释（标 GM80-TODO 或已确认），均为 8.0 确认不需要或
//     已用其它方案解决，注释仅是移植记录。详见 PORTING_NOTES.md。
//
// 重映射方法（SOP §1d）：每个 8.1 补丁点 =（方法, D3D8 槽位），在 8.0 对应函数内按
// `FF 50/FF 90 <槽位>` 定位调用点，反汇编核对语义后替换地址。补丁值与指令编码两版本一致。
// 本轮用修复后的 find_bytes 对每个槽位做全二进制扫描，确认 8.0 全部 device 调用点都已覆盖
// （D3D 模块外的同槽位命中经核实均为非 D3D 接口：自定义 Delphi 接口/VCL/声音等）。
// ============================================================================================

// [GM80] 8.0 的 D3DX 是动态 LoadLibrary，runner 的加载器(sub_49A254)用字符串 "\D3DX8.dll" 拼路径。
// 改指这个插件字符串，runner 就会加载 D3DX9_43.dll（插件随扩展分发），GetProcAddress 解析出 D3DX9 同名函数。
static const char d3dx9_dll_name[] = "\\D3DX9_43.dll";

// [GM80] gm_log/gm_readback/GM_WRITE 已移至 gm_log.cpp/gm_log.h(2026-08-05, 对齐 GMSave 结构)

HRESULT WINAPI CheckDeviceMultiSampleType(IDirect3D9 *d3d, UINT Adapter,
                                          D3DDEVTYPE DeviceType,
                                          D3DFORMAT SurfaceFormat,
                                          BOOL Windowed,
                                          D3DMULTISAMPLE_TYPE MultiSampleType) {
    HRESULT hr = d3d->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, nullptr);
#if GM80_LOG
    gm_log("CheckDeviceMultiSampleType -> 0x%X", (unsigned)hr);
#endif
    return hr;
}

HRESULT WINAPI GetDisplayMode(IDirect3DDevice9 *dev,
                              D3DDISPLAYMODE *pMode
) {
    HRESULT hr = dev->GetDisplayMode(0, pMode);
#if GM80_LOG
    gm_log("GetDisplayMode -> 0x%X w=%u h=%u fmt=%u",
           (unsigned)hr, pMode ? pMode->Width : 0, pMode ? pMode->Height : 0, pMode ? pMode->Format : 0);
#endif
    return hr;
}

HRESULT WINAPI CreateImageSurface(IDirect3DDevice9 *dev,
                                  UINT Width,
                                  UINT Height,
                                  D3DFORMAT Format,
                                  IDirect3DSurface9 **ppSurface
) {
    HRESULT hr = dev->CreateOffscreenPlainSurface(Width, Height, Format, D3DPOOL_SCRATCH, ppSurface, nullptr);
#if GM80_LOG
    gm_log("CreateImageSurface %ux%u fmt=%u -> 0x%X surf=0x%p", Width, Height, Format, (unsigned)hr,
           ppSurface ? *ppSurface : nullptr);
#endif
    return hr;
}

HRESULT WINAPI GetBackBuffer(IDirect3DDevice9 *dev,
                             UINT BackBuffer,
                             D3DBACKBUFFER_TYPE Type,
                             IDirect3DSurface9 **ppBackBuffer
) {
    HRESULT hr = dev->GetBackBuffer(0, BackBuffer, Type, ppBackBuffer);
#if GM80_LOG
    gm_log("GetBackBuffer idx=%u -> 0x%X surf=0x%p", BackBuffer, (unsigned)hr,
           ppBackBuffer ? *ppBackBuffer : nullptr);
#endif
    return hr;
}

HRESULT WINAPI CreateVertexBuffer(IDirect3DDevice9 *dev,
                                  UINT Length,
                                  DWORD Usage,
                                  DWORD FVF,
                                  D3DPOOL Pool,
                                  IDirect3DVertexBuffer9 **ppVertexBuffer
) {
    return dev->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, nullptr);
}

HRESULT WINAPI SetStreamSource(IDirect3DDevice9 *dev,
                               UINT StreamNumber,
                               IDirect3DVertexBuffer9 *pStreamData,
                               UINT Stride
) {
    return dev->SetStreamSource(StreamNumber, pStreamData, 0, Stride);
}

HRESULT WINAPI CreateDepthStencilSurface(IDirect3DDevice9 *dev,
                                         UINT Width,
                                         UINT Height,
                                         D3DFORMAT Format,
                                         D3DMULTISAMPLE_TYPE MultiSample,
                                         IDirect3DSurface9 **ppSurface
) {
    return dev->CreateDepthStencilSurface(Width, Height, Format, MultiSample, 0, FALSE, ppSurface, nullptr);
}

HRESULT
WINAPI SetRenderTarget(IDirect3DDevice9 *dev, IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pNewZStencil) {
    HRESULT hr = dev->SetRenderTarget(0, pRenderTarget);
    if (SUCCEEDED(hr)) hr = dev->SetDepthStencilSurface(pNewZStencil);
#if GM80_LOG
    gm_log("SetRenderTarget -> 0x%X rt=0x%p zs=0x%p", (unsigned)hr, pRenderTarget, pNewZStencil);
#endif
    return hr;
}

HRESULT WINAPI GetRenderTarget(IDirect3DDevice9 *dev, IDirect3DSurface9 **ppRenderTarget) {
    HRESULT hr = dev->GetRenderTarget(0, ppRenderTarget);
#if GM80_LOG
    gm_log("GetRenderTarget -> 0x%X rt=0x%p", (unsigned)hr, ppRenderTarget ? *ppRenderTarget : nullptr);
#endif
    return hr;
}

HRESULT WINAPI CopyRects(IDirect3DDevice9 *dev,
                         IDirect3DSurface9 *pSourceSurface,
                         CONST RECT *pSourceRectsArray,
                         UINT cRects,
                         IDirect3DSurface9 *pDestinationSurface,
                         CONST POINT *pDestPointsArray
) {
    RECT destRect;
    destRect.left = pDestPointsArray->x;
    destRect.top = pDestPointsArray->y;
    destRect.right = destRect.left + (pSourceRectsArray->right - pSourceRectsArray->left);
    destRect.bottom = destRect.top + (pSourceRectsArray->bottom - pSourceRectsArray->top);
    // [GM80] AddDirtyRect 不需要(2026-08-06 分析确认): ① D3D9 表面无法反向取父纹理(无 GetContainer),
    // 无法调用 IDirect3DTexture9::AddDirtyRect; ② CopyRects 目标多为 default-pool 表面(视频内存直写);
    // ③ 若目标为 managed 纹理, D3DXLoadSurfaceFromSurface 内部 LockRect 已把资源标记脏, 下次 SetTexture
    //    整纹理重传, 正确性无碍(AddDirtyRect 仅是局部重传的性能优化)。
    HRESULT hr = D3DXLoadSurfaceFromSurface(pDestinationSurface, nullptr, &destRect, pSourceSurface, nullptr,
                                            pSourceRectsArray, D3DX_FILTER_NONE, 0);
#if GM80_LOG
    gm_log("CopyRects %dx%d src=0x%p dst=0x%p -> 0x%X",
           destRect.right - destRect.left, destRect.bottom - destRect.top,
           pSourceSurface, pDestinationSurface, (unsigned)hr);
#endif
    return hr;
}

HRESULT WINAPI
D3DXGetErrorStringA(
        HRESULT hr,
        LPSTR pBuffer,
        UINT BufferLen) {
    const wchar_t *wstr = DXGetErrorStringW(hr);
    WideCharToMultiByte(CP_ACP, 0, wstr, -1, pBuffer, BufferLen, nullptr, nullptr);
    return S_OK;
}

HRESULT WINAPI screen_refresh(IDirect3DDevice9 *dev, const RECT *pSourceRect, const RECT *pDestRect, HWND hDestOverride, const RGNDATA *pDirtyRegion) {
    dev->EndScene();
    auto res = dev->Present(pSourceRect, pDestRect, hDestOverride, pDirtyRegion);
    dev->BeginScene();
#if GM80_LOG
    gm_log("screen_refresh Present -> 0x%X", (unsigned)res);
#endif
    return res;
}

void WINAPI regain_device() {
    // force exclusive fullscreen off
    d3d_parameters.Windowed = TRUE;
    d3d_parameters.FullScreen_RefreshRateInHz = 0;
    (*runner_display_reset)();
}

// [GM80] SetVertexShader 包装 —— runner 每次 draw 调 SetVertexShader(FVF), D3D9 用 SetFVF 等价。
// 纯 patch 版不再支持插件侧 shader(交给 GMGraphic 等外部 DLL 直接操作 0x58d388 的 D3D9 设备),
// 原 shaders.cpp 里 using_shader/vertex-declaration 分支已随 shader 功能一并移除。
// 不记日志(每帧调用数千次会刷爆日志文件)。
HRESULT WINAPI SetVertexShader(IDirect3DDevice9 *dev, DWORD fvf) {
    return dev->SetFVF(fvf);
}

short old_cw = 0;
short new_cw = 0;

// [GM80] Reset 接管(2026-08-05 实机确认): 8.0 runner 在 INNER_display_set_size 栈上构造 D3D8 布局的
// present params 传给 device Reset(D3D8 槽 0x38→补丁到 D3D9 槽 0x40), D3D9 按 D3D9 布局读 → 字段错位
// (hDeviceWindow 读到 D3D8.Windowed 等) → Reset 失败返回 0 → d3d_start 弹 "Failed to use 3D mode"。
// 方案: CreateDevice 成功后把设备 vtable 槽 0x40 改指向 ResetDevice 包装, 用创建时的干净 D3D9 pp 副本
// 调真实 Reset(D3D9 要求 Reset 参数与 CreateDevice 一致)。一次挂钩覆盖 d3d_start/d3d_end/display_set_size/display_reset 等全部 Reset 调用点。
static D3DPRESENT_PARAMETERS g_pp9;   // 设备创建时的 pp9 副本(与 CreateDevice 完全一致 → Reset 必成功)
static HWND g_window = nullptr;       // CreateDevice 传入的有效窗口
static HRESULT (WINAPI* real_reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*) = nullptr;  // 原始 D3D9 Reset

// [GM80] CheckDeviceMultiSampleType 接管(2026-08-06): IDA 验证 sub_4A5054@0x4a50ef 调 D3D 对象槽 0x2C
// = CheckDeviceMultiSampleType。D3D9 版本比 D3D8 多第 6 参 pQualityLevels, 8.0 runner 按 D3D8 只传 5 参
// → D3D9 读栈垃圾当输出指针写 → 潜在崩溃(真实游戏实测未触发, 保险起见接管)。
// 方案: D3D 对象 vtable 槽 0x2C → 包装(补 &quality)。
static HRESULT (WINAPI* real_check_ms)(IDirect3D9*, UINT, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE, DWORD*) = nullptr;

HRESULT WINAPI CheckDeviceMultiSampleType_wrap(IDirect3D9 *d3d9, UINT Adapter, D3DDEVTYPE DeviceType,
                                               D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType) {
    DWORD quality = 0;
    if (real_check_ms)
        return real_check_ms(d3d9, Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, &quality);
    return D3DERR_INVALIDCALL;
}

HRESULT WINAPI ResetDevice(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pParams) {
    // [GM80] 设备丢失恢复(2026-08-06, 对齐 gm82dx9 regain_device):
    //   IDA 验证(8.0 D3D_CreateDevice @0x4a2708): Present(0x4a27ab)失败 → INNER_display_set_size
    //   (0x4a27be) → device Reset(槽 0x40)。即 runner 在设备丢失时靠 Reset 恢复。
    //   这里用 TestCooperativeLevel 判定:
    //     D3DERR_DEVICENOTRESET → 设备丢失且可恢复 → 真 Reset(g_pp9 与 CreateDevice 一致 → 必成功)
    //     S_OK                 → 设备正常 → no-op(避免 d3d_start/d3d_end 每帧真 Reset 黑屏)
    //     D3DERR_DEVICELOST    → 暂时不可恢复 → no-op(等下一帧)
    //   GM8 只支持无边框全屏(2026-08-06 确认), 恢复时保持 windowed。
    (void)pParams;
    HRESULT tcl = dev->TestCooperativeLevel();
    if (tcl == D3DERR_DEVICENOTRESET && real_reset) {
        HRESULT hr = real_reset(dev, &g_pp9);
#if GM80_LOG
        gm_log("ResetDevice -> RECOVER 0x%X (device was lost, reset windowed)", (unsigned)hr);
#endif
        return hr;
    }
    return S_OK;
}

// [GM80] DLL 卸载安全(2026-08-06, 对齐 gm82dx9 last_resort):
//   IDA 验证: 8.0 卸载扩展 DLL 走 INNER_external_free(0x518764)循环 FreeLibrary。
//   我们此前把设备 vtable 槽 0x40 改指 ResetDevice(DLL 内), 若 DLL 卸载而设备仍活着,
//   runner 再调 Reset 会跳未映射内存 → 崩溃。此函数在 DllMain(DLL_PROCESS_DETACH) 恢复 vtable。
//   用 __try/__except 兜底(设备可能已释放), 且仅当槽 0x40 仍是我们的钩子才恢复。
void gm80_restore_reset_hook(void) {
    __try {
        IDirect3DDevice9 *dev = *(IDirect3DDevice9 **)0x58d388;
        if (!dev || !real_reset) return;
        void **vt = *(void ***)dev;
        if (!vt) return;
        if (vt[16] != (void *)&ResetDevice) return;   // 钩子已被别处改过 → 不碰
        DWORD oldp;
        if (VirtualProtect(&vt[16], sizeof(void *), PAGE_EXECUTE_READWRITE, &oldp)) {
            vt[16] = (void *)real_reset;
            VirtualProtect(&vt[16], sizeof(void *), oldp, &oldp);
#if GM80_LOG
            gm_log("  vtable[0x40] Reset hook restored (DLL unload)");
#endif
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // 设备已释放/不可读 → 忽略(进程/游戏正在结束)
    }
}

HRESULT WINAPI CreateDevice(IDirect3D9 *d3d9, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
                            D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice9 **ppReturnedDeviceInterface
) {
    // _control87 doesn't seem to let us reset the control word, so we're going asm
    if (old_cw == 0) {
        _asm fnstcw [old_cw];
        new_cw = old_cw | 0x3f;
        _asm fldcw [new_cw];
    }

    present_params = pPresentationParameters;

#if GM80_LOG
    gm_log("CreateDevice: adapter=%u devtype=%u hwnd=0x%p flags=0x%X", Adapter, DeviceType, hFocusWindow, BehaviorFlags);
    if (pPresentationParameters) {
        gm_log("  present : %ux%u fmt=%u win=%d autod=%d(%u) ms=%u swap=%u hwnd=0x%p rt=%u iv=%u",
               pPresentationParameters->BackBufferWidth, pPresentationParameters->BackBufferHeight,
               pPresentationParameters->BackBufferFormat, (int)pPresentationParameters->Windowed,
               (int)pPresentationParameters->EnableAutoDepthStencil, pPresentationParameters->AutoDepthStencilFormat,
               pPresentationParameters->MultiSampleType, pPresentationParameters->SwapEffect,
               pPresentationParameters->hDeviceWindow, pPresentationParameters->FullScreen_RefreshRateInHz,
               pPresentationParameters->PresentationInterval);
    } else {
        gm_log("  present : (null)");
    }
#endif

	// [GM80] 分辨率可调修复(2026-08-06): 原实现(下方注释)把 backbuffer 缩到首个房间的
	// 视图尺寸, 之后 room_set_view 把渲染尺寸/视口改成多大, 都被这个背缓冲夹住 →
	// 可见区域永远卡在初始尺寸(本游戏 800x450, 左上角, 其余纯黑)。
	// IDA 证据(GM8.0 空工程.exe):
	//   - Present 源矩形 = (0,0,renderWidth,renderHeight), 即 GMDIRECT3DINFO 0x58d38c/0x58d390
	//     (D3D_CreateDevice@0x4a2708)。
	//   - 换分辨率走 sub_4A236C@0x4a236c: 只更新 renderWidth/Height + D3D_SetViewport, 不调 Reset
	//     → backbuffer 尺寸自创建后不变。
	//   - D3D8 下 runner 的 sub_4A1DA0 用显示器尺寸(GetDisplayMode)创建 backbuffer, 恒 ≥ 任何
	//     渲染尺寸 → 换分辨率正常。故这里必须保持 runner 传入的 backbuffer 尺寸(显示器), 不缩。
	// 原代码(2026-08-05 加入, 现删除):
	/*
	// get window size from first room
	// [GM80] 8.1: first_room_id = **(int**)0x8452d4; rooms = (*(char***)0x686a4c)[id]
	// 8.0: rooms 数组 = 0x58d4cc（id→room 指针，count=0x58d4d4，GMAPI/INNER_room_set_persistent 确认）；
	//      首个房间 id 取 0（GM 房间从 0 编号）。room 结构体偏移（[0x40] views、[3]/[4] 宽高、+0x44 视图）已确认与 8.1 一致。
	int first_room_id = 0;
	char *room_ptr = (*(char***)0x58d4cc)[first_room_id];
	int desired_width = 0, desired_height = 0;
	if (room_ptr[0x40]) {
		// views
		for (int i = 0; i < 8; i++) {
			int *view_ptr = *(int**)(room_ptr + 0x44 + i * 4);
			int port_width = view_ptr[6] + view_ptr[8];
			int port_height = view_ptr[7] + view_ptr[9];
			if (port_width > desired_width)
				desired_width = port_width;
			if (port_height > desired_height)
				desired_height = port_height;
		}
	} else {
		// no views
		desired_width = ((int*)room_ptr)[3];
		desired_height = ((int*)room_ptr)[4];
	}
	if (desired_width != 0 && desired_height != 0 && desired_width < present_params->BackBufferWidth && desired_height < present_params->BackBufferHeight) {
		present_params->BackBufferWidth = desired_width;
		present_params->BackBufferHeight = desired_height;
	}
	*/

#if GM80_LOG
    gm_log("  backbuffer -> %ux%u (windowed, 保持 runner 传入尺寸, 不再缩到首个房间)",
           present_params ? present_params->BackBufferWidth : 0, present_params ? present_params->BackBufferHeight : 0);
    // D3DX 接管自检: sub_49A254 把解析出的函数写入 14 个全局 0x593868–0x59389c。
    // 位图非零 => 加载器已运行; 若同时 d3dx9_43.dll 已加载 => 这些就是 D3DX9 函数(补丁生效)。
    // 全零 => 加载器尚未运行(补丁还没被消费, 需等运行时再看)或 DLL 名补丁没生效。
    {
        void **dx = (void**)0x593868;
        unsigned bmp = 0;
        for (int i = 0; i < 14; i++) { if (dx[i]) bmp |= (1u << i); }
        gm_log("  d3dx globals 0x593868[14]: bitmap=0x%X d3dx9_43.dll=%s",
               bmp, GetModuleHandleA("D3DX9_43.dll") ? "LOADED" : "not loaded");
    }
#endif
    
    // [GM80] 8.0 runner 在栈上构造的是 D3D8 布局的 present params —— D3D8 没有 MultiSampleQuality 字段,
    // 从 MultiSampleType 起字段在 D3D9 视角全部错位: D3D9.hDeviceWindow(0x1C)读到的是 D3D8.Windowed,
    // 实机是 0xFFFFFFFF 栈垃圾; D3D9.Windowed(0x20)读到的是 D3D8.EnableAutoDepthStencil。传给 CreateDevice
    // 即 D3DERR_NOTAVAILABLE(0x8876086C)。只有开头 4 个字段(0x00-0x0C)两版布局一致, 可安全采用。
    // 探针(2026-08-05, d3d9probe)确认: 有效窗口 + 干净参数的 HAL 设备本机可正常创建(全屏/窗口皆 D3D_OK)。
    D3DPRESENT_PARAMETERS pp9;
    memset(&pp9, 0, sizeof(pp9));
    if (present_params) {
        pp9.BackBufferWidth = present_params->BackBufferWidth;
        pp9.BackBufferHeight = present_params->BackBufferHeight;
        pp9.BackBufferFormat = present_params->BackBufferFormat;
        pp9.BackBufferCount = present_params->BackBufferCount ? present_params->BackBufferCount : 1;
    }
    pp9.SwapEffect = D3DSWAPEFFECT_COPY;
    pp9.hDeviceWindow = hFocusWindow;   // 不读 runner 的字段(错位垃圾); 焦点窗口已验证有效
    // [GM80] GM8 只支持无边框全屏(2026-08-06 确认): "全屏"= runner 把窗口放大到显示尺寸,
    // 设备始终 windowed, Present 自动拉伸到窗口。绝不能设 Windowed=FALSE(D3D9 独占全屏 ≠ GM8 行为)。
    pp9.Windowed = TRUE;
    pp9.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    // [GM80] 深度缓冲(2026-08-06, 对齐 gm82dx9 d3d_parameters): 启用 auto-depth-stencil,
    // 3D 游戏的 d3d_set_hidden(z-test)/d3d_clear_depth 依赖它。D24S8 不支持则回退 D16。
    pp9.EnableAutoDepthStencil = TRUE;
    pp9.AutoDepthStencilFormat = D3DFMT_D24S8;
    if (d3d9->CheckDeviceFormat(Adapter, DeviceType, pp9.BackBufferFormat,
                                D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D24S8) != D3D_OK) {
#if GM80_LOG
        gm_log("  D24S8 depth not supported -> D16");
#endif
        pp9.AutoDepthStencilFormat = D3DFMT_D16;
    }
#if GM80_LOG
    {
        int count = d3d9 ? d3d9->GetAdapterCount() : -1;
        D3DDISPLAYMODE dm;
        HRESULT dmr = d3d9 ? d3d9->GetAdapterDisplayMode(Adapter, &dm) : 0x80000000L;
        gm_log("  diag: adapters=%d displaymode=%ux%u fmt=%u iswindow=%d",
               count, dm.Width, dm.Height, dm.Format,
               hFocusWindow ? IsWindow((HWND)hFocusWindow) : -1);
    }
#endif
    // [GM80] 硬件 VP: 8.0 runner 传 BehaviorFlags=0x22(SWVP|FPU_PRESERVE), 而 gm82dx9-on-8.2 是 0x42(HWVP)。
    // 顶点着色器要走硬件必须在 CreateDevice 用 HWVP —— 这里把 0x20(SWVP) 换成 0x40(HWVP), 保留 FPU_PRESERVE。
    // 若 HWVP 创建失败(老显卡/远程桌面)则回退原始 flags(SWVP)。
    DWORD bf_orig = BehaviorFlags;
    DWORD bf_hw = (bf_orig & ~D3DCREATE_SOFTWARE_VERTEXPROCESSING) | D3DCREATE_HARDWARE_VERTEXPROCESSING;
    DWORD bf_used = bf_hw;
#if GM80_LOG
    gm_log("  behavior flags: runner=0x%X hwvp=0x%X", bf_orig, bf_hw);
#endif
    auto res = d3d9->CreateDevice(Adapter, DeviceType, hFocusWindow, bf_hw, &pp9, ppReturnedDeviceInterface);
    if (FAILED(res) && bf_hw != bf_orig) {
        gm_log("  HWVP(0x%X) failed 0x%X -> retry original(0x%X)", bf_hw, (unsigned)res, bf_orig);
        res = d3d9->CreateDevice(Adapter, DeviceType, hFocusWindow, bf_orig, &pp9, ppReturnedDeviceInterface);
        bf_used = SUCCEEDED(res) ? bf_orig : bf_hw;
    }
    // [GM80] 全屏失败(D3DERR_NOTAVAILABLE 常见于基本显示适配器/远程桌面不支持全屏) → 回退窗口模式
    if (FAILED(res) && !pp9.Windowed) {
        gm_log("  fullscreen CreateDevice failed 0x%X -> retry windowed", (unsigned)res);
        pp9.Windowed = TRUE;
        if (!pp9.BackBufferWidth) pp9.BackBufferWidth = 640;
        if (!pp9.BackBufferHeight) pp9.BackBufferHeight = 480;
        res = d3d9->CreateDevice(Adapter, DeviceType, hFocusWindow, bf_used, &pp9, ppReturnedDeviceInterface);
    }
#if GM80_LOG
    gm_log("  CreateDevice -> 0x%X device=0x%p windowed=%d", (unsigned)res,
           (ppReturnedDeviceInterface && *ppReturnedDeviceInterface) ? *ppReturnedDeviceInterface : nullptr,
           pp9.Windowed);
#endif

    // [GM80] Reset 接管(2026-08-05): 设备创建成功后把 vtable 槽 0x40(Reset)重定向到 ResetDevice 包装。
    // 8.0 runner 的 Reset 调用(传 D3D8 布局 present params)经此包装用创建时的干净 pp9 调真实 Reset。
    if (SUCCEEDED(res) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        g_window = hFocusWindow;
        g_pp9 = pp9;
        if (!real_reset) {
            IDirect3DDevice9 *dev = *ppReturnedDeviceInterface;
            void **vt = *(void ***)dev;
            real_reset = (HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*))vt[16]; // 槽 0x40 = Reset
            DWORD oldp;
            if (VirtualProtect(&vt[16], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldp)) {
                vt[16] = (void*)&ResetDevice;
                VirtualProtect(&vt[16], sizeof(void*), oldp, &oldp);
                gm_log("  vtable[0x40] Reset hooked -> ResetDevice, real_reset=0x%p", (void*)real_reset);
            } else {
                gm_log("  vtable[0x40] hook FAILED (VirtualProtect err=%lu)", GetLastError());
            }
        }
        // [GM80] CheckDeviceMultiSampleType 接管: D3D 对象 vtable 槽 0x2C → 包装(补 pQualityLevels)。
        if (!real_check_ms) {
            void **d3d9_vt = *(void ***)d3d9;
            real_check_ms = (HRESULT (WINAPI*)(IDirect3D9*, UINT, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE, DWORD*))d3d9_vt[11]; // 槽 0x2C/4 = 11
            DWORD oldp2;
            if (VirtualProtect(&d3d9_vt[11], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldp2)) {
                d3d9_vt[11] = (void*)&CheckDeviceMultiSampleType_wrap;
                VirtualProtect(&d3d9_vt[11], sizeof(void*), oldp2, &oldp2);
                gm_log("  d3dobj vtable[0x2C] CheckDeviceMultiSampleType hooked -> wrap");
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

typedef D3DXMATRIX* (__stdcall *d3dx_matrix_func)(
	D3DXMATRIX *pOut,
	FLOAT      w,
	FLOAT      h,
	FLOAT      zn,
	FLOAT      zf);
	
d3dx_matrix_func D3DXMatrixPerspectiveLH_ptr, D3DXMatrixOrthoLH_ptr;

D3DXMATRIX* __stdcall D3DXMatrixPerspectiveLH_inj(
	D3DXMATRIX *pOut,
	FLOAT      w,
	FLOAT      h,
	FLOAT      zn,
	FLOAT      zf
) {
	_asm { fldcw [old_cw] }
	D3DXMatrixPerspectiveLH_ptr(pOut, w, h, zn, zf);
	_asm { fldcw [new_cw] }
}

D3DXMATRIX* __stdcall D3DXMatrixOrthoLH_inj(
	D3DXMATRIX *pOut,
	FLOAT      w,
	FLOAT      h,
	FLOAT      zn,
	FLOAT      zf
) {
	_asm { fldcw [old_cw] }
	D3DXMatrixOrthoLH_ptr(pOut, w, h, zn, zf);
	_asm { fldcw [new_cw] }
}

uint8_t white_pixel_tga[] = {
    0, 0, 2, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 1, 0, 1, 0,
    32, 0, 255, 255, 255, 255
};
void create_white_pixel() {
    D3DXCreateTextureFromFileInMemory(Device, white_pixel_tga, 22, &white_pixel);
}

HRESULT WINAPI SetNullTexture(IDirect3DDevice9 *dev, DWORD Stage, IDirect3DBaseTexture9 *pTexture) {
    return dev->SetTexture(0, white_pixel);
}

const uint8_t reset_patch[] = {
        // note: 0xdc = sizeof(DEVMODEW), 0x44 = offset(dmSize), 0x46 = offset(dmDriverExtra),
        // 0xa0 = offset(dmBitsPerPel)

        // sub esp, 0xdc
        0x81, 0xec, 0xdc, 0x00, 0x00, 0x00,

        // get registry settings
        // mov dword ptr [esp+0x44], 0xdc
        0xc7, 0x44, 0x24, 0x44, 0xdc, 0x00, 0x00, 0x00,
        // push esp
        0x54,
        // push -2
        0x6a, 0xfe,
        // push 0
        0x6a, 0x00,
        // call EnumDisplaySettingsW
        0xe8, 0xf0, 0xf8, 0xde, 0xff,

        // get current settings
        // sub esp, 0xdc
        0x81, 0xec, 0xdc, 0x00, 0x00, 0x00,
        // mov word ptr [esp+0x44], 0xdc
        0xc7, 0x44, 0x24, 0x44, 0xdc, 0x00, 0x00, 0x00,
        // push esp
        0x54,
        // push -1
        0x6a, 0xff,
        // push 0
        0x6a, 0x00,
        // call EnumDisplaySettingsW
        0xe8, 0xd8, 0xf8, 0xde, 0xff,

        // compare blocks with
        // lea eax, [esp+0xa8]
        0x8d, 0x84, 0x24, 0xa8, 0x00, 0x00, 0x00,
        // lea edx, [esp+0x184]
        0x8d, 0x94, 0x24, 0x84, 0x01, 0x00, 0x00,
        // xor ecx, ecx
        0x33, 0xc9,
        // mov cl, 0x14
        0xb1, 0x14,
        // call CompareMem
        0xe8, 0xb5, 0x68, 0xdf, 0xff,

        // are they the same?
        // test al, al
        0x84, 0xc0,
        // jne 2f
        0x75, 0x09,

        // restore display settings
        // push 0 (x2)
        0x6a, 0x00, 0x6a, 0x00,
        // call ChangeDisplaySettingsW
        0xe8, 0x18, 0xf7, 0xde, 0xff,

        // exit
        // add esp, 0x1b8
        0x81, 0xc4, 0xb8, 0x01, 0x00, 0x00,
        // ret
        0xc3,
};

// [GM80] HINSTANCE my_handle 定义已移至根目录 dllmain.cpp(声明在 gm82dx9.h)

// called when the dll is unloaded
// [GM80] 卸载安全已在 DllMain(DLL_PROCESS_DETACH) 用 gm80_restore_reset_hook() 实现(2026-08-06,
// 对齐 gm82dx9 的 last_resort)。此 last_resort_impl/last_resort 是 8.1 原版的裸汇编挂钩, 8.0 不安装
// 该挂钩(0x5795c5), 为死代码。体内 8.1 地址(0x56c094/0x61ede0/0x561bb0/0x40dd98)在 8.0 上不能用, 置空。
void WINAPI last_resort_impl(HANDLE hLibModule) {
    (void)hLibModule;
    return;
#if 0  // 8.1 版本，8.0 禁用
    if (hLibModule == my_handle && Device == nullptr) {
        // clear the surfaces early, while the dll is still loaded
        ((void(*)(void))0x56c094)();
        // overwrite the display reset function to not reset if we're at default
        HANDLE proc = GetCurrentProcess();
        WriteProcessMemory(proc, (void*)(0x61ede0), reset_patch, sizeof(reset_patch), nullptr);
        int ptr = 0x61ede0 - (0x561bb0 + 5);
        WriteProcessMemory(proc, (void*)(0x561bb0 + 1), &ptr, 4, nullptr);
        FlushInstructionCache(proc, (void*)0x61ede0, sizeof(reset_patch));
        FlushInstructionCache(proc, (void*)0x561bb0, 5);
    }
#endif
}

__declspec(naked) void last_resort() {
    _asm {
        push [esp+4]
        call last_resort_impl
        mov eax, 0x40dd98
        jmp eax
    }
}

// [GM80] DllMain 已移至根目录 dllmain.cpp(结构对齐 GMSave)。此处是补丁主体,
// 由 dllmain.cpp 的 DllMain 在 DLL_PROCESS_ATTACH 时调用。
bool gm80_apply_patches(void) {

#if GM80_LOG
    {
        char exe[MAX_PATH], dll[MAX_PATH];
        GetModuleFileNameA(NULL, exe, sizeof(exe));
        GetModuleFileNameA(my_handle, dll, sizeof(dll));
        gm_log("=== GM82DX9 DLL_PROCESS_ATTACH (pid=%u) ===", GetCurrentProcessId());
        gm_log("  exe : %s", exe);
        gm_log("  dll : %s", dll);
    }
#endif

    // =========================================================================================
    // [GM80] DllMain 补丁入口。下方大部分地址已重映射到 8.0（vtable 表见后文 PATCH_* 段）。
    // 注释禁用的 8.1 专属段落(present-params/D3DX/数学/regain 等)均已用 8.0 方案解决或确认不需要
    // (见各段 GM80-确认 标记), 不会在 8.0 上误写。已实现的 8.0 核心：
    //   Direct3DCreate8→9（D3DCreate@0x484df4 的 call@0x484dff）+ SDK 版本（0x4a1e13 push 32）
    //   CreateDevice 包装（sub_4A1DA0 call@0x4a1f1f）
    // =========================================================================================
    HANDLE proc = GetCurrentProcess();

    void *ptr;
    uint16_t offset;

    // [GM80] SDK 版本：8.0 在 sub_4A1DA0 内 `push 0DCh` @0x4a1e13（D3D_SDK_VERSION=220），改 `push 0x20`(32)。
    // 必须用 5 字节 `68 20 00 00 00`（与原 push imm32 同长）。原补丁 2 字节 `6A 20` 留下 3 字节 `00 00 00`，
    // 被解码成 `add [eax],al`，在 eax=0x20(色深 32 检查)时写地址 0x20 → 访问违例崩溃（实机复现，2026-08-05）。
    {
        uint8_t push32[] = {0x68, 0x20, 0x00, 0x00, 0x00};
        WriteProcessMemory(proc, (void *)(0x4a1e13), push32, 5, nullptr);
    }

    // [GM80] Direct3DCreate8→9：8.0 的 D3DCreate@0x484df4 内 `call sub_484DEC`(导入thunk) @0x484dff，
    // 把 rel32 改指 Direct3DCreate9（基址 0x400000，0x484dff+5=0x484e04）
    {
        ptr = (char *)(&Direct3DCreate9) - (0x484e04);
        WriteProcessMemory(proc, (void *)(0x484e00), &ptr, 4, nullptr);
    }

    // [GM80] present-params 已接管(2026-08-06)：8.1 是把 runner 的 present-params 全局(0x85B38C)引用改指
    // d3d_parameters；8.0 的 present params 是在 sub_4A1DA0(0x4a1ea2 附近)与 INNER_display_set_size(0x4a228f
    // 附近)栈上构造，机制不同 → 改为在 CreateDevice 包装内重建干净 D3D9 pp9（见下方 CreateDevice 包装）。
    // 原"格式来源改读 d3d_parameters"方案用于 set_alpha_buffer，纯 patch 版已移除该导出，不再需要。
    // 以下 8.1 地址段在 8.0 不可用，注释禁用保留作移植记录：
    // // D3DPRESENT_PARAMETERS setup
    // ptr = &d3d_parameters;
    // WriteProcessMemory(proc, (void *) (0x61edfd + 1), &ptr, 4, nullptr);
    // WriteProcessMemory(proc, (void *) (0x61faa1 + 1), &ptr, 4, nullptr);
    // WriteProcessMemory(proc, (void *) (0x61fad0 + 1), &ptr, 4, nullptr);
    // WriteProcessMemory(proc, (void *) (0x61fb15 + 1), &ptr, 4, nullptr);
    // uint8_t short_jmp[] = {0xeb, 0x26};
    // WriteProcessMemory(proc, (void *) (0x61eece), short_jmp, 2, nullptr);
    // uint8_t nops[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    // offset = 0x18;
    // WriteProcessMemory(proc, (void *) (0x61ef11 + 2), &offset, 1, nullptr);
    // WriteProcessMemory(proc, (void *) (0x61ef09), nops, 3, nullptr);
    // WriteProcessMemory(proc, (void *) (0x61ef19), nops, 7, nullptr);
    // WriteProcessMemory(proc, (void *) (0x61ef25), nops, 3, nullptr);
    // // set AutoDepthStencilFormat on fail
    // offset = 0x28;
    // WriteProcessMemory(proc, (void *) (0x61efb2 + 2), &offset, 1, nullptr);
    // // set SwapEffect on fail
    // offset = 0x18;
    // WriteProcessMemory(proc, (void *) (0x61f015 + 2), &offset, 1, nullptr);

    // [GM80] D3DCAPS 接管：8.0 调 device GetDeviceCaps（槽 0x1C，两版本相同无需改槽），
    // 但 D3D9 的 D3DCAPS9 比 D3DCAPS8 大，runner 的 caps 缓冲(unk_6C7244)会溢出——必须改指插件 d3d_caps。
    // 两个 `push offset unk_6C7244`（68 44 72 6C 00）：0x4a1f3e(sub_4A1DA0)、0x4a2309(INNER_display_set_size)。
    // 注：d3d_caps 是插件全局（D3DCAPS9 大小），runner 读取 MaxTextureWidth 等字段偏移与 D3DCAPS9 对齐。
    ptr = &d3d_caps;
    WriteProcessMemory(proc, (void *)(0x4a1f3e + 1), &ptr, 4, nullptr);
    WriteProcessMemory(proc, (void *)(0x4a2309 + 1), &ptr, 4, nullptr);

    // [GM80] present-params 已接管(2026-08-06)：8.1 是改 runner 的 present-params 全局引用指 d3d_parameters；
    // 8.0 的 present params 在 sub_4A1DA0/INNER_display_set_size 栈上构造（无全局可重定向）→ 改为在 CreateDevice
    // 包装内重建干净 D3D9 布局的 pp9（原方案"改格式来源读 d3d_parameters.BackBufferFormat"用于 set_alpha_buffer，
    // 纯 patch 版已移除 set_alpha_buffer 导出，此方案不再需要）。
    // 注意：8.0 的 present_params（CreateDevice 包装捕获）指向 sub_4A1DA0 的栈数组，仅在 CreateDevice 期间有效
    // （room-sizing 需要操作真实指针，见 CreateDevice）。resize_backbuffer 已不再写 present_params（见 gm82dx9.cpp）。
    // set_alpha_buffer/set_fullscreen 当前只改 d3d_parameters（插件全局，安全但 runner 不读），待格式来源补丁后生效。

    // [GM80] CheckDeviceMultiSampleType 已接管(2026-08-06)：8.0 在 sub_4A5054@0x4a50ef 调 D3D 对象槽 0x2C
    // = CheckDeviceMultiSampleType。D3D9 比 D3D8 多第 6 参 pQualityLevels → CreateDevice hook 里把 D3D 对象
    // vtable 槽 0x2C 改指 CheckDeviceMultiSampleType_wrap(补 &quality)。0x4a50fa 槽 0x20 = GetAdapterDisplayMode
    // (D3D8/9 签名相同, 无需接管)。
    // （8.1 地址段已移除）

    // [GM80] CreateDevice：8.0 sub_4A1DA0 的 CreateDevice 调用（call@0x4a1f1f，槽 0x3C），
    // 整段重定向到插件 CreateDevice 包装（含 present_params 捕获 + FPU 控制字）
#define PATCH(a) \
                 offset = 0xe8; \
                 GM_WRITE((a), &offset, 1); \
                 ptr = ((char*)(&CreateDevice) - (a + 5)); \
                 GM_WRITE((a + 1), &ptr, 4)
    PATCH(0x4a1f1d); // CreateDevice 重试调用 (sub_4A1DA0, call@0x4a1f1f)
    PATCH(0x4a1ee5); // CreateDevice 第一次尝试 (sub_4A1DA0, call@0x4a1ee7 槽 0x3C)
    // [GM80] 2026-08-05 实机确认：第一个 CreateDevice(0x4a1ee7)在 D3D9 对象上调用槽 0x3C(非 CreateDevice,
    // 是 GetAdapterMonitor)——返回假的成功但不创建设备(0x58d388 保持 NULL),且 __stdcall 栈错位,导致
    // 后续 GetDeviceCaps 区崩溃(0x4a1f49 读 0)。必须与重试调用一起重定向到包装函数。
#undef PATCH

    // [GM80] D3DX 接管：把 sub_49A254 加载器的 DLL 名字符串（mov edx, offset "\D3DX8.dll" @0x49a27b）
    // 改指插件字符串 "\D3DX9_43.dll"。runner 随后 LoadLibraryA("游戏目录\D3DX9_43.dll")，
    // GetProcAddress 解析出 D3DX9 同名函数（D3DXMatrix* / D3DXCreateTexture / D3DXCheckTextureRequirements /
    // D3DXLoadSurfaceFromMemory），存入 14 个全局 0x593868–0x59389c。这样纹理创建走 D3DX9，产出 D3D9 对象。
    ptr = (void*)d3dx9_dll_name;
    WriteProcessMemory(proc, (void *)(0x49a27b + 1), &ptr, 4, nullptr);

    // [GM80] D3DX 时序兜底（2026-08-05 实机确认）：sub_49A254 在 sub_545620（先于扩展加载）运行，
    // 字符串补丁来不及生效，加载的是原始 "\D3DX8.dll"（Win11 无此文件 → LoadLibrary 失败 → 14 个全局 NULL）。
    // 在此直接把 14 个全局(0x593868–0x59389c)写成 D3DX9_43.dll 的导出指针（顺序与 sub_49A254 一致）。
    {
        HMODULE d3dx9 = GetModuleHandleA("D3DX9_43.dll");
        if (!d3dx9) d3dx9 = LoadLibraryA("D3DX9_43.dll");
        static const char *const d3dx9_names[14] = {
            "D3DXMatrixScaling", "D3DXMatrixTranslation", "D3DXMatrixRotationX",
            "D3DXMatrixRotationY", "D3DXMatrixRotationZ", "D3DXMatrixRotationAxis",
            "D3DXMatrixMultiply", "D3DXMatrixLookAtLH", "D3DXMatrixPerspectiveFovLH",
            "D3DXMatrixPerspectiveLH", "D3DXMatrixOrthoLH",
            "D3DXCheckTextureRequirements", "D3DXCreateTexture", "D3DXLoadSurfaceFromMemory",
        };
        void **dx = (void**)0x593868;
        for (int i = 0; i < 14; i++) {
            void *fn = d3dx9 ? (void*)GetProcAddress(d3dx9, d3dx9_names[i]) : nullptr;
            WriteProcessMemory(proc, (void*)(0x593868 + i * 4), &fn, 4, nullptr);
        }
#if GM80_LOG
        {
            unsigned bmp = 0;
            for (int i = 0; i < 14; i++) if (dx[i]) bmp |= (1u << i);
            gm_log("  D3DX globals fallback: d3dx9_43=%s bitmap=0x%X",
                   d3dx9 ? "loaded" : "NOT LOADED", bmp);
        }
#endif
    }

    // [GM80] wrapper 重定向（8.1 用 `E8 rel32` 替换 `mov eax,[eax]`+`call [eax+sz3]` 5字节；
    // sz6 站点用 `90 E8 rel32` 6字节等长替换。8.0 站点同样适用。）
    // ✅ [GM80-确认] CreateVertexBuffer(0x5C)/CreateDepthStencilSurface(0x68)/SetStreamSource(0x14C)：
    //   8.0 全模块扫描未发现这些槽位站点（8.0 原语用 DrawPrimitiveUP），8.1 的这三段补丁不适用。
    //   深度缓冲已通过 CreateDevice pp9 的 EnableAutoDepthStencil 启用（2026-08-06）。

    // SetRenderTarget (0x7C, sz3) —— INNER_surface_set_target / INNER_surface_reset_target
#define PATCH(a) \
                 offset = 0xe8; \
                 GM_WRITE((a), &offset, 1); \
                 ptr = ((char*)(&SetRenderTarget) - (a + 5)); \
                 GM_WRITE((a + 1), &ptr, 4)
    PATCH(0x4a0edb); // SetRenderTarget (INNER_surface_set_target, call@0x4a0edd)
    PATCH(0x4a0f99); // SetRenderTarget (INNER_surface_reset_target, call@0x4a0f9b)
#undef PATCH

    // CopyRects (0x70, sz3) —— 屏幕捕获 / surface copy
#define PATCH(a) \
                 offset = 0xe8; \
                 GM_WRITE((a), &offset, 1); \
                 ptr = ((char*)(&CopyRects) - (a + 5)); \
                 GM_WRITE((a + 1), &ptr, 4)
    PATCH(0x4a143f); // CopyRects (sub_4A12F8 surface copy, call@0x4a1441)
    PATCH(0x4a173d); // CopyRects (call@0x4a173f)
    PATCH(0x4a29a5); // CopyRects (sub_4A286C 屏幕捕获, call@0x4a29a7)
#undef PATCH

    // GetDisplayMode (0x20, sz3) —— 屏幕捕获
#define PATCH(a) \
                 offset = 0xe8; \
                 GM_WRITE((a), &offset, 1); \
                 ptr = ((char*)(&GetDisplayMode) - (a + 5)); \
                 GM_WRITE((a + 1), &ptr, 4)
    PATCH(0x4a28f4); // GetDisplayMode (sub_4A286C, call@0x4a28f6)
#undef PATCH

    // CreateImageSurface (0x6C, sz3)
#define PATCH(a) \
                 offset = 0xe8; \
                 GM_WRITE((a), &offset, 1); \
                 ptr = ((char*)(&CreateImageSurface) - (a + 5)); \
                 GM_WRITE((a + 1), &ptr, 4)
    PATCH(0x4a13d0); // CreateImageSurface (sub_4A12F8, call@0x4a13d2)
    PATCH(0x4a2922); // CreateImageSurface (sub_4A286C, call@0x4a2924)
#undef PATCH

    // GetBackBuffer (0x40, sz3)
#define PATCH(a) \
                 offset = 0xe8; \
                 GM_WRITE((a), &offset, 1); \
                 ptr = ((char*)(&GetBackBuffer) - (a + 5)); \
                 GM_WRITE((a + 1), &ptr, 4)
    PATCH(0x4a294e); // GetBackBuffer (sub_4A286C, call@0x4a2950)
#undef PATCH

    // ✅ [GM80-确认] screen_refresh 整段重定向：8.0 的帧管线分散在
    // INNER_screen_redraw(BeginScene)/D3D_CreateDevice(EndScene+Present)/INNER_screen_refresh(Present)，
    // 已用槽位补丁覆盖（见下），无需 screen_refresh 包装。

    // GetRenderTarget (0x80, sz6) —— INNER_surface_set_target
#define PATCH(a) \
                 offset = 0xe890; \
                 GM_WRITE((a), &offset, 2); \
                 ptr = ((char*)(&GetRenderTarget) - (a + 6)); \
                 GM_WRITE((a + 2), &ptr, 4)
    PATCH(0x4a0eb7); // GetRenderTarget (INNER_surface_set_target)
#undef PATCH

    // ✅ [GM80-确认] SetTexture(0, NULL)→SetNullTexture：8.0 的 SetTexture 站点已统一补到 0x104；
    // 是否区分"置 NULL"站点需要逐个确认参数，暂不启用 SetNullTexture 包装。

    // ✅ [GM80-确认] white pixel 初始化：8.0 无 D3DXCreateTextureFromFileInMemoryEx 调用点（纹理走
    // CreateTexture 直接创建），8.1 的 0x627e5a 重定向段在 8.0 不适用，可整体删除。
    // // initialize white pixel texture
    // ptr = ((char*)(&create_white_pixel) - (0x627e5a + 5));
    // WriteProcessMemory(proc, (void*)(0x627e5a + 1), &ptr, 4, nullptr);

#define PATCH_SIMPLE(a, off) \
        offset = off;        \
        GM_WRITE((a + 2), &offset, 1)
#define PATCH_DOUBLE(a, off) \
        offset = off;        \
        GM_WRITE((a + 2), &offset, 2)
    // =====================================================================
    // [GM80] D3D8→D3D9 vtable 槽位重映射（2026-08-05）
    // 来源：py_eval 全模块扫描 + 逐点字节核对；D3D8 槽位与 8.1 一致，补丁值沿用 8.1 插件。
    // 编码：sz6 = FF 90 disp32（写 1/2 字节于 +2）；sz3 = FF 5? disp8（写 1 字节于 +2）。
    // =====================================================================
    // Reset (0x38→0x40, sz3)
    PATCH_SIMPLE(0x4a22ce, 0x40); // Reset (INNER_display_set_size)
    PATCH_SIMPLE(0x4a22f6, 0x40); // Reset (INNER_display_set_size)

    // Clear (0x90→0xAC, sz6)
    PATCH_SIMPLE(0x4a1f62, 0xac); // Clear (sub_4A1DA0)
    PATCH_SIMPLE(0x4a232d, 0xac); // Clear (INNER_display_set_size)
    PATCH_SIMPLE(0x49cb0b, 0xac); // Clear (INNER_draw_clear)
    PATCH_SIMPLE(0x49cb40, 0xac); // Clear (INNER_draw_clear_alpha)
    PATCH_SIMPLE(0x49e76a, 0xac); // Clear (sub_49E748 = clear_depth)

    // SetViewport (0xA0→0xBC, sz6)
    PATCH_SIMPLE(0x4a2432, 0xbc); // SetViewport (D3D_SetViewport)

    // ✅ [GM80-确认] SetMaterial：8.0 扫描未发现 0xA8 站点（8.1 有 0x56475e→0xc4），无需补丁。

    // SetLight (0xB0→0xCC, sz6) / LightEnable (0xB8→0xD4, sz6)
    PATCH_SIMPLE(0x49f3eb, 0xcc); // SetLight
    PATCH_SIMPLE(0x49f4dd, 0xcc); // SetLight
    PATCH_SIMPLE(0x49f50d, 0xd4); // LightEnable

    // SetTransform (0x94→0xB0, sz6)
    PATCH_SIMPLE(0x4a2555, 0xb0); // SetTransform (sub_4A2440)
    PATCH_SIMPLE(0x4a259a, 0xb0); // SetTransform (sub_4A2440)
    PATCH_SIMPLE(0x4a2685, 0xb0); // SetTransform (sub_4A2440)
    PATCH_SIMPLE(0x4a26c6, 0xb0); // SetTransform (sub_4A2440)
    PATCH_SIMPLE(0x49e8af, 0xb0); // SetTransform (INNER_d3d_set_projection)
    PATCH_SIMPLE(0x49e96b, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49e9c2, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49ead6, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49eb19, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49ec2b, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49ec72, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49ecae, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49ecfc, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49ed4c, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49ed98, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49edf4, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49ee50, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49eed6, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49ef66, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49efe6, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49f062, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49f0ee, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49f17a, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49f239, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49f2dd, 0xb0); // SetTransform
    PATCH_SIMPLE(0x49f319, 0xb0); // SetTransform

    // GetTransform (0x98→0xB4, sz6)
    PATCH_SIMPLE(0x49ef38, 0xb4); // GetTransform
    PATCH_SIMPLE(0x49efb8, 0xb4); // GetTransform
    PATCH_SIMPLE(0x49f034, 0xb4); // GetTransform
    PATCH_SIMPLE(0x49f0c0, 0xb4); // GetTransform
    PATCH_SIMPLE(0x49f14c, 0xb4); // GetTransform
    PATCH_SIMPLE(0x49f20b, 0xb4); // GetTransform
    PATCH_SIMPLE(0x49f2a6, 0xb4); // GetTransform

    // SetRenderState (0xC8→0xE4, sz6)
    PATCH_SIMPLE(0x4a18a5, 0xe4); // SetRenderState (INNER_d3d_set_hidden)
    PATCH_SIMPLE(0x4a18b7, 0xe4); // SetRenderState (INNER_d3d_set_hidden)
    PATCH_SIMPLE(0x4a18c9, 0xe4); // SetRenderState (INNER_d3d_set_hidden)
    PATCH_SIMPLE(0x4a18db, 0xe4); // SetRenderState (INNER_d3d_set_hidden)
    PATCH_SIMPLE(0x4a18ef, 0xe4); // SetRenderState (INNER_d3d_set_hidden)
    PATCH_SIMPLE(0x4a1901, 0xe4); // SetRenderState (INNER_d3d_set_hidden)
    PATCH_SIMPLE(0x4a1925, 0xe4); // SetRenderState (INNER_d3d_set_lighting)
    PATCH_SIMPLE(0x4a193b, 0xe4); // SetRenderState (INNER_d3d_set_lighting)
    PATCH_SIMPLE(0x4a195e, 0xe4); // SetRenderState (INNER_d3d_set_shading)
    PATCH_SIMPLE(0x4a1971, 0xe4); // SetRenderState (INNER_d3d_set_shading)
    PATCH_SIMPLE(0x4a19b2, 0xe4); // SetRenderState (INNER_d3d_set_fog)
    PATCH_SIMPLE(0x4a19cc, 0xe4); // SetRenderState (INNER_d3d_set_fog)
    PATCH_SIMPLE(0x4a19ed, 0xe4); // SetRenderState (INNER_d3d_set_fog)
    PATCH_SIMPLE(0x4a1a0b, 0xe4); // SetRenderState (INNER_d3d_set_fog)
    PATCH_SIMPLE(0x4a1a40, 0xe4); // SetRenderState (INNER_d3d_set_fog)
    PATCH_SIMPLE(0x4a1a58, 0xe4); // SetRenderState (INNER_d3d_set_fog)
    PATCH_SIMPLE(0x4a1a70, 0xe4); // SetRenderState (INNER_d3d_set_fog)
    PATCH_SIMPLE(0x4a1a84, 0xe4); // SetRenderState (INNER_d3d_set_fog)
    PATCH_SIMPLE(0x4a1af7, 0xe4); // SetRenderState (INNER_draw_set_blend_mode_ext)
    PATCH_SIMPLE(0x4a1b0d, 0xe4); // SetRenderState (INNER_draw_set_blend_mode_ext)
    PATCH_SIMPLE(0x4a1b37, 0xe4); // SetRenderState (INNER_d3d_set_culling)
    PATCH_SIMPLE(0x4a1b4b, 0xe4); // SetRenderState (INNER_d3d_set_culling)
    PATCH_SIMPLE(0x4a1d15, 0xe4); // SetRenderState (sub_4A1CFC 恢复渲染状态)

    // SetSamplerState (0xFC→0x114, sz6)。8.0 的 0xFC 站点分两类：sampler（interp/repeat/draw 的
    // ADDRESSU/ADDRESSV/MAGFILTER/MINFILTER）与 stage（blending 的 COLOROP 等）。sampler 站点
    // 需同时把 state 常量从 D3DTSS_* 改成 D3DSAMP_*（数值不同：ADDRESSU 13→1 等）。
    PATCH_DOUBLE(0x4a1b79, 0x114); // SetSamplerState (interp MAGFILTER)
    PATCH_DOUBLE(0x4a1b8d, 0x114); // SetSamplerState (interp MINFILTER)
    PATCH_DOUBLE(0x4a1ba3, 0x114); // SetSamplerState (interp MAGFILTER)
    PATCH_DOUBLE(0x4a1bb7, 0x114); // SetSamplerState (interp MINFILTER)
    PATCH_DOUBLE(0x4a1ca5, 0x114); // SetSamplerState (repeat ADDRESSU)
    PATCH_DOUBLE(0x4a1cb9, 0x114); // SetSamplerState (repeat ADDRESSV)
    PATCH_DOUBLE(0x4a1ccf, 0x114); // SetSamplerState (repeat ADDRESSU)
    PATCH_DOUBLE(0x4a1ce3, 0x114); // SetSamplerState (repeat ADDRESSV)
    PATCH_DOUBLE(0x4a36e9, 0x114); // SetSamplerState (DrawImage ADDRESSU)
    PATCH_DOUBLE(0x4a36ff, 0x114); // SetSamplerState (DrawImage ADDRESSV)
    PATCH_DOUBLE(0x4a3849, 0x114); // SetSamplerState (DrawImage2 ADDRESSU)
    PATCH_DOUBLE(0x4a385f, 0x114); // SetSamplerState (DrawImage2 ADDRESSV)
    PATCH_DOUBLE(0x4a39b2, 0x114); // SetSamplerState (DrawTexture ADDRESSU)
    PATCH_DOUBLE(0x4a39c8, 0x114); // SetSamplerState (DrawTexture ADDRESSV)
    PATCH_DOUBLE(0x4a46a4, 0x114); // SetSamplerState (ADDRESSU)
    PATCH_DOUBLE(0x4a46ba, 0x114); // SetSamplerState (ADDRESSV)

    // [GM80] sampler state 常量：D3DTSS_ADDRESSU(0x0D)→D3DSAMP_ADDRESSU(1)、ADDRESSV(0x0E)→2、
    // MAGFILTER(0x10)→5、MINFILTER(0x11)→6（写 push imm8 的 +1 字节）
    offset = D3DSAMP_MAGFILTER;
    WriteProcessMemory(proc, (void *) (0x4a1b6d + 1), &offset, 1, nullptr);
    WriteProcessMemory(proc, (void *) (0x4a1b97 + 1), &offset, 1, nullptr);
    offset = D3DSAMP_MINFILTER;
    WriteProcessMemory(proc, (void *) (0x4a1b81 + 1), &offset, 1, nullptr);
    WriteProcessMemory(proc, (void *) (0x4a1bab + 1), &offset, 1, nullptr);
    offset = D3DSAMP_ADDRESSU;
    WriteProcessMemory(proc, (void *) (0x4a1c99 + 1), &offset, 1, nullptr);
    WriteProcessMemory(proc, (void *) (0x4a1cc3 + 1), &offset, 1, nullptr);
    WriteProcessMemory(proc, (void *) (0x4a36db + 1), &offset, 1, nullptr);
    WriteProcessMemory(proc, (void *) (0x4a383b + 1), &offset, 1, nullptr);
    WriteProcessMemory(proc, (void *) (0x4a39a4 + 1), &offset, 1, nullptr);
    WriteProcessMemory(proc, (void *) (0x4a4696 + 1), &offset, 1, nullptr);
    offset = D3DSAMP_ADDRESSV;
    WriteProcessMemory(proc, (void *) (0x4a1cad + 1), &offset, 1, nullptr);
    WriteProcessMemory(proc, (void *) (0x4a1cd7 + 1), &offset, 1, nullptr);
    WriteProcessMemory(proc, (void *) (0x4a36f1 + 1), &offset, 1, nullptr);
    WriteProcessMemory(proc, (void *) (0x4a3851 + 1), &offset, 1, nullptr);
    WriteProcessMemory(proc, (void *) (0x4a39ba + 1), &offset, 1, nullptr);
    WriteProcessMemory(proc, (void *) (0x4a46ac + 1), &offset, 1, nullptr);

    // SetTextureStageState (0xFC→0x10C, sz6) — blending 的 COLOROP/COLORARG(1-6)，state 不变
    PATCH_DOUBLE(0x4a1be9, 0x10c); // SetTextureStageState
    PATCH_DOUBLE(0x4a1bfd, 0x10c); // SetTextureStageState
    PATCH_DOUBLE(0x4a1c11, 0x10c); // SetTextureStageState
    PATCH_DOUBLE(0x4a1c25, 0x10c); // SetTextureStageState
    PATCH_DOUBLE(0x4a1c39, 0x10c); // SetTextureStageState
    PATCH_DOUBLE(0x4a1c4d, 0x10c); // SetTextureStageState
    PATCH_DOUBLE(0x4a1c63, 0x10c); // SetTextureStageState
    PATCH_DOUBLE(0x4a1c77, 0x10c); // SetTextureStageState

    // BeginScene (0x88→0xA4, sz6) / EndScene (0x8C→0xA8, sz6) / Present (0x3C→0x44, sz3)
    PATCH_SIMPLE(0x4a26fc, 0xa4); // BeginScene (INNER_screen_redraw)
    PATCH_SIMPLE(0x4a2723, 0xa8); // EndScene (D3D_CreateDevice)
    PATCH_SIMPLE(0x4a27ab, 0x44); // Present (D3D_CreateDevice)
    PATCH_SIMPLE(0x4a2861, 0x44); // Present (INNER_screen_refresh)

    // ✅ [GM80-确认] 8.1 的 screen_refresh 整段重定向(0x6200c2)在 8.0 不需要：8.0 的帧管线分散在
    // INNER_screen_redraw(BeginScene)/D3D_CreateDevice(EndScene+Present)/INNER_screen_refresh(Present)，
    // 上面已用槽位补丁覆盖。若实机出现帧同步问题再考虑加 screen_refresh 包装。

    // SetTexture (0xF4→0x104, sz6)。注：8.1 把置 NULL 的站点重定向到 SetNullTexture(白纹理)，
    // 8.0 侧尚未区分 NULL/非 NULL 站点，暂统一补到 0x104（SetTexture 真实纹理）。
    // [GM80-TODO] 若需还原 8.1 的 SetNullTexture 行为，需逐个确认 8.0 SetTexture 参数是否为 0。
    PATCH_DOUBLE(0x49cb89, 0x104); // SetTexture (INNER_draw_point)
    PATCH_DOUBLE(0x49cc0e, 0x104); // SetTexture (INNER_draw_point_color)
    PATCH_DOUBLE(0x49cca0, 0x104); // SetTexture (INNER_draw_line)
    PATCH_DOUBLE(0x49cdf5, 0x104); // SetTexture (INNER_draw_line_width)
    PATCH_DOUBLE(0x49ceae, 0x104); // SetTexture (INNER_draw_line_color)
    PATCH_DOUBLE(0x49d043, 0x104); // SetTexture (INNER_draw_line_width_color)
    PATCH_DOUBLE(0x49d101, 0x104); // SetTexture (INNER_draw_triangle)
    PATCH_DOUBLE(0x49d20e, 0x104); // SetTexture (INNER_draw_triangle_color)
    PATCH_DOUBLE(0x49d3c7, 0x104); // SetTexture (INNER_draw_rectangle)
    PATCH_DOUBLE(0x49d5ca, 0x104); // SetTexture (INNER_draw_rectangle_color)
    PATCH_DOUBLE(0x49d7c2, 0x104); // SetTexture (sub_49D6EC)
    PATCH_DOUBLE(0x49dc8c, 0x104); // SetTexture (sub_49D99C)
    PATCH_DOUBLE(0x4a359b, 0x104); // SetTexture
    PATCH_DOUBLE(0x4a35b1, 0x104); // SetTexture
    PATCH_DOUBLE(0x4a3715, 0x104); // SetTexture (DrawImage)
    PATCH_DOUBLE(0x4a3875, 0x104); // SetTexture (DrawImage2)
    PATCH_DOUBLE(0x4a39de, 0x104); // SetTexture (DrawTexture)
    PATCH_DOUBLE(0x4a46d9, 0x104); // SetTexture

    // ✅ [GM80-确认] DrawPrimitive(0x118→0x144)：8.0 全模块扫描未发现 0x118 站点（8.1 有 0x568b87 等 3 处），
    // 8.0 只用 DrawPrimitiveUP(0x120)，无需补 DrawPrimitive。

    // DrawPrimitiveUP (0x120→0x14C, sz6)
    PATCH_DOUBLE(0x49cbb6, 0x14c); // DrawPrimitiveUP (INNER_draw_point)
    PATCH_DOUBLE(0x49cc3b, 0x14c); // DrawPrimitiveUP (INNER_draw_point_color)
    PATCH_DOUBLE(0x49ccc9, 0x14c); // DrawPrimitiveUP (INNER_draw_line)
    PATCH_DOUBLE(0x49ce1e, 0x14c); // DrawPrimitiveUP (INNER_draw_line_width)
    PATCH_DOUBLE(0x49ced7, 0x14c); // DrawPrimitiveUP (INNER_draw_line_color)
    PATCH_DOUBLE(0x49d06c, 0x14c); // DrawPrimitiveUP (INNER_draw_line_width_color)
    PATCH_DOUBLE(0x49d137, 0x14c); // DrawPrimitiveUP (INNER_draw_triangle)
    PATCH_DOUBLE(0x49d150, 0x14c); // DrawPrimitiveUP (INNER_draw_triangle)
    PATCH_DOUBLE(0x49d246, 0x14c); // DrawPrimitiveUP (INNER_draw_triangle_color)
    PATCH_DOUBLE(0x49d25f, 0x14c); // DrawPrimitiveUP (INNER_draw_triangle_color)
    PATCH_DOUBLE(0x49d3fd, 0x14c); // DrawPrimitiveUP (INNER_draw_rectangle)
    PATCH_DOUBLE(0x49d416, 0x14c); // DrawPrimitiveUP (INNER_draw_rectangle)
    PATCH_DOUBLE(0x49d602, 0x14c); // DrawPrimitiveUP (INNER_draw_rectangle_color)
    PATCH_DOUBLE(0x49d61b, 0x14c); // DrawPrimitiveUP (INNER_draw_rectangle_color)
    PATCH_DOUBLE(0x49d810, 0x14c); // DrawPrimitiveUP (sub_49D6EC)
    PATCH_DOUBLE(0x49d831, 0x14c); // DrawPrimitiveUP (sub_49D6EC)
    PATCH_DOUBLE(0x49dcdd, 0x14c); // DrawPrimitiveUP (sub_49D99C)
    PATCH_DOUBLE(0x49dd01, 0x14c); // DrawPrimitiveUP (sub_49D99C)
    PATCH_DOUBLE(0x49e1bf, 0x14c); // DrawPrimitiveUP (INNER_draw_primitive_end)
    PATCH_DOUBLE(0x49fb97, 0x14c); // DrawPrimitiveUP
    PATCH_DOUBLE(0x4a3741, 0x14c); // DrawPrimitiveUP (DrawImage)
    PATCH_DOUBLE(0x4a38a1, 0x14c); // DrawPrimitiveUP (DrawImage2)
    PATCH_DOUBLE(0x4a3a0a, 0x14c); // DrawPrimitiveUP (DrawTexture)
    PATCH_DOUBLE(0x4a4919, 0x14c); // DrawPrimitiveUP

    // SetVertexShader (0x130→包装，e890 6字节重定向，sz6 原指令等长替换)
#define PATCH(a) \
                 offset = 0xe890; \
                 GM_WRITE((a), &offset, 2); \
                 ptr = ((char*)(&SetVertexShader) - (a + 6)); \
                 GM_WRITE((a + 2), &ptr, 4)
    PATCH(0x49cb9b); // SetVertexShader (INNER_draw_point)
    PATCH(0x49cc20); // SetVertexShader (INNER_draw_point_color)
    PATCH(0x49ccb2); // SetVertexShader (INNER_draw_line)
    PATCH(0x49ce07); // SetVertexShader (INNER_draw_line_width)
    PATCH(0x49cec0); // SetVertexShader (INNER_draw_line_color)
    PATCH(0x49d055); // SetVertexShader (INNER_draw_line_width_color)
    PATCH(0x49d113); // SetVertexShader (INNER_draw_triangle)
    PATCH(0x49d220); // SetVertexShader (INNER_draw_triangle_color)
    PATCH(0x49d3d9); // SetVertexShader (INNER_draw_rectangle)
    PATCH(0x49d5dc); // SetVertexShader (INNER_draw_rectangle_color)
    PATCH(0x49d7d4); // SetVertexShader (sub_49D6EC)
    PATCH(0x49dc9e); // SetVertexShader (sub_49D99C)
    PATCH(0x49e1a6); // SetVertexShader (INNER_draw_primitive_end)
    PATCH(0x49fb7e); // SetVertexShader
    PATCH(0x4a372a); // SetVertexShader (DrawImage)
    PATCH(0x4a388a); // SetVertexShader (DrawImage2)
    PATCH(0x4a39f3); // SetVertexShader (DrawTexture)
    PATCH(0x4a46ee); // SetVertexShader
#undef PATCH

    // GetSurfaceLevel（纹理接口 0x3C→0x48, sz3）
    PATCH_SIMPLE(0x4a316f, 0x48); // GetSurfaceLevel (sub_4A3124)
    PATCH_SIMPLE(0x4a35ed, 0x48); // GetSurfaceLevel (sub_4A35BC)

    // ✅ [GM80-确认] GetDepthStencilSurface(0x84→0xa0)：8.0 全模块扫描未发现 0x84 站点（8.1 有 0x56b741）。
    // 8.0 深度缓冲经 off_58FC14 间接指针创建，无独立 GetDepthStencilSurface 调用，暂无需补丁。

    // LockRect (0x24→0x34, sz3) / UnlockRect (0x28→0x38, sz3)
    PATCH_SIMPLE(0x4a1466, 0x34); // LockRect (sub_4A12F8 surface copy)
    PATCH_SIMPLE(0x4a2a40, 0x34); // LockRect (sub_4A286C 屏幕捕获)
    PATCH_SIMPLE(0x4a3545, 0x34); // LockRect
    PATCH_SIMPLE(0x4a14d1, 0x38); // UnlockRect (sub_4A12F8)
    PATCH_SIMPLE(0x4a2a95, 0x38); // UnlockRect (sub_4A286C 屏幕捕获)

    // [GM80] D3DX 接管已实现(2026-08-06)：8.1 是替换 0x68fde8–0x68fdf4 函数表；8.0 是动态加载
    // （sub_49A254 GetProcAddress 填充 0x593868–0x59389c 连续块），已在 DllMain 兜底把 14 个全局
    // 写成 D3DX9 导出指针（顺序与 sub_49A254 一致）。以下 8.1 地址段注释保留作移植记录。
    // // overwrite d3dx
    // *(void **) (0x68fde8) = &D3DXGetErrorStringA;
    // *(void **) (0x68fdec) = &D3DXCheckTextureRequirements;
    // *(void **) (0x68fdf0) = &D3DXCreateTexture;
    // *(void **) (0x68fdf4) = &D3DXCreateTextureFromFileInMemoryEx;
    // #define PATCH_D3DX(a, f) \
    //         offset = 0xb890; \
    //         WriteProcessMemory(proc, (void*)(a), &offset, 2, nullptr); \
    //         ptr = &f;        \
    //         WriteProcessMemory(proc, (void*)(a+2), &ptr, 4, nullptr);
    // PATCH_D3DX(0x53122f, D3DXCheckTextureRequirements);
    // PATCH_D3DX(0x531241, D3DXCreateTexture);
    // PATCH_D3DX(0x531253, D3DXCreateTextureFromFileInMemoryEx);
    // PATCH_D3DX(0x531277, D3DXGetErrorStringA);

#define PATCH(addr, func) \
        ptr = (char*)(&func) - (addr + 5); \
        WriteProcessMemory(proc, (void*)(addr + 1), &ptr, 4, nullptr);

    // [GM80] regain_device / last_resort 已实现(2026-08-06)：
    //   - 设备丢失恢复 = ResetDevice 按 TestCooperativeLevel==D3DERR_DEVICENOTRESET 真 Reset
    //     (8.0 路径: D3D_CreateDevice@0x4a2708 Present 失败 → INNER_display_set_size → Reset)
    //   - 卸载安全 = gm80_restore_reset_hook() 在 DllMain(DLL_PROCESS_DETACH) 恢复 vtable 槽 0x40
    // 8.1 原版裸汇编挂钩(0x620012/0x5795c5)在 8.0 不安装, 注释保留作记录。

    // [GM80] 数学 FPU trampoline 已判定不需要(2026-08-06)：8.0 数学函数是 Delphi RTL，CreateDevice 带
    // D3DCREATE_FPU_PRESERVE(0x22) 生效；测试工程 FPU 自检 precision=1。8.1 地址 0x633d70–0x6344b6 不适用。
    // maths
    // PATCH(0x633d70, sqrt_inj)
    // PATCH(0x633e3c, exp_inj)
    // PATCH(0x633ed4, ln_inj)
    // PATCH(0x633f68, log2_inj)
    // PATCH(0x634000, log10_inj)
    // PATCH(0x634110, arcsin_inj)
    // PATCH(0x6341a8, arccos_inj)
    // PATCH(0x634244, arctan_inj)
    // PATCH(0x6342ea, arctan2_inj)
    // PATCH(0x63440e, power_inj)
    // PATCH(0x6344b6, logn_inj)
	
	// ✅ [GM80-确认] 投影矩阵 D3DX 注入不需要(2026-08-06)：该注入是给 D3DXMatrixPerspectiveLH/OrthoLH 包
	// fldcw 恢复 FPU 控制字。8.0 FPU 已实测不受 D3D9 影响(precision=1)，无需注入。
	// 8.1 原地址 0x68fde0/0x68fde4 不适用，注释保留作移植记录。

    FlushInstructionCache(proc, nullptr, 0);

#if GM80_LOG
    // ===================================================================
    // [GM80] 补丁读回验证: 抽查代表性站点, 确认内存真的被改了。
    // 任一 MISMATCH => 该地址没写进去(越界/写保护/地址错), 需立即排查。
    // ===================================================================
    gm_log("--- DllMain patch verification ---");
    int gv = 0;
    gv += gm_readback("SDK_VERSION push 32",       (void*)0x4a1e13, 0x00002068, 4);             // 68 20 00 00
    gv += gm_readback("D3DCreate9 rel32",          (void*)0x484e00, (unsigned)((char*)&Direct3DCreate9 - (char*)0x484e04), 4);
    gv += gm_readback("D3DCAPS @4a1f3e+1",         (void*)0x4a1f3f, (unsigned)(size_t)&d3d_caps, 4);
    gv += gm_readback("D3DCAPS @4a2309+1",         (void*)0x4a230a, (unsigned)(size_t)&d3d_caps, 4);
    gv += gm_readback("CreateDevice call",         (void*)0x4a1f1d, 0xE8, 1);                   // E8
    gv += gm_readback("CreateDevice rel32",        (void*)0x4a1f1e, (unsigned)((char*)&CreateDevice - (char*)0x4a1f22), 4);
    gv += gm_readback("D3DX dll-name ptr",         (void*)0x49a27c, (unsigned)(size_t)d3dx9_dll_name, 4);
    gv += gm_readback("Reset slot @4a22ce+2",      (void*)0x4a22d0, 0x40, 1);
    gv += gm_readback("Clear slot @49cb0b+2",      (void*)0x49cb0d, 0xac, 1);
    gv += gm_readback("SetTexture slot @49cb89+2", (void*)0x49cb8b, 0x0104, 2);
    gv += gm_readback("DrawPrimUP slot @49cbb6+2", (void*)0x49cbb8, 0x014c, 2);
    gv += gm_readback("BeginScene slot @4a26fc+2", (void*)0x4a26fe, 0xa4, 1);
    gv += gm_readback("Present slot @4a27ab+2",    (void*)0x4a27ad, 0x44, 1);
    gv += gm_readback("GetRT jmp @4a0eb7",         (void*)0x4a0eb7, 0xE890, 2);
    gv += gm_readback("SetVS call @49cb9b",        (void*)0x49cb9b, 0xE890, 2);
    gv += gm_readback("SetVS rel32 @49cb9d",       (void*)0x49cb9d, (unsigned)((char*)&SetVertexShader - (char*)0x49cba1), 4);
    g_patch_failures += gv;
    gm_log("--- verification: %s (%u MISMATCH), WriteProcessMemory failures total=%d ---",
           gv ? "FAIL" : "PASS", gv, g_patch_failures);
    gm_log("=== GM82DX9 gm80_apply_patches done ===\n");
#endif

    return true;
}
