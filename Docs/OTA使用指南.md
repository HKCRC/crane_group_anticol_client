# OTA 使用指南（当前工程）

本文档基于当前工程与现网 LTE 模块行为编写，适用于 `crane_group_anticol_client` 的 MQTT + Ymodem OTA。

## 1. 通道与话题约定（已确认）

- LTE 模块收到**话题1**消息后，会通过串口转发为：`1,payload`。
- MCU 若通过串口发送 `1,payload` 给 LTE，LTE 会把 `payload` 发布到**话题1**（系统指令话题）。
- **话题2**用于 OTA 数据通道。
- 当前工具链与该映射一致，可直接使用，无需改代码。

## 2. 固件选择

OTA 使用应用镜像 `.bin` 文件，优先选择当前编译产物：

- `Debug/crane_group_anticol_client.bin`

不要使用 `.elf` 进行 OTA。

## 3. 设备侧 OTA 机制简述

1. 下发系统指令 `craner#AT+OTASTART` 后，设备进入 OTA 等待态。
2. 设备会周期发送 `C`（Ymodem CRC 握手）。
3. OTA 数据走话题2，设备收到后进入 Ymodem 接收、校验、写临时区。
4. 接收完成后写 OTA metadata，延时约 3 秒自动复位。

## 4. 升级前准备

- 固件：准备好 `Debug/crane_group_anticol_client.bin`。
- MQTT：确认 Broker 地址、端口、账号密码可用。
- 设备在线：4G 模块在线且串口链路正常。
- 风险控制：升级过程中保持供电稳定，避免断电。

## 5. 标准升级步骤

### 步骤 1：启动 OTA 工具

在工具目录运行：

- `tool/tool-ota-mqtt-ymodem/src/ota_cli.py`

按提示输入 MQTT 连接参数和目标主题。

### 步骤 2：选择固件

在文件选择框中选择：

- `Debug/crane_group_anticol_client.bin`

### 步骤 3：发送启动命令（话题1）

先通过系统指令通道下发：

- `craner#AT+OTASTART`

设备回复 `craner#OK` 后，表示已进入 OTA 等待。

### 步骤 4：开始 Ymodem 发送（话题2）

在 OTA 工具中执行发送。

- 工具等待设备 `C` 握手。
- 随后发送 block0、数据包、EOT、尾包。
- 设备按 Ymodem ACK/NAK 机制反馈。

### 步骤 5：等待写入完成与复位

传输成功后，设备会：

1. 完成固件校验与 metadata 写入。
2. 延时约 3 秒自动复位。

## 6. 成功判定

满足以下任意组合可判定升级成功：

- OTA 工具显示传输完成（无持续 NAK/超时）。
- 设备自动复位并重新上线。
- 重启后业务线程运行正常（如心跳、上报、寄存器服务正常）。

## 7. 失败处理

### 场景 A：`OTASTART` 失败

- 现象：返回 `craner#ERROR`。
- 处理：先执行 `craner#AT+OTARESET`，再重试 `OTASTART`。

### 场景 B：Ymodem 超时/反复 NAK

- 检查网络抖动、MQTT 主题配置、设备在线状态。
- 重新触发 `OTARESET -> OTASTART` 后重传。

### 场景 C：中途需人工中止

- 发送 `craner#AT+OTARESET` 复位 OTA 状态机。
- 重新从步骤 3 开始。

## 8. 操作注意事项

- 升级期间避免频繁发送其他业务命令。
- 单次升级只使用一个固件文件，不要中途切换。
- 升级失败后先做 OTA 状态重置再重试，避免残留状态影响。

## 9. 建议的现场记录项

每次升级建议记录：

- 时间、设备编号、固件文件名。
- MQTT 目标主题（话题1/话题2）。
- 是否收到 `craner#OK`。
- 是否自动复位成功。
- 升级后版本与功能抽检结果。
