#ifndef OTA_SERVICE_TASK_H_
#define OTA_SERVICE_TASK_H_

#include <stdint.h>
#include "cmsis_os.h"
#include "ota_ymodem_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_TEMP_FLASH_SECTOR_SIZE       (128U * 1024U)
#define OTA_METADATA_BASE_ADDR           0x081E0000U
#define OTA_METADATA_SIZE                (128U * 1024U)
#define OTA_META_MAGIC                   0x4F544131U
#define OTA_QUEUE_DEPTH                  8U
#define OTA_FLASHWORD_SIZE               32U
#define OTA_SESSION_TIMEOUT_MS           (30U * 60U * 1000U)

#define OTA_CHUNK_MAX_SIZE 1030U

typedef struct
{
	uint32_t magic;
	uint32_t image_size;
	uint32_t image_crc32;
	uint32_t reserved[5];
} boot_metadata_t;

typedef enum
{
	OTA_SVC_IDLE = 0,
	OTA_SVC_WAIT_START,
	OTA_SVC_RECEIVING,
	OTA_SVC_VERIFYING,
	OTA_SVC_WRITING_META,
	OTA_SVC_COMPLETED,
	OTA_SVC_FAILED
} ota_service_state_t;

typedef struct
{
	uint16_t len;
	uint8_t data[OTA_CHUNK_MAX_SIZE];
} ota_rx_chunk_t;

typedef struct
{
	uint32_t temp_base;
	uint32_t temp_limit;
	uint32_t meta_base;
	uint32_t max_size;

	uint32_t write_addr;
	uint32_t image_size;
	uint32_t received_size;
	uint32_t crc32;

	uint8_t cache[OTA_FLASHWORD_SIZE] __attribute__((aligned(32)));
	uint32_t cache_len;

	ota_service_state_t state;
	int last_error;
	uint8_t reboot_pending;
	uint32_t session_start_tick;

	osMessageQueueId_t rx_queue;
	osThreadId_t consumer_task;

	ota_ymodem_ctx_t ymodem;
} ota_service_t;

extern ota_service_t g_ota;

/**
 * Initialize OTA service on UART8
 * Must be called once during system setup, before task creation
 * 
 * @param temp_fw_addr: Base address of temporary firmware area (0x08020000)
 * @param fw_max_size: Maximum firmware size (896KB)
 * @return 0 on success, negative on error
 */
int ota_init_service(uint32_t temp_fw_addr, uint32_t fw_max_size);

/**
 * Create OTA consumer task
 * Must be called after ota_init_service()
 * This task will consume data from the OTA queue and process Ymodem protocol
 * 
 * @return Task ID on success, NULL on error
 */
osThreadId_t ota_create_consumer_task(void);

/**
 * Start OTA firmware transfer on UART8
 * Sends initial 'C' to request Ymodem transmission from host
 * @return 0 on success, negative on error
 */
int ota_start_transfer_callback(void);

/**
 * Reset OTA firmware transfer state and abort any active Ymodem session
 * @return 0 on success, negative on error
 */
int ota_reset_transfer_callback(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_SERVICE_TASK_H_ */
