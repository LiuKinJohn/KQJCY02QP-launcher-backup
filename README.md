# 米家空气检测仪 KQJCY02QP · 自建 LauncherOS

这是一台米家空气检测仪上实测运行的自建 Launcher。仓库保存的是当前 `r20261110-6` 的源码、自建 ARM 二进制与启动监护脚本，供开发、核对和版本留存。Launcher 通过末段启动服务接管界面；启动失败时由监护脚本回退原厂界面。

**这份公开仓库里面的固件留存不是、不是、不是可直接刷写的整机固件包。** 仅用作恢复时必须从可信本地备份补齐资源使用，并核对目标设备的分区与启动环境。

## 设备概况

| 项目 | 实测信息 |
|---|---|
| 型号 | 米家空气检测仪 KQJCY02QP |
| SoC | Allwinner sun8iw 系列，ARMv7 Cortex-A7 四核 |
| 内存 | 251,868 KiB，约 246 MiB；无 swap |
| 系统 | Tina Linux，Linux 3.4.39，BusyBox 1.24.1 |
| 屏幕 | 800 × 480 横屏逻辑画布；面板帧缓冲 480 × 800，32 bpp，双缓冲 |
| 触摸 | FocalTech `fts_ts`，`/dev/input/event1`，多点触摸事件支持 |
| 存储 | NAND 分区含只读 rootfs、ext4 overlay、约 68 MiB UDISK；Launcher 版本包位于 UDISK |
| 显示与网络 | `/dev/fb0` 帧缓冲、`/dev/disp`、`/dev/mali`；Wi-Fi |

Launcher 读取 SHTC3 温湿度、光照、TVOC 与 PM2.5。PM2.5 来自 `/dev/ttyS3` 的传感器帧，使用原厂校准系数；空气质量 AQI 由 PM2.5 估算，原厂eCO2通过基于TVOC和其它多项数据合并计算而来，还没完全搞定。电池与供电状态来自 Linux `power_supply`。这些读数已做原厂数据路径的内部核对。

## 当前功能

- 重写了六项传感器看板与详情；24 小时半小时均值趋势、30 天每日最高/最低回顾，历史文件在设备上持久保存。
- 时钟、三页基于华风爱科api的天气、设置与设备状态、日历、便签、只读系统文件浏览。
- Wi-Fi 连接、联网校时与时区设置、中英文界面、浅色/深色主题。
- 开机自启前核验文件；运行中监控 Launcher 心跳，异常时回退原厂界面并保留 ADB。

## 文件

- `src/`：当前 Launcher 的 C 源码、组件头文件、天气获取脚本和生成工具。`src/launcher.c` 是主源码；编译依赖的生成头文件已留存。字体与字体图集没有放入公开仓库。
- `release/r20261110-6/launcher`：设备当前运行的自建 ARM 静态二进制，MD5 `95f909eac9195a4b884a9cee42f54de6`。
- `release/r20261110-6/guard.sh`：启动完整性检查、心跳监护与原厂回退逻辑，MD5 `15caa18f3df9a8b0e1fd3ef7f73897ef`。
- `release/r20261110-6/launcherboot.init`：设备 `/etc/init.d/launcherboot` 的副本，MD5 `a7defa3e7cb5cce88ef8841a6d183756`。

## 构建与运行边界

项目使用 Zig 0.13 的 `zig cc` 交叉编译为 `arm-linux-musleabihf` 的静态程序：

```sh
zig cc -target arm-linux-musleabihf -mcpu=cortex_a7 -static -O2 -o launcher src/launcher.c -lm
```

拉取时，设备端运行还需要原厂程序的可靠副本和对应字体图集，监护脚本会按设备的版本包清单验证它们。不要直接用此仓库覆盖 bootloader、内核或原厂 rootfs。目前使用ai工具，已验证短期真实/填充数据的保存、重启读回与图表交互。

仓库中的 GNOME symbolic 图标原始文件保留了其 LGPL 文本。其他第三方字体、原厂程序、标识不做收录。
