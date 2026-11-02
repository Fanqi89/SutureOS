## 🚀 本次更新 (v0.21.0 → v0.22.0)

**SutureOS 第一次能把网上的文件下载到本地盘：`wget http://<ip>[:port]/<path> [outfile]`。
从真实 HTTP 服务器取回 3 KiB 二进制与文本文件，宿主侧解析 FAT32 镜像逐字节比对相同。
启动呈现与 GUI 外观不变。**

---

### 📦 新增：`wget`（HTTP/1.0 GET over 内核 TCP 客户端）

| 项目 | 内容 |
|---|---|
| 用法 | `wget http://10.0.2.2:8089/dl.bin /mnt/fat/DL.BIN`（省略出参时按 URL 最后一段落到当前目录） |
| 落点 | `kernel/shell/shell.c`：URL 解析 + 请求组装 + 状态行/响应头解析 + 边收边写 VFS + Content-Length 校验 |
| 依赖 | 已存在的 `tcp_connect/tcp_send/tcp_recv/tcp_close`（内核 TCP 客户端）与 VFS 写路径，**没有新增协议代码** |
| 行为 | `HTTP 200` 才落盘；状态非 200 直接报错不写文件；`Content-Length` 存在时校验收到的字节数，短读会 WARNING |

- 只接受 **IPv4 字面量**（DNS 解析是独立能力，下载工具不隐式依赖它；文档里写明）。
- `Host:`、`User-Agent: SutureOS-wget/1.0`、`Connection: close` 三个头都发；响应头最多缓存 2 KiB。
- 边收边写（1460 字节一批），因此文件大小不受 shell 缓冲区限制。

### ✅ 验收（真实 HTTP 服务器 + 真实 QEMU + 宿主逐字节）

| 项目 | 结果 |
|---|---|
| 下载二进制 | guest `wget http://10.0.2.2:8089/dl.bin /mnt/fat/DL.BIN` → `HTTP 200, content-length=3072` → `wrote 3072 bytes` |
| 下载文本 | `hello.txt` → 落盘 + `cat /mnt/fat/HELLO2.TXT` 打印 `hello from the host via wget` |
| **宿主验证** | 关 QEMU 后宿主解析 `build/fat32_ata.img`：`DL.BIN` 与服务器上的 3072 字节 **sha256 完全相同**（a870a52a6121f125…），`HELLO2.TXT` 逐字节相同 |
| 脚本 | `scripts/wget_acceptance.py`（宿主起 `ThreadingHTTPServer`，QEMU slirp 把 guest 的 10.0.2.2 转宿主 loopback，QMP 注入命令，事后宿主解析镜像）→ **8/8 PASS** |
| 回归 | 内核自检 13/14、FAT32 三自检 PASS、终端中文 10/10、GUI 中文 4/4、无 PANIC、外观门禁 0 像素 |

### 🧪 新增脚本

- `scripts/wget_acceptance.py`：宿主 HTTP 服务 + QMP 注入 + **宿主侧 FAT32 解析逐字节比对**（不信任客人输出）。

### ⚠️ 已知限制（如实记录）

1. **只支持 IPv4 字面量**：`wget http://10.0.2.2/...`；`http://example.com/...` 会明确报 `bad url`
   （内核有 `nslookup`，但下载工具不隐式依赖解析器）。
2. 只有 **HTTP/1.0 GET**：不支持 HTTPS、POST、重定向跟随（3xx 会如实报 `server returned 3xx`）、
   分块传输（`Transfer-Encoding: chunked` 会按关闭连接处理）、断点续传、代理。
3. 响应头超过 2 KiB 会被截断并可能解析失败；`Content-Length` 缺失时以对端关闭为结束。
4. 下载大文件时进度不可见（只有完成后的 `wrote N bytes`）；超时 5 秒。
5. USB 存储（EHCI）正在另一个分支开发，本版不含。

### 📜 出处与许可（本版）

- `wget` 是 SutureOS 自有实现（GPL-3.0），复用内核已有的 TCP 客户端与 VFS，没有移植外部 HTTP 代码。

### 📦 下载

- 附件 `sutureos.iso`（15349760 字节）
- sha256：`bec90f0104f85e6e6aa53f476051f1280f5cb96450e1864eaf16ae68b8d88b44`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.22.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.22.0-alpha
