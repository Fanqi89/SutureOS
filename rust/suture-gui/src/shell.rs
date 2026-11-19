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
const MENU_ITEMS_Y: i32 = MENU_Y + 46;

const WIN_BODY: &str = "SutureOS 桌面外壳";

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
const fn win2() -> Win {
    Win { x: 210, y: 430, w: 360, h: 240, nx: 210, ny: 430, nw: 360, nh: 240,
          minimized: false, maximized: false, z: 3, title: "图片查看器", accent: 0xf5a524, open: false }
}
const fn win3() -> Win {
    Win { x: 560, y: 180, w: 330, h: 230, nx: 560, ny: 180, nw: 330, nh: 230,
          minimized: false, maximized: false, z: 4, title: "文本编辑器", accent: 0xe05a8a, open: false }
}

struct Shell {
    wins: [Win; 4],
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
    menu_sel: i32,
    /* v0.48.0: window open fade-in (frames left) + which window */
    intro_frames: u32,
    intro_idx: i32,
    started: bool,
    frame_ms: u32,
    prof_frames: u32,
    prof_full: u32,
    prof_cache_hit: u32,
    prof_cache_miss: u32,
    prof_copy: u64,
    prof_win: u64,
    prof_bar: u64,
    prof_total: u64,
}

static mut SHELL: Shell = Shell {
    wins: [win0(), win1(), win2(), win3()],
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
    menu_sel: 0,
    intro_frames: 0,
    intro_idx: -1,
    started: false,
    frame_ms: 0,
    prof_frames: 0,
    prof_full: 0,
    prof_cache_hit: 0,
    prof_cache_miss: 0,
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
static mut LAST_STATE: [i32; 20] = [-1; 20];
/* v0.46.0 window layer cache: a window body + its shadow is position-locked and
 * content-static (frosted sampling only depends on the wallpaper), so once drawn
 * it can be blitted. That turns a 400 ms full repaint into a few memcpys for
 * everything except a window that is moving/resizing right now. */
const WC_STRIDE: usize = 512;
const WC_H: usize = 352;
const WC_MARGIN_L: i32 = 8;
const WC_MARGIN_T: i32 = 14;
const WC_MARGIN_R: i32 = 12;
const WC_MARGIN_B: i32 = 16;
static mut WIN_CACHE: [[u32; WC_STRIDE * WC_H]; 4] = [[0; WC_STRIDE * WC_H]; 4];
static mut WIN_KEY: [i32; 4] = [i32::MIN; 4];
static mut WSPLIT: (u64, u64, u64, u32) = (0, 0, 0, 0);
/* v0.47.0: rounded-corner coverage LUT. The analytic path called the
 * soft-float sqrt once per corner pixel; on this no-SSE target that made a
 * window body ~400 ms. 16x16 entries, built once. */
static mut CORNER_COV: [[u8; 16]; 16] = [[0; 16]; 16];
/* v0.45.0: the scene cache (FRAME) no longer contains the cursor. The cursor is
 * composited straight into the framebuffer, so pointer motion only needs to
 * restore the two small rectangles from FRAME and redraw the arrow. This avoids
 * the clipping pitfalls that killed the v0.44 dirty-rect attempt. */
static mut CUR_SEEN: bool = false;
static mut CUR_PX: i32 = -1;
static mut CUR_PY: i32 = -1;
static mut PREV_HOVER: i32 = -2;
static mut PREV_MX: i32 = -1;
static mut PREV_MY: i32 = -1;
static mut CURSOR_FRAMES: u32 = 0;
/* the cursor bitmap's bounding box (see swrast::cursor) */
const CUR_W: i32 = 24;
const CUR_H: i32 = 28;
static mut LAST_SUM: u32 = 0;
static mut IDLE_FRAMES: u32 = 0;

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
                          stride: SCREEN_W as usize, clip: (0, 0, 1024, 768) };
    // wallpaper: 3-stop gradient + two soft colour blobs (the acrylic source)
    s.vgrad3(0, 0, SCREEN_W, SCREEN_H, 0x1b2440, 0x121a2b, 0x090d16, 0.45);
    s.hgrad_round(60.0, 90.0, 500.0, 560.0, 96.0, 0x3f7bff, 0x9a5cff, 0xd0);
    s.hgrad_round(600.0, 60.0, 1000.0, 400.0, 96.0, 0x12b8a6, 0x2f6bff, 0xc0);

    // small mirror of the wallpaper, blurred -> cheap "frosted" source for
    // windows/taskbar/menu (sampled with nearest neighbour, looks right for
    // translucent surfaces)
    let small = &mut *core::ptr::addr_of_mut!(SMALL);
    let mut ss = Surface { px: &mut small[..], w: SMALL_W, h: SMALL_H, stride: SMALL_W, clip: (0, 0, 1024, 768) };
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
    // rounded-corner coverage table (radius up to 12, integer pixel centres)
    {
        let cc = &mut *core::ptr::addr_of_mut!(CORNER_COV);
        let r = 12i32;
        for dy in 0..16usize {
            for dx in 0..16usize {
                let fx = dx as f32 + 0.5;
                let fy = dy as f32 + 0.5;
                let cx = crate::swrast::clampf(fx, r as f32, r as f32);
                let cy = crate::swrast::clampf(fy, r as f32, r as f32);
                let ddx = fx - cx;
                let ddy = fy - cy;
                let d = crate::swrast::fsqrt(ddx * ddx + ddy * ddy) - r as f32;
                let cov = crate::swrast::clampf(0.5 - d, 0.0, 1.0);
                cc[dy][dx] = (cov * 255.0) as u8;
            }
        }
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
    // v0.47.0: the interior (full coverage) is an INTEGER-only loop. It used to
    // evaluate f32 comparisons per pixel; on this target every f32 op is a soft
    // float call (compiler_builtins), which made a 430x300 window cost ~250 ms.
    // Only the rounded-corner band still needs the analytic sqrt coverage.
    let rr = r.max(0.0).min(((x1 - x0) as f32) * 0.5).min(((y1 - y0) as f32) * 0.5);
    let ri = rr as i32;
    let ys = y0.max(0).max(s.clip.1);
    let ye = y1.min(s.h as i32).min(s.clip.3);
    let xs = x0.max(0).max(s.clip.0);
    let xe = x1.min(s.w as i32).min(s.clip.2);

    let in_x0 = x0 + ri;
    let in_x1 = x1 - ri;
    let in_y0 = y0 + ri;
    let in_y1 = y1 - ri;

    for y in ys..ye {
        let inside_v = y >= in_y0 && y < in_y1;
        let row = y as usize * s.stride;
        if inside_v {
            // left band (rounded corner region) then interior then right band
            let li = xs.max(in_x0.min(xe));
            let ri_end = xe.min(in_x1.max(xs));
            for x in xs..li {
                frost_edge_px(s, row, x, y, x0, y0, x1, y1, ri, tint_alpha);
            }
            for x in li..ri_end {
                let base = small_sample(x, y);
                let c = blend_px_raw(base, 0xffffff, tint_alpha);
                s.px[row + x as usize] = c & 0x00ff_ffff;
            }
            for x in ri_end..xe {
                frost_edge_px(s, row, x, y, x0, y0, x1, y1, ri, tint_alpha);
            }
        } else {
            for x in xs..xe {
                frost_edge_px(s, row, x, y, x0, y0, x1, y1, ri, tint_alpha);
            }
        }
    }
}

/// Rounded-corner coverage pixel, using the precomputed LUT (no soft-float sqrt
/// per pixel). `ri` is the corner radius in pixels.
#[inline]
fn frost_edge_px(s: &mut Surface<'_>, row: usize, x: i32, y: i32,
                 x0: i32, y0: i32, x1: i32, y1: i32, ri: i32, tint_alpha: u32) {
    // distance (in whole pixels) from the inner rect, clamped into the LUT
    let dx = if x < x0 + ri {
        x0 + ri - x
    } else if x >= x1 - ri {
        x - (x1 - ri - 1)
    } else {
        0
    };
    let dy = if y < y0 + ri {
        y0 + ri - y
    } else if y >= y1 - ri {
        y - (y1 - ri - 1)
    } else {
        0
    };
    let (ix, iy) = (dx.clamp(0, 15) as usize, dy.clamp(0, 15) as usize);
    let cov255 = unsafe { (&*core::ptr::addr_of!(CORNER_COV))[iy][ix] } as u32;
    if cov255 == 0 {
        return;
    }
    let base = small_sample(x, y);
    let i = y as usize * s.stride + x as usize;
    let dst = s.px[i] & 0x00ff_ffff;
    let c = blend_px_raw(base, 0xffffff, tint_alpha * cov255 / 255);
    s.px[i] = blend_px_raw(dst, c, cov255) & 0x00ff_ffff;
}

fn control_rect(w: &Win) -> (i32, i32, i32, i32) {
    /* title-bar controls: minimise, maximise, close, 22x22 each */
    let y = w.y + (TITLE_H - 22) / 2;
    (w.x + w.w - 3 - 22 * 3 - 2 * 4, y, 22, 22)
}

/// (x, w) of the taskbar window button `i`, or None when hidden.
fn wb_rect(wins: &[Win; 4], i: usize) -> Option<(i32, i32)> {
    let mut x = 118;
    for k in 0..i {
        if wins[k].open { x += 96 + 8; }
    }
    if !wins[i].open { return None; }
    Some((x, 96))
}

const MENU_ITEMS: [&str; 4] =
    ["系统设置", "任务管理器", "图片查看器", "文本编辑器"];

/// Z above every window: the shell only has 4, so a counter is enough.
fn next_z(sh: &Shell) -> i32 {
    let mut m = 0;
    for w in sh.wins.iter() {
        if w.z > m { m = w.z; }
    }
    m + 1
}

/// First visible (open, not minimised) window other than `skip`.
fn next_visible(sh: &Shell, skip: usize) -> Option<usize> {
    for (i, w) in sh.wins.iter().enumerate() {
        if i != skip && w.open && !w.minimized {
            return Some(i);
        }
    }
    None
}

/// Open / restore / focus window `idx` from the start menu (v0.43.0: shared by
/// the mouse click path and the new keyboard Enter path).
fn launch_window(sh: &mut Shell, idx: usize) {
    if idx >= sh.wins.len() {
        return;
    }
    if !sh.wins[idx].open {
        sh.wins[idx].open = true;
        sh.wins[idx].minimized = false;
    }
    sh.wins[idx].minimized = false;
    sh.wins[idx].z = next_z(sh);
    sh.focus = idx;
    sh.menu_open = false;
    sh.intro_frames = 3;
    sh.intro_idx = idx as i32;
    log2(c"menu launch", idx as i32, 0);
}

fn hit_win(wins: &[Win; 4], mx: i32, my: i32) -> i32 {
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
    key: i32,
    clock_hhmm: i32,
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
    let mut s = Surface { px: &mut frame[..], w: wu, h: hu, stride: wu, clip: (0, 0, 1024, 768) };
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
                /* menu rows launch / focus the matching window (v0.42.0) */
                for k in 0..MENU_ITEMS.len() {
                    let ry = MENU_Y + 46 + k as i32 * 44;
                    if cx >= MENU_X + 10 && cx < MENU_X + MENU_W - 10 &&
                       cy >= ry && cy < ry + 38 {
                        launch_window(sh, k);
                    }
                }
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
                for i in 0..4 {
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
                                sh.wins[i].z = next_z(sh);
                            } else if is_focus {
                                sh.wins[i].minimized = true;
                                log2(c"minimize", i as i32, 0);
                                /* focus the other visible window */
                                if let Some(other) = next_visible(sh, i) {
                                    sh.focus = other;
                                }
                            } else {
                                sh.wins[i].minimized = false;
                                sh.focus = i;
                                sh.wins[i].z = next_z(sh);
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
                                if let Some(other) = next_visible(sh, hw as usize) {
                                    sh.focus = other;
                                }
                            } else {
                                sh.wins[hw as usize].open = false;
                                log2(c"close", hw, 0);
                                if let Some(other) = next_visible(sh, hw as usize) {
                                    sh.focus = other;
                                }
                            }
                        } else if cy >= wref.y && cy < wref.y + TITLE_H {
                            /* focus + begin drag */
                            sh.wins[hw as usize].z = next_z(sh);
                            sh.focus = hw as usize;
                            sh.dragging = true;
                            sh.drag_idx = hw as usize;
                            sh.drag_dx = cx - wref.x;
                            sh.drag_dy = cy - wref.y;
                            log2(c"drag begin", hw, cx);
                        } else {
                            sh.wins[hw as usize].z = next_z(sh);
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
    // keyboard: menu navigation (v0.43.0). Scancodes are PS/2 set-1 make codes:
    // up 0x48, down 0x50, enter 0x1C. The kernel keeps polling and passes the
    // last pressed key; ESC is handled by the C loop (it leaves the shell).
    if sh.menu_open {
        match key {
            0x48 => {
                sh.menu_sel -= 1;
                if sh.menu_sel < 0 {
                    sh.menu_sel = MENU_ITEMS.len() as i32 - 1;
                }
                log2(c"menu sel", sh.menu_sel, 0);
            }
            0x50 => {
                sh.menu_sel += 1;
                if sh.menu_sel >= MENU_ITEMS.len() as i32 {
                    sh.menu_sel = 0;
                }
                log2(c"menu sel", sh.menu_sel, 0);
            }
            0x1C => {
                let idx = sh.menu_sel.max(0) as usize;
                if idx < MENU_ITEMS.len() {
                    launch_window(sh, idx);
                }
                log2(c"menu enter", idx as i32, 0);
            }
            _ => {}
        }
    } else if key == 0x50 || key == 0x48 {
        // up/down with the menu closed is ignored (keeps the log honest)
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

    // 0. idle skip (v0.42.0): if nothing that affects the picture changed (no
    // pointer move, no click, no drag, no open ripple), do not repaint at all.
    // At ~2 fps a full repaint is expensive, and re-rendering identical pixels
    // buys nothing.
    let mut st = [0i32; 20];
    st[0] = mx_c;
    st[1] = my_c;
    st[2] = buttons;
    st[3] = click_x;
    st[4] = click_y;
    st[5] = sh.dragging as i32;
    st[6] = sh.menu_open as i32;
    st[7] = sh.focus as i32;
    st[8] = sh.dragging as i32;
    for i in 0..4 {
        let w = sh.wins[i];
        st[9 + i * 2] = w.x ^ (w.y << 10) ^ (w.w << 20) ^ (w.open as i32);
        st[10 + i * 2] = (w.minimized as i32) | ((w.maximized as i32) << 1) |
                         ((w.z & 0xff) << 8);
    }
    let last = unsafe { &*core::ptr::addr_of!(LAST_STATE) };
    if *last == st && sh.intro_frames == 0 {
        IDLE_FRAMES += 1;
        if IDLE_FRAMES == 1 {
            log0(c"idle: frame skipped (nothing changed)");
        }
        return LAST_SUM;
    }
    unsafe {
        let l = &mut *core::ptr::addr_of_mut!(LAST_STATE);
        *l = st;
        IDLE_FRAMES = 0;
    }

    // ---- fast path: only the pointer moved (v0.45.0) ----
    // FRAME holds the scene WITHOUT the cursor, so erasing the old arrow is just
    // a small blit from FRAME, and drawing the new one is a small blit + arrow.
    {
        let hover = hover_code(sh, mx_c, my_c);
        let prev_hover = unsafe { PREV_HOVER };
        let pointer_moved = mx_c != unsafe { PREV_MX } || my_c != unsafe { PREV_MY };
        if click_x < 0 && click_y < 0 && key < 0 && !sh.dragging && pointer_moved &&
           hover == prev_hover && unsafe { CUR_SEEN } {
            let frame = &*core::ptr::addr_of!(FRAME);
            let (px, py) = unsafe { (CUR_PX, CUR_PY) };
            blit_rect(frame, wu, fb, pitch_px as usize,
                      (px - 2, py - 2, px + CUR_W + 2, py + CUR_H + 2));
            blit_rect(frame, wu, fb, pitch_px as usize,
                      (mx_c - 2, my_c - 2, mx_c + CUR_W + 2, my_c + CUR_H + 2));
            draw_cursor(fb, pitch_px as usize, wu, hu, mx_c, my_c);
            unsafe {
                CUR_PX = mx_c;
                CUR_PY = my_c;
                PREV_MX = mx_c;
                PREV_MY = my_c;
                let l = &mut *core::ptr::addr_of_mut!(LAST_STATE);
                *l = st;
                CURSOR_FRAMES += 1;
                if CURSOR_FRAMES == 1 || CURSOR_FRAMES % 60 == 0 {
                    console_printf(b"[gui-shell] cursor: fast path frames=%d\n\0".as_ptr(),
                                   CURSOR_FRAMES as i32);
                }
            }
            return LAST_SUM;
        }
        unsafe { PREV_HOVER = hover; }
    }

    // 4. draw windows bottom-up
    let t_win0 = ticks();
    let mut order = [0usize, 1, 2, 3];
    for a in 0..3 {
        for b in (a + 1)..4 {
            if sh.wins[order[b]].z < sh.wins[order[a]].z {
                order.swap(a, b);
            }
        }
    }
    for &i in order.iter() {
        let win = sh.wins[i];
        if !win.open || win.minimized {
            unsafe {
                WIN_KEY[i] = i32::MIN;
            }
            continue;
        }
        let key = win_key(&win, i);
        let hit = unsafe { WIN_KEY[i] == key };
        // Only reuse a cached layer when no lower window overlaps it: the shadow
        // in the cache was composited against wallpaper alone.
        let mut overlaps = false;
        for &j in order.iter() {
            if j == i || sh.wins[j].z >= win.z || !sh.wins[j].open || sh.wins[j].minimized {
                continue;
            }
            let a = sh.wins[j];
            if a.x - WC_MARGIN_L < win.x + win.w + WC_MARGIN_R &&
               win.x - WC_MARGIN_L < a.x + a.w + WC_MARGIN_R &&
               a.y - WC_MARGIN_T < win.y + win.h + WC_MARGIN_B &&
               win.y - WC_MARGIN_T < a.y + a.h + WC_MARGIN_B {
                overlaps = true;
            }
        }
        let frame = &mut *core::ptr::addr_of_mut!(FRAME);
        if hit && !overlaps {
            if blit_window_cache(frame, wu, i, &win) {
                sh.prof_cache_hit += 1;
                continue;
            }
        }
        {
            let mut cs = Surface {
                px: &mut *core::ptr::addr_of_mut!(FRAME),
                w: wu, h: hu, stride: wu, clip: (0, 0, SCREEN_W, SCREEN_H),
            };
            draw_window(&mut cs, &win);
        }
        sh.prof_cache_miss += 1;
        if !overlaps {
            let frame = &*core::ptr::addr_of!(FRAME);
            save_window_cache(frame, wu, i, &win);
            unsafe {
                WIN_KEY[i] = key;
            }
        }
    }

    /* v0.48.0: 3-frame open fade. Dimming the freshly opened window for a few
     * frames reads as a soft appear and costs one rect fill (~17 ms). */
    if sh.intro_frames > 0 {
        let idx = sh.intro_idx;
        if idx >= 0 && idx < 4 && sh.wins[idx as usize].open {
            let w = sh.wins[idx as usize];
            let a = match sh.intro_frames {
                3 => 0x58u32,
                2 => 0x2cu32,
                _ => 0x12u32,
            };
            s.rect_fill(w.x, w.y, w.x + w.w, w.y + w.h, 0x000000, a);
            log2(c"intro fade", sh.intro_frames as i32, idx);
        }
        sh.intro_frames -= 1;
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
    for i in 0..4 {
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
    /* clock on the right: real wall time from the RTC (v0.44.0) */
    {
        let hh = (clock_hhmm / 100) % 24;
        let mm = clock_hhmm % 100;
        let digits = [b'0' + (hh / 10) as u8, b'0' + (hh % 10) as u8,
                      b':',
                      b'0' + (mm / 10) as u8, b'0' + (mm % 10) as u8];
        let chars = [digits[0] as char, digits[1] as char, digits[2] as char,
                     digits[3] as char, digits[4] as char];
        let mut x = SCREEN_W - 96;
        for ch in chars.iter() {
            x += s.char_ascii(x, TASKBAR_Y + 12, *ch, 0xd6e2f5, 0xd0, 1);
        }
    }

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
        let items = MENU_ITEMS;
        for (k, name) in items.iter().enumerate() {
            let y = MENU_Y + 46 + k as i32 * 44;
            let hovered = mx_c >= MENU_X + 10 && mx_c < MENU_X + MENU_W - 10 &&
                          my_c >= y && my_c < y + 38;
            let selected = k as i32 == sh.menu_sel;
            s.rect_fill(MENU_X + 10, y, MENU_X + MENU_W - 10, y + 38, 0xffffff,
                        if hovered { 0x2c } else if selected { 0x22 } else { 0x12 });
            s.rect_fill(MENU_X + 18, y + 8, MENU_X + 40, y + 30, 0x8fc2ff, 0xd8);
            s.text(MENU_X + 50, y + 11, name, 0xeef4ff, 0xe8, 1);
        }
    }

    sh.prof_frames += 1;
    sh.prof_full += 1;
    sh.prof_copy += t_copy;
    sh.prof_win += t_win;
    sh.prof_bar += t_bar;
    if sh.prof_frames >= 30 {
        unsafe {
            console_printf(b"[gui-shell] profile/30: full=%d hit=%d miss=%d win=%llu bar=%llu ticks\n\0".as_ptr(),
                           sh.prof_full as i32, sh.prof_cache_hit as i32,
                           sh.prof_cache_miss as i32, sh.prof_win, sh.prof_bar);
        }
        sh.prof_frames = 0;
        sh.prof_full = 0;
        sh.prof_cache_hit = 0;
        sh.prof_cache_miss = 0;
        sh.prof_copy = 0;
        sh.prof_win = 0;
        sh.prof_bar = 0;
    }

    // 7. present the scene (no cursor in FRAME), then composite the cursor
    {
        let frame = &*core::ptr::addr_of!(FRAME);
        blit_rect(frame, wu, fb, pitch_px as usize, (0, 0, SCREEN_W, SCREEN_H));
        draw_cursor(fb, pitch_px as usize, wu, hu, mx_c, my_c);
        unsafe {
            CUR_SEEN = true;
            CUR_PX = mx_c;
            CUR_PY = my_c;
            PREV_MX = mx_c;
            PREV_MY = my_c;
        }
    }

    let mut hash: u32 = 0x811c9dc5;
    for v in s.px.iter() {
        hash ^= v & 0x00ff_ffff;
        hash = hash.wrapping_mul(0x0100_0193);
    }
    LAST_SUM = hash;
    hash
}

/// Code describing what the pointer hovers; any change needs the real painter.
fn hover_code(sh: &Shell, mx: i32, my: i32) -> i32 {
    let start = (8, TASKBAR_Y + 5, 34, 28);
    if mx >= start.0 && mx < start.0 + start.2 && my >= start.1 && my < start.1 + start.3 {
        return 100;
    }
    for i in 0..4 {
        if let Some((bx, bw)) = wb_rect(&sh.wins, i) {
            if mx >= bx && mx < bx + bw && my >= TASKBAR_Y + 5 && my < TASKBAR_Y + TASKBAR_H - 5 {
                return 200 + i as i32;
            }
        }
    }
    if sh.menu_open && mx >= MENU_X + 10 && mx < MENU_X + MENU_W - 10 &&
       my >= MENU_ITEMS_Y && my < MENU_ITEMS_Y + MENU_ITEMS.len() as i32 * 44 {
        return 300 + (my - MENU_ITEMS_Y) / 44;
    }
    let hw = hit_win(&sh.wins, mx, my);
    if hw >= 0 {
        let w = sh.wins[hw as usize];
        let (bx, _by, bw, _bh) = control_rect(&w);
        if my >= w.y && my < w.y + TITLE_H {
            if mx >= bx && mx < bx + 3 * bw + 8 {
                return 400 + hw * 4 + (mx - bx) / (bw + 4);
            }
            return 500 + hw;
        }
        return 600 + hw;
    }
    0
}

/// Copy a rectangle of the scene cache into the framebuffer.
fn blit_rect(frame: &[u32], stride: usize, fb: *mut u32, pitch_px: usize,
             r: (i32, i32, i32, i32)) {
    let x0 = r.0.max(0) as usize;
    let y0 = r.1.max(0) as usize;
    let x1 = (r.2.max(0) as usize).min(SCREEN_W as usize);
    let y1 = (r.3.max(0) as usize).min(SCREEN_H as usize);
    if x1 <= x0 || y1 <= y0 {
        return;
    }
    let dst = unsafe { core::slice::from_raw_parts_mut(fb, pitch_px * SCREEN_H as usize) };
    for y in y0..y1 {
        let d = y * pitch_px + x0;
        let srow = y * stride + x0;
        dst[d..d + (x1 - x0)].copy_from_slice(&frame[srow..srow + (x1 - x0)]);
    }
}

/// Composite the arrow cursor directly into the framebuffer.
fn draw_cursor(fb: *mut u32, pitch_px: usize, w: usize, h: usize, x: i32, y: i32) {
    let px = unsafe { core::slice::from_raw_parts_mut(fb, pitch_px * h) };
    let mut s = Surface { px, w, h, stride: pitch_px, clip: (0, 0, SCREEN_W, SCREEN_H) };
    swrast::cursor(&mut s, x, y);
}

/// Cache key for a window layer: only geometry matters (title/accent/body are
/// static per window index, and focus is not drawn differently).
fn win_key(w: &Win, slot: usize) -> i32 {
    w.x.wrapping_mul(7919)
        .wrapping_add(w.y.wrapping_mul(104_729))
        .wrapping_add(w.w.wrapping_mul(31))
        .wrapping_add(w.h.wrapping_mul(17))
        .wrapping_add(slot as i32)
}

/// Blit a cached window layer (body + shadow) into the scene cache.
fn blit_window_cache(scene: &mut [u32], stride: usize, slot: usize, w: &Win) -> bool {
    let cw = (w.w + WC_MARGIN_L + WC_MARGIN_R) as usize;
    let ch = (w.h + WC_MARGIN_T + WC_MARGIN_B) as usize;
    if cw > WC_STRIDE || ch > WC_H || cw == 0 || ch == 0 {
        return false;
    }
    let cache = unsafe { &(&*core::ptr::addr_of!(WIN_CACHE))[slot] };
    let dx0 = (w.x - WC_MARGIN_L).max(0) as usize;
    let dy0 = (w.y - WC_MARGIN_T).max(0) as usize;
    let dx1 = ((w.x - WC_MARGIN_L + cw as i32) as usize).min(SCREEN_W as usize);
    let dy1 = ((w.y - WC_MARGIN_T + ch as i32) as usize).min(SCREEN_H as usize);
    if dx1 <= dx0 || dy1 <= dy0 {
        return true;
    }
    let ox = (dx0 as i32 - (w.x - WC_MARGIN_L)) as usize;
    let oy = (dy0 as i32 - (w.y - WC_MARGIN_T)) as usize;
    for y in dy0..dy1 {
        let d = y * stride + dx0;
        let srow = (y - dy0 + oy) * WC_STRIDE + ox;
        let n = dx1 - dx0;
        scene[d..d + n].copy_from_slice(&cache[srow..srow + n]);
    }
    true
}

/// Store the just-drawn window layer (body + shadow) into its cache slot.
fn save_window_cache(scene: &[u32], stride: usize, slot: usize, w: &Win) {
    let cw = (w.w + WC_MARGIN_L + WC_MARGIN_R) as usize;
    let ch = (w.h + WC_MARGIN_T + WC_MARGIN_B) as usize;
    if cw > WC_STRIDE || ch > WC_H || cw == 0 || ch == 0 {
        return;
    }
    let cache = unsafe { &mut (&mut *core::ptr::addr_of_mut!(WIN_CACHE))[slot] };
    for row in 0..ch {
        let y = w.y - WC_MARGIN_T + row as i32;
        let base = row * WC_STRIDE;
        if y < 0 || y >= SCREEN_H {
            for col in 0..cw {
                cache[base + col] = 0;
            }
            continue;
        }
        let rowbase = y as usize * stride;
        for col in 0..cw {
            let x = w.x - WC_MARGIN_L + col as i32;
            cache[base + col] = if x < 0 || x >= SCREEN_W {
                0
            } else {
                scene[rowbase + x as usize]
            };
        }
    }
}

fn draw_window(s: &mut Surface<'_>, w: &Win) {
    let t_all = ticks();
    /* two-layer soft shadow: plain rect fills (no AA, no sqrt) */
    s.rect_fill(w.x + 2, w.y + 10, w.x + w.w + 2, w.y + w.h + 12, 0x000000, 0x30);
    s.rect_fill(w.x - 2, w.y + 4, w.x + w.w + 6, w.y + w.h + 4, 0x000000, 0x20);
    let t_shadow = ticks() - t_all;

    /* body: frosted acrylic + rounded silhouette */
    frosted(s, w.x, w.y, w.x + w.w, w.y + w.h, 12.0, 0x2a);
    let t_frost = ticks() - t_all - t_shadow;
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
    s.text(w.x + 18, w.y + TITLE_H + 16, WIN_BODY, 0xdfe7f5, 0xb0, 1);
    let cy = w.y + TITLE_H + 44;
    s.rect_fill(w.x + 16, cy, w.x + w.w - 16, cy + 56, 0xffffff, 0x12);
    s.text(w.x + 28, cy + 12, "圆角 + 轻亚克力 + 微透叠层", 0xcfe0ff, 0xc0, 1);
    s.text(w.x + 28, cy + 32, "窗口可拖动 / 最小化 / 最大化", 0x9fb2cc, 0xb0, 1);
    s.text(w.x + 18, (w.y + w.h - 26) as i32, "窗口管理原型 v0.48", 0x8fa3bd, 0x90, 1);
    unsafe {
        let d = &mut *core::ptr::addr_of_mut!(WSPLIT);
        d.0 += t_shadow;
        d.1 += t_frost;
        d.2 += ticks() - t_all - t_shadow - t_frost;
        d.3 += 1;
        if d.3 >= 4 {
            console_printf(b"[gui-shell] wsplit shadow=%d frosted=%d chrome=%d ticks (x4)\n\0".as_ptr(),
                           d.0 as i32, d.1 as i32, d.2 as i32);
            *d = (0, 0, 0, 0);
        }
    }
}
