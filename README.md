# 小米 14 上的 Ubuntu GNOME 桌面

把小米 14 变成一台能日常使用的 Linux 电脑：Android 照常运行，同时在一个 Ubuntu 26.04 容器里跑完整的 GNOME 50 桌面，画面显示在手机屏幕上；插上 USB，还能把 Mac 的屏幕变成这台 GNOME 的第二块显示器（XScreen），鼠标、键盘、剪贴板、声音、文件都通。

这个仓库收录了整套方案中**我们自己写的部分**：启动与服务脚本、mutter / Mesa 补丁、系统集成（网络、蓝牙、存储、屏幕安全区、权限认证），以及 XScreen 扩展屏（手机端守护进程 + Mac 应用 + scrcpy 兼容层）。上游组件（Anland、mutter、Mesa、Ubuntu 本身）不在仓库里，下文会写清楚它们从哪来、我们改了什么。

---

## 目录

1. [效果一览](#效果一览)
2. [整体架构](#整体架构)
3. [前提条件](#前提条件)
4. [手机上的完整目录布局](#手机上的完整目录布局)
5. [仓库结构](#仓库结构)
6. [各部分是怎么工作的](#各部分是怎么工作的)
7. [安装部署](#安装部署)
8. [日常使用](#日常使用)
9. [从源码构建](#从源码构建)
10. [排错与诊断](#排错与诊断)
11. [已知限制](#已知限制)
12. [上游与致谢](#上游与致谢)

---

## 效果一览

**手机上的 GNOME**

- GNOME 50（mutter 50.1，Anland 后端）直接画在手机屏幕上，120 Hz，横竖屏都能用，旋转后缩放保持不变。
- 顶栏透明，内容自动避开前置摄像头挖孔和屏幕圆角（时钟位置不动）。
- 设置里的 Wi‑Fi、蓝牙、移动数据、飞行模式都是真的：它们直接操作手机的无线电，Android 仍然是唯一的硬件拥有者。
- Ubuntu 有自己独立的 400 GiB 存储空间，在“设置 › 关于”、“磁盘”、“文件”里显示为“Ubuntu 系统盘”；手机自己的存储不暴露给桌面。
- 安装软件、改系统设置时弹出正常的 GNOME 密码对话框（容器里没有 logind，我们补了一个桌面认证代理）。
- Vulkan（Turnip）可用；对 Chromium/Electron 应用做了兼容处理（见[已知限制](#已知限制)）。
- 负载调优走“均衡”路线：只在桌面运行时给 GPU 一个下限频率、给 gnome-shell 一个调度利用率下限，空闲时照常省电。

**XScreen：Mac 当扩展屏**

- 通过 USB（adb 转发）把 GNOME 的一块虚拟显示器送到 Mac 上，端到端延迟约几十毫秒（手机端采集到发出 p50 ≈ 11–16 ms，点击到新画面平均 30–90 ms）。
- 手机硬件编码 HEVC，Mac 硬件解码（VideoToolbox），两头都不占 CPU。
- 自动适配 Mac 的显示器：外接屏默认全屏、内置屏默认窗口，分辨率、刷新率、缩放按显示器本身推导；在 GNOME 设置里改过的配置按显示器分别记住，换 Mac、换显示器不需要任何“注册”。
- 鼠标像 scrcpy 一样：悬停直接跟随；**点击画面即捕获**，鼠标可以一路移到手机屏幕上；**单按 ⌥ 释放**。
- 剪贴板双向同步：文字、图片、文件（单次最多 512 MiB）。
- 声音：GNOME 里多出一个“XScreen（显示器名）”音频输出，连上即默认，从 Mac 当前的扬声器/耳机播放。
- 文件传输：把文件拖到 XScreen 窗口 → 进 GNOME 的“下载”；在“文件”里右键 › 脚本 › **发送到 Mac** → 进 Mac 的“下载”。两边都有通知。
- 也兼容原版 scrcpy 客户端（4.0+）：不装 XScreen 的电脑（Windows/Linux/Mac）用 `scrcpy` 也能接入这台桌面。

---

## 整体架构

```
┌──────────────────────────── 小米 14 ─────────────────────────────┐
│ Android（KernelSU root）                                            │
│   Anland 显示 App ← 画面 ─┐        Android 无线电（Wi‑Fi/蓝牙/基带）  │
│   android/ubuntu26        │                ▲                         │
│   android/ubuntu26-service│                │ app_process Java 代理    │
│        │ 挂载 400G 镜像、私有挂载命名空间、负载调优                       │
│        ▼                  │                │                         │
│ ┌──── Ubuntu 26.04 chroot（/data/local/ubuntu26/rootfs）──────────┐ │
│ │ session/ubuntu26-gnome：system D-Bus、磁盘、无线电桥接            │ │
│ │ gnome-shell + mutter（Anland 后端 + 本仓库补丁）─────────────────┘ │
│ │ radios/：NetworkManager / ModemManager / BlueZ 的 D-Bus 实现      │ │
│ │ xscreen/phone：xscreend ── 虚拟显示器 → 硬件编码 → TCP 7845        │ │
│ └──────────────────────────────────────────────────────┬─────────┘ │
└────────────────────────────────────────────────────────┼───────────┘
                                                USB（adb forward）
┌───────────────────────── Mac ───────────────────────────┼───────────┐
│ xscreen/mac：XScreen.app ── 硬件解码 → 显示；输入、剪贴板、声音、文件 │
│ （或）原版 scrcpy + xscreen/scrcpy-server 兼容层（端口 7846）          │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 前提条件

| 项目 | 要求 |
| --- | --- |
| 手机 | 小米 14（骁龙 8 Gen 3，Adreno 750，Android 的 kgsl GPU 驱动） |
| Root | KernelSU（脚本使用 `/data/adb/ksu/bin/busybox`，并用 `su -M` 进入全局挂载命名空间） |
| 显示 | [Anland: Termux](https://github.com/lfdevs/anland-termux) 5.13.x：Android 显示 App + Termux 守护进程 |
| 容器 | Ubuntu 26.04 arm64 rootfs：镜像 `/data/local/ubuntu26/ubuntu26.ext4`（ext4，loop 挂载到 `/data/local/ubuntu26/rootfs`） |
| 桌面 | Anland 发布的 Ubuntu 版 mutter（`50.1-0ubuntu91`），再打上本仓库的补丁 |
| Mac | Apple Silicon，macOS 14+，Homebrew 的 `adb`（`android-platform-tools`），Xcode 命令行工具；可选 `scrcpy` |

> Ubuntu 容器和 Anland 本身的安装，请先按 Anland 的官方指南完成（Ubuntu 26.04 + Chroot + Mutter）。本仓库是在那之上的改造。

---

## 手机上的完整目录布局

整套系统在手机上只占一个目录：`/data/local/ubuntu26`（root 所有，权限 700）。Ubuntu 的根文件系统挂在其中的 `rootfs` 下，所以**容器里的 `/X` 在 Android 上就是 `/data/local/ubuntu26/rootfs/X`**。下面每个文件同时给出两种写法：左边是 Android（`adb shell` / root shell）看到的完整路径，`#` 后面是它在容器里的路径和作用。

```text
/data/local/ubuntu26/                                   # 整套系统的根目录（Android 侧）
├── ubuntu26                                            # 启动器：挂载镜像、进入容器（android/ubuntu26）
├── ubuntu26-service                                    # 服务控制：gnome / ssh 启停、调优（android/ubuntu26-service）
├── ubuntu26.ext4                                       # Ubuntu 根文件系统镜像，预分配 400 GiB
├── bridge/
│   ├── native-network-agent.jar                        # 无线电代理，在 Android 侧用 app_process 运行
│   └── agent-startup.log                               # 代理启动日志
├── run/                                                # 运行状态（服务启动时写入）
│   ├── gnome                                           # GNOME 服务的 PID 和挂载命名空间
│   ├── ssh                                             # SSH 服务的 PID 和挂载命名空间
│   └── gpu-min-pwrlevel                                # 调优前的 GPU 最低档位，停止时据此还原
├── logs/
│   ├── gnome.log                                       # 本次 GNOME 会话的全部输出（gnome-shell、mutter）
│   ├── gnome.log.1                                     # 上一次会话
│   └── ssh.log
└── rootfs/                                             # ← ubuntu26.ext4 挂载点 = 容器里的 /
    ├── usr/local/sbin/
    │   ├── ubuntu26-gnome                              # /usr/local/sbin/ubuntu26-gnome：桌面会话入口
    │   ├── ubuntu26-sshd                               # /usr/local/sbin/ubuntu26-sshd：SSH 服务
    │   └── ubuntu26-network-allow.py                   # /usr/local/sbin/ubuntu26-network-allow.py：后台联网放行
    ├── usr/local/bin/
    │   ├── startgnome-anland                           # /usr/local/bin/startgnome-anland：Anland 的 GNOME 启动脚本
    │   ├── pkexec                                      # /usr/local/bin/pkexec：pkexec 垫片（排在 /usr/bin/pkexec 前）
    │   ├── xscreend                                    # /usr/local/bin/xscreend：XScreen 手机端守护进程
    │   └── xscreend-start                              # /usr/local/bin/xscreend-start：后台启动 xscreend 并写日志
    ├── usr/local/libexec/
    │   ├── ubuntu26-native-network                     # /usr/local/libexec/ubuntu26-native-network：无线电 D-Bus 服务
    │   ├── ubuntu26-rfkill                             # /usr/local/libexec/ubuntu26-rfkill：rfkill 接口
    │   ├── ubuntu26-system-disk                        # /usr/local/libexec/ubuntu26-system-disk：系统盘设备节点
    │   ├── ubuntu26-auth-agent                         # /usr/local/libexec/ubuntu26-auth-agent：Polkit 认证代理
    │   └── ubuntu26-hostnamed                          # /usr/local/libexec/ubuntu26-hostnamed：hostnamed 启动器
    ├── usr/local/share/dbus-1/system-services/
    │   └── org.freedesktop.hostname1.service           # hostnamed 的 D-Bus 按需启动
    ├── usr/lib/aarch64-linux-gnu/
    │   └── libvulkan_freedreno.so                      # 打过补丁的 Turnip（Vulkan）驱动
    ├── usr/share/gnome-shell/extensions/
    │   └── screen-safe-area@ubuntu26.local/            # 顶栏透明 / 避开挖孔与圆角的扩展
    ├── etc/dbus-1/system.d/
    │   ├── ubuntu26-radios.conf                        # ModemManager / BlueZ / 自定义接口的访问策略
    │   └── org.freedesktop.NetworkManager.conf         # NetworkManager 接口的访问策略
    ├── etc/xdg/autostart/
    │   ├── ubuntu26-rfkill.desktop                     # 登录时启动 rfkill 接口
    │   └── xscreend.desktop                            # 登录时启动 xscreend
    ├── home/ubuntu/                                    # 桌面用户（none，UID 60000）的主目录
    │   ├── .config/autostart/ubuntu26-package-auth.desktop   # 登录时启动认证代理
    │   ├── .config/monitors.xml                        # GNOME 保存的显示器配置（按显示器身份）
    │   ├── .local/share/applications/zcode.desktop     # ZCode 启动参数修正
    │   ├── .local/share/nautilus/scripts/发送到 Mac    # “文件”右键 › 脚本 › 发送到 Mac
    │   └── .local/state/
    │       ├── xscreend.log                            # xscreend 日志（上一次为 xscreend.log.1）
    │       └── anland/                                 # 无线电桥接、系统服务、Anland 的日志
    ├── opt/xscreen/                                    # 在手机上构建用的工作目录
    │   ├── phone/                                      # xscreend 源码（本仓库 xscreen/phone）与 build/
    │   ├── protocol/                                   # xs-protocol.h（本仓库 xscreen/protocol）
    │   ├── build/                                      # mutter 源码与构建出的 .deb
    │   └── backup/                                     # 替换系统文件前的备份（不要放在 lib 目录里）
    ├── tmp/anland/display_daemon.sock                  # ← 绑定自 Termux 的 /data/data/com.termux/files/usr/tmp/anland
    └── dev/                                            # 每个会话私有的 tmpfs，只绑定需要的设备：
                                                        #   kgsl-3d0（GPU）、dri/、dma_heap/、高通视频编解码节点（如 video33）
```

其他会用到的 Android 侧路径：

| 路径 | 用途 |
| --- | --- |
| `/data/adb/ksu/bin/busybox` | KernelSU 自带的 busybox，启动脚本都用它 |
| `/data/data/com.termux/files/usr/tmp/anland/` | Anland 守护进程的 socket 目录，绑定进容器的 `/tmp/anland` |
| `/sys/class/kgsl/kgsl-3d0/min_pwrlevel` | GPU 最低频率档位（调优时修改，停止时还原） |
| `/proc/1/root/sys/fs/bpf/netd_shared/map_netd_uid_owner_map` | Android 网络策略的 BPF 表（只为 UID 60000 加“允许后台”） |

---

## 仓库结构

每个目录对应一块功能；“装到哪里”一列是它在手机上的完整路径（Android 侧看到的路径）。

| 路径 | 作用 | 装到哪里 |
| --- | --- | --- |
| `android/ubuntu26` | 挂载镜像、建立私有挂载命名空间、进入容器 | `/data/local/ubuntu26/ubuntu26` |
| `android/ubuntu26-service` | GNOME / SSH 的启停与状态，负载调优 | `/data/local/ubuntu26/ubuntu26-service` |
| `session/ubuntu26-gnome` | 桌面会话入口：system D-Bus、系统盘、无线电桥接、启动 GNOME | `/data/local/ubuntu26/rootfs/usr/local/sbin/ubuntu26-gnome` |
| `session/startgnome-anland` | Anland 的 GNOME 启动脚本（小改，见下文） | `/data/local/ubuntu26/rootfs/usr/local/bin/startgnome-anland` |
| `session/ubuntu26-sshd` | 容器内的 SSH 服务 | `/data/local/ubuntu26/rootfs/usr/local/sbin/ubuntu26-sshd` |
| `session/ubuntu26-network-allow.py` | 让 Ubuntu 用户在 Android 后台网络策略下联网 | `/data/local/ubuntu26/rootfs/usr/local/sbin/ubuntu26-network-allow.py` |
| `session/ubuntu26-hostnamed` | “设置 › 关于”所需的 hostnamed 启动器 | `/data/local/ubuntu26/rootfs/usr/local/libexec/ubuntu26-hostnamed` |
| `session/org.freedesktop.hostname1.service` | hostnamed 的 D-Bus 按需启动 | `/data/local/ubuntu26/rootfs/usr/local/share/dbus-1/system-services/org.freedesktop.hostname1.service` |
| `patches/mutter/` | mutter 补丁 0021–0025 | 在 `/data/local/ubuntu26/rootfs/opt/xscreen/build` 构建成 mutter 包 |
| `patches/mesa/` | Turnip（Vulkan）在 kgsl 上的修复 | 构建出 `/data/local/ubuntu26/rootfs/usr/lib/aarch64-linux-gnu/libvulkan_freedreno.so` |
| `patches/deploy-mutter.sh` | 在手机上安装自建 mutter 包并重启 GNOME | 在 Mac 上运行 |
| `radios/ubuntu26-native-network` | 无线电 D-Bus 服务 | `/data/local/ubuntu26/rootfs/usr/local/libexec/ubuntu26-native-network` |
| `radios/ubuntu26-rfkill` | rfkill 接口 | `/data/local/ubuntu26/rootfs/usr/local/libexec/ubuntu26-rfkill` |
| `radios/ubuntu26-rfkill.desktop` | rfkill 接口自启动 | `/data/local/ubuntu26/rootfs/etc/xdg/autostart/ubuntu26-rfkill.desktop` |
| `radios/ubuntu26-radios.conf` | D-Bus 访问策略 | `/data/local/ubuntu26/rootfs/etc/dbus-1/system.d/ubuntu26-radios.conf` |
| `radios/org.freedesktop.NetworkManager.conf` | D-Bus 访问策略 | `/data/local/ubuntu26/rootfs/etc/dbus-1/system.d/org.freedesktop.NetworkManager.conf` |
| `radios/native-network-agent.jar` | Android 侧无线电代理（源码 `radios/agent/`） | `/data/local/ubuntu26/bridge/native-network-agent.jar` |
| `storage/ubuntu26-system-disk` | 让 UDisks 看到“Ubuntu 系统盘” | `/data/local/ubuntu26/rootfs/usr/local/libexec/ubuntu26-system-disk` |
| `shell-extension/screen-safe-area@ubuntu26.local/` | 透明顶栏，避开挖孔和圆角 | `/data/local/ubuntu26/rootfs/usr/share/gnome-shell/extensions/screen-safe-area@ubuntu26.local/` |
| `auth/ubuntu26-auth-agent` | Polkit 认证代理 | `/data/local/ubuntu26/rootfs/usr/local/libexec/ubuntu26-auth-agent` |
| `auth/pkexec` | pkexec 垫片 | `/data/local/ubuntu26/rootfs/usr/local/bin/pkexec` |
| `auth/ubuntu26-package-auth.desktop` | 认证代理自启动 | `/data/local/ubuntu26/rootfs/home/ubuntu/.config/autostart/ubuntu26-package-auth.desktop` |
| `apps/zcode.desktop` | ZCode 的启动参数修正 | `/data/local/ubuntu26/rootfs/home/ubuntu/.local/share/applications/zcode.desktop` |
| `xscreen/protocol/` | XScreen 线协议（C 头文件，两端共用） | — |
| `xscreen/phone/` | 手机端守护进程 `xscreend`（C，meson） | 源码 `/data/local/ubuntu26/rootfs/opt/xscreen/phone`，安装位置见[安装部署](#8-xscreen) |
| `xscreen/mac/` | Mac 应用 XScreen.app（Swift） | Mac |
| `xscreen/scrcpy-server/` | 原版 scrcpy 客户端的兼容层与启动脚本 | Mac |
| `xscreen/probe/` | 开发时用的采集/探测小工具 | — |

---

## 各部分是怎么工作的

### 1. 启动链

```
su -M -c '/data/local/ubuntu26/ubuntu26-service gnome start'
  └─ ubuntu26 exec /usr/local/sbin/ubuntu26-gnome     （新的私有挂载命名空间）
       ├─ ubuntu26-system-disk                          （UDisks 用的设备节点）
       ├─ dbus-daemon --system
       ├─ ubuntu26-native-network                       （无线电 D-Bus 服务）
       └─ startgnome-anland → gnome-session → gnome-shell
```

- **`ubuntu26`** 负责把 `/data/local/ubuntu26/ubuntu26.ext4` loop 挂到 `/data/local/ubuntu26/rootfs`，然后为每个会话 `unshare` 出一个私有挂载命名空间：`/dev`（只绑定需要的节点：GPU `kgsl-3d0`、`dri`、`dma_heap`、高通视频编解码节点）、`/proc`、`/sys`、`/run` 都只在这个命名空间里可见，Android 自己的视图不受影响。它还只把 Anland 的 IPC 目录共享进来，Termux 其他文件保持私有。
- **`ubuntu26-service`** 记录每个服务的 PID 和挂载命名空间，用它们来判断“真的在运行”（不只是看 PID），停止时只杀属于这个命名空间的进程。`gnome` 启动后会自动执行一次 `tune`。
- **均衡调优**（`tune`）：GNOME 运行期间把 GPU 最低频率设到 366 MHz（GPU 空闲时照样掉电，下限只在工作时生效，避免短动画和推流时卡在最低档）；给 gnome-shell 设 `uclamp.min=250`。停止 GNOME 时还原。
- **联网**：Android 的后台网络策略会拦截没注册过的 Linux UID。`ubuntu26-network-allow.py` 只在 netd 的 BPF 表里给 Ubuntu 的 UID 60000 加一个“允许后台”标记，不动其他任何规则。
- **`startgnome-anland`** 相对 Anland 原版只有三处改动：允许生成有上限的 core dump；不再用 root 去改目录权限（由 `ubuntu26-gnome` 事先准备好）；system bus 健康检查改用 `ListNames`。

### 2. 图形：mutter 补丁

基于 Anland 的 mutter `50.1-0ubuntu91`（已应用其 debian/patches），我们加了 5 个补丁：

| 补丁 | 解决的问题 |
| --- | --- |
| `0021-anland-redraw-only-damage` | Anland 的显示 App 归还缓冲区的顺序和发出顺序不同。给每块缓冲区记录“上次画到哪一帧”，告诉合成器真实的 buffer age，于是每帧只重画变化的部分。 |
| `0022-screen-cast-wait-for-render-fence` | 屏幕录制（XScreen 用的就是它）在 GPU 画完之前就把 dma-buf 交出去了。高通 kgsl 没有 dma-buf 隐式同步，消费者会读到画了一半的帧。现在等渲染 fence 信号后再交付。 |
| `0023-screen-cast-virtual-monitor-identity` | 让 `RecordVirtual` 创建的虚拟显示器可以带上厂商/型号/序列号。XScreen 用 Mac 显示器的名字和 UUID 作身份，GNOME 才能按显示器分别记住设置。 |
| `0024-anland-vblank-frame-timing` | 帧时钟改用面板真实的 vblank 时间戳（`/dev/dri/card0`，无需 DRM master）和 GPU 完成时间，动画节奏不再靠估算。 |
| `0025-anland-keep-scale-across-rotation` | 旋转时保持当前缩放；首次使用时按“短边约 600 逻辑像素”自动给出缩放。 |

> `patches/mutter/` 里的 5 个补丁按顺序打在基线上，与手机上运行的源码逐字节一致（已验证）。

### 3. 图形：Vulkan（Turnip）

lfdevs 的 Mesa 分支（`dev/adreno-main`）在 kgsl 上提交命令时会写到两个被内核保护的寄存器（`RB_CCU_CNTL`、`RB_CCU_DBG_ECO_CNTL`，报 `CP_PROTECT` 故障）。`patches/mesa/0001` 在 kgsl 上跳过它们，并用 `CP_NON_CONTEXT_REG_BUNCH` 写其余非上下文寄存器（与 freedreno 在 A7XX 上的做法一致）。

### 4. 无线电：网络、蓝牙、移动数据

原则：**Android 始终是无线电的唯一拥有者**，桌面只是一个“遥控器”。

- `radios/ubuntu26-native-network`（root 运行的 Python）在 system bus 上实现了 GNOME 需要的接口：`org.freedesktop.NetworkManager`（Wi‑Fi 与移动网络）、`org.freedesktop.ModemManager1`、`org.bluez`，以及我们自己的 `local.ubuntu26.Radios`、`local.ubuntu26.Display`（后者把屏幕挖孔、圆角、旋转告诉顶栏扩展）。
- 真正的操作由 `radios/agent/NetAgent.java` 完成：它通过 Android 的 `app_process` 在 Android 侧运行，调用系统 API（需要 `ActivityThread.initializeMainlineModules` 才能拿到 Telephony/Bluetooth 服务；设置项走 `settings` 命令）。编译好的 jar 放在 Android 侧的 `/data/local/ubuntu26/bridge/native-network-agent.jar`。
- `radios/ubuntu26-rfkill` 在 session bus 上替代 gnome-settings-daemon 的 rfkill 接口，让快速设置里的开关、飞行模式与手机状态一致。
- `radios/ubuntu26-radios.conf`、`org.freedesktop.NetworkManager.conf` 是对应的 D-Bus 访问策略。

### 5. 存储：独立的 Ubuntu 系统盘

- Ubuntu 的根文件系统是一个**完全预分配**的 400 GiB 镜像（`fallocate`），所以这块空间是真正预留给 Ubuntu 的，Android 那边剩多少一目了然。
- `ubuntu26` 在挂载前关闭 loop 设备的 discard，防止 ext4 的 discard 把预留空间“打洞”还给 Android。
- 容器里没有 udev，UDisks 因此看不到任何磁盘。`storage/ubuntu26-system-disk` 只为这一个 loop 设备建节点和 udev 记录（标记为系统盘，名字“系统盘”），GNOME 的“关于”、“磁盘”、“文件”就能正确显示 Ubuntu 自己的容量。手机的分区不会出现在桌面里。

### 6. 屏幕安全区（顶栏扩展）

`shell-extension/screen-safe-area@ubuntu26.local`：从 `local.ubuntu26.Display` 读取当前旋转下的挖孔和圆角形状，
- 顶栏透明（Yaru 主题用了 `!important`，所以通过样式类覆盖）；
- 顶栏左右两侧按圆角留出空隙；
- 横屏时用 strut 让窗口避开挖孔区域；
- 时钟位置保持不动。

### 7. 权限认证与应用修正

容器里没有 logind，GNOME Shell 自带的 Polkit 认证代理注册不上，`pkexec` / 软件安装会一直等。

- `auth/ubuntu26-auth-agent`：按进程注册的 Polkit 认证代理（GTK 对话框）。开机自启早于合成器就绪时会自动重试；异常时返回失败而不是卡住。
- `auth/pkexec`：放在 `/usr/local/bin`，在 `PATH` 里排在 `/usr/bin/pkexec` 前面的垫片。普通用户调用时先向代理登记，再执行原版 `pkexec`，保留原版的 PAM 语义、参数、环境和退出码。
- `auth/ubuntu26-package-auth.desktop`：代理的自启动项（崩溃后自动重启）。

### 8. 应用兼容

`apps/zcode.desktop`：ZCode（Electron）以 `--disable-gpu-rasterization` 启动。kgsl 没有 dma-buf 隐式同步，Chromium 的 GPU 光栅化会让文字花屏；改成 CPU 光栅化后正常，合成仍然走 GPU。这个文件的注释与 ZCode 自己写的不同，ZCode 更新时不会覆盖它。

### 9. XScreen：Mac 扩展屏

**手机端 `xscreend`**（`xscreen/phone/`，C）
- 通过 mutter 的 RemoteDesktop + ScreenCast（`RecordVirtual`）创建一块虚拟显示器，身份取自 Mac 显示器（名字 + UUID）。
- 从 PipeWire 拿 dma-buf 帧，用 GLES compute shader 转 NV12，只转换有变化的区域（按每块编码输入缓冲区分别跟踪损伤）。
- 交给高通硬件编码器（V4L2，`/dev/video33`）编 HEVC，40 Mbit/s；画面静止约 150 ms 后补一帧高质量关键帧，文字更锐利。
- 输入通过 libei 注入；剪贴板走 RemoteDesktop 的剪贴板接口；声音是一个 PipeWire `Audio/Sink`；文件收发、通知、“发送到 Mac”的 D-Bus 方法（`local.xscreen.XScreen.SendFiles`）也都在这里。
- 只接受 root 和 adb shell（UID 0/2000）的连接，同一时间只服务一个客户端。

**线协议**（`xscreen/protocol/xs-protocol.h`）：12 字节消息头（魔数、类型、长度）+ 负载，小端。视频、光标、剪贴板、音频、文件各有消息类型；新消息对旧客户端是可忽略的。

**Mac 端 XScreen.app**（`xscreen/mac/`，Swift）
- **显示**：VideoToolbox 硬件解码，每帧解码完的 IOSurface 直接作为普通图层内容显示，无拷贝。（最初用的 `AVSampleBufferDisplayLayer` 在经过缩放的外接屏上会卡住几秒，所以换掉了。）
- **显示器适配**：窗口打开在上次用过的显示器上（否则优先外接屏）；拖到另一块显示器时自动以那块显示器的身份重连。外接屏首选模式为原生分辨率（上限 4K 级别，保护手机负载），缩放让界面大小与 Mac 一致；内置屏首选 80% 大小的窗口。
- **鼠标**：悬停时 GNOME 的指针跟随 Mac 指针，由 macOS 在本地绘制 GNOME 的光标样式，零延迟；点击即捕获，发送相对移动，可移到手机屏幕；单按 ⌥ 或切走窗口即释放。
- **其他**：剪贴板轮询同步、AVAudioEngine 播放（约 40 ms 抖动缓冲）、文件拖放与接收通知；连接期间向 macOS 声明“延迟敏感”，避免被 App Nap 节流。

**scrcpy 兼容层**（`xscreen/scrcpy-server/`）
- `xscreen-scrcpy-server`：替代 scrcpy 推到手机上的 server，本身只做转发：把客户端的视频/控制连接接到 `xscreend` 的 7846 端口。
- `xscreen-scrcpy`：启动脚本。会先让 XScreen 让出连接、结束后再恢复；键盘用 UHID（GNOME 的输入法生效），鼠标用绝对坐标；运行期间由 `xscreen-ime-guard` 把 Mac 的输入法切到英文布局，避免搜狗等输入法吞掉按键。

---

## 安装部署

> 下面假设 Anland + Ubuntu 26.04 chroot 已经能启动 GNOME。所有手机侧命令都需要 root。替换正在使用的文件时，用“复制到旁边再改名”的方式（Ubuntu 26 的 uutils `install` 不能覆盖正在运行的文件）。

下面所有路径都是 Android 侧的完整路径（在 `adb shell` 里 `su` 之后使用）。容器里的同一个文件，去掉开头的 `/data/local/ubuntu26/rootfs` 即可。

### 1. 启动脚本（Android 侧）

```bash
adb push android/ubuntu26 android/ubuntu26-service /data/local/tmp/
adb shell "su -M -c 'cp /data/local/tmp/ubuntu26 /data/local/tmp/ubuntu26-service /data/local/ubuntu26/ && chmod 700 /data/local/ubuntu26/ubuntu26 /data/local/ubuntu26/ubuntu26-service'"
```

### 2. 桌面会话

| 文件 | 完整路径 | 权限 |
| --- | --- | --- |
| `session/ubuntu26-gnome` | `/data/local/ubuntu26/rootfs/usr/local/sbin/ubuntu26-gnome` | 755 |
| `session/ubuntu26-sshd` | `/data/local/ubuntu26/rootfs/usr/local/sbin/ubuntu26-sshd` | 755 |
| `session/ubuntu26-network-allow.py` | `/data/local/ubuntu26/rootfs/usr/local/sbin/ubuntu26-network-allow.py` | 755 |
| `session/startgnome-anland` | `/data/local/ubuntu26/rootfs/usr/local/bin/startgnome-anland` | 755 |
| `session/ubuntu26-hostnamed` | `/data/local/ubuntu26/rootfs/usr/local/libexec/ubuntu26-hostnamed` | 755 |
| `session/org.freedesktop.hostname1.service` | `/data/local/ubuntu26/rootfs/usr/local/share/dbus-1/system-services/org.freedesktop.hostname1.service` | 644 |

### 3. mutter 与 Mesa

见[从源码构建](#从源码构建)。mutter 包装好后，在 Mac 上运行：

```bash
patches/deploy-mutter.sh
```

它会安装 `/data/local/ubuntu26/rootfs/opt/xscreen/build` 里的包并重启 GNOME。

> ⚠️ 不要把旧的 `.so` 备份放在 `/data/local/ubuntu26/rootfs/usr/lib/aarch64-linux-gnu` 里。`ldconfig` 会把它当成库，把 `libmutter` 的软链接指错，gnome-shell 就起不来了。备份请放到别处（我们用 `/data/local/ubuntu26/rootfs/opt/xscreen/backup/`）。

### 4. 无线电（网络、蓝牙、移动数据）

| 文件 | 完整路径 | 权限 |
| --- | --- | --- |
| `radios/ubuntu26-native-network` | `/data/local/ubuntu26/rootfs/usr/local/libexec/ubuntu26-native-network` | 755 |
| `radios/ubuntu26-rfkill` | `/data/local/ubuntu26/rootfs/usr/local/libexec/ubuntu26-rfkill` | 755 |
| `radios/ubuntu26-radios.conf` | `/data/local/ubuntu26/rootfs/etc/dbus-1/system.d/ubuntu26-radios.conf` | 644 |
| `radios/org.freedesktop.NetworkManager.conf` | `/data/local/ubuntu26/rootfs/etc/dbus-1/system.d/org.freedesktop.NetworkManager.conf` | 644 |
| `radios/ubuntu26-rfkill.desktop` | `/data/local/ubuntu26/rootfs/etc/xdg/autostart/ubuntu26-rfkill.desktop` | 644 |
| `radios/native-network-agent.jar` | `/data/local/ubuntu26/bridge/native-network-agent.jar` | 400 |

`radios/install.sh` 是当初从旧方案迁移时用的脚本（会顺带删除旧的 android-connectivity 扩展），可作参考。

### 5. 存储

| 文件 | 完整路径 | 权限 |
| --- | --- | --- |
| `storage/ubuntu26-system-disk` | `/data/local/ubuntu26/rootfs/usr/local/libexec/ubuntu26-system-disk` | 755 |

镜像 `/data/local/ubuntu26/ubuntu26.ext4` 用 `fallocate` 预分配到想要的大小（我们是 400 GiB），再 `resize2fs`。`ubuntu26` 会自动关闭 discard。

### 6. 顶栏扩展

| 文件 | 完整路径 |
| --- | --- |
| `shell-extension/screen-safe-area@ubuntu26.local/` | `/data/local/ubuntu26/rootfs/usr/share/gnome-shell/extensions/screen-safe-area@ubuntu26.local/` |

然后在“扩展”应用里启用，或：`gnome-extensions enable screen-safe-area@ubuntu26.local`。

### 7. 权限认证与应用修正

| 文件 | 完整路径 | 权限 |
| --- | --- | --- |
| `auth/ubuntu26-auth-agent` | `/data/local/ubuntu26/rootfs/usr/local/libexec/ubuntu26-auth-agent` | 755 |
| `auth/pkexec` | `/data/local/ubuntu26/rootfs/usr/local/bin/pkexec` | 755 |
| `auth/ubuntu26-package-auth.desktop` | `/data/local/ubuntu26/rootfs/home/ubuntu/.config/autostart/ubuntu26-package-auth.desktop` | 644，属主 60000 |
| `apps/zcode.desktop` | `/data/local/ubuntu26/rootfs/home/ubuntu/.local/share/applications/zcode.desktop` | 644，属主 60000 |

### 8. XScreen

手机端（在容器里构建，见下一节）：

| 文件 | 完整路径 | 权限 |
| --- | --- | --- |
| `xscreen/phone/` 源码 | `/data/local/ubuntu26/rootfs/opt/xscreen/phone/`（`xscreen/protocol/` 放到 `/data/local/ubuntu26/rootfs/opt/xscreen/protocol/`） | — |
| `xscreend`（构建产物） | `/data/local/ubuntu26/rootfs/usr/local/bin/xscreend` | 755 |
| `xscreen/phone/data/xscreend-start` | `/data/local/ubuntu26/rootfs/usr/local/bin/xscreend-start` | 755 |
| `xscreen/phone/data/xscreend.desktop` | `/data/local/ubuntu26/rootfs/etc/xdg/autostart/xscreend.desktop` | 644 |
| `xscreen/phone/data/发送到 Mac` | `/data/local/ubuntu26/rootfs/home/ubuntu/.local/share/nautilus/scripts/发送到 Mac` | 755，属主 60000 |

Mac 端：

```bash
xscreen/mac/bundle.sh                 # 生成 xscreen/mac/build/XScreen.app
open xscreen/mac/build/XScreen.app
```

---

## 日常使用

**启动 / 停止桌面**（在 Mac 上，或手机的 root shell 里去掉 `adb shell`）：

```bash
adb shell "su -M -c '/data/local/ubuntu26/ubuntu26-service gnome start'"
adb shell "su -M -c '/data/local/ubuntu26/ubuntu26-service gnome stop'"
adb shell "su -M -c '/data/local/ubuntu26/ubuntu26-service status'"
```

**进入容器的命令行**：

```bash
adb shell "su -M -c '/data/local/ubuntu26/ubuntu26'"
```

**XScreen**

| 操作 | 方法 |
| --- | --- |
| 连接 | 插上 USB，打开 XScreen.app（会自动建立 adb 转发，断开后自动重连） |
| 换显示器 | 直接把窗口拖过去，约 2 秒后以新显示器身份重连 |
| 分辨率 / 缩放 / 位置 | GNOME“设置 › 显示器”，按显示器分别记住 |
| 把鼠标交给 GNOME（可移到手机屏） | 在画面里点一下；窗口副标题会显示“按 ⌥ 释放鼠标” |
| 收回鼠标 | 单按 ⌥，或切换到别的应用 |
| Mac → GNOME 传文件 | 拖到 XScreen 窗口，存进 GNOME 的“下载” |
| GNOME → Mac 传文件 | “文件”里右键 › 脚本 › 发送到 Mac，存进 Mac 的“下载” |
| 复制粘贴 | 两边照常复制粘贴：文字、图片、文件（≤ 512 MiB） |
| 声音 | 自动；也可以在 GNOME“设置 › 声音”里选输出 |

**用原版 scrcpy 接入**（不装 XScreen 的电脑也行）：

```bash
xscreen/scrcpy-server/xscreen-scrcpy 2560x1440
```

默认鼠标用绝对坐标、不捕获；加 `-M`（UHID 鼠标）时鼠标会被捕获，用 Alt/⌘ 释放。scrcpy 客户端不支持把鼠标移到手机屏幕，需要的话请用 XScreen。

---

## 从源码构建

### mutter

```bash
git clone https://github.com/lfdevs/mutter.git      # 版本 50.1-0ubuntu91
# 应用其 debian/patches，再按顺序应用本仓库的 patches/mutter/00*.patch
# 在 debian/changelog 里把版本号加上本地后缀（我们用 +xsN），然后在手机容器里：
dpkg-buildpackage -b -uc -us
```

我们在手机上的 `/data/local/ubuntu26/rootfs/opt/xscreen/build`（容器里的 `/opt/xscreen/build`）里构建，产物由 `patches/deploy-mutter.sh` 安装。

### Mesa（Turnip）

```bash
git clone https://github.com/lfdevs/mesa-for-android-container.git   # 分支 dev/adreno-main
git apply patches/mesa/0001-turnip-kgsl-skip-protected-ccu-regs.patch
```

按该仓库的说明构建 freedreno Vulkan 驱动（kgsl），替换 `/data/local/ubuntu26/rootfs/usr/lib/aarch64-linux-gnu/libvulkan_freedreno.so`（原文件备份到 `/data/local/ubuntu26/rootfs/opt/xscreen/backup/`，不要留在 lib 目录里）。

### xscreend（在手机容器里）

```bash
apt install meson ninja-build pkgconf libglib2.0-dev libpipewire-0.3-dev libei-dev \
            libegl-dev libgles-dev libdrm-dev
cd /opt/xscreen/phone        # Android 侧即 /data/local/ubuntu26/rootfs/opt/xscreen/phone
meson setup build
ninja -C build
meson test -C build          # 单元测试：损伤跟踪、scrcpy 协议
cp build/xscreend /usr/local/bin/xscreend.new && mv -f /usr/local/bin/xscreend.new /usr/local/bin/xscreend
```

### Mac 应用

```bash
xscreen/mac/bundle.sh
```

### 两个 Android 侧小 jar

`radios/native-network-agent.jar` 和 `xscreen/scrcpy-server/xscreen-scrcpy-server` 都是很小的 dex 包，仓库里直接附带了构建好的版本。要自己编：用 `javac --release 11` 编译（classpath 指向 Android 的 `android.jar`），再用 d8 转成 `classes.dex` 打进 jar：

```bash
javac --release 11 -cp android.jar -d classes <源文件>
java -cp r8lib.jar com.android.tools.r8.D8 --release --min-api 31 --lib android.jar \
     --output dex $(find classes -name '*.class')
(cd dex && jar cf ../输出.jar classes.dex)
```

---

## 排错与诊断

**日志在哪**

| 内容 | 位置 |
| --- | --- |
| GNOME（gnome-shell、mutter） | `/data/local/ubuntu26/logs/gnome.log`（上一次的在 `/data/local/ubuntu26/logs/gnome.log.1`） |
| xscreend | `/data/local/ubuntu26/rootfs/home/ubuntu/.local/state/xscreend.log`（上一次的在 `xscreend.log.1`） |
| 无线电桥接、系统服务 | `/data/local/ubuntu26/rootfs/home/ubuntu/.local/state/anland/` |
| 无线电代理（Android 侧） | `/data/local/ubuntu26/bridge/agent-startup.log` |
| GNOME 保存的显示器配置 | `/data/local/ubuntu26/rootfs/home/ubuntu/.config/monitors.xml` |

**XScreen 的诊断开关**

- 手机端：`XS_DEBUG_TIMING=1` 启动 xscreend，日志每 5 秒一行，包括各阶段耗时（采集→转换→编码→发送）、收到的输入事件数、“按下到新画面”的延迟。`XS_DEBUG=1` 会打印每一帧的细节，量很大，只在短时间排查时用（它本身会拖慢采集线程）。
- Mac 端：`open --env XSCREEN_DEBUG=1 --stdout 日志 --stderr 日志 XScreen.app`，每 5 秒输出解码帧数、发出的输入事件数、窗口焦点和到达间隔。
  ⚠️ 请用 `open` 启动，不要在某些后台 shell 里直接执行二进制：子进程会继承 shell 的低优先级（nice），表现为“卡几秒”。

**看数据判断瓶颈**：Mac 发出的输入数 ≈ 手机收到的输入数，说明输入没问题；手机产出的帧数 ≈ Mac 收到的帧数，说明传输没问题；两者都正常而画面不动，问题在 Mac 的显示环节。

**常见问题**

| 现象 | 原因与处理 |
| --- | --- |
| gnome-shell 起不来，日志里 libmutter 找不到符号 | lib 目录里有 `.so` 备份被 `ldconfig` 选中，移走后 `ldconfig` |
| Electron/Chromium 应用文字花屏 | 加 `--disable-gpu-rasterization`（见 `apps/zcode.desktop`） |
| 画面全绿 | 编码器拿到的是全零缓冲（GPU 转换没生效）。日志里每次连接都有 `first frame N KiB`，正常是几百 KiB，异常时接近 0 |
| XScreen 在某块显示器上画面不跟手 | 用 Mac 端诊断对比输入数和帧数，定位在哪一段 |
| GNOME 好像没记住显示器设置 | 设置是按“显示器身份”记的；先确认 XScreen 窗口确实在那块显示器上 |

---

## 已知限制

- **kgsl 没有 dma-buf 隐式同步**：Chromium/Electron 的 GPU 光栅化需要关掉；Chromium 在 Wayland 下也不能用 Vulkan。
- **手机单独使用时跨旋转的显示配置**：GNOME 按“已连接的显示器组合”保存配置，手机屏横竖屏的分辨率不同，所以只有手机屏时，存下的配置只对保存时的方向有效。
- **一次只服务一个客户端**：XScreen 和 scrcpy 不能同时连接（`xscreen-scrcpy` 会自动让 XScreen 让位）。
- **声音只有输出**：Mac 的麦克风暂不送进 GNOME。
- **scrcpy 客户端**：不能把鼠标移到手机屏；不传声音。

---

## 上游与致谢

- [Anland: Termux](https://github.com/lfdevs/anland-termux)（lfdevs）：Android 上的 Wayland 显示通道和 Anland 版 mutter
- [lfdevs/mutter](https://github.com/lfdevs/mutter)：mutter 50.1-0ubuntu91（Anland 后端）
- [lfdevs/mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container)：带 kgsl 支持的 Mesa
- [scrcpy](https://github.com/Genymobile/scrcpy)（Genymobile）：兼容层实现了它的协议
- GNOME、PipeWire、libei、Ubuntu
