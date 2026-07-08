#ifndef __TIME_SYNC_H__
#define __TIME_SYNC_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void time_sync_init(void);

void time_sync_set_root(bool is_root);

void time_sync_start_beacon(uint32_t interval_ms);

uint64_t time_sync_get_network_time_us(void);

int64_t time_sync_get_offset_us(void);

double time_sync_get_drift_ppm(void);

bool time_sync_is_synchronized(void);

int8_t time_sync_get_last_beacon_rssi(void);

#ifdef __cplusplus
}
#endif

#endif // __TIME_SYNC_H__
