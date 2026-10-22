# 发布规范（RELEASING）

这份文档规定 SutureOS 的版本号、标签内容、Release 与 ISO 产物的做法。目标：
**每个版本都能从仓库里查到"改了什么"，也能直接下载到当时的 ISO。**

## 1. 版本号

- 遵循 **SemVer 2.0.0**：`MAJOR.MINOR.PATCH[-PRE]`，例如 `0.6.0-alpha`。
- 唯一来源：[`include/sutureos/version.h`](../include/sutureos/version.h)。
  内核开机横幅、shell 欢迎语、`/etc/motd`、ISO 里的 `system/manifest.json`
  全部从这个头文件取值（`scripts/build_iso.py` 直接解析它），**发版只改这一处**。
- 递增规则：
  - `MAJOR`：不兼容的大改（目前仍是 `0`，alpha 阶段）
  - `MINOR`：成组的新能力（新驱动、新文件系统、新子系统）
  - `PATCH`：修 bug、文档、工具链
  - `PRE`：`alpha` / `beta` / `rc`，可带序号（`alpha.1`、`beta.2`）

## 2. 标签（tag）

- 类型：**附注标签**（annotated，不是轻量标签），名字 `v<版本号>`，与版本号完全一致。
- 旧的标签**永不移动、不删除**——新旧版本在仓库里共存，`git tag -l` 就能看到完整版本史。
- 标签说明（tag message）就是"这个版本做了什么"，用下面这个结构：

```
SutureOS v<版本号> —— <中文一句话主题> (<English descriptor>)

一句话：<这个版本最重要的 2-3 件事>

■ 关键修复
- <问题现象 → 根因 → 怎么修>（一条一件事，写清"为什么"）

■ 新增
- <新能力 + 验收数字（7/7、138/138 之类）>

■ 验证
- <自检结果 / 宿主验收 / 真机或虚拟机实测>

■ 产物（发布页附件）
- <ISO 文件名>  <字节数>
- sha256  <哈希>
- 下载  <直链>
- 发布页 <链接>

■ 已知限制
- <这一版明确没做的事 / 未验证的地方>
```

`■ 产物` 这一段是必须的：ISO 挂在 GitHub Release 上，标签里写清文件名、大小和
sha256，任何人拿到标签就能核对下载到的文件。

## 3. GitHub Release

- 每个标签对应一个 Release，标题格式：
  `SutureOS v<版本号>: <中文概括> (<English descriptor>)`
  例：`SutureOS v0.6.0-alpha: 真机兼容与桌面可用 (Hypervisor & Desktop Update)`
- 正文用 `docs/release-notes-v<版本号>.md` 提供，结构固定：
  - `## 🚀 本次更新 (上一版 → 本版)`
  - `### 🛠 关键修复` / `### ✨ 新增能力` / `### ✅ 本次验证`
  - `### 📦 下载与体验`（含 sha256 与启动命令）
  - `### ⚠️ 已知限制`
- **附件固定叫 `sutureos.iso`**（与 v0.1/v0.5 的历史 Release 保持一致），这样下载
  链接永远是 `releases/download/v<版本>/sutureos.iso`。

## 4. 一次发布的完整流程

```powershell
# 1) 改代码，并把版本号写进唯一的头文件
#    include/sutureos/version.h  ->  SUTUREOS_VERSION_MINOR / PATCH / PRE

# 2) 构建 + 跑验收
py -3 scripts\build.py
py -3 scripts\build_iso.py

# 3) 写这一版的说明（照着第 2 节的结构）
#    docs\release-notes-v<版本号>.md

# 4) 提交 + 打附注标签 + 推送 main 和 tags
powershell -NoProfile -File scripts\release.ps1 `
    -Message "feat: ..." -NotesFile <同一份说明或标签正文文件>

# 5) 创建/更新 Release 并上传 ISO（同一份说明文件）
powershell -NoProfile -File scripts\publish_release.ps1 `
    -Name "SutureOS v<版本号>: <中文概括> (<English descriptor>)" `
    -NotesFile docs\release-notes-v<版本号>.md
```

`release.ps1` 与 `publish_release.ps1` 都会自动：

- 从 `version.h` 读版本号，不需要手写标签名；
- 从 `$env:GITHUB_TOKEN` 或 git 凭据管理器取 token；
- 从 Windows 系统代理取代理（本机直连 GitHub 的 TLS 会被重置，必须走代理）；
- 幂等：标签已存在就不动它，Release 已存在就更新标题/正文，同名附件先删再传。

## 5. 备份与历史 Release

- 每个 Release 都留着历史 ISO（GitHub 不会覆盖别版本的文件），所以
  `v0.1.0-alpha`、`v0.5.0-alpha`、`v0.6.0-alpha` 的镜像都能分别下载。
- 不把 ISO 提交进 git：12 MB 级别的二进制会永久留在历史里，Release 附件才是正确位置。
