#ifndef __TOPOLOGY_AGGREGATION_H__
#define __TOPOLOGY_AGGREGATION_H__

#include <stdint.h>
#include <stdbool.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOPO_MAX_NODES 32

void topology_aggregation_init(void);

void topo_table_upsert(const char *node_json);

int topo_table_get_count(void);

char* topo_table_get_json(int index);

cJSON* topo_table_build_full_json(void);

#ifdef __cplusplus
}
#endif

#endif /* __TOPOLOGY_AGGREGATION_H__ */
