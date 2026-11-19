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

pub mod swrast;

use swrast::Surface;

// ---------------------------------------------------------------------------
// Kernel ABI
// ---------------------------------------------------------------------------
extern "C" {
    fn console_printf(fmt: *const u8, ...) -> i32;
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

/// Draw the whole prototype frame into the framebuffer.
///
/// `fb` is the linear framebuffer as u32 pixels (0x00RRGGBB); `stride` is the
/// pitch in pixels (>= w). No allocation: everything is drawn in place.
pub fn draw_scene(s: &mut Surface) {
    let (w, h) = (s.w as i32, s.h as i32);

    // 1. wallpaper: 3-stop vertical gradient. A gradient on the window
    //    background is impossible in the Slint renderer (TODO there), which is
    //    exactly why it is a plain full-screen rect here.
    s.vgrad3(0, 0, w, h, WALL_TOP, WALL_MID, WALL_BOT, 0.45);

    // 2/3. colour backdrops: they are what makes the panel's translucency
    //      measurable (same panel pixel over backdrop vs over wallpaper).
    s.hgrad_round(90.0, 110.0, 510.0, 530.0, 96.0, 0x3f7bff, 0x9a5cff, 0xd8);
    s.hgrad_round(640.0, 90.0, 940.0, 390.0, 96.0, 0x12b8a6, 0x2f6bff, 0xc0);

    // 4. main panel: emulated soft shadow, translucent fill, hairline border.
    s.fake_shadow(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, PANEL_R, 8.0, 0x000000, 1.0);
    s.round_rect(
        PANEL_X,
        PANEL_Y,
        PANEL_X + PANEL_W,
        PANEL_Y + PANEL_H,
        PANEL_R,
        0xffffff,
        0x26,
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
    s.round_rect(lx, ly, lx + lw, ly + 74.0, 12.0, 0xffffff, 0x12);
    s.round_rect(lx + 12.0, ly + 14.0, lx + lw - 12.0, ly + 60.0, 10.0, 0xffffff, 0x16);
    s.round_rect(lx + 24.0, ly + 26.0, lx + lw - 24.0, ly + 48.0, 8.0, 0xffffff, 0x1c);

    // 7. two list rows.
    for row in 0..2 {
        let ry = PANEL_Y + 220.0 + row as f32 * 38.0;
        s.round_rect(lx, ry, lx + lw, ry + 30.0, 9.0, 0xffffff, 0x10);
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
            a.min(255),
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
    0x0003_0000 // v3 = hand-written rasteriser + text (v1 = Slint build, v2 = shapes only)
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
            stride: pu,
        };
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
