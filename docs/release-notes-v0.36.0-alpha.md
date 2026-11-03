## 🚀 本次更新 (v0.35.0 → v0.36.0)

两个方向各进一步：**①系统升级（只做覆盖）**——装好的机器可以用新 ISO 覆盖系统文件升级，
用户数据（`/etc`、`/home`、`/root`、`/SUTUREOS/PERSIST`）一律保留；
**②Rust 系统层第一步**——`no_std` Rust 核心真的链接进内核并在开机自检里跑起来（为 GUI 重做的
Rust 技术栈打地基）。

---

### 📦 ① 系统升级：`upgrade`（只做覆盖）

在装好的系统上用新 ISO 启动，root 执行 `upgrade`：

| 步骤 | 内容 |
|---|---|
| 找安装介质 | 扫 ATAPI 光驱里的 ISO9660 卷（同 `install`） |
| 找装好的系统 | **按内容识别**：枚举磁盘 → 挂 FAT32 → 有 `/SUTUREOS/INSTALLED` 即目标盘；再从 MBR 读分区几何 |
| 覆盖系统文件 | `/kernel.elf`（新内核）、`/limine-bios.sys`（新 stage2）、`/limine.conf`（重新生成）；写完重刷 MBR + Limine stage 2 |
| 版本记录 | 记 `/SUTUREOS/VERSION`（`version=<新版本>`、`upgraded=1`） |
| **不碰的东西** | `/etc`、`/home`、`/root`、整个 `/SUTUREOS/PERSIST` —— 用户数据、账号、密码全部保留 |
| 写盘加速 | 复用安装器的 `fatfs_set_bulk_mode(1)`（否则 2.7 MB 内核重写会逐扇区回读校验，QEMU 上甚至会撞 ATA 超时） |

`kernel/install/installer.c` 新增 `installer_upgrade()`（约 200 行，复用安装器的 ISO 定位 / 拷贝 / 引导区写入），
shell 新增 `upgrade` 命令（要求 root，与 `install` 同权限）。

### ✅ 升级验收（四阶段，真 QEMU + 宿主独立解析）

| 阶段 | 证据 |
|---|---|
| 1. 用 **v0.35 ISO** 安装 | `[install] done (drive 0, partition LBA 2048, root_password=set, user=none)`，随后 `write /etc/motd PREUPGRADE-MARKER` + `sync`（写进 PERSIST）+ 正常关机 |
| 2. 用 **v0.36 ISO** 升级 | `[upgrade] installed system on drive 0 (partition LBA 2048..262143)` → `[upgrade] done (drive 0, system files overwritten, user data kept)` |
| 3. 宿主解析装好的盘 | `/KERNEL.ELF` 与 `build/kernel.elf` **逐字节相同**（2,707,032 B，sha `b3de65a8…`）；`PREUPGRADE-MARKER` 仍在卷上（用户数据未丢） |
| 4. **只挂该盘启动（无 ISO）** | banner 显示 `version 0.36.0-alpha` → `cat /etc/motd` 仍是升级前的 marker → `persist` 报卷状态 → 无 PANIC |
| 脚本 | `scripts/upgrade_acceptance.py` → **11/11 PASS** |

### 📦 ② Rust 系统层第一步（为 GUI 重做打地基）

| 项目 | 内容 |
|---|---|
| `rust/suture-core` | `#![no_std]` + `staticlib`，目标 `x86_64-unknown-none`，无依赖、无分配；导出 `suture_rust_selftest/expected/version`（C ABI） |
| `kernel/rust/probe.c` | C 侧声明 + **weak 回退**（工具链缺失时如实打 `[rust] core: not linked (C fallback)`）+ 开机探针 |
| `scripts/build.py` | 编译期调 rustup 的 cargo 构建静态库，并从 `libsuture_core.a` 里**手工提取 crate 对象**再当普通 .o 链接 |
| 为什么不用 `--whole-archive` | 会把 `compiler_builtins` 的 `memcpy/memset` 拉进来，与 `kernel/lib/string.c` 重复定义；而**GNU ld 不会用从未被拉取的归档成员覆盖 weak 定义**（`-u` 也无效），所以按 ar 格式（含 GNU 长名表 `//` + `/NNN`）提取那一个对象 |
| 开机自检 | `[rust] core v10000 linked, selftest=2171600cbe2b2b8c expected=2171600cbe2b2b8c PASS` |

### ⚠️ 已知限制（如实记录）

1. 升级是**覆盖**：不保留旧内核回滚（旧版本在 Git/Release 里，可以重新装）；不检查版本新旧；没有签名校验。
2. 升级只覆盖 `/kernel.elf`、`/limine-bios.sys`、`/limine.conf` 和引导区；`/etc` 里的模板不会更新（用户数据优先）。
3. 仅 BIOS/MBR + Limine（无 UEFI）；升级要求从 ISO 启动（不支持在线升级/网络升级）。
4. Rust 核心目前只是"链接+ABI+自检"验证，还没有替换任何 C 子系统；Slint/Smithay 尚未接入（见 GUI 重做计划）。
5. 既有 ATA 写路径在 QEMU 主机很忙时偶发 `ata write/flush timeout`，重跑即过（数据落盘正确、未改 ATA 驱动）。

### 📜 出处与许可（本版）

- 升级逻辑与 Rust 核心均为 SutureOS 自有实现（GPL-3.0）；Rust 依赖仅 `core`/`compiler_builtins`（Rust 工具链自带，MIT/Apache-2.0 双授权），没有引入第三方运行时。

### 📦 下载

- 附件 `sutureos.iso`（16021504 字节）
- sha256：`211483983755676a6493920c4d8a2bd02d9015183ecef7e806b69e64df65b46d`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.36.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.36.0-alpha
