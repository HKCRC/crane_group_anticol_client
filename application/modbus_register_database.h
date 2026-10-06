#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "cmsis_os.h"
#include "task.h"

// 寄存器映射定义
// 保持寄存器 (Holding Registers) - 可读写
#define REG_SYSTEM_VERSION   0x0000
#define REG_RESERVE_0001       0x0001

#define REG_RESERVE_0002  0x0002
#define REG_DATA_VAILD 0x0003 

/* Bit definitions for REG_DATA_VAILD */
#define REG_DATA_VAILD_BIT_SWING_ENCODER   0U
#define REG_DATA_VAILD_BIT_LUFFING_ENCODER 1U
#define REG_DATA_VAILD_BIT_HOOK_ENCODER    2U
#define REG_DATA_VAILD_BIT_LOAD_WEIGHT     3U

#define REG_SWING_ENCODER_COUNT    0x0004
#define REG_SWING_ENCODER_VALUE    0x0005 
#define REG_LUFFING_ENCODER_COUNT  0x0006
#define REG_LUFFING_ENCODER_VALUE  0x0007 
#define REG_HOOK_ENCODER_COUNT     0x0008
#define REG_HOOK_ENCODER_VALUE     0x0009

#define REG_LIFTING_MOMENT     0x000A
#define REG_LIFTING_MOMENT_PCT 0x000C
#define REG_LOAD_WEIGHT        0x000E
#define REG_LOAD_WEIGHT_PCT    0x0010
#define REG_HOOK_LOWERING_HEIGHT 0x0012
#define REG_HOLDING_SIZE       0x0020



extern uint16_t holding_reg_data[REG_HOLDING_SIZE];
#ifdef __cplusplus
}
#endif