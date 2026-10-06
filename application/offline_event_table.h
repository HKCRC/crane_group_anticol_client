#ifndef __OFFLINE_EVENT_TABLE_H__
#define __OFFLINE_EVENT_TABLE_H__

#include "offline_manage_task.h"
#include "main.h"

#define GET_TICK_TIME() osKernelGetTickCount()
#define STATUS_LED_ON  GPIO_PIN_SET
#define STATUS_LED_OFF GPIO_PIN_RESET
#define SET_STATUS_LED(STATE)                                                     \
    do                                                                            \
    {                                                                             \
        GPIO_PinState status_led_state = (STATE);                                 \
        HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, status_led_state);             \
        HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, status_led_state);             \
    } while (0)

/* ============================================================================
 * PROJECT-SPECIFIC OFFLINE EVENT DEFINITIONS
 * 
 * This file contains all project-specific offline event configurations.
 * Different projects can define different events, error levels, and handlers
 * while using the same generic offline_manage_task.c implementation.
 * ============================================================================ */

/* Error level definitions - can be customized per project */
#define OFFLINE_ERROR_LEVEL   0   /* Critical - triggers system reset */
#define OFFLINE_WARNING_LEVEL 1   /* Warning - logged but no reset */
#define APP_PROTECT_LEVEL     2   /* Application-specific protection */

/* Beep configuration */
#define BEEP_DISABLE 0xFF

/* System protect event configuration */
#define SYSTEM_PROTECT NO_OFFLINE

/* ============================================================================
 * Event Handler Function Declarations
 * 
 * Each project implements these handlers as needed. Set to NULL if not used.
 * ============================================================================ */

/* System protection handlers */
void system_protect_offline_first(void);
void system_protect_offline(void);

/* Project-specific event handlers - implement as needed */
void bms_offline_first(void);
void gps_offline_first(void);
void heartbeat_offline_first(void);

/* ============================================================================
 * Offline Event Table Definition and Configuration
 * 
 * Configure project-specific offline events, timeouts, and handlers.
 * The table must include SYSTEM_PROTECT as the first entry (index 0).
 * ============================================================================ */

extern struct offline_manage_obj offline_event_table[];
extern int offline_event_table_size;

#endif /* __OFFLINE_EVENT_TABLE_H__ */
