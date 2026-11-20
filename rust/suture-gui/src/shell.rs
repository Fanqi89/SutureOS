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

/* v0.58.0: soft shadow ring around a window body, one alpha per pixel of
 * distance (1 px out ... 8 px out). The shadow is composited against whatever is
 * already in the scene, so it darkens the wallpaper, a window below it or a
 * neighbour's acrylic band instead of painting the hard black halo the baked
 * layer used to leave behind. */
const SHADOW_RING: [u32; 8] = [0x22, 0x1c, 0x16, 0x10, 0x0b, 0x07, 0x04, 0x02];

/* v0.59.0: the shared placeholder body text is gone - every window paints its
 * own application content through crate::apps::draw_body. */

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
    /* v0.51.0: where the frosted acrylic samples the blurred wallpaper. Window
     * layers are rendered at their HOME anchor with this offset, so a layer can be
     * blitted anywhere without re-rasterising (compositor semantics). */
    sox: i32,
    soy: i32,
    /* v0.52.0: acrylic sampling anchor, fixed for the window's whole life so the
     * glass pattern never changes when the window moves (no re-anchor pop). */
    ax: i32,
    ay: i32,
}

const fn win0() -> Win {
    Win { x: 150, y: 120, w: 430, h: 300, nx: 150, ny: 120, nw: 430, nh: 300,
          minimized: false, maximized: false, z: 1, title: "系统设置", accent: 0x4f8dff, open: true, sox: 0, soy: 0, ax: 150, ay: 120 }
}
const fn win1() -> Win {
    Win { x: 470, y: 300, w: 400, h: 260, nx: 470, ny: 300, nw: 400, nh: 260,
          minimized: false, maximized: false, z: 2, title: "任务管理器", accent: 0x22c1a8, open: true, sox: 0, soy: 0, ax: 470, ay: 300 }
}
const fn win2() -> Win {
    Win { x: 210, y: 430, w: 360, h: 240, nx: 210, ny: 430, nw: 360, nh: 240,
          minimized: false, maximized: false, z: 3, title: "图片查看器", accent: 0xf5a524, open: false, sox: 0, soy: 0, ax: 210, ay: 430 }
}
const fn win3() -> Win {
    Win { x: 560, y: 180, w: 330, h: 230, nx: 560, ny: 180, nw: 330, nh: 230,
          minimized: false, maximized: false, z: 4, title: "文本编辑器", accent: 0xe05a8a, open: false, sox: 0, soy: 0, ax: 560, ay: 180 }
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
    /* v0.57.1: logo shutdown animation (S key): frame counter while playing */
    shutting: bool,
    shut_frame: u32,
    /* v0.48.0: window open fade-in (frames left) + which window */
    intro_frames: u32,
    intro_idx: i32,
    menu_intro: u32,
    /* v0.49.0: deferred close/minimize with a 2-frame fade-out */
    act_frames: u32,
    act_kind: i32,
    act_idx: i32,
    started: bool,
    frame_ms: u32,
    prof_frames: u32,
    prof_full: u32,
    prof_cache_hit: u32,
    prof_taskbar_hit: u32,
    prof_drag_reuse: u32,
    drag_frames_logged: u32,
    /* v0.50.0: previous dragged-window position (for partial present) */
    drag_last_x: i32,
    drag_last_y: i32,
    prof_cache_miss: u32,
    prof_copy: u64,
    prof_win: u64,
    prof_bar: u64,
    prof_total: u64,
    /* v0.58.0: per-window keyboard focus. Every window owns a small editable
     * line; printable keys go to the focused window, Enter commits it to the
     * serial log, Backspace deletes one character. */
    input: [[u8; 33]; 4],
    ilen: [u32; 4],
    /* v0.58.0: true maximise/restore transition. `scale_from`/`scale_to` are
     * lerped over SCALE_TOTAL frames; the geometry itself already sits at its
     * target so hit-testing stays honest during the animation. */
    scale_frames: u32,
    scale_idx: i32,
    scale_from: [i32; 4],
    scale_to: [i32; 4],
    scale_kind: i32,
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
    shutting: false,
    shut_frame: 0,
    intro_frames: 0,
    intro_idx: -1,
    menu_intro: 0,
    act_frames: 0,
    act_kind: 0,
    act_idx: -1,
    started: false,
    frame_ms: 0,
    prof_frames: 0,
    prof_full: 0,
    prof_cache_hit: 0,
    prof_taskbar_hit: 0,
    prof_drag_reuse: 0,
    drag_frames_logged: 0,
    drag_last_x: -1,
    drag_last_y: -1,
    prof_cache_miss: 0,
    prof_copy: 0,
    prof_win: 0,
    prof_bar: 0,
    prof_total: 0,
    input: [[0; 33]; 4],
    ilen: [0; 4],
    scale_frames: 0,
    scale_idx: -1,
    scale_from: [0; 4],
    scale_to: [0; 4],
    scale_kind: 0,
};

// ---------------------------------------------------------------- caches
static mut WALL: [u32; (SCREEN_W as usize) * (SCREEN_H as usize)] =
    [0; (SCREEN_W as usize) * (SCREEN_H as usize)];
static mut SMALL: [u32; SMALL_W * SMALL_H] = [0; SMALL_W * SMALL_H];
static mut FRAME: [u32; (SCREEN_W as usize) * (SCREEN_H as usize)] =
    [0; (SCREEN_W as usize) * (SCREEN_H as usize)];
/// v0.60.0: second scene buffer for the swap chain (back buffer while FRAME is
/// the front). 3 MiB of .bss - the ELF file does not grow.
static mut FRAME_B: [u32; (SCREEN_W as usize) * (SCREEN_H as usize)] =
    [0; (SCREEN_W as usize) * (SCREEN_H as usize)];
/// Which buffer is on the framebuffer: 0 = FRAME, 1 = FRAME_B.
static mut FRONT: usize = 0;
/* ------------------------------------------------------- present / swap chain
 * v0.60.0: explicit double-buffered present path.
 *
 * The shell always renders into the BACK scene buffer; a present swaps it to the
 * front and copies the front into the framebuffer. The front buffer therefore
 * always holds exactly the frame that is on screen, which gives two things:
 *   - a screendump can never catch a half-painted frame (the old single buffer
 *     already presented in one blit, so this keeps that property);
 *   - a frame can be dropped without corrupting the image, which is what the
 *     vsync gate below uses.
 *
 * The kernel only exposes the 100 Hz tick, so "vsync" is emulated as "at most one
 * present per tick" (VSYNC_MS = 10). A frame that arrives earlier is still
 * rendered, but its present is coalesced (skipped += 1) and stays pending until
 * a later due frame (or the idle flush) shows it.
 */
pub const VSYNC_MS: u32 = 10;

#[derive(Clone, Copy)]
pub struct SwapState {
    /// generation id of the front buffer; +1 on every present
    pub generation: u32,
    /// presents performed (one present == one framebuffer update)
    pub presents: u32,
    /// rendered frames whose present was coalesced by the vsync gate
    pub skipped: u32,
    /// scene frames rendered, whether presented or coalesced
    pub produced: u32,
    /// presents that swapped a freshly rendered scene (not a cursor-only update)
    pub scene_presents: u32,
    /// now_ms of the last present
    pub last_present_ms: u32,
}

static mut SWAP: SwapState = SwapState {
    generation: 0,
    presents: 0,
    skipped: 0,
    produced: 0,
    scene_presents: 0,
    last_present_ms: 0,
};
static mut SWAP_STARTED: bool = false;
static mut SWAP_IVL_SUM: u32 = 0;
static mut SWAP_IVL_N: u32 = 0;
/// A rendered scene is waiting for its (coalesced) present.
static mut SWAP_PENDING: bool = false;

/// The scene buffer that is currently on the framebuffer.
#[inline]
unsafe fn scene_front() -> &'static [u32] {
    if FRONT == 0 {
        &*core::ptr::addr_of!(FRAME)
    } else {
        &*core::ptr::addr_of!(FRAME_B)
    }
}

#[inline]
unsafe fn scene_front_mut() -> &'static mut [u32] {
    if FRONT == 0 {
        &mut *core::ptr::addr_of_mut!(FRAME)
    } else {
        &mut *core::ptr::addr_of_mut!(FRAME_B)
    }
}

/// The scene buffer being rendered (always the one that is NOT displayed).
#[inline]
unsafe fn scene_back_mut() -> &'static mut [u32] {
    if FRONT == 0 {
        &mut *core::ptr::addr_of_mut!(FRAME_B)
    } else {
        &mut *core::ptr::addr_of_mut!(FRAME)
    }
}

/// True when a present is allowed: at least one 10 ms tick since the last one.
#[inline]
unsafe fn swap_due(now_ms: u32) -> bool {
    !SWAP_STARTED || now_ms.wrapping_sub(SWAP.last_present_ms) >= VSYNC_MS
}

/// Account for one framebuffer update (scene swap or cursor-only overlay).
#[inline]
unsafe fn swap_commit(now_ms: u32, scene: bool) {
    let st = &mut *core::ptr::addr_of_mut!(SWAP);
    if SWAP_STARTED {
        let ivl = now_ms.wrapping_sub(st.last_present_ms);
        SWAP_IVL_SUM = SWAP_IVL_SUM.saturating_add(ivl);
        SWAP_IVL_N += 1;
    }
    SWAP_STARTED = true;
    st.generation = st.generation.wrapping_add(1);
    st.presents = st.presents.wrapping_add(1);
    st.last_present_ms = now_ms;
    if scene {
        st.scene_presents = st.scene_presents.wrapping_add(1);
    }
}

/// Log the swap-chain counters once (`guishell swapstat`). Returns `presents`.
pub unsafe fn swap_report() -> u32 {
    let st = &*core::ptr::addr_of!(SWAP);
    let avg = if SWAP_IVL_N > 0 { SWAP_IVL_SUM / SWAP_IVL_N } else { 0 };
    console_printf(
        b"[gui-shell] swapstat presents=%d gens=%d skipped=%d interval_ms=%d rendered=%d scene=%d\n\0"
            .as_ptr(),
        st.presents as i32,
        st.generation as i32,
        st.skipped as i32,
        avg as i32,
        st.produced as i32,
        st.scene_presents as i32,
    );
    st.presents
}

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
/* v0.50.0: the taskbar strip (frosted band + buttons + clock) only changes when a
 * window opens/closes/minimises, focus moves, the pointer hovers a button, or the
 * clock ticks. Cache it so a drag repaint is a blit instead of a re-render. */
const TB_H: usize = 38;
static mut TASKBAR_CACHE: [u32; 1024 * TB_H] = [0; 1024 * TB_H];
static mut TASKBAR_KEY: i32 = i32::MIN;
/* v0.58.0: the cached window body size. The layer is normally blitted 1:1, but
 * the maximise/restore transition samples it scaled, so the source size has to
 * be known even after the window geometry already jumped to its target. */
static mut LAYER_WH: [(i32, i32); 4] = [(0, 0); 4];
/* v0.58.0: nearest-neighbour index tables for the transition pass (one division
 * per column/row instead of one per pixel). */
static mut SCALED_X: [u16; 1024] = [0; 1024];
static mut SCALED_Y: [u16; 768] = [0; 768];
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
    frosted_at(s, x0, y0, x1, y1, r, tint_alpha, 0, 0);
}

/// `frosted` with an explicit wallpaper sampling anchor `(x + sx, y + sy)`.
/// A window layer is rendered at its home anchor and later blitted anywhere, so
/// the sample offset is what keeps the acrylic look stable while dragging.
fn frosted_at(s: &mut Surface<'_>, x0: i32, y0: i32, x1: i32, y1: i32, r: f32,
              tint_alpha: u32, sx: i32, sy: i32) {
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
                frost_edge_px(s, row, x, y, x0, y0, x1, y1, ri, tint_alpha, sx, sy);
            }
            for x in li..ri_end {
                let base = small_sample(x + sx, y + sy);
                let c = blend_px_raw(base, 0xffffff, tint_alpha);
                s.px[row + x as usize] = c & 0x00ff_ffff;
            }
            for x in ri_end..xe {
                frost_edge_px(s, row, x, y, x0, y0, x1, y1, ri, tint_alpha, sx, sy);
            }
        } else {
            for x in xs..xe {
                frost_edge_px(s, row, x, y, x0, y0, x1, y1, ri, tint_alpha, sx, sy);
            }
        }
    }
}

/// Rounded-corner coverage pixel, using the precomputed LUT (no soft-float sqrt
/// per pixel). `ri` is the corner radius in pixels.
#[inline]
fn frost_edge_px(s: &mut Surface<'_>, row: usize, x: i32, y: i32,
                 x0: i32, y0: i32, x1: i32, y1: i32, ri: i32, tint_alpha: u32,
                 sx: i32, sy: i32) {
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
    let base = small_sample(x + sx, y + sy);
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

/* v0.59.0: one accent per app, shared by the start-menu row, the window title
 * bar icon and the pseudo-3D plate (matches Win::accent). */
const APP_ACCENTS: [u32; 4] = [0x4f8dff, 0x22c1a8, 0xf5a524, 0xe05a8a];

const MENU_ITEMS: [&str; 4] =
    ["系统设置", "任务管理器", "图片查看器", "文本编辑器"];

/// Z above every window: the shell only has 4, so a counter is enough.
static mut Z_COUNTER: i32 = 100;

fn next_z(sh: &Shell) -> i32 {
    /* v0.57.1: a monotonic counter, not "max+1": a menu launch and a focus bump
     * in the same frame used to be able to produce the SAME z value, after which
     * hit_win() routed a close-button click to the window underneath. */
    let _ = sh;
    unsafe {
        Z_COUNTER += 1;
        Z_COUNTER
    }
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
/// Start the fade-out that precedes a close/minimize (v0.49.0).
/// kind: 1 = minimize, 2 = close.
fn begin_act(sh: &mut Shell, kind: i32, idx: usize) {
    if idx >= sh.wins.len() || !sh.wins[idx].open || sh.wins[idx].minimized {
        return;
    }
    sh.act_frames = 2;
    sh.act_kind = kind;
    sh.act_idx = idx as i32;
    log2(if kind == 1 { c"anim minimize" } else { c"anim close" }, idx as i32, 0);
}

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

/// v0.58.0: start a maximise/restore transition. The geometry is applied
/// immediately (the taskbar, hit-testing and the layer key follow the target
/// state) while `scale_from`/`scale_to` drive the visual interpolation, because
/// the layer cache only ever holds the normal-size body.
fn request_maximize(sh: &mut Shell, idx: usize, want: bool) {
    if idx >= sh.wins.len() {
        return;
    }
    let from = {
        let w = sh.wins[idx];
        [w.x, w.y, w.w, w.h]
    };
    let z = next_z(sh);
    let to;
    if want {
        let w = &mut sh.wins[idx];
        w.nx = from[0];
        w.ny = from[1];
        w.nw = from[2];
        w.nh = from[3];
        w.maximized = true;
        w.x = 0;
        w.y = 0;
        w.w = SCREEN_W;
        w.h = TASKBAR_Y;
        to = [0, 0, SCREEN_W, TASKBAR_Y];
    } else {
        let w = &mut sh.wins[idx];
        w.maximized = false;
        w.x = w.nx;
        w.y = w.ny;
        w.w = w.nw;
        w.h = w.nh;
        to = [w.nx, w.ny, w.nw, w.nh];
    }
    sh.wins[idx].z = z;
    sh.wins[idx].minimized = false;
    sh.focus = idx;
    sh.scale_from = from;
    sh.scale_to = to;
    sh.scale_frames = 4;
    sh.scale_idx = idx as i32;
    sh.scale_kind = if want { 1 } else { 2 };
    log2(c"scale begin", idx as i32, sh.scale_kind);
    log2(if want { c"maximize" } else { c"restore" }, idx as i32, 0);
}

/// v0.58.0: composite the cached window body sampled into an arbitrary rectangle
/// (nearest neighbour, opaque). Used by the maximise/restore transition.
fn composite_scaled(scene: &mut [u32], stride: usize, slot: usize, r: &Win) -> bool {
    if slot >= 4 {
        return false;
    }
    let (sw, shh) = unsafe { (&*core::ptr::addr_of!(LAYER_WH))[slot] };
    if sw <= 0 || shh <= 0 || r.w <= 0 || r.h <= 0
        || r.w > SCREEN_W || r.h > SCREEN_H || r.x < 0 || r.y < 0
    {
        return false;
    }
    let xmap = unsafe { &mut *core::ptr::addr_of_mut!(SCALED_X) };
    let ymap = unsafe { &mut *core::ptr::addr_of_mut!(SCALED_Y) };
    for i in 0..r.w as usize {
        xmap[i] = ((i as i64 * sw as i64) / r.w as i64) as u16;
    }
    for j in 0..r.h as usize {
        ymap[j] = ((j as i64 * shh as i64) / r.h as i64) as u16;
    }
    let cache = unsafe { &(&*core::ptr::addr_of!(WIN_CACHE))[slot] };
    let x0 = r.x.max(0);
    let y0 = r.y.max(0);
    let x1 = (r.x + r.w).min(SCREEN_W);
    let y1 = (r.y + r.h).min(SCREEN_H);
    if x1 <= x0 || y1 <= y0 {
        return true;
    }
    for dy in y0..y1 {
        let sy = ymap[(dy - r.y) as usize] as usize + WC_MARGIN_T as usize;
        let srow = sy * WC_STRIDE;
        let drow = dy as usize * stride;
        for dx in x0..x1 {
            let sx = xmap[(dx - r.x) as usize] as usize + WC_MARGIN_L as usize;
            scene[drow + dx as usize] = cache[srow + sx];
        }
    }
    true
}

fn hit_win(wins: &[Win; 4], mx: i32, my: i32, focus: i32) -> i32 {
    let mut best = -1;
    let mut bestz = -1;
    for (i, w) in wins.iter().enumerate() {
        if !w.open || w.minimized { continue; }
        if mx >= w.x && mx < w.x + w.w && my >= w.y && my < w.y + w.h {
            if w.z > bestz || (w.z == bestz && i as i32 == focus) {
                best = i as i32;
                bestz = w.z;
            }
        }
    }
    best
}

// ---------------------------------------------------------------- logo assets
extern "C" {
    static g_logo24: [u32; 24 * 24];
    static g_logo48: [u32; 48 * 48];
    static g_logo768: [u32; 768 * 768];
    fn suture_power_off();
}

const LOGO_SMALL_SIZE: usize = 24;
const LOGO_MENU_SIZE: usize = 48;
const LOGO_FULL_SIZE: usize = 768;
const LOGO_TILE: i32 = 256;          /* 3x3 tiles of the 768px logo */

/// Copy a rectangular region of a generated logo bitmap to the scene (1:1).
/// v0.59.0: the bitmaps are 0xAARRGGBB — the logo ladder has a transparent
/// background, so transparent pixels leave the surface alone and partial alpha
/// is blended (the old code kept the PNG's white RGB and drew a white square).
fn blit_logo_region(s: &mut Surface, data: &[u32], lw: usize,
                    sx: i32, sy: i32, tw: i32, th: i32, dx: i32, dy: i32) {
    for row in 0..th {
        let dst_y = dy + row;
        if dst_y < 0 || dst_y >= SCREEN_H { continue; }
        for col in 0..tw {
            let dst_x = dx + col;
            if dst_x < 0 || dst_x >= SCREEN_W { continue; }
            let src = data[((sy + row) as usize) * lw + (sx + col) as usize];
            let a = (src >> 24) & 0xff;
            if a == 0 {
                continue;
            }
            let i = (dst_y as usize) * s.stride + dst_x as usize;
            if a >= 255 {
                s.px[i] = src & 0x00ff_ffff;
            } else {
                let d = s.px[i] & 0x00ff_ffff;
                s.px[i] = blend_px_raw(d, src & 0x00ff_ffff, a);
            }
        }
    }
}

/// One frame of the shutdown animation: dim the desktop, draw the 768px logo,
/// then hide its 3x3 tiles in reverse order (tile 8 first) - the reverse of the
/// boot reveal described in logo/README.md, faster (10 frames ~= 0.7 s at the
/// shell's frame rate), and it does not wait for the animation to finish.
unsafe fn shutdown_step(fb: *mut u32, w: i32, h: i32, pitch_px: i32,
                        sh: &mut Shell, mx: i32, my: i32) -> u32 {
    let (wu, hu) = (w as usize, h as usize);
    /* v0.60.0: this animation dims the frame it is about to show, so it paints
     * straight into the FRONT buffer (which holds the last presented desktop)
     * and blits it unconditionally - no swap happens on the shutdown path. */
    let frame = scene_front_mut();
    {
        let mut s = Surface { px: &mut frame[..], w: wu, h: hu, stride: wu,
                              clip: (0, 0, 1024, 768) };
        s.rect_fill(0, 0, SCREEN_W, SCREEN_H, 0x000000, 0xd8);
        let hidden = sh.shut_frame.min(10);
        for ty in 0..3 {
            for tx in 0..3 {
                let idx = ty * 3 + tx;
                if (idx as u32) < 9u32.saturating_sub(hidden) {
                    blit_logo_region(&mut s, &g_logo768, LOGO_FULL_SIZE,
                                     tx * LOGO_TILE, ty * LOGO_TILE,
                                     LOGO_TILE, LOGO_TILE,
                                     128 + tx * LOGO_TILE, ty * LOGO_TILE);
                }
            }
        }
    }
    {
        let frame = scene_front();
        blit_rect(frame, wu, fb, pitch_px as usize, (0, 0, SCREEN_W, SCREEN_H));
        draw_cursor(fb, pitch_px as usize, wu, hu, mx.clamp(0, w - 1), my.clamp(0, h - 1));
    }
    console_printf(b"[gui-shell] logo shutdown %d\n\0".as_ptr(), sh.shut_frame as i32);
    sh.shut_frame += 1;
    if sh.shut_frame >= 10 {
        log0(c"logo shutdown done");
        suture_power_off();
        log0(c"logo shutdown: power_off returned (failure)");
    }
    0
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
    ch: i32,
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
    /* v0.60.0: swap chain - every frame is rendered into the BACK scene buffer
     * and only becomes visible when the present block at the end swaps it to the
     * front. Cached pieces (window layers, taskbar strip) are composited into
     * this buffer exactly like before. */
    let frame = scene_back_mut();
    let mut s = Surface { px: &mut frame[..], w: wu, h: hu, stride: wu, clip: (0, 0, 1024, 768) };
    let sh = &mut *core::ptr::addr_of_mut!(SHELL);
    if !sh.started {
        sh.started = true;
        sh.frame_ms = now_ms;
        log0(c"shell: loop started");
    }
    /* v0.57.1: logo shutdown animation runs before (and instead of) the desktop
     * render; it ends by powering the machine off. */
    if sh.shutting {
        return shutdown_step(fb, w, h, pitch_px, sh, mx, my);
    }

    // 1. wallpaper from cache
    let t_frame0 = ticks();
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
    let mut hw = hit_win(&sh.wins, mx_c, my_c, sh.focus as i32);
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
        /* v0.57.1: hit-test the click at the click coordinates, not at the current
         * pointer: a click can be consumed while PS/2 move packets are still in
         * flight, which sent close-button clicks to the stale pointer's window. */
        let hw = hit_win(&sh.wins, click_x, click_y, sh.focus as i32);
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
                if sh.menu_open {
                    sh.menu_intro = 2;
                }
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
                                /* v0.58.0: true transition instead of a pop */
                                let want = !sh.wins[i].maximized;
                                request_maximize(sh, i, want);
                            } else if is_focus {
                                begin_act(sh, 1, i);
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
                                let want = !sh.wins[hw as usize].maximized;
                                request_maximize(sh, hw as usize, want);
                            } else if idx == 0 {
                                begin_act(sh, 1, hw as usize);
                            } else {
                                begin_act(sh, 2, hw as usize);
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
    } else if key == 0x44 && !sh.shutting {
        /* F10 = shutdown: play the reverse logo animation, then power off.
         * (v0.58.0 moved this off 'S': the shell now accepts typed text, so
         * every printable key has to reach the focused window.) */
        sh.shutting = true;
        sh.shut_frame = 0;
        log0(c"logo shutdown start");
    } else if key == 0x50 || key == 0x48 {
        // up/down with the menu closed is ignored (keeps the log honest)
    }

    /* v0.58.0: per-window keyboard focus. Tab cycles the window focus, printable
     * keys edit the focused window's line, Enter commits it to the log and
     * Backspace deletes one character. Arrow keys stay with the start menu
     * (handled above); F10 stays the shutdown key. */
    if !sh.menu_open && !sh.shutting {
        let fi = sh.focus;
        if ch == 0x09 {
            let mut n = fi;
            for _ in 0..4 {
                n = (n + 1) % 4;
                if sh.wins[n].open && !sh.wins[n].minimized {
                    break;
                }
            }
            if n != fi {
                sh.wins[n].z = next_z(sh);
                sh.focus = n;
                log2(c"focus cycle", n as i32, 0);
            }
        } else if sh.wins[fi].open && !sh.wins[fi].minimized {
            let len = sh.ilen[fi] as usize;
            if ch == 0x0a || ch == 0x0d {
                if len > 0 {
                    let buf = &sh.input[fi];
                    console_printf(b"[gui-shell] input win=%d text=%s\n\0".as_ptr(),
                                   fi as i32, buf.as_ptr());
                    log2(c"input commit", fi as i32, len as i32);
                    sh.ilen[fi] = 0;
                    sh.input[fi][0] = 0;
                }
            } else if ch == 0x08 {
                if len > 0 {
                    sh.ilen[fi] -= 1;
                    sh.input[fi][len - 1] = 0;
                    log2(c"input backspace", fi as i32, (len - 1) as i32);
                }
            } else if ch >= 0x20 && ch < 0x7f && len < 32 {
                sh.input[fi][len] = ch as u8;
                sh.input[fi][len + 1] = 0;
                sh.ilen[fi] = len as u32 + 1;
                if len == 0 {
                    log2(c"input start", fi as i32, 0);
                }
            }
        }
    }

    if !left_down && sh.dragging {
        // v0.52.0: while dragging the layer keeps its glass pattern (anchored),
        // so the drag itself is a pure blit. At the drop we move the anchor to the
        // final position and re-rasterise once, so the acrylic matches the
        // wallpaper the window now sits on.
        {
            let di = sh.drag_idx;
            sh.wins[di].ax = sh.wins[di].nx;
            sh.wins[di].ay = sh.wins[di].ny;
            unsafe {
                WIN_KEY[di] = i32::MIN;
            }
        }
        log2(c"drag end: layer re-anchored", sh.drag_idx as i32, 0);
        sh.dragging = false;
        log2(c"drag end", sh.wins[sh.drag_idx].x, sh.wins[sh.drag_idx].y);
    }
    if sh.dragging {
        let bidx = sh.drag_idx;
        if sh.drag_last_x < 0 {
            sh.drag_last_x = sh.wins[bidx].x;
            sh.drag_last_y = sh.wins[bidx].y;
        }
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
    /* v0.58.0: the fingerprint also covers keyboard focus, the focused window's
     * editable line and the transition counter, so typing repaints and the
     * scale animation can never be skipped as "idle". */
    st[17] = sh.focus as i32;
    {
        let mut ih = sh.ilen[sh.focus] as i32;
        for b in sh.input[sh.focus].iter() {
            ih = ih.wrapping_mul(31).wrapping_add(*b as i32);
        }
        st[18] = ih;
    }
    /* v0.59.0: one repaint per second so the live numbers in the task manager
     * window (uptime) keep moving while the desktop is otherwise idle. */
    st[19] = (sh.scale_frames as i32) | (((now / 1000) as i32) << 8);
    let last = unsafe { &*core::ptr::addr_of!(LAST_STATE) };
    if *last == st && sh.intro_frames == 0 && sh.act_frames == 0 && sh.menu_intro == 0
        && sh.scale_frames == 0
    {
        /* v0.60.0: if the last rendered frame was coalesced by the vsync gate,
         * present it now instead of leaving the screen a generation behind. */
        if unsafe { SWAP_PENDING } && swap_due(now_ms) {
            unsafe {
                FRONT ^= 1;
                SWAP_PENDING = false;
            }
            let frame = scene_front();
            blit_rect(frame, wu, fb, pitch_px as usize, (0, 0, SCREEN_W, SCREEN_H));
            draw_cursor(fb, pitch_px as usize, wu, hu, mx_c, my_c);
            unsafe {
                swap_commit(now_ms, true);
                CUR_SEEN = true;
                CUR_PX = mx_c;
                CUR_PY = my_c;
                PREV_MX = mx_c;
                PREV_MY = my_c;
            }
            return LAST_SUM;
        }
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
        if click_x < 0 && click_y < 0 && key < 0 && ch < 0 && !sh.dragging
           && sh.scale_frames == 0 && pointer_moved
           && hover == prev_hover && unsafe { CUR_SEEN } {
            /* v0.60.0: a cursor-only update is still a framebuffer update, so it
             * obeys the same vsync gate. It does NOT swap the scene buffers (the
             * scene is unchanged): FRONT is what is on screen, so erasing the old
             * arrow is a blit from it. */
            if !swap_due(now_ms) {
                unsafe { SWAP.skipped += 1; }
                return LAST_SUM;
            }
            let frame = scene_front();
            let (px, py) = unsafe { (CUR_PX, CUR_PY) };
            blit_rect(frame, wu, fb, pitch_px as usize,
                      (px - 2, py - 2, px + CUR_W + 2, py + CUR_H + 2));
            blit_rect(frame, wu, fb, pitch_px as usize,
                      (mx_c - 2, my_c - 2, mx_c + CUR_W + 2, my_c + CUR_H + 2));
            draw_cursor(fb, pitch_px as usize, wu, hu, mx_c, my_c);
            unsafe {
                swap_commit(now_ms, false);
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
        /* v0.58.0: a window under a maximise/restore transition is drawn by the
         * transition pass (scaled layer), so the normal compose must skip it. */
        if sh.scale_frames > 0 && sh.scale_idx == i as i32 {
            continue;
        }
        let key = layer_key(&win, i);
        let hit = unsafe { WIN_KEY[i] == key };
        if !hit {
            if render_window_layer(i, &win) {
                unsafe {
                    WIN_KEY[i] = key;
                }
                sh.prof_cache_miss += 1;
            } else {
                draw_window(&mut s, &win, true);
                continue;
            }
        } else {
            sh.prof_cache_hit += 1;
        }
        /* v0.58.0: shadow first (alpha over the scene), then the opaque body. */
        draw_window_shadow(&mut s, &win);
        let frame = scene_back_mut();
        if composite_window_layer(frame, wu, i, &win) {
            if sh.dragging && sh.drag_idx == i {
                sh.prof_drag_reuse += 1;
                if sh.prof_drag_reuse == 1 {
                    log0(c"drag: reusing window layer (composited)");
                }
            }
        }
    }

    /* v0.59.0: live per-window content (task manager counters) is painted after
     * the cached window layers, so the layers stay static and the numbers move. */
    for &i in order.iter() {
        let win = sh.wins[i];
        if !win.open || win.minimized {
            continue;
        }
        crate::apps::draw_live(&mut s, win.accent, win.x, win.y, win.w, win.h, now);
    }

    /* v0.49.0: 2-frame fade-out before a close/minimize actually happens */
    if sh.act_frames > 0 {
        let idx = sh.act_idx;
        if idx >= 0 && idx < 4 && sh.wins[idx as usize].open && !sh.wins[idx as usize].minimized {
            let w = sh.wins[idx as usize];
            let a = if sh.act_frames == 2 { 0x2cu32 } else { 0x64u32 };
            s.rect_fill(w.x, w.y, w.x + w.w, w.y + w.h, 0x000000, a);
        }
        sh.act_frames -= 1;
        if sh.act_frames == 0 && idx >= 0 && idx < 4 {
            let ui = idx as usize;
            if sh.act_kind == 1 {
                sh.wins[ui].minimized = true;
                log2(c"minimize", idx, 0);
                if let Some(other) = next_visible(sh, ui) {
                    sh.focus = other;
                }
            } else if sh.act_kind == 2 {
                sh.wins[ui].open = false;
                log2(c"close", idx, 0);
                if let Some(other) = next_visible(sh, ui) {
                    sh.focus = other;
                }
            }
            log2(c"anim done", sh.act_kind, idx);
            sh.act_kind = 0;
            sh.act_idx = -1;
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

    /* v0.58.0: maximise/restore transition. The geometry already jumped to its
     * target, so the animation samples the HOME-anchored body layer into the
     * interpolated rectangle: the window grows / shrinks smoothly. */
    if sh.scale_frames > 0 {
        let idx = sh.scale_idx;
        if idx >= 0 && idx < 4 && sh.wins[idx as usize].open
            && !sh.wins[idx as usize].minimized
        {
            let total: i32 = 4;
            let step = total - sh.scale_frames as i32 + 1;
            let mut r = [0i32; 4];
            for k in 0..4usize {
                let a = sh.scale_from[k];
                let b = sh.scale_to[k];
                r[k] = a + (b - a) * step / total;
            }
            let mut tmp = sh.wins[idx as usize];
            tmp.x = r[0];
            tmp.y = r[1];
            tmp.w = r[2].max(24);
            tmp.h = r[3].max(24);
            draw_window_shadow(&mut s, &tmp);
            let frame = scene_back_mut();
            if !composite_scaled(frame, wu, idx as usize, &tmp) {
                draw_window(&mut s, &tmp, false);
            }
            log2(c"scale", idx, step);
        }
        sh.scale_frames -= 1;
        if sh.scale_frames == 0 {
            let idx = sh.scale_idx;
            log2(c"scale done", idx, sh.scale_kind);
            /* the pre-existing fade-in now follows the transition, so the
             * settled frame still reads as a soft arrival */
            sh.intro_frames = 3;
            sh.intro_idx = idx;
            sh.scale_idx = -1;
            sh.scale_kind = 0;
        }
    }

    /* v0.58.0: keyboard-focus indicator. The focused window shows an editable
     * line (label + typed ASCII + caret); it is drawn after the window body so
     * the cached layer stays static while the line changes. */
    if !sh.menu_open && !sh.shutting {
        let fi = sh.focus;
        let w = sh.wins[fi];
        let y = w.y + TITLE_H + 104;
        /* v0.59.x: the image viewer has no text content, and when it shows a
         * committed Wayland surface the focus hint would sit on top of the
         * client's pixels - skip the editable line for that window. */
        if w.open && !w.minimized && fi != 2 && y + 20 < w.y + w.h && w.y >= 0 {
            s.rect_fill(w.x + 16, y - 4, w.x + w.w - 16, y + 20, 0x000000, 0x24);
            s.text(w.x + 24, y, "输入:", 0x9fb2cc, 0xd0, 1);
            let x0 = w.x + 24 + 46 + 8;
            let len = sh.ilen[fi] as usize;
            if len == 0 {
                s.text(x0, y, "键入文本 Tab 切换窗口", 0x60718a, 0xc0, 1);
            } else {
                let mut x = x0;
                let buf = &sh.input[fi];
                let mut k = 0usize;
                while k < len && x < w.x + w.w - 40 {
                    x += s.char_ascii(x, y, buf[k] as char, 0xffffff, 0xf0, 1);
                    k += 1;
                }
                s.rect_fill(x, y, x + 2, y + 16, 0x8fc2ff, 0xe0);
            }
        }
    }

    let t_win = ticks() - t_win0;
    // 5. taskbar (acrylic strip sampled from the frosted source)
    let t_bar0 = ticks();
    let tb_key = taskbar_key(sh, clock_hhmm);
    let tb_hit = unsafe { TASKBAR_KEY == tb_key };
    if tb_hit {
        let frame = scene_back_mut();
        if !blit_taskbar(frame, wu) {
            // strip cache unreadable: fall back to a real render
            frosted(&mut s, 0, TASKBAR_Y, SCREEN_W, SCREEN_H, 0.0, 0x30);
s.rect_fill(0, TASKBAR_Y, SCREEN_W, SCREEN_H, 0xffffff, 0x08);
/* start button: rounded tile + the same pseudo-3D plate glyph as the icon */
let hover_start = mx_c >= start_btn.0 && mx_c < start_btn.0 + start_btn.2 &&
                  my_c >= start_btn.1 && my_c < start_btn.1 + start_btn.3;
s.rect_fill(start_btn.0, start_btn.1, start_btn.0 + start_btn.2,
            start_btn.1 + start_btn.3, 0xffffff,
            if hover_start { 0x2c } else { 0x16 });
/* v0.57.1: the real SutureOS logo (24px step of the asset ladder) */
blit_logo_region(&mut s, &g_logo24, LOGO_SMALL_SIZE,
                 0, 0, LOGO_SMALL_SIZE as i32, LOGO_SMALL_SIZE as i32,
                 start_btn.0 + 5, start_btn.1 + 2);

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
        } else {
            sh.prof_taskbar_hit += 1;
        }
    } else {
    frosted(&mut s, 0, TASKBAR_Y, SCREEN_W, SCREEN_H, 0.0, 0x30);
s.rect_fill(0, TASKBAR_Y, SCREEN_W, SCREEN_H, 0xffffff, 0x08);
/* start button: rounded tile + the same pseudo-3D plate glyph as the icon */
let hover_start = mx_c >= start_btn.0 && mx_c < start_btn.0 + start_btn.2 &&
                  my_c >= start_btn.1 && my_c < start_btn.1 + start_btn.3;
s.rect_fill(start_btn.0, start_btn.1, start_btn.0 + start_btn.2,
            start_btn.1 + start_btn.3, 0xffffff,
            if hover_start { 0x2c } else { 0x16 });
/* v0.57.1: the real SutureOS logo (24px step of the asset ladder) */
blit_logo_region(&mut s, &g_logo24, LOGO_SMALL_SIZE,
                 0, 0, LOGO_SMALL_SIZE as i32, LOGO_SMALL_SIZE as i32,
                 start_btn.0 + 5, start_btn.1 + 2);

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
        let frame = scene_back_mut();
        save_taskbar(frame, wu);
        unsafe {
            TASKBAR_KEY = tb_key;
        }
    }
    /* v0.52.0: 2-frame fade-in for the start menu (dim -> clear) */
    if sh.menu_intro > 0 {
        let a = if sh.menu_intro == 2 { 0x50u32 } else { 0x24u32 };
        s.rect_fill(MENU_X, MENU_Y, MENU_X + MENU_W, MENU_Y + MENU_H + 8, 0x000000, a);
        log2(c"menu fade", sh.menu_intro as i32, 0);
        sh.menu_intro -= 1;
    }

    let t_bar = ticks() - t_bar0;
    if sh.dragging {
        let total = ticks() - t_frame0;
        if sh.drag_frames_logged < 20 {
            sh.drag_frames_logged += 1;
            console_printf(b"[gui-shell] drag repaint %d ticks (copy %d win %d bar %d)\n\0".as_ptr(),
                           total as i32, t_copy as i32, t_win as i32, t_bar as i32);
        }
    }

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
        /* v0.57.1: start-menu icon = 48px step of the logo ladder */
blit_logo_region(&mut s, &g_logo48, LOGO_MENU_SIZE,
                 0, 0, LOGO_MENU_SIZE as i32, LOGO_MENU_SIZE as i32,
                 MENU_X + 10, MENU_Y + 2);
s.text(MENU_X + 68, MENU_Y + 16, "常用应用", 0xffffff, 0xf0, 1);
        let items = MENU_ITEMS;
        for (k, name) in items.iter().enumerate() {
            let y = MENU_Y + 46 + k as i32 * 44;
            let hovered = mx_c >= MENU_X + 10 && mx_c < MENU_X + MENU_W - 10 &&
                          my_c >= y && my_c < y + 38;
            let selected = k as i32 == sh.menu_sel;
            s.rect_fill(MENU_X + 10, y, MENU_X + MENU_W - 10, y + 38, 0xffffff,
                        if hovered { 0x2c } else if selected { 0x22 } else { 0x12 });
            let accent = APP_ACCENTS[k];
            if hovered || selected {
                s.rect_fill(MENU_X + 10, y + 5, MENU_X + 13, y + 33, accent, 0xd8);
            }
            draw_app_icon(&mut s, MENU_X + 18, y + 8, 22, accent, k as i32);
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
            let st = &*core::ptr::addr_of!(SWAP);
            let avg = if SWAP_IVL_N > 0 { SWAP_IVL_SUM / SWAP_IVL_N } else { 0 };
            console_printf(
                b"[gui-shell] swap presents=%d gens=%d skipped=%d interval_ms=%d\n\0".as_ptr(),
                st.presents as i32,
                st.generation as i32,
                st.skipped as i32,
                avg as i32,
            );
            SWAP_IVL_SUM = 0;
            SWAP_IVL_N = 0;
        }
        sh.prof_frames = 0;
        sh.prof_full = 0;
        sh.prof_cache_hit = 0;
        sh.prof_cache_miss = 0;
        sh.prof_copy = 0;
        sh.prof_win = 0;
        sh.prof_bar = 0;
    }

    // 7. present: vsync gate, swap back->front, blit the front, then the cursor
    unsafe { SWAP.produced += 1; }
    if !swap_due(now_ms) {
        /* coalesced: this frame stays in the back buffer and is shown by the
         * next due frame (or by the idle flush) - a tick is 10 ms and the C loop
         * paces frames by 3 ticks, so in practice this path is a guard. */
        unsafe {
            SWAP.skipped += 1;
            SWAP_PENDING = true;
        }
        return LAST_SUM;
    }
    /* the freshly rendered back buffer becomes the front buffer */
    unsafe { FRONT ^= 1; }
    {
        let frame = scene_front();
        // v0.50.0: while dragging, only the window's old and new rectangles can
        // differ from what is already on screen — present just those two.
        if sh.dragging {
            let w = sh.wins[sh.drag_idx];
            let old_rect = (sh.drag_last_x - 20, sh.drag_last_y - 20,
                            sh.drag_last_x + w.w + 40, sh.drag_last_y + w.h + 40);
            let new_rect = (w.x - 20, w.y - 20, w.x + w.w + 40, w.y + w.h + 40);
            blit_rect(frame, wu, fb, pitch_px as usize, old_rect);
            blit_rect(frame, wu, fb, pitch_px as usize, new_rect);
            sh.drag_last_x = w.x;
            sh.drag_last_y = w.y;
        } else {
            blit_rect(frame, wu, fb, pitch_px as usize, (0, 0, SCREEN_W, SCREEN_H));
            sh.drag_last_x = -1;
            sh.drag_last_y = -1;
        }
        draw_cursor(fb, pitch_px as usize, wu, hu, mx_c, my_c);
        unsafe {
            swap_commit(now_ms, true);
            SWAP_PENDING = false;
            CUR_SEEN = true;
            CUR_PX = mx_c;
            CUR_PY = my_c;
            PREV_MX = mx_c;
            PREV_MY = my_c;
        }
    }

    /* v0.50.0: sampled checksum. The old loop hashed all 786k pixels after every
     * repaint (~2-3 ticks = 20-30 ms) — a real cost inside a drag. Every 8th
     * pixel is enough to tell frames apart for the idle-skip bookkeeping. */
    let mut hash: u32 = 0x811c9dc5;
    let mut k = 0usize;
    while k < s.px.len() {
        hash ^= s.px[k] & 0x00ff_ffff;
        hash = hash.wrapping_mul(0x0100_0193);
        k += 8;
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
    let hw = hit_win(&sh.wins, mx, my, sh.focus as i32);
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

/// v0.51.0: a window LAYER is rendered once at its home anchor and then blitted
/// wherever the window is. The key therefore only covers size + slot (position is
/// free); dragging never invalidates it.
fn layer_key(w: &Win, slot: usize) -> i32 {
    let mut k = w.w.wrapping_mul(31).wrapping_add(w.h.wrapping_mul(17)).
        wrapping_add(slot as i32);
    /* v0.59.x: the image viewer can show a committed Wayland surface, so its
     * layer must be re-rasterised when the kernel server reports a new commit. */
    if slot == 2 {
        k = k.wrapping_mul(31).wrapping_add(crate::apps::wl_epoch());
    }
    k
}

/// Render the window layer (body + shadow, margins included) at its HOME anchor.
/// The result is a plain RGB image that can be blitted anywhere (v0.51.0: this is
/// what makes dragging a pure blit rather than a re-rasterise).
fn render_window_layer(slot: usize, w: &Win) -> bool {
    let lw = (w.w + WC_MARGIN_L + WC_MARGIN_R) as usize;
    let lh = (w.h + WC_MARGIN_T + WC_MARGIN_B) as usize;
    if lw == 0 || lh == 0 || lw > WC_STRIDE || lh > WC_H {
        return false;
    }
    let cache = unsafe { &mut (&mut *core::ptr::addr_of_mut!(WIN_CACHE))[slot] };
    /* v0.58.0: prefill with the wallpaper under the HOME anchor so the rounded
     * corner AA blends into the real backdrop instead of black. The body itself
     * is opaque, so only the 12 px corner band is affected. */
    let wall = unsafe { &*core::ptr::addr_of!(WALL) };
    let bx = w.ax - WC_MARGIN_L;
    let by = w.ay - WC_MARGIN_T;
    for ly in 0..lh {
        let wy = (by + ly as i32).clamp(0, SCREEN_H - 1) as usize;
        let wrow = wy * SCREEN_W as usize;
        let drow = ly * WC_STRIDE;
        for lx in 0..lw {
            let wx = (bx + lx as i32).clamp(0, SCREEN_W - 1) as usize;
            cache[drow + lx] = wall[wrow + wx];
        }
    }
    let mut cs = Surface { px: &mut cache[..], w: lw, h: lh, stride: WC_STRIDE,
                           clip: (0, 0, lw as i32, lh as i32) };
    let mut wc = *w;
    wc.x = WC_MARGIN_L;
    wc.y = WC_MARGIN_T;
    wc.sox = w.ax - WC_MARGIN_L;
    wc.soy = w.ay - WC_MARGIN_T;
    /* v0.58.0: the shadow is no longer part of the layer (it has to blend with
     * the scene), so only the body is rendered here. */
    draw_window(&mut cs, &wc, false);
    unsafe { (&mut *core::ptr::addr_of_mut!(LAYER_WH))[slot] = (w.w, w.h); }
    true
}

/// Blit the cached layer (body + shadow + margins) at the window's current
/// position. Rows are memcpy'd; nothing is blended, so this costs ~1 ms/window.
fn composite_window_layer(scene: &mut [u32], stride: usize, slot: usize, w: &Win) -> bool {
    let lw = (w.w + WC_MARGIN_L + WC_MARGIN_R) as usize;
    let lh = (w.h + WC_MARGIN_T + WC_MARGIN_B) as usize;
    if lw == 0 || lh == 0 || lw > WC_STRIDE || lh > WC_H {
        return false;
    }
    let cache = unsafe { &(&*core::ptr::addr_of!(WIN_CACHE))[slot] };
    /* v0.58.0: only the body is copied; the margins hold the wallpaper prefill
     * and the shadow is drawn separately (alpha-composited over the scene). */
    let _ = (lw, lh);                    /* validated above, not needed for the copy */
    if w.w <= 0 || w.h <= 0 {
        return true;
    }
    let x0 = w.x;
    let y0 = w.y;
    let sx0 = (-x0).max(0) as usize;
    let sy0 = (-y0).max(0) as usize;
    let ex = (w.w as usize).min((SCREEN_W - x0).max(0) as usize);
    let ey = (w.h as usize).min((SCREEN_H - y0).max(0) as usize);
    if ex <= sx0 || ey <= sy0 {
        return true;
    }
    let n = ex - sx0;
    for ly in sy0..ey {
        let dy = (y0 + ly as i32) as usize;
        let d = dy * stride + (x0 + sx0 as i32) as usize;
        let srow = (WC_MARGIN_T as usize + ly) * WC_STRIDE
                   + WC_MARGIN_L as usize + sx0;
        scene[d..d + n].copy_from_slice(&cache[srow..srow + n]);
    }
    true
}

/// Cache key for the taskbar strip: everything that changes its pixels.
fn taskbar_key(sh: &Shell, clock_hhmm: i32) -> i32 {
    let mut k = clock_hhmm
        .wrapping_mul(31)
        .wrapping_add((sh.focus as i32).wrapping_mul(977))
        .wrapping_add((sh.hover_wb + 1).wrapping_mul(131))
        .wrapping_add((sh.hover_pin + 1).wrapping_mul(67));
    for i in 0..4 {
        let w = sh.wins[i];
        k = k.wrapping_add((w.open as i32) * (7 + i as i32))
             .wrapping_add((w.minimized as i32) * (101 + i as i32))
             .wrapping_add((w.z & 0xff) * (13 + i as i32));
    }
    k
}

/// Blit the cached taskbar strip into the scene (returns false on a miss).
fn blit_taskbar(scene: &mut [u32], stride: usize) -> bool {
    if unsafe { TASKBAR_KEY } == i32::MIN {
        return false;
    }
    let cache = unsafe { &*core::ptr::addr_of!(TASKBAR_CACHE) };
    let y0 = TASKBAR_Y as usize;
    let n = SCREEN_W as usize;
    for row in 0..TB_H {
        let d = (y0 + row) * stride;
        let src = row * n;
        scene[d..d + n].copy_from_slice(&cache[src..src + n]);
    }
    true
}

/// Save the just-rendered taskbar strip.
fn save_taskbar(scene: &[u32], stride: usize) {
    let cache = unsafe { &mut *core::ptr::addr_of_mut!(TASKBAR_CACHE) };
    let y0 = TASKBAR_Y as usize;
    let n = SCREEN_W as usize;
    for row in 0..TB_H {
        let s = (y0 + row) * stride;
        let dst = row * n;
        cache[dst..dst + n].copy_from_slice(&scene[s..s + n]);
    }
}

/// v0.59.0: integer channel scaling (no f32: soft-float is ~20x slower here).
#[inline]
fn shade_px(c: u32, num: u32, den: u32) -> u32 {
    let ch = |sh: u32| -> u32 {
        let v = ((c >> sh) & 0xff) * num / den;
        if v > 255 { 255 } else { v }
    };
    (ch(16) << 16) | (ch(8) << 8) | ch(0)
}

#[inline]
fn lerp_px(a: u32, b: u32, i: i32, n: i32) -> u32 {
    if n <= 0 {
        return a;
    }
    let ch = |sh: u32| -> u32 {
        let av = ((a >> sh) & 0xff) as i32;
        let bv = ((b >> sh) & 0xff) as i32;
        (av + (bv - av) * i / n).clamp(0, 255) as u32
    };
    (ch(16) << 16) | (ch(8) << 8) | ch(0)
}

/// Which glyph belongs to a window accent (0 settings, 1 task manager,
/// 2 image viewer, 3 text editor).
#[inline]
fn glyph_for_accent(accent: u32) -> i32 {
    match accent {
        0x22c1a8 => 1,
        0xf5a524 => 2,
        0xe05a8a => 3,
        _ => 0,
    }
}

/// v0.59.0: pseudo-3D Fluent app icon: rounded plate with a vertical gradient,
/// a specular top highlight, a dark bottom rim and a soft drop shadow. The
/// rounded silhouette is drawn with per-row insets (integer rect fills only) so
/// a 22 px icon costs no soft-float sqrt.
fn draw_app_icon(s: &mut Surface<'_>, x: i32, y: i32, size: i32, accent: u32,
                 glyph: i32) {
    if size < 8 {
        return;
    }
    let r = if size >= 20 { 3 } else { 2 };
    let light = shade_px(accent, 150, 100);
    let dark = shade_px(accent, 58, 100);
    /* drop shadow (pseudo-3D: the plate floats above the surface) */
    s.rect_fill(x + 2, y + 2, x + size + 2, y + size + 2, 0x000000, 0x34);
    for i in 0..size {
        /* inset profile: r, r-1, 0, 0, ... 0, r-1, r */
        let ins = if i < r {
            r - i
        } else if i >= size - r {
            r - (size - 1 - i)
        } else {
            0
        };
        let c = lerp_px(light, dark, i, (size - 1).max(1));
        s.rect_fill(x + ins, y + i, x + size - ins, y + i + 1, c, 0xff);
    }
    /* specular highlight + bottom rim (the actual 3D cue) */
    s.rect_fill(x + r, y + 1, x + size - r, y + 2, 0xffffff, 0x70);
    s.rect_fill(x + r, y + size - 3, x + size - r, y + size - 2, 0x000000, 0x38);
    /* glyph */
    let gx = x + size / 2;
    let gy = y + size / 2;
    let g = if size >= 20 { 4 } else { 3 };
    match glyph {
        0 => {                       /* settings: ring */
            s.rect_fill(gx - g, gy - 1, gx + g, gy + 1, 0xffffff, 0xd8);
            s.rect_fill(gx - 1, gy - g, gx + 1, gy + g, 0xffffff, 0xd8);
        }
        1 => {                       /* task manager: bars */
            s.rect_fill(gx - g, gy + 1, gx - g + 2, gy + g, 0xffffff, 0xd0);
            s.rect_fill(gx - 1, gy - 2, gx + 1, gy + g, 0xffffff, 0xd0);
            s.rect_fill(gx + g - 2, gy - g, gx + g, gy + g, 0xffffff, 0xd0);
        }
        2 => {                       /* image viewer: frame */
            s.rect_fill(gx - g, gy - g, gx + g, gy - g + 1, 0xffffff, 0xd0);
            s.rect_fill(gx - g, gy + g - 1, gx + g, gy + g, 0xffffff, 0xd0);
            s.rect_fill(gx - g, gy - g + 1, gx - g + 1, gy + g - 1, 0xffffff, 0xd0);
            s.rect_fill(gx + g - 1, gy - g + 1, gx + g, gy + g - 1, 0xffffff, 0xd0);
            s.rect_fill(gx - 2, gy, gx + 2, gy + 2, 0xffffff, 0xc0);
        }
        _ => {                       /* text editor: text lines */
            s.rect_fill(gx - g, gy - g, gx + g, gy - g + 1, 0xffffff, 0xd0);
            s.rect_fill(gx - g, gy, gx + 1, gy + 1, 0xffffff, 0xd0);
            s.rect_fill(gx - g, gy + g - 1, gx + g, gy + g, 0xffffff, 0xd0);
        }
    }
}

fn draw_window_shadow(s: &mut Surface<'_>, w: &Win) {
    /* v0.58.0: eight one-pixel rings around the body, each ring one rect fill per
     * side so a corner pixel is covered exactly once. Every ring blends black
     * over what is already there, which is what makes the shadow correct over
     * the wallpaper *and* over a window underneath. Cost is the ring perimeter
     * only (~12k px for a 430x300 window). */
    for (k, alpha) in SHADOW_RING.iter().enumerate() {
        let d = k as i32 + 1;
        let (x0, y0) = (w.x - d, w.y - d);
        let (x1, y1) = (w.x + w.w + d, w.y + w.h + d);
        s.rect_fill(x0, y0, x1, y0 + 1, 0x000000, *alpha);          /* top */
        s.rect_fill(x0, y1 - 1, x1, y1, 0x000000, *alpha);          /* bottom */
        s.rect_fill(x0, y0 + 1, x0 + 1, y1 - 1, 0x000000, *alpha);  /* left */
        s.rect_fill(x1 - 1, y0 + 1, x1, y1 - 1, 0x000000, *alpha);  /* right */
    }
}

fn draw_window(s: &mut Surface<'_>, w: &Win, with_shadow: bool) {
    let t_all = ticks();
    if with_shadow {
        draw_window_shadow(s, w);
    }
    let t_shadow = ticks() - t_all;

    /* body: frosted acrylic + rounded silhouette */
    frosted_at(s, w.x, w.y, w.x + w.w, w.y + w.h, 12.0, 0x2a, w.sox, w.soy);
    let t_frost = ticks() - t_all - t_shadow;
    /* hairline border: four 1 px rect fills (no analytic AA pass) */
    s.rect_fill(w.x + 8, w.y, w.x + w.w - 8, w.y + 1, 0xffffff, 0x30);
    s.rect_fill(w.x + 8, w.y + w.h - 1, w.x + w.w - 8, w.y + w.h, 0xffffff, 0x30);
    s.rect_fill(w.x, w.y + 8, w.x + 1, w.y + w.h - 8, 0xffffff, 0x30);
    s.rect_fill(w.x + w.w - 1, w.y + 8, w.x + w.w, w.y + w.h - 8, 0xffffff, 0x30);

    /* title bar: translucent strip + pseudo-3D app icon + title text + a faint
     * accent underline (Fluent cue; the rounded top corners come from the body's
     * AA silhouette) */
    s.rect_fill(w.x + 2, w.y + 1, w.x + w.w - 2, w.y + TITLE_H, 0xffffff, 0x14);
    draw_app_icon(s, w.x + 9, w.y + 8, TITLE_H - 16, w.accent,
                  glyph_for_accent(w.accent));
    s.text(w.x + 30, w.y + 7, w.title, 0xf2f6ff, 0xf0, 1);
    s.rect_fill(w.x + 2, w.y + TITLE_H - 1, w.x + w.w - 2, w.y + TITLE_H,
                w.accent, 0x2a);

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

    /* v0.59.0: per-window application content (settings / task manager /
     * image viewer / editor) is painted by the apps module. */
    crate::apps::draw_body(s, w.accent, w.x, w.y, w.w, w.h);
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
