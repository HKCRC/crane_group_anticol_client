#include "ota_service_task.h"
#include "ota_ymodem_protocol.h"

#include "stm32h7xx_hal.h"
#include "stm32h7xx_hal_flash.h"
#include "stm32h7xx_hal_flash_ex.h"

#include "board_manage.h"
#include "uart_manage.h"
#include "offline_manage_task.h"

#define LOG_I(...) printf(__VA_ARGS__)
#define LOG_E(...) printf(__VA_ARGS__)
#define LOG_D(...) printf(__VA_ARGS__)

ota_service_t g_ota;

static uint32_t ota_init_crc32(void);
static void ota_reset_runtime_state(ota_service_t *svc);

static int ota_interface_send(const uint8_t *buf, uint16_t len, void *user)
{
	(void)user;
	
	// 创建临时缓冲区，添加"2,"前缀
	uint8_t temp_buf[128U];
	temp_buf[0] = '2';
	temp_buf[1] = ',';
	
	// 复制原始数据到前缀后面
	if ((buf != NULL) && (len > 0U))
	{
		memcpy(&temp_buf[2], buf, len);
	}
	
	// 发送带前缀的数据
	return uart_manage_dma_send_by_name("4g", temp_buf, len + 2U);
}

int ota_start_transfer_callback(void)
{
	if ((g_ota.state == OTA_SVC_RECEIVING) || (g_ota.state == OTA_SVC_VERIFYING) || (g_ota.state == OTA_SVC_WRITING_META))
	{
		return -1;
	}

	if (g_ota.state != OTA_SVC_IDLE)
	{
		ota_reset_runtime_state(&g_ota);
	}

	if (g_ota.state != OTA_SVC_IDLE)
	{
		return -1;
	}

	if (g_ota.rx_queue != NULL)
	{
		(void)osMessageQueueReset(g_ota.rx_queue);
	}

	g_ota.ymodem.state = OTA_YMODEM_STATE_WAIT_HEADER;
	g_ota.ymodem.frame_len = 0U;
	g_ota.ymodem.frame_expected = 0U;
	g_ota.ymodem.block_num = 1U;
	g_ota.ymodem.eot_count = 0U;
	g_ota.ymodem.file_size = 0U;
	g_ota.received_size = 0U;
	g_ota.image_size = 0U;
	g_ota.write_addr = g_ota.temp_base;
	g_ota.cache_len = 0U;
	g_ota.crc32 = ota_init_crc32();
	g_ota.session_start_tick = (uint32_t)xTaskGetTickCount();
	g_ota.state = OTA_SVC_WAIT_START;

	offline_manage_disable_all_event();

	return 0;
}

int ota_reset_transfer_callback(void)
{
	ota_reset_runtime_state(&g_ota);
	return 0;
}

static uint32_t ota_init_crc32(void)
{
	return 0xFFFFFFFFU;
}

static void ota_reset_runtime_state(ota_service_t *svc)
{
	if (svc == NULL)
	{
		return;
	}

	if (svc->state != OTA_SVC_IDLE)
	{
		(void)ymodem_request_abort(&svc->ymodem);
	}

	if (svc->rx_queue != NULL)
	{
		(void)osMessageQueueReset(svc->rx_queue);
	}

	{
		ota_ymodem_callbacks_t cb = svc->ymodem.cb;
		ymodem_init_procotol(&svc->ymodem, &cb);
	}

	svc->state = OTA_SVC_IDLE;
	svc->last_error = 0;
	svc->reboot_pending = 0U;
	svc->received_size = 0U;
	svc->image_size = 0U;
	svc->write_addr = svc->temp_base;
	svc->cache_len = 0U;
	svc->crc32 = ota_init_crc32();
	svc->session_start_tick = 0U;
}

static uint32_t ota_update_crc32(uint32_t crc, const uint8_t *data, uint32_t len)
{
	uint32_t c = crc;
	for (uint32_t i = 0U; i < len; ++i)
	{
		c ^= (uint32_t)data[i];
		for (uint8_t b = 0U; b < 8U; ++b)
		{
			if ((c & 1U) != 0U)
			{
				c = (c >> 1U) ^ 0xEDB88320U;
			}
			else
			{
				c >>= 1U;
			}
		}
	}
	return c;
}

static uint32_t ota_finalize_crc32(uint32_t crc)
{
	return crc ^ 0xFFFFFFFFU;
}

static uint32_t ota_get_bank(uint32_t address)
{
	return (address < 0x08100000U) ? FLASH_BANK_1 : FLASH_BANK_2;
}

static uint32_t ota_get_sector(uint32_t address)
{
	uint32_t bank_base = (address < 0x08100000U) ? 0x08000000U : 0x08100000U;
	uint32_t sector = (address - bank_base) / OTA_TEMP_FLASH_SECTOR_SIZE;
	return (uint32_t)FLASH_SECTOR_0 + sector;
}

static int ota_flash_erase_range(uint32_t start, uint32_t length)
{
	if (length == 0U)
	{
		return -1;
	}

	uint32_t end = start + length - 1U;
	uint32_t bank = ota_get_bank(start);
	uint32_t first_sector = ota_get_sector(start);
	uint32_t last_sector = ota_get_sector(end);

	FLASH_EraseInitTypeDef erase = {0};
	uint32_t error = 0U;

	erase.TypeErase = FLASH_TYPEERASE_SECTORS;
	erase.Banks = bank;
	erase.Sector = first_sector;
	erase.NbSectors = (last_sector - first_sector) + 1U;

	HAL_FLASH_Unlock();
	HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&erase, &error);
	HAL_FLASH_Lock();

	return (st == HAL_OK) ? 0 : -1;
}

static int ota_flash_program_flashword(uint32_t addr, const uint8_t *data)
{
	if ((addr % OTA_FLASHWORD_SIZE) != 0U)
	{
		LOG_E("OTA flashword alignment error: 0x%08lX\r\n", (unsigned long)addr);
		return -1;
	}

	HAL_FLASH_Unlock();
	HAL_StatusTypeDef st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, addr, (uint64_t)(uintptr_t)data);
	HAL_FLASH_Lock();

	if (st != HAL_OK)
	{
		LOG_E("OTA flash program failed at 0x%08lX, status=%d\r\n", (unsigned long)addr, (int)st);
	}
    
	return (st == HAL_OK) ? 0 : -1;
}

static int ota_flash_write_stream(ota_service_t *svc, const uint8_t *data, uint32_t len)
{
	if ((svc == NULL) || (data == NULL))
	{
		return -1;
	}

	uint32_t offset = 0U;
	while (offset < len)
	{
		uint32_t space = OTA_FLASHWORD_SIZE - svc->cache_len;
		uint32_t to_copy = len - offset;
		if (to_copy > space)
		{
			to_copy = space;
		}

		memcpy(&svc->cache[svc->cache_len], &data[offset], to_copy);
		svc->cache_len += to_copy;
		offset += to_copy;

		if (svc->cache_len == OTA_FLASHWORD_SIZE)
		{
			if (ota_flash_program_flashword(svc->write_addr, svc->cache) != 0)
			{
				return -1;
			}
			svc->write_addr += OTA_FLASHWORD_SIZE;
			svc->cache_len = 0U;
		}
	}

	return 0;
}

static int ota_flash_flush_cache(ota_service_t *svc)
{
	if (svc == NULL)
	{
		return -1;
	}

	if (svc->cache_len == 0U)
	{
		return 0;
	}

	for (uint32_t i = svc->cache_len; i < OTA_FLASHWORD_SIZE; ++i)
	{
		svc->cache[i] = 0xFFU;
	}

	if (ota_flash_program_flashword(svc->write_addr, svc->cache) != 0)
	{
		return -1;
	}

	svc->write_addr += OTA_FLASHWORD_SIZE;
	svc->cache_len = 0U;
	return 0;
}

static int ota_protocol_on_begin(const char *filename, uint32_t size, void *user)
{
	ota_service_t *svc = (ota_service_t *)user;
	(void)filename;

	if ((svc == NULL) || (size == 0U) || (size > svc->max_size))
	{
		return -1;
	}

	svc->image_size = size;
	svc->received_size = 0U;
	svc->write_addr = svc->temp_base;
	svc->cache_len = 0U;
	svc->crc32 = ota_init_crc32();

	if (ota_flash_erase_range(svc->temp_base, svc->temp_limit - svc->temp_base) != 0)
	{
		return -1;
	}

	svc->state = OTA_SVC_RECEIVING;
	LOG_I("OTA begin: filename=%s size=%lu\r\n", filename ? filename : "<null>", (unsigned long)size);
		/* header received: consumer task will stop sending 'C' */
	return 0;
}

static int ota_protocol_on_data(const uint8_t *data, uint32_t len, void *user)
{
	ota_service_t *svc = (ota_service_t *)user;
	if ((svc == NULL) || (data == NULL) || (len == 0U))
	{
		return -1;
	}

	uint32_t remaining = (svc->image_size > svc->received_size) ? (svc->image_size - svc->received_size) : 0U;
	uint32_t write_len = (len > remaining) ? remaining : len;
	if (write_len == 0U)
	{
		return -1;
	}

	if (ota_flash_write_stream(svc, data, write_len) != 0)
	{
		return -1;
	}

	svc->crc32 = ota_update_crc32(svc->crc32, data, write_len);
	svc->received_size += write_len;
	return 0;
}

static int ota_protocol_on_finish(uint32_t size, void *user)
{
	ota_service_t *svc = (ota_service_t *)user;
	if (svc == NULL)
	{
		return -1;
	}

	if (size != svc->image_size)
	{
		return -1;
	}

	svc->state = OTA_SVC_VERIFYING;
	if (ota_flash_flush_cache(svc) != 0)
	{
		return -1;
	}

	boot_metadata_t meta = {0};
	meta.magic = OTA_META_MAGIC;
	meta.image_size = svc->image_size;
	meta.image_crc32 = ota_finalize_crc32(svc->crc32);

	svc->state = OTA_SVC_WRITING_META;
	if (ota_flash_erase_range(svc->meta_base, OTA_METADATA_SIZE) != 0)
	{
		return -1;
	}

	if (ota_flash_program_flashword(svc->meta_base, (const uint8_t *)&meta) != 0)
	{
		return -1;
	}

	LOG_I("OTA metadata written: size=%lu crc=0x%08lX\r\n",
	      (unsigned long)meta.image_size,
	      (unsigned long)meta.image_crc32);

		svc->state = OTA_SVC_COMPLETED;
		svc->reboot_pending = 1U;
		/* stop sending 'C' handled by consumer loop */

	return 0;
}

static void ota_protocol_on_error(int err, void *user)
{
	ota_service_t *svc = (ota_service_t *)user;
	if (svc == NULL)
	{
		return;
	}

	svc->last_error = err;
	svc->state = OTA_SVC_FAILED;
	svc->reboot_pending = 0U;
		/* stop sending 'C' handled by consumer loop */
	(void)ota_flash_erase_range(svc->temp_base, svc->temp_limit - svc->temp_base);

}

static void ota_init_ymodem_procotol(ota_service_t *svc)
{
	ota_ymodem_callbacks_t cb;
	memset(&cb, 0, sizeof(cb));
	cb.send = ota_interface_send;
	cb.on_begin = ota_protocol_on_begin;
	cb.on_data = ota_protocol_on_data;
	cb.on_finish = ota_protocol_on_finish;
	cb.on_error = ota_protocol_on_error;
	cb.user = svc;

	ymodem_init_procotol(&svc->ymodem, &cb);
}

static void ota_consumer_task(void *argument)
{
	ota_service_t *svc = (ota_service_t *)argument;
	ota_rx_chunk_t msg;
	uint32_t sendc_acc = 0U; /* ms accumulator */
	const uint32_t poll_ms = 100U;
	for (;;)
	{
        vTaskDelay(pdMS_TO_TICKS(10));
		FEED_SYS_WATCHDOG();

		if (svc->state != OTA_SVC_IDLE)
		{
			uint32_t elapsed_ticks =
				(uint32_t)xTaskGetTickCount() - svc->session_start_tick;
			if (elapsed_ticks >= (uint32_t)pdMS_TO_TICKS(OTA_SESSION_TIMEOUT_MS))
			{
				LOG_E("OTA session timeout after 30 minutes, exiting OTA mode\r\n");
				ota_reset_runtime_state(svc);
				sendc_acc = 0U;
				continue;
			}
		}

		osStatus_t st = osMessageQueueGet(svc->rx_queue, &msg, NULL, poll_ms);
		if (st == osOK)
		{
			int err = ymodem_feed_frame(&svc->ymodem, msg.data, msg.len);
			if (err != 0)
			{
				LOG_E("OTA Ymodem feed error: %d\r\n", err);
			}
			/* reset accumulator when data arrives */
			sendc_acc = 0U;

			if (svc->reboot_pending != 0U)
			{
				LOG_I("OTA complete, rebooting after flush...\r\n");
				osDelay(3000U);
				NVIC_SystemReset();
			}
		}
		else
		{
			/* timeout */
			if (svc->state == OTA_SVC_WAIT_START)
			{
				sendc_acc += poll_ms;
				if (sendc_acc >= 5000U)
				{
					/* protocol layer handles handshake send */
					(void)ymodem_request_start(&svc->ymodem);
					sendc_acc = 0U;
				}
			}
			else
			{
				sendc_acc = 0U;
			}
		}
	}
}

int ota_init_service(uint32_t temp_fw_addr, uint32_t fw_max_size)
{
	memset(&g_ota, 0, sizeof(g_ota));
	g_ota.temp_base = temp_fw_addr;
	g_ota.temp_limit = temp_fw_addr + fw_max_size;
	g_ota.meta_base = OTA_METADATA_BASE_ADDR;
	g_ota.max_size = fw_max_size;
	g_ota.state = OTA_SVC_IDLE;
	g_ota.last_error = 0;
	g_ota.reboot_pending = 0U;
	g_ota.session_start_tick = 0U;

	ota_init_ymodem_procotol(&g_ota);

	return 0;
}

osThreadId_t ota_create_consumer_task(void)
{
	if (g_ota.rx_queue == NULL)
	{
		g_ota.rx_queue = osMessageQueueNew(OTA_QUEUE_DEPTH, sizeof(ota_rx_chunk_t), NULL);
		if (g_ota.rx_queue == NULL)
		{
			return NULL;
		}
	}

	if (g_ota.consumer_task == NULL)
	{
		osThreadAttr_t attr = {0};
		attr.name = "ota_consumer";
		attr.stack_size = 4096 * 4U;
		attr.priority = osPriorityHigh;
		g_ota.consumer_task = osThreadNew(ota_consumer_task, &g_ota, &attr);
	}

	return g_ota.consumer_task;
}

