# OverlayFS 只读根文件系统设计

对应 `docs/M7系统化部署设计.md` 第 1 节（存储防坏卡）与配置持久化 helper 在只读环境下的闭环。

本文是实施前的设计文档，**不是操作手册**。第 7 节的实施步骤必须在满足第 6 节现场条件的前提下执行。

## 1. 要解决的问题

现场设备直接断电是常态操作，不会有人先执行 `shutdown`。ext4 根文件系统在写入过程中掉电可能损坏，轻则触发 fsck 修复，重则系统起不来。目标是让根文件系统在运行期间不接受任何写入，日常写入全部落在内存里，断电时 SD 卡上没有正在进行的写操作。

## 2. 现场实测环境

以下为 2026-07-20 在 `dcdw@192.168.11.4` 实测确认，不是推测：

| 项 | 实测值 |
|---|---|
| 系统 | Debian GNU/Linux 13 (trixie)，Raspberry Pi 5 |
| 根文件系统 | `/dev/mmcblk0p2` ext4 `rw,noatime`，29G 总量/已用 7.6G |
| 启动分区 | `/dev/mmcblk0p1` vfat，挂载于 `/boot/firmware`，505M/已用 87M |
| 内存 | 4049 MiB |
| 活动 swap | 仅 `zram0`（2G，RAM 支撑） |
| `dphys-swapfile` | 已禁用（`not-found`） |
| 遗留文件 | `/var/swap` 2.0G，**未激活** |
| `raspi-config` | 已安装，含 `enable_overlayfs` / `disable_overlayfs` / `enable_bootro` / `disable_bootro` |
| `overlayroot` 包 | **未安装**，apt 中有 `0.18.debian14` 可用 |

注：收尾工作文档中「Raspberry Pi OS 26.04」的说法与实测不符，实际是 Debian 13 (trixie) 基础的 Raspberry Pi OS，实施时以实测为准。

## 3. 实现机制

Raspberry Pi OS 的 `raspi-config` 不自研 overlay 逻辑，而是依赖 Debian 的 `overlayroot` 包。`enable_overlayfs` 的实际动作是：

1. 若未安装则 `apt-get install -y overlayroot`；
2. 把 `/boot/firmware` 临时挂为可写；
3. 向 `/boot/firmware/cmdline.txt` 前置 `overlayroot=tmpfs`；
4. 按原状态决定是否把 `/boot/firmware` 恢复为只读。

当前 `cmdline.txt` 内容（实测）：

```
console=serial0,115200 console=tty1 root=PARTUUID=e59e7ed8-02 rootfstype=ext4 fsck.repair=yes rootwait quiet splash plymouth.ignore-serial-consoles cfg80211.ieee80211_regdom=CN
```

**回退生命线**：触发开关只是 `cmdline.txt` 里的一个字符串，而该文件位于独立的 vfat 分区。即使系统完全起不来，把 SD 卡插到任意一台电脑上删掉 `overlayroot=tmpfs` 即可恢复。这是本方案可控性的基础，实施前必须先备份该文件。

## 4. 三层只读策略

| 层 | 策略 | 理由 |
|---|---|---|
| 根文件系统 `/` | overlay 只读，写入落 tmpfs | 主要防护目标 |
| `/boot/firmware` | **第一阶段保持可写**，稳定后再单独启用 `enable_bootro` | 它是回退生命线，过早锁死会让远程恢复更困难；且启动分区平时不写，风险本就低 |
| 应用配置 `config/config.json` | 需要专门的持久化通道，见第 5 节 | 这是本方案最容易被忽略的坑 |

日志侧的前置条件**已经完成**（2026-07-20）：journald 已 `Storage=volatile` + `RuntimeMaxUse=16M`；业务日志 `logging.file` 为空字符串，只走 journald，不产生落盘日志文件。因此 overlay 启用后没有遗留的日志写入路径需要处理。

## 5. 配置持久化：本方案的核心难点

`config/config.json` 位于 `/home/dcdw/cns_rpi/config/`，在根文件系统上。**overlay 启用后，helper 写入的新配置会落进 tmpfs 上层，重启即消失** —— 表现为「配置命令返回成功 ACK，但设备重启后参数回退」，比不做 overlay 更糟，因为它是静默失败。

现有 helper（`/usr/local/libexec/cns-rpi-apply-config`，87 行）的写入逻辑本身质量很好，实测包含：临时文件暂存 → `os.fsync(文件)` → `os.replace` 原子替换 → `os.fsync(目录)`。**这套逻辑不需要改**，需要改的只是「写到哪一层」。

### 方案对比

| 方案 | 做法 | 结论 |
|---|---|---|
| **A. helper 直接 remount 底层并原子写入** | 读 overlay 视图里的候选 → remount lowerdir 可写 → 在底层原子替换 → 恢复只读 | **已采用**，见下 |
| A′. helper 经 `overlayroot-chroot` 写底层 | 先 chroot 进 lowerdir 再复用现有逻辑 | **否决**，原因见下 |
| B. 配置移到 `/boot/firmware` | vfat 分区不参与 overlay，天然持久 | 否决。vfat 无属主/权限语义，掉电一致性弱于 ext4，与「防坏卡」初衷相悖 |
| C. 单独划持久化分区 | 最干净的工程解 | 需要对 29G 卡重新分区，远程执行风险过高。若将来有机会离线操作（可插读卡器），值得回头采用 |

### ⚠️ 方案 A 已被实测推翻（2026-07-20）

overlay 启用后实测发现，方案 A 的"恢复只读"这一步**在 overlayroot 架构下无法完成**：

```
mount: /media/root-ro: mount point is busy
```

排查结论（均为实测）：

- `fuser -vm /media/root-ro` 只显示 `kernel mount`，没有任何用户进程占用；
- 没有 deleted-but-open 的 inode（`(deleted)` 条目全是 tmpfs 上的 memfd）；
- **停掉主服务后依然失败**，与业务进程无关；
- 上游 `overlayroot-chroot` 自身也有同样限制——其代码中 `mount -o remount,ro` 失败时仅打印 `Note that [$mp] is still mounted read/write` 后继续。

根因是 overlayfs 正把 `/media/root-ro` 作为 lowerdir 挂载，内核不允许在此期间将其翻回只读。因此"remount rw → 写 → remount ro"用在**根分区的 lowerdir 上**是走不通的：写入本身成功，但文件系统会停在可写状态，OverlayFS 的断电保护失效——这比不做更危险，因为它看起来是成功的。

### 已验证可行的替代：配置放在独立文件系统上

关键区别在于**是不是同一个超级块**。独立于 overlay 的文件系统读写状态完全自主。实测验证（loop 挂载的 ext4 镜像，overlay 同时处于挂载状态）：

| 步骤 | 结果 |
|---|---|
| 平时以 `ro` 挂载 | 写入被拒：`Read-only file system` |
| `remount,rw` | 成功 |
| 写入 + `sync` | 成功 |
| `remount,ro` | **成功**——这正是 `/media/root-ro` 做不到的 |
| overlay 状态 | 全程保持 `/ overlay` 正常 |

同样的独立性在 `/boot/firmware` 上也已验证：它是独立 vfat 分区，可自由 `remount,ro` / `remount,rw`。

**方案**：在 `/boot/firmware` 上放一个固定大小的 ext4 镜像文件，loop 挂载到配置目录，平时 `ro`，写配置时短暂切 `rw`。

为什么套一层 ext4 镜像而不直接把 `config.json` 放在 vfat 上：vfat 无法表达 `dcdw:dcdw 0600` 的属主与权限（主程序以 dcdw 运行且该文件是 0600），也缺少日志与可靠的 fsync 语义。镜像内是 ext4，原子 rename、fsync 与权限语义全部保留；镜像文件大小固定，写入只改数据块、不改 vfat 元数据。

### 为什么否决 `overlayroot-chroot`

本文初稿曾推荐 A′，实际推演后发现它不成立：

> 主程序生成的候选文件 `.config.json.tmp.XXXX` 落在 overlay 的**内存上层**。`overlayroot-chroot` 是 chroot 进 `lowerdir`，在那个视图里上层的候选文件根本不存在，helper 拿不到要写的内容。

因此回到 `docs/M7系统化部署设计.md` 第 1 节原本就写对的思路：remount → 写 → sync → 恢复只读，不使用 chroot。

### 已验证的机制细节

`overlayroot` 包已下载解包检查（未安装，零系统改动），确认：

- 包内默认 `/etc/overlayroot.conf` 为 `overlayroot=""`、`overlayroot_cfgdisk="disabled"` —— **安装本身不会启用 overlay**，启用只由 `cmdline.txt` 的 `overlayroot=tmpfs` 触发，装包步骤因此是低风险的；
- `overlayroot-chroot` 从 `/proc/mounts` 中匹配根挂载点上的 overlay 条目、取 `lowerdir=` 到下一个逗号为止，并要求该路径本身是挂载点。helper 的解析逻辑与之保持一致；
- initramfs 机制可用（`auto_initramfs=1`，`/boot/firmware/initramfs_2712` 存在，`initramfs-tools` 已安装），overlayroot 依赖的 initramfs 钩子有效，不构成阻断。

### helper 的实现要点（已实施）

`scripts/cns-rpi-apply-config.py` 已支持只读环境，非 overlay 环境下行为完全不变：

- `detect_overlay_lowerdir()` 解析持久层；解析不出挂载点时**抛错而不是退化为写内存层**，避免静默丢配置；
- 路径校验（候选必须在固定目录、文件名必须匹配固定模式、目标必须等于固定路径）**在切换到持久层之前完成**，持久化映射不构成绕过校验的旁路；
- 暂存文件建在持久层目录内，保证 `os.replace` 与目标同文件系统、rename 具备原子性；
- 写入后 `fsync` 文件与目录，再 `os.sync()`，然后才恢复只读——现场是直接断电，不能依赖延迟回写；
- 恢复只读放在 `finally`，写入失败也不会把设备停在可写状态。

配置只写 lowerdir、不写 overlay 上层，因此上层不会产生遮蔽 `config.json` 的副本，底层新内容对当前运行的系统立即可见，不必等重启。

### 持久化闭环的验收标准

沿用收尾工作文档 §8.2 的要求：

- helper 只能修改固定的 `config/config.json`，不得获得任意 shell 或任意文件写权限；
- 配置成功应用后，**断电重新上电仍保留新参数**（这是与当前状态的关键区别）；
- 写入任何阶段失败时，旧配置仍完整可用；
- helper 返回结果不确定时，设备端幂等记录能避免重复破坏配置。

## 6. 实施前必须满足的现场条件

**这一步不能纯远程做。** 启用 overlay 需要修改 `cmdline.txt` 并重启，一旦启动失败，SSH 立即失联，只能靠物理接触恢复。

- 实施时必须有人在设备旁，或至少能随时取下 SD 卡；
- 先备份 `/boot/firmware/cmdline.txt`（同时在设备外留一份副本）；
- 预留一台能读 SD 卡的电脑。

### 实施前的清理项

`/var/swap` 有 2.0G 遗留 swap 文件，当前未激活且 `dphys-swapfile` 已禁用。**启用 overlay 前应删除该文件并确认服务保持 disabled**：overlay 下若 swap 文件被误激活，swap 会写进内存上层，形成自噬式内存耗尽。当前活动 swap 只有 `zram0`（RAM 支撑），在 overlay 下是安全的，无需改动。

## 7. 实施步骤

分两次重启，不要合并。

**第一次 —— 只装包，不改 cmdline：**

```bash
sudo cp /boot/firmware/cmdline.txt /boot/firmware/cmdline.txt.bak
sudo apt-get install -y overlayroot
sudo rm -f /var/swap
sudo reboot
```

重启后确认系统正常、SSH 可用、`cns-rpi.service` 为 `active`。此时尚未启用 overlay，风险为零。

**第二次 —— 启用 overlay：**

```bash
sudo raspi-config nonint enable_overlayfs
sudo reboot
```

**第三次（可选，稳定运行一段时间后再做）—— 锁只读启动分区：**

```bash
sudo raspi-config nonint enable_bootro
sudo reboot
```

## 8. 重启后的验证

必须验证「实际挂载为只读」，而不是「配置文件改了」——这是收尾工作文档特别强调的一点。

```bash
findmnt -n -o TARGET,SOURCE,FSTYPE,OPTIONS /      # 期望 fstype 为 overlay
touch /tmp_write_probe 2>&1                        # 期望落在内存层，重启后消失
grep -o 'overlayroot=[a-z]*' /proc/cmdline         # 确认内核参数已生效
systemctl is-active cns-rpi.service                # 主服务不受影响
```

判据：`findmnt` 显示根挂载类型为 `overlay`（而非改动前的 `ext4 rw,noatime`）。仅检查 `cmdline.txt` 内容不构成验证。

## 9. 内存写层容量与耗尽行为

overlay 上层是 tmpfs，占用的是那 4049 MiB 物理内存。当前可用内存约 3.5 GiB，而运行期写入主要来自：

- journald：已硬限 16 MiB；
- 业务日志：不落盘（`logging.file` 为空）；
- 临时文件与运行时状态：量级很小。

因此正常运行不会接近上限。但必须明确耗尽后的行为：**tmpfs 写满后写入返回 `ENOSPC`，不会自动淘汰旧数据**（这与 journald 的 `RuntimeMaxUse` 滚动淘汰不同——后者是 journald 自己实现的策略，不是 tmpfs 的行为）。

实施时应实测并记录：给 overlay 上层设定显式容量上限，以及写满后主程序的表现。这一项在本设计中**尚未验证**，不得假定「反正内存够大」。

## 10. 与后续验收的关系

本方案完成后才能开展收尾工作文档 §8.5 的物理断电验收，且断电测试必须覆盖「修改配置后立即断电」这一最危险时序，重复多轮（建议不少于 5 轮），不能只成功一次即通过。

## 11. 权限模型的现状偏差

`docs/M7系统化部署设计.md` 第 1 节设计的是"业务程序通过极窄 sudoers 规则调用 helper，本身不持有 remount 权限"。实测发现该前提当前**并不成立**：

```
User dcdw may run the following commands on raspberrypi:
    (ALL : ALL) ALL
    (ALL) NOPASSWD: ALL
```

`dcdw` 已由 `/etc/sudoers.d/dcdw-nopasswd` 授予完整免密 sudo，而主服务以 `User=dcdw` 运行。因此在收掉这条全局规则之前，"helper 权限最小化"只是纸面约束。收窄它会影响 `scripts/deploy.sh`（依赖免密 sudo），属于独立的加固项，需单独安排。

实施时有一个必须避免的陷阱：**绝不能把 `sudo overlayroot-chroot *` 或 `sudo mount *` 写进 sudoers**——两者都等价于直接授予 root。若将来收窄权限，应只放行固定入口、固定参数的专用脚本。

## 12. 当前状态

**OverlayFS 已启用**（2026-07-20）。实测确认：

- 根挂载为 `overlayroot / overlay`，`lowerdir=/media/root-ro`（`/dev/mmcblk0p2` ext4 只读）、`upperdir=/media/root-rw/overlay`（tmpfs）——本文第 5 节此前"待现场确认"的挂载点推测得到验证；
- 主服务、systemd watchdog、串口自动发现、MQTT 均正常。重启后串口设备号由 `/dev/ttyUSB5` 变为 `/dev/ttyUSB0`，正好印证了自动发现相对固定设备号的必要性。

**helper 当前处于中间态，尚不可用于 overlay 生产环境。** 已验证有效的部分：

- 检测到 overlay 且非 root 时经 `sudo -n` 自动提权，主服务以 `dcdw` 运行、无权 remount 的问题已解决；
- 写入持久层成功，属主与权限在提权写入后正确还原为 `dcdw:dcdw 0600`；
- 14 项 helper 单元测试通过（原 4 项）。

**但 overlay 路径存在已知缺陷**：写入完成后无法把 `/media/root-ro` 恢复为只读（原因见第 5 节），文件系统会停在可写状态。**在改为独立文件系统方案之前，不要依赖 overlay 环境下的配置命令**。非 overlay 环境不受影响，行为与改造前完全一致。

非 overlay 环境下的完整链路回归已通过：经 MQTT 下发配置命令、收到 `status: applied` 的 ACK、参数落盘、进程退出并由 systemd 拉起、发布节拍实测由 1 秒变为 2 秒，随后原样还原。

**下一步**：按第 5 节的独立文件系统方案改造——在 `/boot/firmware` 上建 ext4 镜像、loop 挂载到配置目录、平时只读。helper 的原子写入、属主还原、自动提权逻辑可全部复用，路径映射部分改为直接 remount 配置挂载点，比现状更简单。

第 9 节的 tmpfs 上层容量上限与写满后行为仍未验证。
