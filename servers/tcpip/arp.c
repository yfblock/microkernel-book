#include "arp.h"
#include "device.h"
#include <libs/common/endian.h>
#include <libs/common/list.h>
#include <libs/common/print.h>
#include <libs/common/string.h>
#include <libs/user/malloc.h>
#include <libs/user/syscall.h>

//ARP表：IPv4地址与MAC地址对应表（缓存）
static struct arp_table table;

//保留一个 Arp 表条目。
static struct arp_entry *alloc_entry(void) {
    struct arp_entry *e = NULL;
    struct arp_entry *oldest = NULL;
    int oldest_time = INT_MAX;
    for (int i = 0; i < ARP_ENTRIES_MAX; i++) {
        if (table.entries[i].time_accessed < oldest_time) {
            oldest = &table.entries[i];
            oldest_time = oldest->time_accessed;
        }

        if (!table.entries[i].in_use) {
            e = &table.entries[i];
            break;
        }
    }

    if (!e) {
        //由于 Arp 表已满，请删除最近未使用的条目。
        DEBUG_ASSERT(oldest);
        e = oldest;
    }

    e->in_use = true;
    list_init(&e->queue);
    return e;
}

//在arp表中找到i pv4地址对应的条目。
static struct arp_entry *lookup_entry(ipv4addr_t ipaddr) {
    for (int i = 0; i < ARP_ENTRIES_MAX; i++) {
        struct arp_entry *e = &table.entries[i];
        if (e->in_use && e->ipaddr == ipaddr) {
            return e;
        }
    }

    return NULL;
}

//发送 Arp 数据包。
static void arp_transmit(enum arp_opcode op, ipv4addr_t target_addr,
                         macaddr_t target) {
    struct arp_packet p;
    p.hw_type = hton16(1);//以太网
    p.proto_type = hton16(0x0800);//IPv4
    p.hw_size = MACADDR_LEN;
    p.proto_size = 4;
    p.opcode = hton16(op);
    p.sender_addr = hton32(device_get_ipaddr());
    p.target_addr = hton32(target_addr);
    memcpy(&p.sender, device_get_macaddr(), MACADDR_LEN);
    memcpy(&p.target, target, MACADDR_LEN);

    ethernet_transmit(ETHER_TYPE_ARP, IPV4_ADDR_BROADCAST,
                      mbuf_new(&p, sizeof(p)));
}

//我从 pv4 地址解析 mac 地址。
bool arp_resolve(ipv4addr_t ipaddr, macaddr_t *macaddr) {
    ASSERT(ipaddr != IPV4_ADDR_UNSPECIFIED);

    if (ipaddr == IPV4_ADDR_BROADCAST) {
        memcpy(macaddr, MACADDR_BROADCAST, MACADDR_LEN);
        return true;
    }

    struct arp_entry *e = lookup_entry(ipaddr);
    if (!e || !e->resolved) {
        return false;
    }

    e->time_accessed = sys_uptime();
    memcpy(macaddr, e->macaddr, MACADDR_LEN);
    return true;
}

//将新报文添加到ARP表项的ARP响应等待报文列表中。
//
//当收到IPv4地址dst对应的ARP响应时，发送添加了该功能的报文。
void arp_enqueue(enum ether_type type, ipv4addr_t dst, mbuf_t payload) {
    struct arp_entry *e = lookup_entry(dst);
    ASSERT(!e || !e->resolved);
    if (!e) {
        //Arp 表中没有条目，因此创建一个新条目。
        e = alloc_entry();
        e->resolved = false;
        e->ipaddr = dst;
        e->time_accessed = sys_uptime();
    }

    //添加到Arp响应等待包列表中。
    struct arp_queue_entry *qe = (struct arp_queue_entry *) malloc(sizeof(*qe));
    qe->dst = dst;
    qe->type = type;
    qe->payload = payload;
    list_elem_init(&qe->next);
    list_push_back(&e->queue, &qe->next);
}

//发送 Arp 请求。
void arp_request(ipv4addr_t addr) {
    arp_transmit(ARP_OP_REQUEST, addr, MACADDR_BROADCAST);
}

//将mac地址注册到Arp表中。收到 arp 响应时调用。
void arp_register_macaddr(ipv4addr_t ipaddr, macaddr_t macaddr) {
    struct arp_entry *e = lookup_entry(ipaddr);
    if (!e) {
        e = alloc_entry();
    }

    e->resolved = true;
    e->ipaddr = ipaddr;
    e->time_accessed = sys_uptime();
    memcpy(e->macaddr, macaddr, MACADDR_LEN);

    //发送在Arp响应等待报文列表中注册的报文。
    LIST_FOR_EACH (qe, &e->queue, struct arp_queue_entry, next) {
        ethernet_transmit(qe->type, qe->dst, qe->payload);
        list_remove(&qe->next);
        free(qe);
    }
}

//Arp包接收处理。
void arp_receive(mbuf_t pkt) {
    struct arp_packet p;
    if (mbuf_read(&pkt, &p, sizeof(p)) != sizeof(p)) {
        return;
    }

    uint16_t opcode = ntoh16(p.opcode);
    ipv4addr_t sender_addr = ntoh32(p.sender_addr);
    ipv4addr_t target_addr = ntoh32(p.target_addr);
    switch (opcode) {
        //ARP请求
        case ARP_OP_REQUEST:
            if (device_get_ipaddr() != target_addr) {
                break;
            }

            arp_transmit(ARP_OP_REPLY, sender_addr, p.sender);
            break;
        //ARP响应
        case ARP_OP_REPLY:
            arp_register_macaddr(sender_addr, p.sender);
            break;
    }

    mbuf_delete(pkt);
}
