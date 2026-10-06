#include "ota_ymodem_protocol.h"

#include "board_manage.h"

#define LOG_D(...) printf(__VA_ARGS__)
#define LOG_I(...) printf(__VA_ARGS__)
#define LOG_E(...) printf(__VA_ARGS__)

#define YMODEM_SOH                0x01U
#define YMODEM_STX                0x02U
#define YMODEM_EOT                0x04U
#define YMODEM_ACK                0x06U
#define YMODEM_NAK                0x15U
#define YMODEM_CAN                0x18U
#define YMODEM_CRC_CHAR           0x43U
#define YMODEM_SOH_DATA_LEN       128U
#define YMODEM_STX_DATA_LEN       1024U
#define YMODEM_FRAME_OVERHEAD     5U

static int ymodem_send_byte(const ota_ymodem_callbacks_t *cb, uint8_t val)
{
	if ((cb == NULL) || (cb->send == NULL))
	{
		return OTA_YMODEM_ERR_PARAM;
	}
    
	vTaskDelay(60);
    LOG_D("Ymodem send byte: 0x%02X\r\n", val);
	return cb->send(&val, 1U, cb->user);
}

int ymodem_request_abort(ota_ymodem_ctx_t *ctx)
{
	if (ctx == NULL)
	{
		return OTA_YMODEM_ERR_PARAM;
	}

	(void)ymodem_send_byte(&ctx->cb, YMODEM_CAN);
	(void)ymodem_send_byte(&ctx->cb, YMODEM_CAN);
	ctx->state = OTA_YMODEM_STATE_ABORTED;
	return OTA_YMODEM_ERR_ABORTED;
}

static uint16_t ymodem_get_crc16(const uint8_t *buf, uint32_t len)
{
	uint16_t crc = 0U;

	for (uint32_t i = 0U; i < len; ++i)
	{
		crc ^= (uint16_t)buf[i] << 8;
		for (uint8_t j = 0U; j < 8U; ++j)
		{
			if ((crc & 0x8000U) != 0U)
			{
				crc = (uint16_t)((crc << 1U) ^ 0x1021U);
			}
			else
			{
				crc <<= 1U;
			}
		}
	}

	return crc;
}

static void ymodem_notify_error(ota_ymodem_ctx_t *ctx, int err)
{
	if ((ctx != NULL) && (ctx->cb.on_error != NULL))
	{
		ctx->cb.on_error(err, ctx->cb.user);
	}
}

static int ymodem_handle_control(ota_ymodem_ctx_t *ctx, uint8_t val)
{
	if (ctx == NULL)
	{
		return OTA_YMODEM_ERR_PARAM;
	}
    
    // 正常传输结束：两次确认（NAK → ACK），RECV_DATA → WAIT_EOT → WAIT_LAST_EMPTY → DONE
	if (val == YMODEM_EOT)
	{
		if (ctx->state == OTA_YMODEM_STATE_RECV_DATA)
		{
			(void)ymodem_send_byte(&ctx->cb, YMODEM_NAK);
			ctx->state = OTA_YMODEM_STATE_WAIT_EOT;
			ctx->eot_count = 1U;
			return OTA_YMODEM_OK;
		}

		if (ctx->state == OTA_YMODEM_STATE_WAIT_EOT)
		{
			(void)ymodem_send_byte(&ctx->cb, YMODEM_ACK);
			(void)ymodem_send_byte(&ctx->cb, YMODEM_CRC_CHAR);
			ctx->state = OTA_YMODEM_STATE_WAIT_LAST_EMPTY;
			return OTA_YMODEM_OK;
		}
	}
    
    // 异常中止：无握手，立即生效，任意状态 → ABORTED
	if (val == YMODEM_CAN)
	{
		ctx->state = OTA_YMODEM_STATE_ABORTED;
		ymodem_notify_error(ctx, OTA_YMODEM_ERR_ABORTED);
		return OTA_YMODEM_ERR_ABORTED;
	}

	return OTA_YMODEM_OK;
}

static int ymodem_handle_header(ota_ymodem_ctx_t *ctx, const uint8_t *data, uint16_t len)
{
	if ((ctx == NULL) || (data == NULL) || (len == 0U))
	{
		return OTA_YMODEM_ERR_PARAM;
	}
     
    // 获取文件名
	const char *filename = (const char *)data;
    // 传输最终确认包
	if (filename[0] == '\0')
	{
		ctx->state = OTA_YMODEM_STATE_DONE;
		if (ctx->cb.on_finish != NULL)
		{
			int ret = ctx->cb.on_finish(ctx->file_size, ctx->cb.user);
			if (ret != 0)
			{
				(void)ymodem_request_abort(ctx);
				ymodem_notify_error(ctx, ret);
				return ret;
			}
		}
		(void)ymodem_send_byte(&ctx->cb, YMODEM_ACK);
		return OTA_YMODEM_OK;
	}
    
    // 首包信息包
    // 获取文件大小
	const char *size_str = filename + strlen(filename) + 1U;
	uint32_t size_val = 0U;
	while ((*size_str >= '0') && (*size_str <= '9'))
	{
		size_val = (size_val * 10U) + (uint32_t)(*size_str - '0');
		size_str++;
	}
	ctx->file_size = size_val;
    
    // 让上层应用准备接收环境（擦除 Flash）
	if (ctx->cb.on_begin != NULL)
	{
		int ret = ctx->cb.on_begin(filename, size_val, ctx->cb.user);
		if (ret != 0)
		{
			(void)ymodem_request_abort(ctx);
			ymodem_notify_error(ctx, ret);
			return ret;
		}
	}
    
    // 发送 ACK + CRC 请求数据包，进入数据接收状态
	(void)ymodem_send_byte(&ctx->cb, YMODEM_ACK);
	(void)ymodem_send_byte(&ctx->cb, YMODEM_CRC_CHAR);
	ctx->state = OTA_YMODEM_STATE_RECV_DATA;
	ctx->block_num = 1U;
	ctx->eot_count = 0U;
	return OTA_YMODEM_OK;
}

static int ymodem_handle_data(ota_ymodem_ctx_t *ctx, const uint8_t *data, uint16_t len)
{
	if ((ctx == NULL) || (data == NULL) || (len == 0U))
	{
		return OTA_YMODEM_ERR_PARAM;
	}

	if (ctx->cb.on_data != NULL)
	{
		int ret = ctx->cb.on_data(data, len, ctx->cb.user);
		if (ret != 0)
		{
			(void)ymodem_request_abort(ctx);
			ymodem_notify_error(ctx, ret);
			return ret;
		}
	}

	(void)ymodem_send_byte(&ctx->cb, YMODEM_ACK);
	return OTA_YMODEM_OK;
}

static int ymodem_process_frame(ota_ymodem_ctx_t *ctx)
{
    // 值检查：防御性编程，防止空指针解引用和无效操作
	if ((ctx == NULL) || (ctx->frame_len < YMODEM_FRAME_OVERHEAD))
	{
		return OTA_YMODEM_ERR_PARAM;
	}
    
    // 检查帧长
	uint8_t head = ctx->frame_buf[0];
	uint16_t data_len = (head == YMODEM_SOH) ? YMODEM_SOH_DATA_LEN : YMODEM_STX_DATA_LEN;
	if (ctx->frame_len != (uint16_t)(data_len + YMODEM_FRAME_OVERHEAD))
	{
		LOG_D("Ymodem frame length mismatch: expected %u, got %u\r\n", (unsigned)(data_len + YMODEM_FRAME_OVERHEAD), (unsigned)ctx->frame_len);
		return OTA_YMODEM_ERR_PARAM;
	}
    
    // 检查帧序号
	uint8_t blk = ctx->frame_buf[1];
	uint8_t blk_inv = ctx->frame_buf[2];
	if ((uint8_t)(blk + blk_inv) != 0xFFU)
	{
		(void)ymodem_send_byte(&ctx->cb, YMODEM_NAK);
		return OTA_YMODEM_ERR_SEQ;
	}
    
    // 检验CRC
	const uint8_t *data = &ctx->frame_buf[3];
	uint16_t crc_rx = (uint16_t)((uint16_t)ctx->frame_buf[3 + data_len] << 8) | ctx->frame_buf[4 + data_len];
	uint16_t crc_calc = ymodem_get_crc16(data, data_len);
	if (crc_rx != crc_calc)
	{
		(void)ymodem_send_byte(&ctx->cb, YMODEM_NAK);
		return OTA_YMODEM_ERR_CRC;
	}
    
    // 仅在等待文件头或结束空包时，将0号包解释为信息包。
    // 接收固件数据时，8位包序号允许从255正常回绕到0。
	if ((blk == 0U) &&
	    ((ctx->state == OTA_YMODEM_STATE_WAIT_HEADER) ||
	     (ctx->state == OTA_YMODEM_STATE_WAIT_LAST_EMPTY)))
	{
		return ymodem_handle_header(ctx, data, data_len);
	}

	if (ctx->state != OTA_YMODEM_STATE_RECV_DATA)
	{
		/* request sender to abort transfer */
		(void)ymodem_send_byte(&ctx->cb, YMODEM_CAN);
		(void)ymodem_send_byte(&ctx->cb, YMODEM_CAN);
		ctx->state = OTA_YMODEM_STATE_ABORTED;
		ymodem_notify_error(ctx, OTA_YMODEM_ERR_ABORTED);
		return OTA_YMODEM_ERR_ABORTED;
	}
 
    // 处理数据包
	if (blk == ctx->block_num)
	{
		int ret = ymodem_handle_data(ctx, data, data_len);
		if (ret == OTA_YMODEM_OK)
		{
			ctx->block_num++;
		}
		return ret;
	}
    
    // 处理重包
	if (blk == (uint8_t)(ctx->block_num - 1U))
	{
		(void)ymodem_send_byte(&ctx->cb, YMODEM_ACK);
		return OTA_YMODEM_OK;
	}

	(void)ymodem_send_byte(&ctx->cb, YMODEM_NAK);
	return OTA_YMODEM_ERR_SEQ;
}

int ymodem_feed_frame(ota_ymodem_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
	if ((ctx == NULL) || (frame == NULL) || (len == 0U))
	{
		return OTA_YMODEM_ERR_PARAM;
	}

	if (len == 1U)
	{
		uint8_t ctrl = frame[0];
		if ((ctrl == YMODEM_EOT) || (ctrl == YMODEM_CAN))
		{
			return ymodem_handle_control(ctx, ctrl);
		}
		return OTA_YMODEM_ERR_PARAM;
	}

	if (len > (uint16_t)sizeof(ctx->frame_buf))
	{
		return OTA_YMODEM_ERR_OVERFLOW;
	}

	memcpy(ctx->frame_buf, frame, len);
	ctx->frame_len = len;
	ctx->frame_expected = len;
	int ret = ymodem_process_frame(ctx);
	ctx->frame_expected = 0U;
	ctx->frame_len = 0U;
	return ret;
}

void ymodem_init_procotol(ota_ymodem_ctx_t *ctx, const ota_ymodem_callbacks_t *cb)
{
	if (ctx == NULL)
	{
		return;
	}

	memset(ctx, 0, sizeof(*ctx));
	if (cb != NULL)
	{
		ctx->cb = *cb;
	}
	ctx->state = OTA_YMODEM_STATE_WAIT_HEADER;
}

int ymodem_request_start(ota_ymodem_ctx_t *ctx)
{
	if ((ctx == NULL) || (ctx->cb.send == NULL))
	{
		return OTA_YMODEM_ERR_PARAM;
	}
    
    return ymodem_send_byte(&ctx->cb, YMODEM_CRC_CHAR);
}
