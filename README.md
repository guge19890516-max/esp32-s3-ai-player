# ESP32-S3 触摸屏 AI 聊天固件

微雪 **ESP32-S3-Touch-LCD-3.5B**（ESP32-S3R8，3.5 寸 320×480 触摸屏）独立运行的 AI 助手固件：上电即用，无需手机或电脑配合。中文界面，自带软键盘与拼音输入法。

当前版本：**1.3.0**

## 仓库结构

- 本仓库（esp32-s3-ai-player）：仅固件与契约文档 `docs/`（App-ESP32 控制行协议、解析规则 Schema）
- 手机 APP（FrameLens，Kotlin/Compose）已独立为单独仓库：https://github.com/guge19890516-max/framelens-app ，本地路径 `f:\ESP32\framelens-app`，不再存放于本仓库；两端以 `docs/app-protocol.md` 为唯一对齐契约

## 功能

**AI 聊天**

- 屏幕软键盘输入（中文拼音 / 英文 / 符号页、退格长按连删）
- 调用任意 OpenAI 兼容接口（默认 DeepSeek，也支持本地模型服务）
- 气泡式对话界面，可拖动滚动并带滚动条；键盘可收起，收起后聊天区自动铺满全屏
- 聊天记录有内存上限（80 条 / 12 万字符），超限自动丢弃最早的，长时间聊天不会吃光堆
- 发给模型的历史按字数（6000 字）从最近往回裁剪，避免请求体过大
- WiFi 扫描 / 选择 / 密码输入，断线自动重连；密码使用独立输入缓冲，不会清掉正在写的聊天内容
- API 地址、模型、Key 均可在屏幕上配置，存 NVS 断电不丢

**蓝牙 / 串口助手**

- **BLE 串口**：设备作为 BLE 外设提供 Nordic UART 风格 GATT 服务，手机用 nRF Connect、蓝牙调试助手、Serial BLE 等 App 连接，收发文本与 HEX 报文
- **WiFi TCP 串口**：设备内置 TCP 服务器（端口 8888），电脑或手机用任意 TCP 调试工具连接
- 串口页面支持 HEX / 文本双视图显示、HEX 模式发送原始字节、自动追加 `\r\n`、清空记录
- 收发报文实时滚动显示（带滚动条），最多保留最近 60 条；TCP 一次读取整包，不再把一帧拆成多条
- BLE 发送改为队列 + 分片限速，不再阻塞主循环

**AI 报文分析**

- 报文记录页列出所有收发明细（方向、时间、长度、内容）
- 点任意一条即进入独立分析页，AI 以「嵌入式协议分析专家」角色逐字节解释协议、指令含义、异常风险，并给出建议的回复指令

**OTA 空中升级（四种方式）**

| 方式 | 入口 | 说明 |
|---|---|---|
| ArduinoOTA 网络推送 | 设置 → 固件升级 → 网络推送 | 同网内电脑 / 手机一键推送，屏幕显示实时进度条 |
| URL 下载升级 | 设置 → 固件升级 → URL 地址升级 | 设备自行从 HTTP 地址下载 `firmware.bin`（电脑开 HTTP 服务器即可），地址存 NVS |
| **TF 卡升级** | 设置 → 固件升级 → TF 卡升级（或直接重启） | 卡根目录放 `firmware.bin`，**重启自动检测并升级**，成功后自动改名 `.done` 防重复 |
| **蓝牙推送升级** | 设置 → 固件升级 → 蓝牙推送升级 | 电脑浏览器开 `tools/ota_web/index.html`（Web Bluetooth）直接把 `firmware.bin` 推给设备 |

- 升级过程自动暂停 BLE 广播省内存，失败保留旧固件槽位可重推
- 蓝牙 / TF 通道升级前都会校验固件大小，超过备用分区容量直接拒绝；蓝牙通道额外做 CRC32 校验
- 设置页可查看固件版本、当前运行槽位、目标分区大小、空闲内存

## 硬件

- 主控：ESP32-S3R8（16MB Flash / 8MB PSRAM）
- 屏幕：AXS15231B QSPI 320×480（触摸一体）
- TF 卡：SD_MMC 1-bit，CLK=GPIO11 / CMD=GPIO10 / D0=GPIO9
- 开发板：微雪 ESP32-S3-Touch-LCD-3.5B
- 依赖库：[GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX) ≥ 1.6.0、[U8g2](https://github.com/olikraus/u8g2) ≥ 2.35.0、[ArduinoJson](https://github.com/bblanchon/ArduinoJson) ≥ 7.0、[NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) 1.4.x

## 目录结构

```
platformio.ini              构建配置（src_dir = esp32_ai_chat）
partitions/                 自定义分区表
esp32_ai_chat/              全部固件源码
  esp32_ai_chat.ino         界面 / 状态机 / 触摸分发
  ui.*                      画布、字体、绘图文具、滚动条
  keyboard.*  pinyin_dict.h 软键盘与拼音词表
  chat_store.*              气泡消息与折行
  ai_client.*               后台任务里的 HTTP 对话请求
  serial_link.*             BLE 串口 + TCP 串口
  frame_log.*               报文记录
  config_store.*            NVS 配置
  ota_update.*              OTA 状态机、ArduinoOTA、URL 下载
  ota_sd.*                  TF 卡开机检测升级
  ota_ble.*                 蓝牙 OTA GATT 服务
tools/
  ota_web/index.html        蓝牙升级网页（Web Bluetooth）
  capture_log.py            串口日志抓取
```

## 分区方案（重要）

固件使用双 OTA 分区，**Arduino IDE 与 PlatformIO 必须使用同一张分区表**，否则 OTA 会写错地址：

| 平台 | 设置 |
|---|---|
| Arduino IDE | 导入 `partitions/app3M_fat1M_8MB.csv` 后选它 |
| PlatformIO | `board_build.partitions = partitions/app3M_fat1M_8MB.csv` |

对应布局：app0 3MB @0x10000、app1 3MB @0x310000、ffat 1.75MB @0x610000、coredump 128KB @0x7D0000，**结束于 0x7F0000**。

分区表尾地址刻意压在 **8MB 以内**：ESP-IDF 启动时按 bootloader 镜像头的 Flash Size 字段校验分区表，烧录工具若把镜像头写成 8MB（探测值或手动选项），超过 8MB 的分区表会被 `Failed to verify partition table` 拒绝并进入复位循环。本表在 8MB / 16MB 两种镜像头下都能通过校验。

Arduino IDE 导入自定义分区表：把 `partitions/app3M_fat1M_8MB.csv` 复制到 sketchbook 的 `hardware/espressif/esp32/tools/partitions/` 目录，重启 IDE 后在 Partition Scheme 下拉里选择。

## 编译

### 方式一：Arduino IDE

1. 开发板管理器安装 **esp32** 板支持包
2. 库管理器安装 GFX Library for Arduino、U8g2、ArduinoJson、NimBLE-Arduino
3. 打开 `esp32_ai_chat/esp32_ai_chat.ino`
4. 开发板选 **ESP32S3 Dev Module**，关键设置：

| 选项 | 值 |
|---|---|
| PSRAM | OPI PSRAM |
| Flash Size | 16MB |
| Flash Mode | QIO 80MHz |
| Partition Scheme | 导入的 `app3M_fat1M_8MB` |
| USB CDC On Boot | Enabled |

5. 编译上传。卡在 `Connecting...` 时按住板上 BOOT 轻点 EN，或把 Upload Speed 降到 115200

### 方式二：PlatformIO / GitHub Actions

仓库根目录已带 `platformio.ini`（`src_dir = esp32_ai_chat`），本地 `pio run` 或推送后由 GitHub Actions 自动编译，在 Actions 页面下载 `firmware-bin` 构件，内含 `firmware.bin`（纯应用分区，用于 OTA）和 `merged_flash.bin`（bootloader + 分区表 + 应用，单文件刷入用）。

板型固定为 `esp32-s3-devkitc1-n16r8`（16MB Flash + 8MB Octal PSRAM），**不要改成不带 `-n16r8` 的通用板**，否则不会链接 OPI PSRAM 库，320×480 画布分配不到内存会反复重启。

或用浏览器版工具：<https://espressif.github.io/esptool-js/> （Chrome/Edge + 数据线，选 ESP32-S3、921600、地址 0x0，Flash Size 填 16MB）。串口日志走原生 USB，打开串口助手即可看到 `[boot]` 阶段信息；若看不到，用 USB-TTL 接 GPIO43(TX)/GPIO44(RX)、115200。

## OTA 使用

**方式 A：ArduinoOTA 推送（电脑执行）**

```bash
pip install arduino-ota
arduino-ota --port <设备IP> --hostname ESP32-AI-S3 -i esp32s3 firmware.bin
```

或用 Arduino IDE「Export Program」导出的 Python 脚本推送。设备端：设置 → 固件升级，确认「网络推送」为已开启。

**方式 B：URL 升级**

```bash
cd firmware目录
python -m http.server 8000
```

设备端：设置 → 固件升级 → URL 地址升级 → 填 `http://<电脑IP>:8000/firmware.bin` → 保存并开始。地址会存进 NVS，下次不用重填。注意只能传 `firmware.bin`，不能传 `merged_flash.bin`。

**方式 C：TF 卡升级（重启自动检测）**

1. 把 `firmware.bin` 拷到 TF 卡**根目录**（也支持 `update.bin`、`firmware/firmware.bin`，或根目录任意合法 `*.bin`）
2. 插卡，重启设备
3. 开机检测到固件后显示文件名、版本、大小，**3 秒倒计时后自动升级**（期间点屏幕可取消）
4. 升级完成自动重启；固件被改名为 `firmware.bin.done`，下次开机不会重复刷同一个文件

不重启也可以：设置 → 固件升级 → 点「TF 卡升级」这一行，会现场扫卡并立即开始。

**方式 D：蓝牙推送升级（网页端）**

1. 设备进 设置 → 固件升级 → 蓝牙推送升级，确认蓝牙名 `ESP32-S3-AI`
2. 电脑用 Chrome / Edge 打开网页（Web Bluetooth 要求安全上下文，**不能直接双击 html**）：

   ```bash
   cd tools/ota_web
   python -m http.server 8000
   ```

   浏览器访问 <http://localhost:8000/>（也可把该目录部署到 GitHub Pages 等 https 站点）
3. 点「搜索并连接」，在弹窗里选 `ESP32-S3-AI`
4. 选择 `firmware.bin`（页面会校验镜像头并计算 CRC32，选错文件会直接提示）
5. 点「开始升级」，进度条 + 速度 + 剩余时间实时显示；上传完设备自校验并重启

   - 分片默认 244 字节（设备 MTU 247），环境差可降到 128 / 64 / 20
   - 2.3MB 固件大约 2–6 分钟，期间别断开连接，断了设备会自动中止并保留旧固件

## 首次使用

1. 上电进入桌面，点击对应图标
2. ⚙ 设置 → WiFi 设置 → 选择热点 → 输入密码 → 连接
3. ⚙ 设置 → AI 接口设置 → 填 API Key（云端预设已填 DeepSeek 地址与模型）
4. 进入「聊天」输入消息；进入「串口」调试外设；进入「固件升级」做 OTA

## 说明

- HTTPS 使用 `setInsecure()` 不校验证书（嵌入式常见做法，Key 只发往你自己填写的服务器）
- 聊天记录、报文记录仅保存在内存，重启清空；WiFi 与 API 配置、OTA URL 存 NVS
- ESP32-S3 仅支持 BLE 5.0，**不支持经典蓝牙 SPP**，串口助手需使用支持 BLE 的 App
- BLE + WiFi 同时开启会占用较多内存，若串口页异常可先重启设备
- `.ino.cpp` 是 PlatformIO 生成物，不要提交；仓库 `.gitignore` 已忽略

## 致谢

- 微雪 ESP32-S3-Touch-LCD-3.5B 及 AXS15231B 驱动支持
- Moon On Our Nation 的 Arduino_GFX、olikraus 的 U8g2、bblanchon 的 ArduinoJson、h2zero 的 NimBLE-Arduino

## License

[MIT](LICENSE)
