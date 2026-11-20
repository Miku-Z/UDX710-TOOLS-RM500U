# RM500U-CN 管理中心（UDX710-TOOLS 适配版）

[🇺🇸 English README](README.md)

基于开源项目 [LeoChen-CoreMind/UDX710-TOOLS](https://github.com/LeoChen-CoreMind/UDX710-TOOLS) 修改适配，面向 **RM500U-CN（UNISOC UDX710 平台）+ 有线网卡转接板（如 PCIe RTL8168 / eth0）** 的轻量级嵌入式 5G Web 管理后台。

---

## 相比原版的主要改动

### 1. 5G NR 射频与硬件监控增强（`/api/nr_radio_detail`）
- **四天线实时监测**：展示 `ANT0–ANT3` 各自实时 RSRP 与 SINR。
- **准确的 MIMO / 调制 / MCS / 带宽显示**：
  - 修复 `AT+SP5GCMDS="get nr synch_param",16` 字段偏移及非 NR 驻网保护；
  - 修正活跃下载时误判“待机”以及下行 MIMO 层数错映射到上行的问题，准确区分 `4R 单流 (1层)`、`2x2 MIMO (2层)`、`4x4 MIMO (3层/4层)` 与 `4x4 就绪 (待机)`；
  - 新增信道带宽（MHz / RB）、Point-A ARFCN、VoNR/VoLTE + SMS IMS 注册状态显示；
  - 前端为 `/api/nr_radio_detail` 增加带防重入锁的 **2.5 秒** 独立快速轮询，实时捕捉空口突发调度。
- **多温区与板卡状态**：新增 SoC、5G 基带（NRCP）、5G 功放（PA5G）、主板多路温度监测，CPU 主频显示、`eth0` 有线网口协商速率/双工状态，并修正内存 Cache 统计。

### 2. 5G SA 短信收发全链路修复
- 启动与后台线程自动启用 `AT+IMSEN=1`、配置 `AT+CPMS="ME","ME","ME"` 与 `AT+CNMI=2,1,0,0,0`，无需人工敲 AT。
- 新增 `ME -> SQLite` 自动 PDU 排空与 5 秒去重，支持中文/长短信 **UDH 分片重组** 与真实发送状态回执（`MessageState`）。
- 移除原版遗留的“R106 短信修复”手动开关。

### 3. RM500U-CN 平台适配与功能精简
- **系统信息与选网**：支持从 `/etc/quectel-project-name` 读取型号与固件版本（`AT+CGMR` 兜底），兼容 `/proc/cpuinfo` 十六进制 `Serial`，选网优先走 `org.ofono.RadioSettings` 并支持 `AT+SPTESTMODE` 回退，主小区频段支持 `AT+SPENGMD` 兜底。
- **USB 网卡模式重构**：移除原版高风险 configfs/IPA 热切换，改为直接读写 `/mnt/data/usbenum.ini`（`virtualcn`）+ `sync()` 落盘，支持 **RNDIS / CDC-ECM / CDC-NCM / MBIM**，重启后生效。
- **网络工具兼容**：兼容 `net-tools 2.x` 的 `ifconfig` 输出格式（`eth0:` 冒号与 `ether` 字段）。
- **裁剪非 RM500U-CN 硬件项**：禁用电池/充电控制、手机壳模式、OTA 在线更新弹窗及无效的 SIM 卡槽 2 页面。
- **路径与端口解耦**：`vnstat`、`rathole`、`Plugins` 优先使用当前目录相对路径（`./`），支持指定任意端口启动（如 `./server 6688`）。

---

## 编译指南

### GitHub Actions 自动编译
推送代码后，[`.github/workflows/build.yml`](.github/workflows/build.yml) 会自动下载 ARM 官方 `gcc-arm-9.2-2019.12`（`aarch64-none-linux-gnu`，目标 `GLIBC 2.30`，兼容模块 `GLIBC 2.31`）与 Node.js，编译并产出可直接部署的 Artifact（包含 `server`、`dist/`、`start.sh`）。

### 本地手动交叉编译
> 注意：模块系统内置为 `GLIBC 2.31`，请使用兼容 `GLIBC <= 2.31` 的交叉工具链（例如 ARM 官方 GCC 9.2），避免使用高版本 Ubuntu 默认编译器导致模块运行报 `GLIBC` 版本不兼容。

```bash
# 1. 构建前端静态资源
cd web
npm install
npm run build

# 2. 交叉编译后端（aarch64）
cd ../src
make CC=aarch64-none-linux-gnu-gcc STRIP=aarch64-none-linux-gnu-strip
# 产物位于 src/build/ofono-server
```

---

## 模块端部署说明

1. 将编译产物 `src/build/ofono-server`（重命名为 `server`）与 `web/dist/` 上传至模块目录（例如 `/home/root/6677/` 或 `/home/root/6688/`）。
2. 如从原版升级，直接覆盖旧目录中的 `server` 和 `dist/` 即可保留原有数据库与配套工具；全新空白目录如需流量统计和内网穿透，请自行放置 `vnstat`、`vnstatd`、`vnstatd.conf`、`rathole` 到同目录（短信所需的 `sqlite3` 模块系统通常已内置）。
3. 启动服务（默认端口 `6677`，也可通过第一个参数自定义端口）：
   ```sh
   chmod +x server start.sh
   ./start.sh 6688
   ```
4. 浏览器访问 `http://<模块IP>:6688`（默认密码 `admin`）。

### 前提条件说明
- 本仓库仅包含 WebUI 与后台服务，**不包含**外接转接板的 PCIe 网卡内核驱动（如 `r8169.ko` / `yt6801.ko`）及系统级网络初始化脚本，部署前请确保模块网络与 `ofonod` 服务已正常运行。
- 5G SA 下收发短信需要 SIM 卡已开通运营商 VoLTE / IMS 短信业务。

---

## 版权声明、修改说明与开源协议

- **上游原项目**：[LeoChen-CoreMind/UDX710-TOOLS](https://github.com/LeoChen-CoreMind/UDX710-TOOLS)，原始代码版权归原作者及贡献者所有。
- **修改声明**：本衍生版本于 **2026 年 11 月** 基于上述上游代码进行了大幅修改与平台适配，以支持 RM500U-CN 平台、5G SA 短信收发及 4 天线 NR 监控等功能。
- **开源协议**：遵循原项目相同的 **GNU General Public License v3.0 (GPLv3)** 协议开源（完整协议文本见 [LICENSE](LICENSE)）。
- **免责声明**：本软件按“原样（AS-IS）”提供，不附带任何明示或暗示担保；锁频、切换 USB 网卡模式或系统配置操作均存在断网或变砖风险，请使用者自行评估承担风险。
