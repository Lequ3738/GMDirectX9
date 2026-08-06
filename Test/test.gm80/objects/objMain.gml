#define Create_0
/*"/*'/**//* YYD ACTION
lib_id=1
action_id=603
applies_to=self
*/
d3d_start();
surf = surface_create(64, 64);
#define Destroy_0
/*"/*'/**//* YYD ACTION
lib_id=1
action_id=603
applies_to=self
*/
surface_free(surf);
d3d_end();
#define Draw_0
/*"/*'/**//* YYD ACTION
lib_id=1
action_id=603
applies_to=self
*/
draw_set_color(c_blue);
ok = true;

// ---- 第 0 层(先画垫底): 3D 旋转立方体——测 D3DX9 投影/视角矩阵 + 顶点变换路径 ----
// GM8.0 3D API 注意: 无 d3d_set_zwriteenable/ztestenable, 深度测试用 d3d_set_hidden(true);
// 相机用 d3d_set_projection_ext(xfrom,yfrom,zfrom,xto,yto,zto,xup,yup,zup,angle,aspect,znear,zfar)。
// 相机绕原点转, 两个错位方块产生透视视差。若 D3D9 破坏矩阵/FPU 精度, 会明显抖动或变形。

d3d_set_hidden(true);
ang = (current_time / 1000) * 30;
d3d_set_projection_ext(250 * cos(ang * pi / 180), 0, 250 * sin(ang * pi / 180),
                       0, 0, 0, 0, 1, 0, 45, 1.333, 1, 1000);
draw_set_color(c_aqua);
d3d_draw_block(-40, -40, -40, 40, 40, 40, -1, 1, 1);
draw_set_color(c_fuchsia);
d3d_draw_block(25, 25, 25, 75, 75, 75, -1, 1, 1);

/*draw_text(8, 8, "1. dllcheck: " + string(ok) + "  (820 = DLL已加载, 补丁已打)");

// ---- 第 3 层: 渲染(走重映射的 D3D9 槽位)——第 1、2 层正常后再放开 ----
draw_set_color(c_red);
draw_rectangle(20, 80, 120, 180, false);      // 触发 SetTexture(NULL)+DrawPrimitiveUP+SetVertexShader
draw_set_color(c_lime);
draw_circle(220, 130, 50, false);

// ---- 第 4 层: surface(走重映射的 SetRenderTarget) ----
// 注: 之前版本在 surface_free 之后又 draw_surface 了一次 → "Trying to use non-existing surface"。
// 这是测试代码 bug(释放后再用), 不是插件问题。surface 在 Destroy 事件释放, draw 里只用不释放。
if (surface_exists(surf)) {
    surface_set_target(surf);
        draw_clear(c_blue);
        draw_set_color(c_yellow);
        draw_circle(32, 32, 16, false);
    surface_reset_target();
    draw_surface(surf, 320, 80);
    draw_text(8, 216, "4. surface: create+set_target+draw_surface OK (见蓝色方块内黄圆)");
} else {
    draw_text(8, 200, "4. surface_create FAILED");
}

// ---- 第 5 层: FPU 精度自检——判定 8.0 是否需要数学 trampoline ----
// 阈值 0.00000001: 若 D3D9 把 FPU 降为单精度(24位尾数), sqrt(2) 误差~1e-7 会超阈值 FAIL;
// 保持双/扩展精度则误差~1e-15, 稳过。这是 gm82dx9 给 8.1 加 trampoline 的根因测试。
ok = true;
if (abs(sqrt(2) - 1.4142135623730951) > 0.00000001) { ok = false; }
if (abs(sqrt(3) - 1.7320508075688772) > 0.00000001) { ok = false; }
if (abs(exp(1) - 2.718281828459045) > 0.00000001) { ok = false; }
if (abs(ln(2) - 0.6931471805599453) > 0.00000001) { ok = false; }
if (abs(log2(8) - 3) > 0.00000001) { ok = false; }
if (abs(log10(1000) - 3) > 0.00000001) { ok = false; }
if (abs(arcsin(0.5) - 0.5235987755982988) > 0.00000001) { ok = false; }
if (abs(arccos(-1) - 3.141592653589793) > 0.00000001) { ok = false; }
if (abs(arctan(1) - 0.7853981633974483) > 0.00000001) { ok = false; }
if (abs(arctan2(1, 1) - 0.7853981633974483) > 0.00000001) { ok = false; }
if (abs(power(2, 0.5) - 1.4142135623730951) > 0.00000001) { ok = false; }
if ok col = c_lime else col = c_red
draw_set_color(col);
draw_text(8, 232, "5. FPU precision: " + string(ok) + "  sqrt2=" + string(sqrt(2)) + "  ln2=" + string(ln(2)) + "  atan1=" + string(arctan(1)));

// ---- 第 6 层: 分辨率切换(手动按键, 测 Reset)——注意 runner 传 Reset 的 present-params 仍是 D3D8 布局, 可能不生效 ----
draw_set_color(c_yellow);
draw_text(8, 248, "6. F11=320x240  F12=640x480  (触发 device Reset, 可能不生效)");
if (keyboard_check_pressed(vk_f11)) { display_set_size(320, 240); }
if (keyboard_check_pressed(vk_f12)) { display_set_size(640, 480); }
