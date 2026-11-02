## 🚀 本次更新 (v0.26.0 → v0.27.0)

**Intel HDA 音频（B9）：QEMU 里真实出声。控制器探测 → CORB/RIRB 发 codec verb → 枚举 AFG/widget →
找"输出引脚→转换器"路径 → 环形 BDL 播放 440 Hz 正弦 1 秒；宿主侧解析录到的 WAV，证明
48000 Hz / 2ch / 16bit / 1.002 s / 0.5000 FS / 440.01 Hz。启动呈现与 GUI 外观不变。**

---

### 📦 新增：Intel HDA（High Definition Audio）

| 项目 | 内容 |
|---|---|
| 文件 | `kernel/drivers/hda.c`（1236 行，新）+ `hda.h`（117 行，新）+ `scripts/verify_hda_wav.py`（274 行，新）；`kmain.c` 在 AC'97 之后接入；`shell.c` 新增 `hdatest [freq] [ms]` |
| 来源 | 骨架 **PlantOS（MIT）`src/kernel/drivers/sound/hda.c`**；QEMU/规范细节参考 ReliefOS 的 HDA 设计文档（只读参考，未拷代码）。SutureOS 侧改动：PCI/MMIO 映射、PIT 轮询、codec 路径 DFS、环形 BDL、正弦合成、自检与宿主验收脚本 |
| 能力 | PCI 探测（class 0x04/0x03、8086:2668）→ BAR0 MMIO（`mmu_map_mmio_identity`）→ GCTL CRST 复位 → CORB/RIRB DMA verb → codec/AFG/widget 枚举 → 输出引脚到转换器的路径 DFS（支持 mixer/selector 跳）→ 0 dB amp + EAPD + PIN_OUT_ENABLE → SD0 环形 BDL 播放 + LPIB 轮询 → 整数正弦合成 |
| 日志判据 | `[hda] controller ... codec mask=0x1 gcap=0x4401` / `codec vendor=0x1af4 device=0x22` / `converter fmt=0x11 stream=1 ch=0 dac=2 pin=3` / `beep 440Hz 1000ms` / `selftest: PASS played 48000 frames @48000 Hz` |

### ✅ 验收（真实 QEMU 11.1 + 宿主 WAV 校验）

| 项目 | 结果 |
|---|---|
| 无 HDA 启动 | 与基线相比模板差异**只有一行** `[hda] no controller`；内核自检 13/14、FAT32 四自检 PASS、宿主 21/21、GUI 外观门禁 **0 像素** |
| 有 HDA 启动 | `-audiodev wav,id=a0,path=build\hda.wav,out.frequency=48000 -device intel-hda -device hda-duplex,audiodev=a0` → 逐行判据见上，`selftest: PASS` |
| **宿主 WAV** | `scripts/verify_hda_wav.py build/hda.wav` → **6/6 PASS**：48000 Hz、2 声道、16 bit、音段 1.002 s、峰值 0.5000 FS、440.01 Hz（441 次过零） |
| shell | `hdatest` 实测走同一条播放路径 |
| 构建 | 警告 45 条 = 基线 45 条（零新增） |

### 🧠 QEMU 实测踩到的四个硬坑（已写进代码注释）

1. **CORB 第一条命令必须写到索引 1**：复位后 CORBWP=0，写回 0 不会产生新命令。
2. **RINTCNT 是"每批响应数"**：QEMU 在 `rirb_count == rirb_cnt` 时停止消费 CORB；必须 `RINTCNT=1`
   + 打开 RIRB 响应状态位 + **每收一条响应 W1C 清 RIRBSTS**，否则只回第一条 verb。
3. **SD_CTL 的 RUN/DMA start 是 bit1(0x02)**，不是 bit2；stream tag 在 bits23:20。
4. `GET_PARAMETER` id：widget cap=0x09、PCM=0x0a、PIN_CAP=0x0c、CONNLIST_LEN=0x0e、OUT_AMP_CAP=0x12；
   输出 SD0 基址 = 0x100。

### ⚠️ 已知限制（如实记录，别当已验）

1. **只在 QEMU 11.1 的 `intel-hda` + `hda-duplex` 上验证，真机未测**。
2. 单 codec、单模拟输出 pin、其路径上的第一个转换器；**无采集（录音）**、无音量/混音控件
   （amp 统一 0 dB）、无中断（纯轮询）。
3. 固定 48 kHz / 16-bit / 立体声，其它采样率如实拒绝；单次播放 ≤ 1.0 s。
4. QEMU codec 用虚拟时钟拉数据，guest 轮询会让最后约 7.7 KB 滞留在 codec FIFO：驱动在 LPIB 环回后
   清 staging 并保持 stream 运行 600 ms（drain 窗口），所以 WAV 是"1.000 s 音 + 静音尾巴"
   （总约 1.26 s），校验按**音段**判定（代码与脚本都有注释）。
5. 有 HDA 时开机多约 1–2 s（放音 + drain）。

### 📜 出处与许可（本版）

- `kernel/drivers/hda.c`：骨架 **PlantOS（MIT）**，SutureOS 侧改动逐条写在文件头；MIT 可并入
  GPL-3.0 项目。ReliefOS 资料只作寄存器/流程参考，未拷贝代码（许可证已记录在
  `docs/licensing-audit.md`）。

### 📦 下载

- 附件 `sutureos.iso`（15603712 字节）
- sha256：`204ca45966384d7ca063c6e5af9e836571bc936ece5d636d19108511e2ca5fce`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.27.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.27.0-alpha
