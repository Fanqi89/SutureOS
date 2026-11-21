//! SutureOS GUI prototype (Stage 0/1): the first frame, drawn in Rust.
//!
//! ## What is in here
//! 1. `swrast` — a hand-written software rasteriser (rounded rects with analytic
//!    AA, axis-aligned gradients, alpha compositing, stacked-rect shadows,
//!    pseudo-3D icons). This draws the prototype frame that `guislint` shows.
//! 2. (feature `slint`) the Slint integration that this batch proved links and
//!    initialises inside the kernel. It is kept in the tree, off by default,
//!    because Slint's software renderer hangs on the full scene (see
//!    docs/slint-gui-stage0.md): `[gui-slint] platform ready` -> `stage: draw`
//!    -> never returns. `cargo build --features slint` reproduces it.
//!
//! ## Kernel ABI used here (nothing else)
//! * `console_printf(fmt, ...)`      — kernel/lib/kprintf.c (no width/flags)
//! * `lemis_heap_alloc/free`         — kernel/mm/heap.c (16-byte aligned payload)
//! * `g_tick_count`                  — kernel/drivers/mb_pit.c, ~100 Hz
//!
//! The C side (`kernel/gui/slint_bridge.c`) owns the weak fallbacks and the
//! serial-log line; the shell command is `guislint`.

#![no_std]

#[cfg(feature = "slint")]
extern crate alloc;

/* v0.59.0: per-window application content (settings rows, live task manager
 * numbers, the logo image viewer, the editor buffer) lives in its own module so
 * the shell keeps owning only window management. */
pub mod apps;
pub mod shell;
pub mod swrast;

use swrast::Surface;

// ---------------------------------------------------------------------------
// Kernel ABI
// ---------------------------------------------------------------------------
extern "C" {
    fn console_printf(fmt: *const u8, ...) -> i32;
    static g_tick_count: u64;
}

fn log0(msg: &core::ffi::CStr) {
    unsafe { console_printf(b"[gui-slint] %s\n\0".as_ptr(), msg.as_ptr()) };
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    log0(c"PANIC in Rust GUI");
    loop {
        core::hint::spin_loop();
    }
}

// ---------------------------------------------------------------------------
// Scene constants (kept in one place: scripts/slint_gui_acceptance.py asserts
// these exact coordinates).
// ---------------------------------------------------------------------------
const WALL_TOP: u32 = 0x1a2338;
const WALL_MID: u32 = 0x121a2b;
const WALL_BOT: u32 = 0x0a0e18;

const PANEL_X: f32 = 150.0;
const PANEL_Y: f32 = 170.0;
const PANEL_W: f32 = 560.0;
const PANEL_H: f32 = 320.0;
const PANEL_R: f32 = 18.0;

const CARD_X: f32 = 760.0;
const CARD_Y: f32 = 420.0;
const CARD_W: f32 = 210.0;
const CARD_H: f32 = 210.0;
const CARD_R: f32 = 16.0;

const ICON_S: f32 = 72.0;
const ICON1_X: f32 = PANEL_X + 28.0;
const ICON1_Y: f32 = PANEL_Y + 28.0;
const ICON2_X: f32 = CARD_X + 69.0;

/* Text layout (v0.38.0). scripts/slint_gui_acceptance.py mirrors these exact
 * numbers to pixel-match the glyphs, so keep the two in sync. */
const TITLE_X: i32 = 262;                 /* PANEL_X + 112 */
const TITLE_Y: i32 = 200;                 /* PANEL_Y + 30  */
const SUBTITLE_X: i32 = 262;
const SUBTITLE_Y: i32 = 246;              /* PANEL_Y + 76  */
const ROW_TEXT_DX: i32 = 166;             /* PANEL_X + 28 + 16 (= lx + 16) */
const ROW1_Y: i32 = 396;                  /* PANEL_Y + 220 + 7  */
const ROW2_Y: i32 = 434;                  /* + 38               */
const CARD_TEXT_X: i32 = 786;             /* CARD_X + 26        */
const CARD_TEXT_Y: i32 = 548;             /* CARD_Y + 128       */
const ICON2_Y: f32 = CARD_Y + 40.0;

/// Draw the whole prototype frame into the framebuffer (static prototype).
///
/// `fb` is the linear framebuffer as u32 pixels (0x00RRGGBB); `stride` is the
/// pitch in pixels (>= w). No allocation: everything is drawn in place.
pub fn draw_scene(s: &mut Surface) {
    draw_scene_ui(s, 1.0, -1, (0, 0, 0, 0));
}

/// Interactive variant: `fade` 0..1 ramps the panel in, `hover` is the list row
/// under the pointer (-1 = none) and `ripple` is (x, y, age_ms, duration_ms) of
/// the last click, drawn as an expanding translucent ring.
pub fn draw_scene_ui(s: &mut Surface, fade: f32, hover: i32,
                     ripple: (i32, i32, u32, u32)) {
    let (w, h) = (s.w as i32, s.h as i32);
    let fa = |a: u32| -> u32 { ((a as f32) * fade) as u32 };

    // 1. wallpaper: 3-stop vertical gradient. A gradient on the window
    //    background is impossible in the Slint renderer (TODO there), which is
    //    exactly why it is a plain full-screen rect here.
    s.vgrad3(0, 0, w, h, WALL_TOP, WALL_MID, WALL_BOT, 0.45);

    // 2/3. colour backdrops: they are what makes the panel's translucency
    //      measurable (same panel pixel over backdrop vs over wallpaper).
    s.hgrad_round(90.0, 110.0, 510.0, 530.0, 96.0, 0x3f7bff, 0x9a5cff, 0xd8);
    s.hgrad_round(640.0, 90.0, 940.0, 390.0, 96.0, 0x12b8a6, 0x2f6bff, 0xc0);

    // 3b. acrylic (v0.39.0): blur the backdrop under the panel BEFORE the panel
    //     is composited, so the panel shows a frosted copy of the blobs/wallpaper
    //     behind it instead of a semi-transparent copy. Radius 16 px reads as
    //     "light acrylic"; the halo margin is part of the blur so edges stay
    //     smooth. Cost: two separable passes over ~(560+32)x(320+32) = O(200k) px.
    swrast::acrylic_blur(s, PANEL_X as i32, PANEL_Y as i32,
                         (PANEL_X + PANEL_W) as i32,
                         (PANEL_Y + PANEL_H) as i32, 18);
    swrast::acrylic_blur(s, CARD_X as i32, CARD_Y as i32,
                         (CARD_X + CARD_W) as i32,
                         (CARD_Y + CARD_H) as i32, 14);

    // 4. main panel: emulated soft shadow, translucent fill, hairline border.
    s.fake_shadow(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, PANEL_R, 8.0, 0x000000, 1.0);
    s.round_rect(
        PANEL_X,
        PANEL_Y,
        PANEL_X + PANEL_W,
        PANEL_Y + PANEL_H,
        PANEL_R,
        0xf4f8ff,
        fa(0x3a),
    );
    s.round_rect_stroke(
        PANEL_X,
        PANEL_Y,
        PANEL_X + PANEL_W,
        PANEL_Y + PANEL_H,
        PANEL_R,
        0xffffff,
        0x40,
    );

    // 5. pseudo-3D icon in the panel.
    s.icon(ICON1_X, ICON1_Y, ICON_S);

    // 6. micro translucent layer stack (light acrylic; no blur yet).
    let (lx, ly, lw) = (PANEL_X + 28.0, PANEL_Y + 128.0, 504.0);
    s.round_rect(lx, ly, lx + lw, ly + 74.0, 12.0, 0xffffff, fa(0x12));
    s.round_rect(lx + 12.0, ly + 14.0, lx + lw - 12.0, ly + 60.0, 10.0, 0xffffff, fa(0x16));
    s.round_rect(lx + 24.0, ly + 26.0, lx + lw - 24.0, ly + 48.0, 8.0, 0xffffff, fa(0x1c));

    // 7. two list rows.
    for row in 0..2 {
        let ry = PANEL_Y + 220.0 + row as f32 * 38.0;
        let hot = hover == row as i32;
        s.round_rect(lx, ry, lx + lw, ry + 30.0, 9.0, 0xffffff,
                     fa(if hot { 0x34 } else { 0x10 }));
        if hot {
            // left accent bar: the fluent focus/selection indicator
            s.round_rect(lx + 4.0, ry + 6.0, lx + 7.0, ry + 24.0, 1.5, 0x4f8dff, 255);
        }
    }

    // 8/9. secondary card: shadow + vertical translucent gradient + icon.
    s.fake_shadow(CARD_X, CARD_Y, CARD_W, CARD_H, CARD_R, 6.0, 0x000000, 0.9);
    // vertical alpha gradient (0x30 -> 0x12) drawn row by row inside the shape
    for row in 0..(CARD_H as i32) {
        let t = row as f32 / CARD_H;
        let a = (0x30 as f32 + (0x12 as f32 - 0x30 as f32) * t) as u32;
        s.round_rect(
            CARD_X,
            CARD_Y + row as f32,
            CARD_X + CARD_W,
            CARD_Y + row as f32 + 1.0,
            CARD_R,
            0xffffff,
            fa(a.min(255)),
        );
    }
    s.round_rect_stroke(CARD_X, CARD_Y, CARD_X + CARD_W, CARD_Y + CARD_H, CARD_R, 0xffffff, 0x3a);
    s.icon(ICON2_X, ICON2_Y, ICON_S);
    // the card's copy is real text now (see the text block below), so the
    // placeholder bars that used to sit here were removed.

    // 10. text (v0.38.0): ASCII from font8x16, Chinese from the kernel's GB2312
    //     16x16 blob, both fetched through the C bridge (no second font copy).
    //     Headings use alpha 255 so acceptance can pixel-match the glyphs.
    s.text(TITLE_X, TITLE_Y, "SutureOS 设置", 0xffffff, 255, 2);
    s.text(SUBTITLE_X, SUBTITLE_Y, "圆角 + 轻亚克力 + 微透叠层", 0xcfe0ff, 255, 1);
    s.text(ROW_TEXT_DX, ROW1_Y, "账户与登录", 0xe8eefb, 255, 1);
    s.text(ROW_TEXT_DX, ROW2_Y, "显示与外观", 0xe8eefb, 255, 1);
    s.text(CARD_TEXT_X, CARD_TEXT_Y, "个性化", 0xe8eefb, 255, 1);
    s.text(CARD_TEXT_X, CARD_TEXT_Y + 26, "控制面板", 0x9fb2cc, 255, 1);

    // 11. click ripple (v0.40.0): expanding translucent ring, 450 ms
    let (rx, ry, age, dur) = ripple;
    if dur > 0 && age < dur {
        let t = age as f32 / dur as f32;
        let rad = 6.0 + t * 46.0;
        let alpha = ((1.0 - t) * 0.42 * 255.0) as u32;
        let steps = 180;
        for i in 0..steps {
            let ang = (i as f32) * 6.2831853 / steps as f32;
            let px = rx as f32 + libm::cosf(ang) * rad;
            let py = ry as f32 + libm::sinf(ang) * rad;
            s.blend_px(px as usize, py as usize, 0x9fc4ff, alpha);
        }
    }
}

/// FNV-1a over the framebuffer's RGB bits — the kernel prints this so "a frame
/// really exists" is a log assertion instead of a claim.
fn checksum(px: &[u32]) -> u32 {
    let mut hash: u32 = 0x811c9dc5;
    for v in px {
        hash ^= v & 0x00ff_ffff;
        hash = hash.wrapping_mul(0x0100_0193);
    }
    hash
}

// ---------------------------------------------------------------------------
// C ABI exported to the kernel (kernel/gui/slint_bridge.c)
// ---------------------------------------------------------------------------

/// Non-zero when the Rust GUI object is really linked into the image.
#[no_mangle]
pub extern "C" fn suture_gui_version() -> u32 {
    0x0004_0000 // v4 = rasteriser + text + acrylic blur (v2 shapes, v3 text, v1 Slint)
}

/// Which renderer backs this build: 2 = hand-written rasteriser, 1 = Slint.
#[no_mangle]
pub extern "C" fn suture_gui_renderer() -> u32 {
    if cfg!(feature = "slint") {
        1
    } else {
        2
    }
}


// ---------------------------------------------------------------------------
// Static scene cache (v0.40.0)
// ---------------------------------------------------------------------------
// One full-scene render costs ~1.5 s in QEMU TCG (analytic anti-aliased rounded
// rects + 4-layer shadows do a sqrt per pixel), which is far too slow to redraw
// at 30 fps. The prototype scene is static though: wallpaper, blobs, acrylic
// blur, panel, text never change between frames. So the scene is rendered ONCE
// into this cache and each interactive frame composites the cache + the small
// dynamic layers (hover highlight, click ripple, cursor).
const CACHE_W: usize = 1024;
const CACHE_H: usize = 768;
static mut SCENE_CACHE: [u32; CACHE_W * CACHE_H] = [0; CACHE_W * CACHE_H];
static mut SCENE_BUILT: bool = false;
static mut SCENE_SUM: u32 = 0;

/// Seed the interactive scene cache from an already-rendered framebuffer.
///
/// `guislint` draws the static frame first (slow: analytic AA + shadows cost a
/// sqrt per pixel, ~seconds under TCG), so copying THAT image into the cache
/// avoids rendering the same scene a second time before the loop can start.
///
/// # Safety
/// `fb` must be a readable framebuffer of at least `pitch_px * h` u32 pixels.
#[no_mangle]
pub unsafe extern "C" fn suture_gui_cache_seed(
    fb: *const u32,
    w: i32,
    h: i32,
    pitch_px: i32,
) -> u32 {
    if fb.is_null() || w <= 0 || h <= 0 || pitch_px < w {
        return 0;
    }
    let (wu, hu, pu) = (w as usize, h as usize, pitch_px as usize);
    if wu > CACHE_W || hu > CACHE_H {
        return 0;
    }
    let cache = &mut *core::ptr::addr_of_mut!(SCENE_CACHE);
    let src = core::slice::from_raw_parts(fb, pu * hu);
    for y in 0..hu {
        cache[y * CACHE_W..y * CACHE_W + wu]
            .copy_from_slice(&src[y * pu..y * pu + wu]);
    }
    SCENE_SUM = checksum(&cache[..CACHE_W * hu]);
    SCENE_BUILT = true;
    SCENE_SUM
}

unsafe fn ensure_scene_cache(w: usize, h: usize, stride: usize) -> bool {
    if SCENE_BUILT {
        return true;
    }
    if w > CACHE_W || h > CACHE_H {
        return false;                    /* cache shape mismatch: caller falls back */
    }
    let cache = &mut *core::ptr::addr_of_mut!(SCENE_CACHE);
    let mut cs = swrast::Surface { px: &mut cache[..], w, h, stride, clip: (0, 0, 1024, 768) };
    draw_scene(&mut cs);
    SCENE_SUM = checksum(&cs.px[..stride * h]);
    SCENE_BUILT = true;
    true
}

// ---------------------------------------------------------------------------
// Interactive mode (v0.40.0)
// ---------------------------------------------------------------------------
// The shell command `guislint` first renders the static frame (so the v0.39
// pixel checks still run against a deterministic image) and then enters this
// loop: the kernel polls PS/2 mouse + keyboard and calls us once per frame with
// the pointer position, pressed buttons, click edges and a millisecond clock.
// Everything animated here is a pure function of that clock, never of a hidden
// timer, so a screenshot at time T is reproducible.
struct UiState {
    start_ms: u32,
    hover: i32,
    click_x: i32,
    click_y: i32,
    click_ms: u32,
    have_click: bool,
}

static mut UI: UiState = UiState {
    start_ms: 0,
    hover: -1,
    click_x: 0,
    click_y: 0,
    click_ms: 0,
    have_click: false,
};

const FADE_MS: u32 = 450;
const RIPPLE_MS: u32 = 450;

fn log_i32(msg: &core::ffi::CStr, a: i32, b: i32) {
    unsafe { console_printf(b"[gui-slint] %s %d %d\n\0".as_ptr(), msg.as_ptr(), a as i32, b as i32) };
}

/// One interactive frame. Returns the frame checksum (0 = nothing drawn).
///
/// # Safety
/// `fb` must be a valid framebuffer of at least `pitch_px * h` u32 pixels.
#[no_mangle]
pub unsafe extern "C" fn suture_gui_interactive_step(
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
) -> u32 {
    if fb.is_null() || w <= 0 || h <= 0 || pitch_px < w {
        return 0;
    }
    let (wu, hu, pu) = (w as usize, h as usize, pitch_px as usize);
    let mut s = Surface {
        px: core::slice::from_raw_parts_mut(fb, pu * hu),
        w: wu,
        h: hu,
        stride: pu, clip: (0, 0, 1024, 768) };
    let ui = &mut *core::ptr::addr_of_mut!(UI);
    if ui.start_ms == 0 {
        ui.start_ms = now_ms;
        log0(c"interactive: start");
    }
    let elapsed = now_ms.wrapping_sub(ui.start_ms);

    if !ensure_scene_cache(wu, hu, wu) {
        /* framebuffer larger than the cache: honest slow path */
        draw_scene_ui(&mut s, 1.0, -1, (0, 0, 0, 0));
        swrast::cursor(&mut s, mx.clamp(0, w - 1), my.clamp(0, h - 1));
        return checksum(s.px);
    }
    log0(c"scene cached");

    // 1. composite the static scene (fade-in during the first FADE_MS)
    let cache = &*core::ptr::addr_of!(SCENE_CACHE);
    if elapsed < FADE_MS {
        let a = ((elapsed as f32 / FADE_MS as f32) * 255.0) as u32;
        for y in 0..hu {
            let dst = y * pu;
            let src = y * CACHE_W;
            for x in 0..wu {
                let c = cache[src + x];
                let r = (((c >> 16) & 0xff) * a) / 255;
                let g = (((c >> 8) & 0xff) * a) / 255;
                let bl = ((c & 0xff) * a) / 255;
                s.px[dst + x] = (r << 16) | (g << 8) | bl;
            }
        }
    } else {
        for y in 0..hu {
            let dst = y * pu;
            let src = y * CACHE_W;
            s.px[dst..dst + wu].copy_from_slice(&cache[src..src + wu]);
        }
    }

    // 2. hover highlight for the list rows (the cache holds the idle look)
    let row0_y = (PANEL_Y as i32) + 220;
    let row1_y = row0_y + 38;
    let row_x0 = (PANEL_X as i32) + 28;
    let row_x1 = row_x0 + 504;
    let mut hover: i32 = -1;
    if mx >= row_x0 && mx < row_x1 {
        if my >= row0_y && my < row0_y + 30 {
            hover = 0;
        } else if my >= row1_y && my < row1_y + 30 {
            hover = 1;
        }
    }
    if hover != ui.hover {
        ui.hover = hover;
        log_i32(c"hover row", hover, my);
    }
    for row in 0..2 {
        let ry = row0_y + row * 38;
        let hot = hover == row;
        s.round_rect(row_x0 as f32, ry as f32, row_x1 as f32, (ry + 30) as f32, 9.0,
                     0xffffff, if hot { 0x34 } else { 0x00 });
        if hot {
            s.round_rect((row_x0 + 4) as f32, (ry + 6) as f32,
                         (row_x0 + 7) as f32, (ry + 24) as f32, 1.5, 0x4f8dff, 255);
        }
    }

    // 3. click edge -> ripple + log
    if click_x >= 0 && click_y >= 0 {
        ui.click_x = click_x;
        ui.click_y = click_y;
        ui.click_ms = now_ms;
        ui.have_click = true;
        log_i32(c"click at", click_x, click_y);
        let _ = (click_btn, buttons);
    }
    if ui.have_click {
        let age = now_ms.wrapping_sub(ui.click_ms);
        if age < RIPPLE_MS {
            let t = age as f32 / RIPPLE_MS as f32;
            let rad = 6.0 + t * 46.0;
            let alpha = ((1.0 - t) * 0.42 * 255.0) as u32;
            let steps = 180;
            for i in 0..steps {
                let ang = (i as f32) * 6.2831853 / steps as f32;
                let px = ui.click_x as f32 + libm::cosf(ang) * rad;
                let py = ui.click_y as f32 + libm::sinf(ang) * rad;
                if px >= 0.0 && py >= 0.0 {
                    s.blend_px(px as usize, py as usize, 0x9fc4ff, alpha);
                }
            }
        }
    }

    // 4. cursor on top
    swrast::cursor(&mut s, mx.clamp(0, w - 1), my.clamp(0, h - 1));
    checksum(s.px)
}

/// Draw one static frame straight into the linear framebuffer.
///
/// `fb` must point to `pitch_px * h` u32 pixels; `pitch_px` is the stride in
/// pixels (>= w). Returns a checksum of the drawn pixels (0 = nothing drawn).
///
/// # Safety
/// `fb` must be a valid writable framebuffer of at least `pitch_px * h` u32s.
#[no_mangle]
pub unsafe extern "C" fn suture_gui_selftest_render(
    fb: *mut u32,
    w: i32,
    h: i32,
    pitch_px: i32,
) -> u32 {
    if fb.is_null() || w <= 0 || h <= 0 || pitch_px < w {
        return 0;
    }
    let (wu, hu, pu) = (w as usize, h as usize, pitch_px as usize);
    let pixels = core::slice::from_raw_parts_mut(fb, pu * hu);

    #[cfg(feature = "slint")]
    {
        // Slint path (kept for the next batch; see the module docs).
        return slint_impl::render(pixels, wu, hu, pu);
    }

    #[cfg(not(feature = "slint"))]
    {
        let mut s = Surface {
            px: pixels,
            w: wu,
            h: hu,
            stride: pu, clip: (0, 0, 1024, 768) };
        draw_scene(&mut s);
        checksum(&s.px[..pu * hu])
    }
}

/// Frames drawn so far (diagnostics for the kernel log). The Stage-0 prototype
/// is a single static frame, so this is 0 or 1.
#[no_mangle]
pub extern "C" fn suture_gui_frames() -> u32 {
    1
}

// ---------------------------------------------------------------------------
// Optional Slint backend (feature `slint`). Kept because the link/init work is
// reusable; the full-scene draw hangs inside i-slint-renderer-software.
// ---------------------------------------------------------------------------
#[cfg(feature = "slint")]
mod slint_impl {
    use alloc::boxed::Box;
    use alloc::rc::Rc;
    use core::cell::UnsafeCell;
    use core::time::Duration;

    use slint::platform::software_renderer::{
        MinimalSoftwareWindow, PremultipliedRgbaColor, RepaintBufferType, TargetPixel,
    };
    use slint::platform::{Platform, WindowAdapter};
    use slint::{PhysicalSize, PlatformError};

    slint::include_modules!();

    extern "C" {
        fn lemis_heap_alloc(size: usize) -> *mut u8;
        fn lemis_heap_free(ptr: *mut u8);
        static g_tick_count: u64;
    }

    struct KernelHeap;
    unsafe impl alloc::alloc::GlobalAlloc for KernelHeap {
        unsafe fn alloc(&self, layout: core::alloc::Layout) -> *mut u8 {
            // lemis_heap_alloc guarantees 16-byte alignment; over-aligned
            // requests are over-allocated and carry the raw pointer 16 bytes
            // before the payload (which keeps the payload 16-byte aligned).
            const BACKREF: usize = 16;
            if layout.align() <= 16 {
                return lemis_heap_alloc(layout.size());
            }
            let raw = lemis_heap_alloc(layout.size() + layout.align() + BACKREF);
            if raw.is_null() {
                return raw;
            }
            let p = ((raw as usize) + BACKREF + layout.align() - 1) & !(layout.align() - 1);
            ((p - BACKREF) as *mut usize).write_volatile(raw as usize);
            p as *mut u8
        }
        unsafe fn dealloc(&self, ptr: *mut u8, layout: core::alloc::Layout) {
            if layout.align() <= 16 {
                lemis_heap_free(ptr);
            } else {
                let backref = (ptr as usize - 16) as *const usize;
                lemis_heap_free(backref.read_volatile() as *mut u8);
            }
        }
    }

    #[global_allocator]
    static ALLOC: KernelHeap = KernelHeap;

    #[derive(Clone, Copy, Default)]
    pub struct Bgrx(pub u32);

    impl TargetPixel for Bgrx {
        fn blend(&mut self, c: PremultipliedRgbaColor) {
            let a = (u8::MAX - c.alpha) as u32;
            let r = ((self.0 >> 16) & 0xff) * a / 255 + c.red as u32;
            let g = ((self.0 >> 8) & 0xff) * a / 255 + c.green as u32;
            let b = (self.0 & 0xff) * a / 255 + c.blue as u32;
            self.0 = (r << 16) | (g << 8) | b;
        }
        fn from_rgb(r: u8, g: u8, b: u8) -> Self {
            Bgrx(((r as u32) << 16) | ((g as u32) << 8) | b as u32)
        }
        fn background() -> Self {
            Bgrx(0)
        }
    }

    struct KernelPlatform {
        window: Rc<MinimalSoftwareWindow>,
    }

    impl Platform for KernelPlatform {
        fn create_window_adapter(&self) -> Result<Rc<dyn WindowAdapter>, PlatformError> {
            Ok(self.window.clone())
        }
        fn duration_since_start(&self) -> Duration {
            let ticks = unsafe { core::ptr::read_volatile(core::ptr::addr_of!(g_tick_count)) };
            Duration::from_millis(ticks.saturating_mul(10))
        }
    }

    struct State {
        window: Option<Rc<MinimalSoftwareWindow>>,
        ui: Option<DemoWindow>,
    }
    struct Cell(UnsafeCell<State>);
    unsafe impl Sync for Cell {}
    static GUI: Cell = Cell(UnsafeCell::new(State {
        window: None,
        ui: None,
    }));

    pub fn render(px: &mut [u32], w: usize, h: usize, stride: usize) -> u32 {
        let st = unsafe { &mut *GUI.0.get() };
        if st.window.is_none() {
            let window = MinimalSoftwareWindow::new(RepaintBufferType::NewBuffer);
            if slint::platform::set_platform(Box::new(KernelPlatform {
                window: window.clone(),
            }))
            .is_err()
            {
                return 0;
            }
            match DemoWindow::new() {
                Ok(ui) => {
                    window.set_size(PhysicalSize::new(w as u32, h as u32));
                    st.ui = Some(ui);
                    st.window = Some(window);
                }
                Err(_) => return 0,
            }
        }
        slint::platform::update_timers_and_animations();
        let Some(window) = st.window.as_ref() else {
            return 0;
        };
        let buf = unsafe {
            core::slice::from_raw_parts_mut(px.as_mut_ptr() as *mut Bgrx, stride * h)
        };
        window.draw_if_needed(|renderer| {
            renderer.render(buf, stride);
        });
        super::checksum(px)
    }
}

/// Draw one shell frame (windows + taskbar + start menu). Returns checksum.
///
/// # Safety
/// `fb` must be a valid framebuffer of at least `pitch_px * h` u32 pixels.
#[no_mangle]
pub unsafe extern "C" fn suture_gui_shell_step(
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
    key: i32,
    ch: i32,
    clock_hhmm: i32,
) -> u32 {
    shell::shell_step(fb, w, h, pitch_px, mx, my, buttons, click_x, click_y,
                      click_btn, now_ms, key, ch, clock_hhmm)
}

/// Log the swap-chain counters once (`guishell swapstat`). Returns presents.
#[no_mangle]
pub unsafe extern "C" fn suture_gui_swap_report() -> u32 {
    shell::swap_report()
}

/// Kernel tick counter (~100 Hz), exposed for the shell frame profiler.
#[no_mangle]
pub extern "C" fn shell_tick() -> u64 {
    unsafe { g_tick_count }
}

#[no_mangle]
pub extern "C" fn suture_gui_wayland_rect(x: *mut i32, y: *mut i32,
                                          w: *mut i32, h: *mut i32) -> i32 {
    match crate::shell::wayland_rect() {
        Some((rx, ry, rw, rh)) => {
            unsafe {
                *x = rx;
                *y = ry;
                *w = rw;
                *h = rh;
            }
            1
        }
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn suture_gui_wayland_focused() -> i32 {
    crate::shell::wayland_focused() as i32
}
