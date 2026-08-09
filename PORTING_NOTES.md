# GMDirectX9 —— gm82dx9 移植到 GameMaker 8.0

> 由 `C:\Users\Lequ\Desktop\gm82dx9-main` 复制而来（2026-08-05），目标：把面向 GM 8.1/8.2 的 DX8→DX9 渲染后端升级插件移植到 GM 8.0（Delphi runner）。
> 权威分析见 `C:\Users\Lequ\Desktop\gm82dx9 移植到 GM8.0 可行性报告.md`。
> IDA 实例：`827d` = 空工程.exe（GM8.0）；`utmm` = 空工程 8.2.exe（GM8.1，插件原目标）。

---

## 0. 当前状态：WIP —— vtable 重映射已完成，设备初始化接管待做

**进度（2026-08-05）**：
- ✅ D3D8→D3D9 vtable 重映射（~150 点）已完成并写入 `inject.cpp`，经修复后的 find_bytes 全二进制交叉验证（见 §1）。
- ✅ 设备创建链：Direct3DCreate8→9、SDK 版本、CreateDevice 包装、room 读取。
- ✅ D3DCAPS 接管（caps 缓冲改指 d3d_caps，防 D3DCAPS9 溢出）、D3DX 接管（DLL 名改指 D3DX9_43）。
- ✅ **2026-08-05 实机崩溃修复（调试器定位）**：① SDK 版本补丁原用 2 字节 `6A 20` 覆盖 5 字节 `push 0DCh`，留下 3 字节 `00 00 00` 被解码为 `add [eax],al`，在 eax=0x20（色深 32 检查）时写地址 0x20 → 访问违例（崩溃点 runner+0xA1E15，早于 CreateDevice）。已改为 5 字节 `68 20 00 00 00`。② 确认 D3DX 时序问题：`sub_49A254` 在 `sub_545620`（先于扩展加载）运行，DLL 名字符串补丁来不及生效，加载原始 `\D3DX8.dll`（Win11 无此文件 → 14 个全局 NULL）。已加"兜底重写 14 个全局（0x593868–0x59389c）为 D3DX9 导出指针"。③ **第二个崩溃**：sub_4A1DA0 里**第一个 CreateDevice 调用（0x4a1ee7，槽 0x3C）没补丁**。它在 D3D9 对象上调用槽 0x3C（实为 GetAdapterMonitor，非 CreateDevice）→ 返回假成功、不创建设备（0x58d388 保持 NULL）、__stdcall 栈错位 → 后续 0x4a1f49（GetDeviceCaps 区）读 0 崩溃。这也解释了包装函数从未打日志（第一个调用先返回成功，重试调用/包装函数不执行）。已把 0x4a1ee5 也重定向到包装函数。
- ⏳ **剩余**：D3DPRESENT_PARAMETERS 格式来源接管、数学 trampoline 判定。待实机验证上述修复后继续。
- ✅ **日志设施已加入**（2026-08-05）：`gm_log`（见 §6）。无需调试器即可验证补丁生效与关键路径执行。
- ✅ **编译/链接已验证**（2026-08-05）：MSVC 2022 x86，`MSBuild GMDirectX9.sln /p:Configuration=Release /p:Platform=x86` → `Release/GMDirectX9.dll`（54万字节，导出接口完整，x86 PE32）。
- ✅ **项目结构已改为 GMSave 式原生 VS 工程**（2026-08-05）：sln/vcxproj/filters 在根目录，`dllmain.cpp`/`pch.h`/`framework.h` 在根目录，源码在 `source/`（含拆出的 `gm_log.h/gm_log.cpp`），第三方在 `Librarys/`（DX 头/库/DirectXMath/dxerr/D3DX9_43.dll），输出 `Debug/Release/`，中间件 `obj/`。CMake 已移除。
- ✅ **fxc 用 Windows SDK 自带**（10.0.26100.0/x86，无需 DXSDK June 2010），由 vcxproj PreBuildEvent 定位，shader 编译进 `source/vs_pass.vs3`、`ps_pass.ps3`。
- ✅ 顺手修了编码/构建问题（详见 §6.1）：源码统一 UTF-8+BOM + `/utf-8`、去掉 `/WX`、DLL 名 = `GMDirectX9.dll`。
- 📁 下文中出现的 `inject.cpp`/`gm82dx9.cpp` 等均指 `source/` 子目录下的文件（2026-08-05 重构后）。

源码中所有"尚未替换/待验证"处标注了 `GM80-TODO` 注释。

---

## 1. 已替换为 8.0 的部分（✅ 已确认，直接改）

| 文件 | 改动 | 证据 |
|---|---|---|
| `gm_interface.cpp` `d3d9_device` | `0x6886a8` → **`0x58d388`** | GMAPI `GMDIRECT3DINFO.direct3dDevice` |
| `gm_interface.cpp` `gm_surfaces` | `0x84527c` → **`0x6c7240`** | GMAPI `GM80_ADDRESS_ARRAY_SURFACES` + `INNER_surface_create` 反编译 |
| `gm_interface.cpp` `gm_textures` | `0x85b3c4` → **`0x6c7320`** | GMAPI `GM80_ADDRESS_ARRAY_TEXTURES` + `sub_4A2FB4` 反编译 |
| `gm82dx9.cpp` `runner_display_reset` | `0x61f9f4` → **`0x4a2228`**（= `INNER_display_set_size`） | 直接反汇编（被 `display_reset` 调用） |
| `gm82dx9.cpp` `runner_clear_depth` | `0x563a8c` → **`0x49e748`** | 跨版本相似度 1.0 + 语义逐句核对 |
| `gm82dx9.cpp` `runner_surface_count` | `0x6869a4` → **`0x58d378`** | GMAPI `GM80_ADDRESS_ARRAYSIZE_SURFACES` |
| `gm82dx9.cpp` resize 内联 asm | `0x61fbc0` → **`0x4a236c`** | 跨版本相似度 1.0，eax=宽/edx=高 一致 |
| `shaders.cpp` 纹理数组 | `0x85b3c4` → **`0x6c7320`** | 同上 |
| `gm82dx9.h` `GMSurface` 结构体 | **删除 `zbuffer` 字段**（8.1 20B/项 → 8.0 16B/项） | GMAPI `GMSURFACE` + `INNER_surface_create`（16B: textureId,width,height,exists） |
| `inject.cpp` CreateDevice 包装 room 读取 | rooms 数组 `0x686a4c`→**`0x58d4cc`**；首个房间 id 取 0 | `INNER_room_set_persistent` 反编译；room 结构体偏移（[0x40]/[3]/[4]/+0x44）两版本一致 |
| `inject.cpp` **D3D8→D3D9 vtable 重映射（~150 点）** | 全部 8.1 补丁点换为 8.0 地址（Reset/Clear/SetTransform/GetTransform/SetRenderState/SetSamplerState/SetTextureStageState/BeginScene/EndScene/Present/SetTexture/DrawPrimitiveUP/SetVertexShader/GetSurfaceLevel/LockRect/UnlockRect + 屏幕捕获/表面包装） | py_eval 全模块扫描 + **修复后的 find_bytes 全二进制交叉验证**（各槽位计数吻合；D3D 模块外同槽位命中均确认为非 D3D 接口） |
| `inject.cpp` 设备创建 | Direct3DCreate8→9（call@`0x484dff`）+ SDK 版本（`0x4a1e13` push 32）+ CreateDevice 包装（call@`0x4a1f1f`） | 反汇编验证 |

---

## 2. 已加 GM80-TODO、待下一轮验证的部分

### 2.1 inject.cpp —— vtable 重映射 + 设备初始化接管（2026-08-05）
- ✅ **vtable 调用点重映射已完成**：Reset/Clear/SetTransform/GetTransform/SetRenderState/SetSamplerState/SetTextureStageState/BeginScene/EndScene/Present/SetTexture/DrawPrimitiveUP/SetVertexShader/GetSurfaceLevel/LockRect/UnlockRect + 屏幕捕获/表面包装（GetDisplayMode/CreateImageSurface/GetBackBuffer/CopyRects/SetRenderTarget/GetRenderTarget）。
  - 验证：修复后的 find_bytes 全二进制扫描各 D3D8 槽位，计数与补丁表逐一吻合；D3D 模块外的同槽位命中经核实均为**非 D3D 接口**（自定义 Delphi 接口 sub_407654 QI / VCL / 声音接口等），不补丁。
- ✅ **D3DCAPS 接管**：8.0 调 device `GetDeviceCaps`（槽 0x1C，两版本相同无需改槽）；`push offset unk_6C7244` @ `0x4a1f3e` / `0x4a2309` 改指 `&d3d_caps`（D3DCAPS9 大小，防溢出）。
- ✅ **D3DX 接管**：把 `sub_49A254` 加载器的 DLL 名字符串（`mov edx, offset "\D3DX8.dll"` @ `0x49a27b`）改指插件字符串 `"\D3DX9_43.dll"`。**但 2026-08-05 实机确认时序是反的**：`sub_49A254` 在 `sub_545620`（先于扩展加载 sub_51D040）运行，字符串补丁来不及生效，原始 `\D3DX8.dll` 加载失败（Win11 无此文件）→ 14 个全局 NULL。**已加兜底**：DllMain 内直接把 14 个全局（0x593868–0x59389c）重写为 D3DX9_43.dll 的 GetProcAddress 指针（顺序与 sub_49A254 一致）。
- ⏳ **仍待下一轮**：
- **D3DPRESENT_PARAMETERS 接管**：8.0 的 present-params 在 `sub_4A1DA0`（0x4a1ea2 等）与 `INNER_display_set_size`（0x4a228f 等）栈上构造；方案 = 把格式来源改读 `d3d_parameters.BackBufferFormat`。当前 set_alpha_buffer/set_fullscreen 只改插件 d3d_parameters（安全但 runner 不读），resize_backbuffer 已不再写 present_params（指向死栈）。
- **数学 FPU trampoline**：8.0 可能不需要（CreateDevice 带 `D3DCREATE_FPU_PRESERVE`），实测决定。
- **CreateDevice 重试调用 + 0x4a1ee7**（D3D8 对象 0x3C 槽）确认是否也需重定向。
- **regain_device / last_resort**：8.0 未安装对应挂钩，已禁用。

### 2.2 其他文件
- `gm_interface.cpp` `dx9_backbuffer_format`（8.1 值 0x85b394 保留但标记）：8.0 无独立格式全局，方案 = present-params 构造点改指 `d3d_parameters` 后写 `d3d_parameters.BackBufferFormat`。
- `gm82dx9.cpp` `__gm82dx9_setalphabuffer`：同上（当前会写 0x85b394，8.0 镜像外，已注释警示）。
- `gm82dx9.cpp` `__gm82dx9_surface_set_depth`：8.0 的 GMSURFACE 无 zbuffer 字段，已改为返回 -1，待重写。
- `transform.cpp` `d3d_transform_add_stack_top`：transform 栈全局（8.1 值 0x686434/0x71234c）未定位，已 no-op。
- `shaders.cpp` `__gm82dx9_sampler_set` 的 `texture_exists` 内联 asm（8.1 值 0x620ff8）：8.0 等价函数未定位，暂时按"纹理存在"处理。

---

## 3. 下一轮验证流程（遵循桌面 SOP）

1. **枚举 8.0 vtable 调用点**：对每个 8.1 补丁点（方法, D3D8 槽位），用跨版本相似性定位 8.0 对应函数，再在函数内按 `FF 50/FF 90 <槽位>` 找具体调用点，反汇编核对语义后替换地址。补丁值/编码不变。
2. **设备创建链**：`D3DCreate`→`Direct3DCreate9` 重定向 + SDK 版本（0x4a1e13）+ CreateDevice 包装（0x4a1f1f，注意重试逻辑）→ present-params 接管（0x4a1ea2/0x4a228f）→ D3DCAPS 接管。
3. **D3DX**：确认 DllMain 与 `sub_49A254` 的时序，把 14 个全局（0x593868–0x59389c）改指 D3DX9。
4. **剩余钩子**：`regain_device`/`last_resort`/数学段（按需）。
5. **实测**：必须用**非空工程**（有表面/3D/surface_set_depth 场景），钩子日志复核 Clear(ZBUFFER)/Present/DrawPrimitiveUP 走到 D3D9。**流程见 §6**。
6. 每个地址验证后把 `GM80-TODO` 注释改为 `[GM80] ✅` 并记录证据。

---

## 6. 日志驱动的测试方法（2026-08-05）

插件内置 `gm_log`：写 **`%TEMP%\gm82dx9_port.log`**（追加）+ OutputDebugStringA（Sysinternals **DbgView** 可实时看）。
实现只在 `inject.cpp`，声明在 `gm82dx9.h`，各 TU 共用。开关 `GM80_LOG`（默认 1，改 0 即全部编译掉）。
**loader-lock 安全**：只用 kernel32 文件函数 + 栈上 sprintf，不在 DllMain 里碰 CRT 文件/堆函数。

### 6.1 编译（原生 VS 工程，结构对齐 GMSave）

- **构建（已实测通过，2026-08-05）**：`MSBuild GMDirectX9.sln /p:Configuration=Release /p:Platform=x86`（或 VS2022 打开 `GMDirectX9.sln` F7，或 `build.bat`）。
- 产物：`Release/GMDirectX9.dll`（x86 PE32，导出接口完整）；shader 由 PreBuildEvent 用 Windows SDK 自带 fxc 编译到 `source/vs_pass.vs3`、`source/ps_pass.ps3`。
- 结构要点（对齐 GMSave，2026-08-05 重构）：
  - 根目录：`dllmain.cpp`（DllMain 入口，调 `gm80_apply_patches()`）、`pch.h/pch.cpp/framework.h`、`GMDirectX9.sln/.vcxproj/.filters`、`build.bat`
  - `source/`：全部源码（含日志模块 `gm_log.h/gm_log.cpp`、`gm82dx9.h`、`.gml` 脚本、shader 源/产物）
  - `Librarys/`：DX 头（d3d9.h/d3dx9.h/DirectXMath/dxerr）+ `D3DX9_43.dll`；`Librarys/lib/`：d3d9.lib/d3dx9.lib
  - 输出 `Debug//Release/`，中间件 `obj/Debug//obj/Release/`
- 与 GMSave 的有意差异（源码 ANSI 风格决定）：
  - `CharacterSet=MultiByte`（GMSave 是 Unicode；本代码 MessageBox 传 char*）
  - 不加 `NOMINMAX`（源码用 windows.h 的 min/max 宏）
  - `SDLCheck=false`（插件含大量 __asm）
  - `TargetName=GMDirectX9`（生成 `GMDirectX9.dll`；导出符号仍为 `__gm82dx9_*`）
  - 子文件夹名 `source`（GMSave 是 `<工程名>/<工程名>/`，用户明确要求不嵌套同名文件夹）
- **编码修复**：源码 UTF-8 **带 BOM** + `/utf-8`。此前 UTF-8 无 BOM 被 MSVC 按 936 误读，中文注释末字节可能变 `\` 导致**下一行被吞成续行**（真实踩过：shaders.cpp 一行 `auto textures` 被吞，编译器报 `textures` 未定义）。
- **/WX 已去掉**：预存 C4244（double→DWORD，GM 扩展天然如此）。
- gex 打包：已删除（用户要求纯净 DLL，见 §4）。

### 6.2 部署（GM 8.0 扩展）

1. GM 8.0 里新建/打开一个**非空工程**（有背景或精灵 + 至少一个对象）。
2. 用 GM 8.0 IDE 新建扩展指向 `GMDirectX9.dll`（原 gm82dx9.gej 已删，需重建函数清单，见 §4）。
3. `GMDirectX9.dll` 与 `D3DX9_43.dll` 放进工程扩展目录（插件自身调 D3DX9 函数，必须随扩展分发）。
4. 运行时删掉旧的 `%TEMP%\gm82dx9_port.log`，F5 运行，退出后看日志。

> **建议分层测**：第 1 层只让 DLL 被加载（扩展注册后不调用任何插件 GML 函数），确认日志 #1–#5（补丁+设备）后，
> 第 2 层再加 GML 调用（`__gm82dx9_dllcheck`/`cleardepth`/`resize_backbuffer` 等）验证 #6。
> 注意首个测试**先别调 `__gm82dx9_setalphabuffer`**（8.0 尚未接管 present-params，会写镜像外地址，见 §2.2）。
> 只测"补丁层"（DllMain 是否写成功、不崩溃）的话，可先不建扩展：用 x64dbg/IDA 加载 `空工程.exe`，
> 在图形初始化（0x4a1da0）**之前**的入口断点处 `LoadLibraryA("GMDirectX9.dll")`，再放行。日志的读回验证
> 会给出补丁是否全部落盘。注意：这种方式必须赶在图形初始化前注入，否则 D3DX/CreateDevice 补丁错过时机。

### 6.3 日志判定点（按顺序核对）

| # | 日志内容 | 含义 |
|---|---|---|
| 1 | `=== GM82DX9 DLL_PROCESS_ATTACH ===` + `exe:` 路径 | DLL 被 runner 加载，确认进程是 GM 8.0（路径含游戏 exe） |
| 2 | `--- DllMain patch verification --- ... PASS (0 MISMATCH)` | 16 个代表性补丁点读回全部命中；`WriteProcessMemory FAILED` 或 `MISMATCH` = 该地址没写进去，需查 |
| 3 | `CreateDevice: adapter=... device=0x...` + `CreateDevice -> 0x0 (S_OK)` | D3D9 设备创建成功，**核心证明** |
| 4 | `d3dx globals 0x593868[14]: bitmap=0x3FFF d3dx9_43.dll=LOADED` | D3DX9 已加载且 14 个函数指针已填充 = D3DX 接管生效。`bitmap=0` 且 `not loaded` = 时序问题（DllMain 晚于 sub_49A254） |
| 5 | 游戏中绘制/用表面时出现 `SetRenderTarget / GetRenderTarget / SetVertexShader / CreateImageSurface / CopyRects / GetDisplayMode / GetBackBuffer` | **vtable 重映射真正走到 D3D9 的直接证据**（这些包装只在 runner 调用重定向后的站点时触发） |
| 6 | `__gm82dx9_dllcheck()` / `surface_get_count()` / `__gm82dx9_resize_backbuffer(...)` | GML 层已调到插件导出函数 |
| 7 | 窗口正常显示、帧不崩 | 渲染管线（BeginScene/EndScene/Present/Clear 槽位）正确 |

### 6.4 排查

- **验证阶段 MISMATCH/FAILED** → 该地址不可写或常量错。先禁掉对应补丁组（加 `//`），其余组继续测，二分定位。
- **CreateDevice 没出现** → D3D 设备没走到插件包装：查 0x4a1f1d 补丁 + 重试调用（0x4a1ee7）是否也需要重定向。
- **bitmap=0 / d3dx9 not loaded** → DllMain 晚于 sub_49A254，需要兜底：在 create_white_pixel 或首个 GML 调用时重写 14 个全局。
- **崩溃**：先用 DbgView 看最后一条日志，确定崩在哪个阶段；再用 D3D9 **debug runtime**（DirectX SDK Debug Runtimes）抓 API 误用。
- 跑完一轮把 `GM80-TODO` 改成 `[GM80] ✅` + 证据，同步更新本表。

---

## 4. 打包注意（2026-08-05 更新）

- **`GMDirectX9.gex` 已生成**（`py gm82gex.py GMDirectX9.gej --noinstall`，~1MB）。
- **最终形态：包内只有 2 个 DLL** —— `GMDirectX9.dll` + `D3DX9_43.dll`。**无 gml 脚本、无 shader**（gml 层一路踩 8.0 缺失函数如 `ds_list_add_many`/`buffer_*`，已整个移除）。函数全部由 gej 里 DLL 条目的函数清单直接注册（已逐一校验都是真实 DLL 导出；`D3DXCheckVersion` 是 D3DX9_43.dll 的导出）。
- **gej 无依赖**：`dependencies: []`。
- 使用：扩展启动时加载 DLL（早于图形初始化，见 §2.1），GML 里直接调导出函数名（如 `surface_get_count()`、`argb_get_color()`、`d3d_transform_xyzst()`、`__gm82dx9_*`）。
- `source/*.gml` 仍在仓库（已剥离 buffer 的版本）但**不再打包**；如不需要可删除。
- 注意：DLL 仍导出少数 buffer 相关内部函数（`__gm82dx9_buffer_to_surface` 等），8.0 无 buffer 系统故不可用，但注册无害。
- `Librarys/D3DX9_43.dll`、`Librarys/lib/d3d9.lib`、`Librarys/lib/d3dx9.lib` 随仓库分发，插件自用 D3DX9 函数可用。

---

## 5. 已确认的 8.0 关键地址速查（绝对地址，基址 0x400000）

| 地址 | 内容 |
|---|---|
| `0x484DF4` | `D3DCreate`（调 `Direct3DCreate8(220)`） |
| `0x4A1DA0` | 图形初始化（present params + CreateDevice @0x4a1f1f 槽 0x3C；SDK 版本 push @0x4a1e13） |
| `0x58D384` / `0x58D388` | IDirect3D8 对象 / **device 全局**（GMAPI GMDIRECT3DINFO） |
| `0x58FBE4` | 静态指针常量 `0x0058D388`（绘图路径二级间接，**无需补丁**） |
| `0x4A2228` | `INNER_display_set_size`（= runner_display_reset） |
| `0x4A27CC` / `0x4A2708` | `INNER_screen_refresh` / `D3D_CreateDevice`（Present） |
| `0x49E748` | clear_depth |
| `0x4A236C` | resize_backbuffer |
| `0x49E0D0` | `INNER_draw_primitive_end`（SetVertexShader 0x130 + DrawPrimitiveUP 0x120） |
| `0x49CAA8` / `0x49CB18` | `INNER_draw_clear` / `_alpha`（Clear 0x90） |
| `0x58D378` / `0x6C7240` | surface count / surface 数组（16B/项） |
| `0x58D3D0` / `0x6C7320` | texture count / texture 数组（24B/项） |
| `0x58D4CC` / `0x58D4D4` | rooms 数组（id→指针）/ count |
| `0x593868`–`0x59389C` | 14 个 D3DX8 函数指针全局（连续块） |
| `off_58F820` / `off_58FC14` | CreateTexture / CreateDepthStencil 间接指针（需接管为 D3D9 槽位） |

## 6. 2026-08-05 崩溃根因(第 5 处): present params 布局错位 → hDeviceWindow=0xFFFFFFFF

现象: CreateDevice 返回 0x8876086C(D3DERR_NOTAVAILABLE), 全屏+窗口回退都失败; "Failed to initialize drawing surfaces" 弹窗 + Runtime error 216。

排查: 独立探针 `d3d9probe.cpp`(Job tmp, vs2022 cl)证实 **机器 D3D9 完全正常**:
  GetAdapterCount=1 → AMD Radeon(aticfx32.dll); HAL windowed/fullscreen/REF 全部 D3D_OK。
  (Win32 有两个显示适配器: GameViewer Virtual Display + AMD 核显, 但 D3D9 只枚举 AMD —— 虚拟适配器不参与。)
所以问题在插件, 不在机器。

根因: 8.0 runner 传的是 **D3D8 布局**的 D3DPRESENT_PARAMETERS。D3D8 无 MultiSampleQuality 字段,
从 MultiSampleType 起 D3D8/D3D9 布局全部错位:
  D3D9.hDeviceWindow(0x1C) ← 实际是 D3D8.Windowed = 0xFFFFFFFF(栈垃圾)
  D3D9.Windowed(0x20)      ← 实际是 D3D8.EnableAutoDepthStencil = 0
包装函数把 D3D9 偏移读出的 hDeviceWindow=0xFFFFFFFF 直接传给 CreateDevice → 无效句柄 → NOTAVAILABLE。
(此前第 4 处注释猜的"未初始化字段被 D3D9 严格校验拒绝"方向对, 但具体错位字段判断有误——clean pp9 仍拷贝了错位字段。)

修复(inject.cpp CreateDevice): pp9 只采用两版布局一致的头部 4 字段(0x00-0x0C),
hDeviceWindow = hFocusWindow(已验证有效), 暂强制 Windowed=TRUE(探针验证窗口模式可行),
PresentationInterval=IMMEDIATE。全屏语义(Windowed/刷新率/BackBufferFormat 接管)留后续。

产物: Release/GMDirectX9.dll + GMDirectX9.gex(重新打包)。待实机: 重新应用 gex + 重编译。

## 7. 2026-08-05 surface 管线验证通过(误报排查)

窗口创建修复后(设备 D3D_OK, windowed=1), 测试工程第 4 层报 "Trying to use non-existing surface"。
排查结论: **插件 surface 管线本来就通**。pid 26924 日志完整序列:
  CreateDevice -> 0x0 device=0x060E5960 windowed=1
  surface_get_count() -> 1                            (surface_create 成功, 计数=1)
  GetRenderTarget  -> 0x0 rt=backbuffer               (INNER_surface_set_target 保存当前 RT)
  SetRenderTarget  -> 0x0 rt=surface                  (切换到 surface)
  SetRenderTarget  -> 0x0 rt=backbuffer               (INNER_surface_reset_target 切回)
报错根因是**测试代码 bug**: `surface_free(surf)` 后又 `draw_surface(surf)`(释放后再用) → draw_surface 校验失败弹错。
已改 objMain.gml: 只用一个 set_target/reset 周期, 不在 draw 里 free(改由 Destroy 事件 free)。
补充: GetRT 补丁宏是 `90 E8 rel32`(0xe890 小端=90 E8, NOP+call, 6 字节等长替换 sz6 的 FF 90 disp32), 已验证有效。

## 8. ✅ 2026-08-05 首帧渲染验收通过(里程碑)

测试工程 objMain 四层全部正常绘制:
  1. dllcheck 文本(插件导出 + 补丁状态)
  2. argb 计算 + surface_get_count(=1)
  3. draw_rectangle / draw_circle(DrawPrimitiveUP + SetTexture + SetVertexShader 重映射槽位)
  4. surface_create → surface_set_target(GetRT+SetRT 重映射) → 画黄圆 → reset → draw_surface → 蓝色方块内黄圆
含默认字体 draw_text(字体纹理走 D3DX9 路径)也正常。
当前状态: 设备 windowed=1(暂强制窗口), 全屏接管(GM80-TODO)尚未实现。

## 9. 2026-08-05 Reset 接管(vtable 槽 0x40 钩子)——修复 d3d_start "Failed to use 3D mode"

现象: 3D 测试块 d3d_start() 弹 "Failed to use 3D mode"。
根因: INNER_d3d_start → INNER_display_set_size → **device->Reset**(D3D8 槽 0x38→补丁到 D3D9 槽 0x40, 正确)。
runner 在 INNER_display_set_size 栈上构造的是 **D3D8 布局** present params, D3D9 Reset 按 D3D9 布局读 →
字段错位(hDeviceWindow 读到 D3D8.Windowed 等) → Reset 失败返回 0 → d3d_start 弹错。与 CreateDevice 同类错位。

修复(inject.cpp): CreateDevice 成功后把**设备 vtable 槽 0x40(Reset)重定向到插件 ResetDevice 包装**。
ResetDevice 用 CreateDevice 时的干净 D3DPRESENT_PARAMETERS 副本(g_pp9, 与 CreateDevice 逐字节一致,
D3D9 要求 Reset 参数与 CreateDevice 相同)调真实 Reset(real_reset = 原 vtable[16] 保存)。VirtualProtect
把 vtable 页改可写后写指针。一次挂钩覆盖全部 Reset 调用点: d3d_start/d3d_end/display_set_size/
display_reset/display_set_frequency/display_set_colordepth 等。
暂不采纳 runner 的宽高(F11/F12 不生效是预期), 尺寸接管留给全屏(D3DPRESENT_PARAMETERS 格式来源)工作。

## 10. ✅ 2026-08-06 FPU 精度判定: 8.0 不需要数学 trampoline

测试工程第 5 层 FPU 自检(11 个 GML 数学函数 vs 理论值, 阈值 0.00000001):
**5. FPU precision: 1** —— D3D9 未破坏 FPU 控制字, 数学保持双/扩展精度。
结论: 8.0 跳过 gm82dx9 的数学 FPU trampoline(8.1 地址 0x633d70-0x6344b6 段)。
依据: 8.0 的 CreateDevice 传 BehaviorFlags=0x22(含 D3DCREATE_FPU_PRESERVE), D3D9 遵守 → 控制字不被改写。
注: 8.2 也传 FPU_PRESERVE(0x42); gm82dx9 加 trampoline 很可能是为 8.1(主目标, 可能不传 FPU_PRESERVE)做的实测修复。

## 11. ✅ 2026-08-06 纯 patch 化重构(零导出)

按用户决策: GMDirectX9 变为**纯补丁插件, 不提供任何 GML 导出函数**, 唯一目的 = 把 GM8.0 runner 的 DX8 渲染后端完整替换为 DX9, 保持原功能不变。shader 支持改由外部 DLL(如 GMGraphic)直接操作 0x58d388 的 D3D9 设备实现。

改动:
- **移除编译**: source/gm82dx9.cpp, gm_interface.cpp, shaders.cpp, transform.cpp, vertex_buffers.cpp(146 个 __gm82dx9_* 导出)
- **新增 source/patch_support.cpp**: 补丁核心仅需的 3 个共享符号 —— present_params / runner_display_reset(0x4a2228) / d3d9_device(0x58d388)
- **SetVertexShader 简化**: 原 shaders.cpp 的 using_shader/vertex-declaration 分支移除, 改为直接 dev->SetFVF(fvf)(runner 每次 draw 的 FVF 调用等价物)
- **gej**: GMDirectX9.dll 函数表 146→0; D3DX9_43.dll 保留(1 个隐藏 D3DXCheckVersion); version 2.0
- **验证**: dumpbin /exports 无 export table(零导出)
- 依赖: inject.cpp(补丁核心) + gm_log + dllmain + dxerr + patch_support

握手点: d3d9_device = 0x58d388 = GMAPI GMDIRECT3DINFO.direct3dDevice, 其它 DLL 直接读此地址拿 D3D9 设备。

## 12. 2026-08-06 present-params 接管修正: GM8 只支持无边框全屏

用户确认: GM8 的"全屏"= **无边框窗口**(runner 把窗口放大到显示尺寸), 不是 D3D9 独占全屏。
因此 present-params 接管里设 `Windowed=FALSE` 的独占全屏逻辑**已回退**(2026-08-06 01:13 构建)。

当前正确行为:
- CreateDevice 始终 `Windowed=TRUE`; "全屏"时 runner 放大窗口, Present 自动拉伸(room-size 后缓冲)。
- ResetDevice 始终 no-op(避免 d3d_start/d3d_end 每帧真 Reset 黑屏; GM8 显示变化由窗口+Present 处理, 无需重建设备)。
- HWVP(0x22→0x42)保留。

未决: 后缓冲尺寸。GM8 原始 = 显示模式尺寸(1920x1080), 本插件 gm82dx9 的 room-sizing 缩成房间尺寸(640x480)。
"功能不变"倾向显示模式尺寸(全屏清晰), 但 room-size 可用。待用户决定是否去掉 room-sizing。

## 13. ✅ 2026-08-06 深度缓冲 + 设备丢失恢复 + 卸载安全(按 SOP 实现)

按《通用反汇编项目 SOP.md》流程(IDA 验证 → 改动 → 编译 → 待实测):

### ① 深度缓冲(对齐 gm82dx9 d3d_parameters)
- CreateDevice pp9 加 `EnableAutoDepthStencil=TRUE` + `AutoDepthStencilFormat=D24S8`;
  `CheckDeviceFormat` 验证 D24S8 不支持则回退 D16。
- 作用: 3D 的 d3d_set_hidden(z-test)/d3d_clear_depth 依赖交换链深度缓冲。

### ② 设备丢失恢复(对齐 gm82dx9 regain_device)
- IDA 验证: 8.0 `D3D_CreateDevice`(0x4a2708, cross-version 对照 8.2 sub_61FF5C score 1.0):
  Present(0x4a27ab, 槽 0x44)失败(<0) → `INNER_display_set_size`(0x4a27be) → device Reset(槽 0x40)。
  即 runner 在设备丢失时靠 Reset 恢复。
- ResetDevice 改为 TestCooperativeLevel 判定:
  - `D3DERR_DEVICENOTRESET` → 真 Reset(g_pp9 与 CreateDevice 一致)恢复设备
  - `S_OK` / `D3DERR_DEVICELOST` → no-op(避免 d3d_start/end 每帧真 Reset 黑屏; 等待可恢复)

### ③ DLL 卸载安全(对齐 gm82dx9 last_resort)
- IDA 验证: 8.0 卸载扩展走 `INNER_external_free`(0x518764, cross-version 对照 8.2 sub_579558 score 0.80)。
- 我们覆盖了设备 vtable 槽 0x40 → 卸载时须恢复, 否则 runner 跳未映射的 ResetDevice。
- 实现: `gm80_restore_reset_hook()` 在 DllMain(DLL_PROCESS_DETACH) 调用; __try/__except 兜底
  (设备可能已释放), 且仅当槽 0x40 仍是我们的钩子才恢复。比 gm82dx9 的裸汇编 call-site 挂钩更简洁安全。

待实测: 3D 方块深度遮挡(放开测试工程被注释的 3D 块)、alt-tab 切走切回设备恢复。

## 14. ✅ 2026-08-06 3D + 深度缓冲实测通过

用户确认: 使用 GMDirectX9 插件下 **3D 绘制正常**。
- 深度缓冲(§13① EnableAutoDepthStencil D24S8)生效, 3D 方块正确遮挡。
- 先前黑屏根因 = 测试代码**每帧 d3d_start + d3d_end 来回切换**(GM8 里 d3d_end 翻转 3D 状态
  → INNER_display_set_size → 每帧 device Reset → 清空渲染), 与插件无关。原生 D3D8 同样黑。
  正确用法: d3d_start() 每帧调用是幂等的(仅首次 Reset), 不配 d3d_end。
- 反汇编确认的 GM8.0 3D 细节: d3d_set_projection(9参)只设视图矩阵(SetTransform 2);
  d3d_set_projection_ext(13参)设视图+透视投影; d3d_set_projection_perspective(5参)首个参数实际被当角度用
  (帮助文档 (x,y,w,h,angle) 与实现矛盾, SOP §6-1 "标签错位" 实例)。

## 15. 修复: 空函数表导致 GM8 不加载 DLL(纯 patch 回归)

现象(2026-08-06 实机): 载入 GMDirectX9 gex 后不加载 d3d9, 日志 0 字节(DllMain 没跑)。
IDA 验证根因: GM8 扩展加载器 **sub_518368**(external_define 核心)是**按函数逐个 LoadLibrary+GetProcAddress**
的 —— gex 函数表为 0 个 → GM8 从不 LoadLibrary GMDirectX9.dll → DllMain 不执行 → 补丁不打。
扩展文件仍会被解压到 temp(所以能看到 DLL 在 gm_ttt_* 目录), 但从不加载。

修复: patch_support.cpp 加一个最小导出 `gm82dx9_loaded()`(返回 1)作为**加载触发**, gej 注册为
hidden 函数(与 D3DX9_43.dll 的 D3DXCheckVersion 同款 hidden)。它不是 GML 功能, 只是让 GM8 加载 DLL。
验证: dumpbin /exports 显示 1 个 ordinal(gm82dx9_loaded)。

教训(SOP §6): "纯 patch 零导出"与"GM8 必须至少注册一个函数才加载 DLL"矛盾 → 需要一个最小占位导出。

## 16. ✅ 2026-08-06 真游戏实测通过(收尾)

用户反编译多个别人发布的 GM8 游戏,应用 GMDirectX9 插件:**全部能跑,无图形问题**。
这覆盖了此前最大的验证缺口(精灵贴图/背景/粒子/动画等真实路径)。
插件达到"GM8 游戏原样跑在 D3D9 后端"的完整状态。

已验证能力全集: 2D/字体/surface/3D+深度缓冲/alt-tab 恢复/HWVP/无边框全屏/纯 patch(1 隐藏加载触发导出)。

未决(非阻塞): 后缓冲尺寸(当前=房间尺寸, 与 gm82dx9 一致; 改显示模式=GM8.0 原版, 用户未表态, 维持现状)。

## 17. ✅ 2026-08-06 收尾轮: 保险项 + 注释清理 + 日志移除

1. **CheckDeviceMultiSampleType 接管**(保险项): IDA 验证 sub_4A5054@0x4a50ef 调 D3D 对象槽 0x2C。
   D3D9 版本比 D3D8 多第 6 参 pQualityLevels → CreateDevice hook 里把 D3D 对象 vtable 槽 0x2C 改指
   CheckDeviceMultiSampleType_wrap(补 &quality)。0x4a50fa(槽 0x20=GetAdapterDisplayMode)签名两版相同, 无需接管。
2. **AddDirtyRect 判定不需要**(CopyRects 里原 TODO): ① D3D9 表面无法反向取父纹理(无 GetContainer);
   ② 目标多为 default-pool; ③ managed 纹理靠 LockRect 标记脏即可保证正确(AddDirtyRect 仅局部重传性能优化)。注释已改结论。
3. **注释清理**: 顶部 TODO 列表更新为完成态; 已解决的项(FPU/present-params/regain/D3DX/CheckDeviceMultiSampleType)
   标"已实现"; 8.0 确认不需要的项(SetMaterial/DrawPrimitive/GetDepthStencilSurface/SetNullTexture/white pixel/
   screen_refresh/CreateVertexBuffer/投影矩阵注入)标"✅ GM80-确认"。剩余 GM80-TODO 仅 1 处条件性备注。
4. **日志移除**: GM80_LOG=0(gm_log.cpp 整体编译掉, 不再写 %TEMP%\gm82dx9_port.log); 删除 CreateDevice 的
   gm_wrapper_hit.txt 调试标记写入。dumpbin 验证仅 1 个导出 gm82dx9_loaded(加载触发)。

## 18. ✅ 2026-08-08 SetVertexShader 恢复"自定义 VS 跨绘制存活"(对照 gm82dx9 原版 using_shader 分支)

**背景**: §11 纯 patch 化时把原 shaders.cpp 的 `using_shader`/顶点声明分支删成 `dev->SetFVF(fvf)`。
后果: GMGraphic 等外部 DLL 直接在 0x58d388 设备上绑自定义顶点着色器后, runner 每次绘制的
`SetVertexShader(FVF)`(D3D9 里 FVF 与 VS 互斥)会把自定义 VS 冲掉 → 固定管线兜底 → ps-only 只能
ps_2_0。经查 gm82dx9 原版已解决该问题(钩子内 FVF→顶点声明翻译), 本移植版需恢复。

**解法(不依赖 DLL 间共享标志)**: 钩子内 `dev->GetVertexShader()`, 非空(=外部 DLL 绑了自定义 VS)时
把引擎 FVF 重置翻译成 `SetVertexDeclaration`(三种引擎布局), 自定义 VS 保持绑定; 空时照旧 `SetFVF(fvf)`。
状态直接取自设备, GMGraphic 无需任何改动。

**IDA 验证(2026-08-08, 空工程.exe / y09x)**: 18 个 SetVertexShader 调用点全部 `call [eax+130h]`,
FVF 与布局仅三种, 与 gm82dx9 的 elems 完全一致:

| 布局 | FVF | stride | 调用点 | 顶点数据 |
|---|---|---|---|---|
| shape | `0x42` XYZ\|DIFFUSE | 16 | draw_point/line/triangle/rectangle 等 | FLOAT3+D3DCOLOR |
| 2d | `0x142` XYZ\|DIFFUSE\|TEX1 | 24 | DrawImage/DrawImage2/DrawTexture/sub_4A459C | FLOAT3+D3DCOLOR+FLOAT2 |
| 3d | `0x152` XYZ\|NORMAL\|DIFFUSE\|TEX1 | 36 | INNER_d3d_primitive_end | FLOAT3+FLOAT3+D3DCOLOR+FLOAT2 |

声明偏移正确性论证: 引擎现在以这三个 FVF 在固定管线下渲染正常, 顶点数据布局必为 FVF 隐含布局,
声明镜像即可(shape@0/12, 2d@0/12/16, 3d@0/12/24/28)。

**实现**: `inject.cpp` SetVertexShader 钩子(含 `gm80_elems_shape/2d/3d` + 懒创建 `gm80_decl_*`)。
GetVertexShader 不 AddRef 返回对象(D3D9 惯例, SDK save/restore 示例同款), 不 Release, 避免每帧
数千次调用泄漏/双释放。未知 FVF + 自定义 VS 激活时返回 D3DERR_INVALIDCALL(引擎忽略返回值,
保持上次声明; 引擎实际只用三种 FVF, 不会走到)。

**编译**: `MSBuild GMDirectX9.sln -p:Configuration=Release -p:Platform=x86` ✅ 通过。

**待实测**: GM8.0 工程挂 GMDirectX9.dll + GMGraphic, 绑一个带自定义 VS 的 shader(vs_3_0/ps_3_0),
确认① shader 激活后 draw_sprite/draw_primitive 等引擎绘制仍正常(走自定义 VS); ② ps_3_0 不再全透明
(修复前 ps_3_0 因 FVF 管线喂不进 v0/v1 而全透明)。注意: 自定义 VS 需按 gm82dx9 模型写成通用变换
(mul(pos, WVP) + 透传 color/texcoord, 参考 vs_pass.hlsl), 以适配引擎三种顶点布局。

## 19. ✅ 2026-08-08 仿固定管线 VS —— 无 VS 场景也能用 SM3.0（GMGraphic 配合）

**背景**: §18 的钩子只解决"自定义 VS 跨绘制存活"。但 ps-only 着色器(SDF/用户 shader_create 空 VS)
在固定管线(FVF)下仍只能 ps_2_0: D3D9 固定管线只把插值喂进 ps_2.x 的 t0/v0, ps_3_0 的输入
(`dcl_texcoord v0` + `dcl_color v1`) 读 0 → 全透明。只有顶点着色器能把数据喂进 ps_3_0 的 v0/v1。

**方案(用户定调, 2026-08-08)**: 
- GMDirectX9: `SetVertexShader(FVF)` 钩子无 VS 但自定义 PS 激活时, 绑【仿固定管线 VS】(FFP 等价的
  pos*W*V*P + 透传 color/uv), SM3.0 无 VS 也能用。无自定义 PS 时保持 SetFVF 真固定管线(引擎 FFP
  绘制零回归: 点大小/雾/光照照旧)。
- GMGraphic: ① `shader_create` ps 版本无条件按设备能力选 ps_3_0(删"无 VS 强制 ps_2_0"); ② 自绘
  (shader_set ps-only 分支 + vertex::end)在 D3D9 绑共享透传 VS + 声明, 不再走 SetFVF; D3D8 保持
  固定顶点管线(ps_1.4 无 v0/v1 路由问题, 最高 ps_1.4)。

**矩阵约定(关键, 2026-08-08 实测修正)**: 必须写 `mul(uWVP, pos)`(矩阵在前), 与 gm82dx9 一致。
原因: HLSL float4x4 在常量寄存器里默认【列主序】(register c0 = 矩阵列 0), 而 SetVertexShaderConstantF
写入的是 D3D 行主序矩阵(行=c0..c3) → shader 读到的矩阵 = 传入矩阵的转置 S^T。
`mul(uWVP, pos)` = S^T*pos = pos*S(含投影平移, 与 FFP 等价); `mul(pos, uWVP)` = pos*S^T = S*pos,
**丢掉投影的平移分量(如 [0,view_w]→NDC[-1,1] 的 -1)** → 精灵整体偏移半个屏幕、大量出屏。
fxc 证据: `mul(uWVP,pos)` 编译成跨寄存器累加 `mad c0*v.x+c1*v.y+c2*v.z+c3*v.w`(=pos*S);
`mul(pos,uWVP)` 编译成 `dp4 oPos.x, v0, c0` 单寄存器点积(=pos*S^T)。
我方初版误写 `mul(pos, uWVP)` 导致实机图像错乱(背景纯色 + 角落三角形 + 对角线碎片), 已修正。
WVP 常量: uWVP 显式 `register(c0)`, 写 c0-c3(4 寄存器)。

**运行时编译验证(2026-08-08)**: 用真实 d3dx9_43.dll 的 D3DXCompileShader 编译透传 VS 的 HLSL
(x86 C++ 测试程序): hr=D3D_OK, GetConstantDesc 返回 RegisterSet=VS/RegisterIndex=0/RegisterCount=4
—— `register(c0)` 被 d3dx9_43.dll 遵守, uWVP 恰在 c0-c3。编译与寄存器均无误。

**第二处坑(2026-08-08, 实机"纯色"): 透传 VS 常驻管线破坏 FVP 像素纹理采样**。
矩阵修正后图像从"碎片"变成"整屏纯色"。定位: 运行时编译+寄存器+矩阵都正确, 唯一与 gm82dx9 的
差异是 —— 我们把透传 VS 绑进了【无 shader 的绘制】(GMGraphic 图集/引擎精灵), 而 gm82dx9 默认
无 shader 时用纯 SetFVF(FFP), 只在用 shader 时才上 VS。VS 绑定 + FVP 像素管线(无 PS)的组合在
实机不采样纹理 → 全部渲染成顶点色 → 纯色。**修复**: GMGraphic 加 `g_vs_needed` 标志 —— 仅当
ps-only shader 激活(有 PS 无 VS, ps_3_0 需要 VS 喂 v0/v1)时才在 shader_set/vertex::end 绑透传 VS;
无 shader 的图集/自绘保持 FVP。GMDirectX9 钩子本来就只在 GetPixelShader()!=null 时才绑 fake VS,
且 shader_reset 会清 VS, 无此问题。

**HLSL 验证**(fxc, vs_2_0): `uWVP c0(4)`; `dcl_position v0 / dcl_color v1 / dcl_texcoord v2`;
`dp4 oPos.* v0, c0..c3`(行向量正确); `mov oD0, v1`(color→oD0→ps v1); `mov oT0.xy, v2`(uv→oT0→ps v0)。
SDF 着色器 ps_3_0 编译验证: `dcl_texcoord v0.xy` + `dcl_color v1` + `dcl_2d s0`, 与透传 VS 输出精确匹配。
vs_2_0 喂 ps_3_0 合法(D3D9 vs/ps 版本相互独立)。

**实现**:
- `inject.cpp`: `gm80_fake_ffp_vs`(懒编译) + `gm80_update_fake_ffp_wvp`(每绘制刷 WVP) +
  `gm80_bind_fake_ffp`(绑 VS+声明) + SetVertexShader 钩子加"无 VS + 有 PS → 仿固定管线 VS"分支;
  声明切换路径若当前 VS 是本钩子的 fake VS 则每绘制刷一次 WVP(投影可能已变)。
- GMGraphic: `d3d_adapter.h/cpp` 加 `set_vertex_shader_passthrough(VertexFmt)`(impl8=FVF 固定管线,
  impl9=透传 VS+声明+WVP); `shader.cpp` ps_profile 无条件 + shader_set/vertex::end 的 ps-only 分支。
- D3D9 分支不静态链 d3dx9.lib(与 D3DX8 撞名), 透传 VS 用已解析的 s_compile(D3DXCompileShader)编译,
  WVP 乘法手写 mul4x4。

**已知边界**: 
- 3D(带光照/雾/镜面)绘制 + ps-only shader 时, 仿固定管线 VS 只做 passthrough, 不做光照/雾——
  引擎 3D 若在 ps-only shader 下绘制会缺光照(用户 2D 为主, 可接受)。
- 仿固定管线 VS 无点大小控制(D3DRS_POINTSIZE 只作用于固定管线)——ps-only 下点精灵会退化为 1px。
- 设备 Reset 后 gm80_fake_ffp_vs/decl 理论失效; GM8 windowed 不会设备丢失(ResetDevice 只对
  D3DERR_DEVICENOTRESET 真 Reset), 暂不处理, 与既有 gm80_decl_* 生命周期一致。

**编译**: 两工程 `MSBuild *.sln -p:Configuration=Release -p:Platform=x86` ✅ 通过。

**待实测矩阵**:
1. GM8.0 + GMDirectX9 + GMGraphic, 无任何 shader: 引擎 draw_sprite/draw_text 正常(走 SetFVF, 回归测试)。
2. 绑带自定义 VS 的 shader(vs_3_0/ps_3_0): 引擎绘制正常, ps_3_0 不再全透明(§18 回归)。
3. ps-only shader(shader_create 空 VS, 如 SDF 文字): 自动 ps_3_0, GMGraphic 文字/图集正常; 
   引擎 draw_sprite 在 ps-only 下也正常(走仿固定管线 VS)。
4. 中途 d3d_set_projection 换视图: 仿固定管线 VS 的 WVP 跟随(每绘制刷)。
5. D3D8 模式(纯 GMGraphic, 无 GMDirectX9): SDF asm ps_1.4、图集、draw_primitive 全部照旧(回归)。

**最终结论(2026-08-09, 实机验证 Nature Edition, 已修复达成 SM3.0)**:
- 纯色根因 = **ps_3_0 + vs_2_0 透传 VS 实机全透明**。vs_2_0 输出是隐式 oT0/oD0(SM2 体系),
  没有 ps_3.0 的 vN 输入路由认的显式语义; 只有 vs_3.0 用 dcl 声明输出语义(dcl_color o1 /
  dcl_texcoord o2, fxc 验证)才能喂进 ps_3.0 的 v0/v1。**结论: SM2(vs_2.0)顶点输出不能配
  SM3(ps_3.0)像素输入, VS 必须同为 vs_3.0。**
- **修复(最终)**: ① 两个透传/仿固定 VS 全部改为 `vs_3_0` 编译(inject.cpp + d3d_adapter9.cpp);
  ② shader_create ps-only 放开 `ps_profile()`。实机全部正常: ps_3.0(HLSL Gray/Gamma 等)与
  asm ps_1.4(加载屏 psTest 等)都正常 → **ps-only 也能用 SM3.0 达成**。
- 诊断过程(钩子日志 + shader_set/create 日志 + passthrough WVP 日志): 游戏全部 ps-only;
  passthrough VS 的 WVP 实测正确(W=identity, V≈identity, P=2D 正交, WVP_44=1);
  矩阵约定 mul(uWVP,pos) 正确。问题只在 VS 版本(SM2 vs SM3)。
