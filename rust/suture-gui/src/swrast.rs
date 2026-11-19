//! Hand-written software rasteriser for the SutureOS GUI prototype frame.
//!
//! Why this exists: the Slint route links and initialises inside the kernel
//! (`[gui-slint] platform ready`, timers pumped) but its software renderer hangs
//! on anything richer than a solid rect + one rounded rect: a scene with
//! gradients and/or the 4-layer emulated shadow never returns from
//! `SoftwareRenderer::render()`. Bisected in this batch (docs/slint-gui-stage0.md):
//! minimal scene renders (checksum OK), full scene stops after `stage: draw`.
//! So the Stage-0 deliverable draws the SAME design by hand — rounded corners,
//! axis-aligned gradients, alpha compositing, stacked-rect shadows, pseudo-3D
//! icon — with no dependency except `libm` (no_std f32 math).
//!
//! Writes straight into the framebuffer (0x00RRGGBB little-endian BGRX), the
//! same format kernel/gui/gui.c uses.

// no_std f32 helpers: `floor/sqrt/powi/max/min/clamp` live in std, so the
// handful we need are re-implemented here on top of libm.
#[inline]
fn fmax(a: f32, b: f32) -> f32 {
    if a > b {
        a
    } else {
        b
    }
}
#[inline]
fn fmin(a: f32, b: f32) -> f32 {
    if a < b {
        a
    } else {
        b
    }
}
#[inline]
fn fabs(a: f32) -> f32 {
    if a < 0.0 {
        -a
    } else {
        a
    }
}
#[inline]
fn clampf(v: f32, lo: f32, hi: f32) -> f32 {
    fmin(fmax(v, lo), hi)
}
#[inline]
fn fsqrt(a: f32) -> f32 {
    libm::sqrtf(if a > 0.0 { a } else { 0.0 })
}
#[inline]
fn ffloor(a: f32) -> f32 {
    libm::floorf(a)
}
#[inline]
fn fceil(a: f32) -> f32 {
    libm::ceilf(a)
}

/// Linear interpolation of two 0xRRGGBB colours, t in 0..=1.
pub fn lerp_rgb(a: u32, b: u32, t: f32) -> u32 {
    let t = clampf(t, 0.0, 1.0);
    let mix = |sh: u32| -> u32 {
        let ca = ((a >> sh) & 0xff) as f32;
        let cb = ((b >> sh) & 0xff) as f32;
        ((ca + (cb - ca) * t) as u32) & 0xff
    };
    (mix(16) << 16) | (mix(8) << 8) | mix(0)
}

#[inline]
fn blend(dst: u32, src: u32, alpha: u32) -> u32 {
    let inv = 255 - alpha;
    let r = (((dst >> 16) & 0xff) * inv + ((src >> 16) & 0xff) * alpha) / 255;
    let g = (((dst >> 8) & 0xff) * inv + ((src >> 8) & 0xff) * alpha) / 255;
    let b = ((dst & 0xff) * inv + (src & 0xff) * alpha) / 255;
    (r << 16) | (g << 8) | b
}

pub struct Surface<'a> {
    pub px: &'a mut [u32],
    pub w: usize,
    pub h: usize,
    pub stride: usize,
}

impl<'a> Surface<'a> {
    #[inline]
    pub fn get(&self, x: usize, y: usize) -> u32 {
        if x >= self.w || y >= self.h {
            return 0;
        }
        self.px[y * self.stride + x] & 0x00ff_ffff
    }

    #[inline]
    pub fn put(&mut self, x: usize, y: usize, c: u32) {
        if x < self.w && y < self.h {
            self.px[y * self.stride + x] = c & 0x00ff_ffff;
        }
    }

    #[inline]
    pub fn blend_px(&mut self, x: usize, y: usize, c: u32, a: u32) {
        if x < self.w && y < self.h && a > 0 {
            let i = y * self.stride + x;
            let d = self.px[i] & 0x00ff_ffff;
            self.px[i] = if a >= 255 { c & 0x00ff_ffff } else { blend(d, c, a) };
        }
    }

    /// Vertical gradient (180deg) over an axis-aligned rect, opaque.
    pub fn vgrad(&mut self, x0: i32, y0: i32, x1: i32, y1: i32, top: u32, bottom: u32) {
        let hgt = fmax((y1 - y0) as f32, 1.0);
        for y in y0.max(0)..y1.min(self.h as i32) {
            let c = lerp_rgb(top, bottom, (y - y0) as f32 / hgt);
            for x in x0.max(0)..x1.min(self.w as i32) {
                self.put(x as usize, y as usize, c);
            }
        }
    }

    /// Vertical gradient with a middle stop (wallpaper: 0% / mid / 100%).
    pub fn vgrad3(
        &mut self,
        x0: i32,
        y0: i32,
        x1: i32,
        y1: i32,
        c0: u32,
        c1: u32,
        c2: u32,
        mid: f32,
    ) {
        let hgt = fmax((y1 - y0) as f32, 1.0);
        for y in y0.max(0)..y1.min(self.h as i32) {
            let t = (y - y0) as f32 / hgt;
            let c = if t <= mid {
                lerp_rgb(c0, c1, t / mid)
            } else {
                lerp_rgb(c1, c2, (t - mid) / (1.0 - mid))
            };
            for x in x0.max(0)..x1.min(self.w as i32) {
                self.put(x as usize, y as usize, c);
            }
        }
    }

    /// Horizontal gradient (90deg) over an axis-aligned rect, opaque.
    pub fn hgrad(&mut self, x0: i32, y0: i32, x1: i32, y1: i32, left: u32, right: u32) {
        let wid = fmax((x1 - x0) as f32, 1.0);
        for x in x0.max(0)..x1.min(self.w as i32) {
            let c = lerp_rgb(left, right, (x - x0) as f32 / wid);
            for y in y0.max(0)..y1.min(self.h as i32) {
                self.put(x as usize, y as usize, c);
            }
        }
    }

    /// Horizontal gradient (90deg) inside a rounded shape, alpha composited.
    /// Used for the wallpaper colour blobs so they read as soft glow instead of
    /// debug rectangles.
    pub fn hgrad_round(
        &mut self,
        x0: f32,
        y0: f32,
        x1: f32,
        y1: f32,
        r: f32,
        left: u32,
        right: u32,
        alpha: u32,
    ) {
        let r = clampf(r, 0.0, fmin((x1 - x0) * 0.5, (y1 - y0) * 0.5));
        let wid = fmax(x1 - x0, 1.0);
        let rows = (y1 - y0) as i32;
        for row in 0..rows {
            let fy = y0 + row as f32;
            let dyc = fabs((fy + 0.5) - (y0 + (y1 - y0) * 0.5));
            let half = (y1 - y0) * 0.5;
            let inset = if dyc > half - r {
                let over = dyc - (half - r);
                r - fsqrt(r * r - over * over)
            } else {
                0.0
            };
            let (xa, xb) = (x0 + inset, x1 - inset);
            let steps = (xb - xa) as i32;
            for step in 0..steps {
                let fx = xa + step as f32;
                let c = lerp_rgb(left, right, (fx - x0) / wid);
                self.blend_px(fx as usize, fy as usize, c, alpha);
            }
        }
    }

    /// Anti-aliased rounded rectangle, alpha composited.
    ///
    /// One pixel of analytic AA at the boundary: the signed distance to the
    /// rounded outline is exact, so `0.5 - distance` is the edge coverage.
    pub fn round_rect(&mut self, x0: f32, y0: f32, x1: f32, y1: f32, r: f32, c: u32, alpha: u32) {
        if alpha == 0 || x1 <= x0 || y1 <= y0 {
            return;
        }
        let r = clampf(r, 0.0, fmin((x1 - x0) * 0.5, (y1 - y0) * 0.5));
        let ys = fmax(ffloor(y0), 0.0) as i32;
        let ye = fmin(fceil(y1), self.h as f32) as i32;
        let xs = fmax(ffloor(x0), 0.0) as i32;
        let xe = fmin(fceil(x1), self.w as f32) as i32;
        for y in ys..ye {
            let fy = y as f32 + 0.5;
            if fy < y0 || fy >= y1 {
                continue;
            }
            for x in xs..xe {
                let fx = x as f32 + 0.5;
                if fx < x0 || fx >= x1 {
                    continue;
                }
                let cx = clampf(fx, x0 + r, x1 - r);
                let cy = clampf(fy, y0 + r, y1 - r);
                let dx = fx - cx;
                let dy = fy - cy;
                let d = fsqrt(dx * dx + dy * dy) - r;
                let cov = clampf(0.5 - d, 0.0, 1.0);
                if cov > 0.0 {
                    let a = (cov * alpha as f32) as u32;
                    self.blend_px(x as usize, y as usize, c, if a > 255 { 255 } else { a });
                }
            }
        }
    }

    /// 1 px rounded outline (panel / card hairline border).
    pub fn round_rect_stroke(
        &mut self,
        x0: f32,
        y0: f32,
        x1: f32,
        y1: f32,
        r: f32,
        c: u32,
        alpha: u32,
    ) {
        if alpha == 0 || x1 <= x0 || y1 <= y0 {
            return;
        }
        let r = clampf(r, 0.0, fmin((x1 - x0) * 0.5, (y1 - y0) * 0.5));
        let ys = fmax(ffloor(y0), 0.0) as i32;
        let ye = fmin(fceil(y1), self.h as f32) as i32;
        let xs = fmax(ffloor(x0), 0.0) as i32;
        let xe = fmin(fceil(x1), self.w as f32) as i32;
        for y in ys..ye {
            let fy = y as f32 + 0.5;
            for x in xs..xe {
                let fx = x as f32 + 0.5;
                let cx = clampf(fx, x0 + r, x1 - r);
                let cy = clampf(fy, y0 + r, y1 - r);
                let dx = fx - cx;
                let dy = fy - cy;
                // absolute distance to the outline: inside the straight edges the
                // nearest outline point is the edge itself
                let d_outline = if dx == 0.0 && dy == 0.0 {
                    // inside the core: distance to the nearest straight edge
                    fmin(
                        fmin(fx - x0, x1 - fx),
                        fmin(fy - y0, y1 - fy),
                    )
                } else {
                    fabs(fsqrt(dx * dx + dy * dy) - r)
                };
                if d_outline < 1.0 {
                    let a = ((1.0 - d_outline) * alpha as f32) as u32;
                    self.blend_px(x as usize, y as usize, c, if a > 255 { 255 } else { a });
                }
            }
        }
    }

    /// Emulated soft shadow: 4 nested translucent rounded rects (the same
    /// construction validated in the Slint feasibility spike; a real drop shadow
    /// is impossible there because draw_box_shadow is a `// TODO` stub).
    pub fn fake_shadow(&mut self, x: f32, y: f32, w: f32, h: f32, r: f32, dy: f32, tint: u32, strength: f32) {
        let layers = [
            (-10.0f32, 10.0f32, 0.05f32),
            (-7.0, 7.0, 0.07),
            (-4.0, 4.0, 0.10),
            (-1.0, 1.0, 0.14),
        ];
        for (m, rr, a) in layers {
            let alpha = (a * strength * 255.0) as u32;
            self.round_rect(
                x + m,
                y + dy + m,
                x + w - m,
                y + h + dy - m,
                r + rr,
                tint,
                if alpha > 255 { 255 } else { alpha },
            );
        }
    }

    /// Pseudo-3D app icon: vertical gradient body (clipped to the rounded shape
    /// per row), specular band on top, inner shade at the bottom, glyph plate.
    pub fn icon(&mut self, x: f32, y: f32, s: f32) {
        let r = s * 0.24;
        // body: one row at a time so the gradient follows the rounded silhouette
        let rows = s as i32;
        for row in 0..rows {
            let t = row as f32 / s;
            let c = lerp_rgb(0x8fc2ff, 0x2c4fb0, t);
            let fy = y + row as f32;
            let dyc = fabs((fy + 0.5) - (y + s * 0.5));
            let half = s * 0.5;
            let inset = if dyc > half - r {
                let over = dyc - (half - r);
                r - fsqrt(r * r - over * over)
            } else {
                0.0
            };
            self.round_rect(x + inset, fy, x + s - inset, fy + 1.0, fmax(s * 0.5 - inset, 0.0), c, 255);
        }
        // specular band (white -> transparent) over the top third
        let bx = x + s * 0.10;
        let bw = s * 0.80;
        let by = y + s * 0.07;
        let bh = s * 0.34;
        let band_rows = bh as i32;
        for row in 0..band_rows {
            let t = row as f32 / bh;
            let a = ((1.0 - t) * 0.72 * 255.0) as u32;
            self.round_rect(bx, by + row as f32, bx + bw, by + row as f32 + 1.0, bh * 0.45, 0xffffff, a);
        }
        // bottom inner shade
        let sy = y + s * 0.78;
        let sh = s * 0.22;
        let shade_rows = sh as i32;
        for row in 0..shade_rows {
            let t = row as f32 / sh;
            let a = (t * 0.20 * 255.0) as u32;
            self.round_rect(x, sy + row as f32, x + s, sy + row as f32 + 1.0, r, 0x000000, a);
        }
        // glyph plate
        self.round_rect(
            x + s * 0.30,
            y + s * 0.36,
            x + s * 0.70,
            y + s * 0.64,
            s * 0.14,
            0xffffff,
            0xd0,
        );
    }
}

// ===========================================================================
// Text (v0.38.0)
// ---------------------------------------------------------------------------
// The kernel already carries two bitmap fonts and the new renderer draws with
// the SAME assets instead of shipping a second copy (the GB2312 blob is 238 KiB):
//
//   * ASCII 32..126 -> font8x16  (8x16, one byte per row, MSB = left pixel)
//   * GB2312 subset -> cjk16     (16x16, two bytes per row, MSB = left pixel)
//
// Both are fetched through kernel/gui/slint_bridge.c. Alpha 255 is used for
// headings so acceptance can pixel-match the glyphs exactly; lower alphas are
// blended over whatever is behind the text.
extern "C" {
    fn slint_bridge_ascii(ch: u32, out: *mut u8) -> i32;
    fn slint_bridge_cjk(codepoint: u32, out: *mut u8) -> i32;
}

/// Advance of one ASCII cell at `scale` (glyph width + 2 px side bearing).
#[inline]
pub fn advance_ascii(scale: i32) -> i32 {
    10 * scale
}

/// Advance of one wide (CJK) cell at `scale`.
#[inline]
pub fn advance_cjk(scale: i32) -> i32 {
    18 * scale
}

impl<'a> Surface<'a> {
    /// Blit a 1-bit glyph mask scaled by `scale` (nearest neighbour), blended
    /// with `alpha` over the destination.
    fn glyph_mask(&mut self, x: i32, y: i32, w: i32, h: i32, rows: &[u8],
                  row_bytes: usize, fg: u32, alpha: u32, scale: i32) {
        for r in 0..h {
            for c in 0..w {
                let byte = rows[(r as usize) * row_bytes + (c as usize) / 8];
                let bit = (byte >> (7 - (c % 8))) & 1;
                if bit == 0 {
                    continue;
                }
                for dy in 0..scale {
                    for dx in 0..scale {
                        let px = x + c * scale + dx;
                        let py = y + r * scale + dy;
                        if px >= 0 && py >= 0 {
                            self.blend_px(px as usize, py as usize, fg, alpha);
                        }
                    }
                }
            }
        }
    }

    /// Draw one ASCII character; returns the advance in pixels (0 if unsupported).
    pub fn char_ascii(&mut self, x: i32, y: i32, ch: char, fg: u32, alpha: u32,
                      scale: i32) -> i32 {
        let mut g = [0u8; 16];
        let ok = unsafe { slint_bridge_ascii(ch as u32, g.as_mut_ptr()) };
        if ok == 0 {
            return advance_ascii(scale);
        }
        self.glyph_mask(x, y, 8, 16, &g, 1, fg, alpha, scale);
        advance_ascii(scale)
    }

    /// Draw one wide (CJK) character; returns the advance in pixels.
    pub fn char_cjk(&mut self, x: i32, y: i32, cp: u32, fg: u32, alpha: u32,
                    scale: i32) -> i32 {
        let mut g = [0u8; 32];
        let ok = unsafe { slint_bridge_cjk(cp, g.as_mut_ptr()) };
        if ok == 0 {
            return advance_cjk(scale);
        }
        self.glyph_mask(x, y, 16, 16, &g, 2, fg, alpha, scale);
        advance_cjk(scale)
    }

    /// Draw a UTF-8 string (ASCII + GB2312 CJK). Returns the total advance.
    pub fn text(&mut self, x: i32, y: i32, text: &str, fg: u32, alpha: u32,
                scale: i32) -> i32 {
        let mut pen = x;
        let bytes = text.as_bytes();
        let mut i = 0;
        while i < bytes.len() {
            let b = bytes[i];
            let (cp, len) = if b < 0x80 {
                (b as u32, 1)
            } else if b & 0xE0 == 0xC0 && i + 1 < bytes.len() {
                ((((b & 0x1F) as u32) << 6) | ((bytes[i + 1] & 0x3F) as u32), 2)
            } else if b & 0xF0 == 0xE0 && i + 2 < bytes.len() {
                ((((b & 0x0F) as u32) << 12)
                    | (((bytes[i + 1] & 0x3F) as u32) << 6)
                    | ((bytes[i + 2] & 0x3F) as u32), 3)
            } else if b & 0xF8 == 0xF0 && i + 3 < bytes.len() {
                ((((b & 0x07) as u32) << 18)
                    | (((bytes[i + 1] & 0x3F) as u32) << 12)
                    | (((bytes[i + 2] & 0x3F) as u32) << 6)
                    | ((bytes[i + 3] & 0x3F) as u32), 4)
            } else {
                (b as u32, 1)
            };
            i += len;
            if cp < 0x80 {
                pen += self.char_ascii(pen, y, core::char::from_u32(cp).unwrap_or('?'),
                                       fg, alpha, scale);
            } else if self.char_cjk(pen, y, cp, fg, alpha, scale) == 0 {
                pen += advance_cjk(scale);
            } else {
                pen += advance_cjk(scale);
            }
        }
        pen
    }
}

// ===========================================================================
// Acrylic (v0.39.0): separable box blur of the backdrop region
// ---------------------------------------------------------------------------
// "Light acrylic" = the panel shows a BLURRED copy of what is behind it, not a
// semi-transparent copy. Two separable passes (horizontal, then vertical) over a
// temp buffer of the blur rect, O(area * (2r+1)).
//
// IMPORTANT: the three channels are accumulated SEPARATELY (acc_r/g/b). Summing
// the packed 0x00RRGGBB words and dividing once is wrong - the blue field of the
// sum overflows its 8 bits and carries into green (and green into red), which is
// exactly the coloured banding the first version showed on screen.
//
// Memory: the temp buffer covers the blur rect only; the main panel expanded by
// the radius is ~600x360 px = 0.85 MiB of .bss, capped by ACRYLIC_MAX_PX.
const ACRYLIC_MAX_PX: usize = 1024 * 480;
static mut ACRYLIC_TMP: [u32; ACRYLIC_MAX_PX] = [0; ACRYLIC_MAX_PX];

#[inline]
fn rgb_parts(c: u32) -> (u32, u32, u32) {
    ((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff)
}

#[inline]
fn rgb_pack(r: u32, g: u32, b: u32) -> u32 {
    ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff)
}

/// Blur the rectangle (x0,y0)-(x1,y1) IN PLACE. Pixels outside the rectangle are
/// read (so the blur has real neighbours at the edge) but never written, which
/// keeps the drop shadow just outside the acrylic edge intact.
pub fn acrylic_blur(s: &mut Surface<'_>, x0: i32, y0: i32, x1: i32, y1: i32,
                    radius: i32) {
    let r = radius.clamp(1, 24) as i32;
    let rx = (x0 - r).max(0);
    let ry = (y0 - r).max(0);
    let rx1 = (x1 + r).min(s.w as i32);
    let ry1 = (y1 + r).min(s.h as i32);
    let bw = (rx1 - rx) as usize;
    let bh = (ry1 - ry) as usize;
    if bw == 0 || bh == 0 || bw * bh > ACRYLIC_MAX_PX {
        return;
    }
    let win = (2 * r + 1) as u32;
    let tmp = unsafe { &mut *core::ptr::addr_of_mut!(ACRYLIC_TMP) };
    let row_last = s.w as i32 - 1;

    // pass 1: horizontal box blur of every source row of the expanded rect
    for j in 0..bh {
        let base = (ry as usize + j) * s.stride;
        let mut ar: u32 = 0;
        let mut ag: u32 = 0;
        let mut ab: u32 = 0;
        for k in 0..win {
            let sx = (rx + k as i32 - r).clamp(0, row_last) as usize;
            let (cr, cg, cb) = rgb_parts(s.px[base + sx]);
            ar += cr;
            ag += cg;
            ab += cb;
        }
        for x in 0..bw {
            tmp[j * bw + x] = rgb_pack(ar / win, ag / win, ab / win);
            let drop = rx + x as i32 - r;
            if drop >= 0 {
                let (cr, cg, cb) = rgb_parts(s.px[base + drop as usize]);
                ar -= cr;
                ag -= cg;
                ab -= cb;
            }
            let add = rx + x as i32 + r + 1;
            if add <= row_last {
                let (cr, cg, cb) = rgb_parts(s.px[base + add as usize]);
                ar += cr;
                ag += cg;
                ab += cb;
            }
        }
    }

    // pass 2: vertical box blur, written only inside the clip rectangle
    for j in 0..bh {
        let dy = ry as i32 + j as i32;
        if dy < y0 || dy >= y1 {
            continue;
        }
        let dst = dy as usize * s.stride;
        for x in 0..bw {
            let dxx = rx as i32 + x as i32;
            if dxx < x0 || dxx >= x1 {
                continue;
            }
            let mut ar: u32 = 0;
            let mut ag: u32 = 0;
            let mut ab: u32 = 0;
            for k in 0..win {
                let jj = (j as i32 + k as i32 - r).clamp(0, bh as i32 - 1) as usize;
                let (cr, cg, cb) = rgb_parts(tmp[jj * bw + x]);
                ar += cr;
                ag += cg;
                ab += cb;
            }
            s.px[dst + dxx as usize] = rgb_pack(ar / win, ag / win, ab / win);
        }
    }
}
