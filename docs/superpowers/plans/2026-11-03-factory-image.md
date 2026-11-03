# Factory Image Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans. Steps use checkbox syntax for tracking.

**Goal:** 提供可维护的同一出厂镜像，Pi 上电后启动 5G，从 F407 读取编号，服务器人工确认后自动取得更新凭据。

**Architecture:** 复用发布包 Installer，拆分离线布局、配置导入和在线启动。官方 image-gen 配方安装运行依赖、通用配置及同源发布包；登记客户端等待 F407 绑定，使用随机登记凭据请求服务器人工批准，取得节点令牌后启用现有维护轮询。

**Tech Stack:** Python 3 标准库、systemd、NetworkManager、Raspberry Pi rpi-image-gen、GitHub Actions native ARM64。

**Spec:** `docs/superpowers/specs/2026-11-03-工厂镜像设计.md`

## Global Constraints

- trixie ARM64、`dcdw`、普通可写根目录，复用现有更新协议。
- 通用镜像不含真实令牌、绑定文件或已使用的机器身份；业务配置是全批次通用配置。
- 首次启动无需下载或安装软件；不逐台写入编号/凭据。
- 登记及响应 schema_version 为 1；POST /api/maintenance/v1/enrollments。
- 无默认 SSH 密码；服务器批准登记，不新增整盘 OTA 或维护程序远程自升级。

## Review Focus

- 首次尚无 F407 绑定时正常等待；业务/拨号不依赖维护令牌。
- 离线安装不得调用宿主 systemd 或生成指向构建机的绝对链接。
- 反复登记、断电重启复用请求，不得替换既有维护身份或绑定。
- 5G 网卡由 NetworkManager DHCP 管理，不能只装拨号脚本而遗漏 IP/DNS/路由。
- 凭据不得出现在镜像、构建日志或成功标记中。

### Task 1: 离线布局与自动登记

**Files:** 修改 `scripts/maintenance/install_release.py`、`scripts/build_release_package.py`、`CMakeLists.txt`；新增 `scripts/maintenance/enrollment_client.py`、`tests/test_enrollment_client.py` 和 image 登记 unit/timer。

**Interfaces:** Installer.prepare(release) 安装布局和启用 unit，不启动服务；Installer.configure(release, business_config, maintenance_config) 预检并导入配置，不启动服务；bootstrap 组合二者并维持原有观察。登记配置位于 /etc/cns-rpi-enrollment.json，随机登记状态位于 /var/lib/cns-rpi-maintenance/enrollment.json，批准结果写 /etc/cns-rpi-maintenance.json。

- [x] 写测试：离线布局无 restart/observe；helper 在另一根目录仍为有效相对链接；真实 loopback HTTP 登记 pending→approved，重启复用凭据。
- [x] 运行 unittest 观察 RED。
- [x] 提取 prepare/configure，实现 enrollment；测试错误响应、身份冲突、已有配置和绑定保留。
- [ ] 运行新测试及原安装/客户端测试至 GREEN，并加入 CTest。
- [ ] 提交此功能。

### Task 2: 镜像配方与构建工作流

**Files:** 新增 `scripts/image/`、`image/`、`.github/workflows/factory-image.yml` 及镜像构建测试。

**Interfaces:** 输入同源 `dist/` 发布包，输出 `.img.xz`、`.sha256`、构建记录；seed 脚本使用 Task 1 的 Installer.prepare，以离线 systemctl --root 启用 unit。

- [ ] 写测试：真实发布包可离线安装，镜像无令牌/绑定；配置及 DHCP profile 权限正确；未批准不执行维护 poll。
- [ ] 运行测试观察 RED。
- [ ] 根据固定 image-gen 提交的实际接口实现 recipe、seed、构建与 artifact 工作流。
- [ ] 运行本地回归和 ARM64 CI；运行镜像工作流验证镜像生成与离线文件检查。
- [ ] 提交构建链。

### Task 3: 交付与独立复核

**Files:** 更新 README、部署手册、镜像手册和变更记录。

**Interfaces:** 使用者获取同一镜像，烧录，上电，服务器看到 F407 编号并确认设备，之后可在 Web 选择版本升级。

- [ ] 文档说明版本/来源、自动编号与审批、未联网时排查和重置限制。
- [ ] 独立审阅实际文件及构建证据，修复具体阻塞问题并回归。
- [ ] 更新现有草稿 PR，不合并或发布；明确区分构建、实体启动与服务器联调。
