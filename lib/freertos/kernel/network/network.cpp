/* Copyright 2018 Canaan Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "network.h"
#include "FreeRTOS.h"
#include "devices.h"
#include "kernel/driver_impl.hpp"
#include "task.h"
#include <lwip/etharp.h>
#include <lwip/init.h>
#include <lwip/snmp.h>
#include <lwip/tcpip.h>
#include <lwip/dhcp.h>
#include <lwip/netif.h>
#include <lwip/netdb.h>
#include <netif/ethernet.h>
#include <string.h>
using namespace sys;

#define MAX_DHCP_TRIES 5



/* 网络初始化 */
int network_init()
{
    /* 对 tcp ip 进行初始化 */
    tcpip_init(NULL, NULL);

    return 0;
}




/* 设备接口类 */
class k_ethernet_interface : public virtual object_access, public heap_object, public exclusive_object_access, private network_adapter_handler
{
public:
    /* 设备构造函数 */
    k_ethernet_interface(object_accessor<network_adapter_driver> adapter, /* 设备对象 */
    const ip_address_t &ip_address, /* 设备地址 */
    const ip_address_t &net_mask,   /* 子网掩码 */
    const ip_address_t &gateway)    /* 网关地址 */
        : adapter_(std::move(adapter))
    {
        /* ipv4 设备地址、子网掩码、网关 */
        ip4_addr_t ipaddr, netmask, gw;

        completion_event_ = xSemaphoreCreateCounting(20, 0);

        /* 将入口参数地址格式转换为 ipv4 地址 */
        IP4_ADDR(&ipaddr, ip_address.data[0], ip_address.data[1], ip_address.data[2], ip_address.data[3]);
        IP4_ADDR(&netmask, net_mask.data[0], net_mask.data[1], net_mask.data[2], net_mask.data[3]);
        IP4_ADDR(&gw, gateway.data[0], gateway.data[1], gateway.data[2], gateway.data[3]);

        /* 注册此对象 */
        if (!netif_add(&netif_, &ipaddr, &netmask, &gw, this, ethernetif_init, ethernet_input))
            throw std::runtime_error("Unable to init netif.");
    }

    /* 启用和关闭网卡设备 */
    void set_enable(bool enable)
    {
        if (enable)
        {
            /* 启用网卡 */
            netif_set_up(&netif_);

            TaskHandle_t h;

            /*  创建 poll 进程 ??? */
            auto ret = xTaskCreate(poll_thread, "poll", 10240, this, 3, &h);

            configASSERT(ret == pdTRUE);
        }
        else
        {
            /* 停用网卡 */
            netif_set_down(&netif_);
        }
    }

    /* 设为默认网卡 */
    void set_as_default()
    {
        netif_set_default(&netif_);
    }

    /* dhcp 地址池 */
    dhcp_state_t dhcp_pooling()
    {
        auto &netif = netif_;
        uint32_t ip_address;
        dhcp_state_t dhcp_state;
        dhcp_state = DHCP_START;

        for (;;)
        {
            /* dhcp 对状态 */
            switch (dhcp_state)
            {
                /* 开始申请地址 */
                case DHCP_START:
                {
                    dhcp_start(&netif);
                    ip_address = 0;
                    dhcp_state = DHCP_WAIT_ADDRESS;
                }
                break;

                /* 等待返回地址 */
                case DHCP_WAIT_ADDRESS:
                {
                    ip_address = netif.ip_addr.addr;

                    if (ip_address != 0)
                    {
                        /* 成功获取到地址 */
                        dhcp_state = DHCP_ADDRESS_ASSIGNED;

                        dhcp_stop(&netif);
                        dhcp_cleanup(&netif);
                        return dhcp_state;
                    }
                    else
                    {
                        struct dhcp *dhcp = netif_dhcp_data(&netif);

                        /* dhcp 尝试次数高过最大值 */
                        if (dhcp->tries > MAX_DHCP_TRIES)
                        {
                            dhcp_state = DHCP_TIMEOUT;
                            dhcp_stop(&netif);
                            dhcp_cleanup(&netif);
                            return dhcp_state;
                        }
                    }
                }
                break;

            default:
                return dhcp_state;
            }

            /* 延迟后再次检查状态 */
            vTaskDelay(250);
        }
        return DHCP_FAIL;
    }


    /* 设置 ip 地址 */
    void set_addr(const ip_address_t &ip_address, const ip_address_t &net_mask, const ip_address_t &gate_way)
    {
        ip4_addr_t ipaddr, netmask, gw;
        IP4_ADDR(&ipaddr, ip_address.data[0], ip_address.data[1], ip_address.data[2], ip_address.data[3]);
        IP4_ADDR(&netmask, net_mask.data[0], net_mask.data[1], net_mask.data[2], net_mask.data[3]);
        IP4_ADDR(&gw, gate_way.data[0], gate_way.data[1], gate_way.data[2], gate_way.data[3]);

        netif_set_addr(&netif_, &ipaddr, &netmask, &gw);
    }


    /* 获取当前 ip 地址 */
    void get_addr(ip_address_t &ip_address, ip_address_t &net_mask, ip_address_t &gate_way)
    {
        ip_address.data[0] = ip4_addr1(&netif_.ip_addr);
        ip_address.data[1] = ip4_addr2(&netif_.ip_addr);
        ip_address.data[2] = ip4_addr3(&netif_.ip_addr);
        ip_address.data[3] = ip4_addr4(&netif_.ip_addr);

        net_mask.data[0] = ip4_addr1(&netif_.gw);
        net_mask.data[1] = ip4_addr2(&netif_.gw);
        net_mask.data[2] = ip4_addr3(&netif_.gw);
        net_mask.data[3] = ip4_addr4(&netif_.gw);

        gate_way.data[0] = ip4_addr1(&netif_.netmask);
        gate_way.data[1] = ip4_addr2(&netif_.netmask);
        gate_way.data[2] = ip4_addr3(&netif_.netmask);
        gate_way.data[3] = ip4_addr4(&netif_.netmask);
    }

private:
    /* 有数据包 */
    virtual void notify_input() override
    {
        while (adapter_->is_packet_available())
        {
            ethernetif_input(&netif_);
        }
    }


    static void poll_thread(void *args)
    {
        auto &ethnetif = *reinterpret_cast<k_ethernet_interface *>(args);
        auto &adapter = ethnetif.adapter_;
        while (1)
        {
            /* 收到包时, 发出通知 */
            if (xSemaphoreTake(ethnetif.completion_event_, portMAX_DELAY) == pdTRUE)
            {
                if (adapter->interface_check())
                {
                    adapter->disable_rx();

                    ethnetif.notify_input();

                    adapter->enable_rx();
                }
            }
        }
    }

    /* 以太网接口初始化 */
    static err_t ethernetif_init(struct netif *netif)
    {
#if LWIP_NETIF_HOSTNAME
        /* Initialize interface hostname */
        netif->hostname = "lwip";
#endif /* LWIP_NETIF_HOSTNAME */

#if LWIP_IPV4
        netif->output = etharp_output;
#endif /* LWIP_IPV4 */
#if LWIP_IPV6
        netif->output_ip6 = ethip6_output;
#endif /* LWIP_IPV6 */
        netif->linkoutput = low_level_output;

        /* initialize the hardware */
        low_level_init(netif);

        return ERR_OK;
    }

    static void ethernetif_input(struct netif *netif)
    {
        struct pbuf *p;

        /* move received packet into a new pbuf */
        p = low_level_input(netif);
        /* if no packet could be read, silently ignore this */
        if (p != NULL)
        {
            /* pass all packets to ethernet_input, which decides what packets it supports */
            if (netif->input(p, netif) != ERR_OK)
            {
                LWIP_DEBUGF(NETIF_DEBUG, ("ethernetif_input: IP input error\n"));
                pbuf_free(p);
                p = NULL;
            }
        }
    }

    static void low_level_init(struct netif *netif)
    {
        auto &ethnetif = *reinterpret_cast<k_ethernet_interface *>(netif->state);
        auto &adapter = ethnetif.adapter_;

        auto mac_address = adapter->get_mac_address();
        /* set MAC hardware address length */
        netif->hwaddr_len = ETHARP_HWADDR_LEN;

        /* set MAC hardware address */
        netif->hwaddr[0] = mac_address.data[0];
        netif->hwaddr[1] = mac_address.data[1];
        netif->hwaddr[2] = mac_address.data[2];
        netif->hwaddr[3] = mac_address.data[3];
        netif->hwaddr[4] = mac_address.data[4];
        netif->hwaddr[5] = mac_address.data[5];

        /* maximum transfer unit */
        netif->mtu = 1500;

        /* device capabilities */
        /* don't set NETIF_FLAG_ETHARP if this device is not an ethernet one */
        netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;

#if LWIP_IPV6 && LWIP_IPV6_MLD
        /*
	* For hardware/netifs that implement MAC filtering.
	* All-nodes link-local is handled by default, so we must let the hardware know
	* to allow multicast packets in.
	* Should set mld_mac_filter previously. */
        if (netif->mld_mac_filter != NULL)
        {
            ip6_addr_t ip6_allnodes_ll;
            ip6_addr_set_allnodes_linklocal(&ip6_allnodes_ll);
            netif->mld_mac_filter(netif, &ip6_allnodes_ll, NETIF_ADD_MAC_FILTER);
        }
#endif /* LWIP_IPV6 && LWIP_IPV6_MLD */

        /* Do whatever else is needed to initialize interface. */
        adapter->reset(ethnetif.completion_event_);
    }

    static struct pbuf *low_level_input(struct netif *netif)
    {
        auto &ethnetif = *reinterpret_cast<k_ethernet_interface *>(netif->state);
        auto &adapter = ethnetif.adapter_;
        struct pbuf *p, *q;
        u16_t len;

        /* Obtain the size of the packet and put it into the "len" variable. */
        len = adapter->begin_receive();

#if ETH_PAD_SIZE
        len += ETH_PAD_SIZE; /* allow room for Ethernet padding */
#endif

        /* We allocate a pbuf chain of pbufs from the pool. */
        p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);

        if (p != NULL)
        {

#if ETH_PAD_SIZE
            pbuf_remove_header(p, ETH_PAD_SIZE); /* drop the padding word */
#endif

            /* We iterate over the pbuf chain until we have read the entire
											 * packet into the pbuf. */
            for (q = p; q != NULL; q = q->next)
            {
                /* Read enough bytes to fill this pbuf in the chain. The
			     * available data in the pbuf is given by the q->len
			     * variable.
			     * This does not necessarily have to be a memcpy, you can also preallocate
			     * pbufs for a DMA-enabled MAC and after receiving truncate it to the
			     * actually received size. In this case, ensure the tot_len member of the
			     * pbuf is the sum of the chained pbuf len members.
			     */
                adapter->receive({ (uint8_t *)q->payload, q->len });
            }

            adapter->end_receive();

            MIB2_STATS_NETIF_ADD(netif, ifinoctets, p->tot_len);
            if (((u8_t *)p->payload)[0] & 1)
            {
                /* broadcast or multicast packet*/
                MIB2_STATS_NETIF_INC(netif, ifinnucastpkts);
            }
            else
            {
                /* unicast packet*/
                MIB2_STATS_NETIF_INC(netif, ifinucastpkts);
            }
#if ETH_PAD_SIZE
            pbuf_add_header(p, ETH_PAD_SIZE); /* reclaim the padding word */
#endif

            LINK_STATS_INC(link.recv);
        }
        else
        {
            adapter->end_receive();

            LINK_STATS_INC(link.memerr);
            LINK_STATS_INC(link.drop);
            MIB2_STATS_NETIF_INC(netif, ifindiscards);
        }

        return p;
    }

    static err_t low_level_output(struct netif *netif, struct pbuf *p)
    {
        auto &ethnetif = *reinterpret_cast<k_ethernet_interface *>(netif->state);
        auto &adapter = ethnetif.adapter_;
        struct pbuf *q;

        adapter->begin_send(p->tot_len);

#if ETH_PAD_SIZE
        pbuf_remove_header(p, ETH_PAD_SIZE); /* drop the padding word */
#endif

        for (q = p; q != NULL; q = q->next)
        {
            /* Send the data from the pbuf to the interface, one pbuf at a
		       time. The size of the data in each pbuf is kept in the ->len
		       variable. */
            adapter->send({ (uint8_t *)q->payload, q->len });
        }

        adapter->end_send();

        MIB2_STATS_NETIF_ADD(netif, ifoutoctets, p->tot_len);
        if (((u8_t *)p->payload)[0] & 1)
        {
            /* broadcast or multicast packet*/
            MIB2_STATS_NETIF_INC(netif, ifoutnucastpkts);
        }
        else
        {
            /* unicast packet */
            MIB2_STATS_NETIF_INC(netif, ifoutucastpkts);
        }
        /* increase ifoutdiscards or ifouterrors on error */

#if ETH_PAD_SIZE
        pbuf_add_header(p, ETH_PAD_SIZE); /* reclaim the padding word */
#endif

        LINK_STATS_INC(link.xmit);

        return ERR_OK;
    }

private:
    object_accessor<network_adapter_driver> adapter_;
    netif netif_;
    SemaphoreHandle_t completion_event_;
};



#define NETIF_ENTRY                                    \
    auto &obj = system_handle_to_object(netif_handle); \
    configASSERT(obj.is<k_ethernet_interface>());      \
    auto f = obj.as<k_ethernet_interface>();

#define CATCH_ALL \
    catch (...) { return -1; }

handle_t network_interface_add(handle_t adapter_handle, const ip_address_t *ip_address, const ip_address_t *net_mask, const ip_address_t *gateway)
{
    try
    {
        if (!ip_address || !net_mask || !gateway)
            return -1;

        auto netif = make_object<k_ethernet_interface>(system_handle_to_object(adapter_handle).move_as<network_adapter_driver>(), *ip_address, *net_mask, *gateway);
        netif->add_ref(); // Pin the object
        return system_alloc_handle(make_accessor<object_access>(netif));
    }
    catch (...)
    {
        return NULL_HANDLE;
    }
}

int network_interface_set_enable(handle_t netif_handle, bool enable)
{
    try
    {
        NETIF_ENTRY;

        f->set_enable(enable);
        return 0;
    }
    CATCH_ALL;
}

int network_interface_set_as_default(handle_t netif_handle)
{
    try
    {
        NETIF_ENTRY;

        f->set_as_default();
        return 0;
    }
    CATCH_ALL;
}

int network_set_addr(handle_t netif_handle, const ip_address_t *ip_address, const ip_address_t *net_mask, const ip_address_t *gateway)
{
    try
    {
        NETIF_ENTRY;

        f->set_addr(*ip_address, *net_mask, *gateway);
        return 0;
    }
    CATCH_ALL;
}

int network_get_addr(handle_t netif_handle, ip_address_t *ip_address, ip_address_t *net_mask, ip_address_t *gateway)
{
    try
    {
        NETIF_ENTRY;

        f->get_addr(*ip_address, *net_mask, *gateway);
        return 0;
    }
    CATCH_ALL;
}

dhcp_state_t network_interface_dhcp_pooling(handle_t netif_handle)
{
    try
    {
        NETIF_ENTRY;

        return f->dhcp_pooling();
    }
    catch (...)
    {
        return DHCP_FAIL;
    }
}

int network_socket_gethostbyname(const char *name, hostent_t *hostent)
{
    try
    {
        struct hostent *lwip_hostent = lwip_gethostbyname(name);
        hostent->h_name = lwip_hostent->h_name;
        hostent->h_aliases = lwip_hostent->h_aliases;
        hostent->h_length = lwip_hostent->h_length;
        hostent->h_addr_list = reinterpret_cast<uint8_t **>(lwip_hostent->h_addr_list);
        switch (lwip_hostent->h_addrtype)
        {
        case AF_INET:
            hostent->h_addrtype = AF_INTERNETWORK;
            break;
        default:
            throw std::invalid_argument("Invalid address type.");
        }
        return 0;
    }
    CATCH_ALL;
}