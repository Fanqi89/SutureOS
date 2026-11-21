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
    /* kernel/vfs/vfs.c: whole-file read, heap buffer the caller frees. */
    fn vfs_read_file(path: *const u8, len: *mut usize) -> *mut u8;
    fn lemis_heap_free(p: *mut u8);
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
    x = text_at(s, x, fy, sw_s, TEXT_MID, 0xe2);
    x = text_at(s, x, fy, "x", TEXT_MID, 0xe2);
    x = text_at(s, x, fy, sh_s, TEXT_MID, 0xe2);
    x = text_at(s, x, fy, " ・ scale ", TEXT_LOW, 0xdc);
    let sc_s = u32_str(&mut b2, scale as u32);
    x = text_at(s, x, fy, sc_s, TEXT_LOW, 0xdc);
    x = text_at(s, x, fy, " ・ commit ", TEXT_LOW, 0xdc);
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
        x2 = text_at(s, x2, fy2, "客户端 ", TEXT_LOW, 0xd8);
        let ns = u32_str(&mut b3, if n > 0 { n as u32 } else { 0 });
        x2 = text_at(s, x2, fy2, ns, TEXT_MID, 0xe0);
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
            x2 = text_at(s, x2, fy2, "  #", TEXT_LOW, 0xd0);
            let si = u32_str(&mut b3, i as u32);
            x2 = text_at(s, x2, fy2, si, TEXT_LOW, 0xd0);
            if x2 > x1 - 90 {
                break;
            }
            x2 = text_at(s, x2, fy2, " ", TEXT_LOW, 0xd0);
            let ws = u32_str(&mut b3, iw as u32);
            x2 = text_at(s, x2, fy2, ws, TEXT_LOW, 0xd0);
            x2 = text_at(s, x2, fy2, "x", TEXT_LOW, 0xd0);
            let hs = u32_str(&mut b3, ih as u32);
            x2 = text_at(s, x2, fy2, hs, TEXT_LOW, 0xd0);
            /* the scale is already on the first footer line; keeping this row
             * to "#i WxH" lets three clients fit inside the window */
        }
    }
}

// ---------------------------------------------------------------- editor buffer

/* v0.61: the editor window owns a REAL multi-line buffer instead of a baked-in
 * listing. The buffer lives here (window content belongs to this module); the
 * shell's key router calls `editor_key`/`editor_move`, and F5 asks for
 * `editor_snapshot()` which the shell writes to /mnt/fat/GUIEDIT.TXT through the
 * kernel VFS. Colours/geometry are unchanged integer drawing. */

pub const ED_LINES: usize = 10;
pub const ED_COLS: usize = 38;

static mut ED_BUF: [[u8; ED_COLS]; ED_LINES] = [[0u8; ED_COLS]; ED_LINES];
static mut ED_LEN: [usize; ED_LINES] = [0usize; ED_LINES];
static mut ED_ROWS: usize = 0;
static mut ED_LINE: usize = 0;
static mut ED_COL: usize = 0;
static mut ED_EPOCH: i32 = 0;
static mut ED_READY: bool = false;

unsafe fn ed_seed(row: usize, text: &[u8]) {
    let n = text.len().min(ED_COLS);
    let mut k = 0;
    while k < n {
        ED_BUF[row][k] = text[k];
        k += 1;
    }
    ED_LEN[row] = n;
}

/// Lazily seed the buffer with the listing v0.59 shipped, so the window looks the
/// same before the first keystroke (and every earlier pixel assertion still has
/// real text to find).
unsafe fn ed_init() {
    if ED_READY {
        return;
    }
    ED_READY = true;
    ED_ROWS = 4;
    ed_seed(0, b"fn main() {");
    ed_seed(1, b"    let mut n = 42;");
    ed_seed(2, b"    n += 1;");
    ed_seed(3, b"}");
    ED_LINE = 1;
    ED_COL = ED_LEN[1];
    ED_EPOCH = 1;
}

/// Monotonic counter the shell mixes into its damage decision (a keystroke has to
/// repaint the editor window even though no rect/geometry moved).
pub fn editor_epoch() -> i32 {
    unsafe { ED_EPOCH }
}

pub fn editor_caret() -> (i32, i32, i32) {
    unsafe {
        ed_init();
        (ED_LINE as i32, ED_COL as i32, ED_ROWS as i32)
    }
}

unsafe fn ed_shift_right(row: usize, from: usize, n: usize) {
    let len = ED_LEN[row];
    if n == 0 || from > len || len == ED_COLS {
        return;                     /* from == len is a plain append at the end */
    }
    let n = n.min(ED_COLS - len);
    let mut k = len;
    while k > from {
        ED_BUF[row][k + n - 1] = ED_BUF[row][k - 1];
        k -= 1;
    }
    ED_LEN[row] = len + n;
}

unsafe fn ed_delete_at(row: usize, at: usize) {
    let len = ED_LEN[row];
    if at >= len {
        return;
    }
    let mut k = at;
    while k + 1 < len {
        ED_BUF[row][k] = ED_BUF[row][k + 1];
        k += 1;
    }
    ED_LEN[row] = len - 1;
    ED_BUF[row][len - 1] = 0;
}

/// One keystroke from the shell: printable ASCII, Backspace (0x08) or Enter.
pub fn editor_key(ch: u8) {
    unsafe {
        ed_init();
        if ch == 0x08 {
            if ED_COL > 0 {
                ed_delete_at(ED_LINE, ED_COL - 1);
                ED_COL -= 1;
            } else if ED_LINE > 0 {
                /* join with the previous line (classic editor behaviour) */
                let prev = ED_LINE - 1;
                let plen = ED_LEN[prev];
                let cur = ED_LINE;
                let clen = ED_LEN[cur];
                let mut k = 0;
                while k < clen && plen + k < ED_COLS {
                    ED_BUF[prev][plen + k] = ED_BUF[cur][k];
                    k += 1;
                }
                ED_LEN[prev] = (plen + clen).min(ED_COLS);
                /* close the gap: shift the remaining rows up one */
                let mut r = cur;
                while r + 1 < ED_ROWS {
                    ED_LEN[r] = ED_LEN[r + 1];
                    ed_seed(r, core::slice::from_raw_parts(ED_BUF[r + 1].as_ptr(),
                                                           ED_LEN[r + 1]));
                    r += 1;
                }
                if ED_ROWS > 1 {
                    ED_ROWS -= 1;
                    ED_LEN[ED_ROWS] = 0;
                }
                ED_LINE = prev;
                ED_COL = plen;
            }
        } else if ch == 0x0a || ch == 0x0d {
            if ED_ROWS < ED_LINES {
                /* split: text right of the caret moves to the new row */
                let cur = ED_LINE;
                let len = ED_LEN[cur];
                let head = ED_COL.min(len);
                let tail = len - head;
                let mut r = ED_ROWS;
                while r > cur + 1 {
                    ED_LEN[r] = ED_LEN[r - 1];
                    ed_seed(r, core::slice::from_raw_parts(ED_BUF[r - 1].as_ptr(),
                                                           ED_LEN[r - 1]));
                    r -= 1;
                }
                ED_ROWS += 1;
                let mut k = 0;
                while k < tail && k < ED_COLS {
                    ED_BUF[cur + 1][k] = ED_BUF[cur][head + k];
                    k += 1;
                }
                ED_LEN[cur + 1] = tail.min(ED_COLS);
                ED_LEN[cur] = head;
                ED_LINE = cur + 1;
                ED_COL = 0;
            }
        } else if ch >= 0x20 && ch < 0x7f {
            if ED_LEN[ED_LINE] < ED_COLS {
                ed_shift_right(ED_LINE, ED_COL, 1);
                ED_BUF[ED_LINE][ED_COL] = ch;
                ED_COL += 1;
            }
        }
        ED_EPOCH = ED_EPOCH.wrapping_add(1);
    }
}

/// Caret movement: 1 left, 2 right, 3 up, 4 down, 5 home, 6 end.
pub fn editor_move(kind: i32) {
    unsafe {
        ed_init();
        match kind {
            1 => {
                if ED_COL > 0 {
                    ED_COL -= 1;
                } else if ED_LINE > 0 {
                    ED_LINE -= 1;
                    ED_COL = ED_LEN[ED_LINE];
                }
            }
            2 => {
                if ED_COL < ED_LEN[ED_LINE] {
                    ED_COL += 1;
                } else if ED_LINE + 1 < ED_ROWS {
                    ED_LINE += 1;
                    ED_COL = 0;
                }
            }
            3 => {
                if ED_LINE > 0 {
                    ED_LINE -= 1;
                    ED_COL = ED_COL.min(ED_LEN[ED_LINE]);
                }
            }
            4 => {
                if ED_LINE + 1 < ED_ROWS {
                    ED_LINE += 1;
                    ED_COL = ED_COL.min(ED_LEN[ED_LINE]);
                }
            }
            5 => ED_COL = 0,
            _ => ED_COL = ED_LEN[ED_LINE],
        }
    }
}

/// Copy the buffer into `out` as lines joined by '\n' plus a trailing newline.
/// Returns the byte count (0 when `out` is too small - the caller reports that
/// honestly instead of saving a truncated file).
pub fn editor_snapshot(out: &mut [u8]) -> usize {
    unsafe {
        ed_init();
        let mut n = 0usize;
        for row in 0..ED_ROWS {
            for k in 0..ED_LEN[row] {
                if n + 1 >= out.len() {
                    return 0;
                }
                out[n] = ED_BUF[row][k];
                n += 1;
            }
            if n + 1 >= out.len() {
                return 0;
            }
            out[n] = b'\n';
            n += 1;
        }
        n
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
        ACC_CALC => calc_body(s, accent, wx, wy, w, h),
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
/* ------------------------------------------------------------- image files
 * v0.62: the viewer shows real files from the FAT32 volume, and it can show two
 * formats: BMP (24/32bpp BIFF) and QOI ("Quite OK Image"). Left/Right in the
 * window switch between the supported files, so the acceptance can prove both
 * decoders on the same window.
 *
 * Everything is static (the crate has no allocator): one 256x256 pixel buffer is
 * plenty for the fixtures and for the small bitmaps such a volume carries. */
const IMG_MAX_W: usize = 256;
const IMG_MAX_H: usize = 256;
static mut IMG_PX: [u32; IMG_MAX_W * IMG_MAX_H] = [0; IMG_MAX_W * IMG_MAX_H];
static mut IMG_W: i32 = 0;
static mut IMG_H: i32 = 0;
static mut IMG_IDX: usize = usize::MAX;         /* usize::MAX = nothing loaded */
static mut IMG_NAME: [u8; 16] = [0; 16];
static mut IMG_NAME_LEN: usize = 0;

const IMG_COUNT: usize = 2;
const IMG_PATHS: [&[u8]; IMG_COUNT] = [b"/mnt/fat/IMG.BMP\0", b"/mnt/fat/IMG.QOI\0"];
const IMG_NAMES: [&str; IMG_COUNT] = ["IMG.BMP", "IMG.QOI"];
/* NUL-terminated copies for console_printf("%s"): a `&str` has no terminator
 * and the kernel would print whatever follows it in memory. */
const IMG_NAMES_C: [&[u8]; IMG_COUNT] = [b"IMG.BMP\0", b"IMG.QOI\0"];

#[inline]
fn rd_u16(b: &[u8], o: usize) -> u32 {
    (b[o] as u32) | ((b[o + 1] as u32) << 8)
}

#[inline]
fn rd_u32(b: &[u8], o: usize) -> u32 {
    (b[o] as u32)
        | ((b[o + 1] as u32) << 8)
        | ((b[o + 2] as u32) << 16)
        | ((b[o + 3] as u32) << 24)
}

#[inline]
fn rd_be32(b: &[u8], o: usize) -> u32 {
    ((b[o] as u32) << 24)
        | ((b[o + 1] as u32) << 16)
        | ((b[o + 2] as u32) << 8)
        | (b[o + 3] as u32)
}

#[inline]
fn pack_rgb(r: u8, g: u8, b: u8) -> u32 {
    ((r as u32) << 16) | ((g as u32) << 8) | (b as u32)
}

/// Decode a BMP into IMG_PX. Only uncompressed 24/32bpp BIFF: that is what the
/// volume carries and what a 32bpp framebuffer can blit without conversion.
unsafe fn bmp_decode(b: &[u8]) -> bool {
    if b.len() < 54 || b[0] != b'B' || b[1] != b'M' {
        return false;
    }
    let data_off = rd_u32(b, 10) as usize;
    let w = rd_u32(b, 18) as i32;
    let h_signed = rd_u32(b, 22) as i32;
    let bpp = rd_u16(b, 28) as i32;
    if rd_u32(b, 30) != 0 || w <= 0 || h_signed == 0 {
        return false;
    }
    let h = if h_signed < 0 { -h_signed } else { h_signed };
    let bytes_pp = bpp as usize / 8;
    if (bpp != 24 && bpp != 32)
        || w as usize > IMG_MAX_W
        || h as usize > IMG_MAX_H
    {
        return false;
    }
    let stride = (w as usize * bytes_pp + 3) & !3usize;
    if data_off + stride * h as usize > b.len() {
        return false;
    }
    let bottom_up = h_signed > 0;
    let dst = &mut *core::ptr::addr_of_mut!(IMG_PX);
    let mut row = 0usize;
    while row < h as usize {
        let src_row = if bottom_up { h as usize - 1 - row } else { row };
        let base = data_off + src_row * stride;
        let mut x = 0usize;
        while x < w as usize {
            let o = base + x * bytes_pp;
            dst[row * IMG_MAX_W + x] = pack_rgb(b[o + 2], b[o + 1], b[o]);
            x += 1;
        }
        row += 1;
    }
    IMG_W = w;
    IMG_H = h;
    true
}

/// Decode a QOI file into IMG_PX (the full opcode set, RGB/RGBA, 3 or 4 channels).
unsafe fn qoi_decode(b: &[u8]) -> bool {
    if b.len() < 22 || &b[0..4] != b"qoif" {
        return false;
    }
    let w = rd_be32(b, 4) as i32;
    let h = rd_be32(b, 8) as i32;
    let channels = b[12];
    if (channels != 3 && channels != 4) || w <= 0 || h <= 0 {
        return false;
    }
    if w as usize > IMG_MAX_W || h as usize > IMG_MAX_H {
        return false;
    }
    let wu = w as usize;
    let total = wu * h as usize;
    let dst = &mut *core::ptr::addr_of_mut!(IMG_PX);
    let mut index = [[0u8, 0u8, 0u8, 255u8]; 64];
    let mut px = [0u8, 0u8, 0u8, 255u8];
    let mut p = 14usize;
    let mut i = 0usize;
    while i < total {
        if p >= b.len() {
            return false;
        }
        let b1 = b[p];
        p += 1;
        let mut run = 1usize;
        if b1 == 0xfe {
            if p + 3 > b.len() {
                return false;
            }
            px[0] = b[p];
            px[1] = b[p + 1];
            px[2] = b[p + 2];
            p += 3;
        } else if b1 == 0xff {
            if p + 4 > b.len() {
                return false;
            }
            px[0] = b[p];
            px[1] = b[p + 1];
            px[2] = b[p + 2];
            px[3] = b[p + 3];
            p += 4;
        } else {
            match b1 >> 6 {
                0 => {
                    px = index[(b1 & 0x3f) as usize];
                }
                1 => {
                    let d = b1 & 0x3f;
                    px[0] = px[0].wrapping_add(((d >> 4) & 3).wrapping_sub(2));
                    px[1] = px[1].wrapping_add(((d >> 2) & 3).wrapping_sub(2));
                    px[2] = px[2].wrapping_add((d & 3).wrapping_sub(2));
                }
                2 => {
                    if p >= b.len() {
                        return false;
                    }
                    let b2 = b[p];
                    p += 1;
                    let dg = (b1 & 0x3f).wrapping_sub(32);
                    let dr = ((b2 >> 4) & 0x0f).wrapping_sub(8);
                    let db = (b2 & 0x0f).wrapping_sub(8);
                    px[0] = px[0].wrapping_add(dg).wrapping_add(dr);
                    px[1] = px[1].wrapping_add(dg);
                    px[2] = px[2].wrapping_add(dg).wrapping_add(db);
                }
                _ => {
                    run = (b1 & 0x3f) as usize + 1;
                }
            }
            let hi = ((px[0] as usize) * 3
                + (px[1] as usize) * 5
                + (px[2] as usize) * 7
                + (px[3] as usize) * 11)
                % 64;
            index[hi] = px;
        }
        let mut k = 0usize;
        while k < run && i < total {
            dst[(i / wu) * IMG_MAX_W + (i % wu)] = pack_rgb(px[0], px[1], px[2]);
            i += 1;
            k += 1;
        }
    }
    IMG_W = w;
    IMG_H = h;
    true
}

/// Load one supported file by index. On success the caption name is recorded, so
/// the window can never claim a file it did not decode.
unsafe fn img_load_idx(idx: usize) -> bool {
    let mut len: usize = 0;
    let p = vfs_read_file(IMG_PATHS[idx].as_ptr(), &mut len);
    if p.is_null() {
        console_printf(b"[gui-shell] image: %s not found\n\0".as_ptr(),
                       IMG_NAMES_C[idx].as_ptr());
        return false;
    }
    let ok = {
        let b = core::slice::from_raw_parts(p, len);
        let decoded = if idx == 0 { bmp_decode(b) } else { qoi_decode(b) };
        if decoded {
            console_printf(
                b"[gui-shell] image: loaded %s %dx%d (%d bytes)\n\0".as_ptr(),
                IMG_NAMES_C[idx].as_ptr(), IMG_W, IMG_H, len as i32);
        }
        decoded
    };
    lemis_heap_free(p);
    if ok {
        IMG_IDX = idx;
        let name = IMG_NAMES[idx].as_bytes();
        let mut k = 0usize;
        while k < name.len() && k < 15 {
            IMG_NAME[k] = name[k];
            k += 1;
        }
        IMG_NAME_LEN = k;
    }
    ok
}

/// Switch to the next (dir > 0) or previous (dir < 0) supported file. Returns true
/// when a different file was decoded - the shell then invalidates the window layer.
pub fn image_shift(dir: i32) -> bool {
    unsafe {
        let start = if IMG_IDX == usize::MAX { 0 } else { IMG_IDX };
        let mut step = 1usize;
        while step <= IMG_COUNT {
            let idx = if dir >= 0 {
                (start + step) % IMG_COUNT
            } else {
                (start + IMG_COUNT - step) % IMG_COUNT
            };
            if idx != IMG_IDX && img_load_idx(idx) {
                return true;
            }
            step += 1;
        }
        false
    }
}

/// True when the window is currently showing a decoded file.
pub fn image_ready() -> bool {
    unsafe { IMG_IDX != usize::MAX }
}

fn img_name() -> &'static str {
    unsafe { core::str::from_utf8_unchecked(&IMG_NAME[..IMG_NAME_LEN]) }
}

fn image_body(s: &mut Surface<'_>, accent: u32, wx: i32, wy: i32, w: i32, h: i32) {
    /* a committed Wayland surface wins over any file preview - that is the
     * compositor integration (ring-3 client's shm buffer shown in a window). */
    if wayland_surface_body(s, accent, wx, wy, w, h) {
        return;
    }
    unsafe {
        if IMG_IDX == usize::MAX {
            /* first paint: load the first supported file once */
            let _ = img_load_idx(0) || img_load_idx(1);
        }
    }
    let x0 = wx + PAD;
    let x1 = wx + w - PAD;
    let have_img = image_ready();
    if have_img {
        let (iw, ih) = unsafe { (IMG_W, IMG_H) };
        let name = img_name();
        text(s, x0, wy + TITLE_H + 8, name, TEXT_MID, 0xf0);
        let cy0 = wy + TITLE_H + 30;
        let avail_w = x1 - x0;
        let avail_h = (wy + h - 26) - cy0;
        if avail_w < 8 || avail_h < 8 {
            return;
        }
        let mut scale = (avail_w / iw).min(avail_h / ih);
        if scale < 1 {
            scale = 1;
        }
        let dw = iw * scale;
        let dh = ih * scale;
        let ox = x0 + (avail_w - dw) / 2;
        let oy = cy0 + (avail_h - dh) / 2;
        /* matte behind the picture so a small image does not float on glass */
        s.rect_fill(x0, cy0, x1, cy0 + avail_h, 0x2a3038, 0xff);
        s.rect_fill(ox - 1, oy - 1, ox + dw + 1, oy, accent, 0x80);
        s.rect_fill(ox - 1, oy + dh, ox + dw + 1, oy + dh + 1, accent, 0x80);
        s.rect_fill(ox - 1, oy, ox, oy + dh, accent, 0x80);
        s.rect_fill(ox + dw, oy, ox + dw + 1, oy + dh, accent, 0x80);
        let px = unsafe { &*core::ptr::addr_of!(IMG_PX) };
        let mut dy = 0i32;
        while dy < dh {
            let sy = (dy / scale) as usize;
            let mut dx = 0i32;
            while dx < dw {
                let sx = (dx / scale) as usize;
                let c = px[sy * IMG_MAX_W + sx];
                s.put((ox + dx) as usize, (oy + dy) as usize, c);
                dx += 1;
            }
            dy += 1;
        }
        /* caption: real name, decoded size, zoom factor, and the hint for browsing */
        let cy_cap = cy0 + avail_h + 6;
        let alpha = 0xe4u32;
        let mut b1 = [0u8; 12];
        let mut b2 = [0u8; 12];
        let mut b3 = [0u8; 12];
        let s_w = u32_str(&mut b1, iw as u32);
        let s_h = u32_str(&mut b2, ih as u32);
        let s_sc = u32_str(&mut b3, scale as u32);
        let mut cx = text_at(s, x0, cy_cap, name, TEXT_LOW, alpha);
        cx = text_at(s, cx, cy_cap, "  \u{00b7}  ", TEXT_LOW, alpha);
        cx = text_at(s, cx, cy_cap, s_w, TEXT_LOW, alpha);
        cx = text_at(s, cx, cy_cap, "x", TEXT_LOW, alpha);
        cx = text_at(s, cx, cy_cap, s_h, TEXT_LOW, alpha);
        cx = text_at(s, cx, cy_cap, "  \u{00b7}  scale ", TEXT_LOW, alpha);
        cx = text_at(s, cx, cy_cap, s_sc, TEXT_LOW, alpha);
        let _ = text_at(s, cx, cy_cap, "  \u{00b7}  \u{2190}\u{2192}", TEXT_LOW, 0x88);
        let _ = w;
        return;
    }
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
    let lx = x0 + (cw - logo) / 2;
    for dy in 0..logo {
        let sy = (dy / 3) as usize;
        for dx in 0..logo {
            let sx = (dx / 3) as usize;
            let src = unsafe { g_logo48[sy * 48 + sx] } & 0x00ff_ffff;
            let r = (src >> 16) & 0xff;
            let g = (src >> 8) & 0xff;
            let b = src & 0xff;
            if r >= 244 && g >= 244 && b >= 244 {
                continue;
            }
            s.put((lx + dx) as usize, (cy0 + dy) as usize, src);
        }
    }
    text(s, x0, cy1 + 6, "no IMG.BMP / IMG.QOI on the volume", TEXT_LOW, 0xe4);
}

/// 文本编辑器: , a current-line highlight and a caret
/// block, plus a status bar.
/// 计算器（v0.63）：整数四则运算 + 键盘输入 + 4x4 键盘布局。
pub const ACC_CALC: u32 = 0xff6a4d;

/* state: accumulator, pending operator, the number being typed, error flag */
static mut CAL_ACC: i64 = 0;
static mut CAL_OP: u8 = 0;            /* 0, b'+', b'-', b'*', b'/' */
static mut CAL_ENTRY: i64 = 0;
static mut CAL_LEN: u32 = 0;          /* digits typed since the last operator */
static mut CAL_ERR: bool = false;
static mut CAL_TXT: [u8; 18] = [0; 18];
static mut CAL_TXT_LEN: usize = 1;

/// Format a signed integer into `out`, returns the byte length.
fn cal_fmt(out: &mut [u8; 18], v: i64) -> usize {
    let mut n = 18usize;
    let neg = v < 0;
    let mut u = if neg { (v as i128).unsigned_abs() as u128 } else { v as u128 };
    if u == 0 {
        out[17] = b'0';
        if neg {
            out[16] = b'-';
            return 2;
        }
        return 1;
    }
    while u > 0 && n > 0 {
        n -= 1;
        out[n] = b'0' + (u % 10) as u8;
        u /= 10;
    }
    if neg && n > 0 {
        n -= 1;
        out[n] = b'-';
    }
    let len = 18 - n;
    /* move to the front so callers can read out[..len] */
    let mut k = 0usize;
    while k < len {
        out[k] = out[n + k];
        k += 1;
    }
    len
}

unsafe fn cal_refresh() {
    let txt = &mut *core::ptr::addr_of_mut!(CAL_TXT);
    let v = if CAL_LEN > 0 { CAL_ENTRY } else { CAL_ACC };
    let mut buf = [0u8; 18];
    let n = cal_fmt(&mut buf, v);
    let mut k = 0usize;
    while k < n {
        txt[k] = buf[k];
        k += 1;
    }
    CAL_TXT_LEN = n;
}

unsafe fn cal_apply() -> bool {
    if CAL_OP == 0 {
        CAL_ACC = CAL_ENTRY;
        return true;
    }
    match CAL_OP {
        b'+' => CAL_ACC = CAL_ACC.saturating_add(CAL_ENTRY),
        b'-' => CAL_ACC = CAL_ACC.saturating_sub(CAL_ENTRY),
        b'*' => CAL_ACC = CAL_ACC.saturating_mul(CAL_ENTRY),
        b'/' => {
            if CAL_ENTRY == 0 {
                CAL_ERR = true;
                CAL_ACC = 0;
                return false;
            }
            CAL_ACC /= CAL_ENTRY;
        }
        _ => {}
    }
    true
}

/// Handle one key. `key` is the raw scancode, `ch` the decoded ASCII (0 when the
/// scancode has no character). Returns true when the key was consumed.
pub fn calc_key(key: i32, ch: i32) -> bool {
    let _ = key;
    unsafe {
        let c = ch as u32;
        if c >= b'0' as u32 && c <= b'9' as u32 {
            if CAL_ERR {
                CAL_ACC = 0;
                CAL_OP = 0;
                CAL_ERR = false;
                CAL_LEN = 0;
            }
            let d = (c - b'0' as u32) as i64;
            if CAL_LEN == 0 {
                CAL_ENTRY = d;
            } else if CAL_ENTRY < 100_000_000 {
                CAL_ENTRY = CAL_ENTRY * 10 + d;
            } else {
                return true;            /* ignore: keep the display readable */
            }
            CAL_LEN += 1;
            cal_refresh();
            return true;
        }
        match c {
            /* operators */
            43 | 45 | 42 | 47 => {
                if CAL_ERR {
                    return true;
                }
                if CAL_LEN > 0 {
                    cal_apply();
                    CAL_LEN = 0;
                } else if CAL_OP == 0 {
                    CAL_ACC = CAL_ENTRY;
                }
                CAL_OP = c as u8;
                cal_refresh();
                true
            }
            /* '=' finishes the pending operation */
            61 | 10 | 13 => {
                if CAL_ERR {
                    return true;
                }
                if CAL_OP != 0 {
                    let ok = cal_apply();
                    CAL_OP = 0;
                    CAL_LEN = 0;
                    CAL_ENTRY = 0;
                    cal_refresh();
                    if ok {
                        /* the kernel's %d is unsigned: print the sign separately */
                        if CAL_ACC < 0 {
                            console_printf(b"[gui-shell] calc result -%d\n\0".as_ptr(),
                                           (0i64 - CAL_ACC) as i64);
                        } else {
                            console_printf(b"[gui-shell] calc result %d\n\0".as_ptr(),
                                           CAL_ACC);
                        }
                    } else {
                        console_printf(b"[gui-shell] calc error (division by zero)\n\0"
                                       .as_ptr());
                    }
                }
                true
            }
            /* clear */
            99 | 67 => {
                CAL_ACC = 0;
                CAL_ENTRY = 0;
                CAL_OP = 0;
                CAL_LEN = 0;
                CAL_ERR = false;
                cal_refresh();
                console_printf(b"[gui-shell] calc clear\n\0".as_ptr());
                true
            }
            _ => {
                /* Backspace: drop the last typed digit (or clear the error) */
                if c == 0x08 {
                    if CAL_LEN > 0 {
                        CAL_ENTRY /= 10;
                        CAL_LEN -= 1;
                        if CAL_LEN == 0 {
                            CAL_ENTRY = 0;
                        }
                    }
                    CAL_ERR = false;
                    cal_refresh();
                    return true;
                }
                false
            }
        }
    }
}

unsafe fn cal_text() -> &'static str {
    if CAL_ERR {
        return "err";
    }
    let txt: &[u8; 18] = &*core::ptr::addr_of!(CAL_TXT);
    core::str::from_utf8_unchecked(&txt[..CAL_TXT_LEN])
}

fn calc_body(s: &mut Surface<'_>, accent: u32, wx: i32, wy: i32, w: i32, h: i32) {
    unsafe {
        if CAL_TXT_LEN == 1 && CAL_TXT[0] == 0 {
            CAL_TXT[0] = b'0';
            CAL_TXT_LEN = 1;
        }
    }
    let x0 = wx + PAD;
    let x1 = wx + w - PAD;
    /* display strip */
    let dy0 = wy + TITLE_H + 8;
    let dy1 = dy0 + 44;
    if x1 - x0 < 40 || dy1 > wy + h - 20 {
        return;
    }
    s.rect_fill(x0, dy0, x1, dy1, 0x1c2028, 0xf0);
    s.rect_fill(x0, dy0, x1, dy0 + 1, accent, 0x90);
    let txt = unsafe { cal_text() };
    let tw = 10 * 2 * txt.len() as i32;
    let tx = if x1 - tw - 8 > x0 { x1 - tw - 8 } else { x0 + 4 };
    s.text(tx, dy0 + 12, txt, 0xffe6c8, 0xff, 2);

    /* 4x4 keypad */
    let keys = [
        ["7", "8", "9", "/"],
        ["4", "5", "6", "*"],
        ["1", "2", "3", "-"],
        ["C", "0", "=", "+"],
    ];
    let grid_y = dy1 + 10;
    let avail_h = (wy + h - 12) - grid_y;
    let cell_w = ((x1 - x0) / 4).max(1);
    let cell_h = (avail_h / 4).max(1);
    let pad = 4;
    let mut row = 0usize;
    while row < 4 {
        let mut col = 0usize;
        while col < 4 {
            let cx0 = x0 + col as i32 * cell_w + pad;
            let cy0 = grid_y + row as i32 * cell_h + pad;
            let cx1 = x0 + (col as i32 + 1) * cell_w - pad;
            let cy1 = grid_y + (row as i32 + 1) * cell_h - pad;
            let label = keys[row][col];
            let op = match label {
                "/" => b'/' as u32,
                "*" => b'*' as u32,
                "-" => b'-' as u32,
                "+" => b'+' as u32,
                "=" => b'=' as u32,
                _ => 0,
            };
            let pending = unsafe { CAL_OP as u32 } == op && op != 0;
            let plate = if pending { lighten(accent, 30) } else { 0x2a3140 };
            s.rect_fill(cx0, cy0, cx1, cy1, plate, 0xff);
            s.rect_fill(cx0, cy0, cx1, cy0 + 1, 0xffffff, 0x2c);
            let txt_c = if op != 0 || label == "C" { 0xffd9a8 } else { 0xe8eef8 };
            let tw = 10 * label.len() as i32;
            s.text(cx0 + ((cx1 - cx0) - tw) / 2, cy0 + ((cy1 - cy0) - 16) / 2,
                   label, txt_c, 0xf0, 1);
            col += 1;
        }
        row += 1;
    }
    let _ = h;
}

fn editor_body(s: &mut Surface<'_>, accent: u32, wx: i32, wy: i32, w: i32, h: i32) {
    let x0 = wx + PAD;
    let x1 = wx + w - PAD;
    let y0 = wy + TITLE_H + 10;
    let (cur_line, cur_col, rows) = editor_caret();
    if y0 + 3 * 18 + 42 > wy + h {
        return;                     /* small window: no room for the listing */
    }
    let visible = (((wy + h - 34 - y0) / 18).max(1) as i32).min(rows);
    /* line-number gutter */
    s.rect_fill(x0, y0, x0 + 26, y0 + 18 * visible, 0x000000, 0x34);
    let mut row = 0i32;
    while row < visible {
        let ly = y0 + 4 + row * 18;
        if row == cur_line {
            /* current line highlight (the caret line) */
            s.rect_fill(x0 + 26, ly - 1, x1, ly + 17, 0xffffff, 0x0a);
        }
        /* one-digit line numbers (the buffer is capped at ED_LINES = 10) */
        let mut nb = [b'0'; 1];
        nb[0] = b'0' + ((row + 1) % 10) as u8;
        let label = core::str::from_utf8(&nb[..]).unwrap_or("?");
        text(s, x0 + 14, ly, label, TEXT_LOW, 0xc0);
        let (line, len) = unsafe { (&ED_BUF[row as usize], ED_LEN[row as usize]) };
        let mut tx = x0 + 34;
        let mut k = 0usize;
        while k < len {
            let ch = line[k] as char;
            tx += s.char_ascii(tx, ly, ch, TEXT_HI, 0xf0, 1);
            k += 1;
            if tx > x1 - 12 {
                break;
            }
        }
        if row == cur_line {
            let cx = x0 + 34 + cur_col.min(len as i32) * 10;
            s.rect_fill(cx, ly, cx + 2, ly + 16, accent, 0xf0);
        }
        row += 1;
    }
    /* status bar along the window's bottom edge: Ln/Col + how to save */
    let sy = wy + h - 26;
    s.rect_fill(x0, sy - 7, x1, sy - 6, 0xffffff, 0x1c);
    s.rect_fill(x0, sy - 5, x1, sy + 18, 0x000000, 0x1c);
    let mut b0 = [0u8; 12];
    let mut b1 = [0u8; 12];
    let mut b2 = [0u8; 12];
    let mut x = x0 + 2;
    x = text_at(s, x, sy, "Ln ", TEXT_LOW, 0xd0);
    x = text_at(s, x, sy, u32_str(&mut b0, (cur_line + 1) as u32), TEXT_LOW, 0xd0);
    x = text_at(s, x, sy, ", Col ", TEXT_LOW, 0xd0);
    x = text_at(s, x, sy, u32_str(&mut b1, (cur_col + 1) as u32), TEXT_LOW, 0xd0);
    x = text_at(s, x, sy, " ・ ", TEXT_LOW, 0xc8);
    x = text_at(s, x, sy, u32_str(&mut b2, rows as u32), TEXT_LOW, 0xd0);
    x = text_at(s, x, sy, " 行 ・ F5 保存", TEXT_LOW, 0xc8);
    let _ = x;
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
