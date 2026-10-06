# LTE 数据上传协议 3.0 与 3.1 差异

## 1. 配置方式

LTE 二进制数据上传协议由编译期宏决定：

```c
CONFIG_UPLOAD_PROTOCOL_VERSION
```

允许值只有：

- `0x30`：协议 3.0，不包含吊钩下降高度。
- `0x31`：协议 3.1，包含吊钩下降高度。

当前工程默认值在 `application/report_atc_data_task.h` 中定义为 `0x31`。修改版本后必须重新编译和烧录固件，运行期间不能动态切换。

## 2. 共同规则

两个版本均采用以下规则：

- 帧头 `header` 固定为 `0xA5`。
- `protocol` 使用 BCD 表示版本：`0x30` 表示 3.0，`0x31` 表示 3.1。
- 多字节整数使用 little-endian。
- 浮点数使用 IEEE-754 `float32`，按 little-endian 传输。
- CRC 使用 CRC-16/CCITT，初始值为 `0x0000`，多项式为 `0x1021`。
- CRC 覆盖从 `header` 到数据区末尾的所有字节，不包含 CRC 字段自身。

总帧长度计算：

```text
frame_len = 4 + data_len + 2
```

其中前 4 字节是帧头，最后 2 字节是 CRC。

## 3. 差异汇总

| 项目 | 协议 3.0 | 协议 3.1 |
|---|---:|---:|
| `protocol` | `0x30` | `0x31` |
| `data_len` | 26 | 30 |
| 总帧长度 | 32 字节 | 36 字节 |
| CRC 偏移 | 30 | 34 |
| 吊钩下降高度 | 无 | 有，偏移 30、长度 4 字节 |

协议 3.1 只在协议 3.0 的数据区末尾增加 `hook_lowering_height_m`。偏移 0 至 29 的字段在两个版本中完全一致。

## 4. 协议 3.0 帧结构

| 偏移 | 长度 | 类型 | 字段 | 说明 |
|---:|---:|---|---|---|
| 0 | 1 | `uint8` | header | 固定 `0xA5` |
| 1 | 1 | `uint8` | protocol | 固定 `0x30`，表示 3.0 |
| 2 | 1 | `uint8` | capability_flag | 设备能力标志 |
| 3 | 1 | `uint8` | data_len | 固定 `26` |
| 4 | 2 | `uint16` | vaild_flag | 数据有效标志 |
| 6 | 2 | `uint16` | slewing_turn_count | 回转编码器圈数 |
| 8 | 2 | `uint16` | slewing_single_value | 回转编码器单圈值 |
| 10 | 2 | `uint16` / `int16` | luffing_turn_count / imu_roll | 变幅编码器圈数或 IMU 原始值 |
| 12 | 2 | `uint16` / `int16` | luffing_single_value / imu_pitch | 变幅编码器单圈值或 IMU 原始值 |
| 14 | 4 | `float32` | lifting_moment_tm | 起重力矩 |
| 18 | 4 | `float32` | lifting_moment_pct | 起重量百分比，单位 `%` |
| 22 | 4 | `float32` | load_weight_t | 吊重，单位 `t` |
| 26 | 4 | `float32` | load_weight_pct | 吊重百分比，单位 `%` |
| 30 | 2 | `uint16` | crc16 | 对偏移 0 至 29 的 30 字节计算 |

## 5. 协议 3.1 帧结构

协议 3.1 保留协议 3.0 的全部字段，并在吊重百分比之后增加：

| 偏移 | 长度 | 类型 | 字段 | 说明 |
|---:|---:|---|---|---|
| 30 | 4 | `float32` | hook_lowering_height_m | 吊钩下降高度，单位 `m` |
| 34 | 2 | `uint16` | crc16 | 对偏移 0 至 33 的 34 字节计算 |

帧头中的 `data_len` 固定为 30，总帧长度为 36 字节。

## 6. 上位机兼容解析

上位机不应固定按 32 或 36 字节接收，而应按以下顺序解析：

1. 检查 `header == 0xA5`。
2. 读取 `protocol` 和 `data_len`。
3. 计算 `frame_len = 4 + data_len + 2`，等待完整帧。
4. 检查版本和长度组合：`0x30/26` 或 `0x31/30`。
5. 对前 `4 + data_len` 字节计算 CRC，并与末尾 2 字节比较。
6. 解析两个版本共有的偏移 0 至 29。
7. 仅当 `protocol == 0x31` 且 `data_len >= 30` 时解析偏移 30 处的吊钩下降高度。

推荐判断逻辑：

```text
if protocol == 0x30 and data_len == 26:
    parse_common_fields()
elif protocol == 0x31 and data_len == 30:
    parse_common_fields()
    parse_hook_lowering_height()
else:
    reject_as_unsupported_version_or_length()
```

不能仅检查 `protocol` 而忽略 `data_len`，也不能在 3.0 帧中读取偏移 30 的 `float32`；该位置在 3.0 中是 CRC。
