# 构建与测试（Build & Test）

## 1. 环境要求

| 组件 | 用途 | 本机路径（MSYS2 + QEMU） |
|------|------|--------------------------|
| clang 22 | C/汇编编译（freestanding, x86_64-unknown-none-elf） | `C:\msys64\mingw64\bin\clang.exe` |
| x86_64-elf binutils | ELF 链接器/objcopy/nm（自建，见 §2） | 项目 `tools\cross\bin\` 或临时目录 |
| QEMU | 启动验证 | `C:\Program Files\qemu\qemu-system-x86_64.exe` |

不需要 NASM（汇编一律 GAS `.S`，由 clang 集成汇编器编译）、不需要 GRUB
（multiboot1 由 QEMU `-kernel` 直接加载）。

## 2. 工具链引导（一次性）

MSYS2 自带的 `ld.lld` 是 MinGW/COFF 驱动，**不能链接 ELF**（实测
`ld.lld: unknown file type`、`-T` 参数不存在），GNU binutils 的 `ld` 只支持
`i386pe/i386pep` 仿真。因此从 GNU 源码自建 x86_64-elf binutils：

```bash
# 在 MSYS2 bash 中（注意：ftp.gnu.org 超时，用国内镜像）
MIRROR=https://mirrors.ustc.edu.cn/gnu/binutils   # 或 tuna/nju
curl -fsSL -o binutils.tar.xz "$MIRROR/binutils-2.44.tar.xz"
tar -xf binutils.tar.xz && cd binutils-2.44
./configure --build=x86_64-w64-mingw32 --host=x86_64-w64-mingw32 \
            --target=x86_64-elf --prefix=<安装目录> \
            --disable-gdb --disable-gdbserver --disable-gprofng \
            --disable-nls --disable-werror CFLAGS="-O2 -std=gnu17"
make -j$(nproc) && make install
```

两个坑（踩过）：
1. **必须显式 `--host/--build=x86_64-w64-mingw32`**：否则 autoconf 探测 host 为
   msys，libiberty 会去编 `pex-unix.c`，在 Windows 上因缺 `wait/fcntl/F_GETFD` 失败。
2. **`-std=gnu17`**：gcc 16 默认 C23，隐式函数声明变硬错误。

产物拷贝到项目 `tools\cross\bin\`（`build.ps1` 会按
`tools\cross\bin` → 临时目录 顺序查找 `x86_64-elf-ld.exe`）。

## 3. 构建

```powershell
cd C:\Users\fanqi\Desktop\缝合怪系统
.\scripts\build.ps1          # 增量编译 + 链接 → build\kernel.elf
.\scripts\build.ps1 -Clean   # 全量重建
```

编译参数要点：`--target=x86_64-unknown-none-elf -ffreestanding -mno-red-zone
-mno-sse -mno-mmx -mno-80387 -fno-pic -fno-stack-protector -nostdinc
-isystem <clang-resource>/include -Wall -Wextra`。

## 4. 静态排异检查（符号冲突）

```powershell
.\tests\symbol-scan.ps1       # nm 扫描全部 .o 的全局定义，重复即 FAIL
```

## 5. QEMU 动态验证

```powershell
.\scripts\run-qemu.ps1 -TimeoutSeconds 8
```

脚本行为：QEMU `-display none -serial file:build\serial.log -kernel build\kernel.elf`
启动 → 轮询串口日志直到出现 `[stitch] halted:` 或超时 → 杀掉 QEMU → 打印日志 →
判定标准：出现 `[stitch] boot chain verified` 且所有 `[selftest][x] PASS`
（任一 FAIL 或缺失即退出码 1）。

**判定逻辑在 run-qemu.ps1 内实现（机械化，不靠人眼）**，输出即
docs/verification-report.md 的数据源。

## 6. 手动观察

```powershell
# 直接看串口日志
Get-Content build\serial.log
# 反汇编入口检查
& 'C:\Users\fanqi\AppData\Local\Temp\xb64\install\bin\x86_64-elf-objdump.exe' -d build\kernel.elf | Select-Object -First 40
```

## 7. VMware 说明（可选）

QEMU `-kernel` 直通是主验证路径。若需 VMware 实测，需把内核做成可启动磁盘：
用 `x86_64-elf-objcopy -O binary` 取平坦镜像 + 自写 512 字节 multiboot/加载扇区
（路线图 Phase-2 的磁盘镜像任务），VMware 挂载该磁盘启动。当前阶段验证以
QEMU 为准（任务允许 QEMU **或** VMware）。
