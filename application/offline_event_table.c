#include "offline_event_table.h"
#include "board_manage.h"
#include "offline_manage_task.h"
#include "modbus_register_database.h"
#include "modbus_register_interface.h"

#include <stdio.h>

#define LOG_D(...) // printf(__VA_ARGS__)
#define LOG_I(...) printf(__VA_ARGS__)
#define LOG_E(...) printf(__VA_ARGS__)

offline_event offline_reset_event = NO_OFFLINE;
/* ============================================================================
 * PROJECT-SPECIFIC EVENT HANDLER IMPLEMENTATIONS
 * ============================================================================ */

uint32_t protect_trigger_time = 0;
void system_protect_offline_first(void)
{
    LOG_E("System protect triggered by offline event[%d]\r\n", offline_reset_event);
    protect_trigger_time = osKernelGetTickCount();
}

void system_protect_offline(void)
{
    if(protect_trigger_time == 0){
        protect_trigger_time = osKernelGetTickCount();
    }
    if (osKernelGetTickCount() - protect_trigger_time > pdMS_TO_TICKS(10 * 60 * 1000)){
        __set_FAULTMASK(1);
        NVIC_SystemReset();
    }
}

void system_protect_online_first(void)
{
    LOG_I("system protect online, reset trigger time\r\n");
    protect_trigger_time = 0;
}

void read_load_weight_offline_first(void)
{
    mb_reg_write_bit(REG_DATA_VAILD, REG_DATA_VAILD_BIT_LOAD_WEIGHT, false);
    mb_reg_write_float(REG_LOAD_WEIGHT, 0.0f);
    mb_reg_write_float(REG_LOAD_WEIGHT_PCT, 0.0f);
    mb_reg_write_float(REG_LIFTING_MOMENT, 0.0f);
    mb_reg_write_float(REG_LIFTING_MOMENT_PCT, 0.0f);
    mb_reg_write_float(REG_HOOK_LOWERING_HEIGHT, 0.0f);
}
/* ============================================================================
 * PROJECT-SPECIFIC OFFLINE EVENT TABLE
 * ============================================================================ */

struct offline_manage_obj offline_event_table[] =
{
    /* use designated initializers to avoid field-order bugs */
    {
        .event = SYSTEM_PROTECT,
        .enable = ENABLE,
        .error_level = OFFLINE_ERROR_LEVEL,
        .group_id = 0,
        .offline_time = 0,
        .offline_first_func = system_protect_offline_first,
        .offline_func = system_protect_offline,
        .online_first_func = system_protect_online_first,
        .online_func = NULL
    },
    {
        .event = OFFLINE_ETH_MANAGE,
        .enable = ENABLE,
        .error_level = OFFLINE_ERROR_LEVEL,
        .group_id = 3,
        .offline_time = 3000,
        .offline_first_func = NULL,
        .offline_func = NULL,
        .online_first_func = NULL,
        .online_func = NULL
    },
    {
        .event = OFFLINE_READ_SWING_ENCODER,
        .enable = ENABLE,
        .error_level = OFFLINE_ERROR_LEVEL,
        .group_id = 2,
        .offline_time = 3000,
        .offline_first_func = NULL,
        .offline_func = NULL,
        .online_first_func = NULL,
        .online_func = NULL
    },
    {
        .event = OFFLINE_READ_LUFFING_ENCODER,
        .enable = ENABLE,
        .error_level = OFFLINE_ERROR_LEVEL,
        .group_id = 2,
        .offline_time = 3000,
        .offline_first_func = NULL,
        .offline_func = NULL,
        .online_first_func = NULL,
        .online_func = NULL
    },
    {
        .event = OFFLINE_READ_LOAD_WEIGHT,
        .enable = DISABLE,
        .error_level = OFFLINE_WARNING_LEVEL,
        .group_id = 1,
        .offline_time = 3000,
        .offline_first_func = read_load_weight_offline_first,
        .offline_func = NULL,
        .online_first_func = NULL,
        .online_func = NULL
    },
    {
        .event = OFFLINE_PUBLISH_MQTT,
        .enable = ENABLE,
        .error_level = OFFLINE_ERROR_LEVEL,
        .group_id = 3,
        .offline_time = 3000,
        .offline_first_func = NULL,
        .offline_func = NULL,
        .online_first_func = NULL,
        .online_func = NULL
    },
};

/* Table size for runtime use (instead of sizeof calculations) */
int offline_event_table_size = sizeof(offline_event_table) / sizeof(struct offline_manage_obj);
