## 🚀 本次更新 (v0.11.2 → v0.11.3)

补完 5 项清单里的 **第 4、5 项**：socket 系统调用家族 + DHCP/DNS 客户端 + 真正的 signal
语义（sigframe + 
t_sigreturn）。**启动呈现与 GUI 外观零变化。**

### ✨ 新增

- **socket 家族**：socket/connect/accept/sendto/recvfrom/sendmsg/recvmsg/shutdown/bind/
  listen/getsockname/getpeername/setsockopt/getsockopt，接在内核既有 UDP/TCP 实现上，
  带用户指针校验，socket fd 进入 fd 表
- **DHCP 客户端**（kernel/net/dhcp.c）：DISCOVER/REQUEST，解析 OFFER/ACK 后写入
  IP/掩码/网关/DNS；**只在 shell dhcp 命令时运行，不在开机阶段发起**
- **DNS 解析器**（kernel/net/dns.c）：A 记录查询与解析，
slookup <name> 触发
- **signal 真语义**：handler 前在用户栈构造 sigframe（完整上下文 + 返回跳板），
  
t_sigreturn 恢复全部寄存器与 rip/rsp/rflags；sigaction/sigprocmask 掩码生效；
  SIG_DFL 只终止该进程
- **ring-3 验收程序 sigtest**：装 SIGUSR1 handler → kill(getpid()) → 校验 →
  通过跳板返回 → 退出 0；
un sigtest 可跑

### ✅ 验证

- syscalls: 76 registered（v0.11.2 是 62，+14）
- 自检 13/14（moos_intr 既有判定）、
ing3 selftest 9/9、userspace selftest 9/9、
  [fat32] write self-test PASS、无 PANIC
- **启动日志行数 265 == 基线 265，零增删行**（只有地址/时间/计数等数据值不同）
- GUI 目录零改动；新增自检成功不打印

### 📦 下载

- 附件 sutureos.iso（13973504 字节）
- sha256：$issha

### ⚠️ 仍保留 -ENOSYS

- link(86)/symlink(88)：VFS 没有链接钩子（ramfs/FAT32 单父节点、ext2/SquashFS 只读）
- truncate(77)、wait4（无子进程记账）