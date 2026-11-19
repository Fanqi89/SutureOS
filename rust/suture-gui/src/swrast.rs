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
