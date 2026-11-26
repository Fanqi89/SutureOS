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
/* v0.63: one place for the window count. Growing this is still a real change
 * (every window-indexed array below uses it), but no literal 5 is left to find. */
const WINS: usize = 6;
const MENU_W: i32 = 300;
const MENU_X: i32 = 12;
/* v0.62: the menu grows with the application registry (4 rows until the Wayland
 * window became a menu entry). The panel is anchored to the taskbar, so its height
 * is 46 px of header + one 44 px row per item + 14 px of footer. */
const MENU_HEAD_H: i32 = 46;
const MENU_ROW_H: i32 = 44;
const MENU_FOOT_H: i32 = 14;

/* v0.64: the start menu scrolls instead of growing without bound. The panel is
 * at most MENU_VISIBLE rows tall (324 px, i.e. exactly the height every existing
 * acceptance has calibrated pixel coordinates against); anything past that is
 * reachable with the keyboard, the scrollbar strip or Home/End. */
const MENU_VISIBLE: usize = 6;

#[inline]
fn menu_visible_rows() -> usize {
    if MENU_ROWS.len() > MENU_VISIBLE { MENU_VISIBLE } else { MENU_ROWS.len() }
}

#[inline]
fn menu_scroll_max() -> usize {
    if MENU_ROWS.len() > MENU_VISIBLE { MENU_ROWS.len() - MENU_VISIBLE } else { 0 }
}

#[inline]
fn menu_h() -> i32 {
    MENU_HEAD_H + menu_visible_rows() as i32 * MENU_ROW_H + MENU_FOOT_H
}

#[inline]
fn menu_y() -> i32 {
    TASKBAR_Y - menu_h() - 8
}

#[inline]
fn menu_items_y() -> i32 {
    menu_y() + MENU_HEAD_H
}

#[inline]
fn menu_rect() -> Rect {
    Rect {
        x0: MENU_X - 2,
        y0: menu_y() - 2,
        x1: MENU_X + MENU_W + 4,
        y1: menu_y() + menu_h() + 12,
    }
}

/* v0.58.0: soft shadow ring around a window body, one alpha per pixel of
 * distance (1 px out ... 8 px out). The shadow is composited against whatever is
 * already in the scene, so it darkens the wallpaper, a window below it or a
 * neighbour's acrylic band instead of painting the hard black halo the baked
 * layer used to leave behind. */
const SHADOW_RING: [u32; 8] = [0x22, 0x1c, 0x16, 0x10, 0x0b, 0x07, 0x04, 0x02];

/* ---- v0.71: two desktop styles ------------------------------------------
 * STYLE_GLASS (0, default) is the look built from v0.37 to v0.70: gradient
 * wallpaper, acrylic (blurred) window bodies, soft shadow rings, translucent
 * taskbar / start menu / context menu.
 * STYLE_FLAT (1) is the flat language the ConlyesOS author drew for his v0.70
 * tree: one opaque wallpaper colour, opaque panels, 1 px hairlines, no shadow
 * and no blur. His palette is reproduced verbatim below (credited in
 * docs/virtio-gpu.md and docs/provenance.md).
 * The switch lives in 系统设置 -> 外观风格 and is persisted in SHELL.CFG, so both
 * styles survive a reboot; the default is glass, which is what the appearance
 * baseline is pinned to. */
pub const STYLE_GLASS: u8 = 0;
pub const STYLE_FLAT: u8 = 1;
const FLAT_WALL: u32 = 0x0e1726;   /* desktop backdrop        */
const FLAT_PANEL: u32 = 0x161d29;  /* window body             */
const FLAT_TITLE: u32 = 0x1f2734;  /* title strip             */
const FLAT_BAR: u32 = 0x0b1018;    /* taskbar strip           */
const FLAT_MENU: u32 = 0x131a24;   /* start / context menu    */
const FLAT_LINE: u32 = 0x2b3547;   /* hairlines               */
const FLAT_HOVER: u32 = 0x223046;  /* hover fill              */
const FLAT_PRESS: u32 = 0x2b3d5c;  /* active fill             */
const FLAT_TEXT: u32 = 0xe9effb;
/* The active style, mirrored in a module static so the pure draw helpers
 * (draw_window and friends only take a Surface + Win) can branch without a new
 * parameter on every call site. shell_step refreshes it from the Shell at entry
 * and the cache key includes it, so a switch can never blit a stale layer. */
static mut STYLE: u8 = STYLE_GLASS;

#[inline]
fn flat_ui() -> bool {
    unsafe { STYLE == STYLE_FLAT }
}

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
/* v0.60: the Wayland client surface gets a window of its own ("合成器视图").
 * Opened with F2 rather than from the start menu - see the note at the top of
 * this patch/commit. The window is 440x330 so a 320x200 surface is shown 1:1
 * (the body falls back to scale 1 when 2x would not fit). */
const fn win4() -> Win {
    Win { x: 250, y: 330, w: 440, h: 330, nx: 250, ny: 330, nw: 440, nh: 330,
          minimized: false, maximized: false, z: 5, title: "Wayland 客户端", accent: 0x7a5cff, open: false, sox: 0, soy: 0, ax: 250, ay: 330 }
}
/* v0.63: 计算器 (window 5). Placed bottom-left so it does not cover the two
 * default windows at the top; size is what a 4x4 keypad + display needs. */
const fn win5() -> Win {
    /* v0.64: the calculator grew (fixed-point decimals + memory keys) so the
     * keypad is 5x6 now and needs 320x330. */
    Win { x: 600, y: 210, w: 320, h: 360, nx: 600, ny: 210, nw: 320, nh: 360,
          minimized: false, maximized: false, z: 6, title: "计算器", accent: crate::apps::ACC_CALC, open: false, sox: 0, soy: 0, ax: 600, ay: 210 }
}

struct Shell {
    wins: [Win; WINS],
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
    /* v0.64: first visible start-menu row (the panel is MENU_VISIBLE rows tall) */
    menu_scroll: usize,
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
    /* v0.68: how many drag frames re-anchored the acrylic (live glass) */
    drag_glass_frames: u32,
    /* v0.71: desktop style (STYLE_GLASS / STYLE_FLAT), persisted in SHELL.CFG */
    style: u8,
    /* v0.69: taskbar context menu + runtime pinned list (persisted to SHELL.CFG) */
    ctx_open: bool,
    ctx_kind: i32,      /* 1 = window button, 2 = pinned button */
    ctx_idx: i32,       /* slot (kind 1) / pin index (kind 2) */
    ctx_x: i32,
    ctx_y: i32,
    ctx_sel: i32,
    ctx_prev: i32,      /* (open<<16)|(kind<<8)|sel - a change forces a full frame */
    pinned: [i32; 8],
    pinned_n: i32,
    force_full: bool,
    prof_cache_miss: u32,
    prof_copy: u64,
    prof_win: u64,
    prof_bar: u64,
    prof_total: u64,
    /* v0.58.0: per-window keyboard focus. Every window owns a small editable
     * line; printable keys go to the focused window, Enter commits it to the
     * serial log, Backspace deletes one character. */
    input: [[u8; 33]; WINS],
    ilen: [u32; WINS],
    /* v0.58.0: true maximise/restore transition. `scale_from`/`scale_to` are
     * lerped over SCALE_TOTAL frames; the geometry itself already sits at its
     * target so hit-testing stays honest during the animation. */
    scale_frames: u32,
    scale_idx: i32,
    scale_from: [i32; 4],
    scale_to: [i32; 4],
    scale_kind: i32,
    /* v0.61: the editor's text changed since the last damage computation, so the
     * next frame has to re-rasterise its window layer and repaint that rect. */
    dirty_editor: bool,
    dirty_image: bool,
    dirty_calc: bool,
}

static mut SHELL: Shell = Shell {
    wins: [win0(), win1(), win2(), win3(), win4(), win5()],
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
    menu_scroll: 0,
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
    drag_glass_frames: 0,
    style: STYLE_GLASS,
    ctx_open: false,
    ctx_kind: 0,
    ctx_idx: -1,
    ctx_x: 0,
    ctx_y: 0,
    ctx_sel: -1,
    ctx_prev: -1,
    pinned: [0, 1, 5, 3, -1, -1, -1, -1],
    pinned_n: 4,
    force_full: false,
    prof_cache_miss: 0,
    prof_copy: 0,
    prof_win: 0,
    prof_bar: 0,
    prof_total: 0,
    input: [[0; 33]; WINS],
    ilen: [0; WINS],
    scale_frames: 0,
    scale_idx: -1,
    scale_from: [0; 4],
    scale_to: [0; 4],
    scale_kind: 0,
    dirty_editor: false,
    dirty_image: false,
    dirty_calc: false,
};

// ---------------------------------------------------------------- caches
static mut WALL: [u32; (SCREEN_W as usize) * (SCREEN_H as usize)] =
    [0; (SCREEN_W as usize) * (SCREEN_H as usize)];
static mut SMALL: [u32; SMALL_W * SMALL_H] = [0; SMALL_W * SMALL_H];
/* =====================================================================
 * v0.61.0: three frame buffers, a 60 Hz presentation grid and damage
 * =====================================================================
 *
 * Buffers (all 3 MiB of .bss - the ELF itself does not grow):
 *
 *   DRAW      the only buffer a frame may render into
 *   READY     rendered completely, waiting for its slot on the 60 Hz grid
 *   PRESENTED its pixels are exactly what the framebuffer currently shows
 *   FREE      holds no frame that could be trusted
 *
 * Invariants (enforced here, counted and logged):
 *   1. exactly one buffer is DRAW while a frame renders;
 *   2. `present` only ever blits a READY buffer - a DRAW buffer is never
 *      presented, and a READY buffer is never rendered into;
 *   3. after a present the blitted buffer is the only PRESENTED one, and it
 *      stays the base for the next frame's damage composition (it is the only
 *      buffer whose content is known to match the screen).
 *
 * The kernel has no vsync interrupt source, so "vsync" here means: present on
 * the 60 Hz wall-clock grid of the kernel's high-resolution time source (HPET
 * via `time_source_now_ns()`). A frame whose present would land inside the
 * current slot waits for the next boundary (READY); if another frame is
 * rendered in the meantime it is composed in a second buffer (carry-forward
 * copy) instead of clobbering the pending one.
 */
static mut FRAME_A: [u32; (SCREEN_W as usize) * (SCREEN_H as usize)] =
    [0; (SCREEN_W as usize) * (SCREEN_H as usize)];
static mut FRAME_B: [u32; (SCREEN_W as usize) * (SCREEN_H as usize)] =
    [0; (SCREEN_W as usize) * (SCREEN_H as usize)];
static mut FRAME_C: [u32; (SCREEN_W as usize) * (SCREEN_H as usize)] =
    [0; (SCREEN_W as usize) * (SCREEN_H as usize)];

const BUF_FREE: u8 = 0;
const BUF_DRAW: u8 = 1;
const BUF_READY: u8 = 2;
const BUF_PRESENTED: u8 = 3;

static mut BUF_STATE: [u8; 3] = [BUF_FREE; 3];
static mut BUF_VALID: [bool; 3] = [false; 3];
/// Index of the buffer in DRAW state.
static mut DRAW_BUF: usize = 0;
/// Index of the buffer in READY state (-1 = none).
static mut READY_BUF: i32 = -1;
/// Index of the buffer in PRESENTED state (-1 = none).
static mut PRESENT_BUF: i32 = -1;

/// One letter per buffer state, for `guishell swapstat`:
/// D = DRAW, R = READY, P = PRESENTED, F = FREE.
#[inline]
fn buf_state_letter(i: usize) -> u8 {
    unsafe {
        match BUF_STATE[i] {
            BUF_DRAW => b'D',
            BUF_READY => b'R',
            BUF_PRESENTED => b'P',
            _ => b'F',
        }
    }
}

#[inline]
unsafe fn buf_ptr(i: usize) -> *mut u32 {
    match i {
        1 => core::ptr::addr_of_mut!(FRAME_B) as *mut u32,
        2 => core::ptr::addr_of_mut!(FRAME_C) as *mut u32,
        _ => core::ptr::addr_of_mut!(FRAME_A) as *mut u32,
    }
}

#[inline]
unsafe fn buf_slice(i: usize) -> &'static mut [u32] {
    core::slice::from_raw_parts_mut(buf_ptr(i), (SCREEN_W as usize) * (SCREEN_H as usize))
}

/// True when the buffer currently holds a frame that matches the screen.
#[inline]
unsafe fn buf_is_front(i: i32) -> bool {
    i >= 0 && BUF_VALID[i as usize]
}

// ------------------------------------------------------- high resolution clock
/// C side (`kernel/gui/slint_bridge.c`): monotonic nanoseconds from the kernel's
/// best time source - HPET at 100 MHz when present, the 100 Hz tick otherwise.
extern "C" {
    fn suture_gui_time_ns() -> u64;
    /* v0.64: next vertical retrace, forwarded by kernel/gui/slint_bridge.c to
     * kernel/gui/vsync.c. 1 = waited for an edge, 0 = timed out, -1 = no source. */
    fn suture_gui_vsync_wait() -> i32;
    fn suture_gui_vsync_hz100() -> i32;
}

#[inline]
fn time_ns() -> u64 {
    unsafe { suture_gui_time_ns() }
}

/// 60 Hz presentation grid: 16.666 ms per slot, in nanoseconds.
pub const FRAME_NS: u64 = 16_666_667;

#[derive(Clone, Copy)]
pub struct SwapState {
    /// generation id of the presented frame; +1 on every present
    pub generation: u32,
    /// framebuffer updates performed (one present == one update)
    pub presents: u32,
    /// buffers handed to the framebuffer (always == presents; logged for the
    /// field list the task asks for, and asserted equal by the acceptance)
    pub presented: u32,
    /// produced frames that were superseded before they reached the screen
    /// (conservation: produced == presented + skipped + pending)
    pub skipped: u32,
    /// present attempts the 60 Hz gate postponed (the frame stays READY)
    pub deferred: u32,
    /// a finished frame is still waiting for its slot (0/1)
    pub pending: u32,
    /// produced frames that missed their slot (production->present latency over
    /// 1.5 grid slots) plus frames superseded before they reached the screen
    pub dropped: u32,
    /// production -> present latency (us): average and worst case
    pub latency_sum_us: u32,
    pub latency_max_us: u32,
    /// frames whose present landed more than 1.5 slots after they were produced
    pub late_frames: u32,
    /// scene frames rendered, presented or not
    pub produced: u32,
    /// presents that carried freshly composed scene pixels
    pub scene_presents: u32,
    /// now_ms of the last present (the shell's own frame clock)
    pub last_present_ms: u32,
    /// now_ns of the last present (HPET time base)
    pub last_present_ns: u64,
    /// distance of the last present from the nearest 60 Hz grid boundary
    pub jitter_us: u32,
    pub jitter_sum_us: u32,
    pub jitter_max_us: u32,
    /// interval accounting (ms) for the last logging window
    pub interval_sum_ms: u32,
    pub interval_n: u32,
    /// interval histogram, ms buckets: <17, <34, <50, <100, <200, >=200
    pub hist: [u32; 6],
    /// pixels composed through the damage path / pixels a full repaint would have
    pub damaged_px: u64,
    pub framed_px: u64,
    /// frames that took the damage path / the full repaint path
    pub damage_frames: u32,
    pub full_frames: u32,
    /// damage oracle runs and mismatches (must stay 0)
    pub oracle_checks: u32,
    pub oracle_mismatch: u32,
    /// present completion checks and mismatches (must stay 0)
    pub tear_checks: u32,
    pub tear_mismatch: u32,
    /// frames whose base had to be copied into another buffer (READY pending)
    pub carry_copies: u32,
    /// state-machine invariants checked at every present / violations found
    pub buf_checks: u32,
    pub buf_bad: u32,
    /// v0.64: present-side vertical retrace accounting
    pub vsync_edges: u32,
    pub vsync_timeouts: u32,
    pub vsync_unavail: u32,
    /// refresh rate the driver measured (Hz * 100), 0 when it has none
    pub vsync_hz100: i32,
    /// retrace wait histogram (us): <500, <2000, <6000, <12000, >=12000
    pub wait_hist: [u32; 5],
    pub wait_sum_us: u32,
    pub wait_max_us: u32,
    /// production -> present latency histogram (ms): <17, <34, <50, <100, >=100
    pub lat_hist: [u32; 5],
}

static mut SWAP: SwapState = SwapState {
    generation: 0,
    presents: 0,
    presented: 0,
    skipped: 0,
    deferred: 0,
    pending: 0,
    dropped: 0,
    latency_sum_us: 0,
    latency_max_us: 0,
    late_frames: 0,
    produced: 0,
    scene_presents: 0,
    last_present_ms: 0,
    last_present_ns: 0,
    jitter_us: 0,
    jitter_sum_us: 0,
    jitter_max_us: 0,
    interval_sum_ms: 0,
    interval_n: 0,
    hist: [0; 6],
    damaged_px: 0,
    framed_px: 0,
    damage_frames: 0,
    full_frames: 0,
    oracle_checks: 0,
    oracle_mismatch: 0,
    tear_checks: 0,
    tear_mismatch: 0,
    carry_copies: 0,
    buf_checks: 0,
    buf_bad: 0,
    vsync_edges: 0,
    vsync_timeouts: 0,
    vsync_unavail: 0,
    vsync_hz100: 0,
    wait_hist: [0; 5],
    wait_sum_us: 0,
    wait_max_us: 0,
    lat_hist: [0; 5],
};
static mut SWAP_STARTED: bool = false;
/// Time of the first present: grid slot k begins at VSYNC_BASE_NS + k*FRAME_NS.
static mut VSYNC_BASE_NS: u64 = 0;
/// The next grid boundary the present path is allowed to use.
static mut NEXT_SLOT: u64 = 1;
/// Damage accumulated since the last framebuffer update. A frame whose present
/// the grid defers is *not* lost: its rectangles stay here until a present
/// actually blits them (that is what keeps the screen correct when a frame is
/// superseded by the next one).
static mut ACCUM: Damage = Damage::new();
/// Time (ns) at which that frame finished rendering: production -> present
/// latency is what the dropped-frame accounting is based on.
static mut READY_PRODUCED_NS: u64 = 0;
/// Upper bound on the damage list (each entry costs a rect blit + a compose).
/* v0.61: damage merging works on a 32x32 tile grid (see `impl Damage`) and the
 * rebuilt rectangle list may be longer than the old six-block cap. */
const MAX_DAMAGE: usize = 16;
const DAMAGE_TILE: i32 = 32;
const DAMAGE_TW: usize = 32;                    /* 1024 / 32 */
const DAMAGE_TH: usize = 24;                    /* 768 / 32 */
const DAMAGE_WORDS: usize = (DAMAGE_TW * DAMAGE_TH + 63) / 64;   /* 12 */

/// True when the current time reached the next 60 Hz boundary.
#[inline]
unsafe fn vsync_due(now_ns: u64) -> bool {
    if !SWAP_STARTED {
        return true;
    }
    let boundary = VSYNC_BASE_NS.saturating_add(NEXT_SLOT.saturating_mul(FRAME_NS));
    now_ns >= boundary
}

/// Microseconds between `now_ns` and the nearest 60 Hz grid boundary.
#[inline]
unsafe fn vsync_jitter_us(now_ns: u64) -> u32 {
    let phase = now_ns.wrapping_sub(VSYNC_BASE_NS) % FRAME_NS;
    let d = if phase > FRAME_NS / 2 { FRAME_NS - phase } else { phase };
    (d / 1000) as u32
}

/// v0.64: one bounded wait for the next vertical retrace, recorded in the swap
/// histograms. Returns the driver verdict (1 edge / 0 timeout / -1 no source).
///
/// The driver bounds the wait itself (~40 ms), so a machine without a readable
/// vblank source cannot stall the desktop: the 60 Hz HPET grid above stays the
/// only pacing and every attempt lands in `vsync_unavail`.
unsafe fn vsync_present_wait() -> i32 {
    let t0 = time_ns();
    let verdict = suture_gui_vsync_wait();
    let us = (time_ns().saturating_sub(t0) / 1000) as u32;
    let st = &mut *core::ptr::addr_of_mut!(SWAP);
    match verdict {
        1 => {
            st.vsync_edges = st.vsync_edges.wrapping_add(1);
            let b = if us < 500 {
                0
            } else if us < 2000 {
                1
            } else if us < 6000 {
                2
            } else if us < 12000 {
                3
            } else {
                4
            };
            st.wait_hist[b] = st.wait_hist[b].wrapping_add(1);
            st.wait_sum_us = st.wait_sum_us.saturating_add(us);
            if us > st.wait_max_us {
                st.wait_max_us = us;
            }
        }
        0 => {
            st.vsync_timeouts = st.vsync_timeouts.wrapping_add(1);
        }
        _ => {
            st.vsync_unavail = st.vsync_unavail.wrapping_add(1);
        }
    }
    let hz = suture_gui_vsync_hz100();
    if hz > 0 {
        st.vsync_hz100 = hz;
    }
    verdict
}

/// Account for one framebuffer update: interval/jitter/dropped/histogram.
unsafe fn swap_commit(now_ns: u64, now_ms: u32, scene: bool, px: u64, latency_ns: u64) {
    let st = &mut *core::ptr::addr_of_mut!(SWAP);
    if !SWAP_STARTED {
        SWAP_STARTED = true;
        VSYNC_BASE_NS = now_ns;
        NEXT_SLOT = 1;
    } else {
        let ivl_ns = now_ns.saturating_sub(st.last_present_ns);
        let ivl_ms = (ivl_ns / 1_000_000) as u32;
        st.interval_sum_ms = st.interval_sum_ms.saturating_add(ivl_ms);
        st.interval_n += 1;
        WLOG_IVL_SUM = WLOG_IVL_SUM.saturating_add(ivl_ms);
        WLOG_IVL_N += 1;
        let bucket = if ivl_ms < 17 {
            0
        } else if ivl_ms < 34 {
            1
        } else if ivl_ms < 50 {
            2
        } else if ivl_ms < 100 {
            3
        } else if ivl_ms < 200 {
            4
        } else {
            5
        };
        st.hist[bucket] += 1;
        let jit = vsync_jitter_us(now_ns);
        st.jitter_us = jit;
        st.jitter_sum_us = st.jitter_sum_us.saturating_add(jit);
        WLOG_JIT_SUM = WLOG_JIT_SUM.saturating_add(jit);
        if jit > st.jitter_max_us {
            st.jitter_max_us = jit;
        }
        /* a produced frame that reaches the screen more than 1.5 grid slots
         * after it was rendered missed its slot: count the slots it skipped */
        let lat_slots = latency_ns / FRAME_NS;
        if latency_ns > FRAME_NS + FRAME_NS / 2 {
            st.late_frames = st.late_frames.wrapping_add(1);
            if lat_slots > 1 {
                st.dropped = st.dropped.wrapping_add((lat_slots - 1) as u32);
            }
        }
        let lat_us = (latency_ns / 1000) as u32;
        st.latency_sum_us = st.latency_sum_us.saturating_add(lat_us);
        if lat_us > st.latency_max_us {
            st.latency_max_us = lat_us;
        }
        WLOG_LAT_SUM = WLOG_LAT_SUM.saturating_add(lat_us);
        if lat_us > WLOG_LAT_MAX {
            WLOG_LAT_MAX = lat_us;
        }
        /* v0.64: explicit production -> present latency histogram (ms buckets) */
        let lat_ms = (latency_ns / 1_000_000) as u32;
        let lb = if lat_ms < 17 {
            0
        } else if lat_ms < 34 {
            1
        } else if lat_ms < 50 {
            2
        } else if lat_ms < 100 {
            3
        } else {
            4
        };
        st.lat_hist[lb] = st.lat_hist[lb].wrapping_add(1);
        let k = now_ns.saturating_sub(VSYNC_BASE_NS) / FRAME_NS + 1;
        NEXT_SLOT = k;
    }
    st.generation = st.generation.wrapping_add(1);
    st.presents = st.presents.wrapping_add(1);
    st.presented = st.presents;
    st.last_present_ns = now_ns;
    st.last_present_ms = now_ms;
    if scene {
        st.scene_presents = st.scene_presents.wrapping_add(1);
    }
    st.damaged_px = st.damaged_px.saturating_add(px);
}

/// Log the swap-chain counters once (`guishell swapstat`). Returns `presents`.
pub unsafe fn swap_report() -> u32 {
    let st = &*core::ptr::addr_of!(SWAP);
    let avg = if st.interval_n > 0 { st.interval_sum_ms / st.interval_n } else { 0 };
    let jit = if st.interval_n > 0 { st.jitter_sum_us / st.interval_n } else { 0 };
    console_printf(
        b"[gui-shell] swapstat presents=%d gens=%d presented=%d dropped=%d jitter_us=%d \
interval_ms=%d skipped=%d rendered=%d scene=%d carry=%d full=%d dmg=%d \
dmg_px=%llu framed_px=%llu oracle=%d mismatch=%d tear=%d tear_bad=%d \
late=%d latency_us=%d latency_max_us=%d \
hist=%d,%d,%d,%d,%d,%d deferred=%d pending=%d bufs=%c%c%c \
buf_checks=%d buf_bad=%d \
vsync_edges=%d vsync_timeouts=%d vsync_unavail=%d vsync_hz=%d \
wait_us=%d wait_max_us=%d wait_hist=%d,%d,%d,%d,%d \
lat_hist=%d,%d,%d,%d,%d\n\0"
            .as_ptr(),
        st.presents as i32,
        st.generation as i32,
        st.presented as i32,
        st.dropped as i32,
        jit as i32,
        avg as i32,
        st.skipped as i32,
        st.produced as i32,
        st.scene_presents as i32,
        st.carry_copies as i32,
        st.full_frames as i32,
        st.damage_frames as i32,
        st.damaged_px,
        st.framed_px,
        st.oracle_checks as i32,
        st.oracle_mismatch as i32,
        st.tear_checks as i32,
        st.tear_mismatch as i32,
        st.late_frames as i32,
        if st.interval_n > 0 { (st.latency_sum_us / st.interval_n) as i32 } else { 0 },
        st.latency_max_us as i32,
        st.hist[0] as i32,
        st.hist[1] as i32,
        st.hist[2] as i32,
        st.hist[3] as i32,
        st.hist[4] as i32,
        st.hist[5] as i32,
        st.deferred as i32,
        (READY_BUF >= 0) as i32,
        buf_state_letter(0) as i32,
        buf_state_letter(1) as i32,
        buf_state_letter(2) as i32,
        st.buf_checks as i32,
        st.buf_bad as i32,
        st.vsync_edges as i32,
        st.vsync_timeouts as i32,
        st.vsync_unavail as i32,
        st.vsync_hz100,
        if st.vsync_edges > 0 { (st.wait_sum_us / st.vsync_edges) as i32 } else { 0 },
        st.wait_max_us as i32,
        st.wait_hist[0] as i32,
        st.wait_hist[1] as i32,
        st.wait_hist[2] as i32,
        st.wait_hist[3] as i32,
        st.wait_hist[4] as i32,
        st.lat_hist[0] as i32,
        st.lat_hist[1] as i32,
        st.lat_hist[2] as i32,
        st.lat_hist[3] as i32,
        st.lat_hist[4] as i32,
    );
    st.presents
}

// ------------------------------------------------------------ damage rectangles
/// Inclusive-exclusive rectangle in screen pixels.
#[derive(Clone, Copy, PartialEq)]
struct Rect {
    x0: i32,
    y0: i32,
    x1: i32,
    y1: i32,
}

impl Rect {
    const EMPTY: Rect = Rect { x0: 1, y0: 1, x1: 0, y1: 0 };

    #[inline]
    fn new(x0: i32, y0: i32, x1: i32, y1: i32) -> Rect {
        Rect { x0, y0, x1, y1 }
    }

    #[inline]
    fn is_empty(&self) -> bool {
        self.x1 <= self.x0 || self.y1 <= self.y0
    }

    #[inline]
    fn area(&self) -> i64 {
        let w = self.x1 - self.x0;
        let h = self.y1 - self.y0;
        if w <= 0 || h <= 0 {
            0
        } else {
            w as i64 * h as i64
        }
    }

    #[inline]
    fn union(self, o: Rect) -> Rect {
        Rect {
            x0: self.x0.min(o.x0),
            y0: self.y0.min(o.y0),
            x1: self.x1.max(o.x1),
            y1: self.y1.max(o.y1),
        }
    }

    #[inline]
    fn intersect(self, o: Rect) -> Rect {
        Rect {
            x0: self.x0.max(o.x0),
            y0: self.y0.max(o.y0),
            x1: self.x1.min(o.x1),
            y1: self.y1.min(o.y1),
        }
    }

    #[inline]
    fn overlaps(self, o: Rect) -> bool {
        !self.intersect(o).is_empty()
    }

    #[inline]
    fn expand(self, d: i32) -> Rect {
        Rect {
            x0: self.x0 - d,
            y0: self.y0 - d,
            x1: self.x1 + d,
            y1: self.y1 + d,
        }
    }

    #[inline]
    fn clipped(self) -> Rect {
        self.intersect(Rect::new(0, 0, SCREEN_W, SCREEN_H))
    }

    #[inline]
    fn as_tuple(self) -> (i32, i32, i32, i32) {
        (self.x0, self.y0, self.x1, self.y1)
    }
}

/// Shadow bleed: the eight one-pixel rings reach 8 px out, plus a little slack.
const SHADOW_MARGIN: i32 = 10;
const TASKBAR_RECT: Rect = Rect { x0: 0, y0: TASKBAR_Y, x1: SCREEN_W, y1: SCREEN_H };

/// A small list of screen rectangles to recompose. Rectangles that touch are
/// merged (one bigger recompose is cheaper than two), the rest stay separate -
/// a single boundigin box over two distant controls is exactly the mistake that
/// made the v0.43 prototype repaint the whole screen for a menu click.
#[derive(Clone, Copy)]
struct Damage {
    /* v0.61: tile bitmap (32x24 tiles of 32x32 px) is the source of truth; `r`/`n`
     * are rebuilt from it after every add so the consumers (present, cursor fast
     * path, stats, oracle) do not need to know about tiles. */
    tiles: [u64; DAMAGE_WORDS],
    r: [Rect; MAX_DAMAGE],
    n: usize,
    full: bool,
}

impl Damage {
    const fn new() -> Damage {
        Damage { tiles: [0u64; DAMAGE_WORDS], r: [Rect::EMPTY; MAX_DAMAGE],
                 n: 0, full: false }
    }

    fn clear(&mut self) {
        self.tiles = [0u64; DAMAGE_WORDS];
        self.n = 0;
        self.full = false;
    }

    fn is_empty(&self) -> bool {
        !self.full && self.n == 0
    }

    fn full_screen(&mut self) {
        self.full = true;
        self.n = 0;
        self.tiles = [0u64; DAMAGE_WORDS];
    }

    #[inline]
    fn tile_set(&mut self, col: usize, row: usize) {
        let i = row * DAMAGE_TW + col;
        self.tiles[i >> 6] |= 1u64 << (i & 63);
    }

    #[inline]
    fn tile_get(&self, col: usize, row: usize) -> bool {
        let i = row * DAMAGE_TW + col;
        (self.tiles[i >> 6] >> (i & 63)) & 1 != 0
    }

    /// Mark every tile the (clipped) rectangle touches, then rebuild the rects.
    fn add(&mut self, r0: Rect) {
        if self.full {
            return;
        }
        let r = r0.clipped();
        if r.is_empty() {
            return;
        }
        let c0 = (r.x0 / DAMAGE_TILE).max(0) as usize;
        let c1 = ((((r.x1 + DAMAGE_TILE - 1) / DAMAGE_TILE) as usize)).min(DAMAGE_TW);
        let r_0 = (r.y0 / DAMAGE_TILE).max(0) as usize;
        let r_1 = ((((r.y1 + DAMAGE_TILE - 1) / DAMAGE_TILE) as usize)).min(DAMAGE_TH);
        let mut row = r_0;
        while row < r_1 {
            let mut col = c0;
            while col < c1 {
                self.tile_set(col, row);
                col += 1;
            }
            row += 1;
        }
        self.rebuild();
    }

    /// Greedy scan: per tile row, runs of set tiles are merged with the previous
    /// row's runs when the column span matches exactly, which is what turns a
    /// window-sized block into one rectangle and keeps distant hot spots apart.
    fn rebuild(&mut self) {
        let mut out = [Rect::EMPTY; MAX_DAMAGE];
        let mut n = 0usize;
        /* open runs of the previous row: (c0, c1, index into out) */
        let mut open = [(0usize, 0usize, 0usize); MAX_DAMAGE];
        let mut open_n = 0usize;
        let mut row = 0usize;
        while row < DAMAGE_TH {
            let mut runs = [(0usize, 0usize); MAX_DAMAGE];
            let mut runs_n = 0usize;
            let mut col = 0usize;
            while col < DAMAGE_TW {
                if self.tile_get(col, row) {
                    let c0 = col;
                    while col < DAMAGE_TW && self.tile_get(col, row) {
                        col += 1;
                    }
                    if runs_n < MAX_DAMAGE {
                        runs[runs_n] = (c0, col);
                        runs_n += 1;
                    } else {
                        runs[MAX_DAMAGE - 1].1 = col;   /* fold overflow */
                    }
                } else {
                    col += 1;
                }
            }
            let mut next = [(0usize, 0usize, 0usize); MAX_DAMAGE];
            let mut next_n = 0usize;
            let mut k = 0usize;
            while k < runs_n {
                let (c0, c1) = runs[k];
                let mut done = false;
                let mut o = 0usize;
                while o < open_n {
                    let (o0, o1, idx) = open[o];
                    if !done && o0 == c0 && o1 == c1 {
                        out[idx].y1 = ((row + 1) as i32) * DAMAGE_TILE;
                        if next_n < MAX_DAMAGE {
                            next[next_n] = (c0, c1, idx);
                            next_n += 1;
                        }
                        done = true;
                    }
                    o += 1;
                }
                if !done {
                    let nr = Rect::new((c0 as i32) * DAMAGE_TILE,
                                       (row as i32) * DAMAGE_TILE,
                                       (c1 as i32) * DAMAGE_TILE,
                                       ((row + 1) as i32) * DAMAGE_TILE);
                    let idx;
                    if n < MAX_DAMAGE {
                        idx = n;
                        out[n] = nr;
                        n += 1;
                    } else {
                        /* rectangle cap reached: fold into the last one (always
                         * conservative - a bigger repaint, never a missed one) */
                        idx = MAX_DAMAGE - 1;
                        out[idx] = out[idx].union(nr);
                    }
                    if next_n < MAX_DAMAGE {
                        next[next_n] = (c0, c1, idx);
                        next_n += 1;
                    }
                }
                k += 1;
            }
            open = next;
            open_n = next_n;
            row += 1;
        }
        self.r = out;
        self.n = n;
    }

    /// Tiles currently set (statistics only).
    fn tile_count(&self) -> usize {
        let mut c = 0usize;
        for row in 0..DAMAGE_TH {
            for col in 0..DAMAGE_TW {
                if self.tile_get(col, row) {
                    c += 1;
                }
            }
        }
        c
    }

    fn area(&self) -> i64 {
        if self.full {
            return SCREEN_W as i64 * SCREEN_H as i64;
        }
        self.tile_count() as i64 * (DAMAGE_TILE as i64) * (DAMAGE_TILE as i64)
    }
}

/// Per-window snapshot used to diff one frame against the previous one.
#[derive(Clone, Copy)]
struct WinSnap {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
    open: bool,
    minimized: bool,
    z: i32,
}

impl WinSnap {
    const fn empty() -> WinSnap {
        WinSnap { x: 0, y: 0, w: 0, h: 0, open: false, minimized: false, z: 0 }
    }

    fn of(w: &Win) -> WinSnap {
        WinSnap {
            x: w.x,
            y: w.y,
            w: w.w,
            h: w.h,
            open: w.open,
            minimized: w.minimized,
            z: w.z,
        }
    }

    fn visible(&self) -> bool {
        self.open && !self.minimized
    }

    fn rect(&self) -> Rect {
        Rect::new(self.x, self.y, self.x + self.w, self.y + self.h)
    }
}

/// Everything the damage diff needs from the frame that is currently on screen.
#[derive(Clone, Copy)]
struct Snap {
    ready: bool,
    mx: i32,
    my: i32,
    hover_wb: i32,
    hover_start: bool,
    menu_open: bool,
    menu_sel: i32,
    menu_scroll: i32,
    menu_hover: i32,
    menu_alpha: u32,
    focus: i32,
    input_hash: i32,
    clock: i32,
    live_tick: u32,
    wl_epoch: i32,
    act_frames: u32,
    act_idx: i32,
    intro_frames: u32,
    intro_idx: i32,
    scale_frames: u32,
    wins: [WinSnap; WINS],
}

impl Snap {
    const fn empty() -> Snap {
        Snap {
            ready: false,
            mx: -1,
            my: -1,
            hover_wb: -1,
            hover_start: false,
            menu_open: false,
            menu_sel: -1,
            menu_scroll: 0,
            menu_hover: -1,
            menu_alpha: 0,
            focus: 0,
            input_hash: 0,
            clock: 0,
            live_tick: 0,
            wl_epoch: 0,
            act_frames: 0,
            act_idx: -1,
            intro_frames: 0,
            intro_idx: -1,
            scale_frames: 0,
            wins: [WinSnap::empty(); WINS],
        }
    }

    fn visible_rect(&self, idx: i32) -> Rect {
        if idx < 0 || idx >= WINS as i32 {
            return Rect::EMPTY;
        }
        let w = self.wins[idx as usize];
        if !w.visible() {
            return Rect::EMPTY;
        }
        w.rect().expand(SHADOW_MARGIN)
    }
}

static mut SNAP: Snap = Snap::empty();
/* per logging window (30 frames): present interval / jitter sums */
static mut WLOG_IVL_SUM: u32 = 0;
static mut WLOG_IVL_N: u32 = 0;
static mut WLOG_JIT_SUM: u32 = 0;
static mut WLOG_LAT_SUM: u32 = 0;
static mut WLOG_LAT_MAX: u32 = 0;
/* v0.61: peak number of 32x32 damage tiles in the current 30-frame window */
static mut WLOG_TILES_MAX: i32 = 0;
/// Diagnostics for the damage oracle: the rectangle list of the frame it
/// is about to verify (logged on a mismatch).
static mut ORACLE_INFO: [i32; 9] = [0; 9];

static mut XMAP: [u16; SCREEN_W as usize] = [0; SCREEN_W as usize];
static mut YMAP: [u16; SCREEN_H as usize] = [0; SCREEN_H as usize];
static mut CACHED: bool = false;
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
static mut WIN_CACHE: [[u32; WC_STRIDE * WC_H]; WINS] = [[0; WC_STRIDE * WC_H]; WINS];
static mut WIN_KEY: [i32; WINS] = [i32::MIN; WINS];
/* v0.58.0: the cached window body size. The layer is normally blitted 1:1, but
 * the maximise/restore transition samples it scaled, so the source size has to
 * be known even after the window geometry already jumped to its target. */
static mut LAYER_WH: [(i32, i32); WINS] = [(0, 0); WINS];
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
extern "C" {
    /* kernel/vfs: write a whole file (the editor's F5 save path) */
    fn vfs_write_file(path: *const u8, buf: *const u8, len: u32) -> i32;
    /* v0.69: load the pinned taskbar apps from /mnt/fat/SHELL.CFG */
    fn vfs_read_file(path: *const u8, len: *mut usize) -> *mut u8;
    fn lemis_heap_free(p: *mut u8);
    /* kernel/ipc/wl_server.c (weak: no-op until the server defines it) */
    fn wl_server_desktop_pump();
}

/// v0.61: F5 in the editor window saves the buffer to /mnt/fat/GUIEDIT.TXT.
/// The bytes come from `apps::editor_snapshot`, the write goes straight to the
/// kernel VFS, and the serial line carries both counts so the acceptance can
/// check the host-side file byte for byte.
fn editor_save() {
    let mut buf = [0u8; 512];
    let n = crate::apps::editor_snapshot(&mut buf);
    if n == 0 {
        unsafe {
            console_printf(b"[gui-shell] editor save FAILED (buffer too small)\n\0".as_ptr());
        }
        return;
    }
    let rc = unsafe {
        vfs_write_file(b"/mnt/fat/GUIEDIT.TXT\0".as_ptr(), buf.as_ptr(), n as u32)
    };
    unsafe {
        console_printf(b"[gui-shell] editor saved %d bytes rc=%d\n\0".as_ptr(),
                       n as i32, rc);
    }
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
    if flat_ui() {
        /* v0.71 flat style: one opaque colour, no gradient, no blobs, no vignette
         * (the flat language never pretends the backdrop is lit). */
        s.rect_fill(0, 0, SCREEN_W, SCREEN_H, FLAT_WALL, 255);
    } else {
        // wallpaper: 3-stop gradient + two soft colour blobs (the acrylic source)
        s.vgrad3(0, 0, SCREEN_W, SCREEN_H, 0x1b2440, 0x121a2b, 0x090d16, 0.45);
        s.hgrad_round(60.0, 90.0, 500.0, 560.0, 96.0, 0x3f7bff, 0x9a5cff, 0xd0);
        s.hgrad_round(600.0, 60.0, 1000.0, 400.0, 96.0, 0x12b8a6, 0x2f6bff, 0xc0);
        /* v0.60 polish: a faint cool glow hugging the top edge, then an integer
         * vignette - six 8 px bands per edge whose alpha falls off with distance.
         * Both run once, when the cache is built; nothing here is per frame. */
        s.hgrad_round(300.0, 24.0, 780.0, 320.0, 120.0, 0x8fb4ff, 0x1d2a4d, 0x1c);
        for i in 0..6 {
            let a = (26 - i * 4) as u32;
            let d = i * 8;
            s.rect_fill(0, d, SCREEN_W, d + 8, 0x000000, a);
            s.rect_fill(0, SCREEN_H - d - 8, SCREEN_W, SCREEN_H - d, 0x000000, a);
            s.rect_fill(d, 0, d + 8, SCREEN_H, 0x000000, a);
            s.rect_fill(SCREEN_W - d - 8, 0, SCREEN_W - d, SCREEN_H, 0x000000, a);
        }
    }
    // small mirror of the wallpaper, blurred -> cheap "frosted" source for
    // windows/taskbar/menu (sampled with nearest neighbour, looks right for
    // translucent surfaces)
    let small = &mut *core::ptr::addr_of_mut!(SMALL);
    let mut ss = Surface { px: &mut small[..], w: SMALL_W, h: SMALL_H, stride: SMALL_W, clip: (0, 0, 1024, 768) };
    if flat_ui() {
        /* v0.71: the flat style has no frosted source, but the taskbar/menu code
         * still asks for the cache geometry - keep it the flat backdrop colour. */
        ss.rect_fill(0, 0, SMALL_W as i32, SMALL_H as i32, FLAT_WALL, 255);
    } else {
        ss.vgrad3(0, 0, SMALL_W as i32, SMALL_H as i32, 0x1b2440, 0x121a2b, 0x090d16, 0.45);
        let sx = SMALL_W as f32 / SCREEN_W as f32;
        let sy = SMALL_H as f32 / SCREEN_H as f32;
        ss.hgrad_round(60.0 * sx, 90.0 * sy, 500.0 * sx, 560.0 * sy, 24.0,
                       0x3f7bff, 0x9a5cff, 0xd0);
        ss.hgrad_round(600.0 * sx, 60.0 * sy, 1000.0 * sx, 400.0 * sy, 24.0,
                       0x12b8a6, 0x2f6bff, 0xc0);
        /* v0.60: the frosted source mirrors the new wallpaper depth, otherwise a
         * window parked near an edge would be lit differently from the desktop. */
        ss.hgrad_round(300.0 * sx, 24.0 * sy, 780.0 * sx, 320.0 * sy, 30.0,
                       0x8fb4ff, 0x1d2a4d, 0x1c);
        for i in 0..6 {
            let a = (26 - i * 4) as u32;
            let d = i * 2;
            ss.rect_fill(0, d, SMALL_W as i32, d + 2, 0x000000, a);
            ss.rect_fill(0, SMALL_H as i32 - d - 2, SMALL_W as i32,
                         SMALL_H as i32 - d, 0x000000, a);
            ss.rect_fill(d, 0, d + 2, SMALL_H as i32, 0x000000, a);
            ss.rect_fill(SMALL_W as i32 - d - 2, 0, SMALL_W as i32 - d,
                         SMALL_H as i32, 0x000000, a);
        }
        swrast::acrylic_blur(&mut ss, 0, 0, SMALL_W as i32, SMALL_H as i32, 6);
    }


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

/* v0.68: complete taskbar layout.
 *   [start 8..42] [pinned app icons 50..] [window buttons after them] [clock]
 * Pinned buttons are icon shortcuts for applications that are NOT open (once a
 * window is open its window button represents it, like the v0.34 C desktop).
 * v0.69: the pinned list is runtime state, so the window buttons start where the
 * CURRENT pinned section ends (default 4 pins => x=250). */
const PIN_DEFAULT: [i32; 4] = [0, 1, 5, 3];
const PIN_MAX: i32 = 8;
const PIN_X0: i32 = 50;
const PIN_PITCH: i32 = 48;
const PIN_W: i32 = 44;
/// First window-button x for a given pinned count (4 pins => 250).
fn wb_x0(sh: &Shell) -> i32 {
    PIN_X0 + sh.pinned_n.max(0) * PIN_PITCH + 8
}

/// Slot pinned in position `p` (-1 when empty).
fn pin_slot(sh: &Shell, p: i32) -> i32 {
    if p >= 0 && p < sh.pinned_n && p < PIN_MAX {
        sh.pinned[p as usize]
    } else {
        -1
    }
}

fn is_pinned(sh: &Shell, slot: i32) -> bool {
    let mut p = 0;
    while p < sh.pinned_n {
        if sh.pinned[p as usize] == slot {
            return true;
        }
        p += 1;
    }
    false
}

/// (x, w) of pinned button `p`.
fn pin_rect(p: i32) -> (i32, i32) {
    (PIN_X0 + p * PIN_PITCH, PIN_W)
}

/// (x, w) of the taskbar window button `i`, or None when hidden.
fn wb_rect(sh: &Shell, i: usize) -> Option<(i32, i32)> {
    let mut x = wb_x0(sh);
    for k in 0..i {
        if sh.wins[k].open { x += 96 + 8; }
    }
    if !sh.wins[i].open { return None; }
    Some((x, 96))
}

// ---------------------------------------------------- v0.69 context menu
const CTX_W: i32 = 138;
const CTX_ROW_H: i32 = 30;
const CTX_PAD: i32 = 4;

/// Number of rows in the open context menu.
fn ctx_rows(sh: &Shell) -> i32 {
    if sh.ctx_kind == 1 {
        let slot = sh.ctx_idx;
        let mut rows = 3;                       /* maximise/restore, minimise, close */
        if slot >= 0 && slot < WINS as i32 && !is_pinned(sh, slot) && sh.pinned_n < PIN_MAX {
            rows += 1;                          /* 固定到任务栏 */
        }
        rows
    } else {
        2                                       /* 打开, 取消固定 */
    }
}

fn ctx_h(sh: &Shell) -> i32 {
    ctx_rows(sh) * CTX_ROW_H + 2 * CTX_PAD
}

fn ctx_rect(sh: &Shell) -> (i32, i32, i32, i32) {
    let w = CTX_W;
    let h = ctx_h(sh);
    let x = (sh.ctx_x - 40).clamp(4, SCREEN_W - w - 4);
    let y = (sh.ctx_y - h - 6).clamp(4, TASKBAR_Y - h - 4);
    (x, y, w, h)
}

fn ctx_row_rect(sh: &Shell, r: i32) -> (i32, i32, i32, i32) {
    let (x, y, w, _h) = ctx_rect(sh);
    (x, y + CTX_PAD + r * CTX_ROW_H, w, CTX_ROW_H)
}

/// Row index under a point, or -1.
fn ctx_row_at(sh: &Shell, mx: i32, my: i32) -> i32 {
    if !sh.ctx_open {
        return -1;
    }
    let (x, y, w, h) = ctx_rect(sh);
    if mx < x || mx >= x + w || my < y || my >= y + h {
        return -1;
    }
    let r = (my - y - CTX_PAD) / CTX_ROW_H;
    if r < 0 || r >= ctx_rows(sh) { -1 } else { r }
}

/// Label shown on row `r`.
fn ctx_label(sh: &Shell, r: i32) -> &'static str {
    if sh.ctx_kind == 1 {
        let maxed = sh.ctx_idx >= 0 && sh.ctx_idx < WINS as i32
            && sh.wins[sh.ctx_idx as usize].maximized;
        match r {
            0 => if maxed { "还原" } else { "最大化" },
            1 => "最小化",
            2 => "关闭",
            _ => "固定到任务栏",
        }
    } else if r == 0 {
        "打开"
    } else {
        "取消固定"
    }
}

/// v0.71: switch the desktop style. Saves the config, forces a full frame and
/// (because `layer_key` includes the style) makes every cached window body stale,
/// so the new look is never mixed with the old one.
fn set_style(sh: &mut Shell, v: u8) {
    if v == sh.style || v > STYLE_FLAT {
        return;
    }
    sh.style = v;
    unsafe { STYLE = v; }
    crate::apps::settings_set_style(v);
    /* The wallpaper and the frosted mirror are built once into static caches, so
     * a runtime switch has to rebuild them - otherwise the flat desktop would
     * keep showing the glass gradient behind its opaque panels. */
    unsafe { build_caches(); }
    sh.force_full = true;
    sh.menu_intro = 2;
    log0(if v == STYLE_FLAT { c"style: flat" } else { c"style: glass (fluent)" });
    taskbar_save(sh);
}

/// Persist the pinned list to the FAT32 test volume.
fn taskbar_save(sh: &Shell) {
    let mut buf = [0u8; 96];
    let head = b"pinned=";
    let mut n = 0usize;
    let mut k = 0usize;
    while k < head.len() {
        buf[n] = head[k];
        n += 1;
        k += 1;
    }
    let mut p = 0i32;
    while p < sh.pinned_n && p < PIN_MAX {
        if p > 0 {
            buf[n] = b',';
            n += 1;
        }
        buf[n] = b'0' + (sh.pinned[p as usize].max(0) as u8 % 10);
        n += 1;
        p += 1;
    }
    buf[n] = 10u8;
    n += 1;
    /* v0.71: the desktop style lives in the same config file, so both styles
     * survive a reboot. */
    let style_line: &[u8] = if sh.style == STYLE_FLAT {
        b"style=flat\n"
    } else {
        b"style=glass\n"
    };
    for b in style_line.iter() {
        buf[n] = *b;
        n += 1;
    }
    let rc = unsafe {
        vfs_write_file(b"/mnt/fat/SHELL.CFG\0".as_ptr(), buf.as_ptr(), n as u32)
    };
    unsafe {
        console_printf(
            b"[gui-shell] shell cfg saved: pinned=%d style=%d rc=%d\n\0".as_ptr(),
            sh.pinned_n as i64, sh.style as i64, rc as i64);
    }
}

/// Load the pinned list (falls back to the defaults when the file is missing).
fn taskbar_load(sh: &mut Shell) {
    let mut len: usize = 0;
    let p = unsafe { vfs_read_file(b"/mnt/fat/SHELL.CFG\0".as_ptr(), &mut len) };
    if p.is_null() || len < 8 {
        if !p.is_null() {
            unsafe { lemis_heap_free(p) };
        }
        unsafe {
            console_printf(b"[gui-shell] taskbar pinned defaults (no SHELL.CFG)\n\0".as_ptr());
        }
        return;
    }
    let bytes = unsafe { core::slice::from_raw_parts(p, len) };
    let mut slots = [0i32; 8];
    let mut n = 0usize;
    let mut after_eq = false;
    let mut cur = 0i32;
    let mut have_digit = false;
    for k in 0..len {
        let b = bytes[k];
        if b == b'=' {
            after_eq = true;
            continue;
        }
        if !after_eq {
            continue;
        }
        if b.is_ascii_digit() {
            cur = (b - b'0') as i32;
            have_digit = true;
        } else if b == b',' || b == 10u8 || b == 13u8 {
            if have_digit && n < slots.len() {
                slots[n] = cur;
                n += 1;
            }
            have_digit = false;
            cur = 0;
        }
    }
    if have_digit && n < slots.len() {
        slots[n] = cur;
        n += 1;
    }
    /* v0.71: a second line `style=flat|glass` selects the desktop style. */
    let mut style = STYLE_GLASS;
    let mut q = 0usize;
    while q + 6 < len {
        if bytes[q] == b's' && &bytes[q..q + 6] == b"style=" {
            let mut e = q + 6;
            while e < len && bytes[e] != b'\n' && bytes[e] != b'\r' {
                e += 1;
            }
            if &bytes[q + 6..e] == b"flat" {
                style = STYLE_FLAT;
            }
            break;
        }
        q += 1;
    }
    sh.style = style;
    unsafe { lemis_heap_free(p) };
    if n > 0 {
        sh.pinned_n = n.min(PIN_MAX as usize) as i32;
        let mut k = 0usize;
        while k < slots.len() {
            sh.pinned[k] = slots[k];
            k += 1;
        }
        unsafe {
            console_printf(b"[gui-shell] shell cfg loaded: pinned=%d style=%d from SHELL.CFG
 ".as_ptr(),
                       sh.pinned_n as i64, sh.style as i64);
        }
    } else {
        unsafe {
            console_printf(b"[gui-shell] shell cfg defaults (empty SHELL.CFG)
 ".as_ptr());
        }
    }
}

fn taskbar_pin(sh: &mut Shell, slot: usize) {
    if sh.pinned_n >= PIN_MAX || is_pinned(sh, slot as i32) {
        return;
    }
    sh.pinned[sh.pinned_n as usize] = slot as i32;
    sh.pinned_n += 1;
    log2(c"pin add", slot as i32, sh.pinned_n);
    taskbar_save(sh);
    sh.force_full = true;
}

fn taskbar_unpin(sh: &mut Shell, p: i32) {
    if p < 0 || p >= sh.pinned_n {
        return;
    }
    let slot = sh.pinned[p as usize];
    let mut k = p;
    while k + 1 < sh.pinned_n {
        sh.pinned[k as usize] = sh.pinned[(k + 1) as usize];
        k += 1;
    }
    sh.pinned_n -= 1;
    sh.pinned[sh.pinned_n.max(0) as usize] = -1;
    log2(c"pin unpin", slot, sh.pinned_n);
    taskbar_save(sh);
    sh.force_full = true;
}

/// Run row `r` of the open context menu.
fn ctx_act(sh: &mut Shell, r: i32) {
    if sh.ctx_kind == 1 {
        let slot = sh.ctx_idx;
        if slot < 0 || slot >= WINS as i32 {
            sh.ctx_open = false;
            return;
        }
        let i = slot as usize;
        match r {
            0 => {
                let want = !sh.wins[i].maximized;
                request_maximize(sh, i, want);
                log2(c"ctx act maximize", slot, want as i32);
            }
            1 => {
                begin_act(sh, 1, i);
                log2(c"ctx act minimize", slot, 0);
            }
            2 => {
                begin_act(sh, 2, i);
                log2(c"ctx act close", slot, 0);
            }
            _ => {
                taskbar_pin(sh, i);
                log2(c"ctx act pin", slot, sh.pinned_n);
            }
        }
    } else {
        let p = sh.ctx_idx;
        if p < 0 || p >= sh.pinned_n {
            sh.ctx_open = false;
            return;
        }
        if r == 0 {
            let slot = sh.pinned[p as usize].max(0) as usize;
            launch_window(sh, slot);
            log2(c"ctx act open", slot as i32, p);
        } else {
            taskbar_unpin(sh, p);
            log2(c"ctx act unpin", p, sh.pinned_n);
        }
    }
    sh.ctx_open = false;
    sh.force_full = true;
}

/// Draw the open context menu (after the taskbar, before the start menu).
fn draw_ctx_menu(s: &mut Surface<'_>, sh: &Shell) {
    if !sh.ctx_open {
        return;
    }
    let (x, y, w, h) = ctx_rect(sh);
    if flat_ui() {
        /* v0.71 flat popup: opaque panel + hairline, no shadow, no acrylic */
        s.round_rect(x as f32, y as f32, (x + w) as f32, (y + h) as f32,
                     8.0, FLAT_MENU, 255);
        s.round_rect_stroke(x as f32 + 0.5, y as f32 + 0.5,
                            (x + w) as f32 - 0.5, (y + h) as f32 - 0.5,
                            8.0, FLAT_LINE, 255);
    } else {
    s.rect_fill(x + 3, y + 4, x + w + 3, y + h + 5, 0x000000, 0x52);
    frosted(s, x, y, x + w, y + h, 10.0, 0x44);
    s.rect_fill(x, y, x + w, y + 1, 0xffffff, 0x3a);
    s.rect_fill(x, y + h - 1, x + w, y + h, 0x000000, 0x30);
    }
    let rows = ctx_rows(sh);
    let mut r = 0;
    while r < rows {
        let (rx, ry, rw, rh) = ctx_row_rect(sh, r);
        let selected = r == sh.ctx_sel;
        if selected {
            rrow(s, rx + 4, ry + 2, rx + rw - 4, ry + rh - 2, 6, 0xffffff, 0x24);
        }
        let danger = (sh.ctx_kind == 1 && r == 2) || (sh.ctx_kind != 1 && r == 1);
        s.text(rx + 14, ry + 8, ctx_label(sh, r),
               if danger { 0xffbdbd } else { 0xe8eefb },
               if selected { 0xff } else { 0xe0 }, 1);
        r += 1;
    }
}

/* v0.64: the start menu is a registry of rows. A row either opens a window slot
 * or runs a shell action, so the menu can grow past the window count without
 * inventing a new window for every command. One accent per row, shared by the
 * row icon, the window title bar icon and the pseudo-3D plate (matches
 * Win::accent). */
pub enum MenuTarget {
    Window(usize),
    ExitToTerminal,
}

pub struct MenuRow {
    pub label: &'static str,
    pub accent: u32,
    pub target: MenuTarget,
}

pub const MENU_ROWS: [MenuRow; 7] = [
    MenuRow { label: "系统设置", accent: 0x4f8dff, target: MenuTarget::Window(0) },
    MenuRow { label: "任务管理器", accent: 0x22c1a8, target: MenuTarget::Window(1) },
    MenuRow { label: "图片查看器", accent: 0xf5a524, target: MenuTarget::Window(2) },
    MenuRow { label: "文本编辑器", accent: 0xe05a8a, target: MenuTarget::Window(3) },
    MenuRow { label: "Wayland 客户端", accent: 0x7a5cff, target: MenuTarget::Window(4) },
    MenuRow { label: "计算器", accent: crate::apps::ACC_CALC, target: MenuTarget::Window(5) },
    /* the 7th row is an action, not a window: it leaves the desktop the same way
     * ESC does - this is what makes the menu longer than the panel. */
    MenuRow { label: "退出到终端", accent: 0x8b94a6, target: MenuTarget::ExitToTerminal },
];

/// Start menu row under (mx, my) for the given scroll offset, -1 when none.
fn menu_row_at(mx: i32, my: i32, scroll: usize) -> i32 {
    let count = menu_visible_rows();
    let mut k = 0usize;
    while k < count {
        let idx = scroll + k;
        if idx >= MENU_ROWS.len() {
            break;
        }
        let y = menu_items_y() + k as i32 * MENU_ROW_H;
        if mx >= MENU_X + 10 && mx < MENU_X + MENU_W - 10 && my >= y && my < y + 38 {
            return idx as i32;
        }
        k += 1;
    }
    -1
}

/// v0.64: scroll the list by `dir` rows, bounded. Returns true when it moved.
fn menu_scroll_by(sh: &mut Shell, dir: i32) -> bool {
    let max = menu_scroll_max() as i32;
    let cur = sh.menu_scroll as i32;
    let next = (cur + dir).clamp(0, max);
    if next != cur {
        sh.menu_scroll = next as usize;
        log2(c"menu scroll", next, max);
        true
    } else {
        false
    }
}

/// Keep the keyboard selection inside the visible window.
fn menu_scroll_into_view(sh: &mut Shell) {
    let sel = sh.menu_sel.max(0) as usize;
    let vis = menu_visible_rows();
    let cur = sh.menu_scroll;
    let mut want = cur;
    if sel < cur {
        want = sel;
    }
    if sel >= cur + vis {
        want = sel + 1 - vis;
    }
    if want != cur {
        sh.menu_scroll = want.min(menu_scroll_max());
        log2(c"menu scroll", sh.menu_scroll as i32, menu_scroll_max() as i32);
    }
}

/// The scrollbar strip (right margin of the panel). Returns -1 (up) / +1 (down)
/// when the click landed in its upper / lower half.
fn menu_scroll_click(cx: i32, cy: i32) -> Option<i32> {
    if MENU_ROWS.len() <= MENU_VISIBLE {
        return None;
    }
    let x0 = MENU_X + MENU_W - 13;
    let x1 = MENU_X + MENU_W - 4;
    let y0 = menu_items_y();
    let y1 = y0 + menu_visible_rows() as i32 * MENU_ROW_H;
    if cx < x0 || cx >= x1 || cy < y0 || cy >= y1 {
        return None;
    }
    Some(if cy < (y0 + y1) / 2 { -1 } else { 1 })
}

/// v0.64: run a menu row (window slot or shell action).
fn menu_activate(sh: &mut Shell, idx: usize) {
    if idx >= MENU_ROWS.len() {
        return;
    }
    match MENU_ROWS[idx].target {
        MenuTarget::Window(slot) => launch_window(sh, slot),
        MenuTarget::ExitToTerminal => {
            unsafe {
                EXIT_REQ = true;
            }
            sh.menu_open = false;
            log0(c"menu: exit to terminal");
        }
    }
}

/// Which built-in key handler a window owns (v0.64).
#[derive(Clone, Copy, PartialEq)]
pub enum KeySink {
    /// the window consumes no keys but still has no one-line input band
    None,
    Editor,
    Calc,
    Image,
}

/// One row of the keyboard-ownership registry.
pub struct KeyOwner {
    pub slot: usize,
    pub accent: u32,
    pub name: &'static str,
    pub sink: KeySink,
}

/* v0.64: per-window keyboard ownership registry.
 *
 * Before this, the frame step carried an if-chain (editor / calculator /
 * picture viewer / generic input band) and `draw_focus_line` repeated the same
 * knowledge as a hard-coded skip list of window indices.
 * Every new application had to be added in two places and the two could
 * disagree - a window that consumed keys but stayed off the skip list got the
 * input band painted over its content (that is exactly what happened to the
 * calculator in v0.63). The table below is now the only source of truth:
 *   - the frame step walks it instead of the if-chain,
 *   - `draw_focus_line` asks it whether the window hosts the input band,
 *   - `keys` counts how many windows own the keyboard so an acceptance can
 *     assert the table the shell really walks.
 * A window that is NOT in this table is a plain text-input window. */
pub const KEY_OWNERS: [KeyOwner; 4] = [
    KeyOwner { slot: 2, accent: crate::apps::ACC_IMAGE, name: "image", sink: KeySink::Image },
    KeyOwner { slot: 3, accent: crate::apps::ACC_EDITOR, name: "editor", sink: KeySink::Editor },
    /* the Wayland compositor view paints client pixels edge to edge and has no
     * text line of its own (its keys go to the client, not to a shell band) */
    KeyOwner { slot: 4, accent: crate::apps::ACC_WAYLAND, name: "wayland", sink: KeySink::None },
    KeyOwner { slot: 5, accent: crate::apps::ACC_CALC, name: "calc", sink: KeySink::Calc },
];

/// Registry lookup: (window slot, accent) -> owning row.
pub fn key_owner_for(slot: usize, accent: u32) -> Option<&'static KeyOwner> {
    let mut k = 0usize;
    while k < KEY_OWNERS.len() {
        if KEY_OWNERS[k].slot == slot && KEY_OWNERS[k].accent == accent {
            return Some(&KEY_OWNERS[k]);
        }
        k += 1;
    }
    None
}

/// Bit per owning slot (used by the boot log and the acceptance).
pub fn key_owner_mask() -> i32 {
    let mut mask = 0i32;
    let mut k = 0usize;
    while k < KEY_OWNERS.len() {
        mask |= 1 << KEY_OWNERS[k].slot;
        k += 1;
    }
    mask
}

/// v0.65: editor keys (was an inline if-chain branch in `shell_step`).
/// v0.66: Ctrl+Z / Ctrl+Y / Ctrl+S and PageUp/PageDown; the modifier mask comes
/// from the kernel's PS/2 decoder (`mbos_kbd_mod_state`).
fn editor_keys(mods: i32, key: i32, ch: i32) -> bool {
    /* v0.66: the PS/2 decoder already folded Ctrl+<letter> into an ASCII control
     * code, so the shortcut does not depend on a modifier snapshot at all. */
    match ch {
        0x1a => {                                    /* Ctrl+Z */
            crate::apps::editor_undo();
            unsafe {
                WIN_KEY[3] = i32::MIN;
            }
            log2(c"editor ctrl", key, ch);
            return true;
        }
        0x19 | 0x9a => {                             /* Ctrl+Y / Ctrl+Shift+Z */
            crate::apps::editor_redo();
            unsafe {
                WIN_KEY[3] = i32::MIN;
            }
            log2(c"editor ctrl", key, ch);
            return true;
        }
        0x13 => {                                    /* Ctrl+S */
            editor_save();
            log2(c"editor ctrl", key, ch);
            return true;
        }
        _ => {}
    }
    /* bit0 = ctrl (MBOS_KBD_CTRL), bit1 = alt, bit2 = shift */
    if (mods & 1) != 0 {
        match key {
            0x2c => crate::apps::editor_undo(),      /* Ctrl+Z */
            0x15 => crate::apps::editor_redo(),      /* Ctrl+Y */
            0x1f => editor_save(),                   /* Ctrl+S */
            _ => return false,        /* Ctrl+<other> must not insert a letter */
        }
        unsafe {
            WIN_KEY[3] = i32::MIN;
        }
        log2(c"editor ctrl", key, mods);
        return true;
    }
    let mut handled = true;
    match key {
        0x4b => crate::apps::editor_move(1),     /* left  */
        0x4d => crate::apps::editor_move(2),     /* right */
        0x48 => crate::apps::editor_move(3),     /* up    */
        0x50 => crate::apps::editor_move(4),     /* down  */
        0x47 => crate::apps::editor_move(5),     /* home  */
        0x4f => crate::apps::editor_move(6),     /* end   */
        0x49 => crate::apps::editor_move(7),     /* pgup  */
        0x51 => crate::apps::editor_move(8),     /* pgdn  */
        0x3f => editor_save(),                   /* F5    */
        _ => {
            if ch == 0x08 || ch == 0x0a || ch == 0x0d || (ch >= 0x20 && ch < 0x7f) {
                crate::apps::editor_key(ch as u8);
            } else {
                handled = false;
            }
        }
    }
    if handled {
        unsafe {
            WIN_KEY[3] = i32::MIN;
        }
        log2(c"editor key", key, ch);
    }
    handled
}

/// v0.64: calculator keys (digits, operators, '.', '%', '=', C, Backspace and
/// the m/n/r/l memory shortcuts; anything else falls through).
fn calc_keys(key: i32, ch: i32) -> bool {
    if crate::apps::calc_key(key, ch) {
        unsafe {
            WIN_KEY[5] = i32::MIN;
        }
        log2(c"calc key", key, ch);
        true
    } else {
        false
    }
}

/// v0.64: picture viewer keys (Left/Right browse the supported files).
fn image_keys(key: i32, ch: i32) -> bool {
    let _ = ch;
    if key == 0x4b || key == 0x4d {
        let dir = if key == 0x4b { -1 } else { 1 };
        if crate::apps::image_shift(dir) {
            unsafe {
                WIN_KEY[2] = i32::MIN;
            }
            /* 1 = next, 2 = previous (log args are read as unsigned) */
            log2(c"image shift", if dir > 0 { 1 } else { 2 }, 0);
            return true;
        }
    }
    false
}

/// v0.64: run the focused window's handler and flag the layer/damage work it
/// implies. Returns true when the window's pixels changed.
fn dispatch_owned_key(sh: &mut Shell, owner: &KeyOwner, mods: i32, key: i32,
                      ch: i32) -> bool {
    let changed = match owner.sink {
        KeySink::Editor => editor_keys(mods, key, ch),
        KeySink::Calc => calc_keys(key, ch),
        KeySink::Image => image_keys(key, ch),
        KeySink::None => false,
    };
    if changed {
        match owner.sink {
            KeySink::Editor => sh.dirty_editor = true,
            KeySink::Calc => sh.dirty_calc = true,
            KeySink::Image => sh.dirty_image = true,
            KeySink::None => {}
        }
    }
    let _ = owner.name;
    changed
}

/// v0.64: set by the start menu's 退出到终端 row; the C runner polls it after
/// every frame and leaves the desktop loop (the same exit ESC performs).
static mut EXIT_REQ: bool = false;

#[no_mangle]
pub extern "C" fn suture_gui_shell_exit() -> i32 {
    unsafe {
        if EXIT_REQ {
            1
        } else {
            0
        }
    }
}

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
fn composite_scaled(scene: &mut [u32], stride: usize, slot: usize, r: &Win,
                    clip: Rect) -> bool {
    if slot >= WINS {
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
    let x0 = r.x.max(0).max(clip.x0);
    let y0 = r.y.max(0).max(clip.y0);
    let x1 = (r.x + r.w).min(SCREEN_W).min(clip.x1);
    let y1 = (r.y + r.h).min(SCREEN_H).min(clip.y1);
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

fn hit_win(wins: &[Win; WINS], mx: i32, my: i32, focus: i32) -> i32 {
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
            if !s.in_clip(dst_x, dst_y) {
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
    /* this animation dims the frame it is about to show, so it paints into the
     * buffer that holds the desktop and blits it unconditionally - no swap
     * happens on the shutdown path. */
    let sh_buf = if PRESENT_BUF >= 0 { PRESENT_BUF as usize } else { DRAW_BUF };
    let frame = buf_slice(sh_buf);
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
        let frame: &[u32] = core::slice::from_raw_parts(
            buf_ptr(sh_buf), (SCREEN_W as usize) * (SCREEN_H as usize));
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

// ------------------------------------------------------------- frame scratch
/// Scalar snapshot of one frame: everything the compose pass reads that is not
/// already in `Shell`, captured once so damage detection, composition and the
/// oracle all draw exactly the same values.
struct FrameCtx {
    now: u32,
    now_ns: u64,
    mx: i32,
    my: i32,
    clock: i32,
    hover_wb: i32,
    hover_start: bool,
    menu_hover: i32,
    act_alpha: u32,
    act_idx: i32,
    intro_alpha: u32,
    intro_idx: i32,
    menu_alpha: u32,
    scale: bool,
    scale_idx: i32,
    scale_rect: [i32; 4],
    live_tick: u32,
    /// Clock handed to `apps::draw_live`: pinned to the live refresh tick so the
    /// heartbeat dot has a deterministic phase (see `live_tick` in shell_step).
    live_now: u32,
}

/// Screen rectangle of the arrow cursor (drawn into the framebuffer only, never
/// into a scene buffer).
#[inline]
fn cursor_rect(x: i32, y: i32) -> Rect {
    if x < 0 || y < 0 {
        return Rect::EMPTY;
    }
    Rect::new(x, y, x + CUR_W, y + CUR_H).expand(1)
}

/// Rectangle of the focused window's editable line; mirrors `draw_focus_line`.
#[inline]
fn input_line_rect(w: &Win) -> Rect {
    if !w.open || w.minimized {
        return Rect::EMPTY;
    }
    /* v0.71: 系统设置 draws its hint at the bottom of the body (the default
     * position sat on the clickable 外观风格 row), so the damage rect has to
     * follow it - otherwise typing never repaints that band and the damage
     * oracle catches a mismatch. Keep this rule identical to draw_focus_line. */
    let y = if w.accent == crate::apps::ACC_SETTINGS {
        (w.y + w.h - 30).max(w.y + TITLE_H + 8)
    } else {
        w.y + TITLE_H + 104
    };
    if !(y + 20 < w.y + w.h) || w.y < 0 || w.w < 80 {
        return Rect::EMPTY;
    }
    Rect::new(w.x + 16, y - 4, w.x + w.w - 16, y + 20).expand(2)
}

/// Rectangles of the task manager's live values: the uptime line, the CPU% line
/// and the 8x8 heartbeat dot, exactly where `apps::draw_live` paints them
/// (TM_UPTIME_Y = TITLE_H + 138 at +92 px, TM_CPU_Y = TITLE_H + 164 at +46 px,
/// dot at TITLE_H + 12 near the right edge). Three small rectangles keep a
/// once-a-second invalidation at ~2 500 px instead of 65 000.
#[inline]
fn live_rects(w: &Win, out: &mut [Rect; 3]) {
    let x0 = w.x + 16 + 4;
    let y1 = w.y + TITLE_H + 138;
    let y2 = w.y + TITLE_H + 164;
    out[0] = Rect::new(x0 + 52, y1 - 2, x0 + 200, y1 + 18);
    out[1] = Rect::new(x0 + 6, y2 - 2, x0 + 140, y2 + 18);
    out[2] = Rect::new(w.x + w.w - 30, w.y + TITLE_H + 10,
                       w.x + w.w - 12, w.y + TITLE_H + 22);
}

#[inline]
fn is_taskman(w: &Win) -> bool {
    w.accent == crate::apps::ACC_TASKMAN
}

// --------------------------------------------------------------- buffer state
/// Pick the buffer this frame renders into and put it in DRAW state.
unsafe fn begin_frame() -> usize {
    /* a frame is still waiting for its grid slot: compose the next one into a
     * different buffer (carrying the complete image over) instead of clobbering
     * the pending one - this is what the third buffer is for. */
    if READY_BUF >= 0 {
        let src = READY_BUF as usize;
        let mut free = -1i32;
        for i in 0..3 {
            if BUF_STATE[i] == BUF_FREE && i != src {
                free = i as i32;
                break;
            }
        }
        if free >= 0 {
            let f = free as usize;
            buf_slice(f).copy_from_slice(buf_slice(src));
            BUF_VALID[f] = BUF_VALID[src];
            BUF_STATE[f] = BUF_DRAW;
            DRAW_BUF = f;
            SWAP.carry_copies = SWAP.carry_copies.wrapping_add(1);
            return f;
        }
        /* no third buffer available: the pending frame is superseded */
        BUF_STATE[src] = BUF_DRAW;
        DRAW_BUF = src;
        return src;
    }
    /* normal path: the PRESENTED buffer holds exactly what is on the screen, so
     * it is the natural base for a damage frame */
    if PRESENT_BUF >= 0 {
        let p = PRESENT_BUF as usize;
        if BUF_STATE[p] == BUF_PRESENTED {
            BUF_STATE[p] = BUF_DRAW;
            DRAW_BUF = p;
            PRESENT_BUF = -1;
            return p;
        }
    }
    let d = DRAW_BUF;
    BUF_STATE[d] = BUF_DRAW;
    d
}

/// Hand the drawn buffer over to the present stage (DRAW -> READY).
unsafe fn finish_frame() {
    let d = DRAW_BUF;
    if READY_BUF >= 0 && READY_BUF != d as i32 {
        BUF_STATE[READY_BUF as usize] = BUF_FREE;
        BUF_VALID[READY_BUF as usize] = false;
        /* the newer frame replaced it before it was shown */
        SWAP.skipped = SWAP.skipped.wrapping_add(1);
        SWAP.dropped = SWAP.dropped.wrapping_add(1);
    }
    BUF_STATE[d] = BUF_READY;
    BUF_VALID[d] = true;
    READY_BUF = d as i32;
    READY_PRODUCED_NS = time_ns();
    SWAP.produced = SWAP.produced.wrapping_add(1);
}

// ------------------------------------------------------------------- composing
/// Copy the wallpaper cache into `r`.
fn copy_wall(s: &mut Surface<'_>, r: Rect) {
    let wall = unsafe { &*core::ptr::addr_of!(WALL) };
    let x0 = r.x0.max(0) as usize;
    let x1 = (r.x1 as usize).min(s.w).min(SCREEN_W as usize);
    let y0 = r.y0.max(0) as usize;
    let y1 = (r.y1 as usize).min(s.h).min(SCREEN_H as usize);
    if x1 <= x0 || y1 <= y0 {
        return;
    }
    for y in y0..y1 {
        let d = y * s.stride;
        let w = y * SCREEN_W as usize;
        s.px[d + x0..d + x1].copy_from_slice(&wall[w + x0..w + x1]);
    }
}

/// Taskbar strip: acrylic sample, start button, logo, window buttons, clock.
fn draw_taskbar(s: &mut Surface<'_>, sh: &Shell, ctx: &FrameCtx) {
    if flat_ui() {
        /* v0.71 flat strip: one opaque band + one hairline, no acrylic */
        s.rect_fill(0, TASKBAR_Y, SCREEN_W, SCREEN_H, FLAT_BAR, 255);
        s.rect_fill(0, TASKBAR_Y, SCREEN_W, TASKBAR_Y + 1, FLAT_LINE, 255);
    } else {
    frosted(s, 0, TASKBAR_Y, SCREEN_W, SCREEN_H, 0.0, 0x30);
    s.rect_fill(0, TASKBAR_Y, SCREEN_W, SCREEN_H, 0xffffff, 0x08);
    /* v0.60 polish: 1 px top hairline and a darker bottom edge give the strip a
     * glass thickness instead of a flat band */
    s.rect_fill(0, TASKBAR_Y, SCREEN_W, TASKBAR_Y + 1, 0xffffff, 0x26);
    s.rect_fill(0, SCREEN_H - 1, SCREEN_W, SCREEN_H, 0x000000, 0x2c);
    }
    let start_btn = (8, TASKBAR_Y + 5, 34, 28);
    rrow(s, start_btn.0, start_btn.1, start_btn.0 + start_btn.2,
         start_btn.1 + start_btn.3, 7, 0xffffff,
         if ctx.hover_start { 0x2c } else { 0x16 });
    /* v0.57.1: the real SutureOS logo (24px step of the asset ladder) */
    let logo = unsafe { &*core::ptr::addr_of!(g_logo24) };
    blit_logo_region(s, logo, LOGO_SMALL_SIZE, 0, 0,
                     LOGO_SMALL_SIZE as i32, LOGO_SMALL_SIZE as i32,
                     start_btn.0 + 5, start_btn.1 + 2);
    /* v0.68: pinned application buttons (only while the app is closed) */
    let mut p = 0i32;
    while p < sh.pinned_n && p < PIN_MAX {
        let slot = pin_slot(sh, p);
        if slot >= 0 && slot < WINS as i32 && !sh.wins[slot as usize].open {
            let (bx, bw) = pin_rect(p);
            let hovered = ctx.hover_wb == -p - 2;
            rrow(s, bx, TASKBAR_Y + 6, bx + bw, TASKBAR_Y + TASKBAR_H - 6, 8,
                 0xffffff, if hovered { 0x2a } else { 0x10 });
            draw_app_icon(s, bx + 11, TASKBAR_Y + 7, 24, sh.wins[slot as usize].accent,
                          glyph_for_accent(sh.wins[slot as usize].accent));
        }
        p += 1;
    }
    for i in 0..WINS {
        if let Some((bx, bw)) = wb_rect(sh, i) {
            let hovered = ctx.hover_wb == i as i32;
            let active = sh.focus == i && !sh.wins[i].minimized;
            rrow(s, bx, TASKBAR_Y + 6, bx + bw, TASKBAR_Y + TASKBAR_H - 6, 7,
                 0xffffff,
                 if active { 0x30 } else if hovered { 0x20 } else { 0x12 });
            if active {
                /* Fluent 11 cue: a rounded underline hugging the strip's bottom */
                rrow(s, bx + 14, TASKBAR_Y + TASKBAR_H - 9, bx + bw - 14,
                     TASKBAR_Y + TASKBAR_H - 6, 1, 0x4f8dff, 255);
            }
            s.text(bx + 10, TASKBAR_Y + 12, sh.wins[i].title, 0xe8eefb,
                   if sh.wins[i].minimized { 0x90 } else { 0xf0 }, 1);
        }
    }
    /* clock on the right: real wall time from the RTC (v0.44.0) */
    let hh = (ctx.clock / 100) % 24;
    let mm = ctx.clock % 100;
    let digits = [b'0' + (hh / 10) as u8, b'0' + (hh % 10) as u8, b':',
                  b'0' + (mm / 10) as u8, b'0' + (mm % 10) as u8];
    let mut x = SCREEN_W - 96;
    for ch in digits.iter() {
        x += s.char_ascii(x, TASKBAR_Y + 12, *ch as char, 0xd6e2f5, 0xd0, 1);
    }
}

/// Start menu block (drawn only while the menu is open).
fn draw_menu(s: &mut Surface<'_>, sh: &Shell, ctx: &FrameCtx) {
    let (my, mh) = (menu_y(), menu_h());
    if flat_ui() {
        /* v0.71 flat panel: opaque fill + 1 px outline, no shadow, no blur */
        s.round_rect(MENU_X as f32, my as f32, (MENU_X + MENU_W) as f32,
                     (my + mh) as f32, 10.0, FLAT_MENU, 255);
        s.round_rect_stroke(MENU_X as f32 + 0.5, my as f32 + 0.5,
                            (MENU_X + MENU_W) as f32 - 0.5,
                            (my + mh) as f32 - 0.5, 10.0, FLAT_LINE, 255);
        s.rect_fill(MENU_X + 1, my + 46, MENU_X + MENU_W - 1, my + 47,
                    FLAT_LINE, 255);
    } else {
    s.rect_fill(MENU_X + 3, my + 10, MENU_X + MENU_W + 3, my + mh + 12,
                0x000000, 0x44);
    frosted(s, MENU_X, my, MENU_X + MENU_W, my + mh, 14.0, 0x34);
    s.rect_fill(MENU_X + 10, my, MENU_X + MENU_W - 10, my + 1, 0xffffff, 0x3c);
    s.rect_fill(MENU_X + 10, my + mh - 1, MENU_X + MENU_W - 10, my + mh,
                0xffffff, 0x3c);
    s.rect_fill(MENU_X, my + 10, MENU_X + 1, my + mh - 10, 0xffffff, 0x3c);
    s.rect_fill(MENU_X + MENU_W - 1, my + 10, MENU_X + MENU_W, my + mh - 10,
                0xffffff, 0x3c);
    }
    /* v0.57.1: start-menu icon = 48px step of the logo ladder */
    let logo = unsafe { &*core::ptr::addr_of!(g_logo48) };
    blit_logo_region(s, logo, LOGO_MENU_SIZE, 0, 0,
                     LOGO_MENU_SIZE as i32, LOGO_MENU_SIZE as i32,
                     MENU_X + 10, my + 2);
    s.text(MENU_X + 68, my + 16, "常用应用", 0xffffff, 0xf0, 1);
    /* v0.64: only the visible slice of the registry is drawn; MENU_ROWS can be
     * longer than the panel. */
    let first = sh.menu_scroll.min(menu_scroll_max());
    let count = menu_visible_rows();
    let mut k = 0usize;
    while k < count {
        let idx = first + k;
        if idx >= MENU_ROWS.len() {
            break;
        }
        let row = &MENU_ROWS[idx];
        let y = menu_items_y() + k as i32 * MENU_ROW_H;
        let hovered = ctx.menu_hover == idx as i32;
        let selected = idx as i32 == sh.menu_sel;
        rrow(s, MENU_X + 10, y, MENU_X + MENU_W - 10, y + 38, 8, 0xffffff,
             if hovered { 0x2c } else if selected { 0x22 } else { 0x12 });
        let accent = row.accent;
        if hovered || selected {
            s.rect_fill(MENU_X + 10, y + 5, MENU_X + 13, y + 33, accent, 0xd8);
        }
        draw_app_icon(s, MENU_X + 18, y + 8, 22, accent, idx as i32);
        s.text(MENU_X + 50, y + 11, row.label, 0xeef4ff, 0xe8, 1);
        k += 1;
    }
    /* scrollbar: a track in the panel's right margin plus a proportional thumb
     * (only when the registry is longer than the panel) */
    if MENU_ROWS.len() > MENU_VISIBLE {
        let tx0 = MENU_X + MENU_W - 12;
        let tx1 = MENU_X + MENU_W - 6;
        let ty0 = menu_items_y() + 2;
        let ty1 = menu_items_y() + count as i32 * MENU_ROW_H - 2;
        s.rect_fill(tx0, ty0, tx1, ty1, 0x000000, 0x38);
        let track = ty1 - ty0;
        let thumb_h = (track * count as i32 / MENU_ROWS.len() as i32).max(20);
        let max = menu_scroll_max().max(1) as i32;
        let ny = ty0 + (track - thumb_h) * first as i32 / max;
        s.rect_fill(tx0, ny, tx1, ny + thumb_h, 0xa9c4ff, 0x88);
    }
    /* v0.52.0: 2-frame fade-in for the start menu (dim -> clear) */
    if ctx.menu_alpha > 0 {
        s.rect_fill(MENU_X, my, MENU_X + MENU_W, my + mh + 8,
                    0x000000, ctx.menu_alpha);
    }
}

/// Keyboard-focus indicator: the focused window shows an editable line.
fn draw_focus_line(s: &mut Surface<'_>, sh: &Shell, _ctx: &FrameCtx) {
    let fi = sh.focus;
    let w = sh.wins[fi];
    /* v0.71: 系统设置 draws its hint at the bottom of the body - the default
     * position (TITLE_H + 104) sits right on top of the clickable 外观风格 row. */
    let y = if w.accent == crate::apps::ACC_SETTINGS {
        (w.y + w.h - 30).max(w.y + TITLE_H + 8)
    } else {
        w.y + TITLE_H + 104
    };
    /* v0.59.x: the image viewer has no text content, and when it shows a
     * committed Wayland surface the focus hint would sit on top of the client's
     * pixels - skip the editable line for that window. */
    /* v0.63: windows that consume the keyboard themselves (picture viewer,
     * editor, Wayland surface, calculator) must not get the input band - on the
     * calculator it sat right on top of the 4x4 keypad. */
    /* v0.71: 系统设置 is a click-driven window (the 外观风格 row toggles the
     * desktop style), so the free-text input band no longer belongs there - it
     * was drawn straight across that row. */
    if w.open && !w.minimized && key_owner_for(fi, w.accent).is_none()
        && y + 20 < w.y + w.h && w.y >= 0 {
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

/// v0.58.0 maximise/restore transition: the geometry already jumped to its
/// target, so the animation samples the HOME-anchored body layer into the
/// interpolated rectangle.
fn draw_scale_transition(s: &mut Surface<'_>, wu: usize, sh: &Shell, ctx: &FrameCtx) {
    let idx = ctx.scale_idx;
    if idx < 0 || idx >= WINS as i32 {
        return;
    }
    let w = sh.wins[idx as usize];
    if !w.open || w.minimized {
        return;
    }
    let mut tmp = w;
    tmp.x = ctx.scale_rect[0];
    tmp.y = ctx.scale_rect[1];
    tmp.w = ctx.scale_rect[2].max(24);
    tmp.h = ctx.scale_rect[3].max(24);
    /* v0.68: draw the window itself at the interpolated rectangle. The old path
     * sampled the cached home-anchored layer, so windows whose layer had a
     * different size/age (the ones opened at startup vs. the ones opened from the
     * menu) animated visibly differently. One direct render = one animation. */
    draw_window(s, &tmp, true);
    unsafe {
        WIN_KEY[idx as usize] = i32::MIN;
    }
    let _ = (wu, s.clip);
}

/// Compose the rectangle `r` of the desktop from scratch: wallpaper, window
/// layers, live overlays, animations, focus line, taskbar, start menu.
///
/// This is the ONLY composition path: a full repaint is `compose_rect` over the
/// whole screen, a damage frame is the same function over the damaged
/// rectangles. That is what makes the two identical by construction; the
/// `damage: identical` oracle verifies that the damage list was complete.
fn compose_rect(scene: &mut [u32], wu: usize, sh: &mut Shell, r: Rect, ctx: &FrameCtx) {
    let mut s = Surface {
        px: scene,
        w: wu,
        h: SCREEN_H as usize,
        stride: wu,
        clip: (r.x0, r.y0, r.x1, r.y1),
    };
    /* 1. wallpaper */
    copy_wall(&mut s, r);
    /* 2. windows bottom-up (cached body layer + freshly blended shadow) */
    let mut order = [0usize, 1, 2, 3, 4, 5];
    for a in 0..(WINS - 1) {
        for b in (a + 1)..WINS {
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
        if ctx.scale && ctx.scale_idx == i as i32 {
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
                crate::apps::draw_live(&mut s, win.accent, win.x, win.y, win.w,
                                       win.h, ctx.live_now);
                continue;
            }
        } else {
            sh.prof_cache_hit += 1;
        }
        draw_window_shadow(&mut s, &win);
        let scene_ref = &mut s.px[..];
        composite_window_layer(scene_ref, wu, i, &win, r);
        /* 3. live per-window content (task manager counters + heartbeat dot) is
         * painted right after ITS layer, inside the bottom-up loop: a window
         * above it then covers it like any other content. Painting the whole live
         * pass after the loop (the v0.59 behaviour) let the task manager's
         * heartbeat dot show through the editor window - the layer caches stay
         * static either way. */
        crate::apps::draw_live(&mut s, win.accent, win.x, win.y, win.w, win.h,
                               ctx.live_now);
    }
    /* 4. v0.49.0: 2-frame fade-out before a close/minimize actually happens */
    if ctx.act_alpha > 0 {
        let idx = ctx.act_idx;
        if idx >= 0 && idx < WINS as i32 {
            let w = sh.wins[idx as usize];
            if w.open && !w.minimized {
                s.rect_fill(w.x, w.y, w.x + w.w, w.y + w.h, 0x000000, ctx.act_alpha);
            }
        }
    }
    /* 5. v0.48.0: 3-frame open fade (also used after minimise/restore) */
    if ctx.intro_alpha > 0 {
        let idx = ctx.intro_idx;
        if idx >= 0 && idx < WINS as i32 {
            let w = sh.wins[idx as usize];
            if w.open {
                s.rect_fill(w.x, w.y, w.x + w.w, w.y + w.h, 0x000000, ctx.intro_alpha);
            }
        }
    }
    /* 6. maximise/restore transition */
    if ctx.scale {
        draw_scale_transition(&mut s, wu, sh, ctx);
    }
    /* 7. keyboard-focus indicator */
    if !sh.menu_open && !sh.shutting {
        draw_focus_line(&mut s, sh, ctx);
    }
    /* 8. taskbar */
    draw_taskbar(&mut s, sh, ctx);
    /* 9. start menu */
    if sh.menu_open {
        draw_menu(&mut s, sh, ctx);
    }
    /* 10. v0.69: taskbar context menu (topmost) */
    draw_ctx_menu(&mut s, sh);
}

/// Compose one list of rectangles into `buf`.
unsafe fn compose_list(buf: usize, wu: usize, sh: &mut Shell, rects: &[Rect; MAX_DAMAGE],
                       n: usize, ctx: &FrameCtx) {
    for k in 0..n {
        if rects[k].is_empty() {
            continue;
        }
        let scene = buf_slice(buf);
        compose_rect(&mut scene[..], wu, sh, rects[k], ctx);
    }
}

// -------------------------------------------------------------------- present
/// Blit the pending damage to the framebuffer, draw the cursor, verify, and run
/// the READY -> PRESENTED transition. Returns true when a present happened.
unsafe fn present_pending(fb: *mut u32, pitch_px: usize, wu: usize, hu: usize,
                          now_ns: u64, now_ms: u32, mx: i32, my: i32,
                          flush: bool) -> bool {
    let src = READY_BUF;
    if src < 0 {
        return false;
    }
    /* state-machine invariants: the present source must be the READY buffer (a
     * buffer that finished rendering and was never rendered into while waiting),
     * and no buffer may still be in DRAW state at present time - rendering and
     * presenting never target the same buffer. */
    SWAP.buf_checks = SWAP.buf_checks.wrapping_add(1);
    let mut live = 0i32;
    for i in 0..3 {
        if BUF_STATE[i] == BUF_DRAW {
            live += 1;
        }
    }
    if BUF_STATE[src as usize] != BUF_READY || READY_BUF != src || live != 0 {
        SWAP.buf_bad = SWAP.buf_bad.wrapping_add(1);
        console_printf(
            b"[gui-shell] buf: INVARIANT VIOLATION src=%d state=%d ready=%d draw_buffers=%d\n\0"
                .as_ptr(),
            src as i32, BUF_STATE[src as usize] as i32, READY_BUF as i32, live as i32);
    }
    if !vsync_due(now_ns) {
        SWAP.deferred = SWAP.deferred.wrapping_add(1);
        return false;
    }
    /* v0.64: real vertical retrace right before the framebuffer write. When the
     * driver reports no readable vblank source this returns -1 immediately and
     * the 60 Hz HPET grid above remains the only pacing (see swapstat). */
    let _vr = vsync_present_wait();
    let idx = src as usize;
    let px = ACCUM.area() as u64;
    {
        let frame = core::slice::from_raw_parts(buf_ptr(idx),
                                                (SCREEN_W as usize) * (SCREEN_H as usize));
        if ACCUM.full {
            blit_rect(frame, wu, fb, pitch_px, (0, 0, SCREEN_W, SCREEN_H));
        } else {
            for k in 0..ACCUM.n {
                blit_rect(frame, wu, fb, pitch_px, ACCUM.r[k].as_tuple());
            }
        }
        draw_cursor(fb, pitch_px, wu, hu, mx, my);
        tear_check(fb, pitch_px, frame, wu, mx, my);
    }
    ACCUM.clear();
    /* state machine: READY -> PRESENTED, the previous PRESENTED -> FREE */
    let old = PRESENT_BUF;
    BUF_STATE[idx] = BUF_PRESENTED;
    PRESENT_BUF = src;
    if old >= 0 && old != src && BUF_STATE[old as usize] == BUF_PRESENTED {
        BUF_STATE[old as usize] = BUF_FREE;
        BUF_VALID[old as usize] = false;
    }
    READY_BUF = -1;
    let latency = now_ns.saturating_sub(READY_PRODUCED_NS);
    swap_commit(now_ns, now_ms, true, px, latency);
    let _ = flush;
    true
}

/// Sampled present completion check.
///
/// SutureOS has no GPU scanline interrupt, so this is NOT a physical tearing
/// measurement: after the blit it samples the framebuffer and the buffer that
/// was just presented and reports `tear: partial present at x,y` when they
/// disagree. A partial or corrupt present shows up here immediately; the
/// acceptance also takes 20 screendumps mid-present for the same reason.
unsafe fn tear_check(fb: *mut u32, pitch_px: usize, frame: &[u32], wu: usize,
                     mx: i32, my: i32) {
    let mut bad = 0u32;
    let mut n = 0u32;
    let total = (SCREEN_W as u32) * (SCREEN_H as u32);
    let mut k = 0u32;
    while k < 96 {
        let idx = k.wrapping_mul(7919).wrapping_add(104_729) % total;
        let x = (idx % SCREEN_W as u32) as i32;
        let y = (idx / SCREEN_W as u32) as i32;
        k += 1;
        /* the cursor lives only in the framebuffer */
        if x >= mx - 2 && x < mx + CUR_W + 2 && y >= my - 2 && y < my + CUR_H + 2 {
            continue;
        }
        let dst = core::ptr::read_volatile(fb.add(y as usize * pitch_px + x as usize));
        let srcv = frame[y as usize * wu + x as usize];
        n += 1;
        if (dst & 0x00ff_ffff) != (srcv & 0x00ff_ffff) {
            bad += 1;
            if bad == 1 {
                console_printf(
                    b"[gui-shell] tear: partial present at %d,%d (fb %06x buf %06x)\n\0".as_ptr(),
                    x, y, dst & 0x00ff_ffff, srcv & 0x00ff_ffff);
            }
        }
    }
    SWAP.tear_checks = SWAP.tear_checks.saturating_add(n);
    SWAP.tear_mismatch = SWAP.tear_mismatch.saturating_add(bad);
}

// ------------------------------------------------------------------- damage diff
/// Diff the previous frame's state against this frame's and return the screen
/// rectangles that have to be recomposed.
fn diff_damage(prev: &Snap, sh: &Shell, ctx: &FrameCtx) -> Damage {
    let mut d = Damage::new();
    if !prev.ready {
        d.full_screen();
        return d;
    }
    /* cursor: the old arrow has to be erased and the new one drawn */
    if prev.mx != ctx.mx || prev.my != ctx.my {
        d.add(cursor_rect(prev.mx, prev.my));
        d.add(cursor_rect(ctx.mx, ctx.my));
    }
    /* windows: geometry, visibility, stacking */
    for i in 0..WINS {
        let a = prev.wins[i];
        let b = WinSnap::of(&sh.wins[i]);
        if a.visible() != b.visible() || a.x != b.x || a.y != b.y || a.w != b.w
            || a.h != b.h || (a.visible() && a.z != b.z)
        {
            d.add(a.rect().expand(SHADOW_MARGIN));
            d.add(b.rect().expand(SHADOW_MARGIN));
        }
    }
    /* taskbar strip: hover, focus, clock, window button set */
    let mut strip = prev.hover_wb != ctx.hover_wb
        || prev.hover_start != ctx.hover_start
        || prev.clock != ctx.clock
        || prev.focus != sh.focus as i32;
    for i in 0..WINS {
        if prev.wins[i].open != sh.wins[i].open
            || prev.wins[i].minimized != sh.wins[i].minimized
        {
            strip = true;
        }
    }
    if strip {
        d.add(TASKBAR_RECT);
    }
    /* start menu (the old menu also has to be erased when it closes) */
    let mut hash = sh.ilen[sh.focus] as i32;
    hash = hash.wrapping_mul(31).wrapping_add(sh.focus as i32);
    for b in sh.input[sh.focus].iter() {
        hash = hash.wrapping_mul(31).wrapping_add(*b as i32);
    }
    let menu_changed = prev.menu_open != sh.menu_open
        || (sh.menu_open
            && (prev.menu_sel != sh.menu_sel || prev.menu_hover != ctx.menu_hover
                || prev.menu_scroll != sh.menu_scroll as i32))
        || prev.menu_alpha != ctx.menu_alpha;
    if menu_changed {
        d.add(menu_rect());
    }
    /* focused window's editable line */
    if prev.focus != sh.focus as i32 || prev.input_hash != hash
        || prev.menu_open != sh.menu_open
    {
        d.add(input_line_rect(&sh.wins[prev.focus as usize]));
        d.add(input_line_rect(&sh.wins[sh.focus]));
    }
    /* a new Wayland commit re-rasterises the image viewer's layer */
    if prev.wl_epoch != crate::apps::wl_epoch() {
        for i in 0..WINS {
            let w = sh.wins[i];
            if w.open && !w.minimized && w.accent == crate::apps::ACC_IMAGE {
                d.add(WinSnap::of(&w).rect().expand(SHADOW_MARGIN));
            }
        }
    }
    /* task manager live values (uptime / CPU% / heartbeat: 500 ms tick) */
    if prev.live_tick != ctx.live_tick {
        for i in 0..WINS {
            let w = sh.wins[i];
            if w.open && !w.minimized && is_taskman(&w) {
                let mut lr = [Rect::EMPTY; 3];
                live_rects(&w, &mut lr);
                for r in lr.iter() {
                    d.add(*r);
                }
            }
        }
    }
    /* animations: cover both the rectangle the previous frame dimmed and the
     * one this frame is about to dim */
    if prev.act_frames > 0 {
        d.add(prev.visible_rect(prev.act_idx));
    }
    if ctx.act_alpha > 0 && ctx.act_idx >= 0 && ctx.act_idx < WINS as i32 {
        let w = sh.wins[ctx.act_idx as usize];
        d.add(WinSnap::of(&w).rect().expand(SHADOW_MARGIN));
    }
    if prev.intro_frames > 0 {
        d.add(prev.visible_rect(prev.intro_idx));
    }
    if ctx.intro_alpha > 0 && ctx.intro_idx >= 0 && ctx.intro_idx < WINS as i32 {
        let w = sh.wins[ctx.intro_idx as usize];
        d.add(WinSnap::of(&w).rect().expand(SHADOW_MARGIN));
    }
    if ctx.menu_alpha > 0 {
        d.add(menu_rect());
    }
    d
}

// --------------------------------------------------------------------- oracle
/// Compose the whole screen into a spare buffer and compare it pixel by pixel
/// with the damage-composed frame. This is the correctness gate of the damage
/// system: a missing damage source shows up here as a mismatch, and the oracle
/// frame is adopted as the repair so the screen cannot stay stale.
unsafe fn run_oracle(draw: usize, wu: usize, sh: &mut Shell, ctx: &FrameCtx) -> bool {
    let mut scratch = -1i32;
    for i in 0..3 {
        if BUF_STATE[i] == BUF_FREE && i != draw && i as i32 != READY_BUF
            && i as i32 != PRESENT_BUF
        {
            scratch = i as i32;
            break;
        }
    }
    if scratch < 0 {
        return true;                /* no spare buffer: try again next frame */
    }
    let sc = scratch as usize;
    BUF_STATE[sc] = BUF_DRAW;       /* temporarily used as the oracle target */
    let mut one = [Rect::EMPTY; MAX_DAMAGE];
    one[0] = Rect::new(0, 0, SCREEN_W, SCREEN_H);
    compose_list(sc, wu, sh, &one, 1, ctx);
    BUF_STATE[sc] = BUF_FREE;
    BUF_VALID[sc] = false;

    let a = core::slice::from_raw_parts(buf_ptr(draw),
                                        (SCREEN_W as usize) * (SCREEN_H as usize));
    let b = core::slice::from_raw_parts(buf_ptr(sc),
                                        (SCREEN_W as usize) * (SCREEN_H as usize));
    let mut i = 0usize;
    let mut bad = 0u32;
    let mut first = 0usize;
    while i < a.len() {
        if (a[i] & 0x00ff_ffff) != (b[i] & 0x00ff_ffff) {
            if bad == 0 {
                first = i;
            }
            bad += 1;
        }
        i += 1;
    }
    if bad > 0 {
        let i = first;
        {
            let x = (i % SCREEN_W as usize) as i32;
            let y = (i / SCREEN_W as usize) as i32;
            SWAP.oracle_mismatch = SWAP.oracle_mismatch.wrapping_add(1);
            console_printf(
                b"[gui-shell] damage: MISMATCH at %d,%d (got %06x want %06x) px=%d rects=%d r0=%d,%d,%d,%d r1=%d,%d,%d,%d\n\0".as_ptr(),
                x, y, a[i] & 0x00ff_ffff, b[i] & 0x00ff_ffff, bad as i32,
                ORACLE_INFO[0] as i32,
                ORACLE_INFO[1], ORACLE_INFO[2], ORACLE_INFO[3], ORACLE_INFO[4],
                ORACLE_INFO[5], ORACLE_INFO[6], ORACLE_INFO[7], ORACLE_INFO[8]);
            /* repair: adopt the oracle frame (the whole screen has to be shown) */
            buf_slice(draw).copy_from_slice(buf_slice(sc));
            return false;
        }
        i += 1;
    }
    SWAP.oracle_checks = SWAP.oracle_checks.wrapping_add(1);
    true
}

// ---------------------------------------------------------------- frame stats
/* per logging window (30 frames) */
static mut WLOG_FRAMES: u32 = 0;
/// Temporary diagnostics budget (frames of `dbg frame` logging).
static mut DMG_DEBUG: u32 = 0;
static mut WLOG_FULL: u32 = 0;
static mut WLOG_DMG: u32 = 0;
static mut WLOG_PX: u64 = 0;
static mut WLOG_FRAMED: u64 = 0;
static mut WLOG_ORACLE: u32 = 0;
static mut WLOG_MISMATCH: u32 = 0;
static mut WLOG_SINCE_ORACLE: u32 = 0;
/// How many damage frames may pass before the oracle re-verifies the whole
/// screen against a full repaint.
const ORACLE_EVERY: u32 = 20;

/// Snapshot the frame that is about to become "the frame on screen".
fn take_snap(sh: &Shell, ctx: &FrameCtx) -> Snap {
    let mut wins = [WinSnap::empty(); WINS];
    for i in 0..WINS {
        wins[i] = WinSnap::of(&sh.wins[i]);
    }
    let mut input_hash = sh.ilen[sh.focus] as i32;
    input_hash = input_hash.wrapping_mul(31).wrapping_add(sh.focus as i32);
    for b in sh.input[sh.focus].iter() {
        input_hash = input_hash.wrapping_mul(31).wrapping_add(*b as i32);
    }
    Snap {
        ready: true,
        mx: ctx.mx,
        my: ctx.my,
        hover_wb: ctx.hover_wb,
        hover_start: ctx.hover_start,
        menu_open: sh.menu_open,
        menu_sel: sh.menu_sel,
        menu_scroll: sh.menu_scroll as i32,
        menu_hover: ctx.menu_hover,
        menu_alpha: ctx.menu_alpha,
        focus: sh.focus as i32,
        input_hash,
        clock: ctx.clock,
        live_tick: ctx.live_tick,
        wl_epoch: crate::apps::wl_epoch(),
        act_frames: sh.act_frames,
        act_idx: sh.act_idx,
        intro_frames: sh.intro_frames,
        intro_idx: sh.intro_idx,
        scale_frames: sh.scale_frames,
        wins,
    }
}

/// Index of the menu row under the pointer (-1 when none). v0.64: the panel
/// scrolls, so the hover index is an absolute registry index.
fn menu_hover_index(mx: i32, my: i32, scroll: usize) -> i32 {
    menu_row_at(mx, my, scroll)
}

// ---------------------------------------------------------------- key queue
/* v0.67: the kernel pushes one event at a time and the next frame drains them
 * all, instead of transporting exactly one event per frame (a burst larger than
 * the bridge queue used to lose keys). */
const KEYQ_LEN: usize = 64;
static mut KEYQ: [(i32, i32, i32); KEYQ_LEN] = [(0, 0, 0); KEYQ_LEN];
static mut KEYQ_HEAD: usize = 0;
static mut KEYQ_TAIL: usize = 0;
static mut KEYQ_DROPPED: u32 = 0;
static mut KEYQ_LOGGED_DROP: bool = false;

/// v0.67: append one decoded key event (key = PS/2 make code or -1, ch = the
/// character the decoder produced or -1, mods = SHIFT/CTRL/ALT bits).
pub unsafe fn push_key(key: i32, ch: i32, mods: i32) {
    let next = (KEYQ_TAIL + 1) % KEYQ_LEN;
    if next == KEYQ_HEAD {
        KEYQ_HEAD = (KEYQ_HEAD + 1) % KEYQ_LEN;
        KEYQ_DROPPED = KEYQ_DROPPED.wrapping_add(1);
        if !KEYQ_LOGGED_DROP {
            KEYQ_LOGGED_DROP = true;
            console_printf(b"[gui-shell] key queue overflow (%d dropped)\n\0".as_ptr(),
                           KEYQ_DROPPED as i64);
        }
    }
    KEYQ[KEYQ_TAIL] = (key, ch, mods);
    KEYQ_TAIL = next;
}

/// v0.67: one key event, factored out of `shell_step` so a frame can apply many.
unsafe fn handle_key(sh: &mut Shell, key: i32, ch: i32, mods: i32) {
    // keyboard: menu navigation (v0.43.0). Scancodes are PS/2 set-1 make codes.
    if sh.menu_open {
        match key {
            0x48 => {
                sh.menu_sel -= 1;
                if sh.menu_sel < 0 {
                    sh.menu_sel = MENU_ROWS.len() as i32 - 1;
                }
                log2(c"menu sel", sh.menu_sel, 0);
                menu_scroll_into_view(sh);
            }
            0x50 => {
                sh.menu_sel += 1;
                if sh.menu_sel >= MENU_ROWS.len() as i32 {
                    sh.menu_sel = 0;
                }
                log2(c"menu sel", sh.menu_sel, 0);
                menu_scroll_into_view(sh);
            }
            /* v0.65: PageUp/PageDown scroll a whole page without moving the
             * selection, so the list can be paged while keeping the cursor. */
            0x49 | 0x51 => {
                let page = menu_visible_rows() as i32;
                let dir = if key == 0x49 { -page } else { page };
                let _ = menu_scroll_by(sh, dir);
            }
            /* v0.64: Home/End jump to the first/last registry row */
            0x47 => {
                sh.menu_sel = 0;
                log2(c"menu sel", 0, 0);
                menu_scroll_into_view(sh);
            }
            0x4f => {
                sh.menu_sel = MENU_ROWS.len() as i32 - 1;
                log2(c"menu sel", sh.menu_sel, 0);
                menu_scroll_into_view(sh);
            }
            0x1C => {
                let idx = sh.menu_sel.max(0) as usize;
                if idx < MENU_ROWS.len() {
                    menu_activate(sh, idx);
                }
                log2(c"menu enter", idx as i32, 0);
            }
            _ => {}
        }
    } else if key == 0x3C && !sh.shutting {
        /* v0.60: F2 toggles the dedicated Wayland compositor window (window #4).
         * v0.62: the window is also a start-menu row now (the menu grows with the
         * registry); F2 stays as the keyboard shortcut and means
         * open -> minimize -> restore. */
        if sh.wins[4].open && !sh.wins[4].minimized && sh.focus == 4 {
            sh.wins[4].minimized = true;
            if let Some(n) = next_visible(sh, 4) {
                sh.wins[n].z = next_z(sh);
                sh.focus = n;
            }
            log0(c"wayland window minimized");
        } else {
            launch_window(sh, 4);
            log0(c"wayland window opened");
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

    /* v0.64: per-window keyboard focus. Tab cycles the window focus; after that
     * the ownership registry (KEY_OWNERS) decides who sees the key - a window
     * with a handler runs it, any other window edits its one-line input band. */
    if !sh.menu_open && !sh.shutting {
        let fi = sh.focus;
        if ch == 0x09 {
            let mut n = fi;
            for _ in 0..WINS {
                n = (n + 1) % WINS;
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
            /* v0.64: one table decides who eats the key. A registered window
             * runs its handler; an unregistered one gets the one-line input
             * band that its acceptance still exercises. */
            if let Some(owner) = key_owner_for(fi, sh.wins[fi].accent) {
                dispatch_owned_key(sh, owner, mods, key, ch);
            } else {
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
    }

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
    click_btn: i32,
    now_ms: u32,
    clock_hhmm: i32,
    wheel: i32,
) -> u32 {
    if fb.is_null() || w <= 0 || h <= 0 || pitch_px < w {
        return 0;
    }
    if !CACHED {
        build_caches();
        log0(c"caches built (wallpaper + frosted source)");
    }
    /* v0.61: let the kernel's Wayland server map the pointer into the retained
     * surface (the desktop window IS the compositor surface). No-op while no
     * surface is retained; the server logs the same lines as the live path. */
    wl_server_desktop_pump();
    let (wu, hu) = (w as usize, h as usize);
    let sh = &mut *core::ptr::addr_of_mut!(SHELL);
    unsafe { STYLE = sh.style; }
    /* the Settings window prints the current style as a row value */
    crate::apps::settings_set_style(sh.style);
    if !sh.started {
        sh.started = true;
        sh.frame_ms = now_ms;
        console_printf(b"[gui-shell] key owners=%d mask=%d\n\0".as_ptr(),
                       KEY_OWNERS.len() as i32, key_owner_mask());
        log0(c"shell: loop started");
        taskbar_load(sh);
        unsafe { STYLE = sh.style; }
        crate::apps::settings_set_style(sh.style);
        unsafe { WIN_KEY[0] = i32::MIN; }
        if sh.style != STYLE_GLASS {
            /* the caches were built before the config was read, so rebuild the
             * wallpaper/frosted source for the persisted style */
            build_caches();
            console_printf(
                b"[gui-shell] caches rebuilt for the persisted style %d\n\0".as_ptr(),
                sh.style as i64);
        }
        console_printf(
            b"[gui-shell] taskbar: %d pinned buttons from x=%d, window buttons from x=%d\n\0"
                .as_ptr(), sh.pinned_n as i64, PIN_X0 as i64, wb_x0(sh) as i64);
    }
    /* v0.57.1: logo shutdown animation runs before (and instead of) the desktop
     * render; it ends by powering the machine off. */
    if sh.shutting {
        return shutdown_step(fb, w, h, pitch_px, sh, mx, my);
    }

    let now_ns = time_ns();
    let now = now_ms;
    let mx_c = mx.clamp(0, w - 1);
    let my_c = my.clamp(0, h - 1);
    let left_down = (buttons & 1) != 0;
    let start_btn = (8, TASKBAR_Y + 5, 34, 28);

    /* hover targets are pure functions of the pointer (they used to be
     * by-products of drawing the taskbar, which a damage frame may skip) */
    let hover_start = mx_c >= start_btn.0 && mx_c < start_btn.0 + start_btn.2
        && my_c >= start_btn.1 && my_c < start_btn.1 + start_btn.3;
    let mut hover_wb = -1;
    for i in 0..WINS {
        if let Some((bx, bw)) = wb_rect(sh, i) {
            if mx_c >= bx && mx_c < bx + bw && my_c >= TASKBAR_Y + 5
                && my_c < TASKBAR_Y + TASKBAR_H - 5
            {
                hover_wb = i as i32;
            }
        }
    }
    if hover_wb < 0 {
        /* v0.68: pinned buttons use negative hover ids (-2 == pin 0) so no new
         * damage-tracking state was needed */
        let mut p = 0i32;
        while p < sh.pinned_n && p < PIN_MAX {
            let slot = pin_slot(sh, p);
            if slot >= 0 && slot < WINS as i32 && !sh.wins[slot as usize].open {
                let (bx, bw) = pin_rect(p);
                if mx_c >= bx && mx_c < bx + bw && my_c >= TASKBAR_Y + 5
                    && my_c < TASKBAR_Y + TASKBAR_H - 5
                {
                    hover_wb = -p - 2;
                }
            }
            p += 1;
        }
    }
    sh.hover_wb = hover_wb;

    /* v0.69: context-menu hover (a change forces a full repaint, see below) */
    if sh.ctx_open {
        sh.ctx_sel = ctx_row_at(sh, mx_c, my_c);
    } else {
        sh.ctx_sel = -1;
    }
    let ctx_sig = (if sh.ctx_open { 1i32 } else { 0 }) * 65536
        + ((sh.ctx_kind & 0xff) << 8) + (sh.ctx_sel & 0xff);
    if ctx_sig != sh.ctx_prev {
        sh.ctx_prev = ctx_sig;
        sh.force_full = true;
    }

    // 2. hit testing: taskbar first (it is on top), then windows
    let mut hw = hit_win(&sh.wins, mx_c, my_c, sh.focus as i32);

    // 3. clicks
    let mut clicked_in_menu = false;
    /* v0.69: taskbar context menu. Right-click (input button 1) on a window or
     * pinned button opens it; a left-click on a row runs that row; a click
     * anywhere else dismisses it. */
    let mut ctx_handled = false;
    if click_x >= 0 && click_y >= 0 {
        let cx = click_x;
        let cy = click_y;
        let on_strip = cy >= TASKBAR_Y + 5 && cy < TASKBAR_Y + TASKBAR_H - 5;
        if sh.ctx_open {
            let row = ctx_row_at(sh, cx, cy);
            if click_btn != 1 && row >= 0 {
                ctx_act(sh, row);
            } else {
                sh.ctx_open = false;
                sh.force_full = true;
                log0(c"ctx menu closed");
            }
            ctx_handled = true;
        } else if click_btn == 1 && on_strip {
            let mut opened = false;
            for i in 0..WINS {
                if let Some((bx, bw)) = wb_rect(sh, i) {
                    if cx >= bx && cx < bx + bw && !opened {
                        sh.ctx_open = true;
                        sh.ctx_kind = 1;
                        sh.ctx_idx = i as i32;
                        sh.ctx_x = cx;
                        sh.ctx_y = cy;
                        opened = true;
                        log2(c"ctx menu open window", i as i32, 0);
                    }
                }
            }
            let mut p = 0i32;
            while p < sh.pinned_n && p < PIN_MAX && !opened {
                let slot = pin_slot(sh, p);
                let (bx, bw) = pin_rect(p);
                if slot >= 0 && slot < WINS as i32 && !sh.wins[slot as usize].open
                    && cx >= bx && cx < bx + bw
                {
                    sh.ctx_open = true;
                    sh.ctx_kind = 2;
                    sh.ctx_idx = p;
                    sh.ctx_x = cx;
                    sh.ctx_y = cy;
                    opened = true;
                    log2(c"ctx menu open pin", slot, p);
                }
                p += 1;
            }
            if opened {
                sh.force_full = true;
            }
            ctx_handled = true;
        }
    }
    if click_x >= 0 && click_y >= 0 && !ctx_handled {
        /* v0.57.1: hit-test the click at the click coordinates, not at the current
         * pointer: a click can be consumed while PS/2 move packets are still in
         * flight, which sent close-button clicks to the stale pointer's window. */
        hw = hit_win(&sh.wins, click_x, click_y, sh.focus as i32);
        let cx = click_x;
        let cy = click_y;
        if sh.menu_open {
            if cx >= MENU_X && cx < MENU_X + MENU_W && cy >= menu_y() && cy < menu_y() + menu_h()
    {
                clicked_in_menu = true;
                /* v0.64: a row launches its window or runs its action; the
                 * scrollbar strip pages the list. */
                let row = menu_row_at(cx, cy, sh.menu_scroll);
                if row >= 0 {
                    menu_activate(sh, row as usize);
                } else if let Some(dir) = menu_scroll_click(cx, cy) {
                    menu_scroll_by(sh, dir);
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
                    menu_scroll_into_view(sh);
                }
            } else {
                /* taskbar window buttons */
                let mut handled = false;
                /* v0.68: pinned app buttons launch (only shown while closed).
                 * v0.69: the pinned list is runtime state. */
                let mut pp = 0i32;
                while pp < sh.pinned_n && pp < PIN_MAX && !handled {
                    let slot = pin_slot(sh, pp);
                    if slot >= 0 && slot < WINS as i32 && !sh.wins[slot as usize].open {
                        let (bx, bw) = pin_rect(pp);
                        if cx >= bx && cx < bx + bw && cy >= TASKBAR_Y + 5
                            && cy < TASKBAR_Y + TASKBAR_H - 5
                        {
                            handled = true;
                            launch_window(sh, slot as usize);
                            log2(c"pin launch", slot, pp);
                        }
                    }
                    pp += 1;
                }
                for i in 0..WINS {
                    if let Some((bx, bw)) = wb_rect(sh, i) {
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
                            sh.drag_glass_frames = 0;
                            sh.drag_dx = cx - wref.x;
                            sh.drag_dy = cy - wref.y;
                            log2(c"drag begin", hw, cx);
                        } else {
                            sh.wins[hw as usize].z = next_z(sh);
                            sh.focus = hw as usize;
                            log2(c"focus", hw, 0);
                            /* v0.64: the calculator keypad is clickable. The hit
                             * test lives in apps.rs (it owns the layout), the
                             * shell only maps screen pixels to window pixels. */
                            if hw == 5 {
                                let slot = crate::apps::calc_keypad_hit(
                                    cx - wref.x, cy - wref.y, wref.w, wref.h);
                                if slot >= 0 && crate::apps::calc_tap(slot) {
                                    unsafe {
                                        WIN_KEY[5] = i32::MIN;
                                    }
                                    sh.dirty_calc = true;
                                }
                            }
                            /* v0.71: 系统设置 rows are clickable - 外观风格
                             * toggles between the glass and the flat style. The
                             * row layout lives in apps.rs (it draws the window). */
                            if hw == 0 && cy >= wref.y + TITLE_H {
                                let row = crate::apps::settings_row_at(
                                    cy - wref.y, wref.h);
                                if row == crate::apps::SETTINGS_ROW_STYLE {
                                    let next = if sh.style == STYLE_FLAT {
                                        STYLE_GLASS
                                    } else {
                                        STYLE_FLAT
                                    };
                                    set_style(sh, next);
                                    unsafe {
                                        WIN_KEY[0] = i32::MIN;
                                    }
                                }
                            }
                        }
                    } else if cy < TASKBAR_Y {
                        /* click on empty desktop: keep the focus as it is */
                    }
                }
            }
        }
        sh.press_ms = now;
    }
    /* v0.65: PS/2 wheel. Positive = away from the user (scroll up). The menu
     * scrolls one row per notch, the picture viewer browses one file; every
     * other surface ignores it (the editor has no scrollback yet). */
    if wheel != 0 && !sh.shutting {
        /* the kernel's %d reads an int64: print the delta explicitly so a
         * negative notch is not logged as 4294967295 */
        unsafe {
            console_printf(b"[gui-shell] wheel %d menu=%d\n\0".as_ptr(),
                           wheel as i64, if sh.menu_open { 1i64 } else { 0i64 });
        }
        if sh.menu_open {
            /* PS/2 Z convention: negative = away from the user (scroll up) */
            let mut n = wheel.clamp(-3, 3);
            while n != 0 {
                let _ = menu_scroll_by(sh, if n < 0 { -1 } else { 1 });
                n += if n < 0 { 1 } else { -1 };
            }
        } else {
            let fi = sh.focus;
            if sh.wins[fi].open && !sh.wins[fi].minimized {
                if let Some(owner) = key_owner_for(fi, sh.wins[fi].accent) {
                    if owner.sink == KeySink::Editor {
                        /* three rows per notch, same sign convention as the menu */
                        crate::apps::editor_scroll(if wheel < 0 { -3 } else { 3 });
                        unsafe {
                            WIN_KEY[3] = i32::MIN;
                        }
                        sh.dirty_editor = true;
                    }
                    if owner.sink == KeySink::Image {
                        /* PS/2 Z: negative (wheel up) = previous file */
                        let dir = if wheel < 0 { -1 } else { 1 };
                        if crate::apps::image_shift(dir) {
                            unsafe {
                                WIN_KEY[2] = i32::MIN;
                            }
                            sh.dirty_image = true;
                            log2(c"image shift", if dir > 0 { 1 } else { 2 }, 0);
                        }
                    }
                }
            }
        }
    }

    /* v0.67: keys arrive in batches. The bridge pushes every event it read this
     * frame and the shell handles them all before rendering, so a typing burst
     * no longer overflows a one-key ABI. */
    while KEYQ_HEAD != KEYQ_TAIL {
        let (k, cc, mm) = KEYQ[KEYQ_HEAD];
        KEYQ_HEAD = (KEYQ_HEAD + 1) % KEYQ_LEN;
        handle_key(sh, k, cc, mm);
    }

    let mut dropped_win: i32 = -1;
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
        dropped_win = sh.drag_idx as i32;
        sh.dragging = false;
        log2(c"drag end", sh.wins[sh.drag_idx].x, sh.wins[sh.drag_idx].y);
    }
    if sh.dragging {
        let bidx = sh.drag_idx;
        let nxp = (mx_c - sh.drag_dx).clamp(0, SCREEN_W - 200);
        let nyp = (my_c - sh.drag_dy).clamp(0, TASKBAR_Y - 80);
        let moved = sh.wins[bidx].x != nxp || sh.wins[bidx].y != nyp;
        sh.wins[bidx].x = nxp;
        sh.wins[bidx].y = nyp;
        if !sh.wins[bidx].maximized {
            sh.wins[bidx].nx = nxp;
            sh.wins[bidx].ny = nyp;
        }
        if moved {
            /* v0.68: LIVE ACRYLIC. The layer cache is anchored to `ax/ay`; keeping
             * it at the home position froze the glass for the whole drag (the user
             * saw it "refresh only after the drop"). Re-anchor to the current
             * position and invalidate the layer so the frosted backdrop re-samples
             * the wallpaper under the window every moved frame. */
            sh.wins[bidx].ax = nxp;
            sh.wins[bidx].ay = nyp;
            unsafe {
                WIN_KEY[bidx] = i32::MIN;
            }
            sh.drag_glass_frames = sh.drag_glass_frames.wrapping_add(1);
            if sh.drag_glass_frames == 1 {
                log0(c"drag: live acrylic (glass re-anchored every moved frame)");
            }
        }
    }

    // ---- frame context: everything the compose pass reads, captured once ----
    let mut ctx = FrameCtx {
        now,
        now_ns,
        mx: mx_c,
        my: my_c,
        clock: clock_hhmm,
        hover_wb,
        hover_start,
        menu_hover: if sh.menu_open { menu_hover_index(mx_c, my_c, sh.menu_scroll) } else { -1 },
        act_alpha: 0,
        act_idx: -1,
        intro_alpha: 0,
        intro_idx: -1,
        menu_alpha: 0,
        scale: false,
        scale_idx: -1,
        scale_rect: [0; 4],
        /* One live refresh per second, driven by the SAME clock apps::draw_live
         * reads (the kernel tick counter): the digit change and the damage tick
         * then land in the same frame, so the damage oracle can verify the live
         * area. `live_now` pins the clock handed to draw_live to the start of
         * that second, which keeps the heartbeat dot (now_ms / 500) in its bright
         * phase - i.e. the desktop stays pixel-identical to the frozen baseline. */
        live_tick: (crate::shell_tick() / 100u64) as u32,
        live_now: (crate::shell_tick() / 100u64) as u32 * 1000,
    };
    if sh.act_frames > 0 && sh.act_idx >= 0 && sh.act_idx < WINS as i32 {
        let w0 = sh.wins[sh.act_idx as usize];
        if w0.open && !w0.minimized {
            ctx.act_alpha = if sh.act_frames == 2 { 0x2cu32 } else { 0x64u32 };
            ctx.act_idx = sh.act_idx;
        }
    }
    if sh.intro_frames > 0 && sh.intro_idx >= 0 && sh.intro_idx < WINS as i32 {
        let w0 = sh.wins[sh.intro_idx as usize];
        if w0.open {
            ctx.intro_alpha = match sh.intro_frames {
                3 => 0x58u32,
                2 => 0x2cu32,
                _ => 0x12u32,
            };
            ctx.intro_idx = sh.intro_idx;
        }
    }
    if sh.menu_intro > 0 {
        ctx.menu_alpha = if sh.menu_intro == 2 { 0x50u32 } else { 0x24u32 };
    }
    if sh.scale_frames > 0 && sh.scale_idx >= 0 && sh.scale_idx < WINS as i32 {
        let idx = sh.scale_idx as usize;
        let w0 = sh.wins[idx];
        if w0.open && !w0.minimized {
            let total: i32 = 4;
            let step = total - sh.scale_frames as i32 + 1;
            let mut r = [0i32; 4];
            for k in 0..4 {
                let a = sh.scale_from[k];
                let b = sh.scale_to[k];
                r[k] = a + (b - a) * step / total;
            }
            ctx.scale = true;
            ctx.scale_idx = sh.scale_idx;
            ctx.scale_rect = r;
            /* v0.68: the gate asserts every window walks the SAME rect sequence */
            console_printf(
                b"[gui-shell] scale rect idx=%d %d,%d %d,%d\n\0".as_ptr(),
                sh.scale_idx as i64, r[0] as i64, r[1] as i64, r[2] as i64,
                r[3] as i64);
        }
    }

    // ---- damage: what changed since the frame that is on screen? ----
    let prev = &*core::ptr::addr_of!(SNAP);
    let mut dmg = diff_damage(prev, sh, &ctx);
    if sh.dirty_calc && sh.wins[5].open && !sh.wins[5].minimized {
        /* a keystroke moves no rect: inject the calculator window's damage */
        dmg.add(WinSnap::of(&sh.wins[5]).rect().expand(SHADOW_MARGIN));
        sh.dirty_calc = false;
    }
    if sh.dirty_image && sh.wins[2].open && !sh.wins[2].minimized {
        /* browsing moves no rect either: inject the window's damage explicitly */
        dmg.add(WinSnap::of(&sh.wins[2]).rect().expand(SHADOW_MARGIN));
        sh.dirty_image = false;
    }
    if sh.dirty_editor && sh.wins[3].open && !sh.wins[3].minimized {
        /* a keystroke moves no rect, so it has to be injected here explicitly */
        dmg.add(WinSnap::of(&sh.wins[3]).rect().expand(SHADOW_MARGIN));
        sh.dirty_editor = false;
    }
    {
        /* v0.61 statistics: how many 32x32 tiles this frame really touched */
        let tc = dmg.tile_count() as i32;
        if tc > WLOG_TILES_MAX {
            WLOG_TILES_MAX = tc;
        }
    }
    if sh.force_full {
        /* v0.69: the context menu / pinned layout can change any pixel of the
         * strip and the popup sits over arbitrary windows - a full frame keeps
         * the damage oracle exact (same decision as the drop frame below). */
        dmg.full_screen();
        sh.force_full = false;
    }
    if dropped_win >= 0 && dropped_win < WINS as i32 {
        dmg.add(WinSnap::of(&sh.wins[dropped_win as usize]).rect().expand(SHADOW_MARGIN));
        /* v0.68: with live acrylic the drop re-anchors the glass, so the WHOLE
         * body changes without a geometry delta - a structural change. Repaint
         * the frame in full (drops are rare) so a damage frame can never disagree
         * with the oracle (this was a real 1-in-8 MISMATCH). */
        dmg.full_screen();
        log0(c"drop: full frame (glass re-anchor)");
    }
    let animating = ctx.act_alpha > 0 || ctx.intro_alpha > 0 || ctx.menu_alpha > 0
        || ctx.scale;

    if dmg.is_empty() && !animating {
        /* nothing changed: only a coalesced present can still need the screen */
        if READY_BUF >= 0 && vsync_due(now_ns) {
            present_pending(fb, pitch_px as usize, wu, hu, now_ns, now, mx_c, my_c, true);
        } else {
            IDLE_FRAMES += 1;
            if IDLE_FRAMES == 1 {
                log0(c"idle: frame skipped (nothing changed)");
            }
        }
        return LAST_SUM;
    }
    IDLE_FRAMES = 0;

    /* the cursor-only fast path (v0.45.0) is now just "the damage is two small
     * rectangles"; keep counting it so the frame-rate checks stay comparable */
    /* v0.61: the tile grid rounds a 28x32 cursor to one or two 32x32 tiles, so
     * the "cursor-only" decision counts tiles instead of pixels (a diagonal
     * move can touch up to four tiles and must still take the fast path). */
    let cursor_only = !dmg.full && dmg.n > 0 && dmg.tile_count() <= 4;

    /* a full repaint is required for the first frame, during the maximise /
     * restore transition, and whenever the base buffer is not a complete frame */
    let base = if READY_BUF >= 0 { READY_BUF } else { PRESENT_BUF };
    let mut full = dmg.full || ctx.scale || !buf_is_front(base);
    let mut rects = [Rect::EMPTY; MAX_DAMAGE];
    let mut nrects = 0usize;
    if !full {
        for k in 0..dmg.n {
            let mut r = dmg.r[k];
            /* the taskbar strip and the (possibly stale) menu block are atomic
             * layers: any damage touching them recomposes them whole */
            if r.overlaps(TASKBAR_RECT) {
                r = r.union(TASKBAR_RECT);
            }
            let mr = menu_rect();
            if r.overlaps(mr) {
                r = r.union(mr);
            }
            let c = r.clipped();
            if !c.is_empty() {
                rects[nrects] = c;
                nrects += 1;
            }
        }
        if nrects == 0 {
            full = true;
        }
    }
    if full {
        rects[0] = Rect::new(0, 0, SCREEN_W, SCREEN_H);
        nrects = 1;
    }

    // ---- render into the DRAW buffer ----
    let draw = begin_frame();
    let t0 = ticks();
    compose_list(draw, wu, sh, &rects, nrects, &ctx);
    let compose_ticks = ticks() - t0;

    let px = if full {
        SCREEN_W as i64 * SCREEN_H as i64
    } else {
        let mut a = 0i64;
        for k in 0..nrects {
            a += rects[k].area();
        }
        a
    };

    // ---- structural change? then re-verify against a full repaint ----
    let mut structural = false;
    for i in 0..WINS {
        if prev.wins[i].open != sh.wins[i].open
            || prev.wins[i].minimized != sh.wins[i].minimized
        {
            structural = true;
        }
    }
    if prev.menu_open != sh.menu_open || prev.focus != sh.focus as i32
        || prev.scale_frames > 0 && sh.scale_frames == 0
    {
        structural = true;
    }
    if !full && (structural || WLOG_SINCE_ORACLE >= ORACLE_EVERY) {
        WLOG_SINCE_ORACLE = 0;
        ORACLE_INFO = [nrects as i32,
                       rects[0].x0, rects[0].y0, rects[0].x1, rects[0].y1,
                       rects[1].x0, rects[1].y0, rects[1].x1, rects[1].y1];
        if !run_oracle(draw, wu, sh, &ctx) {
            /* the oracle frame was adopted: the whole screen must be shown */
            full = true;
            rects = [Rect::EMPTY; MAX_DAMAGE];
            rects[0] = Rect::new(0, 0, SCREEN_W, SCREEN_H);
            nrects = 1;
            WLOG_MISMATCH += 1;
        } else {
            WLOG_ORACLE += 1;
        }
    }

    // ---- accounting ----
    if structural {
        DMG_DEBUG = 8;
    }
    sh.prof_frames += 1;
    WLOG_FRAMES += 1;
    WLOG_SINCE_ORACLE += 1;
    WLOG_PX += px as u64;
    WLOG_FRAMED += SCREEN_W as u64 * SCREEN_H as u64;
    SWAP.framed_px = SWAP.framed_px.saturating_add(SCREEN_W as u64 * SCREEN_H as u64);
    if full {
        sh.prof_full += 1;
        WLOG_FULL += 1;
        SWAP.full_frames = SWAP.full_frames.wrapping_add(1);
    } else {
        WLOG_DMG += 1;
        SWAP.damage_frames = SWAP.damage_frames.wrapping_add(1);
    }
    if cursor_only && !full {
        CURSOR_FRAMES += 1;
        if CURSOR_FRAMES == 1 || CURSOR_FRAMES % 60 == 0 {
            console_printf(b"[gui-shell] cursor: fast path frames=%d\n\0".as_ptr(),
                           CURSOR_FRAMES as i32);
        }
    }
    sh.prof_win += compose_ticks;
    /* the dragged window renders from its cached home-anchored layer: no
     * re-rasterise per frame (v0.50.0 behaviour, now inside the damage compose) */
    if sh.dragging && sh.drag_idx < WINS {
        let dw = sh.wins[sh.drag_idx];
        if unsafe { WIN_KEY[sh.drag_idx] } == layer_key(&dw, sh.drag_idx) {
            sh.prof_drag_reuse += 1;
            if sh.prof_drag_reuse == 1 {
                log0(c"drag: reusing window layer (damage compose)");
            }
        }
    }
    if sh.dragging && sh.drag_frames_logged < 20 {
        sh.drag_frames_logged += 1;
        console_printf(
            b"[gui-shell] drag repaint %d ticks (rects %d px %llu)\n\0".as_ptr(),
            compose_ticks as i32, nrects as i32, px as u64);
    }

    // ---- hand the frame to the present stage and show it if the grid allows ----
    /* everything this frame composed must reach the framebuffer eventually: a
     * present the grid defers keeps these rectangles accumulated (a superseded
     * frame must not silently drop its damage) */
    if full {
        ACCUM.full_screen();
    } else {
        for k in 0..nrects {
            ACCUM.add(rects[k]);
        }
    }
    finish_frame();
    let presented = present_pending(fb, pitch_px as usize, wu, hu, now_ns, now,
                                    mx_c, my_c, false);
    if DMG_DEBUG > 0 {
        DMG_DEBUG -= 1;
        console_printf(
            b"[gui-shell] dbg frame full=%d rects=%d r0=%d,%d,%d,%d r1=%d,%d,%d,%d \
px=%llu present=%d accum=%d\n\0"
                .as_ptr(),
            full as i32, nrects as i32,
            rects[0].x0, rects[0].y0, rects[0].x1, rects[0].y1,
            rects[1].x0, rects[1].y0, rects[1].x1, rects[1].y1,
            px as u64, presented as i32, ACCUM.n as i32);
    }

    /* the frame that was just composed is now the reference for the next diff:
     * snapshot BEFORE the animation counters advance, so the state that was
     * drawn (e.g. "window still open, fading") is what the next frame compares
     * against. */
    SNAP = take_snap(sh, &ctx);

    // ---- animation counters advance after the frame that used them ----
    if sh.act_frames > 0 {
        let idx = sh.act_idx;
        sh.act_frames -= 1;
        /* v0.68 BUGFIX: the bound was a leftover `5` from the 5-window era, so a
         * deferred minimize/close of window 5 (calculator) never executed - it
         * faded out, stayed open and kept focus. */
        if sh.act_frames == 0 && idx >= 0 && idx < WINS as i32 {
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
    if sh.intro_frames > 0 {
        if ctx.intro_alpha > 0 {
            log2(c"intro fade", sh.intro_frames as i32, ctx.intro_idx);
        }
        sh.intro_frames -= 1;
    }
    if sh.menu_intro > 0 {
        log2(c"menu fade", sh.menu_intro as i32, 0);
        sh.menu_intro -= 1;
    }
    if sh.scale_frames > 0 {
        log2(c"scale", sh.scale_idx, sh.scale_frames as i32);
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


    if WLOG_FRAMES >= 30 {
        let st = &mut *core::ptr::addr_of_mut!(SWAP);
        let avg_ivl = if WLOG_IVL_N > 0 { WLOG_IVL_SUM / WLOG_IVL_N } else { 0 };
        let avg_jit = if WLOG_IVL_N > 0 { WLOG_JIT_SUM / WLOG_IVL_N } else { 0 };
        let avg_lat = if WLOG_IVL_N > 0 { WLOG_LAT_SUM / WLOG_IVL_N } else { 0 };
        let permille = if WLOG_FRAMED > 0 { (WLOG_PX * 1000 / WLOG_FRAMED) as i32 } else { 0 };
        console_printf(
            b"[gui-shell] profile/30: full=%d hit=%d miss=%d win=%llu bar=%llu ticks damage=%d full_frames=%d px=%llu framed=%llu permille=%d oracle=%d mismatch=%d\n\0"
                .as_ptr(),
            WLOG_FULL as i32,
            sh.prof_cache_hit as i32,
            sh.prof_cache_miss as i32,
            sh.prof_win,
            0u64,
            WLOG_DMG as i32,
            WLOG_FULL as i32,
            WLOG_PX,
            WLOG_FRAMED,
            permille,
            WLOG_ORACLE as i32,
            WLOG_MISMATCH as i32,
        );
        console_printf(
            b"[gui-shell] swap presents=%d gens=%d presented=%d dropped=%d jitter_us=%d \
interval_ms=%d skipped=%d latency_us=%d latency_max_us=%d \
hist=%d,%d,%d,%d,%d,%d\n\0"
                .as_ptr(),
            st.presents as i32,
            st.generation as i32,
            st.presented as i32,
            st.dropped as i32,
            avg_jit as i32,
            avg_ivl as i32,
            st.skipped as i32,
            avg_lat as i32,
            WLOG_LAT_MAX as i32,
            st.hist[0] as i32,
            st.hist[1] as i32,
            st.hist[2] as i32,
            st.hist[3] as i32,
            st.hist[4] as i32,
            st.hist[5] as i32,
        );
        /* v0.64: retrace source + wait/latency histograms */
        {
            let vmode: &core::ffi::CStr = if st.vsync_edges > 0 {
                c"vblank"
            } else if st.vsync_timeouts > 0 {
                c"timeout"
            } else {
                c"off"
            };
            let wavg = if st.vsync_edges > 0 {
                (st.wait_sum_us / st.vsync_edges) as i32
            } else {
                0
            };
            console_printf(
                b"[gui-shell] vsync: mode=%s hz=%d edges=%d timeouts=%d unavailable=%d \
wait_us=%d wait_max_us=%d wait_hist=%d,%d,%d,%d,%d lat_hist=%d,%d,%d,%d,%d\n\0"
                    .as_ptr(),
                vmode.as_ptr(),
                st.vsync_hz100,
                st.vsync_edges as i32,
                st.vsync_timeouts as i32,
                st.vsync_unavail as i32,
                wavg,
                st.wait_max_us as i32,
                st.wait_hist[0] as i32,
                st.wait_hist[1] as i32,
                st.wait_hist[2] as i32,
                st.wait_hist[3] as i32,
                st.wait_hist[4] as i32,
                st.lat_hist[0] as i32,
                st.lat_hist[1] as i32,
                st.lat_hist[2] as i32,
                st.lat_hist[3] as i32,
                st.lat_hist[4] as i32,
            );
        }
        if WLOG_ORACLE > 0 {
            console_printf(
                b"[gui-shell] damage: identical oracle=%d frames=%d rects=%d tiles=%d permille=%d\n\0"
                    .as_ptr(),
                WLOG_ORACLE as i32, WLOG_FRAMES as i32, nrects as i32,
                WLOG_TILES_MAX, permille);
        }
        sh.prof_frames = 0;
        sh.prof_full = 0;
        sh.prof_cache_hit = 0;
        sh.prof_cache_miss = 0;
        sh.prof_win = 0;
        sh.prof_bar = 0;
        WLOG_IVL_SUM = 0;
        WLOG_IVL_N = 0;
        WLOG_JIT_SUM = 0;
        WLOG_LAT_SUM = 0;
        WLOG_LAT_MAX = 0;
        WLOG_TILES_MAX = 0;
        WLOG_FRAMES = 0;
        WLOG_FULL = 0;
        WLOG_DMG = 0;
        WLOG_PX = 0;
        WLOG_FRAMED = 0;
        WLOG_ORACLE = 0;
        WLOG_MISMATCH = 0;
    }

    /* sampled checksum of the drawn frame (informational: the swap gate and the
     * idle log use it, never correctness) */
    let mut hash: u32 = 0x811c9dc5;
    {
        let frame = core::slice::from_raw_parts(buf_ptr(draw),
                                                (SCREEN_W as usize) * (SCREEN_H as usize));
        let mut k = 0usize;
        while k < frame.len() {
            hash ^= frame[k] & 0x00ff_ffff;
            hash = hash.wrapping_mul(0x0100_0193);
            k += 8;
        }
    }
    LAST_SUM = hash;
    let _ = presented;
    hash
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
    if slot == 2 || slot == 4 {
        k = k.wrapping_mul(31).wrapping_add(crate::apps::wl_epoch());
    }
    /* v0.71: a window body is drawn differently per style, so the style is part
     * of the key - otherwise switching style would keep blitting the old look. */
    k = k.wrapping_mul(31).wrapping_add(unsafe { STYLE as i32 });
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

/// Blit the cached body layer at the window's current position, limited to the
/// rectangle `r` that is being recomposed (the layer itself is position-free
/// since v0.51.0: it is rendered at its home anchor and blitted anywhere).
fn composite_window_layer(scene: &mut [u32], stride: usize, slot: usize, w: &Win,
                          r: Rect) -> bool {
    let lw = (w.w + WC_MARGIN_L + WC_MARGIN_R) as usize;
    let lh = (w.h + WC_MARGIN_T + WC_MARGIN_B) as usize;
    if lw == 0 || lh == 0 || lw > WC_STRIDE || lh > WC_H {
        return false;
    }
    if w.w <= 0 || w.h <= 0 {
        return true;
    }
    let cache = unsafe { &(&*core::ptr::addr_of!(WIN_CACHE))[slot] };
    /* v0.58.0: only the body is copied; the margins hold the wallpaper prefill
     * and the shadow is drawn separately (alpha-composited over the scene). */
    let x0 = w.x.max(r.x0).max(0);
    let y0 = w.y.max(r.y0).max(0);
    let x1 = (w.x + w.w).min(r.x1).min(SCREEN_W);
    let y1 = (w.y + w.h).min(r.y1).min(SCREEN_H);
    if x1 <= x0 || y1 <= y0 {
        return true;
    }
    let n = (x1 - x0) as usize;
    for dy in y0..y1 {
        let ly = dy - w.y;
        let d = dy as usize * stride + x0 as usize;
        let srow = (WC_MARGIN_T + ly) as usize * WC_STRIDE
                   + (WC_MARGIN_L + (x0 - w.x)) as usize;
        scene[d..d + n].copy_from_slice(&cache[srow..srow + n]);
    }
    true
}

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
        0x7a5cff => 4,
        crate::apps::ACC_CALC => 5,
        _ => 0,
    }
}

/// v0.70 flat icon set (redrawn by the ConlyesOS author, merged on
/// request; the rest of this desktop keeps the glass/Fluent chrome):
/// a solid accent plate with an anti-aliased silhouette
/// and one flat white pictogram per application. Nothing is lit - no gradient
/// body, no specular band, no rim light, no drop shadow. Every glyph uses a
/// different construction (sliders / bars / frame / lines / window / keypad)
/// so the six apps stay distinguishable at 16 px without relying on colour.
fn draw_app_icon(s: &mut Surface<'_>, x: i32, y: i32, size: i32, accent: u32,
                 glyph: i32) {
    if size < 8 {
        return;
    }
    let sf = size as f32;
    s.round_rect(x as f32, y as f32, (x + size) as f32, (y + size) as f32,
                 sf * 0.20, accent, 255);
    let gx = x + size / 2;
    let gy = y + size / 2;
    /* v0.70: at title-bar size (16 px) the detailed glyphs collapse into
     * mush, so swap in compact marks that keep a legible silhouette. */
    if size < 18 {
        let g = 4i32;
        let ink = 0xffffffu32;
        match glyph {
            0 => {
                s.rect_fill(gx - g, gy - 3, gx + g, gy - 2, ink, 255);
                s.rect_fill(gx - g, gy + 2, gx + g, gy + 3, ink, 255);
                s.rect_fill(gx - 1, gy - 5, gx + 1, gy - 1, ink, 255);
                s.rect_fill(gx + 1, gy + 1, gx + 3, gy + 5, ink, 255);
            }
            1 => {
                s.rect_fill(gx - g, gy + 3, gx + g, gy + 4, ink, 255);
                s.rect_fill(gx - g, gy, gx - g + 2, gy + 2, ink, 255);
                s.rect_fill(gx - 1, gy - 3, gx + 1, gy + 2, ink, 255);
                s.rect_fill(gx + g - 2, gy - 4, gx + g, gy + 2, ink, 255);
            }
            3 => {
                s.rect_fill(gx - g, gy - 4, gx + g, gy - 3, ink, 255);
                s.rect_fill(gx - g, gy - 1, gx + g - 1, gy, ink, 255);
                s.rect_fill(gx - g, gy + 2, gx + g, gy + 3, ink, 255);
            }
            5 => {
                s.rect_fill(gx - g, gy - 3, gx + g, gy - 2, ink, 255);
                s.rect_fill(gx - g, gy - 1, gx + g, gy, ink, 255);
                s.rect_fill(gx - 3, gy + 2, gx - 1, gy + 4, ink, 235);
                s.rect_fill(gx + 1, gy + 2, gx + 3, gy + 4, ink, 235);
            }
            _ => {
                s.round_rect_stroke((gx - g) as f32, (gy - g) as f32,
                                    (gx + g) as f32, (gy + g) as f32, 1.0, ink, 235);
                s.rect_fill(gx - 2, gy - 2, gx + 1, gy + 1, ink, 255);
            }
        }
        return;
    }

    let g = if size >= 20 { 4 } else { 3 };
    let ink = 0xffffffu32;
    match glyph {
        0 => {                       /* system settings: two flat sliders */
            s.rect_fill(gx - g, gy - 4, gx + g, gy - 3, ink, 255);
            s.rect_fill(gx - g, gy + 3, gx + g, gy + 4, ink, 255);
            s.round_rect((gx - g + 1) as f32, (gy - 6) as f32,
                         (gx - g + 4) as f32, (gy - 1) as f32, 2.0, ink, 255);
            s.round_rect((gx + g - 4) as f32, (gy + 1) as f32,
                         (gx + g - 1) as f32, (gy + 6) as f32, 2.0, ink, 255);
        }
        1 => {                       /* task manager: ascending bars */
            s.rect_fill(gx - g, gy + g - 1, gx + g, gy + g, ink, 255);
            s.rect_fill(gx - g, gy + 1, gx - g + 2, gy + g - 1, ink, 255);
            s.rect_fill(gx - 1, gy - 2, gx + 1, gy + g - 1, ink, 255);
            s.rect_fill(gx + g - 2, gy - g, gx + g, gy + g - 1, ink, 255);
        }
        2 => {                       /* image viewer: frame, sun, horizon */
            s.round_rect_stroke((gx - g) as f32, (gy - g) as f32,
                                (gx + g) as f32, (gy + g) as f32, 1.0, ink, 255);
            s.round_rect((gx - g + 2) as f32, (gy - g + 2) as f32,
                         (gx - g + 4) as f32, (gy - g + 4) as f32, 1.0, ink, 255);
            s.rect_fill(gx - g + 1, gy + 1, gx + g - 1, gy + 2, ink, 255);
            s.rect_fill(gx - 1, gy - 1, gx + 1, gy + 1, ink, 255);
        }
        3 => {                       /* text editor: four document lines */
            s.rect_fill(gx - g, gy - g, gx + g, gy - g + 1, ink, 255);
            s.rect_fill(gx - g, gy - 1, gx + g - 1, gy, ink, 255);
            s.rect_fill(gx - g, gy + 2, gx + g, gy + 3, ink, 255);
            s.rect_fill(gx - g, gy + 5, gx + g - 3, gy + 6, ink, 255);
        }
        4 => {                       /* wayland client: surface + pointer */
            s.round_rect_stroke((gx - g) as f32, (gy - g) as f32,
                                (gx + g) as f32, (gy + g) as f32, 1.0, ink, 200);
            s.rect_fill(gx + 1, gy, gx + 5, gy + 5, ink, 235);
            s.rect_fill(gx + 1, gy + 5, gx + 3, gy + 6, ink, 235);
        }
        5 => {                       /* calculator: equals + keypad dots */
            s.rect_fill(gx - g, gy - g, gx + g, gy - g + 1, ink, 255);
            s.rect_fill(gx - g, gy - g + 2, gx + g, gy - g + 3, ink, 255);
            for dy in [0i32, 4] {
                for dx in [0i32, 4] {
                    s.round_rect((gx - g + dx) as f32, (gy + dy) as f32,
                                 (gx - g + dx + 2) as f32, (gy + dy + 2) as f32,
                                 1.0, ink, 235);
                }
            }
        }
        _ => {                       /* default: three menu lines */
            s.rect_fill(gx - g, gy - g, gx + g, gy - g + 1, ink, 255);
            s.rect_fill(gx - g, gy, gx + 1, gy + 1, ink, 255);
            s.rect_fill(gx - g, gy + g - 1, gx + g, gy + g, ink, 255);
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

/// v0.60: integer rounded rectangle (r <= 6) for the taskbar/menu layers:
/// a middle band plus one rect per corner row. The analytic `round_rect` pass
/// costs one soft-float sqrt per edge pixel and these layers are re-composed on
/// every damage frame, so the integer profile is the right trade here.
fn rrow(s: &mut Surface<'_>, x0: i32, y0: i32, x1: i32, y1: i32, r: i32,
        c: u32, a: u32) {
    let rr = r.min(((y1 - y0) / 2).max(1)).min(((x1 - x0) / 2).max(1));
    for k in 0..rr {
        let ins = rr - k;
        s.rect_fill(x0 + ins, y0 + k, x1 - ins, y0 + k + 1, c, a);
        s.rect_fill(x0 + ins, y1 - 1 - k, x1 - ins, y1 - k, c, a);
    }
    s.rect_fill(x0, y0 + rr, x1, y1 - rr, c, a);
}

/// v0.60: 22x22 title-bar control tile with a 2 px chamfer (integer only).
fn ctl_tile(s: &mut Surface<'_>, x: i32, y: i32, w: i32, h: i32, a: u32) {
    s.rect_fill(x + 2, y, x + w - 2, y + h, 0xffffff, a);
    s.rect_fill(x, y + 2, x + w, y + h - 2, 0xffffff, a);
    s.rect_fill(x + 1, y + 1, x + w - 1, y + h - 1, 0xffffff, a);
}

/// v0.71: the flat window - opaque body, 1 px outline, flat title strip and
/// glyph-only controls. No shadow, no frosted sample, no gradient, no highlight.
fn draw_window_flat(s: &mut Surface<'_>, w: &Win) {
    s.round_rect(w.x as f32, w.y as f32, (w.x + w.w) as f32, (w.y + w.h) as f32,
                 10.0, FLAT_PANEL, 255);
    s.round_rect_stroke(w.x as f32 + 0.5, w.y as f32 + 0.5,
                        (w.x + w.w) as f32 - 0.5, (w.y + w.h) as f32 - 0.5,
                        10.0, FLAT_LINE, 255);
    /* title strip: inside the rounded top, separated by one hairline */
    s.rect_fill(w.x + 2, w.y + 2, w.x + w.w - 2, w.y + TITLE_H, FLAT_TITLE, 255);
    s.rect_fill(w.x + 2, w.y + TITLE_H - 1, w.x + w.w - 2, w.y + TITLE_H,
                FLAT_LINE, 255);
    /* the author kept one accent cue per app: a 2 px identity bar */
    s.rect_fill(w.x + 6, w.y + 7, w.x + 8, w.y + TITLE_H - 6, w.accent, 255);
    draw_app_icon(s, w.x + 14, w.y + 8, TITLE_H - 16, w.accent,
                  glyph_for_accent(w.accent));
    s.text(w.x + 36, w.y + 7, w.title, FLAT_TEXT, 0xff, 1);
    /* glyph-only controls: minimise bar, maximise frame, close cross */
    let (bx, by, bw, bh) = control_rect(w);
    for k in 0..3 {
        let x = bx + k * (bw + 4);
        let ink = 0xd7dfeeu32;
        if k == 0 {
            s.rect_fill(x + 7, by + 14, x + 15, by + 15, ink, 255);
        } else if k == 1 {
            s.rect_fill(x + 7, by + 7, x + 15, by + 8, ink, 255);
            s.rect_fill(x + 7, by + 14, x + 15, by + 15, ink, 255);
            s.rect_fill(x + 7, by + 7, x + 8, by + 15, ink, 255);
            s.rect_fill(x + 14, by + 7, x + 15, by + 15, ink, 255);
        } else {
            for d in 0..8 {
                s.rect_fill(x + 7 + d, by + 7 + d, x + 8 + d, by + 8 + d, ink, 255);
                s.rect_fill(x + 14 - d, by + 7 + d, x + 15 - d, by + 8 + d, ink, 255);
            }
        }
    }
    /* v0.71 BUGFIX: the application content (settings rows, task-manager bars,
     * image, editor, keypad) is painted at the end of the glass path; the flat
     * early-return skipped it, so flat windows came up empty. Same call here. */
    crate::apps::draw_body(s, w.accent, w.x, w.y, w.w, w.h);
}

fn draw_window(s: &mut Surface<'_>, w: &Win, with_shadow: bool) {
    if flat_ui() {
        draw_window_flat(s, w);
        return;
    }
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

    /* title bar: translucent strip + accent identity bar + pseudo-3D app icon +
     * title text + a faint accent underline (Fluent cues; the rounded top corners
     * come from the body's AA silhouette) */
    s.rect_fill(w.x + 2, w.y + 1, w.x + w.w - 2, w.y + TITLE_H, 0xffffff, 0x18);
    /* v0.60 polish: 2 px leading accent bar - the Fluent "which app is this" cue,
     * and it anchors the eye on a busy wallpaper */
    s.rect_fill(w.x + 5, w.y + 9, w.x + 7, w.y + TITLE_H - 9, w.accent, 0xf0);
    draw_app_icon(s, w.x + 15, w.y + 8, TITLE_H - 16, w.accent,
                  glyph_for_accent(w.accent));
    s.text(w.x + 36, w.y + 7, w.title, 0xf4f8ff, 0xf2, 1);
    s.rect_fill(w.x + 2, w.y + TITLE_H - 1, w.x + w.w - 2, w.y + TITLE_H,
                w.accent, 0x3c);
    /* 1 px inner highlight where the title strip meets the body ("glass edge") */
    s.rect_fill(w.x + 9, w.y + TITLE_H, w.x + w.w - 9, w.y + TITLE_H + 1,
                0xffffff, 0x10);

    /* controls */
    let (bx, by, bw, bh) = control_rect(w);
    for k in 0..3 {
        let x = bx + k * (bw + 4);
        ctl_tile(s, x, by, bw, bh, 0x16);
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

// ---------------------------------------------------------------- exports (v0.61)

/// Screen rectangle of the dedicated Wayland window, for the kernel's Wayland
/// server: it converts the PS/2 pointer into surface-local coordinates and only
/// emits `wl_pointer` enter/motion while the pointer is inside this rectangle.
/// `None` while the window is hidden (no pointer events then).
pub fn wayland_rect() -> Option<(i32, i32, i32, i32)> {
    let sh = unsafe { &*core::ptr::addr_of!(SHELL) };
    let w = sh.wins[4];
    if !w.open || w.minimized {
        return None;
    }
    Some((w.x, w.y, w.w, w.h))
}

/// 1 while the Wayland window is the focused window (and no menu is stealing the
/// keyboard), so the server only forwards `wl_keyboard` events in that case.
pub fn wayland_focused() -> bool {
    let sh = unsafe { &*core::ptr::addr_of!(SHELL) };
    sh.wins[4].open && !sh.wins[4].minimized && sh.focus == 4 && !sh.menu_open
}
