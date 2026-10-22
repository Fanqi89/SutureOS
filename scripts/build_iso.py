#!/usr/bin/env python3
# SutureOS ISO builder - Creates professional ISO with Limine + GRUB dual boot
# Implements /boot /system /EFI standard layout
import os
import subprocess
import sys
import shutil
import json
import re
from pathlib import Path

def _sutureos_version():
    """Read the version out of include/sutureos/version.h (single source)."""
    hdr = ROOT / "include" / "sutureos" / "version.h"
    try:
        text = hdr.read_text(encoding="utf-8")
    except OSError:
        return "0.0.0-unknown"
    def num(key):
        m = re.search(r"#define\s+SUTUREOS_VERSION_%s\s+(\d+)" % key, text)
        return m.group(1) if m else "0"
    m = re.search(r'#define\s+SUTUREOS_VERSION_PRE\s+"([^"]+)"', text)
    pre = m.group(1) if m else ""
    v = "%s.%s.%s" % (num("MAJOR"), num("MINOR"), num("PATCH"))
    return v + ("-" + pre if pre else "")



# ROOT follows the script, so a git worktree / CI checkout builds its own
# sources into its own build/ dir. SUTUREOS_ROOT overrides it explicitly.
_here = Path(__file__).resolve().parent.parent
ROOT = Path(os.environ.get("SUTUREOS_ROOT", _here))
BUILD = ROOT / "build"
ISO_DIR = BUILD / "iso_root"
ISO_OUTPUT = BUILD / "sutureos.iso"

LIMINE_BIN = ROOT / "tools" / "limine" / "limine-binary"
GRUB_PREFIX = Path(r"C:\msys64\mingw64")

def run(cmd, cwd=None):
    print(f"  $ {' '.join(cmd)}")
    # Use temp files for stdout/stderr to avoid encoding issues
    import tempfile
    err_path = tempfile.mktemp(suffix='.err')
    out_path = tempfile.mktemp(suffix='.out')
    
    try:
        with open(out_path, 'wb') as fout, open(err_path, 'wb') as ferr:
            result = subprocess.run(cmd, cwd=cwd, stdout=fout, stderr=ferr)
        
        # Read back with error handling
        stderr_text = ""
        stdout_text = ""
        try:
            with open(err_path, 'rb') as f:
                stderr_text = f.read().decode('gbk', errors='replace')
        except:
            try:
                with open(err_path, 'rb') as f:
                    stderr_text = f.read().decode('utf-8', errors='replace')
            except:
                stderr_text = "<could not decode stderr>"
        
        try:
            with open(out_path, 'rb') as f:
                stdout_text = f.read().decode('gbk', errors='replace')
        except:
            try:
                with open(out_path, 'rb') as f:
                    stdout_text = f.read().decode('utf-8', errors='replace')
            except:
                stdout_text = "<could not decode stdout>"
        
        if result.returncode != 0:
            try:
                print(f"FAILED (exit {result.returncode}): {stderr_text}")
            except UnicodeEncodeError:
                print(f"FAILED (exit {result.returncode}): <encoding error in stderr>")
            return False
        if stdout_text.strip():
            try:
                print(f"  {stdout_text.strip()}")
            except UnicodeEncodeError:
                print("  <output contains non-displayable characters>")
        return True
    finally:
        try:
            os.unlink(err_path)
        except:
            pass
        try:
            os.unlink(out_path)
        except:
            pass

def main():
    print("=== SutureOS ISO Builder ===")
    
    # Clean and create ISO root
    if ISO_DIR.exists():
        shutil.rmtree(ISO_DIR)
    ISO_DIR.mkdir(parents=True)
    
    # Create standard directory layout
    dirs = [
        "boot/limine",
        "boot/grub",
        "EFI/BOOT",
        "system/kernel",
        "system/modules",
        "system/initrd",
        "system/config",
    ]
    for d in dirs:
        (ISO_DIR / d).mkdir(parents=True, exist_ok=True)
    
    # 1. Copy kernel to /system/kernel/kernel.elf (primary location)
    kernel_src = BUILD / "kernel.elf"
    kernel_dst = ISO_DIR / "system" / "kernel" / "kernel.elf"
    shutil.copy2(kernel_src, kernel_dst)
    print(f"Copied kernel: {kernel_dst}")
    
    # 2. Copy kernel to /boot/kernel.elf (for multiboot/Limine)
    boot_kernel = ISO_DIR / "boot" / "kernel.elf"
    shutil.copy2(kernel_src, boot_kernel)
    
    # 3. Copy Limine files
    limine_files = [
        ("limine-bios.sys", "boot/limine/limine-bios.sys"),
        ("limine-bios-cd.bin", "boot/limine/limine-bios-cd.bin"),
        ("limine-uefi-cd.bin", "boot/limine/limine-uefi-cd.bin"),
    ]
    for src_name, dst_path in limine_files:
        src = LIMINE_BIN / src_name
        if src.exists():
            shutil.copy2(src, ISO_DIR / dst_path)
            print(f"Copied Limine: {dst_path}")
        else:
            print(f"WARNING: Limine file not found: {src}")
    
    # Copy limine.conf from project boot directory (not from Limine binary)
    project_limine_conf = ROOT / "boot" / "limine.conf"
    if project_limine_conf.exists():
        shutil.copy2(project_limine_conf, ISO_DIR / "boot" / "limine" / "limine.conf")
        shutil.copy2(project_limine_conf, ISO_DIR / "limine.conf")
        print(f"Copied limine.conf from project boot directory")
    else:
        print(f"WARNING: Project limine.conf not found: {project_limine_conf}")
    
    # Also copy Limine binaries to root for Limine scanning
    for src_name, _ in limine_files:
        src = LIMINE_BIN / src_name
        if src.exists():
            shutil.copy2(src, ISO_DIR / src_name)
    
    # 4. Copy GRUB files (if available)
    grub_efi = GRUB_PREFIX / "share" / "grub" / "x86_64-efi"
    if grub_efi.exists():
        # Create GRUB EFI bootloader
        run([
            str(GRUB_PREFIX / "bin" / "grub-mkimage"),
            "-O", "x86_64-efi",
            "-o", str(ISO_DIR / "EFI" / "BOOT" / "BOOTX64.EFI"),
            "-p", "/boot/grub",
            "part_gpt", "part_msdos", "fat", "ext2", "normal",
            "configfile", "linux", "multiboot", "multiboot2",
            "chain", "efi_gop", "efi_uga", "gzio", "iso9660",
            "search", "search_fs_uuid", "search_fs_file", "search_label"
        ])
        
        # Copy GRUB modules
        grub_modules_dst = ISO_DIR / "boot" / "grub" / "x86_64-efi"
        grub_modules_dst.mkdir(parents=True, exist_ok=True)
        for mod in grub_efi.glob("*.mod"):
            shutil.copy2(mod, grub_modules_dst / mod.name)
        
        # Create grub.cfg
        grub_cfg = ISO_DIR / "boot" / "grub" / "grub.cfg"
        grub_cfg.write_text("""set timeout=3
set default=0

menuentry "SutureOS (Multiboot)" {
    multiboot /boot/kernel.elf
    boot
}

menuentry "SutureOS (Verbose)" {
    multiboot /boot/kernel.elf verbose
    boot
}

menuentry "SutureOS (Limine)" {
    chainloader /EFI/BOOT/BOOTX64.EFI
}
""")
        print("Created GRUB EFI bootloader and config")
    else:
        print("WARNING: GRUB not found, using Limine only")
        # Create minimal EFI bootloader using Limine
        limine_uefi = LIMINE_BIN / "limine-uefi-cd.bin"
        if limine_uefi.exists():
            shutil.copy2(limine_uefi, ISO_DIR / "EFI" / "BOOT" / "BOOTX64.EFI")
    
    # 5. Copy system config
    system_config = ISO_DIR / "system" / "config"
    system_config.mkdir(parents=True, exist_ok=True)
    
    # Create system manifest
    manifest = ISO_DIR / "system" / "manifest.json"
    manifest.write_text(json.dumps({
        "name": "SutureOS",
        "version": _sutureos_version(),
        "kernel": "kernel.elf",
        "arch": "x86_64",
        "license": "GPL-3.0",
        "bootloader": ["Limine", "GRUB"],
        "filesystems": ["VimtuFS2", "FatFS", "ext2", "SquashFS"],
        "features": ["SMP", "EL0", "musl", "TCP/IP", "GUI"]
    }, indent=2))
    
    # 6. Create initrd (minimal - just config for now)
    initrd_dir = BUILD / "initrd_root"
    if initrd_dir.exists():
        shutil.rmtree(initrd_dir)
    initrd_dir.mkdir(parents=True)
    (initrd_dir / "etc").mkdir()
    (initrd_dir / "etc" / "sutureos").mkdir()
    (initrd_dir / "etc" / "sutureos" / "init.conf").write_text("""# SutureOS init configuration
root_fs_type=vimtufs2
root_device=/dev/sda1
init=/system/bin/init
log_level=info
""")
    
    # Create initrd as tar.gz
    run(["tar", "-czf", str(ISO_DIR / "system" / "initrd" / "initrd.tar.gz"), "-C", str(initrd_dir), "."])
    
    # 7. Create ISO with xorriso (Limine-compatible)
    print("Building ISO...")
    # Use a simple ASCII-only temp directory
    import tempfile
    temp_dir = Path("C:/sutureos_iso_temp")
    if temp_dir.exists():
        shutil.rmtree(temp_dir, ignore_errors=True)
    temp_dir.mkdir(parents=True, exist_ok=True)
    temp_iso_root = temp_dir / "iso_root"
    print(f"  Copying ISO root to temp location: {temp_iso_root}")
    shutil.copytree(ISO_DIR, temp_iso_root, dirs_exist_ok=True)
    
    # Use MSYS2's xorriso - need to convert Windows path to MSYS2 path
    # C:\sutureos_iso_temp\iso_root -> /c/sutureos_iso_temp/iso_root
    msys_path = "/c/sutureos_iso_temp/iso_root"
    xorriso_path = r"C:\msys64\usr\bin\xorriso.exe"
    xorriso_cmd = [
        xorriso_path,
        "-as", "mkisofs",
        "-o", str(ISO_OUTPUT),
        "-b", "boot/limine/limine-bios-cd.bin",
        "-no-emul-boot",
        "-boot-load-size", "4",
        "-boot-info-table",
        "--efi-boot", "boot/limine/limine-uefi-cd.bin",
        "-efi-boot-part", "--efi-boot-image",
        "--protective-msdos-label",
        "-V", "SUTUREOS",
        "-r", "-J", "-joliet-long",
        msys_path
    ]
    
    if not run(xorriso_cmd):
        # Cleanup temp
        shutil.rmtree(temp_dir, ignore_errors=True)
        print("ISO creation failed")
        return 1
    
    # Cleanup temp
    shutil.rmtree(temp_dir, ignore_errors=True)
    
    # 8. Install Limine to the ISO (makes it bootable on USB/hdd)
    print("Installing Limine to ISO...")
    limine_deploy = LIMINE_BIN / "limine-deploy"
    if limine_deploy.exists():
        run([str(limine_deploy), str(ISO_OUTPUT)])
    else:
        # Try using limine.exe
        limine_exe = LIMINE_BIN / "limine.exe"
        if limine_exe.exists():
            run([str(limine_exe), "iso-install", str(ISO_OUTPUT)])
    
    print(f"\n=== ISO created: {ISO_OUTPUT} ===")
    print(f"Size: {ISO_OUTPUT.stat().st_size / 1024 / 1024:.1f} MB")
    
    # Verify layout
    print("\n=== ISO Layout ===")
    for root, dirs, files in os.walk(ISO_DIR):
        level = root.replace(str(ISO_DIR), '').count(os.sep)
        indent = '  ' * level
        print(f"{indent}{os.path.basename(root)}/")
        subindent = '  ' * (level + 1)
        for file in files:
            print(f"{subindent}{file}")
    
    return 0

if __name__ == "__main__":
    sys.exit(main())