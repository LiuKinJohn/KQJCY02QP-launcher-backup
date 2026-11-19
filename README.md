# 米家空气检测仪 KQJCY02QP · 自建 Launcher 备份

这是一台米家空气检测仪 KQJCY02QP 上实测运行的自建 Launcher 版本备份。备份对应设备上的 `r20261110-6`，不是可直接刷写的整机 NAND 固件包。原厂系统仍在设备上；Launcher 通过末段启动服务接管界面，启动失败时由监护脚本回退到原厂界面。

> 仓库仅供该设备的开发与恢复参考。包内含原厂程序副本和字体资源，建议保持 **Private**；不要直接用于其他型号或随意刷写分区。

## 设备概况

| 项目 | 实测信息 |
|---|---|
| 型号 | 米家空气检测仪 KQJCY02QP |
| SoC | Allwinner sun8iw 系列，ARMv7 Cortex-A7 四核 |
| 内存 | 251,868 KiB，约 246 MiB；无 swap |
| 系统 | Tina Linux，Linux 3.4.39，BusyBox 1.24.1 |
| 屏幕 | 800 × 480 横屏逻辑画布；面板帧缓冲 480 × 800，32 bpp，双缓冲 |
| 触摸 | FocalTech `fts_ts`，`/dev/input/event1`，多点触摸事件 |
| 存储 | NAND 分区含只读 rootfs、ext4 overlay、约 68 MiB UDISK；Launcher 版本包位于 UDISK |
| 显示与网络 | `/dev/fb0` 帧缓冲、`/dev/disp`、`/dev/mali`；2.4 GHz Wi-Fi |

Launcher 读取 SHTC3 温湿度、光照、TVOC 与 PM2.5。PM2.5 来自 `/dev/ttyS3` 的传感器帧，使用原厂校准系数；空气质量 AQI 由 PM2.5 估算。电池与供电状态来自 Linux `power_supply`。这些读数已做原厂数据路径的内部核对，**没有独立标准仪器的绝对精度认证**。

## 当前功能

- 六项传感器看板与详情；24 小时半小时均值趋势、30 天每日最高/最低回顾，历史文件在设备上持久保存。
- 时钟、三页天气、设置与设备状态、日历、便签、只读系统文件浏览。
- Wi-Fi 连接、联网校时与时区设置、中英文界面、浅色/深色主题。
- 开机自启前核验文件；运行中监控 Launcher 心跳，异常时回退原厂界面并保留 ADB。

## 备份内容

- `release/r20261110-6/`：设备当前运行的 ARM 静态二进制、字体图集、监护脚本、原厂界面程序的完好副本、MD5 清单，以及已安装的启动服务脚本副本。
- `src/`：当前 Launcher 的 C 源码、组件头文件、天气获取脚本、图集生成工具与字体资源。`src/launcher.c` 是此版本的主源码。
- `release/r20261110-6/manifest.md5`：设备端当前包的原始校验清单。`launcherboot.init` 单独留存，因为它安装在 `/etc/init.d/`，不在 UDISK 版本包内。

主机与设备在备份时核对的 MD5：

| 文件 | MD5 |
|---|---|
| `launcher` | `95f909eac9195a4b884a9cee42f54de6` |
| `atlas.bin` | `92bba7f6a1117e6410a025f9572b78ef` |
| `BranOS` | `ad09f3e43ca42fbb45a926a7d32492db` |
| `guard.sh` | `15caa18f3df9a8b0e1fd3ef7f73897ef` |
| `launcherboot.init` | `a7defa3e7cb5cce88ef8841a6d183756` |

## 构建与恢复边界

项目使用 Zig 0.13 的 `zig cc` 交叉编译为 `arm-linux-musleabihf` 静态程序；本机已验证的构建命令为：

```sh
zig cc -target arm-linux-musleabihf -mcpu=cortex_a7 -static -O2 -o launcher src/launcher.c -lm
```

`src/sky_stops.h`、`src/m3_palette.h` 和 `src/status_icons.h` 已作为当前构建输入留存。图集生成还依赖 `src/fonts/` 中的字体。恢复时应先校验清单和目标设备的分区/启动环境，再通过版本化目录切换；不要用这个仓库直接覆盖 bootloader、内核或原厂 rootfs。

备份**不包含**设备运行时的 `config.ini`、华风爱科 API Key、Wi-Fi 凭据、历史数据、TVOC 基线及整机 NAND 镜像。这些设备专属内容和原始九分区镜像仍在本机单独保存。历史记录初次实现后只完成了短期真实数据读回；完整 24 小时与 30 天覆盖有待自然运行积累。

