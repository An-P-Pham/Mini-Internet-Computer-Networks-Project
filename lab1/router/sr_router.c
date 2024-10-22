/**********************************************************************
 * file:  sr_router.c
 * date:  Mon Feb 18 12:50:42 PST 2002
 * Contact: casado@stanford.edu
 *
 * Description:
 *
 * This file contains all the functions that interact directly
 * with the routing table, as well as the main entry method
 * for routing.
 *
 **********************************************************************/

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>


#include "sr_if.h"
#include "sr_rt.h"
#include "sr_router.h"
#include "sr_protocol.h"
#include "sr_arpcache.h"
#include "sr_utils.h"

/*---------------------------------------------------------------------
 * Method: sr_init(void)
 * Scope:  Global
 *
 * Initialize the routing subsystem
 *
 *---------------------------------------------------------------------*/

void sr_init(struct sr_instance* sr)
{
    /* REQUIRES */
    assert(sr);

    /* Initialize cache and cache cleanup thread */
    sr_arpcache_init(&(sr->cache));

    pthread_attr_init(&(sr->attr));
    pthread_attr_setdetachstate(&(sr->attr), PTHREAD_CREATE_JOINABLE);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_t thread;

    pthread_create(&thread, &(sr->attr), sr_arpcache_timeout, sr);
    pthread_detach(thread);
    
    /* Add initialization code here! */

} /* -- sr_init -- */

/*---------------------------------------------------------------------
 * Method: sr_handlepacket(uint8_t* p,char* interface)
 * Scope:  Global
 *
 * This method is called each time the router receives a packet on the
 * interface.  The packet buffer, the packet length and the receiving
 * interface are passed in as parameters. The packet is complete with
 * ethernet headers.
 *
 * Note: Both the packet buffer and the character's memory are handled
 * by sr_vns_comm.c that means do NOT delete either.  Make a copy of the
 * packet instead if you intend to keep it around beyond the scope of
 * the method call.
 *
 *---------------------------------------------------------------------*/

void sr_handlepacket(struct sr_instance* sr,
        uint8_t * packet/* lent */,
        unsigned int len,
        char* interface/* lent */)
{
  /* REQUIRES */
  assert(sr);
  assert(packet);
  assert(interface);

  printf("*** -> Received packet of length %d \n",len);

  /* Entry of code */
  static const unsigned int IP_PACKET_SIZE_CHECK = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t);
  static const unsigned int ARP_PACKET_SIZE_CHECK = sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t);
  switch(ethertype(packet))
  {
    case ethertype_arp:
      if(len < ARP_PACKET_SIZE_CHECK)
      {
        return;
      }
      else
      {
        sr_handle_arp_packet_type(sr, packet, len, interface);
      }
      break;
    case ethertype_ip:
      if(len < IP_PACKET_SIZE_CHECK)
      {
        return;
      }
      else
      {
        sr_handle_ip_packet_type(sr, packet, len, interface);
      }
      break;
    default:
      return; /* Neither ARP or IP Packet type */
  }

}/* end sr_ForwardPacket */

void sr_handle_arp_packet_type(struct sr_instance* sr, 
  uint8_t * packet, unsigned int len, char* interface)
{
  /* Do we need ethernetHeader of the incoming packet or just fill out one and send*/

  sr_ethernet_hdr_t *recv_ethernet_hdr = (sr_ethernet_hdr_t *)(packet);  
  sr_arp_hdr_t *recv_arp_hdr = (sr_arp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));
  struct sr_if *recv_if = sr_get_interface(sr, interface);

  if(ntohs(recv_arp_hdr->ar_op) == arp_op_reply) /* We received an ARP Reply */
  {
    /* Cache it, go through request queue and send outstanding packet */
    struct sr_arpreq * arp_req_res = sr_arpcache_insert(&(sr->cache), recv_arp_hdr->ar_sha, recv_arp_hdr->ar_sip);
    if(arp_req_res == NULL)
    {
      return;
    }
    else
    {
      struct sr_packet *pkt = arp_req_res->packets;
      while(pkt != NULL)
      {
        sr_ethernet_hdr_t *ethernetFrame = (sr_ethernet_hdr_t *)(pkt->buf);
        memcpy(ethernetFrame->ether_dhost, recv_arp_hdr->ar_sha, ETHER_ADDR_LEN);
        memcpy(ethernetFrame->ether_shost, recv_if->addr, ETHER_ADDR_LEN);
        sr_send_packet(sr, pkt->buf, pkt->len, recv_if->name);
        pkt = pkt->next;
      }
      sr_arpreq_destroy(&(sr->cache), arp_req_res);
    } 

  }else if(ntohs(recv_arp_hdr->ar_op) == arp_op_request && recv_arp_hdr->ar_tip == recv_if->ip) /* Is this arp request to me? */
  {
    /* Construct ARP reply & send it back */
    uint8_t *ethernet_frame = (uint8_t *)calloc(1, len);
    /* Fill out the ethernet Header */
    sr_ethernet_hdr_t *send_ethernet_hdr = (sr_ethernet_hdr_t *)(ethernet_frame);
    send_ethernet_hdr->ether_type = htons(ethertype_arp);
    memcpy(send_ethernet_hdr->ether_dhost, recv_ethernet_hdr->ether_shost, ETHER_ADDR_LEN);
    memcpy(send_ethernet_hdr->ether_shost, recv_if->addr, ETHER_ADDR_LEN);
    /* Fill out ARP Header */
    sr_arp_hdr_t *send_arp_hdr = (sr_arp_hdr_t *)(ethernet_frame + sizeof(sr_ethernet_hdr_t));
    memcpy(send_arp_hdr, recv_arp_hdr, sizeof(sr_arp_hdr_t));
    send_arp_hdr->ar_tip = recv_arp_hdr->ar_sip;
    send_arp_hdr->ar_sip = recv_if->ip;
    send_arp_hdr->ar_op = htons(arp_op_reply);
    memcpy(send_arp_hdr->ar_tha, recv_ethernet_hdr->ether_shost, ETHER_ADDR_LEN);
    memcpy(send_arp_hdr->ar_sha, recv_if->addr, ETHER_ADDR_LEN);
    /* send arp request */
    sr_send_packet(sr, ethernet_frame, len, recv_if->name);
    free(ethernet_frame);
  }
  else
  {
    return;
  }
}

void sr_handle_ip_packet_type(struct sr_instance* sr, 
  uint8_t * packet, unsigned int len, char* interface)
{

  /* Validate the checksum 
  if(validate_packet(packet, sizeof(sr_ip_hdr_t), ip_header_type) == false)
  {
   return;
  }*/
  sr_ip_hdr_t *ip_hdr = (sr_ip_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));
  uint16_t chk_sum_1, chk_sum_2;
  chk_sum_1 = ip_hdr->ip_sum;
  ip_hdr->ip_sum = 0;
  chk_sum_2 = cksum(ip_hdr, sizeof(sr_ip_hdr_t));
  ip_hdr->ip_sum = chk_sum_2;
  if(chk_sum_1 != chk_sum_2)
  {
    ip_hdr->ip_sum = chk_sum_1;
    return;
  }
  else
  {
    /* We can continue processing */
    struct sr_if* curr_if = ip_packet_forwarding(sr, packet);
    if(curr_if == NULL)
    {
      /* Not on our interface list, thus next hop */
      sr_ip_packet_next_hop(sr, packet, len, interface);
    }
    else
    {
      /* Matches with one of our interface list */
      sr_ip_packet_reply(sr, packet, len, interface, curr_if->ip);
    }
  }
  return;
}

/*
bool validate_packet(uint8_t * packet, unsigned int len, enum sr_packet_header_type pkt_hdr_type)
{
  bool valid = true;
  uint16_t chk_sum_1;
  uint16_t chk_sum_2;

  if(pkt_hdr_type == ip_header_type)
  {
    sr_ip_hdr_t *ip_hdr = (sr_ip_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));
    chk_sum_1 = ip_hdr->ip_sum;
    ip_hdr->ip_sum = 0;
    chk_sum_2 = cksum(ip_hdr, len);
    if(chk_sum_1 != chk_sum_2)
    {
      ip_hdr->ip_sum = chk_sum_1;
      valid = false;
    }
  }else if(pkt_hdr_type == icmp_header_type)
  {
    sr_icmp_hdr_t *icmp_hdr = (sr_icmp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
    chk_sum_1 = icmp_hdr->icmp_sum;
    icmp_hdr->icmp_sum = 0;
    chk_sum_2 = cksum(icmp_hdr, len);
    icmp_hdr->icmp_sum = chk_sum_2;
    if(chk_sum_1 != chk_sum_2)
    {
      icmp_hdr->icmp_sum = chk_sum_1;
      valid = false;
    }
  }
  
  return valid;
}
*/

/* Returns NULL if ip packet is not part of subnet */
struct sr_if *ip_packet_forwarding(struct sr_instance* sr, uint8_t * packet)
{
  sr_ip_hdr_t *ip_hdr = (sr_ip_hdr_t*)(packet + sizeof(sr_ethernet_hdr_t));
  struct sr_if* curr_if = sr->if_list;
  /* We either find a matching ip address within interface list or reach end */
  while(curr_if != NULL)
  {
    if(curr_if->ip == ip_hdr->ip_dst)
    {
      return curr_if;
    }
    else
    {
      curr_if = curr_if->next;
    }
  }

  return curr_if;
} 

void sr_ip_packet_next_hop(struct sr_instance* sr, 
  uint8_t * packet, unsigned int len, char* interface)
{
  sr_ethernet_hdr_t *ethernet_hdr = (sr_ethernet_hdr_t *)(packet);
  sr_ip_hdr_t *ip_hdr = (sr_ip_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));
  struct sr_rt *rt_entry = lpm(sr, ip_hdr->ip_dst);
  if(rt_entry == NULL)
  {
    /* send net unreachable type */
    send_icmp_error_packet(sr, interface, len, 0, ethernet_hdr,
   ip_hdr, DST_NET_UNREACHABLE_TYPE, DST_NET_UNREACHABLE_CODE);
    return;
  }
  else
  {
    ip_hdr->ip_ttl -= 1;
    if(ip_hdr->ip_ttl > 0)
    {
      uint16_t calc_sum;
      ip_hdr->ip_sum = 0;
      calc_sum = cksum(ip_hdr, sizeof(sr_ip_hdr_t));
      ip_hdr->ip_sum = calc_sum;
      struct sr_if *recv_if = sr_get_interface(sr, rt_entry->interface);
      /* frame to next hop */
      struct sr_arpentry *arp_entry = sr_arpcache_lookup(&(sr->cache), rt_entry->gw.s_addr);
      if(arp_entry != NULL)
      {
        memcpy(ethernet_hdr->ether_dhost, (uint8_t *)arp_entry->mac, ETHER_ADDR_LEN);
        memcpy(ethernet_hdr->ether_shost, recv_if->addr, ETHER_ADDR_LEN);
        free(arp_entry);
        sr_send_packet(sr, packet, len, rt_entry->interface);
      }
      else
      {
        struct sr_arpreq *req = sr_arpcache_queuereq(&(sr->cache), rt_entry->gw.s_addr, packet, len, rt_entry->interface);
        handle_arpreq(sr, req);
      }
    }
    else
    {
      /* Time exceeded error code */
      send_icmp_error_packet(sr, interface, len, 0, ethernet_hdr,
        ip_hdr, TIME_EXCEEDED_TYPE, TIME_EXCEEDED_CODE);
      return;
    }
  }
}

void sr_ip_packet_reply(struct sr_instance* sr, 
  uint8_t * packet, unsigned int len, char* interface,
  uint32_t dest_if_ip)
{
  sr_ethernet_hdr_t *ethernet_hdr = (sr_ethernet_hdr_t *)(packet);
  sr_ip_hdr_t *ip_hdr = (sr_ip_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));
  sr_icmp_hdr_t *icmp_hdr = (sr_icmp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));

  if(ip_protocol((uint8_t *)ip_hdr) == ip_protocol_icmp)
  {
    if(len < sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_hdr_t))
    {
      return;
    }
    else
    {
      /* if(validate_packet(packet, len - sizeof(sr_ethernet_hdr_t) - sizeof(sr_ip_hdr_t), icmp_header_type) == false)
       {
         
         return;
       } */
      uint16_t chk_sum_1, chk_sum_2;
      chk_sum_1 = icmp_hdr->icmp_sum;
      icmp_hdr->icmp_sum = 0;
      chk_sum_2 = cksum(icmp_hdr, len - sizeof(sr_ethernet_hdr_t) - sizeof(sr_ip_hdr_t));
      icmp_hdr->icmp_sum = chk_sum_2;
      if(chk_sum_1 != chk_sum_2)
      {
        icmp_hdr->icmp_sum = chk_sum_1;
        return;
      }
      else
      {
        /* Reply with an echo message */
        send_icmp_echo_packet(sr, interface, len, dest_if_ip,
           ethernet_hdr, ip_hdr, icmp_hdr);
        return;
      }
    }
    
  }
  /* IP Protocol is TCP or UDP --> send ICMP ERROR message */
  send_icmp_error_packet(sr, interface, len, dest_if_ip, ethernet_hdr,
   ip_hdr, PORT_UNREACHABLE_TYPE, PORT_UNREACHABLE_CODE);
}

struct sr_rt *lpm(struct sr_instance* sr, uint32_t dest_ip_addr)
{
  struct sr_rt *result = NULL;

  struct sr_rt *curr_rt_entry = sr->routing_table;
  while(curr_rt_entry != NULL)
  {
    if((curr_rt_entry->dest.s_addr & curr_rt_entry->mask.s_addr)
      == (curr_rt_entry->mask.s_addr & dest_ip_addr))
    {
      if(result == NULL || (curr_rt_entry->mask.s_addr > result->mask.s_addr))
      {
        result = curr_rt_entry;
      }
    }
    
    curr_rt_entry = curr_rt_entry->next;
  }

  return result;
}

void send_icmp_echo_packet(struct sr_instance* sr, char* interface, 
  unsigned int len, uint32_t dest_if_ip, sr_ethernet_hdr_t *recv_ethernet_hdr, 
  sr_ip_hdr_t *recv_ip_hdr, sr_icmp_hdr_t *recv_icmp_hdr)
{
  /* Function Variables */
  static const unsigned int total_header_size = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_hdr_t);
  uint8_t *icmp_echo_packet = (uint8_t *)malloc(len);
  sr_ethernet_hdr_t *send_ethernet_hdr = (sr_ethernet_hdr_t *)(icmp_echo_packet);
  sr_ip_hdr_t *send_ip_hdr = (sr_ip_hdr_t *)(icmp_echo_packet + sizeof(sr_ethernet_hdr_t));
  sr_icmp_hdr_t *send_icmp_hdr = (sr_icmp_hdr_t *)(icmp_echo_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
  struct sr_if *send_if = sr_get_interface(sr, interface);
  /* Fill out Ethernet Header */
  memcpy(send_ethernet_hdr->ether_dhost, recv_ethernet_hdr->ether_shost, ETHER_ADDR_LEN);
  memcpy(send_ethernet_hdr->ether_shost, (uint8_t *)send_if->addr, ETHER_ADDR_LEN);
  send_ethernet_hdr->ether_type = htons(ethertype_ip);
  /* Fill out IP Header */
  memcpy(send_ip_hdr, recv_ip_hdr, sizeof(sr_ip_hdr_t));
  send_ip_hdr->ip_ttl = INIT_TTL;
  send_ip_hdr->ip_p = ip_protocol_icmp;
  send_ip_hdr->ip_dst = recv_ip_hdr->ip_src;
  send_ip_hdr->ip_len = htons(len - sizeof(sr_ethernet_hdr_t));
  send_ip_hdr->ip_src = dest_if_ip;
  send_ip_hdr->ip_sum = 0;
  send_ip_hdr->ip_sum = cksum(send_ip_hdr, sizeof(sr_ip_hdr_t));
  /* Copy over payload */
  unsigned int icmp_packet_size = len - sizeof(sr_ethernet_hdr_t) - sizeof(sr_ip_hdr_t);
  memcpy(send_icmp_hdr, recv_icmp_hdr, icmp_packet_size);
  memset(send_icmp_hdr, 0, sizeof(sr_icmp_hdr_t));
  send_icmp_hdr->icmp_sum = cksum(send_icmp_hdr, icmp_packet_size);
  /* Send the packet */
  sr_send_packet(sr, icmp_echo_packet, len, interface);
  free(icmp_echo_packet);
}

void send_icmp_error_packet(struct sr_instance* sr, char * interface, 
  unsigned int len, uint32_t dest_if_ip, sr_ethernet_hdr_t *recv_ethernet_hdr,
   sr_ip_hdr_t *recv_ip_hdr, uint8_t error_type, uint8_t error_code)
{
  /* Function Variables */
  uint8_t *icmp_echo_packet = (uint8_t *)calloc(1, len);
  sr_ethernet_hdr_t *send_ethernet_hdr = (sr_ethernet_hdr_t *)(icmp_echo_packet);
  sr_ip_hdr_t *send_ip_hdr = (sr_ip_hdr_t *)(icmp_echo_packet + sizeof(sr_ethernet_hdr_t));
  sr_icmp_hdr_t *send_icmp_hdr = (sr_icmp_hdr_t *)(icmp_echo_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
  struct sr_if *send_if = sr_get_interface(sr, interface);
  /* Fill out Ethernet Header */
  memcpy(send_ethernet_hdr->ether_dhost, recv_ethernet_hdr->ether_shost, ETHER_ADDR_LEN);
  memcpy(send_ethernet_hdr->ether_shost, (uint8_t *)send_if->addr, ETHER_ADDR_LEN);
  send_ethernet_hdr->ether_type = htons(ethertype_ip);
  /* Fill out IP Header */
  memcpy(send_ip_hdr, recv_ip_hdr, sizeof(sr_ip_hdr_t));
  send_ip_hdr->ip_ttl = INIT_TTL;
  send_ip_hdr->ip_p = ip_protocol_icmp;
  send_ip_hdr->ip_dst = recv_ip_hdr->ip_src;
  send_ip_hdr->ip_len = htons(len - sizeof(sr_ethernet_hdr_t));
  send_ip_hdr->ip_src = 
  (error_type == PORT_UNREACHABLE_TYPE && 
    error_code == PORT_UNREACHABLE_CODE) ? dest_if_ip : send_if->ip;
  send_ip_hdr->ip_sum = 0;
  send_ip_hdr->ip_sum = cksum(send_ip_hdr, sizeof(sr_ip_hdr_t));
  /* Fill out ICMP Header */
  send_icmp_hdr->icmp_type = error_type;
  send_icmp_hdr->icmp_code = error_code;
  send_icmp_hdr->icmp_sum = 0;
  send_icmp_hdr->icmp_sum = cksum(send_icmp_hdr, sizeof(sr_icmp_hdr_t));
  /* Send the packet */
  sr_send_packet(sr, icmp_echo_packet, len, interface);
  free(icmp_echo_packet);
}