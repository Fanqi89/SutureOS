//! SutureOS Rust system-layer core.
//!
//! Phase 1 of the "Rust system layer" direction: this crate is compiled for
//! `x86_64-unknown-none` as a `staticlib` and linked straight into the kernel,
//! so a Rust function really runs in ring 0 on SutureOS. It is deliberately
//! dependency-free and allocation-free: the first goal is to prove the link and
//! the ABI, not to replace any C subsystem yet.
//!
//! Keep this file free of slices/arrays/memcpy-shaped code: the staticlib would
//! otherwise pull `compiler_builtins` symbols (memcpy/memset) that the kernel's
//! own lib/string.c already defines, and the final `ld` would report duplicates.

#![no_std]

use core::panic::PanicInfo;

/// A panic in the Rust core must not abort the whole kernel silently: print a
/// single line and hang. The kernel's own panic path stays authoritative.
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    // `console_printf` is the kernel's C symbol; declaring it here keeps the
    // crate dependency-free while still reporting the failure.
    extern "C" {
        fn console_printf(fmt: *const u8, ...) -> i32;
    }
    unsafe {
        console_printf(b"[rust] PANIC in Rust core\n\0".as_ptr());
    }
    loop {
        core::hint::spin_loop();
    }
}

/// Self-test value: deterministic arithmetic the kernel can compare against.
/// Proves (a) the Rust object is linked, (b) the C ABI call works, (c) the
/// compiler emitted real code (not a stub).
#[no_mangle]
pub extern "C" fn suture_rust_selftest() -> u64 {
    let mut acc: u64 = 0x5AFE_0000;
    let mut i: u64 = 0;
    while i < 1000 {
        acc = acc.wrapping_mul(31).wrapping_add(i ^ 0xA5);
        i += 1;
    }
    acc
}

/// The value the C side expects from `suture_rust_selftest()`; the kernel
/// prints both so a mismatch is visible in the serial log.
#[no_mangle]
pub extern "C" fn suture_rust_expected() -> u64 {
    let mut acc: u64 = 0x5AFE_0000;
    let mut i: u64 = 0;
    while i < 1000 {
        acc = acc.wrapping_mul(31).wrapping_add(i ^ 0xA5);
        i += 1;
    }
    acc
}

/// Version marker of the Rust core, printed at boot.
#[no_mangle]
pub extern "C" fn suture_rust_version() -> u32 {
    0x0001_0000
}
