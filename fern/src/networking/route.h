#ifndef ROUTE_H
#define ROUTE_H

#include "../include/net.h"

#define ROUTE_TABLE_SIZE 32

int  route_add(uint32 dest, uint32 mask, uint32 gateway, const char* ifname, int metric);
int  route_del(uint32 dest, uint32 mask);
int  route_lookup(uint32 dest_ip, uint32* next_hop, char ifname[NET_IFNAME_LEN]);
int  route_dump(net_route_t* out, int max_entries);

void route_init(void);

#endif