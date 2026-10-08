#pragma once

#include "cmsis_os.h"

/*
 * LTE upload protocol version switch (build-time):
 * 0x21 -> legacy protocol (with incrementing packet sequence)
 * 0x30 -> protocol 3.0 (without hook lowering height)
 * 0x31 -> protocol 3.1 (with hook lowering height)
 */
#ifndef CONFIG_UPLOAD_PROTOCOL_VERSION
#define CONFIG_UPLOAD_PROTOCOL_VERSION 0x21U
#endif

void report_atc_data_task(void *argument);
