/* SutureOS Rust core probe (v0.36.0).
 *
 * The Rust system-layer core (rust/suture-core, no_std, staticlib for
 * x86_64-unknown-none) is linked into the kernel by scripts/build.py. This file
 * is the C side of the boundary:
 *
 *   - it declares the Rust entry points,
 *   - it provides WEAK fallbacks so the kernel still links (and reports honestly)
 *     on a machine without a Rust toolchain,
 *   - it prints one boot line so "the Rust object is really in the image" is a
 *     serial-log assertion instead of a claim.
 *
 * A mismatch between value and expected means the Rust code did not run as
 * compiled (e.g. link picked a different object) - that is why the Rust side
 * exposes the expected value from an independent implementation of the same
 * loop.
 */
#include <stitch/types.h>
#include <stitch/console.h>

extern uint64_t suture_rust_selftest(void);
extern uint64_t suture_rust_expected(void);
extern uint32_t suture_rust_version(void);

/* Weak fallbacks: overridden by the Rust staticlib when it is linked in. */
__attribute__((weak)) uint64_t suture_rust_selftest(void) { return 0; }
__attribute__((weak)) uint64_t suture_rust_expected(void) { return 0; }
__attribute__((weak)) uint32_t suture_rust_version(void) { return 0; }

void suture_rust_probe(void) {
    const uint32_t ver = suture_rust_version();
    const uint64_t got = suture_rust_selftest();
    const uint64_t want = suture_rust_expected();

    if (ver == 0) {
        console_printf("[rust] core: not linked (C fallback)\n");
        return;
    }
    console_printf("[rust] core v%x linked, selftest=%x expected=%x %s\n",
                   (int64_t)ver, (int64_t)got, (int64_t)want,
                   got == want ? "PASS" : "FAIL");
}
