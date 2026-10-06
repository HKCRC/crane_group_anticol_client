#include "offline_manage_task.h"

#include "board_manage.h"
#include "offline_event_table.h"

#define LOG_D(...) // printf(__VA_ARGS__)
#define LOG_I(...) printf(__VA_ARGS__)
#define LOG_E(...) printf(__VA_ARGS__)

#define STATE_ONLINE  0
#define STATE_OFFLINE 1

#define OFFLINE_LED_CYCLE_MS     3000U
#define OFFLINE_LED_BLINK_MS     1000U
#define NORMAL_LED_CYCLE_MS      2000U
#define NORMAL_LED_ON_MS         1000U
#define OFFLINE_TASK_PERIOD_MS    100U
#define OFFLINE_MANAGE_PERIOD_MS 1000U
#define OFFLINE_AUTO_REBOOT_MS   (12UL * 60UL * 60UL * 1000UL)

extern struct offline_manage_obj offline_event_table[];
extern int offline_event_table_size;
extern offline_event offline_reset_event;

struct offline_manage_obj offline_manage[OFFLINE_EVENT_MAX_NUM];

static char offline_event_msg[256] = {0};
static void build_offline_event_msg(void)
{
    int offset = snprintf(offline_event_msg, sizeof(offline_event_msg), "offline_events:");
    if ((offset < 0) || (offset >= (int)sizeof(offline_event_msg)))
    {
        offline_event_msg[sizeof(offline_event_msg) - 1] = '\0';
        return;
    }

    int offline_count = 0;
    for (int i = 1; i < OFFLINE_EVENT_MAX_NUM; i++)
    {
        if (offline_manage[i].online_state == STATE_OFFLINE)
        {
            int written = snprintf(
                offline_event_msg + offset,
                sizeof(offline_event_msg) - (size_t)offset,
                (offline_count == 0) ? "%d" : ",%d",
                i);
            if (written < 0)
            {
                break;
            }
            if (written >= (int)(sizeof(offline_event_msg) - (size_t)offset))
            {
                offline_event_msg[sizeof(offline_event_msg) - 1] = '\0';
                break;
            }
            offset += written;
            offline_count++;
        }
    }

    if (offline_count > 0)
    {
       LOG_E("%s\r\n", offline_event_msg);
    }else{
       offline_event_msg[0] = '\0';
    }
    
}

void offline_manage_enable_event(offline_event event)
{
    if ((event >= 0) && (event < OFFLINE_EVENT_MAX_NUM))
    {
         offline_manage[event].enable = 1;
    }
}

void offline_manage_disable_event(offline_event event)
{
    if ((event >= 0) && (event < OFFLINE_EVENT_MAX_NUM))
    {
         offline_manage[event].enable = 0;
    }
}

void offline_manage_disable_all_event(void)
{
    for (int i = 0; i < OFFLINE_EVENT_MAX_NUM; i++)
    {
         offline_manage[i].enable = 0;
    }
}

uint8_t offline_manage_get_system_protect(void)
{
   return offline_manage[SYSTEM_PROTECT].online_state;
}

void offline_manage_update_event(offline_event event)
{
    offline_manage[event].last_time = GET_TICK_TIME();
}

void offline_event_callback(struct offline_manage_obj *obj)
{
    if (obj->online_state == STATE_OFFLINE)
    {
        if (obj->last_state == STATE_ONLINE)
        {
            if (obj->offline_first_func != NULL)
            {
                obj->offline_first_func();
            }
        }
        else
        {
            if (obj->offline_func != NULL)
            {
                obj->offline_func();
            }
        }
    } // obj->online_state == STATE_OFFLINE
    else
    {
        obj->online_state = STATE_ONLINE;
        if (obj->last_state == STATE_OFFLINE)
        {
            if (obj->online_first_func != NULL)
            {
                obj->online_first_func();
            }
        }
        else
        {
            if (obj->online_func != NULL)
            {
                obj->online_func();
            }
        }
    } // obj->online_state != STATE_OFFLINE
    obj->last_state = obj->online_state;
}

void setup_offline_event(struct offline_manage_obj obj)
{
   offline_event event = obj.event;
    if ((event < 0) || (event >= OFFLINE_EVENT_MAX_NUM))
    {
         return;
    }

    offline_manage[event].event = event;
    offline_manage[event].enable = obj.enable;
    offline_manage[event].error_level = obj.error_level;
    offline_manage[event].online_first_func = obj.online_first_func;
    offline_manage[event].offline_first_func = obj.offline_first_func;
    offline_manage[event].online_func = obj.online_func;
    offline_manage[event].offline_func = obj.offline_func;
    offline_manage[event].group_id = obj.group_id;
    offline_manage[event].offline_time = obj.offline_time;
}

void setup_offline_manage(){

    struct offline_manage_obj offline_obj;
    int offline_tab_size = offline_event_table_size;
    /* ensure offline_manage starts in a known state */
    memset(offline_manage, 0, sizeof(offline_manage));
    if (offline_tab_size > 0)
    {
        for (int i = 0; i < offline_tab_size; i++)
        {
            offline_obj.event = offline_event_table[i].event;
            offline_obj.enable = offline_event_table[i].enable;
            offline_obj.error_level = offline_event_table[i].error_level;
  
            offline_obj.online_first_func = offline_event_table[i].online_first_func;
            offline_obj.offline_first_func = offline_event_table[i].offline_first_func;
            offline_obj.online_func = offline_event_table[i].online_func;
            offline_obj.offline_func = offline_event_table[i].offline_func;
  
            offline_obj.group_id = offline_event_table[i].group_id;
            offline_obj.offline_time = offline_event_table[i].offline_time;
            setup_offline_event(offline_obj);
        }
    }
    
    /* initialize timestamps and states to avoid false immediate offline at startup */
    uint32_t now = GET_TICK_TIME();
    for (int i = 0; i < OFFLINE_EVENT_MAX_NUM; i++)
    {
        /* ensure every event has a sane last_time */
        offline_manage[i].last_time = now;
        /* assume online at startup; tasks will update via offline_manage_update_event when active */
        offline_manage[i].last_state = STATE_ONLINE;
        offline_manage[i].online_state = STATE_ONLINE;
    }
    LOG_I("offline event number: %d\r\n", offline_tab_size);
 }

void update_offline_manage()
{
    offline_event display_event = NO_OFFLINE;
    uint8_t error_level = 0XFF;
    uint32_t now = GET_TICK_TIME();
    for (int i = 1; i < OFFLINE_EVENT_MAX_NUM; i++)
    {
        if ((now - offline_manage[i].last_time > offline_manage[i].offline_time) && (offline_manage[i].enable))
        {
            offline_manage[i].online_state = STATE_OFFLINE;
            if (error_level > offline_manage[i].error_level)
            {
                error_level = offline_manage[i].error_level;
            }

            if(offline_manage[i].error_level == OFFLINE_ERROR_LEVEL){
                offline_reset_event = (offline_event)i;
            } 
 
            if (offline_manage[i].group_id > offline_manage[display_event].group_id)
            {
                display_event = (offline_event)i;
            }
        }
        else
        {
            offline_manage[i].online_state = STATE_ONLINE;
        }
    }

    // deal error level
    if ((error_level == OFFLINE_ERROR_LEVEL) && (offline_manage[SYSTEM_PROTECT].enable))
    {
        /* trigger system protect callback (handler will perform logging and reboot) */
        offline_manage[SYSTEM_PROTECT].online_state = STATE_OFFLINE;
        offline_event_callback(&offline_manage[SYSTEM_PROTECT]);
    }
    else
    {
        offline_manage[SYSTEM_PROTECT].online_state = STATE_ONLINE;
        offline_event_callback(&offline_manage[SYSTEM_PROTECT]);
        for (int i = 1; i < OFFLINE_EVENT_MAX_NUM; i++)
        {
            offline_event_callback(&offline_manage[i]);
        }
    }
    
    if (display_event != NO_OFFLINE)
    {
        build_offline_event_msg();
        uint32_t phase = now % OFFLINE_LED_CYCLE_MS;
        uint32_t blink_count = offline_manage[display_event].group_id;
        GPIO_PinState led_state = STATUS_LED_OFF;
        if ((phase < OFFLINE_LED_BLINK_MS) && (blink_count > 0U))
        {
            uint32_t half_period = OFFLINE_LED_BLINK_MS / (2U * blink_count);
            if (half_period == 0U)
            {
                half_period = 1U;
            }
            uint32_t slot = phase / half_period;
            led_state = ((slot % 2U) == 0U) ? STATUS_LED_ON : STATUS_LED_OFF;
        }
        SET_STATUS_LED(led_state);
    }
    else
    {
        uint32_t phase = now % NORMAL_LED_CYCLE_MS;
        GPIO_PinState led_state =
            (phase < NORMAL_LED_ON_MS) ? STATUS_LED_ON : STATUS_LED_OFF;
        SET_STATUS_LED(led_state);
    }

} 

void offline_manage_task(void *argument)
{
    setup_offline_manage();
    const TickType_t xPeriod = pdMS_TO_TICKS(OFFLINE_TASK_PERIOD_MS);
    TickType_t xLastWakeTime = xTaskGetTickCount();
    uint32_t boot_tick = GET_TICK_TIME();
    for (;;)
    {   
        vTaskDelayUntil(&xLastWakeTime, xPeriod);

        uint32_t now = GET_TICK_TIME();
        if ((uint32_t)(now - boot_tick) >= OFFLINE_AUTO_REBOOT_MS)
        {
            LOG_I("system uptime reached 12 hours, rebooting MCU\r\n");
            vTaskDelay(pdMS_TO_TICKS(3000));
            board_system_reset_force();
        }
        update_offline_manage();
    }
}

