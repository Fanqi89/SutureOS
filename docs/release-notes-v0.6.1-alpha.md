## 🚀 本次更新 (v0.6.0 → v0.6.1)

一个 PATCH 版本：修掉「About 窗口写死版本号」这个让 ISO 里混进旧版本字符串的 bug，
并把发布自证流程做成脚本——以后每个版本都要过这套检查。

### 🛠 关键修复

- **About 窗口的版本号是写死的 `v0.5.0-alpha`**：发版只改 `include/sutureos/version.h` 时，
  About 里仍打印旧版本，于是**新 ISO 里同时存在旧版本串**。新的发布校验脚本一上来就抓到：
  ISO 内 `0.5.0-alpha` 命中 2 次、`0.6.1-alpha` 命中 0 次。
  修法：`about_draw()` 改为用 `SUTUREOS_VERSION` 运行时格式化（静态缓冲只算一次）。
  修复后实测：ISO 内 `0.6.1-alpha` 命中 3 次、`0.6.0-alpha` 命中 0 次。

### ✨ 新增（发布工程）

- `scripts/verify_release.ps1` —— 发布自证：
  1. 版本串真的在 ISO 里、旧串已消失；
  2. 本地 ISO 的 `size` + `sha256`；
  3. 回读 Release 附件的 `size` + `digest`（**按附件名匹配**，API 返回顺序不稳定）；
  4. `-DownloadBack` 用 256 KB 分块把附件拉回来再算 sha256（本地 == API digest == 下载回读）；
  5. **所有旧 Release** 与「发布前快照」逐项比对（tag / 附件名 / id / size / digest），
     防止上传把旧版本踩掉；
  6. git 侧：`HEAD == origin/main`、`origin/main...HEAD` 为 `0 0`、工作树干净。
- `scripts/publish_all.ps1` —— **一条命令**完成整套发布：快照旧 Release → 提交 + 附注标签 + 推送
  → 建/更新 Release 并上传 ISO → 自动跑校验，任一步失败即中止。
- `docs/RELEASING.md` 增加「发布自证」一节。

### ✅ 本次验证

- ISO 内版本串双向核对：`0.6.1-alpha` = 3 次、`0.6.0-alpha` = 0 次
- 附件回读：`size` 13,182,976 一致，`digest` 与本地 `sha256` 一致
- 旧 Release（v0.1.0-alpha、v0.5.0-alpha）的附件与 digest 与发布前快照一致
- git：`HEAD == origin/main`、无分叉、工作树干净
- 内核回归不变：自检 13/14、FAT32 写 + 目录操作自检 PASS、SMP 双核上线、RTL8139 ping 通

### 📦 下载与体验

- 附件 `sutureos.iso`（12.57 MiB，13,182,976 字节）
- sha256：`9ca867fd59fcbbf52ec3d84ef8da2bc8dfcf75b773dbfa16005e809c635bcea7`
- QEMU 快速体验：
  ```
  qemu-system-x86_64 -cdrom sutureos.iso -m 512 -smp 2 \
      -device rtl8139,netdev=n0 -netdev user,id=n0
  ```
- 进系统后：`startgui` 进桌面，About 窗口现在显示正确版本号

### ⚠️ 已知限制

- 本机代理拉不动 12.5 MB 整包回读（实测 256 KB range = 3.7 s、1 MB = 52 s、4 MB 超时），
  所以默认以 API `digest` 为权威证明，需要三向校验时加 `-DownloadBack`（慢）
- 功能限制同 v0.6.0-alpha：VMware 默认 e1000 无驱动（无网络）、真实鼠标移动未自动化验证、
  TCP 无 SACK/窗口缩放/拥塞控制、FAT32 仅 8.3 短名、VimtuFS2 未实现
