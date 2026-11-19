//! SutureOS GUI shell prototype (v0.41.0): windows + taskbar + start menu on the
//! new Rust renderer.
//!
//! Scope: this is the first slice of the "window manager migration". It draws a
//! Win11-flavoured desktop shell with TWO windows, a taskbar (start button,
//! pinned apps, per-window buttons), and a start menu. Interactions: click to
//! focus, drag by the title bar, minimise/restore via the taskbar, maximise via
//! the title-bar button (or the same taskbar button with a double click), start
//! menu open/close. Everything is a pure function of the kernel-provided mouse
//! state and millisecond clock, so screenshots are reproducible.
//!
//! Performance: the wallpaper is rendered once into a full-screen cache and the
//! acrylic backdrop once into a small (256x192) blurred cache that windows and
//! the taskbar sample. Per frame we only memcpy the wallpaper and draw the two
//! windows + taskbar + menu, which keeps the prototype interactive under TCG
//! (the analytic-AA full scene costs ~1.5 s per render, see lib.rs).

use crate::swrast::{self, Surface};

// ---------------------------------------------------------------- layout
pub const SCREEN_W: i32 = 1024;
pub const SCREEN_H: i32 = 768;
pub const TASKBAR_H: i32 = 38;
pub const TASKBAR_Y: i32 = SCREEN_H - TASKBAR_H;

const TITLE_H: i32 = 30;
const MENU_W: i32 = 300;
const MENU_H: i32 = 236;
const MENU_X: i32 = 12;
const MENU_Y: i32 = TASKBAR_Y - MENU_H - 8;

const SMALL_W: usize = 256;
const SMALL_H: usize = 192;

#[derive(Clone, Copy)]
struct Win {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
    nx: i32,
    ny: i32,
    nw: i32,
    nh: i32,
    minimized: bool,
    maximized: bool,
    z: i32,
    title: &'static str,
    accent: u32,
    open: bool,
}

const fn win0() -> Win {
    Win { x: 150, y: 120, w: 430, h: 300, nx: 150, ny: 120, nw: 430, nh: 300,
          minimized: false, maximized: false, z: 1, title: "系统设置", accent: 0x4f8dff, open: true }
}
const fn win1() -> Win {
    Win { x: 470, y: 300, w: 400, h: 260, nx: 470, ny: 300, nw: 400, nh: 260,
          minimized: false, maximized: false, z: 2, title: "任务管理器", accent: 0x22c1a8, open: true }
}

struct Shell {
    wins: [Win; 2],
    focus: usize,
    menu_open: bool,
    dragging: bool,
    drag_idx: usize,
    drag_dx: i32,
    drag_dy: i32,
    press_ms: u32,
    press_btn: i32,
    last_click_ms: u32,
    last_click_idx: i32,
    hover_wb: i32,     /* hovered taskbar window button */
    hover_pin: i32,    /* hovered pinned app */
    hover_ctl: i32,    /* hovered title-bar control (0 min,1 max,2 close) */
    hover_win: i32,
    started: bool,
    frame_ms: u32,
    prof_frames: u32,
    prof_copy: u64,
    prof_win: u64,
    prof_bar: u64,
    prof_total: u64,
}

static mut SHELL: Shell = Shell {
    wins: [win0(), win1()],
    focus: 1,
    menu_open: false,
    dragging: false,
    drag_idx: 0,
    drag_dx: 0,
    drag_dy: 0,
    press_ms: 0,
    press_btn: 0,
    last_click_ms: 0,
    last_click_idx: -1,
    hover_wb: -1,
    hover_pin: -1,
    hover_ctl: -1,
    hover_win: -1,
    started: false,
    frame_ms: 0,
    prof_frames: 0,
    prof_copy: 0,
    prof_win: 0,
    prof_bar: 0,
    prof_total: 0,
};

// ---------------------------------------------------------------- caches
static mut WALL: [u32; (SCREEN_W as usize) * (SCREEN_H as usize)] =
    [0; (SCREEN_W as usize) * (SCREEN_H as usize)];
static mut SMALL: [u32; SMALL_W * SMALL_H] = [0; SMALL_W * SMALL_H];
static mut FRAME: [u32; (SCREEN_W as usize) * (SCREEN_H as usize)] =
    [0; (SCREEN_W as usize) * (SCREEN_H as usize)];
static mut XMAP: [u16; SCREEN_W as usize] = [0; SCREEN_W as usize];
static mut YMAP: [u16; SCREEN_H as usize] = [0; SCREEN_H as usize];
static mut CACHED: bool = false;

extern "C" {
    fn console_printf(fmt: *const u8, ...) -> i32;
    static g_tick_count: u64;
}

#[inline]
fn ticks() -> u64 {
    unsafe { crate::shell_tick() }
}

fn log0(msg: &core::ffi::CStr) {
    unsafe { console_printf(b"[gui-shell] %s\n\0".as_ptr(), msg.as_ptr()) };
}
fn log2(msg: &core::ffi::CStr, a: i32, b: i32) {
    unsafe { console_printf(b"[gui-shell] %s %d %d\n\0".as_ptr(), msg.as_ptr(), a, b) };
}

#[inline]
fn blend_px_raw(dst: u32, src: u32, a: u32) -> u32 {
    let inv = 255 - a;
    let r = (((dst >> 16) & 0xff) * inv + ((src >> 16) & 0xff) * a) / 255;
    let g = (((dst >> 8) & 0xff) * inv + ((src >> 8) & 0xff) * a) / 255;
    let b = ((dst & 0xff) * inv + (src & 0xff) * a) / 255;
    (r << 16) | (g << 8) | b
}

unsafe fn build_caches() {
    let wall = &mut *core::ptr::addr_of_mut!(WALL);
    let mut s = Surface { px: &mut wall[..], w: SCREEN_W as usize, h: SCREEN_H as usize,
                          stride: SCREEN_W as usize };
    // wallpaper: 3-stop gradient + two soft colour blobs (the acrylic source)
    s.vgrad3(0, 0, SCREEN_W, SCREEN_H, 0x1b2440, 0x121a2b, 0x090d16, 0.45);
    s.hgrad_round(60.0, 90.0, 500.0, 560.0, 96.0, 0x3f7bff, 0x9a5cff, 0xd0);
    s.hgrad_round(600.0, 60.0, 1000.0, 400.0, 96.0, 0x12b8a6, 0x2f6bff, 0xc0);

    // small mirror of the wallpaper, blurred -> cheap "frosted" source for
    // windows/taskbar/menu (sampled with nearest neighbour, looks right for
    // translucent surfaces)
    let small = &mut *core::ptr::addr_of_mut!(SMALL);
    let mut ss = Surface { px: &mut small[..], w: SMALL_W, h: SMALL_H, stride: SMALL_W };
    ss.vgrad3(0, 0, SMALL_W as i32, SMALL_H as i32, 0x1b2440, 0x121a2b, 0x090d16, 0.45);
    let sx = SMALL_W as f32 / SCREEN_W as f32;
    let sy = SMALL_H as f32 / SCREEN_H as f32;
    ss.hgrad_round(60.0 * sx, 90.0 * sy, 500.0 * sx, 560.0 * sy, 24.0,
                   0x3f7bff, 0x9a5cff, 0xd0);
    ss.hgrad_round(600.0 * sx, 60.0 * sy, 1000.0 * sx, 400.0 * sy, 24.0,
                   0x12b8a6, 0x2f6bff, 0xc0);
    swrast::acrylic_blur(&mut ss, 0, 0, SMALL_W as i32, SMALL_H as i32, 6);

    // screen -> small-buffer coordinate maps (built once, 2 KiB total)
    let xm = &mut *core::ptr::addr_of_mut!(XMAP);
    for x in 0..SCREEN_W as usize {
        xm[x] = ((x * SMALL_W) / (SCREEN_W as usize)) as u16;
    }
    let ym = &mut *core::ptr::addr_of_mut!(YMAP);
    for y in 0..SCREEN_H as usize {
        ym[y] = ((y * SMALL_H) / (SCREEN_H as usize)) as u16;
    }
    CACHED = true;
}

#[inline]
fn small_sample(x: i32, y: i32) -> u32 {
    // Integer mapping only: the kernel is built without SSE, so f32 arithmetic
    // is software-emulated (compiler_builtins) and a per-pixel float multiply
    // alone made a window repaint take >1 s under TCG.
    let small = unsafe { &*core::ptr::addr_of!(SMALL) };
    let xm = unsafe { &*core::ptr::addr_of!(XMAP) };
    let ym = unsafe { &*core::ptr::addr_of!(YMAP) };
    let cx = x.clamp(0, SCREEN_W - 1) as usize;
    let cy = y.clamp(0, SCREEN_H - 1) as usize;
    small[ym[cy] as usize * SMALL_W + xm[cx] as usize] & 0x00ff_ffff
}

/// Frosted rounded rect: fills `(x0,y0)-(x1,y1)` with the blurred wallpaper
/// sampled directly (no per-frame blur) plus a translucent white tint.
fn frosted(s: &mut Surface<'_>, x0: i32, y0: i32, x1: i32, y1: i32, r: f32,
           tint_alpha: u32) {
    // Fast path: the interior (away from the rounded corners) has full
    // coverage, so only the four r x r corner squares need the sqrt test.
    // This is what makes a per-frame window repaint affordable under TCG.
    let rr = r.max(0.0).min(((x1 - x0) as f32) * 0.5).min(((y1 - y0) as f32) * 0.5);
    let ri = rr as i32;
    let ys = y0.max(0);
    let ye = y1.min(s.h as i32);
    let xs = x0.max(0);
    let xe = x1.min(s.w as i32);
    for y in ys..ye {
        let fy = y as f32 + 0.5;
        let in_v = y >= y0 + ri && y < y1 - ri;
        for x in xs..xe {
            let cov = if in_v && x >= x0 + ri && x < x1 - ri {
                1.0
            } else {
                let fx = x as f32 + 0.5;
                let cx = crate::swrast::clampf(fx, x0 as f32 + rr, x1 as f32 - rr);
                let cy = crate::swrast::clampf(fy, y0 as f32 + rr, y1 as f32 - rr);
                let dx = fx - cx;
                let dy = fy - cy;
                let d = crate::swrast::fsqrt(dx * dx + dy * dy) - rr;
                crate::swrast::clampf(0.5 - d, 0.0, 1.0)
            };
            if cov <= 0.0 {
                continue;
            }
            let base = small_sample(x, y);
            let i = y as usize * s.stride + x as usize;
            let dst = s.px[i] & 0x00ff_ffff;
            if cov >= 1.0 {
                let c = blend_px_raw(base, 0xffffff, tint_alpha);
                s.px[i] = blend_px_raw(dst, c, 255) & 0x00ff_ffff;
            } else {
                let c = blend_px_raw(base, 0xffffff, (tint_alpha as f32 * cov) as u32);
                s.px[i] = blend_px_raw(dst, c, (cov * 255.0) as u32) & 0x00ff_ffff;
            }
        }
    }
}

fn control_rect(w: &Win) -> (i32, i32, i32, i32) {
    /* title-bar controls: minimise, maximise, close, 22x22 each */
    let y = w.y + (TITLE_H - 22) / 2;
    (w.x + w.w - 3 - 22 * 3 - 2 * 4, y, 22, 22)
}

/// (x, w) of the taskbar window button `i`, or None when hidden.
fn wb_rect(wins: &[Win; 2], i: usize) -> Option<(i32, i32)> {
    let mut x = 118;
    for k in 0..i {
        if wins[k].open { x += 96 + 8; }
    }
    if !wins[i].open { return None; }
    Some((x, 96))
}

fn hit_win(wins: &[Win; 2], mx: i32, my: i32) -> i32 {
    let mut best = -1;
    let mut bestz = -1;
    for (i, w) in wins.iter().enumerate() {
        if !w.open || w.minimized { continue; }
        if mx >= w.x && mx < w.x + w.w && my >= w.y && my < w.y + w.h && w.z > bestz {
            best = i as i32;
            bestz = w.z;
        }
    }
    best
}

// ---------------------------------------------------------------- frame
pub unsafe fn shell_step(
    fb: *mut u32,
    w: i32,
    h: i32,
    pitch_px: i32,
    mx: i32,
    my: i32,
    buttons: i32,
    click_x: i32,
    click_y: i32,
    _click_btn: i32,
    now_ms: u32,
) -> u32 {
    if fb.is_null() || w <= 0 || h <= 0 || pitch_px < w {
        return 0;
    }
    if !CACHED {
        build_caches();
        log0(c"caches built (wallpaper + frosted source)");
    }
    let (wu, hu) = (w as usize, h as usize);
    /* render into the back buffer, present once at the end (double buffering:
     * a screendump can otherwise catch the framebuffer half-painted) */
    let frame = &mut *core::ptr::addr_of_mut!(FRAME);
    let mut s = Surface { px: &mut frame[..], w: wu, h: hu, stride: wu };
    let sh = &mut *core::ptr::addr_of_mut!(SHELL);
    if !sh.started {
        sh.started = true;
        sh.frame_ms = now_ms;
        log0(c"shell: loop started");
    }

    // 1. wallpaper from cache
    let t_copy0 = ticks();
    let wall = &*core::ptr::addr_of!(WALL);
    for y in 0..hu.min(SCREEN_H as usize) {
        let dst = y * wu;
        let src = y * SCREEN_W as usize;
        let n = wu.min(SCREEN_W as usize);
        s.px[dst..dst + n].copy_from_slice(&wall[src..src + n]);
    }

    let t_copy = ticks() - t_copy0;
    let mx_c = mx.clamp(0, w - 1);
    let my_c = my.clamp(0, h - 1);
    let left_down = (buttons & 1) != 0;
    let now = now_ms;

    // 2. hit testing: taskbar first (it is on top), then windows
    let start_btn = (8, TASKBAR_Y + 5, 34, 28);
    let mut hw = hit_win(&sh.wins, mx_c, my_c);
    let mut hover_ctl = -1;
    if hw >= 0 {
        let wref = sh.wins[hw as usize];
        let (bx, by, bw, bh) = control_rect(&wref);
        if my_c < wref.y + TITLE_H && mx_c >= bx && mx_c < bx + bw * 3 + 8 {
            let idx = (mx_c - bx) / (bw + 4);
            if idx >= 0 && idx <= 2 {
                hover_ctl = idx;
            }
        }
    }

    // 3. clicks
    let mut clicked_in_menu = false;
    if click_x >= 0 && click_y >= 0 {
        let cx = click_x;
        let cy = click_y;
        if sh.menu_open {
            if cx >= MENU_X && cx < MENU_X + MENU_W && cy >= MENU_Y && cy < MENU_Y + MENU_H {
                clicked_in_menu = true;
                /* menu rows are display-only in this slice */
            } else {
                sh.menu_open = false;
                log0(c"menu closed");
            }
        }
        if !clicked_in_menu {
            /* start button */
            if cx >= start_btn.0 && cx < start_btn.0 + start_btn.2 &&
               cy >= start_btn.1 && cy < start_btn.1 + start_btn.3 {
                sh.menu_open = !sh.menu_open;
                log0(if sh.menu_open { c"menu open" } else { c"menu closed" });
            } else {
                /* taskbar window buttons */
                let mut handled = false;
                for i in 0..2 {
                    if let Some((bx, bw)) = wb_rect(&sh.wins, i) {
                        if cx >= bx && cx < bx + bw && cy >= TASKBAR_Y + 5 &&
                           cy < TASKBAR_Y + TASKBAR_H - 5 {
                            handled = true;
                            let double = sh.last_click_idx == i as i32 &&
                                         now.wrapping_sub(sh.last_click_ms) < 1500;
                            sh.last_click_ms = now;
                            sh.last_click_idx = i as i32;
                            let is_focus = sh.focus == i && !sh.wins[i].minimized;
                            if double {
                                /* toggle maximise */
                                if sh.wins[i].maximized {
                                    sh.wins[i].maximized = false;
                                    sh.wins[i].x = sh.wins[i].nx;
                                    sh.wins[i].y = sh.wins[i].ny;
                                    sh.wins[i].w = sh.wins[i].nw;
                                    sh.wins[i].h = sh.wins[i].nh;
                                    log2(c"restore", i as i32, 0);
                                } else {
                                    sh.wins[i].nx = sh.wins[i].x;
                                    sh.wins[i].ny = sh.wins[i].y;
                                    sh.wins[i].nw = sh.wins[i].w;
                                    sh.wins[i].nh = sh.wins[i].h;
                                    sh.wins[i].maximized = true;
                                    sh.wins[i].x = 0;
                                    sh.wins[i].y = 0;
                                    sh.wins[i].w = SCREEN_W;
                                    sh.wins[i].h = TASKBAR_Y;
                                    log2(c"maximize", i as i32, 0);
                                }
                                sh.wins[i].minimized = false;
                                sh.focus = i;
                                sh.wins[i].z = sh.wins[1 - i].z + 1;
                            } else if is_focus {
                                sh.wins[i].minimized = true;
                                log2(c"minimize", i as i32, 0);
                                /* focus the other visible window */
                                let other = 1 - i;
                                if sh.wins[other].open && !sh.wins[other].minimized {
                                    sh.focus = other;
                                }
                            } else {
                                sh.wins[i].minimized = false;
                                sh.focus = i;
                                sh.wins[i].z = sh.wins[1 - i].z + 1;
                                log2(c"restore focus", i as i32, 0);
                            }
                        }
                    }
                }
                if !handled {
                    /* title-bar controls / title bar / window body */
                    if hw >= 0 {
                        let wref = sh.wins[hw as usize];
                        let (bx, by, bw, bh) = control_rect(&wref);
                        if cx >= bx && cx < bx + bw * 3 + 8 && cy >= by && cy < by + bh {
                            let idx = (cx - bx) / (bw + 4);
                            if idx == 1 {
                                if sh.wins[hw as usize].maximized {
                                    let ww = &mut sh.wins[hw as usize];
                                    ww.maximized = false;
                                    ww.x = ww.nx;
                                    ww.y = ww.ny;
                                    ww.w = ww.nw;
                                    ww.h = ww.nh;
                                    log2(c"restore", hw, 0);
                                } else {
                                    let ww = &mut sh.wins[hw as usize];
                                    ww.nx = ww.x;
                                    ww.ny = ww.y;
                                    ww.nw = ww.w;
                                    ww.nh = ww.h;
                                    ww.maximized = true;
                                    ww.x = 0;
                                    ww.y = 0;
                                    ww.w = SCREEN_W;
                                    ww.h = TASKBAR_Y;
                                    log2(c"maximize", hw, 0);
                                }
                            } else if idx == 0 {
                                sh.wins[hw as usize].minimized = true;
                                log2(c"minimize", hw, 0);
                                let other = if hw == 0 { 1 } else { 0 };
                                if sh.wins[other].open && !sh.wins[other].minimized {
                                    sh.focus = other;
                                }
                            } else {
                                sh.wins[hw as usize].open = false;
                                log2(c"close", hw, 0);
                                let other = if hw == 0 { 1 } else { 0 };
                                if sh.wins[other].open && !sh.wins[other].minimized {
                                    sh.focus = other;
                                }
                            }
                        } else if cy >= wref.y && cy < wref.y + TITLE_H {
                            /* focus + begin drag */
                            sh.wins[hw as usize].z = sh.wins[1 - hw as usize].z + 1;
                            sh.focus = hw as usize;
                            sh.dragging = true;
                            sh.drag_idx = hw as usize;
                            sh.drag_dx = cx - wref.x;
                            sh.drag_dy = cy - wref.y;
                            log2(c"drag begin", hw, cx);
                        } else {
                            sh.wins[hw as usize].z = sh.wins[1 - hw as usize].z + 1;
                            sh.focus = hw as usize;
                            log2(c"focus", hw, 0);
                        }
                    } else if cy < TASKBAR_Y {
                        /* click on empty desktop: drop focus highlight? keep simple */
                    }
                }
            }
        }
        sh.press_ms = now;
    }
    if !left_down && sh.dragging {
        sh.dragging = false;
        log2(c"drag end", sh.wins[sh.drag_idx].x, sh.wins[sh.drag_idx].y);
    }
    if sh.dragging {
        let bidx = sh.drag_idx;
        let nxp = (mx_c - sh.drag_dx).clamp(0, SCREEN_W - 200);
        let nyp = (my_c - sh.drag_dy).clamp(0, TASKBAR_Y - 80);
        sh.wins[bidx].x = nxp;
        sh.wins[bidx].y = nyp;
        if !sh.wins[bidx].maximized {
            sh.wins[bidx].nx = nxp;
            sh.wins[bidx].ny = nyp;
        }
    }

    // 4. draw windows bottom-up
    let t_win0 = ticks();
    let mut order = [0usize, 1];
    if sh.wins[0].z > sh.wins[1].z { order = [1, 0]; }
    for &i in order.iter() {
        let win = sh.wins[i];
        if !win.open || win.minimized {
            continue;
        }
        draw_window(&mut s, &win);
    }

    let t_win = ticks() - t_win0;
    // 5. taskbar (acrylic strip sampled from the frosted source)
    let t_bar0 = ticks();
    frosted(&mut s, 0, TASKBAR_Y, SCREEN_W, SCREEN_H, 0.0, 0x30);
    s.rect_fill(0, TASKBAR_Y, SCREEN_W, SCREEN_H, 0xffffff, 0x08);
    /* start button: rounded tile + the same pseudo-3D plate glyph as the icon */
    let hover_start = mx_c >= start_btn.0 && mx_c < start_btn.0 + start_btn.2 &&
                      my_c >= start_btn.1 && my_c < start_btn.1 + start_btn.3;
    s.rect_fill(start_btn.0, start_btn.1, start_btn.0 + start_btn.2,
                start_btn.1 + start_btn.3, 0xffffff,
                if hover_start { 0x2c } else { 0x16 });
    s.rect_fill(start_btn.0 + 10, start_btn.1 + 7, start_btn.0 + 24,
                start_btn.1 + 21, 0x9fd0ff, 0xe0);
    s.rect_fill(start_btn.0 + 12, start_btn.1 + 9, start_btn.0 + 22,
                start_btn.1 + 14, 0xffffff, 0xd0);

    /* window buttons */
    sh.hover_wb = -1;
    for i in 0..2 {
        if let Some((bx, bw)) = wb_rect(&sh.wins, i) {
            let hovered = mx_c >= bx && mx_c < bx + bw && my_c >= TASKBAR_Y + 5 &&
                          my_c < TASKBAR_Y + TASKBAR_H - 5;
            if hovered { sh.hover_wb = i as i32; }
            let active = sh.focus == i && !sh.wins[i].minimized;
            s.rect_fill(bx, TASKBAR_Y + 6, bx + bw, TASKBAR_Y + TASKBAR_H - 6,
                        0xffffff,
                        if active { 0x30 } else if hovered { 0x20 } else { 0x12 });
            if active {
                s.rect_fill(bx + 14, TASKBAR_Y + TASKBAR_H - 9, bx + bw - 14,
                            TASKBAR_Y + TASKBAR_H - 7, 0x4f8dff, 255);
            }
            s.text(bx + 10, TASKBAR_Y + 12, sh.wins[i].title, 0xe8eefb,
                   if sh.wins[i].minimized { 0x90 } else { 0xf0 }, 1);
        }
    }
    /* clock on the right */
    s.text(SCREEN_W - 96, TASKBAR_Y + 12, "12:00", 0xd6e2f5, 0xd0, 1);

    let t_bar = ticks() - t_bar0;
    // 6. start menu
    if sh.menu_open {
        s.rect_fill(MENU_X + 3, MENU_Y + 10, MENU_X + MENU_W + 3,
                    MENU_Y + MENU_H + 12, 0x000000, 0x44);
        frosted(&mut s, MENU_X, MENU_Y, MENU_X + MENU_W, MENU_Y + MENU_H, 14.0, 0x34);
        s.rect_fill(MENU_X + 10, MENU_Y, MENU_X + MENU_W - 10, MENU_Y + 1,
                    0xffffff, 0x3c);
        s.rect_fill(MENU_X + 10, MENU_Y + MENU_H - 1, MENU_X + MENU_W - 10,
                    MENU_Y + MENU_H, 0xffffff, 0x3c);
        s.rect_fill(MENU_X, MENU_Y + 10, MENU_X + 1, MENU_Y + MENU_H - 10,
                    0xffffff, 0x3c);
        s.rect_fill(MENU_X + MENU_W - 1, MENU_Y + 10, MENU_X + MENU_W,
                    MENU_Y + MENU_H - 10, 0xffffff, 0x3c);
        s.text(MENU_X + 18, MENU_Y + 16, "常用应用", 0xffffff, 0xf0, 1);
        let items = ["系统设置", "任务管理器", "图片查看器", "文本编辑器"];
        for (k, name) in items.iter().enumerate() {
            let y = MENU_Y + 46 + k as i32 * 44;
            let hovered = mx_c >= MENU_X + 10 && mx_c < MENU_X + MENU_W - 10 &&
                          my_c >= y && my_c < y + 38;
            s.rect_fill(MENU_X + 10, y, MENU_X + MENU_W - 10, y + 38, 0xffffff,
                        if hovered { 0x2c } else { 0x12 });
            s.rect_fill(MENU_X + 18, y + 8, MENU_X + 40, y + 30, 0x8fc2ff, 0xd8);
            s.text(MENU_X + 50, y + 11, name, 0xeef4ff, 0xe8, 1);
        }
    }

    sh.prof_frames += 1;
    sh.prof_copy += t_copy;
    sh.prof_win += t_win;
    sh.prof_bar += t_bar;
    if sh.prof_frames >= 30 {
        unsafe {
            console_printf(b"[gui-shell] profile/30: copy=%llu win=%llu bar=%llu ticks\n\0".as_ptr(),
                           sh.prof_copy, sh.prof_win, sh.prof_bar);
        }
        sh.prof_frames = 0;
        sh.prof_copy = 0;
        sh.prof_win = 0;
        sh.prof_bar = 0;
    }

    // 7. cursor
    swrast::cursor(&mut s, mx_c, my_c);

    // 8. present: one row copy per scanline into the real framebuffer
    {
        let dst = core::slice::from_raw_parts_mut(fb, (pitch_px as usize) * hu);
        for y in 0..hu {
            let d = y * (pitch_px as usize);
            let srow = y * wu;
            dst[d..d + wu].copy_from_slice(&s.px[srow..srow + wu]);
        }
    }

    let mut hash: u32 = 0x811c9dc5;
    for v in s.px.iter() {
        hash ^= v & 0x00ff_ffff;
        hash = hash.wrapping_mul(0x0100_0193);
    }
    hash
}

fn draw_window(s: &mut Surface<'_>, w: &Win) {
    /* two-layer soft shadow: plain rect fills (no AA, no sqrt) */
    s.rect_fill(w.x + 2, w.y + 10, w.x + w.w + 2, w.y + w.h + 12, 0x000000, 0x30);
    s.rect_fill(w.x - 2, w.y + 4, w.x + w.w + 6, w.y + w.h + 4, 0x000000, 0x20);

    /* body: frosted acrylic + rounded silhouette */
    frosted(s, w.x, w.y, w.x + w.w, w.y + w.h, 12.0, 0x2a);
    /* hairline border: four 1 px rect fills (no analytic AA pass) */
    s.rect_fill(w.x + 8, w.y, w.x + w.w - 8, w.y + 1, 0xffffff, 0x30);
    s.rect_fill(w.x + 8, w.y + w.h - 1, w.x + w.w - 8, w.y + w.h, 0xffffff, 0x30);
    s.rect_fill(w.x, w.y + 8, w.x + 1, w.y + w.h - 8, 0xffffff, 0x30);
    s.rect_fill(w.x + w.w - 1, w.y + 8, w.x + w.w, w.y + w.h - 8, 0xffffff, 0x30);

    /* title bar: translucent strip + title text + accent dot (plain fill; the
     * rounded top corners come from the body's AA silhouette) */
    s.rect_fill(w.x + 2, w.y + 1, w.x + w.w - 2, w.y + TITLE_H, 0xffffff, 0x14);
    s.round_rect((w.x + 12) as f32, (w.y + 11) as f32, (w.x + 20) as f32,
                 (w.y + 19) as f32, 4.0, w.accent, 0xf0);
    s.text(w.x + 28, w.y + 7, w.title, 0xf2f6ff, 0xf0, 1);

    /* controls */
    let (bx, by, bw, bh) = control_rect(w);
    for k in 0..3 {
        let x = bx + k * (bw + 4);
        s.rect_fill(x, by, x + bw, by + bh, 0xffffff, 0x14);
        if k == 0 {
            s.round_rect((x + 7) as f32, (by + 14) as f32, (x + 15) as f32,
                         (by + 16) as f32, 1.0, 0xdfe7f5, 0xe0);
        } else if k == 1 {
            s.rect_fill(x + 7, by + 7, x + 15, by + 8, 0xdfe7f5, 0xe0);
            s.rect_fill(x + 7, by + 14, x + 15, by + 15, 0xdfe7f5, 0xe0);
            s.rect_fill(x + 7, by + 7, x + 8, by + 15, 0xdfe7f5, 0xe0);
            s.rect_fill(x + 14, by + 7, x + 15, by + 15, 0xdfe7f5, 0xe0);
        } else {
            s.round_rect((x + 8) as f32, (by + 8) as f32, (x + 14) as f32,
                         (by + 9) as f32, 0.0, 0xdfe7f5, 0xe0);
            s.round_rect((x + 8) as f32, (by + 13) as f32, (x + 14) as f32,
                         (by + 14) as f32, 0.0, 0xdfe7f5, 0xe0);
        }
    }

    /* body content: a couple of text lines + a small translucent card */
    s.text(w.x + 18, w.y + TITLE_H + 16, "SutureOS 桌面外壳", 0xdfe7f5, 0xb0, 1);
    let cy = w.y + TITLE_H + 44;
    s.rect_fill(w.x + 16, cy, w.x + w.w - 16, cy + 56, 0xffffff, 0x12);
    s.text(w.x + 28, cy + 12, "圆角 + 轻亚克力 + 微透叠层", 0xcfe0ff, 0xc0, 1);
    s.text(w.x + 28, cy + 32, "窗口可拖动 / 最小化 / 最大化", 0x9fb2cc, 0xb0, 1);
    s.text(w.x + 18, (w.y + w.h - 26) as i32, "窗口管理原型 v0.41", 0x8fa3bd, 0x90, 1);
}
