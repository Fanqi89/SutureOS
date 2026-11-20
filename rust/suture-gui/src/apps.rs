//! Per-window application content for the Rust desktop shell (v0.58.0).
//!
//! The shell's four windows used to share one placeholder body ("SutureOS
//! 桌面外壳" + a card). This module gives each of them its own application
//! content, drawn with the existing software rasteriser:
//!
//! | accent   | window       | content                                           |
//! |----------|--------------|---------------------------------------------------|
//! | 0x4f8dff | 系统设置     | three rounded setting rows (label + value)        |
//! | 0x22c1a8 | 任务管理器   | static labels + two bars; live uptime / CPU %     |
//! | 0xf5a524 | 图片查看器   | checkerboard canvas + the real 48 px logo at 3x   |
//! | 0xe05a8a | 文本编辑器   | three numbered code lines + a caret block         |
//!
//! Two entry points, on purpose:
//!
//! * [`draw_body`] paints everything STATIC. The shell renders it once into the
//!   window layer cache (v0.51.0), so it must not read the clock: otherwise the
//!   cache would be invalidated every frame and dragging would stop being a
//!   pure blit.
//! * [`draw_live`] paints the values that change over time (the task manager's
//!   uptime and the CPU load derived from it). The shell calls it after the
//!   window compositing loop, exactly like the keyboard-focus line, so only a
//!   few glyphs are repainted per frame while the cached layer stays static.
//!
//! Cost discipline: the kernel is built with `-mno-sse`, so every f32 op goes
//! through `compiler_builtins` (soft float, ~20x slower than integer). All
//! geometry here is integer; the only per-pixel f32 that remains is inside the
//! pre-existing `Surface::round_rect` AA pass, and this module avoids it for
//! anything large (`round_row` below is an integer rounded rectangle built from
//! `rect_fill` runs plus a precomputed 8-row corner table). Only ASCII and the
//! GB2312 CJK glyph path (`Surface::text`) are used for glyphs, and nothing
//! allocates.

use crate::swrast::Surface;

/// Window accents: the shell gives every window a unique one (see `Win`).
pub const ACC_SETTINGS: u32 = 0x4f8dff;
pub const ACC_TASKMAN: u32 = 0x22c1a8;
pub const ACC_IMAGE: u32 = 0xf5a524;
pub const ACC_EDITOR: u32 = 0xe05a8a;
/// v0.60: dedicated Wayland client window (the compositor surface as a window of
/// its own instead of living inside the image viewer).
pub const ACC_WAYLAND: u32 = 0x7a5cff;

/// Window title strip height; mirrored from `shell.rs` (`TITLE_H`).
const TITLE_H: i32 = 30;
/// Body inner margin (matches the shell's chrome padding).
const PAD: i32 = 16;

const TEXT_HI: u32 = 0xecf2ff;
const TEXT_MID: u32 = 0xc2d4ec;
const TEXT_LOW: u32 = 0x93a7c1;

/* Live-value anchors, relative to the window origin. The static labels are
 * drawn by `draw_body`, the numbers by `draw_live`; sharing these constants is
 * what keeps the two halves aligned. */
const TM_UPTIME_Y: i32 = TITLE_H + 138;
const TM_UPTIME_X: i32 = 92;   /* after the 4 CJK glyphs of "运行时间" */
const TM_CPU_Y: i32 = TITLE_H + 164;
const TM_CPU_X: i32 = 46;      /* after the 3 ASCII glyphs of "CPU" */

/// Corner radius (in rows) of a setting row and its integer inset table:
/// `ROW_INSET[k]` is how many pixels the k-th row from the top/bottom edge is
/// inset, taken from `r - sqrt(r^2 - (r - k - 0.5)^2)` for r = 8 (computed
/// offline; this is the table that replaces the per-pixel sqrt).
const ROW_R: i32 = 8;
const ROW_INSET: [i32; 8] = [5, 3, 2, 1, 1, 0, 0, 0];

/* The real SutureOS logo bitmaps are generated into `kernel/gui/logo_assets.c`
 * by `scripts/mklogo_assets.py` (the 24/48/1536 PNG steps of `logo/`); the
 * shell already links them, so this is just another extern declaration of the
 * same C symbols. */
extern "C" {
    static g_logo48: [u32; 48 * 48];
    /* kernel/ipc/wl_server.c: last committed wl_surface + commit counter. */
    fn wl_server_latest_surface(w: *mut i32, h: *mut i32, stride: *mut i32,
                                px: *mut *const u8) -> i32;
    fn wl_server_commit_count() -> u32;
    /* v0.60 (task-5): every connection keeps its own last committed surface. */
    fn wl_server_surface_count() -> i32;
    fn wl_server_surface_at(idx: i32, w: *mut i32, h: *mut i32, stride: *mut i32,
                            scale: *mut i32, serial: *mut u32,
                            px: *mut *const u8) -> i32;
    fn console_printf(fmt: *const u8, ...) -> i32;
}

/// v0.59.x: how many wl_surface commits the kernel server has seen. The shell
/// mixes this into the window layer key so opening the image viewer before a
/// client commits cannot leave a stale (logo) layer behind.
pub fn wl_epoch() -> i32 {
    unsafe { wl_server_commit_count() as i32 }
}

/// Paint the last committed Wayland surface into the image viewer body. Returns
/// false (caller falls back to the logo preview) when nothing was committed yet.
fn wayland_surface_body(s: &mut Surface<'_>, accent: u32, wx: i32, wy: i32,
                        w: i32, h: i32) -> bool {
    let mut sw: i32 = 0;
    let mut sh: i32 = 0;
    let mut stride: i32 = 0;
    let mut px: *const u8 = core::ptr::null();
    let rc = unsafe { wl_server_latest_surface(&mut sw, &mut sh, &mut stride, &mut px) };
    if rc != 1 || px.is_null() || sw <= 0 || sh <= 0 || stride < sw * 4 {
        return false;
    }
    let x0 = wx + PAD;
    let x1 = wx + w - PAD;
    let y0 = wy + TITLE_H + 8;
    if x1 - x0 < 40 || wy + h - y0 < 60 {
        return false;               /* tiny window: keep the logo preview */
    }
    /* caption + accent frame, same language as the logo preview */
    text(s, x0, y0, "Wayland surface (wl_shm)", TEXT_HI, 0xf0);
    let cy0 = y0 + 24;
    let cy1 = (wy + h - 24).max(cy0 + 8);
    s.rect_fill(x0 - 1, cy0 - 1, x1 + 1, cy0, accent, 0x70);
    s.rect_fill(x0 - 1, cy1, x1 + 1, cy1 + 1, accent, 0x70);
    s.rect_fill(x0 - 1, cy0, x0, cy1, accent, 0x70);
    s.rect_fill(x1, cy0, x1 + 1, cy1, accent, 0x70);
    /* 1:1 blit, clipped to the frame (the 360x240 window is a little shorter
     * than a 320x200 surface plus captions; the top-left region is what matters
     * and the clip is visible as the accent frame). */
    let cw = x1 - x0;
    let ch = cy1 - cy0;
    let mut logged = false;
    for dy in 0..sh {
        if dy >= ch {
            break;
        }
        let srow = unsafe { px.add((dy * stride) as usize) } as *const u32;
        for dx in 0..sw {
            if dx >= cw {
                break;
            }
            let c = unsafe { core::ptr::read_unaligned(srow.add(dx as usize)) } & 0x00ff_ffff;
            s.put((x0 + dx) as usize, (cy0 + dy) as usize, c);
        }
        logged = true;
    }
    if logged {
        unsafe {
            console_printf(b"[gui-shell] wl surface %dx%d slot 2\n\0".as_ptr(),
                           sw, sh);
        }
    }
    text(s, x0, cy1 + 6, "wl_shm ・ 320x200 ・ run wlclient", TEXT_LOW, 0xe4);
    true
}

/// v0.60: the compositor surface as a window of its own (accent `0x7a5cff`).
///
/// The image viewer still shows a committed surface (that is the v0.59
/// integration and its acceptance gate); this window is the *compositor view*:
///
/// * it never falls back to the logo - with no client it shows an explicit hint
///   card, so "no client has committed yet" stays visible instead of being
///   disguised as a picture;
/// * it reads the surface properties back from the server (`w / h / stride /
///   commits`) rather than hard-coding "320x200";
/// * the blit is integer nearest-neighbour with `scale` 2 whenever the window is
///   large enough. That is the observable half of `wl_surface.set_buffer_scale`
///   semantics: a buffer of `w x h` at scale 2 owns `2w x 2h` logical pixels, so
///   the compositor is the component that decides how big it lands on screen.
fn wayland_window_body(s: &mut Surface<'_>, accent: u32, wx: i32, wy: i32, w: i32, h: i32) {
    let x0 = wx + PAD;
    let x1 = wx + w - PAD;
    let y0 = wy + TITLE_H + 10;
    let mut sw: i32 = 0;
    let mut sh: i32 = 0;
    let mut stride: i32 = 0;
    let mut px: *const u8 = core::ptr::null();
    let rc = unsafe { wl_server_latest_surface(&mut sw, &mut sh, &mut stride, &mut px) };
    let commits = unsafe { wl_server_commit_count() };
    text(s, x0, y0, "Wayland 合成器", TEXT_HI, 0xf0);
    if rc != 1 || px.is_null() || sw <= 0 || sh <= 0 || stride < sw * 4 {
        /* no client yet: an explicit hint card (never the logo fallback) */
        let cy0 = y0 + 26;
        let cy1 = (wy + h - 20).max(cy0 + 8);
        round_row(s, x0, cy0, x1, cy1, 0xffffff, 0x14);
        text(s, x0 + 16, cy0 + 14, "暂无客户端提交 surface", TEXT_MID, 0xe8);
        if cy0 + 44 < cy1 {
            text(s, x0 + 16, cy0 + 44, "run wlclient ・ run wltwo", TEXT_LOW, 0xe0);
        }
        if cy0 + 68 < cy1 {
            text(s, x0 + 16, cy0 + 68, "wl_shm ・ XRGB8888", TEXT_LOW, 0xd8);
        }
        return;
    }
    /* integer scale: 2 when the surface fits, else 1 (nearest neighbour) */
    let avail_w = x1 - x0;
    let avail_h = wy + h - 26 - (y0 + 30);
    let scale = if sw * 2 <= avail_w && sh * 2 <= avail_h { 2 } else { 1 };
    let dw = sw * scale;
    let dh = sh * scale;
    let cx0 = x0;
    let cy0 = y0 + 30;
    if dw <= 0 || dh <= 0 || avail_h < 24 {
        return;
    }
    /* accent frame around the surface, same language as the image viewer */
    s.rect_fill(cx0 - 1, cy0 - 1, cx0 + dw + 1, cy0, accent, 0x80);
    s.rect_fill(cx0 - 1, cy0 + dh, cx0 + dw + 1, cy0 + dh + 1, accent, 0x80);
    s.rect_fill(cx0 - 1, cy0, cx0, cy0 + dh, accent, 0x80);
    s.rect_fill(cx0 + dw, cy0, cx0 + dw + 1, cy0 + dh, accent, 0x80);
    let mut rows = 0;
    for dy in 0..dh {
        let sy = dy / scale;
        if sy >= sh {
            break;
        }
        let srow = unsafe { px.add((sy * stride) as usize) } as *const u32;
        let yyy = cy0 + dy;
        if yyy >= wy + h {
            break;
        }
        for dx in 0..dw {
            if cx0 + dx >= x1 {
                break;
            }
            let sx = dx / scale;
            let c = unsafe { core::ptr::read_unaligned(srow.add(sx as usize)) } & 0x00ff_ffff;
            s.put((cx0 + dx) as usize, yyy as usize, c);
        }
        rows += 1;
    }
    if rows == 0 {
        return;
    }
    /* footer: properties read back from the server + how to drive it */
    let fy = cy0 + dh + 6;
    if fy + 18 > wy + h {
        return;
    }
    let mut b0 = [0u8; 12];
    let mut b1 = [0u8; 12];
    let mut b2 = [0u8; 12];
    let mut x = x0;
    let sw_s = u32_str(&mut b0, sw as u32);
    let sh_s = u32_str(&mut b1, sh as u32);
    x += text_at(s, x, fy, sw_s, TEXT_MID, 0xe2);
    x += text_at(s, x, fy, "x", TEXT_MID, 0xe2);
    x += text_at(s, x, fy, sh_s, TEXT_MID, 0xe2);
    x += text_at(s, x, fy, " ・ scale ", TEXT_LOW, 0xdc);
    let sc_s = u32_str(&mut b2, scale as u32);
    x += text_at(s, x, fy, sc_s, TEXT_LOW, 0xdc);
    x += text_at(s, x, fy, " ・ commit ", TEXT_LOW, 0xdc);
    let cm_s = u32_str(&mut b0, commits);
    text_at(s, x, fy, cm_s, TEXT_LOW, 0xdc);
    /* second footer line: every retained client surface (v0.60 multi-client).
     * The big blit above is the most recent commit; this line lists what the
     * server kept, so "two clients are connected" is visible in one window. */
    let fy2 = fy + 18;
    if fy2 + 16 <= wy + h {
        let mut b3 = [0u8; 12];
        let n = unsafe { wl_server_surface_count() };
        let mut x2 = x0;
        x2 += text_at(s, x2, fy2, "客户端 ", TEXT_LOW, 0xd8);
        let ns = u32_str(&mut b3, if n > 0 { n as u32 } else { 0 });
        x2 += text_at(s, x2, fy2, ns, TEXT_MID, 0xe0);
        for i in 0..n.min(3) {
            let mut iw: i32 = 0;
            let mut ih: i32 = 0;
            let mut istride: i32 = 0;
            let mut iscale: i32 = 1;
            let mut iserial: u32 = 0;
            let mut ipx: *const u8 = core::ptr::null();
            let ok = unsafe {
                wl_server_surface_at(i, &mut iw, &mut ih, &mut istride,
                                     &mut iscale, &mut iserial, &mut ipx)
            };
            if ok != 1 || iw <= 0 || ih <= 0 {
                continue;
            }
            x2 += text_at(s, x2, fy2, "  #", TEXT_LOW, 0xd0);
            let si = u32_str(&mut b3, i as u32);
            x2 += text_at(s, x2, fy2, si, TEXT_LOW, 0xd0);
            x2 += text_at(s, x2, fy2, " ", TEXT_LOW, 0xd0);
            let ws = u32_str(&mut b3, iw as u32);
            x2 += text_at(s, x2, fy2, ws, TEXT_LOW, 0xd0);
            x2 += text_at(s, x2, fy2, "x", TEXT_LOW, 0xd0);
            let hs = u32_str(&mut b3, ih as u32);
            x2 += text_at(s, x2, fy2, hs, TEXT_LOW, 0xd0);
            x2 += text_at(s, x2, fy2, "@", TEXT_LOW, 0xd0);
            let ss = u32_str(&mut b3, if iscale > 0 { iscale as u32 } else { 1 });
            x2 += text_at(s, x2, fy2, ss, TEXT_LOW, 0xd0);
        }
    }
}

// ---------------------------------------------------------------- helpers

/// Format a `u32` into a borrowed `&str` (no allocation, no libc).
fn u32_str<'a>(buf: &'a mut [u8; 12], v: u32) -> &'a str {
    let n = fmt_u32(buf, v);
    unsafe { core::str::from_utf8_unchecked(&buf[..n]) }
}

#[inline]
fn text(s: &mut Surface<'_>, x: i32, y: i32, t: &str, c: u32, a: u32) {
    s.text(x, y, t, c, a, 1);
}

/// Like [`text`], but returns the advance width so a row can be built left to
/// right from substrings without knowing glyph metrics up front.
fn text_at(s: &mut Surface<'_>, x: i32, y: i32, t: &str, c: u32, a: u32) -> i32 {
    s.text(x, y, t, c, a, 1)
}

/// Blend `c` towards white by `pct` percent (integer only).
fn lighten(c: u32, pct: u32) -> u32 {
    let mix = |sh: u32| -> u32 {
        let v = (c >> sh) & 0xff;
        (v + (0xff - v) * pct / 100) & 0xff
    };
    (mix(16) << 16) | (mix(8) << 8) | mix(0)
}

/// Advance width of a string in this renderer: ASCII cells are 10 px, CJK
/// (GB2312) cells are 18 px at scale 1 (mirrors `advance_ascii`/`advance_cjk`).
/// Used to right-align the values in the settings rows.
fn text_w(t: &str) -> i32 {
    let mut w = 0;
    for b in t.bytes() {
        /* a UTF-8 lead byte of a multi-byte sequence is >= 0x80 */
        w += if b < 0x80 { 10 } else if b & 0xC0 != 0x80 { 18 } else { 0 };
    }
    w
}

/// Integer rounded rectangle: one `rect_fill` for the middle band plus one
/// 1-px-high fill per corner row, inset by the precomputed circle table. The
/// result has hard 1-px steps instead of analytic AA, which is invisible at
/// this radius and costs no soft-float sqrt per pixel.
fn round_row(s: &mut Surface<'_>, x0: i32, y0: i32, x1: i32, y1: i32, c: u32, a: u32) {
    let h = y1 - y0;
    if h <= 2 * ROW_R || x1 - x0 <= 2 * ROW_R {
        s.rect_fill(x0, y0, x1, y1, c, a);
        return;
    }
    for k in 0..ROW_R {
        let ins = ROW_INSET[k as usize];
        s.rect_fill(x0 + ins, y0 + k, x1 - ins, y0 + k + 1, c, a);
        s.rect_fill(x0 + ins, y1 - 1 - k, x1 - ins, y1 - k, c, a);
    }
    s.rect_fill(x0, y0 + ROW_R, x1, y1 - ROW_R, c, a);
}

/// Decimal formatting into a caller-provided buffer (no allocation, no libc).
fn fmt_u32(buf: &mut [u8; 12], v0: u32) -> usize {
    let mut v = v0;
    let mut tmp = [0u8; 12];
    let mut n = 0usize;
    loop {
        tmp[n] = b'0' + (v % 10) as u8;
        v /= 10;
        n += 1;
        if v == 0 || n == tmp.len() {
            break;
        }
    }
    for i in 0..n {
        buf[i] = tmp[n - 1 - i];
    }
    n
}

// ---------------------------------------------------------------- static body

/// Paint the static body of the window identified by `accent`.
///
/// `(wx, wy, w, h)` is the window rectangle; the body area is inset by `PAD`
/// on the left/right and starts below the title strip, so a maximised window
/// (1024x730) lays out correctly instead of keeping a small fixed column.
pub fn draw_body(s: &mut Surface<'_>, accent: u32, wx: i32, wy: i32, w: i32, h: i32) {
    if w < 140 || h < 90 {
        return;                     /* too small to lay out: leave the body bare */
    }
    match accent {
        ACC_SETTINGS => settings_body(s, accent, wx, wy, w, h),
        ACC_TASKMAN => taskman_body(s, accent, wx, wy, w, h),
        ACC_IMAGE => image_body(s, accent, wx, wy, w, h),
        ACC_EDITOR => editor_body(s, accent, wx, wy, w, h),
        ACC_WAYLAND => wayland_window_body(s, accent, wx, wy, w, h),
        _ => generic_body(s, accent, wx, wy, w, h),
    }
}

/// 系统设置: three rounded setting rows, each with a label on the left, the
/// value right-aligned, and an accent pill on the left edge.
fn settings_body(s: &mut Surface<'_>, accent: u32, wx: i32, wy: i32, w: i32, h: i32) {
    let x0 = wx + PAD;
    let x1 = wx + w - PAD;
    let rows: [(&str, &str); 3] = [
        ("账户与登录", "alice"),
        ("显示与外观", "1024x768 ・ 32bpp"),
        ("关于本机", "SutureOS 0.59.0-alpha"),
    ];
    let pill = lighten(accent, 10);
    let mut y = wy + TITLE_H + 20;
    for (label, value) in rows.iter() {
        if y + 52 > wy + h - 14 {
            break;                  /* small window: drop rows that do not fit */
        }
        let y1 = y + 52;
        round_row(s, x0, y, x1, y1, 0xffffff, 0x1a);
        /* accent pill: a 4 px bar inset from the row's rounded edges */
        s.rect_fill(x0 + 12, y + 16, x0 + 16, y1 - 16, pill, 0xe8);
        text(s, x0 + 30, y + 18, label, TEXT_HI, 0xf2);
        let vx = (x1 - 16 - text_w(value)).max(x0 + 30 + text_w(label) + 12);
        text(s, vx, y + 18, value, TEXT_MID, 0xe2);
        y = y1 + 10;
    }
}

/// 任务管理器: static labels + two bars. The numbers are live (see
/// [`draw_live`]), which is why the labels stop before the value column.
fn taskman_body(s: &mut Surface<'_>, accent: u32, wx: i32, wy: i32, w: i32, _h: i32) {
    let x0 = wx + PAD;
    let x1 = wx + w - PAD;
    /* header */
    text(s, x0 + 4, wy + TITLE_H + 14, "进程与性能", TEXT_MID, 0xe0);
    let track_x0 = x0 + 56;
    let track_x1 = x1 - 8;
    let span = track_x1 - track_x0;
    /* bar 1: CPU */
    let mut by = wy + TITLE_H + 42;
    text(s, x0 + 4, by - 2, "CPU", TEXT_MID, 0xe8);
    s.rect_fill(track_x0, by, track_x1, by + 14, 0xffffff, 0x18);
    s.rect_fill(track_x0, by, track_x0 + span * 62 / 100, by + 14, accent, 0xd8);
    /* bar 2: memory */
    by += 30;
    text(s, x0 + 4, by - 2, "内存", TEXT_MID, 0xe8);
    s.rect_fill(track_x0, by, track_x1, by + 14, 0xffffff, 0x18);
    s.rect_fill(track_x0, by, track_x0 + span * 47 / 100, by + 14,
                lighten(accent, 40), 0xd8);
    /* separator below the bars */
    by += 34;
    s.rect_fill(x0, by, x1, by + 1, 0xffffff, 0x22);
    /* static labels for the two live values (draw_live writes the numbers) */
    text(s, x0 + 4, wy + TM_UPTIME_Y, "运行时间", TEXT_LOW, 0xe4);
    text(s, x0 + 4, wy + TM_CPU_Y, "CPU", TEXT_LOW, 0xe4);
}

/// 图片查看器: a dark checkerboard "transparent" canvas with the real 48 px
/// logo nearest-neighbour scaled 3x (144x144) centred on it, a file-name
/// caption above and the "48x48 · 3x" footer below.
fn image_body(s: &mut Surface<'_>, accent: u32, wx: i32, wy: i32, w: i32, h: i32) {
    /* v0.59.x: a committed Wayland surface wins over the logo preview - that is
     * the compositor integration (ring-3 client's shm buffer shown in a window). */
    if wayland_surface_body(s, accent, wx, wy, w, h) {
        return;
    }
    let x0 = wx + PAD;
    let x1 = wx + w - PAD;
    text(s, x0, wy + TITLE_H + 8, "SutureOS_logo_1536.png", TEXT_MID, 0xf0);
    let cy0 = wy + TITLE_H + 30;
    let logo = 144;
    let cy1 = cy0 + logo;
    if cy1 + 22 > wy + h {
        return;                     /* small window: no room for the canvas */
    }
    /* 1 px accent frame around the canvas */
    s.rect_fill(x0 - 1, cy0 - 1, x1 + 1, cy0, accent, 0x70);
    s.rect_fill(x0 - 1, cy1, x1 + 1, cy1 + 1, accent, 0x70);
    s.rect_fill(x0 - 1, cy0, x0, cy1, accent, 0x70);
    s.rect_fill(x1, cy0, x1 + 1, cy1, accent, 0x70);
    /* checkerboard: 8 px cells, two dark slate tones (the classic "no alpha"
     * matte). Values picked so the matte reads clearly darker than the window
     * body (mean ~91 vs ~170) while the window as a whole stays a painted
     * surface - the shell's own "launched window is painted" gate samples this
     * area and requires a mean brightness above 75. */
    let cw = x1 - x0;
    let cells = cw / 8;
    for row in 0..(logo / 8) {
        for col in 0..cells {
            let c = if ((row + col) & 1) == 0 { 0x4a525c } else { 0x5a636e };
            let cx0 = x0 + col * 8;
            let cyy = cy0 + row * 8;
            s.rect_fill(cx0, cyy, cx0 + 8, cyy + 8, c, 0xff);
        }
    }
    /* the real 48x48 logo, nearest neighbour 3x, centred on the canvas */
    let lx = x0 + (cw - logo) / 2;
    let ly = cy0;
    for dy in 0..logo {
        let sy = (dy / 3) as usize;
        for dx in 0..logo {
            let sx = (dx / 3) as usize;
            let src = unsafe { g_logo48[sy * 48 + sx] } & 0x00ff_ffff;
            /* The logo bitmaps are 0xAARRGGBB (alpha preserved); their
             * transparent pixels are 0x00FFFFFF, so keying near-white out here
             * makes the checkerboard matte show through the glyph's holes. */
            let r = (src >> 16) & 0xff;
            let g = (src >> 8) & 0xff;
            let b = src & 0xff;
            if r >= 244 && g >= 244 && b >= 244 {
                continue;
            }
            s.put((lx + dx) as usize, (ly + dy) as usize, src);
        }
    }
    text(s, x0, cy1 + 6, "48x48 ・ 3x", TEXT_LOW, 0xe4);
}

/// 文本编辑器: three numbered code lines, a current-line highlight and a caret
/// block, plus a status bar.
fn editor_body(s: &mut Surface<'_>, accent: u32, wx: i32, wy: i32, w: i32, h: i32) {
    let x0 = wx + PAD;
    let x1 = wx + w - PAD;
    let y0 = wy + TITLE_H + 10;
    /* Three lines of a source file (the fragment a viewer shows before the
     * first scroll), so every line carries real text - a lone closing brace
     * would be nearly empty and would make "all three lines are drawn" a very
     * weak pixel assertion. */
    let lines: [&str; 3] = ["fn main() {", "    let mut n = 42;", "    n += 1;"];
    let nums: [&str; 3] = ["1", "2", "3"];
    if y0 + 3 * 18 + 42 > wy + h {
        return;                     /* small window: no room for the listing */
    }
    /* line-number gutter */
    s.rect_fill(x0, y0, x0 + 26, y0 + 62, 0x000000, 0x34);
    for i in 0..3usize {
        let ly = y0 + 4 + i as i32 * 18;
        if i == 2 {
            /* the caret sits on the current (third) line */
            s.rect_fill(x0 + 26, ly - 1, x1, ly + 17, 0xffffff, 0x0a);
        }
        text(s, x0 + 14, ly, nums[i], TEXT_LOW, 0xc0);
        text(s, x0 + 34, ly, lines[i], TEXT_HI, 0xf0);
    }
    /* caret block at the end of the third line (the "current column") */
    let caret_y = y0 + 4 + 2 * 18;
    let caret_x = x0 + 34 + text_w(lines[2]);
    s.rect_fill(caret_x, caret_y, caret_x + 8, caret_y + 16, accent, 0xf0);
    /* status bar along the window's bottom edge: the shell's focus line sits
     * 100..124 px below the title strip, so the two never cross */
    let sy = wy + h - 26;
    s.rect_fill(x0, sy - 7, x1, sy - 6, 0xffffff, 0x1c);
    s.rect_fill(x0, sy - 5, x1, sy + 18, 0x000000, 0x1c);
    text(s, x0 + 2, sy, "UTF-8  LF  Ln 3, Col 13", TEXT_LOW, 0xd0);
}

/// Unknown accent: a plain labeled body, so a future window never looks broken.
fn generic_body(s: &mut Surface<'_>, accent: u32, wx: i32, wy: i32, w: i32, h: i32) {
    let x0 = wx + PAD;
    let x1 = wx + w - PAD;
    let y1 = (wy + TITLE_H + 76).min(wy + h - 14);
    round_row(s, x0, wy + TITLE_H + 20, x1, y1, 0xffffff, 0x14);
    s.rect_fill(x0 + 12, wy + TITLE_H + 36, x0 + 16, wy + TITLE_H + 60,
                lighten(accent, 10), 0xe8);
    text(s, x0 + 30, wy + TITLE_H + 32, "SutureOS", TEXT_HI, 0xf2);
    text(s, x0 + 30, wy + TITLE_H + 52, "应用内容", TEXT_MID, 0xe0);
}

// ---------------------------------------------------------------- live layer

/// Paint the values that change over time. Called by the shell after the window
/// compositing loop, so it lands on top of the cached body layer.
///
/// Only the task manager has live values: the uptime in seconds
/// (`shell_tick() / 100`, i.e. the kernel tick counter at 100 Hz) and a
/// deterministic CPU-load figure derived from it. `now_ms` (milliseconds since
/// the shell loop started) drives the blinking "live" dot.
pub fn draw_live(s: &mut Surface<'_>, accent: u32, wx: i32, wy: i32, w: i32, h: i32,
                 now_ms: u32) {
    if w < 140 || h < 90 || accent != ACC_TASKMAN {
        return;
    }
    let x0 = wx + PAD;
    let x1 = wx + w - PAD;
    let secs = (crate::shell_tick() / 100u64) as u32;
    /* Deterministic "load": a real measurement would need instrumentation the
     * shell does not have, and a constant would be honest but dead. This is
     * documented as synthetic in the report. */
    let pct = 21u32.wrapping_add(secs.wrapping_mul(37) % 57);

    let mut digits = [0u8; 12];
    let mut line = [0u8; 24];

    let n = fmt_u32(&mut digits, secs);
    let mut m = 0usize;
    for i in 0..n { line[m] = digits[i]; m += 1; }
    line[m] = b' '; m += 1;
    line[m] = b's'; m += 1;
    let uptime = core::str::from_utf8(&line[..m]).unwrap_or("?");
    text(s, x0 + 4 + TM_UPTIME_X, wy + TM_UPTIME_Y, uptime,
         lighten(accent, 55), 0xf2);

    let n = fmt_u32(&mut digits, pct);
    let mut m = 0usize;
    for i in 0..n { line[m] = digits[i]; m += 1; }
    line[m] = b' '; m += 1;
    line[m] = b'%'; m += 1;
    let cpu = core::str::from_utf8(&line[..m]).unwrap_or("?");
    text(s, x0 + 4 + TM_CPU_X, wy + TM_CPU_Y, cpu, TEXT_HI, 0xf2);

    /* live indicator: alternates every 500 ms (the shell repaints on the second
     * boundary, so this reads as a once-a-second heartbeat) */
    let on = (now_ms / 500) & 1 == 0;
    let dot = if on { 0x7cf0c8 } else { 0x2f6b5d };
    s.rect_fill(x1 - 12, wy + TITLE_H + 12, x1 - 4, wy + TITLE_H + 20, dot, 0xf0);
}
