# 5G 模块开机拨号自动化 设计文档

版本：2026-07-09
状态：已批准，待写实施计划
适用范围：`docs/5G模块拨号验证.md` 里记录的手动 AT 拨号流程，自动化成开机执行的脚本

## 1. 范围

把 `docs/5G模块拨号验证.md` 里手动敲的 AT 命令序列（激活 PDP 上下文 + 绑定 NCM 网卡）封装成一个开机自动跑一次的独立 Python 脚本 + systemd oneshot 单元。跟 `cns_rpi` 主程序（C++/MAVLink/MQTT）完全独立，不共享代码、不共享进程，只共享同一个 `config/config.json` 文件（新增一节）。

**明确不做的事**（避免范围蔓延）：

- 不用 ModemManager/QMI——验证文档已确认 NCM 模式下两条 AT 命令 + NetworkManager 自动 DHCP 就够，不需要引入额外的连接管理框架。
- 不处理 DHCP/路由——`usb0` 一有 carrier，NetworkManager 自动接管，脚本不写任何 `dhclient`/`udhcpc`/路由表操作。
- 不调整 5G/WiFi 路由 metric 优先级——这个问题已经拍板确认（5G metric 更低、优先级更高是期望的产品行为，因为现场不一定有 WiFi），不在这次改动范围内，详见 memory `project_5g_module.md`。
- 不做 PIN 码解锁——验证文档里 `AT+CPIN?` 返回 `READY`，SIM 卡没锁 PIN；如果以后的物联卡需要解锁，需要新的一轮设计。
- 不集成进 `cns_rpi` 主程序或复用 `uart::SerialPort`——AT 指令是纯文本行协议，跟 MAVLink 二进制帧完全不是一回事，用 Python + pyserial 独立实现更简单可靠。
- 不在脚本内部做多次重试——重试策略交给 systemd 的 `Restart=on-failure`，脚本本身只跑一次、有边界超时、成功/失败明确退出码。

## 2. 文件布局

```
scripts/
└── cellular_dialup.py          # 新建：拨号主脚本
systemd/
└── cellular-dialup.service     # 新建：仓库里第一个 systemd 单元
config/
└── config.example.json         # 修改：新增 cellular 节
```

`scripts/install_deps.sh` 需要新增 `python3-serial`（pyserial 的 Debian 包名）到依赖安装列表。

## 3. `config.json` 新增的 `cellular` 节

```json
"cellular": {
  "apn": "cmnet",
  "cid": 1,
  "usb_interface_number": "05",
  "at_port_wait_seconds": 30
}
```

- `apn`：拨号用的 APN 名字。当前测试值 `cmnet` 是手机卡自带的默认值；换成正式物联卡后大概率要改（见 `docs/5G模块拨号验证.md` 测试前提）。
- `cid`：PDP 上下文编号，当前测试值 `1`（对应验证文档里查到的 `cmnet` 上下文）。
- `usb_interface_number`：AT 指令口对应的 USB interface number（字符串，因为 `udevadm` 输出的是两位数字符串如 `"05"`，保留前导零）。这次实测是 `"05"`，理论上换硬件/固件版本可能变，所以做成配置项而不是写死在脚本里。
- `at_port_wait_seconds`：开机时等待 AT 口枚举出现的超时秒数，超时判定这次运行失败（退出非 0），重试交给 systemd。

这一节独立于 `mqtt`/`serial`/`logging`/`identity` 这几节，`cellular_dialup.py` 只读这一节，不关心文件里其它节的内容（`cns_rpi` C++ 程序反过来也完全不读这一节）。

## 4. 拨号序列（`scripts/cellular_dialup.py` 主流程）

1. **找 AT 口**：轮询 `/dev/ttyUSB*`，对每个设备跑 `udevadm info -q property -n <dev>`，取 `ID_USB_INTERFACE_NUM` 属性，跟配置的 `usb_interface_number` 比较。每 1 秒重新扫描一次，最多等 `at_port_wait_seconds` 秒；超时打日志、退出非 0。
2. **打开串口**：pyserial 打开找到的端口（波特率用 AT 口默认的 115200，8N1，不做成配置项——这是 modem AT 口的固定参数，不是运维要调的东西）。
3. **发送 AT 命令的通用规则**：每条命令后跟 `\r\n`，逐行读回应直到匹配到 `OK`、`ERROR`，或 `+CME ERROR: <n>`，单条命令超时固定 5 秒（不做成配置项，实现细节）。超时或非预期错误：打日志、退出非 0。
4. **检查/建立 PDP 上下文**：`AT+CGDCONT?`，解析出 `cid` 对应行的 APN 值；如果没有这个 `cid` 或 APN 不匹配配置，发 `AT+CGDCONT=<cid>,"IPV4V6","<apn>"`。
   - **假设待验证**：这条命令这次真机验证时没有实际测过（当时的 SIM 卡已经预置好上下文），是否需要额外的 AT 参数、返回什么样的 OK/ERROR，等正式物联卡到货后需要单独验证，脚本先按 3GPP 标准语法实现。
5. **激活 PDP 上下文**：`AT+CGACT=1,<cid>`。如果返回 `OK`，直接视为成功。如果返回 `ERROR`/`+CME ERROR`，不去猜这个错误码是不是"已经激活"（这次真机验证没测过重复激活会返回什么，猜错误码容易埋雷）——改用 `AT+CGACT?` 查询当前实际激活的上下文列表，如果目标 `cid` 已经在列表里（状态为已激活），视为成功；否则视为真实失败。
6. **绑定 NCM 网卡**：`AT+QNETDEVCTL=<cid>,1,0`（`0` 是网卡序号，验证文档确认这次只有一张）。
7. **确认网卡起来**：轮询 `ip link show usb0` 直到看到 `LOWER_UP`，最多等 10 秒（不做成配置项）。成功：打日志、退出 0。超时：打日志、退出非 0。
8. DHCP 不用脚本管——NetworkManager 已经在跑，`usb0` 一有 carrier 自动 DHCP（验证文档已确认）。

任何一步遇到非预期的 `ERROR`（不属于第 5 步"已激活"这类已知良性错误）：打日志说明是哪一步、什么错误码，立即退出非 0，不在脚本内部重试单条命令或整个序列。

## 5. systemd 单元

`systemd/cellular-dialup.service`：

```ini
[Unit]
Description=cns_rpi 5G模块开机拨号(AT+CGDCONT/CGACT/QNETDEVCTL)
After=network-pre.target
Before=network.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/bin/python3 /opt/cns_rpi/scripts/cellular_dialup.py /opt/cns_rpi/config/config.json
Restart=on-failure
RestartSec=15

[Install]
WantedBy=multi-user.target
```

- `Type=oneshot` + `RemainAfterExit=yes`：脚本跑完退出后 `systemctl status` 显示 `active (exited)`，不是一闪而过的 `inactive (dead)`，方便现场排查。
- 不依赖 `cns-rpi.service`——这个 systemd 单元还没造出来（M7 才建），5G 拨号跟 MAVLink/UART 链路完全独立，互不阻塞、互不等待。
- `Restart=on-failure` + `RestartSec=15`：失败后 15 秒重试一次，重试次数用 systemd 默认策略（不限次数）。
- `ExecStart` 路径按真机现有部署惯例（`/opt/cns_rpi/`）写的示例值，实施阶段要跟真机实际部署路径核对，不是这次设计要拍板的内容。

## 6. 错误处理

- AT 口在 `at_port_wait_seconds` 内没出现：退出非 0，交给 systemd 按 `RestartSec` 重试。
- 任一步 AT 命令返回非预期错误（含超时）：退出非 0，同上交给 systemd 重试。
- `AT+CGACT` 返回 `ERROR`：用 `AT+CGACT?` 复查目标 `cid` 是否已经处于激活状态，是则视为成功继续往下走，否则视为真实失败。
- `usb0` 迟迟不出现 `LOWER_UP`：退出非 0，同上交给 systemd 重试。
- 脚本本身不做无限循环重试，也不做告警上报——5G 只是可选的备用/主用上网通道，不是 MAVLink/UART 链路的依赖，拨号失败不应该影响 `cns_rpi` 主程序的启动和运行，这次也不设计跟主程序的状态联动（比如把拨号失败状态写进 `state_store`）——如果以后需要，是新的一轮设计。

## 7. 测试计划

**自动化单测**（Python `unittest`，不引入 pytest 等新依赖，不需要真实硬件）：

把"AT 响应字符串 → 该发下一条什么命令/该不该判定失败"这部分纯逻辑，跟"真的开 `/dev/ttyUSB*`、真的调 `udevadm`/`ip link`"这部分显式拆开——前者用假的 AT 响应文本喂给解析/决策函数做单测（覆盖：正常一路 OK；`CGDCONT?`查到的APN不匹配触发`CGDCONT=`；`CGACT`返回`ERROR`但`CGACT?`查到目标cid已激活时判定成功；`CGACT`返回`ERROR`且`CGACT?`里也没有目标cid时判定失败；某一步遇到未知`ERROR`判定失败），后者不写自动化测试——跟 C++ 那边`uart::MavlinkLink`对真实串口"不写自动化单测"的既定惯例一致。

**人工验证**（真机）：

1. 不装 systemd 单元，先 SSH 到真机手动跑一次 `python3 scripts/cellular_dialup.py config/config.json`，对照 `docs/5G模块拨号验证.md` 里已经手动验证过的每一步（`usb0` 变 `LOWER_UP`、NetworkManager 自动分到 IP、能 ping 通公网），确认脚本自动跑出来的结果跟当初手动敲 AT 命令一致。
2. 装上 `systemd/cellular-dialup.service` 并 `enable`，重启真机，确认开机自动完成拨号，`systemctl status cellular-dialup` 显示 `active (exited)`。
3. 正式物联卡到货后：重新跑一次上面两步，同时验证第 4 节里标注"假设待验证"的 `AT+CGDCONT=` 手动建上下文分支（如果新卡没有预置好的 PDP 上下文）。
