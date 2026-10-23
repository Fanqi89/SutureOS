## 🚀 本次更新 (v0.11.1 → v0.11.2)

补完 5 项未完成系统调用中的**前 3 项**，启动呈现与 GUI 外观零变化。

### ✨ 新增

- **dup/dup2/dup3（32/33/292）**：fd 表新增复制条目能力，管道端点复制正确维护
  读写引用计数（EOF 语义不变），不再返回 -ENOSYS
- **creat（85）** = open(O_CREAT|O_WRONLY|O_TRUNC)
- **openat/mkdirat/unlinkat/enameat/statat（257/258/263/264/262）**：
  AT_FDCWD 走当前目录，真实 dirfd 经 fd 表解析后拼相对路径
- **statx（332）**：填充基础统计（mask=STATX_BASIC_STATS）
- **最小 tty 层**（kernel/vfs/tty.c）：fd 0/1/2 支持 TCGETS/TCSETS*/TIOCGWINSZ/TCFLSH
  的 ioctl；其余仍 -ENOTTY

### ✅ 验证

- 启动日志：syscalls: 62 registered（v0.11.1 为 56，+6）
- 回归：自检 13/14（moos_intr 既有判定）、ing3 selftest 9/9、userspace selftest 9/9、无 PANIC
- 新增自检成功时不打印（保持启动日志一致），失败才打印

### 📦 下载

- 附件 sutureos.iso（13817856 字节）
- sha256：$sha

### ⏭ 未完成（下一批）

- **socket 家族 + DHCP/DNS 客户端**
- **signal 真语义**（sigframe 入用户栈 + t_sigreturn 真实恢复）
- 用户态 FB_MAP/INPUT_POLL/SHM_*（号段已让开，内核未实现）