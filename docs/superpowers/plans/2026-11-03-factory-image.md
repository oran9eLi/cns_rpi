# Factory Image Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans. Steps use checkbox syntax for tracking.

**Goal:** 同一Pi5镜像直接上电，从F407获取编号，服务器确认接入后Web指定版本更新。

**Architecture:** 固定官方image-gen预装同源ARM64包及通用配置，Installer分离离线布局与在线启动。统一poll使用F407 device_id和一份首启生成的device_key，不增加节点或登记接口。

**Tech Stack:** Python3标准库、systemd、NetworkManager、rpi-image-gen、GitHub Actions native ARM64。

**Spec:** `docs/superpowers/specs/2026-11-03-工厂镜像设计.md`

## Global Constraints

- Pi5/trixie ARM64、dcdw、普通可写根目录。
- 同一镜像不含编号、接入密钥、绑定或已用机器身份；不逐台填凭据。
- 维护配置仅server_url/business_config/schema_version；密钥运行时root0600生成。
- 单POST /api/maintenance/v1/devices/{device_id}/poll，access为pending/approved/rejected。
- 不改服务器代码、不做整盘OTA、自升级或远程MAVLink。

## Review Focus

- 无F407绑定时正常等待，不启动HTTP请求，不阻碍业务/5G。
- 未批准设备不得收到任务或确认；重启复用密钥和任务结果。
- 离线安装不启动宿主服务，链接不能指向构建机。
- 运行依赖先于seed；NetworkManager提供真实DHCP/IP/DNS/路由。
- 镜像无设备密钥；管道失败不能因tee而假绿。

### Task 1: 统一身份与任务接口

**Files:** scripts/maintenance/{protocol.py,device_access.py,maintenance_client.py,install_release.py}、scripts/bootstrap_pi.py、packager、CMake、配置样例与相应tests。

**Interfaces:** load_device_config(config_path,state_dir,root)->MaintenanceConfig|None；MaintenanceConfig(server_url,device_id,device_key,ca_file=None)。Installer.prepare(release)离线布局无restart；configure导入配置；bootstrap组合二者并保留观察。parse_poll返回ack/job，未批准仅允许空任务。

- [x] 写测试并观察RED：真实HTTP pending/approved、无绑定不请求、重启复用、身份/服务器变更不改密钥、URL路径编码和权限。
- [x] 实现最小协议与接入helper，保留真实Installer任务集成与回退测试。
- [ ] 完整CTest及ARM64 CI GREEN；提交核心改动。

### Task 2: 镜像与构建工作流

**Files:** image/、scripts/image/、factory-image.yml、tests/test_factory_image.py。

**Interfaces:** 输入同源dist包、server_url、通用business config；输出img.xz/sha/build-record/packages/readback。固定上游提交；postbuild hook调用seed(root,archive,sha,commit,business_config,server_url)。

- [x] 写真实packager→Installer→离线systemctl测试，观察RED。
- [x] 实现recipe、manifest依赖、postbuild seed、DHCP与只读回检。
- [x] 执行workflow run块确认Docker退出23会让管道失败（RED→GREEN）。
- [x] 旧接口5c7e8de实际生成172940320字节镜像并通过离线回检，证明构建链可用。
- [ ] 精简协议后重新制镜像、核对产物和摘要，再提交构建结果。

### Task 3: 文档与交付

**Files:** README、远程更新接口契约/部署手册、工厂镜像手册、变更记录。

**Interfaces:** 同一镜像烧录、上电，服务器确认一次，后续Web指定版本更新；只有device_id和一份接入密钥。

- [x] 文档统一删除额外node/enrollment身份与二次令牌流程。
- [ ] 独立审阅并修复具体阻塞，更新草稿PR和验证记录。
- [ ] 明确镜像生成、占位地址、服务器对接及硬件验收边界，不合并或发布。
