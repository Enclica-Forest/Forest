#ifndef NETDEV_H
#define NETDEV_H

#include <stdint.h>
#include <stdbool.h>

#define NETDEV_MAX_NAME 32

typedef struct netdev netdev_t;

typedef int (*netdev_init_t)(netdev_t* dev);
typedef int (*netdev_send_t)(netdev_t* dev, uint8_t* data, size_t len);
typedef void (*netdev_irq_t)(netdev_t* dev);
typedef void (*netdev_get_mac_t)(netdev_t* dev, uint8_t* mac);

typedef enum {
    NETDEV_TYPE_E1000,
    NETDEV_TYPE_RTL8139,
    NETDEV_TYPE_NE2000,
} netdev_type_t;

struct netdev {
    char name[NETDEV_MAX_NAME];
    netdev_type_t type;
    uint8_t mac[6];
    uint32_t io_base;
    uint64_t mem_base;
    uint16_t irq;
    
    bool initialized;
    
    netdev_init_t init;
    netdev_send_t send;
    netdev_irq_t irq_handler;
    netdev_get_mac_t get_mac;
    
    void* private_data;
    
    struct netdev* next;
};

int netdev_register(netdev_t* dev);
int netdev_unregister(netdev_t* dev);
netdev_t* netdev_find_by_name(const char* name);
netdev_t* netdev_find_by_type(netdev_type_t type);
netdev_t* netdev_get_active(void);
void netdev_list_all(void);
int netdev_init(void);

#endif
