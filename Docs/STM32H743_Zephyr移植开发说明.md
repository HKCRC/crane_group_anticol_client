# STM32H743 + Zephyr 移植开发说明

本文档给 `crane_group_anticol_client` 迁移到 STM32H743 + Zephyr 使用。硬件连接、引脚、PHY、RS485 收发控制脚等描述由硬件负责人补充；本文只说明移植同事需要实现的业务功能、软件流程、模块边界和验收方法。

## 1. 移植目标

Zephyr 版本需要保持当前 HAL + FreeRTOS 版本的外部行为一致：

- 通过以太网接收中联 UDP 吊重/力矩数据，解析后写入内部寄存器表。
- 通过两路 Modbus RTU 主站读取回转、变幅编码器数据，写入内部寄存器表。
- 通过一路 Modbus RTU 从站对外提供内部 holding register 数据。
- 通过 4G 串口每 1 秒上报邻塔信息帧。
- 通过 shell/4G 串口处理 AT 命令、OTA Ymodem 数据和透传数据。
- 维护离线监控、数据有效位、状态 LED、看门狗和异常复位流程。
- 保留 OTA 临时固件写入、元数据写入、完成后复位的业务语义。

不要按 CubeMX/HAL 文件逐行搬运。Zephyr 版本应以 Zephyr 的 `k_thread`、`k_mutex`、`k_msgq`、`device`、UART、network、flash、watchdog、GPIO、Devicetree 和 Kconfig 机制重新组织。

## 2. 当前工程关键入口

当前版本的业务入口在 `application/board_manage.c`：

1. `board_setup_components()`
   - 将 `printf` 重定向到 `USART1`。
   - 初始化 UART 服务，注册 `shell` 和 `4g` 两个逻辑串口。
   - 初始化 Modbus RTU 从站寄存器服务。
   - 初始化 OTA 服务，临时固件地址为 `0x08020000`，最大大小为 `896 KiB`。

2. `board_create_user_tasks()`
   - 创建离线监控任务。
   - 创建以太网管理任务。
   - 创建回转编码器读取任务。
   - 创建变幅编码器读取任务。
   - 创建 OTA 消费任务。
   - 创建邻塔信息上报任务。

Zephyr 版本建议保留一个很薄的 `src/main.c`：

```c
int main(void)
{
    board_app_init();
    board_app_start_threads();
    return 0;
}
```

具体业务放到独立模块中，例如：

```text
src/app_board.c
src/offline_monitor.c
src/register_db.c
src/uart_router.c
src/at_command.c
src/encoder_modbus_master.c
src/encoder_modbus_slave.c
src/load_udp_listener.c
src/neighbor_report.c
src/ota_service.c
src/ota_ymodem.c
```

## 3. Zephyr 工程建议结构

建议新工程至少包含：

```text
CMakeLists.txt
prj.conf
boards/<your_board>/<your_board>.dts
boards/<your_board>/<your_board>.yaml
boards/<your_board>/<your_board>_defconfig
src/main.c
src/app_board.c
src/register_db.c
src/offline_monitor.c
src/uart_router.c
src/at_command.c
src/encoder_modbus_master.c
src/encoder_modbus_slave.c
src/load_udp_listener.c
src/neighbor_report.c
src/ota_service.c
src/ota_ymodem.c
include/*.h
```

如果使用现有 Modbus C 库，建议先抽象出 Zephyr UART 适配层，业务层只依赖 `modbus_master_read_holding()`、`modbus_slave_start()` 这类接口。不要让业务代码直接操作 HAL `UART_HandleTypeDef`。

## 4. Devicetree: 硬件描述待补

本节由硬件负责人补齐。移植同事需要根据补齐内容实现 Zephyr DTS。

需要描述的硬件对象：

- MCU: STM32H743，具体封装和 board 名称待补。
- 系统时钟源、HSE/HSI、PLL 策略待补。
- console/shell 串口，当前业务等价为 `USART1`，`115200 8N1`。
- 4G 模块串口，当前业务等价为 `UART8`，`115200 8N1`。
- 回转编码器 Modbus RTU 主站串口，当前业务等价为 `USART2`，`9600 8E1`。
- 变幅编码器 Modbus RTU 主站串口，当前业务等价为 `USART3`，`9600 8E1`。
- 对外 Modbus RTU 从站串口，当前业务等价为 `UART7`，`9600 8N1`。
- 以太网 MAC、PHY 型号、RMII/MII、PHY reset GPIO、MDIO/MDC、PHY 地址待补。
- 状态 LED：心跳灯、离线灯、其他 LED 的 GPIO 和有效电平待补。
- 独立看门狗策略待补。
- 内部 Flash 分区：bootloader、app、OTA 临时区、OTA metadata 区待补。
- QSPI 是否在 Zephyr 版本继续使用待补。

建议 DTS 中提供 aliases，业务代码只通过 alias 或 chosen 获取设备：

```dts
/ {
    aliases {
        shell-uart = &usart1;
        modem-uart = &uart8;
        swing-encoder-uart = &usart2;
        luffing-encoder-uart = &usart3;
        modbus-slave-uart = &uart7;
        heartbeat-led = &heartbeat_led;
        offline-led = &offline_led;
        eth-reset-gpio = &eth_reset;
    };

    chosen {
        zephyr,console = &usart1;
        zephyr,shell-uart = &usart1;
    };
};
```

## 5. Kconfig/prj.conf 软件配置

移植时按实际模块启用，下面是方向清单：

```conf
CONFIG_GPIO=y
CONFIG_SERIAL=y
CONFIG_UART_ASYNC_API=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_LOG=y

CONFIG_MULTITHREADING=y
CONFIG_HEAP_MEM_POOL_SIZE=16384

CONFIG_NETWORKING=y
CONFIG_NET_IPV4=y
CONFIG_NET_UDP=y
CONFIG_NET_SOCKETS=y
CONFIG_NET_SOCKETS_POSIX_NAMES=y

CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_WATCHDOG=y
CONFIG_REBOOT=y
```

如果使用 Zephyr shell 做调试命令，可追加：

```conf
CONFIG_SHELL=y
CONFIG_SHELL_BACKEND_SERIAL=y
```

如果后续引入 MCUboot，OTA 分区和 metadata 应与 bootloader 约定统一，不要继续在业务代码中硬编码 Flash 地址。

## 6. 内部寄存器表

Zephyr 版本必须保留当前 holding register 地址和数据含义，因为邻塔上报、Modbus 从站和内部任务都依赖这张表。

| 地址 | 名称 | 含义 |
| ---: | --- | --- |
| `0x0000` | `REG_SYSTEM_VERSION` | 系统版本，当前初始化为 `0x0100` |
| `0x0003` | `REG_DATA_VAILD` | 数据有效位，名称拼写沿用旧工程 |
| `0x0004` | `REG_SWING_ENCODER_COUNT` | 回转累计圈数 |
| `0x0005` | `REG_SWING_ENCODER_VALUE` | 回转单圈值 |
| `0x0006` | `REG_LUFFING_ENCODER_COUNT` | 变幅累计圈数 |
| `0x0007` | `REG_LUFFING_ENCODER_VALUE` | 变幅单圈值 |
| `0x0008` | `REG_HOOK_ENCODER_COUNT` | 吊钩累计圈数，当前未采集 |
| `0x0009` | `REG_HOOK_ENCODER_VALUE` | 吊钩单圈值，当前未采集 |
| `0x000A` | `REG_LIFTING_MOMENT` | 起重力矩，float32 高字在低地址 |
| `0x000C` | `REG_LIFTING_MOMENT_PCT` | 起重力矩百分比，float32 高字在低地址 |
| `0x000E` | `REG_LOAD_WEIGHT` | 起重量，float32 高字在低地址 |
| `0x0010` | `REG_LOAD_WEIGHT_PCT` | 起重量百分比，float32 高字在低地址 |
| `0x0020` | `REG_HOLDING_SIZE` | 寄存器表大小 |

`REG_DATA_VAILD` 位定义：

| bit | 含义 |
| ---: | --- |
| 0 | 回转编码器有效 |
| 1 | 变幅编码器有效 |
| 2 | 吊钩编码器有效，当前预留 |
| 3 | UDP 吊重/力矩数据有效 |

实现要求：

- 用 `k_mutex` 保护寄存器表读写。
- 提供单寄存器读写、连续读写、离散地址一次性读取、写 bit、写 float 的接口。
- float 写入必须保持旧工程行为：先按 IEEE754 得到 `uint32_t raw`，再写 `raw >> 16` 到低地址，`raw & 0xFFFF` 到高地址。
- 邻塔上报时再按协议要求输出小端字节序。

## 7. 串口路由和 AT 命令

需要实现两个逻辑串口：

| 逻辑名 | 旧工程硬件 | 参数 | 用途 |
| --- | --- | --- | --- |
| `shell` | `USART1` | `115200 8N1` | 日志、调试输入、AT 命令入口 |
| `4g` | `UART8` | `115200 8N1` | 4G 模块、MQTT/OTA/邻塔上报数据通道 |

Zephyr 版本建议使用 UART async API 或 interrupt-driven UART，做成带 RX ring buffer 和 TX queue 的串口服务。需要支持按逻辑名发送，例如：

```c
uart_router_send("shell", data, len);
uart_router_send("4g", data, len);
```

### shell 串口接收流程

1. 收到数据后先尝试解析 `craner#AT` 命令。
2. 如果不是 `craner#AT`，尝试解析 `usr.cn#AT` 命令。
3. 如果都不是，原样回显到 shell，前缀为 `[SHELL] `。

### 4G 串口接收流程

1. 如果数据以 `1,` 开头，去掉前缀后按 AT 命令处理。
2. 如果数据以 `2,` 开头，去掉前缀后投递给 OTA RX 队列。
3. 其他数据同时转发给 shell 和 MQTT 上报通道：
   - shell 方向直接发送原始数据。
   - 4G/MQTT 方向发送时前缀 `1,`。

### AT 命令

必须支持：

| 命令 | 行为 | 回复 |
| --- | --- | --- |
| `craner#AT` | 通用在线测试 | `craner#OK\r\n` |
| `craner#AT+OTASTART` | 启动 Ymodem OTA 接收流程，关闭离线事件监控 | 成功 `craner#OK\r\n`，失败 `craner#ERROR\r\n` |
| `craner#AT+OTARESET` | 中止并重置 OTA 状态 | `craner#OK\r\n` |
| `craner#AT+SYSRESET` | 回复 OK 后延时约 100 ms 复位 | `craner#OK\r\n` |
| `usr.cn#AT...` | 原样转发到 4G 模块，必要时补 `\r\n` | 无本地解析 |

## 8. 以太网和 UDP 吊重数据

当前业务使用静态 IP：

```text
IP      192.168.1.33
Netmask 255.255.255.0
Gateway 192.168.1.1
UDP port 1212
```

Zephyr 版本需要：

1. 复位 PHY，复位时序沿用旧工程：reset 拉低约 100 ms，再拉高约 100 ms。
2. 配置静态 IPv4 地址。
3. 等待 net interface up 和 carrier/link up。
4. 创建 UDP socket，绑定本地端口 `1212`。
5. 接收 UDP payload 后解析中联协议。
6. 解析成功后写入寄存器表，并置 `REG_DATA_VAILD` bit3。
7. 任务正常运行时周期性更新 `OFFLINE_ETH_MANAGE` 事件。

中联 UDP 解析规则：

- 协议标识偏移 `0`，小端 `0x33532D00`。
- 主索引偏移 `8`，小端 `0x0101`。
- 帧长度偏移 `14`，必须等于 UDP payload 长度。
- 数据结构版本偏移 `20`，前两个字节必须为 `0x73 0x17`。
- 起重力矩：数据结构版本偏移之后 `+32`，float32 小端。
- 起重力矩百分比：`+36`，float32 小端。
- 起重量：`+40`，float32 小端。
- 起重量百分比：`+44`，float32 小端。

解析成功后写入：

```text
REG_LIFTING_MOMENT
REG_LIFTING_MOMENT_PCT
REG_LOAD_WEIGHT
REG_LOAD_WEIGHT_PCT
REG_DATA_VAILD bit3 = 1
```

解析失败时不要更新寄存器，不要置有效位。

## 9. 编码器 Modbus RTU 主站

需要实现两路周期读取：

| 业务 | 旧工程串口 | 串口参数 | 从站 ID | 功能码 | 起始地址 | 数量 | 周期 | 写入寄存器 |
| --- | --- | --- | ---: | --- | ---: | ---: | ---: | --- |
| 回转编码器 | `USART2` | `9600 8E1` | `1` | Read Holding Registers | `0x0002` | `2` | `500 ms` | `0x0004`/`0x0005` |
| 变幅编码器 | `USART3` | `9600 8E1` | `1` | Read Holding Registers | `0x0002` | `2` | `500 ms` | `0x0006`/`0x0007` |

流程要求：

1. 初始化 Modbus RTU master。
2. 每 500 ms 查询一次。
3. OTA 非空闲时暂停查询，避免串口/Flash/4G流程互相干扰。
4. 查询成功时写入两个寄存器，并置对应数据有效位。
5. 查询失败时清对应数据有效位。
6. 每次循环更新对应离线事件：
   - `OFFLINE_READ_SWING_ENCODER`
   - `OFFLINE_READ_LUFFING_ENCODER`

调试阶段可以保留 fake data 模式，但必须通过 Kconfig 控制，例如：

```conf
CONFIG_CRANE_ENCODER_FAKE_DATA=y
```

## 10. 对外 Modbus RTU 从站

需要在 `UART7 9600 8N1` 上实现 Modbus RTU slave：

- slave id: `1`
- holding register buffer: 内部寄存器表 `holding_reg_data`
- timeout: `200 ms`
- 表大小: `0x20` 个 `uint16_t`

初始化时：

1. 清空内部寄存器表。
2. 写 `REG_SYSTEM_VERSION = 0x0100`。
3. 写 `REG_DATA_VAILD = 0`。
4. 启动 Modbus RTU slave。

外部主站需要能读取上述 holding registers，用于调试或其他控制器转发。

## 11. 邻塔信息上报

需要每 1 秒通过 `4g` 串口发送一次邻塔信息帧。发送前要加前缀 `3,`，即串口实际发送：

```text
3,<30字节邻塔二进制帧>
```

邻塔二进制帧固定 30 字节：

| 字段 | 长度 | 值/说明 |
| --- | ---: | --- |
| frame version | 1 | `0xA5` |
| content id | 1 | `0x21` |
| sequence | 1 | `uint8_t` 循环递增 |
| data length | 1 | `26` |
| data | 26 | 见下表 |
| crc16 | 2 | CRC16-CCITT，低字节在前 |

数据区 26 字节，小端序：

| 偏移 | 字段 | 来源 |
| ---: | --- | --- |
| 0 | 有效码 `uint16` | `REG_DATA_VAILD` |
| 2 | 回转圈数 `uint16` | `REG_SWING_ENCODER_COUNT` |
| 4 | 回转单圈值 `uint16` | `REG_SWING_ENCODER_VALUE` |
| 6 | 变幅圈数 `uint16` | `REG_LUFFING_ENCODER_COUNT` |
| 8 | 变幅单圈值 `uint16` | `REG_LUFFING_ENCODER_VALUE` |
| 10 | 起重力矩 `float32` | `REG_LIFTING_MOMENT`/`+1` |
| 14 | 起重力矩百分比 `float32` | `REG_LIFTING_MOMENT_PCT`/`+1` |
| 18 | 起重量 `float32` | `REG_LOAD_WEIGHT`/`+1` |
| 22 | 起重量百分比 `float32` | `REG_LOAD_WEIGHT_PCT`/`+1` |

CRC 使用 CRC16-CCITT，poly `0x1021`，初始值 `0x0000`，计算范围为前 28 字节。

发送任务每次成功走完周期后更新 `OFFLINE_PUBLISH_MQTT` 事件。OTA 非空闲时暂停上报。

## 12. OTA 服务

旧工程 OTA 通过 4G 串口复用协议完成：

- `craner#AT+OTASTART` 进入等待 Ymodem。
- 4G 串口收到 `2,` 前缀的数据后，把前缀后的字节投递到 OTA 队列。
- OTA 等待开始时每 5 秒发送一次字符 `C`。
- Ymodem 收到文件头后擦除临时固件区。
- 接收数据时按 Flash write unit 写入临时区，同时计算 CRC32。
- 接收完成后写 OTA metadata。
- 写入成功后延时约 3 秒复位。

Zephyr 版本实现要求：

1. OTA 状态机至少包含 `IDLE`、`WAIT_START`、`RECEIVING`、`VERIFYING`、`WRITING_META`、`COMPLETED`、`FAILED`。
2. OTA 期间其他周期任务检测到 OTA 非空闲时应短延时并暂停业务动作。
3. 使用 `k_msgq` 或 `k_fifo` 承接 `2,` 前缀后的 Ymodem 数据。
4. 使用 Zephyr `flash_area` 或 `flash_write()` 写入临时固件区。
5. Flash 地址、分区大小、metadata 地址必须迁到 partition manager 或 fixed partitions，不建议硬编码。
6. 保留 Ymodem 控制字符行为：`C`、`ACK`、`NAK`、`CAN`、`EOT`。
7. OTA 开始时关闭全部离线事件，避免升级期间误复位。

Flash 分区由硬件和 bootloader 方案确认后补齐：

```text
bootloader:        待补
primary app:       待补
ota temp image:    旧工程为 0x08020000 起，896 KiB，Zephyr 需重新确认
ota metadata:      待补
```

## 13. 离线监控、LED 和复位

需要保留离线事件表和行为：

| 事件 | 默认使能 | 等级 | LED 组 | 超时 |
| --- | --- | --- | ---: | ---: |
| `SYSTEM_PROTECT` | 使能 | error | 0 | 0 |
| `OFFLINE_ETH_MANAGE` | 使能 | error | 3 | `3000 ms` |
| `OFFLINE_READ_SWING_ENCODER` | 使能 | error | 2 | `3000 ms` |
| `OFFLINE_READ_LUFFING_ENCODER` | 使能 | error | 2 | `3000 ms` |
| `OFFLINE_READ_LOAD_WEIGHT` | 禁用 | warning | 1 | `3000 ms` |
| `OFFLINE_PUBLISH_MQTT` | 使能 | error | 3 | `3000 ms` |

离线监控线程：

- 周期 `100 ms`。
- 每次扫描所有事件，判断 `now - last_time > offline_time`。
- 任一 error 级事件离线时触发 `SYSTEM_PROTECT`。
- `SYSTEM_PROTECT` 离线持续超过 10 分钟后强制复位。
- 系统运行满 12 小时后延时约 3 秒复位。
- 无离线事件时心跳 LED 每 1 秒翻转。
- 有离线事件时离线 LED 按最高组号闪烁，心跳 LED 置为固定状态。

`OFFLINE_READ_LOAD_WEIGHT` 离线首次回调需要清空吊重/力矩寄存器并清 bit3；当前事件默认禁用，但代码结构要保留。

## 14. 看门狗和异常处理

Zephyr 版本需要实现：

- 独立看门狗初始化和周期喂狗。
- OTA consumer 或系统主监控线程中喂狗，确保长时间 Ymodem 接收期间不会误复位。
- stack overflow、fatal error、assert 等异常建议统一记录日志后调用 `sys_reboot(SYS_REBOOT_COLD)`。
- 对不可恢复错误，不要进入无喂狗死循环。

## 15. 启动顺序建议

建议 Zephyr 启动顺序：

1. `main()` 调用 `app_board_init()`。
2. 初始化日志、寄存器表、GPIO LED、watchdog。
3. 初始化 UART router，启动 shell 和 4G UART 接收。
4. 初始化 OTA 服务状态机和 RX queue。
5. 初始化 Modbus RTU slave 寄存器服务。
6. 创建线程：
   - `offline_monitor`
   - `load_udp_listener`
   - `swing_encoder_reader`
   - `luffing_encoder_reader`
   - `ota_consumer`
   - `neighbor_report`
7. 网络线程等待链路 up 后绑定 UDP 1212。
8. 各业务线程开始更新离线事件。

## 16. 建议线程优先级和栈

Zephyr 需要根据实际栈使用调整，初始建议：

| 线程 | 周期/触发 | 优先级建议 | 栈建议 |
| --- | --- | ---: | ---: |
| `offline_monitor` | `100 ms` | 5 | 2048 |
| `load_udp_listener` | socket blocking/poll | 5 | 4096 |
| `swing_encoder_reader` | `500 ms` | 5 | 3072 |
| `luffing_encoder_reader` | `500 ms` | 5 | 3072 |
| `ota_consumer` | queue + timeout | 3 | 4096 |
| `neighbor_report` | `1000 ms` | 4 | 3072 |
| UART RX worker | event driven | 3-4 | 2048 |

数字越小优先级越高时，OTA 和 UART RX 可以略高于周期采集任务。

## 17. 验收测试清单

### 启动

- 串口 console 输出启动日志。
- `REG_SYSTEM_VERSION` 初始化为 `0x0100`。
- `REG_DATA_VAILD` 初始为 `0`。
- 心跳 LED 每秒翻转。
- 看门狗不误复位。

### UDP 吊重

- PC 向 `192.168.1.33:1212` 发送符合 `Docs/TC42-中联UDP通信协议.md` 的 UDP payload。
- 寄存器 `0x000A`、`0x000C`、`0x000E`、`0x0010` 更新。
- `REG_DATA_VAILD` bit3 置位。
- 错误 payload 不应更新寄存器。

### 编码器读取

- 接入 Modbus RTU 编码器或模拟从站。
- 回转任务每 500 ms 读 slave `1` holding register `0x0002` 长度 `2`。
- 变幅任务每 500 ms 读 slave `1` holding register `0x0002` 长度 `2`。
- 成功时 bit0/bit1 置位，失败时清位。

### Modbus 从站

- 外部主站通过 `UART7 9600 8N1`、slave id `1` 读取 holding register。
- 能读到版本、有效位、编码器、吊重/力矩寄存器。

### 邻塔上报

- 4G 串口每秒输出 `3,` + 30 字节二进制帧。
- 帧头为 `A5 21`，data length 为 `1A`。
- 序号递增。
- CRC16-CCITT 校验正确。
- 数据区与内部寄存器一致。

### AT 命令

- shell 输入 `craner#AT`，返回 `craner#OK`。
- shell 输入 `craner#AT+SYSRESET`，返回 OK 后复位。
- shell 输入 `usr.cn#AT...`，转发给 4G 模块。
- 4G 输入 `1,craner#AT`，返回带 `1,` 通道语义的 OK。

### OTA

- 发送 `craner#AT+OTASTART` 后进入 `WAIT_START`。
- 设备周期发送 `C`。
- 通过 `2,` 通道输入 Ymodem 帧。
- 成功后写入临时固件区和 metadata，延时复位。
- OTA 期间编码器读取、UDP 处理、邻塔上报按约定暂停或不抢占关键路径。

### 离线保护

- 断开以太网或停止任务更新后，3 秒内触发对应离线事件。
- LED 闪烁组号符合事件表。
- error 离线持续 10 分钟后系统复位。
- 系统运行 12 小时后自动复位。

## 18. 移植优先级

建议按这个顺序做，便于逐步验收：

1. Zephyr board、DTS、console、LED、watchdog、基本线程。
2. 寄存器表和离线监控。
3. UART router、AT 命令、4G/shell 通路。
4. Modbus RTU 从站，对外读寄存器。
5. 两路 Modbus RTU 主站读取编码器。
6. 以太网静态 IP 和 UDP 1212 解析。
7. 邻塔上报帧。
8. OTA Ymodem、Flash 分区、metadata、复位。
9. 长稳测试和异常注入测试。

## 19. 需要硬件负责人补充的信息

移植开始前请补齐：

- Zephyr board 名称和 STM32H743 具体型号。
- 所有 UART TX/RX 引脚、复用功能、DMA 是否必须使用。
- RS485 是否有 DE/RE 控制脚；如果有，分别对应哪路串口、有效电平和收发切换时序。
- 以太网 PHY 型号、PHY 地址、reset GPIO、RMII/MII 引脚和时钟来源。
- LED GPIO、有效电平、丝印含义。
- 看门狗超时时间要求。
- Flash 分区表、bootloader 方案、OTA metadata 格式最终定义。
- QSPI 是否继续使用及用途。
- 4G 模块上电/复位/状态脚是否需要软件控制。

