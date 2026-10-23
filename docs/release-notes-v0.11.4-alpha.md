## 🚀 本次更新 (v0.11.3 → v0.11.4)

**真正的多进程**：fork（写时复制）+ wait4 回收 + getppid。启动呈现与 GUI 外观零变化。

### ✨ 新增

- **进程表 + 协作式调度**（kernel/syscall/process.c）：进程 = {userspace_t + 16 KiB 内核栈 + 预构建 user_context_t}；切换 push rbx/rbp/r12-r15 → swap rsp → pop → ret，再切 CR3 与 %gs:8
- **fork + COW**：PTE 软件位 9 标记写时复制，父子共享物理页并置只读；缺页按"物理帧 → 引用计数"表决定复制还是仅恢复写权限
- **wait4(61) / getppid(110)**：子进程退出成僵尸，父进程回收并拿到 status=sig<<8|code；无子进程 -ECHILD；支持 WNOHANG；fd 表与 signal 槽随 fork 克隆
- **forktest 验收程序** + ring-3 shell 的 ork 命令；新增 clone/vfork/setpgid/getpgrp/getsid 注册 → **81 个系统调用**

### 🔧 关键修复

- 子进程经 int 0x80 退出会让 GS MSR 对留在用户侧，父进程下一次 syscall 从线性地址 8 取内核栈导致整机停止打印；现在每次进程切换都归一化 GS MSR
- 删除 mmu.c 中无引用计数的旧 COW 辅助函数（双重释放风险）

### ✅ 验证

- 干净重建 build=0；启动日志 **265 行 == 基线**、syscalls: 81 registered、自检 13/14、ing3 9/9、userspace 9/9、FAT32 写+ops PASS、无 PANIC
- un forktest：子进程 pid=2、COW 成立（父仍读到 'Q'）、wait4 -> pid=2 status=0x2a、-ECHILD、RESULT PASS 退出 0
- ushell 的 ork 命令同样 PASS；un sigtest 回归 PASS

### 📦 下载

- 附件 sutureos.iso（14059520 字节）
- sha256：$issha

### ⚠️ 限制

协作式调度（无抢占，自旋子进程会饿死兄弟）；一次一个会话；fork 只复制 16 页用户窗口（mmap/brk 不继承）；clone(CLONE_VM)/线程 -EINVAL；wait4 status 编码为 sig<<8|code。