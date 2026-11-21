# 小智 AI 音乐固件（ESP32-S3）

在 ESP32-S3 上运行的「小智 AI」语音助手固件，**增加了通过语音点播在线音乐的能力**。

说出「你好小智，播放晴天」，设备会自动向局域网内的 [MeowMusicServer](https://github.com/OmniX-Space/MeowMusicServer) 请求歌曲，拿到 24kHz 单声道 MP3 流后直接解码播放——不需要手机、不需要 App、不需要任何按键。

本项目基于 [OmniX-Space/xiaozhi-esp32](https://github.com/OmniX-Space/xiaozhi-esp32) 修改，上游完整说明见 [README_UPSTREAM.md](README_UPSTREAM.md)。

> 📦 **不想自己编译？** 直接下载 [最新已编译固件](https://github.com/joe12801/xiaozhi-esp32-music/releases/latest)。
> 两种包：**整包 bin**（烧录地址 `0x0`，会覆盖 NVS，需重新配网）与 **app-only 包**（烧录地址 `0x20000`，保留 WiFi 凭据与设备绑定，升级已有设备推荐用这个）。
> 注意：发布包**不含 WiFi 凭据**，服务器地址是示例值 `192.168.1.100` —— 不是你的 IP 就得自己改 `main/server_config.h` 重新编译。

---

## 功能特性

- **语音点歌**：直接说歌名或「歌手 + 歌名」，自动匹配并播放
- **播放控制**：下一首、上一首、停止播放
- **歌单与收藏**：读取服务器端歌单列表，按歌单连播
- **设备绑定**：把 ESP32 与服务器上的用户账号绑定，共享收藏与歌单
- **断网自恢复**：内置兜底 WiFi 凭据，刷机后无需重新配网
- **完整语音交互**：保留上游的唤醒词识别、屏幕显示、音量控制、闹钟等能力

---

## 硬件要求

| 项目 | 要求 |
|---|---|
| 芯片 | ESP32-S3（建议带 8MB PSRAM） |
| Flash | 16MB（分区表使用 `partitions/v2/16m.csv`） |
| 开发板 | `bread-compact-wifi`（面包板搭的紧凑 WiFi 版） |
| 显示屏 | SSD1306 OLED 128×32（I2C） |
| 麦克风 | INMP441（I2S） |
| 功放 | MAX98357A（I2S） |
| 喇叭 | 4Ω/8Ω 3W |

> 其他板型可在 `menuconfig` 里切换，但本项目只在上述配置上实测通过。
> 如果用 128×64 的 OLED，把 `CONFIG_OLED_SSD1306_128X32` 换成 `CONFIG_OLED_SSD1306_128X64`。

---

## 快速开始

### 1. 准备环境（Windows / Linux 二选一）

需要 [ESP-IDF v5.5.x](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/)。

#### Windows

**方式 A：官方安装器（新手推荐）**

1. 打开 https://dl.espressif.com/dl/esp-idf/ ，下载 **ESP-IDF Installer**（Windows 版）
2. 安装时选择 **ESP-IDF v5.5.x**
3. 装完后从开始菜单打开 **「ESP-IDF 5.5 PowerShell」** —— 这个终端里环境已经自动加载好了，直接就能用 `idf.py`

**方式 B：已有本地 IDF 环境（本项目作者的方式）**

在普通 PowerShell 里手动加载环境：

```powershell
E:\Espressif\frameworks\esp-idf-v5.5.4\export.ps1
```

> 注意：`export.ps1` 必须用 `. ` 调用（点 + 空格），否则环境变量不会留在当前会话。

#### Linux

**① 装系统依赖（Ubuntu / Debian）**

```bash
sudo apt update
sudo apt install -y git wget flex bison gperf python3 python3-pip python3-venv \
  cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
```

**② 拉取 ESP-IDF v5.5.4 并安装工具链**

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git

cd ~/esp/esp-idf
./install.sh esp32s3      # 只装 S3 的工具链，省几 GB 空间
```

**③ 每次开新终端加载环境**

```bash
. $HOME/esp/esp-idf/export.sh
```

嫌麻烦可以加个别名：

```bash
echo 'alias get_idf=". $HOME/esp/esp-idf/export.sh"' >> ~/.bashrc
source ~/.bashrc
# 以后直接敲 get_idf
```

#### 串口设备名（两个平台不一样）

| 平台 | 设备名 | 查看方式 |
|---|---|---|
| Windows | `COM3`、`COM4`… | 设备管理器 → 端口，或 `mode` |
| Linux | `/dev/ttyUSB0`、`/dev/ttyACM0` | `ls /dev/ttyUSB* /dev/ttyACM*` |

Linux 下普通用户默认**没有串口权限**，会报 `Permission denied`。执行一次并重新登录：

```bash
sudo usermod -aG dialout $USER
```

> CH340 芯片的板子在 Linux 上通常是 `/dev/ttyUSB0`；原生 USB 的 S3 是 `/dev/ttyACM0`。
> 如果插上后两个都没有，检查 `dmesg | tail` 有没有识别到设备。

### 2. 修改两处配置（必做）

**① WiFi 名称和密码** — `main/main.cc`（可选）

```c
#define FALLBACK_WIFI_SSID     ""     // 填你的 WiFi 名，留空则走正常配网
#define FALLBACK_WIFI_PASSWORD ""     // 填你的 WiFi 密码
```

留空是默认行为：固件不内置任何 WiFi，刷机后首次启动进入配网流程（手机连设备热点配置）。
填上则会在 NVS 里没有任何 WiFi 记录时**自动写入**，省掉配网步骤；已配过网时不会覆盖。

也可以在命令行覆盖，不必改源码：

```bash
idf.py -DFALLBACK_WIFI_SSID=\"MyWiFi\" -DFALLBACK_WIFI_PASSWORD=\"12345678\" build
```

**② 音乐服务器地址** — `main/server_config.h`

```c
#define MUSIC_SERVER_URL "http://192.168.1.100:2233"
```

把 `192.168.1.100` 换成**运行 MeowMusicServer 那台电脑的局域网 IP**（Windows 下 `ipconfig` 查看 IPv4 地址）。ESP32 和电脑必须在同一个局域网内。

### 3. 设置板型

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

依次进入：

- `Xiaozhi Assistant` → `Board Type` → 选 **Bread Compact WiFi**
- `Xiaozhi Assistant` → `OLED Type` → 选 **SSD1306 128x32**

### 4. 编译与烧录

**Windows（PowerShell）：**

```powershell
idf.py build
idf.py -p COM4 flash monitor
```

**Linux：**

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

把端口换成你自己实际的（Windows 形如 `COM4`，Linux 形如 `/dev/ttyUSB0`）。

烧录完成后设备会自动连接 WiFi，OLED 屏幕上会显示拿到的 IP 地址。

常用操作：

```bash
# 退出串口监视器
Ctrl + ]

# 只烧录不监视
idf.py -p COM4 flash

# 清空 NVS（改过 WiFi 密码但设备还在连旧的，用这个）
idf.py -p COM4 erase-flash

# 编译异常时彻底重来
idf.py fullclean && idf.py build
```

> **编译卡住/极慢**：如果开了 ccache 反而更慢，可以临时禁掉——Windows 用 `$env:CCACHE_DISABLE=1`，Linux 用 `export CCACHE_DISABLE=1`。首次编译拉 `managed_components` 需要好网络，国内建议先挂代理。

#### 不想编译？直接烧发布包

到 [Releases](https://github.com/joe12801/xiaozhi-esp32-music/releases) 下载整包 `xiaozhi_esp32s3_music_v1.0.0_full.bin`，烧到地址 `0x0`：

```bash
# Windows
esptool.py --chip esp32s3 -p COM4 -b 460800 write_flash 0x0 xiaozhi_esp32s3_music_v1.0.0_full.bin

# Linux
esptool.py --chip esp32s3 -p /dev/ttyUSB0 -b 460800 write_flash 0x0 xiaozhi_esp32s3_music_v1.0.0_full.bin
```

或者用乐鑫 Flash Download Tool：选 ESP32-S3、地址填 `0x0`、Flash 大小 16MB，勾选该 bin 下载。

> 发布包里 WiFi 是空的（走配网），服务器地址是 `192.168.1.100` 示例值。你的服务器 IP 不同的话，只能自己改 `main/server_config.h` 重新编译。

### 5. 启动音乐服务器

固件需要配合 [MeowMusicServer 多音源增强版](https://github.com/joe12801/MeowMusicServer-multisource) 使用，在电脑上把它跑起来（监听 `2233` 端口）之后，就可以对设备说「你好小智，播放稻香」了。

服务器支持 **Windows / Linux / Docker** 三种部署方式，具体步骤（含 ffmpeg 安装、防火墙放行、开机自启、systemd 服务）见该仓库 README 的「部署」章节。

> 最容易踩的坑：服务器防火墙没放行 `2233` 端口，表现是设备连上了 WiFi 但点歌一直没反应。

---

## 语音指令示例

```
你好小智，播放晴天
你好小智，我想听周杰伦的稻香
你好小智，放一首刘若英的后来
你好小智，下一首
你好小智，停止播放
你好小智，播放我的歌单
```

---

## 工作原理

```
   ┌──────────┐   ① 语音唤醒/识别     ┌──────────────┐
   │  ESP32-S3 │ ───────────────────► │  小智官方云   │
   │           │ ◄─── ② MCP 工具调用 ─ │  (LLM + ASR)  │
   └─────┬─────┘                       └──────────────┘
         │ ③ HTTP GET /stream_pcm?song=晴天&singer=周杰伦
         ▼
   ┌──────────────────┐  ④ 多音源选曲+取直链
   │ MeowMusicServer  │ ──────────────► 酷我波点 / 网易云 / GD Studio
   │   (局域网 2233)  │ ◄──── 24kHz 单声道 MP3 ────
   └──────────────────┘
```

1. 设备被唤醒后，音频上传到小智官方云做识别和意图判断；
2. 云端通过 **MCP 协议**调用固件注册的 `self.music.play_song` 工具，参数是歌名和歌手；
3. 固件向 `MUSIC_SERVER_URL` 发起 HTTP 请求，服务器实时返回 MP3 音频流；
4. 固件用 ESP-IDF 的 HTTP 流 + MP3 解码器边下边播，无需先下载整首。

固件侧注册的 7 个音乐相关工具（`main/mcp_server.cc`）：

| 工具名 | 作用 |
|---|---|
| `self.music.play_song` | 按歌名 / 歌手点播 |
| `self.music.next_song` | 下一首 |
| `self.music.previous_song` | 上一首 |
| `self.music.stop_playlist` | 停止连播 |
| `self.music.favorite_list` | 读取收藏列表 |
| `self.music.my_playlists` | 读取我的歌单 |
| `self.music.play_playlist` | 按歌单连播 |

以及 3 个设备管理工具：`self.device.bind`、`self.device.unbind`、`self.device.status`。

---

## 目录结构

```
main/
├── main.cc                  # 入口，含 WiFi 兜底凭据
├── server_config.h          # 音乐服务器地址配置 ★ 需修改
├── mcp_server.cc            # MCP 工具注册（音乐/闹钟/设备/屏幕）
├── boards/
│   └── bread-compact-wifi/  # 板级定义（I2S 引脚、OLED、按键）
├── audio/                   # 音频编解码、唤醒词、MP3 播放
├── display/                 # LVGL 界面
├── protocols/               # WebSocket / MQTT 通信协议
└── assets/                  # 字体、表情、提示音
partitions/v2/16m.csv        # 16MB Flash 分区表
```

---

## 更新记录

### v1.6.6 — 大歌单分批拉取（可播完整歌单）
- **修复**：歌单超过约 150 首时，语音「播放 XX 歌单」歌单能拉到但**一首都不播、完全没声音**。
  原因：设备把整包歌曲 JSON 一次性解析进内存，条数过多时解析失败（实测 198 首必现，60/100/150 首正常）。
  现改为：按每页 50 首分页拉取（`offset`/`limit`），每页解析完立即释放再拉下一页，内存峰值只有一页大小，
  198 首及更大的歌单均可完整播放。配合服务端 `/api/user/playlist/songs` 与 `/api/favorite/list` 的分页支持。

### v1.6.5 — 语音打断修复
- **修复**：播放歌单时喊「你好小智」打断，音乐停一下又自动播下一首、盖住对话。
  原因：唤醒走的是应用层 `StopStreaming()`，只停了当前歌曲，歌单管理线程误判「这首播完了」就切下一首。
  现改为：歌单模式下唤醒直接停止整个歌单，麦克风留给用户说话。

### v1.6.3 — 首曲沙沙声修复
- **修复**：歌单第一首歌开头出现沙沙声、听不清（等一会儿又恢复）。
  原因：切歌时旧播放线程停止超时被 `detach`，但它仍存活，与新播放线程并发解码同一个 MP3 解码器（非线程安全）并同时写 I2S。
  现改为：播放代次令牌（旧线程检测到过期立即退出）+ MP3 解码器互斥锁。

### v1.6.1 — 点播自建歌单卡死修复
- **修复**：语音点播「周杰伦的歌单」等自建歌单时卡死无响应。
  原因：旧接口返回整份歌单 JSON（数万字节），设备解析与 POST chunked 处理都有问题。
  现改为：新增精简接口 `GET /api/user/playlist/songs`，按歌单名只返回「歌名 + 歌手」，实测 ~900 字节。

### v1.5.0 — 按歌单名直接播放
- 新增 MCP 工具 `play_playlist_by_name`，支持「播放王杰的歌单」这类指令，由设备自己按名称取歌单，避免大模型拼长 JSON。

### v1.4.0 — 音频卡死修复
- **修复**：音乐播放 / 工具调用后小智卡死无响应（AFE ringbuffer 满、音频线程饿死）。

### v1.2.0 — 播放中唤醒打断
- **修复**：音乐播放中唤醒无法打断（barge-in 失效）。

---

## 常见问题

**Q：编译报 `esp_lcd` 组件缺失 / 版本冲突？**
执行 `idf.py fullclean` 后重新 `idf.py build`，确保 `managed_components` 重新拉取。

**Q：设备连不上 WiFi？**
先确认 `main.cc` 里的 SSID / 密码正确；若之前配过网，旧的错误记录会存在 NVS 里导致兜底逻辑不生效，可执行 `idf.py -p COM4 erase-flash` 清空后重烧。

**Q：点歌没反应，串口打印 HTTP 失败？**
检查电脑防火墙是否放行 `2233` 端口，以及在手机/电脑浏览器里能否打开 `http://<电脑IP>:2233`。

**Q：能听到歌但音质差 / 有断音？**
这是 24kHz 单声道流式解码，属正常表现；若频繁卡顿，检查 WiFi 信号强度（串口日志里的 RSSI 建议优于 -60dBm）。

---

## 免责声明

本项目仅供个人学习与技术研究使用。音乐内容版权归各唱片公司及平台所有，请勿用于商业用途。固件中的语音识别部分依赖小智官方服务。

## 许可证

MIT License，详见 [LICENSE](LICENSE)。上游版权归 深圳新智未来科技有限公司 及 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 各位贡献者所有。
