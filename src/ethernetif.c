#include "lwip/def.h"
#include "lwip/etharp.h"
#include "lwip/ethip6.h"
#include "lwip/mem.h"
#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "lwip/snmp.h"
#include "lwip/stats.h"
#include "netif/ppp/pppoe.h"

#include <pico/enc28j60/enc28j60.h>

#define IFNAME0 'e'
#define IFNAME1 'n'

static void ethernetif_input(struct netif *netif);

/**
 * In this function, the hardware should be initialized.
 * Called from ethernetif_init().
 *
 * @param netif the already initialized lwip network interface structure
 *			for this ethernetif
 */
static int
low_level_init(struct netif *netif)
{
	const struct enc28j60 *eth = netif->state;

	/* set MAC hardware address length */
	netif->hwaddr_len = ETHARP_HWADDR_LEN;

	/* set MAC hardware address */
	netif->hwaddr[0] = eth->mac_address[0];
	netif->hwaddr[1] = eth->mac_address[1];
	netif->hwaddr[2] = eth->mac_address[2];
	netif->hwaddr[3] = eth->mac_address[3];
	netif->hwaddr[4] = eth->mac_address[4];
	netif->hwaddr[5] = eth->mac_address[5];

	/* maximum transfer unit */
	netif->mtu = 1500;

	/* device capabilities */
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;

	#if LWIP_IPV6 && LWIP_IPV6_MLD
	/*
	 * For hardware/netifs that implement MAC filtering.
	 * All-nodes link-local is handled by default, so we must let the hardware know
	 * to allow multicast packets in.
	 * Should set mld_mac_filter previously. */
	if (netif->mld_mac_filter != NULL) {
		ip6_addr_t ip6_allnodes_ll;
		ip6_addr_set_allnodes_linklocal(&ip6_allnodes_ll);
		netif->mld_mac_filter(netif, &ip6_allnodes_ll, NETIF_ADD_MAC_FILTER);
	}
	#endif /* LWIP_IPV6 && LWIP_IPV6_MLD */

	/* Do whatever else is needed to initialize interface. */
	return enc28j60_init(eth);
}

/**
 * This function should do the actual transmission of the packet. The packet is
 * contained in the pbuf that is passed to the function. This pbuf
 * might be chained.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @param p the MAC packet to send (e.g. IP packet including MAC addresses and type)
 * @return ERR_OK if the packet could be sent
 *			 an err_t value if the packet couldn't be sent
 *
 * @note Returning ERR_MEM here if a DMA queue of your MAC is full can lead to
 *		 strange results. You might consider waiting for space in the DMA queue
 *		 to become available since the stack doesn't retry to send a packet
 *		 dropped because of memory failure (except for the TCP timers).
 */

static err_t
low_level_output(struct netif *netif, struct pbuf *p)
{
	const struct enc28j60 *eth = netif->state;
	struct pbuf *q;

	/* Initiate transfer */
	enc28j60_transfer_init(eth);

	#if ETH_PAD_SIZE
	pbuf_remove_header(p, ETH_PAD_SIZE); /* drop the padding word */
	#endif

	for (q = p; q != NULL; q = q->next) {
	/* Send the data from the pbuf to the interface, one pbuf at a
		time. The size of the data in each pbuf is kept in the ->len
		variable. */
		enc28j60_transfer_write(eth, q->payload, q->len);
	}

	/* signal that packet should be sent */
	enc28j60_transfer_send(eth);

	MIB2_STATS_NETIF_ADD(netif, ifoutoctets, p->tot_len);
	if (((u8_t *)p->payload)[0] & 1) {
		/* broadcast or multicast packet*/
		MIB2_STATS_NETIF_INC(netif, ifoutnucastpkts);
	} else {
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

/**
 * Should allocate a pbuf and transfer the bytes of the incoming
 * packet from the interface into the pbuf.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return a pbuf filled with the received packet (including MAC header)
 *			 NULL on memory error
 */
struct pbuf *
low_level_input(const struct netif *netif)
{
	struct enc28j60 *eth = netif->state;
	struct pbuf *p, *q;
	u16_t len;

	/* Obtain the size of the packet and put it into the "len"
	 variable. */
	len = enc28j60_receive_init(eth);

#if ETH_PAD_SIZE
	len += ETH_PAD_SIZE; /* allow room for Ethernet padding */
#endif

	/* We allocate a pbuf chain of pbufs from the pool. */
	p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);

	if (p != NULL) {

		#if ETH_PAD_SIZE
		pbuf_remove_header(p, ETH_PAD_SIZE); /* drop the padding word */
		#endif

		/* We iterate over the pbuf chain until we have read the entire
		 * packet into the pbuf. */
		for (q = p; q != NULL; q = q->next) {
			/* Read enough bytes to fill this pbuf in the chain. The
			 * available data in the pbuf is given by the q->len
			 * variable.
			 * This does not necessarily have to be a memcpy, you can also preallocate
			 * pbufs for a DMA-enabled MAC and after receiving truncate it to the
			 * actually received size. In this case, ensure the tot_len member of the
			 * pbuf is the sum of the chained pbuf len members.
			 */
			enc28j60_receive_read(eth, q->payload, q->len);
		}

		/* acknowledge that packet has been read */
		enc28j60_receive_ack(eth);

		MIB2_STATS_NETIF_ADD(netif, ifinoctets, p->tot_len);
		if (((u8_t *)p->payload)[0] & 1) {
			/* broadcast or multicast packet*/
			MIB2_STATS_NETIF_INC(netif, ifinnucastpkts);
		} else {
			/* unicast packet*/
			MIB2_STATS_NETIF_INC(netif, ifinucastpkts);
		}
		#if ETH_PAD_SIZE
		pbuf_add_header(p, ETH_PAD_SIZE); /* reclaim the padding word */
		#endif

		LINK_STATS_INC(link.recv);
	} else {
		enc28j60_receive_ack(eth);
		LINK_STATS_INC(link.memerr);
		LINK_STATS_INC(link.drop);
		MIB2_STATS_NETIF_INC(netif, ifindiscards);
	}

	return p;
}

/**
 * This function should be called when a packet is ready to be read
 * from the interface. It uses the function low_level_input() that
 * should handle the actual reception of bytes from the network
 * interface. Then the type of the received packet is determined and
 * the appropriate input function is called.
 *
 * @param netif the lwip network interface structure for this ethernetif
 */
static void
ethernetif_input(struct netif *netif)
{
	struct pbuf *p;

	/* move received packet into a new pbuf */
	p = low_level_input(netif);
	/* if no packet could be read, silently ignore this */
	if (p != NULL) {
		/* pass all packets to ethernet_input, which decides what packets it supports */
		if (netif->input(p, netif) != ERR_OK) {
			LWIP_DEBUGF(NETIF_DEBUG, ("ethernetif_input: IP input error\n"));
			pbuf_free(p);
			p = NULL;
		}
	}
}

/**
 * Should be called at the beginning of the program to set up the
 * network interface. It calls the function low_level_init() to do the
 * actual setup of the hardware.
 *
 * This function should be passed as a parameter to netif_add().
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return ERR_OK if the loopif is initialized
 *			 ERR_MEM if private data couldn't be allocated
 *			 any other err_t on error
 */
err_t
ethernetif_init(struct netif *netif)
{
	LWIP_ASSERT("netif != NULL", (netif != NULL));

	#if LWIP_NETIF_HOSTNAME
	/* Initialize interface hostname */
	netif->hostname = "lwip";
	#endif /* LWIP_NETIF_HOSTNAME */

	/*
	 * Initialize the snmp variables and counters inside the struct netif.
	 * The last argument should be replaced with your link speed, in units
	 * of bits per second.
	 */
	MIB2_INIT_NETIF(netif, snmp_ifType_ethernet_csmacd, LINK_SPEED_OF_YOUR_NETIF_IN_BPS);

	netif->name[0] = IFNAME0;
	netif->name[1] = IFNAME1;
	/* We directly use etharp_output() here to save a function call.
	 * You can instead declare your own function an call etharp_output()
	 * from it if you have to do some checks before sending (e.g. if link
	 * is available...) */
	#if LWIP_IPV4
	netif->output = etharp_output;
	#endif /* LWIP_IPV4 */
	#if LWIP_IPV6
	netif->output_ip6 = ethip6_output;
	#endif /* LWIP_IPV6 */
	netif->linkoutput = low_level_output;

	/* initialize the hardware */
	if (low_level_init(netif))
		return ERR_ARG;

	return ERR_OK;
}
