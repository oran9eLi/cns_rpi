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

### 落地形态（已实施）

| 项 | 位置 | 说明 |
|---|---|---|
| 运行时配置 | `/var/lib/cns-rpi/config.json` | 从 git 工作树迁出：它是可变现场状态，FHS 里属于 `/var/lib`。该目录同时是独立文件系统的挂载点 |
| 候选文件暂存 | `/run/cns-rpi/` | 配置目录平时只读，候选不能建在那里。由 systemd `RuntimeDirectory=cns-rpi` 在 tmpfs 上创建，属主为服务用户，停止时自动清理 |
| 持久化载体 | `/boot/firmware/cns-config.img` | 16 MiB 固定大小 ext4 镜像，由 `cns-rpi-config.service` 只读 loop 挂载到配置目录 |

### 为什么不用 fstab 挂载

两个实测得出的理由：

1. **`mount -o ro,loop <img>` 会把 loop 设备本身标记为写保护**（`losetup` 的 `RO=1`），此后 helper 执行 `mount -o remount,rw` 会在设备层被拒，配置永远写不进去。必须先以可写方式建立 loop 设备，再把文件系统挂成 `ro`——fstab 表达不了这个顺序。
2. fstab 或 `.mount` 单元挂载失败可能把系统拖进 emergency shell。现场设备一旦如此就只能物理接触才能恢复。改用独立的 oneshot 服务后，挂载失败只会让 `cns-rpi.service` 起不来，系统本身照常启动、SSH 可用，还能远程排查。

`cns-rpi.service` 通过 `Requires=` + `After=` 依赖该单元：挂载失败时宁可主服务不启动，也不要让它读到挂载点下面那个空目录、把缺省配置当成现场配置跑起来。挂载脚本在镜像不存在时正常退出（开发机、迁移过渡期按普通目录使用），避免 `Requires=` 连带把主服务拖住。

配置目录**不能**直接挂在仓库的 `config/` 上：那会遮蔽仓库跟踪的 `config.example.json`，导致 `git status` 永久显示该文件被删除、工作树永远是脏的。

启动参数相应调整：

```
ExecStart=... /var/lib/cns-rpi/config.json \
    --config-writer=helper \
    --config-helper=/usr/local/libexec/cns-rpi-apply-config \
    --config-staging=/run/cns-rpi
```

顺带修掉一个既有隐患：主程序原先的配置路径默认值是相对路径 `config/config.json`，违反本仓库第 3 节"禁止相对路径"的规定——systemd 拉起时工作目录不确定，该默认值会静默指向错误的文件。现在配置路径必须显式给出且必须是绝对路径，否则启动即报错退出。

`--config-staging` 刻意不设默认值：机器相关的绝对路径不该写死在类型里，留空时沿用配置文件所在目录，与改造前行为一致。

### helper 的实现要点（已实施）

`scripts/cns-rpi-apply-config.py` 已支持只读挂载，配置目录尚未成为独立挂载点时行为完全不变：

- `readonly_mount_for()` 只认"配置目录恰好是只读挂载点"这一种情况；不是挂载点就原地写入，不会去 remount 别的文件系统；
- 检测到只读挂载且当前非 root 时经 `sudo -n` 提权重执行自身——主服务以 `dcdw` 运行、无权 remount；普通环境完全不涉及 sudo；
- 候选文件在可写暂存目录中校验（固定目录、固定文件名模式、固定目标路径），**暂存与原子替换固定发生在目标所在目录**，保证 rename 是同文件系统内的原子操作；
- 以 root 写入后把属主与权限还原为提权前的值——不还原会变成 `root:root`，而该文件是 `0600` 且主程序以 `dcdw` 运行，下次启动将读不到自己的配置；
- 写入后 `fsync` 文件与目录，再 `os.sync()`，然后才恢复只读——现场是直接断电，不能依赖延迟回写；
- 恢复只读放在 `finally`，写入失败也不会把设备停在可写状态。

### 持久化闭环的验收标准

沿用收尾工作文档 §8.2 的要求：

- helper 只能修改固定的 `config/config.json`，不得获得任意 shell 或任意文件写权限；
- 配置成功应用后，**断电重新上电仍保留新参数**（这是与当前状态的关键区别）；
- 写入任何阶段失败时，旧配置仍完整可用；
- helper 返回结果不确定时，设备端幂等记录能避免重复破坏配置。

## 5.5 部署模型：overlay 生效时不能部署

**这是本设计最初遗漏、并在 2026-07-20 造成真实回退的一点。**

OverlayFS 生效后，根文件系统上的**一切**写入都落在内存上层，重启即蒸发。这不只影响配置文件，而是影响所有部署产物：

- `scripts/deploy.sh` 安装的 systemd unit 与 helper；
- `git pull` 拉下来的代码与 `cmake` 构建产物；
- `install`/`mv`/`rm` 造成的任何文件变动。

危险之处在于**部署过程看起来完全成功**：文件确实写进去了、服务确实起来了、功能确实能用，直到下一次重启才暴露。当天就是这样：overlay 生效状态下完成的一整轮部署（迁移配置、安装挂载服务、更新 helper、三次 git pull）在重启后全部回退，而期间所有验证都是通过的。

这台机器生成的 `/etc/fstab` 开头其实早已写明：

```
#  To permanently modify this (or any other file), you should change-root into
#  a writable view of the underlying filesystem using:
#      sudo overlayroot-chroot
```

### 采用的维护模型：临时关闭 overlay

```bash
sudo raspi-config nonint disable_overlayfs && sudo reboot
# 重启后正常部署
cd ~/cns_rpi && git pull && ./scripts/deploy.sh
sudo raspi-config nonint enable_overlayfs && sudo reboot
```

选它而不是 `overlayroot-chroot` 的理由：流程直白、`deploy.sh` 不需要为 chroot 环境做特殊处理，构建、服务重启、挂载关系都在正常环境下进行，结果可靠。代价是每次部署两次重启——教学设备部署频次低，可以接受。

`overlayroot-chroot` 仍适用于**单个文件**的持久化修改（改一行配置、打一个补丁），不适合跑完整部署：构建产物、服务状态和挂载关系都不在 chroot 视图内。

### 已加入的防护

`scripts/deploy.sh` 开头会检查根文件系统类型，为 `overlay` 时**直接拒绝运行**并打印上述维护流程。宁可明确失败，也不要再产生一次"看似成功、重启蒸发"的部署。

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

**第二次 —— 启用 overlay 及其两项配套动作：**

```bash
# 1) swap 改为纯 zram——默认的 zram+file 需要可写根分区，只读下整条链会垮
#    （由 scripts/deploy.sh 安装 systemd/swap-cns-rpi.conf 完成，此处仅确认）
grep Mechanism /etc/rpi/swap.conf.d/50-cns-rpi.conf

# 2) 屏蔽 systemd-remount-fs——见下方说明，必须与 overlay 同时启用/撤销
sudo systemctl mask systemd-remount-fs.service

sudo raspi-config nonint enable_overlayfs
sudo reboot
```

### 为什么必须屏蔽 `systemd-remount-fs`

该单元的职责是按 `/etc/fstab` 重新挂载根文件系统。`/` 变成 overlay 后这件事在内核层面就做不到——overlayfs 不支持 reconfigure：

```
mount: /: fsconfig() failed: overlay: No changes allowed in reconfigure
```

于是它每次开机都失败。功能上无害（overlayroot 在 initramfs 阶段已挂对，没有留给它可做的事），但有两项实际代价：

- **遮蔽真实故障**：`systemctl --failed` 是排查的标准入口，长期挂着一个永远失败的单元会让人习惯性略过整个列表；
- **连累依赖它的单元**：2026-07-20 实测中，swap 整条链就是被它拖垮的——`rpi-setup-loop@var-swap.service` → `systemd-zram-setup@zram0.service` → `dev-zram0.swap` 逐级依赖失败，结果设备完全没有 swap。`systemd-random-seed`、`systemd-quotacheck-root` 等也排在它之后。

用 `mask` 而不是 `disable`：`disable` 只取消开机自启，别的单元仍可把它作为依赖拉起，而它正是被 `local-fs.target` 这类目标拉起的；`mask` 建立指向 `/dev/null` 的符号链接，任何方式都拉不起来。

⚠️ **撤销 overlay 时必须同时撤销这个屏蔽**：

```bash
sudo systemctl unmask systemd-remount-fs.service
```

`/` 变回普通 ext4 后该单元恢复实际职责——把 fstab 里的挂载选项应用到根分区。此时仍屏蔽着，fstab 选项不会生效，而且是**静默失效**。

这一项刻意没有写进 `scripts/deploy.sh`：按维护模型，`deploy.sh` 只在 overlay 关闭时运行，那一刻「维护中稍后要重开 overlay」和「决定永久不用 overlay」两种情况下 `/` 都是 ext4，脚本无法区分，写死任何一种都会在另一种情况下悄悄做错。它属于「启用/撤销 overlay」这个决定的配套动作，与部署的生命周期不同，因此放在本节而不是部署脚本里。

**第三次（可选，稳定运行一段时间后再做）—— 锁只读启动分区：**

```bash
sudo raspi-config nonint enable_bootro
sudo reboot
```

## 8. 重启后的验证

必须验证「实际挂载为只读」，而不是「配置文件改了」——这是收尾工作文档特别强调的一点。

```bash
findmnt -n -o TARGET,SOURCE,FSTYPE,OPTIONS /      # 期望 fstype 为 overlay
grep ' /media/root-ro ' /proc/mounts               # 期望持久层为 ro
grep 'cns-rpi' /proc/mounts                        # 期望配置卷 ext4 ro
touch /tmp_write_probe 2>&1                        # 期望落在内存层，重启后消失
grep -o 'overlayroot=[a-z]*' /proc/cmdline         # 确认内核参数已生效
systemctl is-active cns-rpi-config.service cns-rpi.service
cat /proc/swaps                                    # 期望 /dev/zram0 已激活
systemctl --failed                                 # 期望 0 个失败单元
python3 -c 'import json;print(json.load(open("/var/lib/cns-rpi/config.json"))["runtime"])'
```

判据：

- `findmnt` 显示根挂载类型为 `overlay`（而非改动前的 `ext4 rw,noatime`）。仅检查 `cmdline.txt` 内容不构成验证；
- **失败单元必须为 0**。若 `systemd-remount-fs.service` 出现在失败列表里，说明第 7 节的屏蔽步骤被漏掉了；
- **配置内容必须与重启前一致**——这是持久化闭环唯一的最终证明，其余各项都只能说明卷挂着。

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

**配置已迁至 `/var/lib/cns-rpi/`，代码侧改造完成**：

- 主程序要求显式给出绝对配置路径，去掉了违反第 3 节规定的相对路径默认值；
- 候选文件改在 systemd `RuntimeDirectory=` 提供的 `/run/cns-rpi` 暂存，配置目录变成只读挂载后仍可创建候选；
- helper 支持只读挂载：自动提权、写后还原属主权限、`finally` 恢复只读；
- `scripts/deploy.sh` 会把旧位置的现场配置迁到新路径，旧文件改名为 `config.json.migrated` 保留而非删除，迁移出问题可回退；
- 12 项 helper 单元测试 + 全量 27 项 ctest 通过，`-Wall -Wextra` 零告警。

实机端到端验证（配置目录此时尚未挂载独立文件系统，走的是原地写入分支）：经 MQTT 下发配置命令 → `status: applied` ACK → 新路径落盘 → 属主保持 `dcdw:dcdw 0600` → 暂存目录无残留 → 进程退出并由 systemd 拉起 → 服务恢复正常。

**独立文件系统挂载已建立并验证**：

- `/boot/firmware/cns-config.img`（16 MiB ext4）已创建并写入现场配置；
- `cns-rpi-config.service` 开机只读挂载，`cns-rpi.service` 通过 `Requires=`/`After=` 依赖它；
- 挂载点 `/var/lib/cns-rpi` 已在**持久层**（`/media/root-ro/var/lib/cns-rpi`）创建。这一步不可省略：该目录原先只存在于 overlay 的内存上层，重启就会连同配置一起消失，主服务将因找不到配置而无法启动；
- 依赖链实测：停掉整条链后只启动 `cns-rpi.service`，systemd 自动拉起挂载服务、以 `ro` 挂好卷、主服务正常启动。

**只读环境下的配置持久化闭环已实测通过**：

| 验证项 | 结果 |
|---|---|
| 平时写入被拒 | `Read-only file system` |
| `readonly_mount_for()` 判定 | 正确返回 `/var/lib/cns-rpi` |
| MQTT 配置命令 | `status: applied` |
| 参数落盘 | 生效 |
| **写入后恢复只读** | **成功**——这正是 `/media/root-ro` 做不到的 |
| 属主与权限 | 保持 `dcdw:dcdw 0600` |
| 暂存目录残留 | 无 |
| 幂等 | 重复 `command_id` 返回 `already_applied` |

### 2026-07-20 重启验证：未通过，已定位并修正

首次重启验证**失败**，暴露了第 5.5 节的部署模型缺口：

- `/media/root-ro` 恢复为 `ro` ✓，`/var/swap` 永久删除 ✓（这两项是直接在持久层操作的，所以幸存）；
- 但 `cns-rpi-config.service` 显示 `not-found`、`/var/lib/cns-rpi/config.json` 不存在——overlay 生效期间安装的所有内容都已蒸发；
- 仓库 HEAD 退回 `1a5cf4c`，`/usr/local/libexec/cns-rpi-apply-config` 退回旧版，`cns-rpi.service` 退回旧 unit。

**没有数据永久丢失**：`/boot/firmware/cns-config.img` 完好（配置齐全，含 6 条 `applied_command_ids`），持久层挂载点 `/media/root-ro/var/lib/cns-rpi` 仍在，代码都在 GitHub 上。设备靠回退后的旧 unit 继续正常运行。

修正措施：`deploy.sh` 增加 overlay 检测拒绝运行；本文补充第 5.5 节的维护模型。重新部署需按该节流程执行。

### 2026-07-20 重启后终验：通过

按第 5.5 节的维护模型完成部署（关闭 overlay → 重启 → `git pull` + `deploy.sh` → 启用 overlay → 重启），该模型本身也随之首次跑通。终验结果：

| 项 | 结果 |
|---|---|
| 根文件系统 | `overlay` |
| 持久层 | `ro` |
| 配置卷 | `ext4 ro,relatime`，开机自动挂载 |
| **配置内容** | **`interval=1000`、6 条 `applied_command_ids`，跨多次重启完整保持** |
| swap | `/dev/zram0` 2 GiB 已激活 |
| 失败单元 | 0 |
| `/var/swap` | 已删除，磁盘由 7.6 GiB 降至 5.6 GiB 已用 |
| 主服务 / 挂载服务 | 均 `active`，串口自动发现与 MQTT 正常 |

`/var/swap` 最终是被系统自己清理的：切换到纯 zram 后生成器会产出
`rpi-remove-swap-file@var-swap.service`，它在 overlay 关闭、根分区可写的那次启动中
成功执行（`Finished`）。此前该服务一直因只读根而依赖失败——机制自带清理能力，
只是被只读根卡住，不需要人工删除。

至此第 1 节（只读根文件系统）与配置持久化闭环均已实施并验证。

### 仍未完成

- 第 9 节的 tmpfs 上层容量上限与写满后行为未验证；
- 第 10 节的物理断电验收未开始——前置条件现已全部就绪。
