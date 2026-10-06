#include "report_atc_data_task.h"
#include "modbus_register_interface.h"
#include "modbus_register_database.h"
#include "main.h"
#include "board_manage.h"
#include "uart_manage.h"
#include "ota_service_task.h"
#include "offline_manage_task.h"

#define LOG_D(...) // printf(__VA_ARGS__)
#define LOG_I(...) printf(__VA_ARGS__)  
#define LOG_E(...) printf(__VA_ARGS__)

extern UART_HandleTypeDef huart8;

#define LTE_FRAME_HEADER 0xA5U
#define LTE_PROTOCOL_V30 0x30U
#define LTE_PROTOCOL_V31 0x31U

#if ((CONFIG_UPLOAD_PROTOCOL_VERSION != LTE_PROTOCOL_V30) && (CONFIG_UPLOAD_PROTOCOL_VERSION != LTE_PROTOCOL_V31))
#error "CONFIG_UPLOAD_PROTOCOL_VERSION must be 0x30 or 0x31"
#endif

#if (CONFIG_UPLOAD_PROTOCOL_VERSION == LTE_PROTOCOL_V31)
#define LTE_DATA_LEN 30U
#else
#define LTE_DATA_LEN 26U
#endif

#define LTE_CAPABILITY_FLAG 0x00U
#define LTE_FRAME_LEN (4U + LTE_DATA_LEN + 2U)

static void send_hourly_4g_command(void)
{
    static TickType_t last_send_tick = 0U;
    static const uint8_t keepalive_cmd[] = "usr.cn#AT+Z\r\n";
    const TickType_t now = xTaskGetTickCount();
    const TickType_t interval = pdMS_TO_TICKS(60U * 60U * 1000U);

    if (last_send_tick == 0U)
    {
        last_send_tick = now;
        return;
    }

    if ((TickType_t)(now - last_send_tick) >= interval)
    {
        (void)uart_manage_dma_send_by_name("4g", (uint8_t *)keepalive_cmd, (uint16_t)(sizeof(keepalive_cmd) - 1U));
        last_send_tick = now;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}

void neighbor_report_interface_send(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        return;
    }

	// 创建临时缓冲区，添加"3,"前缀
	uint8_t temp_buf[len + 2U];
	temp_buf[0] = '3';
	temp_buf[1] = ',';
	
	// 复制原始数据到前缀后面
	if ((data != NULL) && (len > 0U))
	{
		memcpy(&temp_buf[2], data, len);
	}

    LOG_D("Neighbor Report ATC Task: total length: %u\r\n", len + 2U);

    /* Use uart_manage DMA send by name registered as "4g" for huart8 */
    (void)uart_manage_dma_send_by_name("4g", (uint8_t *)temp_buf, len + 2U);
}

// CRC16-CCITT (poly 0x1021) initial 0x0000
static uint16_t crc16_ccitt(const uint8_t *buf, uint32_t len)
{
    uint16_t crc = 0;
    for (uint32_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)buf[i] << 8;
        for (uint8_t j = 0; j < 8; ++j) {
            if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x1021);
            else crc <<= 1;
        }
    }
    return crc;
}

// Pack LTE payload data fields
// Common layout(26 bytes):
// [valid(2)] [swing_cnt(2)] [swing_val(2)] [luff_cnt(2)] [luff_val(2)]
// [moment(4)] [moment_pct(4)] [weight(4)] [weight_pct(4)]
// v3.1 appends: [hook_lowering_height(4)]
static void pack_neighbor_data(uint8_t *out)
{
#if (CONFIG_UPLOAD_PROTOCOL_VERSION == LTE_PROTOCOL_V31)
    static const uint16_t addr_list[] = {
        REG_DATA_VAILD,
        REG_SWING_ENCODER_COUNT,
        REG_SWING_ENCODER_VALUE,
        REG_LUFFING_ENCODER_COUNT,
        REG_LUFFING_ENCODER_VALUE,
        REG_LIFTING_MOMENT,
        REG_LIFTING_MOMENT + 1U,
        REG_LIFTING_MOMENT_PCT,
        REG_LIFTING_MOMENT_PCT + 1U,
        REG_LOAD_WEIGHT,
        REG_LOAD_WEIGHT + 1U,
        REG_LOAD_WEIGHT_PCT,
        REG_LOAD_WEIGHT_PCT + 1U,
        REG_HOOK_LOWERING_HEIGHT,
        REG_HOOK_LOWERING_HEIGHT + 1U
    };
#else
    static const uint16_t addr_list[] = {
        REG_DATA_VAILD,
        REG_SWING_ENCODER_COUNT,
        REG_SWING_ENCODER_VALUE,
        REG_LUFFING_ENCODER_COUNT,
        REG_LUFFING_ENCODER_VALUE,
        REG_LIFTING_MOMENT,
        REG_LIFTING_MOMENT + 1U,
        REG_LIFTING_MOMENT_PCT,
        REG_LIFTING_MOMENT_PCT + 1U,
        REG_LOAD_WEIGHT,
        REG_LOAD_WEIGHT + 1U,
        REG_LOAD_WEIGHT_PCT,
        REG_LOAD_WEIGHT_PCT + 1U
    };
#endif
    uint16_t values[sizeof(addr_list) / sizeof(addr_list[0])];

    if (mb_reg_read_multi_u16(addr_list, (uint16_t)(sizeof(values) / sizeof(values[0])), values) != ERR_OK) {
        memset(out, 0, LTE_DATA_LEN);
        return;
    }

    /* Write u16 fields (little-endian per-field: low byte first) */
    out[0] = (uint8_t)(values[0] & 0xFFU);
    out[1] = (uint8_t)(values[0] >> 8);

    out[2] = (uint8_t)(values[1] & 0xFFU);
    out[3] = (uint8_t)(values[1] >> 8);

    out[4] = (uint8_t)(values[2] & 0xFFU);
    out[5] = (uint8_t)(values[2] >> 8);

    out[6] = (uint8_t)(values[3] & 0xFFU);
    out[7] = (uint8_t)(values[3] >> 8);

    out[8] = (uint8_t)(values[4] & 0xFFU);
    out[9] = (uint8_t)(values[4] >> 8);

    /* 32-bit fields: assemble from two u16 registers (high-word << 16 | low-word),
       then write bytes in little-endian order into the frame (LSB first). */
    {
        uint32_t raw = ((uint32_t)values[5] << 16) | values[6];
        out[10] = (uint8_t)(raw & 0xFFU);
        out[11] = (uint8_t)((raw >> 8) & 0xFFU);
        out[12] = (uint8_t)((raw >> 16) & 0xFFU);
        out[13] = (uint8_t)((raw >> 24) & 0xFFU);
    }
    {
        uint32_t raw = ((uint32_t)values[7] << 16) | values[8];
        out[14] = (uint8_t)(raw & 0xFFU);
        out[15] = (uint8_t)((raw >> 8) & 0xFFU);
        out[16] = (uint8_t)((raw >> 16) & 0xFFU);
        out[17] = (uint8_t)((raw >> 24) & 0xFFU);
    }
    {
        uint32_t raw = ((uint32_t)values[9] << 16) | values[10];
        out[18] = (uint8_t)(raw & 0xFFU);
        out[19] = (uint8_t)((raw >> 8) & 0xFFU);
        out[20] = (uint8_t)((raw >> 16) & 0xFFU);
        out[21] = (uint8_t)((raw >> 24) & 0xFFU);
    }
    {
        uint32_t raw = ((uint32_t)values[11] << 16) | values[12];
        out[22] = (uint8_t)(raw & 0xFFU);
        out[23] = (uint8_t)((raw >> 8) & 0xFFU);
        out[24] = (uint8_t)((raw >> 16) & 0xFFU);
        out[25] = (uint8_t)((raw >> 24) & 0xFFU);
    }

#if (CONFIG_UPLOAD_PROTOCOL_VERSION == LTE_PROTOCOL_V31)
    {
        uint32_t raw = ((uint32_t)values[13] << 16) | values[14];
        out[26] = (uint8_t)(raw & 0xFFU);
        out[27] = (uint8_t)((raw >> 8) & 0xFFU);
        out[28] = (uint8_t)((raw >> 16) & 0xFFU);
        out[29] = (uint8_t)((raw >> 24) & 0xFFU);
    }
#endif

#if (ENCODER_FAKE_DATA_MODE == 1)
    /* Fake data aligned to the defined layout: u16s then 4 floats (32-bit little-endian) */
    uint16_t fake_valid = 0b00001011;
    out[0] = (uint8_t)(fake_valid & 0xFFU);
    out[1] = (uint8_t)(fake_valid >> 8);

    for (int i = 0; i < 4; ++i) {
        uint16_t fake_int = 0x01 + i;
        out[2 + i * 2] = (uint8_t)(fake_int & 0xFFU);
        out[3 + i * 2] = (uint8_t)(fake_int >> 8);
    }

    for (size_t i = 0; i < 4; i++) {
        float fake_float = 0.99f + (float)i;
        uint32_t raw;
        memcpy(&raw, &fake_float, sizeof(raw));
        /* ensure frame uses little-endian byte order for 32-bit fields */
        out[10 + i * 4] = (uint8_t)(raw & 0xFFU);
        out[11 + i * 4] = (uint8_t)((raw >> 8) & 0xFFU);
        out[12 + i * 4] = (uint8_t)((raw >> 16) & 0xFFU);
        out[13 + i * 4] = (uint8_t)((raw >> 24) & 0xFFU);
    }

#if (CONFIG_UPLOAD_PROTOCOL_VERSION == LTE_PROTOCOL_V31)
    {
        float fake_hook_height = 4.56f;
        uint32_t raw;
        memcpy(&raw, &fake_hook_height, sizeof(raw));
        out[26] = (uint8_t)(raw & 0xFFU);
        out[27] = (uint8_t)((raw >> 8) & 0xFFU);
        out[28] = (uint8_t)((raw >> 16) & 0xFFU);
        out[29] = (uint8_t)((raw >> 24) & 0xFFU);
    }
#endif
#endif

}

#include "ota_service_task.h"
extern ota_service_t g_ota;
void report_atc_data_task(void *argument)
{
    (void)argument;
    const TickType_t period = pdMS_TO_TICKS(1000U);
    TickType_t last_wake_time = xTaskGetTickCount();
    uint8_t frame[LTE_FRAME_LEN]; // [header][protocol][capability][data_len][payload][crc16]
    frame[0] = LTE_FRAME_HEADER;
    frame[1] = CONFIG_UPLOAD_PROTOCOL_VERSION;
    frame[2] = LTE_CAPABILITY_FLAG;
    frame[3] = LTE_DATA_LEN;

    for (;;) {
        
        if (g_ota.state != OTA_SVC_IDLE)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        vTaskDelayUntil(&last_wake_time, period);
        offline_manage_update_event(OFFLINE_PUBLISH_MQTT);

        // send_hourly_4g_command();

        // pack data
        pack_neighbor_data(&frame[4]);

        // compute CRC over [header..payload]
        uint16_t crc = crc16_ccitt(frame, (uint32_t)(LTE_FRAME_LEN - 2U));
        frame[LTE_FRAME_LEN - 2U] = (uint8_t)(crc & 0xFFU);
        frame[LTE_FRAME_LEN - 1U] = (uint8_t)(crc >> 8);

        // transmit via UART8 (blocking)
        neighbor_report_interface_send(frame, sizeof(frame));
    }
}
