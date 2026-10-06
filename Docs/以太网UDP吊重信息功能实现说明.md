# 以太网 UDP 吊重信息功能实现说明

本文档说明当前工程中“通过以太网 UDP 获取吊重/力矩信息并写入内部寄存器”的实现方式，便于维护和 Zephyr 迁移对照。

## 1. 功能目标

设备通过以太网监听 UDP 端口 `1212`，接收中联协议数据帧，解析出以下 4 个 float 数据：

- 起重力矩（lifting moment）
- 起重力矩百分比（lifting moment pct）
- 起重量（load weight）
- 起重量百分比（load weight pct）

当前版本已扩展为解析 5 个 float 数据，新增：

- 下方高度（hook lowering height，单位 `m`）

解析成功后写入 Modbus holding register，并置 `REG_DATA_VAILD` 的 bit3（吊重数据有效位）。

## 2. 代码入口与模块关系

- 线程创建入口：`application/board_manage.c`
  - `board_create_user_tasks()` 中创建 `eth_manage_task` 线程。
- 功能主实现：`application/eth_manage_task.c`
  - 负责 PHY 复位、静态 IP 设置、链路等待、UDP 监听、协议解析、寄存器写入。
- 寄存器接口：`application/modbus_register_interface.c`
  - `mb_reg_write_float()`：按高 16 位在前（低地址）写入 float。
  - `mb_reg_write_bit()`：置/清有效位。
- 寄存器地址定义：`application/modbus_register_database.h`
- 离线事件联动：`application/offline_event_table.c`

## 3. 以太网初始化流程（eth_manage_task）

`eth_manage_task()` 的流程如下：

1. **PHY 硬复位**
   - `ETH_RESET` 拉低 100 ms，再拉高 100 ms。
2. **配置静态 IP**
   - IP：`192.168.1.33`
   - Netmask：`255.255.255.0`
   - Gateway：`192.168.1.1`
   - 调用 `lwip_set_net_param(false, ...)` 后执行 `MX_LWIP_Init()`。
3. **等待网口可用**
   - 等待 `netif_is_link_up(&gnetif)` 且 `netif_is_up(&gnetif)`。
   - 等待期间持续更新 `OFFLINE_ETH_MANAGE` 事件时间戳。
4. **在 tcpip 线程中初始化 UDP 监听**
   - 通过 `tcpip_callback(eth_udp_listener_init, NULL)` 安排在 lwIP tcpip 上下文执行。

> 说明：`udp_new/udp_bind/udp_recv` 等 lwIP raw API 建议在 tcpip 线程上下文调用，当前实现已满足这一点。

## 4. UDP 监听与接收

### 4.1 监听参数

- 端口：`1212`
- API：lwIP raw UDP（`udp_pcb`）
- 接收回调：`eth_udp_recv()`

### 4.2 接收回调关键行为

1. 判空：`p == NULL` 直接返回。
2. 将 `pbuf` 数据拷贝到本地缓冲 `s_udp_payload[512]`。
   - 使用 `pbuf_copy_partial()`。
   - 当 `tot_len > 512` 时仅拷贝前 512 字节。
3. 调用 `parse_load_metrics()` 校验并解析。
4. 成功则写寄存器并置有效位；失败则不写寄存器。
5. 释放 `pbuf_free(p)`。

## 5. 协议校验与字段提取（parse_load_metrics）

`parse_load_metrics()` 会按固定偏移校验并解析：

### 5.1 校验项

- `payload_len` 必须满足最小长度要求。
- 协议标识（offset `0`，`u32 little-endian`）必须为 `0x33532D00`。
- 主索引（offset `8`，`u16 little-endian`）必须为 `0x0101`。
- 帧长度（offset `14`，`u16 little-endian`）必须等于 `payload_len`。
- 数据结构版本魔术字（offset `20` 起）前两字节必须为 `0x73 0x17`。

### 5.2 数据偏移（相对 offset 20）

- `+32`：起重力矩（float32 little-endian）
- `+36`：起重力矩百分比（float32 little-endian）
- `+40`：起重量（float32 little-endian）
- `+44`：起重量百分比（float32 little-endian）
- `+48`：下方高度（float32 little-endian）

解析函数通过 `read_f32_le()` 读取 4 字节并转换为 `float`。

## 6. 寄存器写入与有效位

解析成功后写入以下寄存器：

- `REG_LIFTING_MOMENT` (`0x000A`)
- `REG_LIFTING_MOMENT_PCT` (`0x000C`)
- `REG_LOAD_WEIGHT` (`0x000E`)
- `REG_LOAD_WEIGHT_PCT` (`0x0010`)
- `REG_HOOK_LOWERING_HEIGHT` (`0x0012`)

然后执行：

- `mb_reg_write_bit(REG_DATA_VAILD, REG_DATA_VAILD_BIT_LOAD_WEIGHT, true)`

即将 `REG_DATA_VAILD`（`0x0003`）的 bit3 置 1，表示吊重数据有效。

### 6.1 float 写入格式说明

`mb_reg_write_float()` 使用 IEEE754 原始位写入两寄存器：

- 低地址：高 16 位（`raw >> 16`）
- 高地址：低 16 位（`raw & 0xFFFF`）

与现有工程邻塔数据打包逻辑保持一致。

## 7. 与 OTA / 离线管理的协同

在 `eth_manage_task` 主循环中：

- 若 `g_ota.state != OTA_SVC_IDLE`，线程短暂延时并跳过常规处理（降低 OTA 干扰）。
- 正常情况下每 `100 ms` 调用 `offline_manage_update_event(OFFLINE_ETH_MANAGE)`，用于离线监控保活。

另外，`OFFLINE_READ_LOAD_WEIGHT` 事件（当前默认禁用）首次离线时会清空吊重相关寄存器并清 bit3（见 `offline_event_table.c`）。

## 8. 关键常量（当前实现）

- UDP 端口：`1212`
- 静态 IP：`192.168.1.33`
- 网关：`192.168.1.1`
- 掩码：`255.255.255.0`
- 协议 ID：`0x33532D00`（小端读取）
- 主索引：`0x0101`
- 版本魔术字：`0x73 0x17`
- 接收缓存：`512` 字节

## 9. 验收建议

1. PC 向 `192.168.1.33:1212` 发送合法 UDP 载荷。
2. 读取 holding register，确认：
   - `0x000A/0x000B`、`0x000C/0x000D`、`0x000E/0x000F`、`0x0010/0x0011` 已更新。
   - `REG_DATA_VAILD` bit3 已置位。
3. 发送非法帧（错误协议 ID、错误长度、错误版本标识），确认寄存器不更新且 bit3 不被错误置位。
4. 断网或停止数据输入，观察离线事件行为是否符合预期。

## 10. 迁移到 Zephyr 时的对照点

迁移时建议保持以下行为一致：

- PHY 复位时序（100 ms low + 100 ms high）。
- 静态 IP 参数与 UDP 1212 监听。
- 协议校验条件与 4 个 float 偏移。
- float 到 holding register 的高/低字写入顺序。
- `REG_DATA_VAILD` bit3 的置位语义。
- OTA 非空闲时对网络业务的让路策略。
- `OFFLINE_ETH_MANAGE` 周期保活更新。
