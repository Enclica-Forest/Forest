#include "route.h"
#include "../include/string.h"

static net_route_t g_routes[ROUTE_TABLE_SIZE];
static bool g_route_ready = false;

void route_init(void) {
    if (g_route_ready) {
        return;
    }
    memory_set((uint8*)g_routes, 0, sizeof(g_routes));

    /* Default route entries: loopback + default via the active NIC (if any). */
    int idx = 0;
    g_routes[idx].used = true;
    g_routes[idx].dest = INADDR_LOOPBACK; /* 127.0.0.0 */
    g_routes[idx].mask = 0x000000ffu;     /* /8 */
    g_routes[idx].gateway = 0;
    strncpy(g_routes[idx].ifname, "lo", NET_IFNAME_LEN - 1);
    g_routes[idx].metric = 0;
    idx++;
    g_routes[idx].used = true;
    g_routes[idx].dest = 0;               /* default */
    g_routes[idx].mask = 0;               /* /0 */
    g_routes[idx].gateway = 0;
    strncpy(g_routes[idx].ifname, "lo", NET_IFNAME_LEN - 1);
    g_routes[idx].metric = 1;
    g_route_ready = true;
}

int route_add(uint32 dest, uint32 mask, uint32 gateway, const char* ifname, int metric) {
    route_init();
    for (int i = 0; i < ROUTE_TABLE_SIZE; i++) {
        if (g_routes[i].used && g_routes[i].dest == dest && g_routes[i].mask == mask) {
            g_routes[i].gateway = gateway;
            g_routes[i].metric = metric;
            if (ifname) {
                memory_set((uint8*)g_routes[i].ifname, 0, NET_IFNAME_LEN);
                strncpy(g_routes[i].ifname, ifname, NET_IFNAME_LEN - 1);
            }
            return 0;
        }
    }
    for (int i = 0; i < ROUTE_TABLE_SIZE; i++) {
        if (!g_routes[i].used) {
            memory_set((uint8*)&g_routes[i], 0, sizeof(net_route_t));
            g_routes[i].used = true;
            g_routes[i].dest = dest;
            g_routes[i].mask = mask;
            g_routes[i].gateway = gateway;
            g_routes[i].metric = metric;
            if (ifname) {
                strncpy(g_routes[i].ifname, ifname, NET_IFNAME_LEN - 1);
            }
            return 0;
        }
    }
    return -1;
}

int route_del(uint32 dest, uint32 mask) {
    route_init();
    for (int i = 0; i < ROUTE_TABLE_SIZE; i++) {
        if (g_routes[i].used && g_routes[i].dest == dest && g_routes[i].mask == mask) {
            memory_set((uint8*)&g_routes[i], 0, sizeof(net_route_t));
            return 0;
        }
    }
    return -1;
}

int route_lookup(uint32 dest_ip, uint32* next_hop, char ifname[NET_IFNAME_LEN]) {
    route_init();
    int best = -1;
    uint32 best_mask = 0;
    for (int i = 0; i < ROUTE_TABLE_SIZE; i++) {
        if (!g_routes[i].used) {
            continue;
        }
        if ((dest_ip & g_routes[i].mask) == (g_routes[i].dest & g_routes[i].mask)) {
            if (best < 0 || g_routes[i].mask >= best_mask) {
                best = i;
                best_mask = g_routes[i].mask;
            }
        }
    }
    if (best < 0) {
        return -1;
    }
    if (next_hop) {
        *next_hop = g_routes[best].gateway ? g_routes[best].gateway : dest_ip;
    }
    if (ifname) {
        memory_copy((const char*)g_routes[best].ifname, ifname, NET_IFNAME_LEN);
    }
    return 0;
}

int route_dump(net_route_t* out, int max_entries) {
    route_init();
    int count = 0;
    for (int i = 0; i < ROUTE_TABLE_SIZE && count < max_entries; i++) {
        if (!g_routes[i].used) {
            continue;
        }
        memory_copy((const char*)&g_routes[i], (char*)&out[count], sizeof(net_route_t));
        count++;
    }
    return count;
}

/* net.h API wrappers */
int32 net_route_add(uint32 dest, uint32 mask, uint32 gateway, const char* ifname, int metric) {
    return route_add(dest, mask, gateway, ifname, metric);
}

int32 net_route_del(uint32 dest, uint32 mask) {
    return route_del(dest, mask);
}

int32 net_route_lookup(uint32 dest_ip, uint32* next_hop, char ifname[NET_IFNAME_LEN]) {
    return route_lookup(dest_ip, next_hop, ifname);
}

int32 net_route_table_dump(net_route_t* out, int32 max_entries) {
    return route_dump(out, max_entries);
}