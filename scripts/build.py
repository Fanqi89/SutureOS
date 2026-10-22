#!/usr/bin/env python3
# StitchOS (缝合怪系统) build script - Python version (handles Unicode paths)
import os
import subprocess
import sys
from pathlib import Path

# ROOT follows the script, so a git worktree / CI checkout builds its own
# sources into its own build/ dir. SUTUREOS_ROOT overrides it explicitly.
_here = Path(__file__).resolve().parent.parent
ROOT = Path(os.environ.get("SUTUREOS_ROOT", _here))
CC = Path(r"C:\msys64\mingw64\bin\clang.exe")
OBJCOPY = Path(r"C:\msys64\mingw64\bin\objcopy.exe")
NASM = Path(r"C:\msys64\usr\bin\nasm.exe")
LD_CANDIDATES = [
    ROOT / "tools" / "cross" / "bin" / "x86_64-elf-ld.exe",
    Path(r"C:\Users\fanqi\AppData\Local\Temp\xb64\install\bin\x86_64-elf-ld.exe"),
]
BUILD = ROOT / "build"
OBJ = BUILD / "obj"

def main():
    # Check clang
    if not CC.exists():
        print(f"clang not found: {CC}")
        return 1
    
    # Find linker
    LD = None
    for cand in LD_CANDIDATES:
        if cand.exists():
            LD = cand
            break
    if not LD:
        print("x86_64-elf-ld not found. Run scripts/fetch-toolchain first.")
        return 1
    
    if not NASM.exists():
        print(f"nasm not found: {NASM}")
        return 1
    
    if not OBJCOPY.exists():
        print(f"objcopy not found: {OBJCOPY}")
        return 1
    
    # Create directories
    if OBJ.exists():
        import shutil
        shutil.rmtree(OBJ)
    OBJ.mkdir(parents=True)
    
    # Get clang resource directory
    result = subprocess.run([str(CC), "-print-resource-dir"], capture_output=True, text=True)
    if result.returncode != 0:
        print("Failed to get clang resource dir")
        return 1
    resource_dir = result.stdout.strip()
    
    # Compiler flags
    cflags = [
        "--target=x86_64-unknown-none-elf",
        "-ffreestanding", "-fno-builtin",
        "-mno-red-zone", "-mno-sse", "-mno-mmx", "-mno-80387",
        "-fno-pic", "-fno-pie", "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
        "-nostdinc", "-isystem", f"{resource_dir}/include",
        f"-I{ROOT}/include",
        f"-I{ROOT}/arch/x86_64",
        f"-I{ROOT}/kernel/interrupts",
        f"-I{ROOT}/kernel/mm",
        f"-I{ROOT}/kernel/drivers",
        f"-I{ROOT}/kernel/syscall",
        f"-I{ROOT}/kernel/sched",
        f"-I{ROOT}/kernel/vfs",
        f"-I{ROOT}/kernel/smp",
        f"-I{ROOT}/kernel/fs",
        f"-I{ROOT}/kernel/fs",
        f"-I{ROOT}/kernel/net",
        f"-I{ROOT}/kernel/gui",
        f"-I{ROOT}/kernel/syscall",
        f"-I{ROOT}/kernel/pkg",
        f"-I{ROOT}/kernel/security",   # userdb / creds / sudoers / sudo
        f"-I{ROOT}/kernel/ipc",        # pipe/fifo
        f"-I{ROOT}/kernel/shell",      # shell_process_line (sudo re-dispatch)
        f"-I{ROOT}/kernel/term",
        "-O2", "-g", "-Wall", "-Wextra"
    ]
    
    # Collect sources
    sources = []
    for pattern in ["boot/*.S", "arch/x86_64/*.c", "arch/x86_64/*.S", 
                    "kernel/**/*.c", "kernel/**/*.S", "lib/*.c"]:
        sources.extend(ROOT.glob(pattern))
    
    # Filter out ap_trampoline.S (needs NASM, compiled separately)
    sources = [s for s in sources if "ap_trampoline" not in str(s)]
    
    # Filter to only files (glob might return dirs for **)
    sources = [s for s in sources if s.is_file()]
    sources = sorted(set(sources))  # deduplicate
    
    if not sources:
        print("no sources found")
        return 1
    
    print(f"Found {len(sources)} source files")
    
    # ---- 1. Compile AP trampoline with NASM ----
    trampoline_src = ROOT / "kernel" / "smp" / "ap_trampoline.asm"
    trampoline_bin = BUILD / "ap_trampoline.bin"
    trampoline_src_str = str(trampoline_src)
    trampoline_bin_str = str(trampoline_bin)
    if os.path.exists(trampoline_src_str):
        print(f"Compiling AP trampoline: {trampoline_src_str}")
        result = subprocess.run([str(NASM), "-f", "bin", trampoline_src_str, "-o", trampoline_bin_str])
        if result.returncode != 0:
            print("AP trampoline compilation failed")
            return 1
        print(f"AP trampoline compiled: {trampoline_bin_str} ({os.path.getsize(trampoline_bin_str)} bytes)")
    else:
        print(f"AP trampoline source not found: {trampoline_src_str}")
        return 1
    
    # ---- 2. Convert trampoline binary to object file with objcopy ----
    # Workaround for MSYS2 objcopy not handling Unicode paths: copy to a
    # FIXED ASCII temp location and run objcopy with the bare file name
    # (cwd=temp_dir). objcopy derives the symbol name from the path it is
    # given: an absolute path yields
    # _binary_C__Users_..._ap_trampoline_bin_start, which the
    # --redefine-sym lines below cannot match. Passing "ap_trampoline.bin"
    # makes the symbols exactly _binary_ap_trampoline_bin_start/_end/_size,
    # so the rename works and the kernel gets stable C-visible symbols.
    trampoline_obj = OBJ / "ap_trampoline.bin.o"
    import tempfile, shutil
    temp_dir = Path(tempfile.gettempdir()) / "sutureos_build"
    temp_dir.mkdir(parents=True, exist_ok=True)
    temp_bin = temp_dir / "ap_trampoline.bin"
    temp_obj = temp_dir / "ap_trampoline.bin.o"
    
    # Copy to temp location
    shutil.copy2(trampoline_bin_str, temp_bin)
    
    print("Converting trampoline binary to object file...")
    result = subprocess.run([
        str(OBJCOPY), "-I", "binary", "-O", "elf64-x86-64", "-B", "i386:x86-64",
        "--rename-section", ".data=.text",  # put in .text section
        "--redefine-sym", "_binary_ap_trampoline_bin_start=ap_trampoline_bin_start",
        "--redefine-sym", "_binary_ap_trampoline_bin_end=ap_trampoline_bin_end",
        "--redefine-sym", "_binary_ap_trampoline_bin_size=ap_trampoline_bin_size",
        "ap_trampoline.bin", "ap_trampoline.bin.o"
    ], cwd=str(temp_dir))
    if result.returncode != 0:
        print("objcopy failed to convert trampoline binary")
        return 1
    
    # Copy back to build directory
    shutil.copy2(temp_obj, trampoline_obj)
    print(f"Trampoline object created: {trampoline_obj}")
    
    # Cleanup temp files
    try:
        os.remove(temp_bin)
        os.remove(temp_obj)
    except:
        pass
    
    # Collect sources
    sources = []
    for pattern in ["boot/*.S", "arch/x86_64/*.c", "arch/x86_64/*.S", 
                    "kernel/**/*.c", "kernel/**/*.S", "lib/*.c"]:
        sources.extend(ROOT.glob(pattern))
    
    # Filter out ap_trampoline.S (compiled separately as binary blob)
    sources = [s for s in sources if "ap_trampoline" not in str(s)]
    
    # Filter to only files (glob might return dirs for **)
    sources = [s for s in sources if s.is_file()]
    sources = sorted(set(sources))  # deduplicate
    
    if not sources:
        print("no sources found")
        return 1
    
    print(f"Found {len(sources)} source files")
    
    # Compile
    objs = []
    fail = 0
    for src in sources:
        rel = src.relative_to(ROOT)
        flat = str(rel).replace("\\", "__").replace("/", "__")
        obj_file = OBJ / (flat + ".o")
        
        print(f"Compiling {rel}")
        args = [str(CC), "-c"] + cflags + ["-o", str(obj_file), str(src)]
        result = subprocess.run(args)
        if result.returncode != 0:
            print(f"COMPILE FAIL: {rel}")
            fail += 1
        else:
            objs.append(str(obj_file))
    
    if fail > 0:
        print(f"{fail} file(s) failed to compile")
        return 1
    
    # Add trampoline object to link
    objs.append(str(trampoline_obj))
    print(f"Compiled {len(objs)-1} source files + 1 trampoline object")
    
    # Link
    elf = BUILD / "kernel.elf"
    ld_args = [str(LD), "-T", str(ROOT / "boot" / "linker.ld"), "-o", str(elf)] + objs
    print("Linking...")
    result = subprocess.run(ld_args)
    if result.returncode != 0:
        print("link failed")
        return 1
    
    print(f"BUILD OK -> {elf}")
    subprocess.run([str(LD), "--version"])
    print(f"kernel.elf: {elf.stat().st_size} bytes")
    return 0

if __name__ == "__main__":
    sys.exit(main())