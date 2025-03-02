/*
 * reader_util.c
 *
 * Copyright (C) 2011-19 - ntop.org
 *
 * This file is part of nDPI, an open source deep packet inspection
 * library based on the OpenDPI and PACE technology by ipoque GmbH
 *
 * nDPI is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * nDPI is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with nDPI.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef HAVE_CONFIG_H
#include "ndpi_config.h"
#endif

#include <stdlib.h>

#ifdef WIN32
#include <winsock2.h> /* winsock.h is included automatically */
#include <process.h>
#include <io.h>
#else
#include <unistd.h>
#include <netinet/in.h>
#include <math.h>
#include <float.h>
#endif

#ifndef ETH_P_IP
#define ETH_P_IP               0x0800 	/* IPv4 */
#endif

#ifndef ETH_P_IPv6
#define ETH_P_IPV6	       0x86dd	/* IPv6 */
#endif

#define SLARP                  0x8035   /* Cisco Slarp */
#define CISCO_D_PROTO          0x2000	/* Cisco Discovery Protocol */

#define VLAN                   0x8100
#define MPLS_UNI               0x8847
#define MPLS_MULTI             0x8848
#define PPPoE                  0x8864
#define SNAP                   0xaa
#define BSTP                   0x42     /* Bridge Spanning Tree Protocol */

/* Keep last 32 packets */
#define DATA_ANALUYSIS_SLIDING_WINDOW    32

/* mask for FCF */
#define	WIFI_DATA                        0x2    /* 0000 0010 */
#define FCF_TYPE(fc)     (((fc) >> 2) & 0x3)    /* 0000 0011 = 0x3 */
#define FCF_SUBTYPE(fc)  (((fc) >> 4) & 0xF)    /* 0000 1111 = 0xF */
#define FCF_TO_DS(fc)        ((fc) & 0x0100)
#define FCF_FROM_DS(fc)      ((fc) & 0x0200)

/* mask for Bad FCF presence */
#define BAD_FCS                         0x50    /* 0101 0000 */

#define GTP_U_V1_PORT                   2152
#define TZSP_PORT                      37008

#ifndef DLT_LINUX_SLL
#define DLT_LINUX_SLL  113
#endif

#include "ndpi_main.h"
#include "reader_util.h"
#include "ndpi_classify.h"

extern u_int8_t enable_protocol_guess, enable_joy_stats, enable_payload_analyzer;
extern u_int8_t verbose, human_readeable_string_len;
extern u_int8_t max_num_udp_dissected_pkts /* 8 */, max_num_tcp_dissected_pkts /* 10 */;

static u_int32_t flow_id = 0;

/* ****************************************************** */

struct flow_id_stats {
  u_int32_t flow_id;
  UT_hash_handle hh;   /* makes this structure hashable */
};

struct packet_id_stats {
  u_int32_t packet_id;
  UT_hash_handle hh;   /* makes this structure hashable */
};

struct payload_stats {
  u_int8_t *pattern;
  u_int8_t pattern_len;
  u_int16_t num_occurrencies;
  struct flow_id_stats *flows;
  struct packet_id_stats *packets;
  UT_hash_handle hh;   /* makes this structure hashable */
};


struct payload_stats *pstats = NULL;
u_int32_t max_num_packets_per_flow      = 10; /* ETTA requires min 10 pkts for record. */
u_int32_t max_packet_payload_dissection = 128;
u_int32_t max_num_reported_top_payloads = 25;
u_int16_t min_pattern_len = 4;
u_int16_t max_pattern_len = 8;

/* *********************************************************** */

void ndpi_analyze_payload(struct ndpi_flow_info *flow,
			  u_int8_t src_to_dst_direction,
			  u_int8_t *payload,
			  u_int16_t payload_len,
			  u_int32_t packet_id) {
  struct payload_stats *ret;
  u_int i;
  struct flow_id_stats *f;
  struct packet_id_stats *p;

#ifdef DEBUG_PAYLOAD
  for(i=0; i<payload_len; i++)
    printf("%c", isprint(payload[i]) ? payload[i] : '.');
  printf("\n");
#endif

  HASH_FIND(hh, pstats, payload, payload_len, ret);
  if(ret == NULL) {
    if((ret = (struct payload_stats*)calloc(1, sizeof(struct payload_stats))) == NULL)
      return; /* OOM */

    if((ret->pattern = (u_int8_t*)malloc(payload_len)) == NULL) {
      free(ret);
      return;
    }

    memcpy(ret->pattern, payload, payload_len);
    ret->pattern_len = payload_len;
    ret->num_occurrencies = 1;

    HASH_ADD(hh, pstats, pattern[0], payload_len, ret);

#ifdef DEBUG_PAYLOAD
    printf("Added element [total: %u]\n", HASH_COUNT(pstats));
#endif
  } else {
    ret->num_occurrencies++;
    // printf("==> %u\n", ret->num_occurrencies);
  }

  HASH_FIND_INT(ret->flows, &flow->flow_id, f);
  if(f == NULL) {
    if((f = (struct flow_id_stats*)calloc(1, sizeof(struct flow_id_stats))) == NULL)
      return; /* OOM */

    f->flow_id = flow->flow_id;
    HASH_ADD_INT(ret->flows, flow_id, f);
  }

  HASH_FIND_INT(ret->packets, &packet_id, p);
  if(p == NULL) {
    if((p = (struct packet_id_stats*)calloc(1, sizeof(struct packet_id_stats))) == NULL)
      return; /* OOM */
    p->packet_id = packet_id;

    HASH_ADD_INT(ret->packets, packet_id, p);
  }
}

/* *********************************************************** */

void ndpi_payload_analyzer(struct ndpi_flow_info *flow,
			   u_int8_t src_to_dst_direction,
			   u_int8_t *payload, u_int16_t payload_len,
			   u_int32_t packet_id) {
  u_int16_t i, j;
  u_int16_t scan_len = ndpi_min(max_packet_payload_dissection, payload_len);

  if((flow->src2dst_packets+flow->dst2src_packets) <= max_num_packets_per_flow) {
#ifdef DEBUG_PAYLOAD
    printf("[hashval: %u][proto: %u][vlan: %u][%s:%u <-> %s:%u][direction: %s][payload_len: %u]\n",
	   flow->hashval, flow->protocol, flow->vlan_id,
	   flow->src_name, flow->src_port,
	   flow->dst_name, flow->dst_port,
	   src_to_dst_direction ? "s2d" : "d2s",
	   payload_len);
#endif
  } else
    return;

  for(i=0; i<scan_len; i++) {
    for(j=min_pattern_len; j <= max_pattern_len; j++) {
      if((i+j) < payload_len) {
	ndpi_analyze_payload(flow, src_to_dst_direction, &payload[i], j, packet_id);
      }
    }
  }
}

/* ***************************************************** */

static int payload_stats_sort_asc(void *_a, void *_b) {
  struct payload_stats *a = (struct payload_stats *)_a;
  struct payload_stats *b = (struct payload_stats *)_b;

  //return(a->num_occurrencies - b->num_occurrencies);
  return(b->num_occurrencies - a->num_occurrencies);
}

/* ***************************************************** */

void print_payload_stat(struct payload_stats *p) {
  u_int i;
  struct flow_id_stats *s, *tmp;
  struct packet_id_stats *s1, *tmp1;

  printf("\t[");

  for(i=0; i<p->pattern_len; i++) {
    printf("%c", isprint(p->pattern[i]) ? p->pattern[i] : '.');
  }

  printf("]");
  for(; i<16; i++) printf(" ");
  printf("[");

  for(i=0; i<p->pattern_len; i++) {
    printf("%s%02X", (i > 0) ? " " : "", isprint(p->pattern[i]) ? p->pattern[i] : '.');
  }

  printf("]");

  for(; i<16; i++) printf("  ");
  for(i=p->pattern_len; i<max_pattern_len; i++) printf(" ");

  printf("[len: %u][num_occurrencies: %u][flowId: ",
	 p->pattern_len, p->num_occurrencies);

  i = 0;
  HASH_ITER(hh, p->flows, s, tmp) {
    printf("%s%u", (i > 0) ? " " : "", s->flow_id);
    i++;
  }

  printf("][packetIds: ");

  /* ******************************** */

  i = 0;
  HASH_ITER(hh, p->packets, s1, tmp1) {
    printf("%s%u", (i > 0) ? " " : "", s1->packet_id);
    i++;
  }

  printf("]\n");


}

/* ***************************************************** */

void ndpi_report_payload_stats() {
  struct payload_stats *p, *tmp;
  u_int num = 0;

  printf("\n\nPayload Analysis\n");

  HASH_SORT(pstats, payload_stats_sort_asc);

  HASH_ITER(hh, pstats, p, tmp) {
    if(num <= max_num_reported_top_payloads)
      print_payload_stat(p);

    free(p->pattern);

    {
      struct flow_id_stats *p1, *tmp1;

      HASH_ITER(hh, p->flows, p1, tmp1) {
	HASH_DEL(p->flows, p1);
	free(p1);
      }
    }

    {
      struct packet_id_stats *p1, *tmp1;

      HASH_ITER(hh, p->packets, p1, tmp1) {
	HASH_DEL(p->packets, p1);
	free(p1);
      }
    }

    HASH_DEL(pstats, p);
    free(p);
    num++;
  }
}



/* ***************************************************** */

void ndpi_free_flow_info_half(struct ndpi_flow_info *flow) {
  if(flow->ndpi_flow) { ndpi_flow_free(flow->ndpi_flow); flow->ndpi_flow = NULL; }
  if(flow->src_id)    { ndpi_free(flow->src_id); flow->src_id = NULL; }
  if(flow->dst_id)    { ndpi_free(flow->dst_id); flow->dst_id = NULL; }
}

/* ***************************************************** */

extern u_int32_t current_ndpi_memory, max_ndpi_memory;

/**
 * @brief malloc wrapper function
 */
static void *malloc_wrapper(size_t size) {
  current_ndpi_memory += size;

  if(current_ndpi_memory > max_ndpi_memory)
    max_ndpi_memory = current_ndpi_memory;

  return malloc(size);
}

/* ***************************************************** */

/**
 * @brief free wrapper function
 */
static void free_wrapper(void *freeable) {
  free(freeable);
}

/* ***************************************************** */

static uint16_t ndpi_get_proto_id(struct ndpi_detection_module_struct *ndpi_mod, const char *name) {
  uint16_t proto_id;
  char *e;
  unsigned long p = strtol(name,&e,0);
  ndpi_proto_defaults_t *proto_defaults = ndpi_get_proto_defaults(ndpi_mod);

  if(e && !*e) {
    if(p < NDPI_MAX_SUPPORTED_PROTOCOLS+NDPI_MAX_NUM_CUSTOM_PROTOCOLS &&
       proto_defaults[p].protoName) return (uint16_t)p;
    return NDPI_PROTOCOL_UNKNOWN;
  }

  for(proto_id=NDPI_PROTOCOL_UNKNOWN; proto_id < NDPI_MAX_SUPPORTED_PROTOCOLS+NDPI_MAX_NUM_CUSTOM_PROTOCOLS; proto_id++) {
    if(proto_defaults[proto_id].protoName &&
       !strcasecmp(proto_defaults[proto_id].protoName,name))
      return proto_id;
  }
  return NDPI_PROTOCOL_UNKNOWN;
}

/* ***************************************************** */

static NDPI_PROTOCOL_BITMASK debug_bitmask;
static char _proto_delim[] = " \t,:;";
static int parse_debug_proto(struct ndpi_detection_module_struct *ndpi_mod, char *str) {
  char *n;
  uint16_t proto;
  char op=1;
  for(n = strtok(str,_proto_delim); n && *n; n = strtok(NULL,_proto_delim)) {
    if(*n == '-') {
      op = 0;
      n++;
    } else if(*n == '+') {
      op = 1;
      n++;
    }
    if(!strcmp(n,"all")) {
      if(op)
	NDPI_BITMASK_SET_ALL(debug_bitmask);
      else
	NDPI_BITMASK_RESET(debug_bitmask);
      continue;
    }
    proto = ndpi_get_proto_id(ndpi_mod, n);
    if(proto == NDPI_PROTOCOL_UNKNOWN && strcmp(n,"unknown") && strcmp(n,"0")) {
      fprintf(stderr,"Invalid protocol %s\n",n);
      return 1;
    }
    if(op)
      NDPI_BITMASK_ADD(debug_bitmask,proto);
    else
      NDPI_BITMASK_DEL(debug_bitmask,proto);
  }
  return 0;
}

/* ***************************************************** */

extern char *_debug_protocols;
static int _debug_protocols_ok = 0;

struct ndpi_workflow* ndpi_workflow_init(const struct ndpi_workflow_prefs * prefs,
					 pcap_t * pcap_handle) {
  struct ndpi_detection_module_struct * module;
  struct ndpi_workflow * workflow;

  set_ndpi_malloc(malloc_wrapper), set_ndpi_free(free_wrapper);
  set_ndpi_flow_malloc(NULL), set_ndpi_flow_free(NULL);

  /* TODO: just needed here to init ndpi malloc wrapper */
  module = ndpi_init_detection_module();

  if(module == NULL) {
    NDPI_LOG(0, NULL, NDPI_LOG_ERROR, "global structure initialization failed\n");
    exit(-1);
  }

  workflow = ndpi_calloc(1, sizeof(struct ndpi_workflow));
  if(workflow == NULL) {
    NDPI_LOG(0, NULL, NDPI_LOG_ERROR, "global structure initialization failed\n");
    ndpi_free(module);
    exit(-1);
  }

  workflow->pcap_handle = pcap_handle;
  workflow->prefs       = *prefs;
  workflow->ndpi_struct = module;

  ndpi_set_log_level(module, nDPI_LogLevel);

  if(_debug_protocols != NULL && ! _debug_protocols_ok) {
    if(parse_debug_proto(module,_debug_protocols))
      exit(-1);
    _debug_protocols_ok = 1;
  }

#ifdef NDPI_ENABLE_DEBUG_MESSAGES
  NDPI_BITMASK_RESET(module->debug_bitmask);

  if(_debug_protocols_ok)
    module->debug_bitmask = debug_bitmask;
#endif

  workflow->ndpi_flows_root = ndpi_calloc(workflow->prefs.num_roots, sizeof(void *));

  return workflow;
}

/* ***************************************************** */

void ndpi_flow_info_freer(void *node) {
  struct ndpi_flow_info *flow = (struct ndpi_flow_info*)node;

  ndpi_free_flow_info_half(flow);

  if(flow->iat_c_to_s) ndpi_free_data_analysis(flow->iat_c_to_s);
  if(flow->iat_s_to_c) ndpi_free_data_analysis(flow->iat_s_to_c);

  if(flow->pktlen_c_to_s) ndpi_free_data_analysis(flow->pktlen_c_to_s);
  if(flow->pktlen_s_to_c) ndpi_free_data_analysis(flow->pktlen_s_to_c);

  if(flow->iat_flow) ndpi_free_data_analysis(flow->iat_flow);

  ndpi_free(flow);
}

/* ***************************************************** */

void ndpi_workflow_free(struct ndpi_workflow * workflow) {
  u_int i;

  for(i=0; i<workflow->prefs.num_roots; i++)
    ndpi_tdestroy(workflow->ndpi_flows_root[i], ndpi_flow_info_freer);

  ndpi_exit_detection_module(workflow->ndpi_struct);
  free(workflow->ndpi_flows_root);
  free(workflow);
}

/* ***************************************************** */

int ndpi_workflow_node_cmp(const void *a, const void *b) {
  const struct ndpi_flow_info *fa = (const struct ndpi_flow_info*)a;
  const struct ndpi_flow_info *fb = (const struct ndpi_flow_info*)b;

  if(fa->hashval < fb->hashval) return(-1); else if(fa->hashval > fb->hashval) return(1);

  /* Flows have the same hash */

  if(fa->vlan_id   < fb->vlan_id   ) return(-1); else { if(fa->vlan_id    > fb->vlan_id   ) return(1); }
  if(fa->protocol  < fb->protocol  ) return(-1); else { if(fa->protocol   > fb->protocol  ) return(1); }

  if(
     (
      (fa->src_ip      == fb->src_ip  )
      && (fa->src_port == fb->src_port)
      && (fa->dst_ip   == fb->dst_ip  )
      && (fa->dst_port == fb->dst_port)
      )
     ||
     (
      (fa->src_ip      == fb->dst_ip  )
      && (fa->src_port == fb->dst_port)
      && (fa->dst_ip   == fb->src_ip  )
      && (fa->dst_port == fb->src_port)
      )
     )
    return(0);

  if(fa->src_ip   < fb->src_ip  ) return(-1); else { if(fa->src_ip   > fb->src_ip  ) return(1); }
  if(fa->src_port < fb->src_port) return(-1); else { if(fa->src_port > fb->src_port) return(1); }
  if(fa->dst_ip   < fb->dst_ip  ) return(-1); else { if(fa->dst_ip   > fb->dst_ip  ) return(1); }
  if(fa->dst_port < fb->dst_port) return(-1); else { if(fa->dst_port > fb->dst_port) return(1); }

  return(0); /* notreached */
}

/* ***************************************************** */

/**
 * \brief Update the byte count for the flow record.
 * \param f Flow data
 * \param x Data to use for update
 * \param len Length of the data (in bytes)
 * \return none
 */
static void
ndpi_flow_update_byte_count(struct ndpi_flow_info *flow, const void *x,
                            unsigned int len, u_int8_t src_to_dst_direction) {
  const unsigned char *data = x;
  u_int32_t i;
  u_int32_t current_count = 0;

  /*
   * implementation note: The spec says that 4000 octets is enough of a
   * sample size to accurately reflect the byte distribution. Also, to avoid
   * wrapping of the byte count at the 16-bit boundry, we stop counting once
   * the 4000th octet has been seen for a flow.
   */

  if((flow->entropy.src2dst_pkt_count+flow->entropy.dst2src_pkt_count) <= max_num_packets_per_flow) {
    /* octet count was already incremented before processing this payload */
    if(src_to_dst_direction) {
      current_count = flow->entropy.src2dst_l4_bytes - len;
    } else {
      current_count = flow->entropy.dst2src_l4_bytes - len;
    }

    if(current_count < ETTA_MIN_OCTETS) {
      for(i=0; i<len; i++) {
        if(src_to_dst_direction) {
          flow->entropy.src2dst_byte_count[data[i]]++;
        } else {
          flow->entropy.dst2src_byte_count[data[i]]++;
        }
        current_count++;
        if(current_count >= ETTA_MIN_OCTETS) {
          break;
        }
      }
    }
  }
}

/* ***************************************************** */

/**
 * \brief Update the byte distribution mean for the flow record.
 * \param f Flow record
 * \param x Data to use for update
 * \param len Length of the data (in bytes)
 * \return none
 */
static void
ndpi_flow_update_byte_dist_mean_var(ndpi_flow_info_t *flow, const void *x,
                                    unsigned int len, u_int8_t src_to_dst_direction) {
  const unsigned char *data = x;
  double delta;
  unsigned int i;

  if((flow->entropy.src2dst_pkt_count+flow->entropy.dst2src_pkt_count) <= max_num_packets_per_flow) {
    for(i=0; i<len; i++) {
      if(src_to_dst_direction) {
        flow->entropy.src2dst_num_bytes += 1;
        delta = ((double)data[i] - flow->entropy.src2dst_bd_mean);
        flow->entropy.src2dst_bd_mean += delta/((double)flow->entropy.src2dst_num_bytes);
        flow->entropy.src2dst_bd_variance += delta*((double)data[i] - flow->entropy.src2dst_bd_mean);
      } else {
        flow->entropy.dst2src_num_bytes += 1;
        delta = ((double)data[i] - flow->entropy.dst2src_bd_mean);
        flow->entropy.dst2src_bd_mean += delta/((double)flow->entropy.dst2src_num_bytes);
        flow->entropy.dst2src_bd_variance += delta*((double)data[i] - flow->entropy.dst2src_bd_mean);
      }
    }
  }
}

/* ***************************************************** */

float ndpi_flow_get_byte_count_entropy(const uint32_t byte_count[256],
				       unsigned int num_bytes)
{
  int i;
  float tmp, sum = 0.0;

  for(i=0; i<256; i++) {
    tmp = (float) byte_count[i] / (float) num_bytes;
    if(tmp > FLT_EPSILON) {
      sum -= tmp * logf(tmp);
    }
  }
  return sum / logf(2.0);
}

/* ***************************************************** */

static struct ndpi_flow_info *get_ndpi_flow_info(struct ndpi_workflow * workflow,
						 const u_int8_t version,
						 u_int16_t vlan_id,
						 const struct ndpi_iphdr *iph,
						 const struct ndpi_ipv6hdr *iph6,
						 u_int16_t ip_offset,
						 u_int16_t ipsize,
						 u_int16_t l4_packet_len,
						 struct ndpi_tcphdr **tcph,
						 struct ndpi_udphdr **udph,
						 u_int16_t *sport, u_int16_t *dport,
						 struct ndpi_id_struct **src,
						 struct ndpi_id_struct **dst,
						 u_int8_t *proto,
						 u_int8_t **payload,
						 u_int16_t *payload_len,
						 u_int8_t *src_to_dst_direction,
                                                 struct timeval when) {
  u_int32_t idx, l4_offset, hashval;
  struct ndpi_flow_info flow;
  void *ret;
  const u_int8_t *l3, *l4;
  u_int32_t l4_data_len = 0XFEEDFACE;

  /*
    Note: to keep things simple (ndpiReader is just a demo app)
    we handle IPv6 a-la-IPv4.
  */
  if(version == IPVERSION) {
    if(ipsize < 20)
      return NULL;

    if((iph->ihl * 4) > ipsize || ipsize < ntohs(iph->tot_len)
       /* || (iph->frag_off & htons(0x1FFF)) != 0 */)
      return NULL;

    l4_offset = iph->ihl * 4;
    l3 = (const u_int8_t*)iph;
  } else {
    l4_offset = sizeof(struct ndpi_ipv6hdr);
    l3 = (const u_int8_t*)iph6;
  }

  *proto = iph->protocol;

  if(l4_packet_len < 64)
    workflow->stats.packet_len[0]++;
  else if(l4_packet_len >= 64 && l4_packet_len < 128)
    workflow->stats.packet_len[1]++;
  else if(l4_packet_len >= 128 && l4_packet_len < 256)
    workflow->stats.packet_len[2]++;
  else if(l4_packet_len >= 256 && l4_packet_len < 1024)
    workflow->stats.packet_len[3]++;
  else if(l4_packet_len >= 1024 && l4_packet_len < 1500)
    workflow->stats.packet_len[4]++;
  else if(l4_packet_len >= 1500)
    workflow->stats.packet_len[5]++;
  
  if(l4_packet_len > workflow->stats.max_packet_len)
    workflow->stats.max_packet_len = l4_packet_len;

  l4 = ((const u_int8_t *) l3 + l4_offset);

  if(*proto == IPPROTO_TCP && l4_packet_len >= sizeof(struct ndpi_tcphdr)) {
    u_int tcp_len;

    // tcp
    workflow->stats.tcp_count++;
    *tcph = (struct ndpi_tcphdr *)l4;
    *sport = ntohs((*tcph)->source), *dport = ntohs((*tcph)->dest);
    tcp_len = ndpi_min(4*(*tcph)->doff, l4_packet_len);
    *payload = (u_int8_t*)&l4[tcp_len];
    *payload_len = ndpi_max(0, l4_packet_len-4*(*tcph)->doff);
    l4_data_len = l4_packet_len - sizeof(struct ndpi_tcphdr);
  } else if(*proto == IPPROTO_UDP && l4_packet_len >= sizeof(struct ndpi_udphdr)) {
    // udp

    workflow->stats.udp_count++;
    *udph = (struct ndpi_udphdr *)l4;
    *sport = ntohs((*udph)->source), *dport = ntohs((*udph)->dest);
    *payload = (u_int8_t*)&l4[sizeof(struct ndpi_udphdr)];
    *payload_len = (l4_packet_len > sizeof(struct ndpi_udphdr)) ? l4_packet_len-sizeof(struct ndpi_udphdr) : 0;
    l4_data_len = l4_packet_len - sizeof(struct ndpi_udphdr);
  } else if(*proto == IPPROTO_ICMP) {
    *payload = (u_int8_t*)&l4[sizeof(struct ndpi_icmphdr )];
    *payload_len = (l4_packet_len > sizeof(struct ndpi_icmphdr)) ? l4_packet_len-sizeof(struct ndpi_icmphdr) : 0;
    l4_data_len = l4_packet_len - sizeof(struct ndpi_icmphdr);
    *sport = *dport = 0;
  } else if(*proto == IPPROTO_ICMPV6) {
    *payload = (u_int8_t*)&l4[sizeof(struct ndpi_icmp6hdr)];
    *payload_len = (l4_packet_len > sizeof(struct ndpi_icmp6hdr)) ? l4_packet_len-sizeof(struct ndpi_icmp6hdr) : 0;
    l4_data_len = l4_packet_len - sizeof(struct ndpi_icmp6hdr);
    *sport = *dport = 0;
  } else {
    // non tcp/udp protocols
    *sport = *dport = 0;
    l4_data_len = 0;
  }

  flow.protocol = iph->protocol, flow.vlan_id = vlan_id;
  flow.src_ip = iph->saddr, flow.dst_ip = iph->daddr;
  flow.src_port = htons(*sport), flow.dst_port = htons(*dport);
  flow.hashval = hashval = flow.protocol + flow.vlan_id + flow.src_ip + flow.dst_ip + flow.src_port + flow.dst_port;

#if 0
  printf("hashval=%u [%u][%u][%u:%u][%u:%u]\n", hashval, flow.protocol, flow.vlan_id,
	 flow.src_ip, flow.src_port, flow.dst_ip, flow.dst_port);
#endif

  idx = hashval % workflow->prefs.num_roots;
  ret = ndpi_tfind(&flow, &workflow->ndpi_flows_root[idx], ndpi_workflow_node_cmp);

  /* to avoid two nodes in one binary tree for a flow */
  int is_changed = 0;
  if(ret == NULL) {
    u_int32_t orig_src_ip = flow.src_ip;
    u_int16_t orig_src_port = flow.src_port;
    u_int32_t orig_dst_ip = flow.dst_ip;
    u_int16_t orig_dst_port = flow.dst_port;

    flow.src_ip = orig_dst_ip;
    flow.src_port = orig_dst_port;
    flow.dst_ip = orig_src_ip;
    flow.dst_port = orig_src_port;

    is_changed = 1;

    ret = ndpi_tfind(&flow, &workflow->ndpi_flows_root[idx], ndpi_workflow_node_cmp);
  }

  if(ret == NULL) {
    if(workflow->stats.ndpi_flow_count == workflow->prefs.max_ndpi_flows) {
      NDPI_LOG(0, workflow->ndpi_struct, NDPI_LOG_ERROR,
	       "maximum flow count (%u) has been exceeded\n",
	       workflow->prefs.max_ndpi_flows);
      exit(-1);
    } else {
      struct ndpi_flow_info *newflow = (struct ndpi_flow_info*)malloc(sizeof(struct ndpi_flow_info));

      if(newflow == NULL) {
	NDPI_LOG(0, workflow->ndpi_struct, NDPI_LOG_ERROR, "[NDPI] %s(1): not enough memory\n", __FUNCTION__);
	return(NULL);
      } else
        workflow->num_allocated_flows++;

      memset(newflow, 0, sizeof(struct ndpi_flow_info));
      newflow->flow_id = flow_id++;
      newflow->hashval = hashval;
      newflow->protocol = iph->protocol, newflow->vlan_id = vlan_id;
      newflow->src_ip = iph->saddr, newflow->dst_ip = iph->daddr;
      newflow->src_port = htons(*sport), newflow->dst_port = htons(*dport);
      newflow->ip_version = version;
      newflow->iat_c_to_s = ndpi_alloc_data_analysis(DATA_ANALUYSIS_SLIDING_WINDOW),
	newflow->iat_s_to_c =  ndpi_alloc_data_analysis(DATA_ANALUYSIS_SLIDING_WINDOW);
      newflow->pktlen_c_to_s = ndpi_alloc_data_analysis(DATA_ANALUYSIS_SLIDING_WINDOW),
	newflow->pktlen_s_to_c =  ndpi_alloc_data_analysis(DATA_ANALUYSIS_SLIDING_WINDOW),
	newflow->iat_flow = ndpi_alloc_data_analysis(DATA_ANALUYSIS_SLIDING_WINDOW);;

      if(version == IPVERSION) {
	inet_ntop(AF_INET, &newflow->src_ip, newflow->src_name, sizeof(newflow->src_name));
	inet_ntop(AF_INET, &newflow->dst_ip, newflow->dst_name, sizeof(newflow->dst_name));
      } else {
	inet_ntop(AF_INET6, &iph6->ip6_src, newflow->src_name, sizeof(newflow->src_name));
	inet_ntop(AF_INET6, &iph6->ip6_dst, newflow->dst_name, sizeof(newflow->dst_name));
	/* For consistency across platforms replace :0: with :: */
	ndpi_patchIPv6Address(newflow->src_name), ndpi_patchIPv6Address(newflow->dst_name);
      }

      if((newflow->ndpi_flow = ndpi_flow_malloc(SIZEOF_FLOW_STRUCT)) == NULL) {
	NDPI_LOG(0, workflow->ndpi_struct, NDPI_LOG_ERROR, "[NDPI] %s(2): not enough memory\n", __FUNCTION__);
	free(newflow);
	return(NULL);
      } else
	memset(newflow->ndpi_flow, 0, SIZEOF_FLOW_STRUCT);

      if((newflow->src_id = ndpi_malloc(SIZEOF_ID_STRUCT)) == NULL) {
	NDPI_LOG(0, workflow->ndpi_struct, NDPI_LOG_ERROR, "[NDPI] %s(3): not enough memory\n", __FUNCTION__);
	free(newflow);
	return(NULL);
      } else
	memset(newflow->src_id, 0, SIZEOF_ID_STRUCT);

      if((newflow->dst_id = ndpi_malloc(SIZEOF_ID_STRUCT)) == NULL) {
	NDPI_LOG(0, workflow->ndpi_struct, NDPI_LOG_ERROR, "[NDPI] %s(4): not enough memory\n", __FUNCTION__);
	free(newflow);
	return(NULL);
      } else
	memset(newflow->dst_id, 0, SIZEOF_ID_STRUCT);

      ndpi_tsearch(newflow, &workflow->ndpi_flows_root[idx], ndpi_workflow_node_cmp); /* Add */
      workflow->stats.ndpi_flow_count++;

      *src = newflow->src_id, *dst = newflow->dst_id;
      newflow->entropy.src2dst_pkt_len[newflow->entropy.src2dst_pkt_count] = l4_data_len;
      newflow->entropy.src2dst_pkt_time[newflow->entropy.src2dst_pkt_count] = when;
      if (newflow->entropy.src2dst_pkt_count == 0) {
        newflow->entropy.src2dst_start = when;
      }
      newflow->entropy.src2dst_pkt_count++;
      // Non zero app data.
      if (l4_data_len != 0XFEEDFACE && l4_data_len != 0) {
        newflow->entropy.src2dst_opackets++;
        newflow->entropy.src2dst_l4_bytes += l4_data_len;
      }
      return newflow;
    }
  } else {
    struct ndpi_flow_info *rflow = *(struct ndpi_flow_info**)ret;

    if(is_changed) {
      if(rflow->src_ip == iph->saddr
	 && rflow->dst_ip == iph->daddr
	 && rflow->src_port == htons(*sport)
	 && rflow->dst_port == htons(*dport)
	 )
	*src = rflow->dst_id, *dst = rflow->src_id, *src_to_dst_direction = 0, rflow->bidirectional = 1;
      else
	*src = rflow->src_id, *dst = rflow->dst_id, *src_to_dst_direction = 1;
    }
    else {
      if(rflow->src_ip == iph->saddr
	 && rflow->dst_ip == iph->daddr
	 && rflow->src_port == htons(*sport)
	 && rflow->dst_port == htons(*dport)
	 )
	*src = rflow->src_id, *dst = rflow->dst_id, *src_to_dst_direction = 1;
      else
	*src = rflow->dst_id, *dst = rflow->src_id, *src_to_dst_direction = 0, rflow->bidirectional = 1;
    }
    if (src_to_dst_direction) {
      if (rflow->entropy.src2dst_pkt_count < max_num_packets_per_flow) {
        rflow->entropy.src2dst_pkt_len[rflow->entropy.src2dst_pkt_count] = l4_data_len;
        rflow->entropy.src2dst_pkt_time[rflow->entropy.src2dst_pkt_count] = when;
        rflow->entropy.src2dst_l4_bytes += l4_data_len;
        rflow->entropy.src2dst_pkt_count++;
      }
      // Non zero app data.
      if (l4_data_len != 0XFEEDFACE && l4_data_len != 0) {
        rflow->entropy.src2dst_opackets++;
      }
    } else {
      if (rflow->entropy.dst2src_pkt_count < max_num_packets_per_flow) {
        rflow->entropy.dst2src_pkt_len[rflow->entropy.dst2src_pkt_count] = l4_data_len;
        rflow->entropy.dst2src_pkt_time[rflow->entropy.dst2src_pkt_count] = when;
        if (rflow->entropy.dst2src_pkt_count == 0) {
          rflow->entropy.dst2src_start = when;
        }
        rflow->entropy.dst2src_l4_bytes += l4_data_len;
        rflow->entropy.dst2src_pkt_count++;
      }
      // Non zero app data.
      if (l4_data_len != 0XFEEDFACE && l4_data_len != 0) {
        rflow->entropy.dst2src_opackets++;
      }
    }
    
    return(rflow);
  }
}

/* ****************************************************** */

static struct ndpi_flow_info *get_ndpi_flow_info6(struct ndpi_workflow * workflow,
						  u_int16_t vlan_id,
						  const struct ndpi_ipv6hdr *iph6,
						  u_int16_t ip_offset,
						  struct ndpi_tcphdr **tcph,
						  struct ndpi_udphdr **udph,
						  u_int16_t *sport, u_int16_t *dport,
						  struct ndpi_id_struct **src,
						  struct ndpi_id_struct **dst,
						  u_int8_t *proto,
						  u_int8_t **payload,
						  u_int16_t *payload_len,
						  u_int8_t *src_to_dst_direction,
                                                  struct timeval when) {
  struct ndpi_iphdr iph;

  memset(&iph, 0, sizeof(iph));
  iph.version = IPVERSION;
  iph.saddr = iph6->ip6_src.u6_addr.u6_addr32[2] + iph6->ip6_src.u6_addr.u6_addr32[3];
  iph.daddr = iph6->ip6_dst.u6_addr.u6_addr32[2] + iph6->ip6_dst.u6_addr.u6_addr32[3];
  iph.protocol = iph6->ip6_hdr.ip6_un1_nxt;

  if(iph.protocol == IPPROTO_DSTOPTS /* IPv6 destination option */) {
    const u_int8_t *options = (const u_int8_t*)iph6 + sizeof(const struct ndpi_ipv6hdr);

    iph.protocol = options[0];
  }

  return(get_ndpi_flow_info(workflow, 6, vlan_id, &iph, iph6, ip_offset,
			    sizeof(struct ndpi_ipv6hdr),
			    ntohs(iph6->ip6_hdr.ip6_un1_plen),
			    tcph, udph, sport, dport,
			    src, dst, proto, payload,
			    payload_len, src_to_dst_direction, when));
}

/* ****************************************************** */

void process_ndpi_collected_info(struct ndpi_workflow * workflow, struct ndpi_flow_info *flow) {
  if(!flow->ndpi_flow) return;

  snprintf(flow->host_server_name, sizeof(flow->host_server_name), "%s",
	   flow->ndpi_flow->host_server_name);

  if(flow->detected_protocol.app_protocol == NDPI_PROTOCOL_DHCP) {
    snprintf(flow->dhcp_fingerprint, sizeof(flow->dhcp_fingerprint), "%s", flow->ndpi_flow->protos.dhcp.fingerprint);
  } else if(flow->detected_protocol.app_protocol == NDPI_PROTOCOL_BITTORRENT) {
    u_int i, j, n = 0;

    for(i=0, j = 0; j < sizeof(flow->bittorent_hash)-1; i++) {
      sprintf(&flow->bittorent_hash[j], "%02x",
	      flow->ndpi_flow->protos.bittorrent.hash[i]);

      j += 2, n += flow->ndpi_flow->protos.bittorrent.hash[i];
    }

    if(n == 0) flow->bittorent_hash[0] = '\0';
  }
  /* MDNS */
  else if(flow->detected_protocol.app_protocol == NDPI_PROTOCOL_MDNS) {
    snprintf(flow->info, sizeof(flow->info), "%s", flow->ndpi_flow->protos.mdns.answer);
  }
  /* UBNTAC2 */
  else if(flow->detected_protocol.app_protocol == NDPI_PROTOCOL_UBNTAC2) {
    snprintf(flow->info, sizeof(flow->info), "%s", flow->ndpi_flow->protos.ubntac2.version);
  }
  /* FTP */
  else if((flow->detected_protocol.app_protocol == NDPI_PROTOCOL_FTP_CONTROL)
	  || /* IMAP */ (flow->detected_protocol.app_protocol == NDPI_PROTOCOL_MAIL_IMAP)
	  || /* POP */  (flow->detected_protocol.app_protocol == NDPI_PROTOCOL_MAIL_POP)
	  || /* SMTP */ (flow->detected_protocol.app_protocol == NDPI_PROTOCOL_MAIL_SMTP)) {
    if(flow->ndpi_flow->protos.ftp_imap_pop_smtp.username[0] != '\0')
      snprintf(flow->info, sizeof(flow->info), "User: %s][Pwd: %s",
	       flow->ndpi_flow->protos.ftp_imap_pop_smtp.username,
	       flow->ndpi_flow->protos.ftp_imap_pop_smtp.password);
  }
  /* KERBEROS */
  else if(flow->detected_protocol.app_protocol == NDPI_PROTOCOL_KERBEROS) {
    if(flow->ndpi_flow->protos.kerberos.cname[0] != '\0') {
      snprintf(flow->info, sizeof(flow->info), "%s (%s)",
	       flow->ndpi_flow->protos.kerberos.cname,
	       flow->ndpi_flow->protos.kerberos.realm);
    }
  }
  /* HTTP */
  else if(flow->detected_protocol.master_protocol == NDPI_PROTOCOL_HTTP) {
    if(flow->ndpi_flow->http.url != NULL) {
      snprintf(flow->http.url, sizeof(flow->http.url), "%s", flow->ndpi_flow->http.url);
      flow->http.response_status_code = flow->ndpi_flow->http.response_status_code;
    }
  }
  else if(flow->detected_protocol.app_protocol != NDPI_PROTOCOL_DNS) {
    /* SSH */
    if(flow->detected_protocol.app_protocol == NDPI_PROTOCOL_SSH) {
      snprintf(flow->ssh_tls.client_info, sizeof(flow->ssh_tls.client_info), "%s",
	       flow->ndpi_flow->protos.ssh.client_signature);
      snprintf(flow->ssh_tls.server_info, sizeof(flow->ssh_tls.server_info), "%s",
	       flow->ndpi_flow->protos.ssh.server_signature);
      snprintf(flow->ssh_tls.client_hassh, sizeof(flow->ssh_tls.client_hassh), "%s",
	       flow->ndpi_flow->protos.ssh.hassh_client);
      snprintf(flow->ssh_tls.server_hassh, sizeof(flow->ssh_tls.server_hassh), "%s",
	       flow->ndpi_flow->protos.ssh.hassh_server);
    }
    /* TLS */
    else if((flow->detected_protocol.app_protocol == NDPI_PROTOCOL_TLS)
	    || (flow->detected_protocol.master_protocol == NDPI_PROTOCOL_TLS)
	    || (flow->ndpi_flow->protos.stun_ssl.ssl.ja3_client[0] != '\0')
	    ) {
      flow->ssh_tls.ssl_version = flow->ndpi_flow->protos.stun_ssl.ssl.ssl_version;
      snprintf(flow->ssh_tls.client_info, sizeof(flow->ssh_tls.client_info), "%s",
	       flow->ndpi_flow->protos.stun_ssl.ssl.client_certificate);
      snprintf(flow->ssh_tls.server_info, sizeof(flow->ssh_tls.server_info), "%s",
	       flow->ndpi_flow->protos.stun_ssl.ssl.server_certificate);
      snprintf(flow->ssh_tls.server_organization, sizeof(flow->ssh_tls.server_organization), "%s",
	       flow->ndpi_flow->protos.stun_ssl.ssl.server_organization);
      flow->ssh_tls.notBefore = flow->ndpi_flow->protos.stun_ssl.ssl.notBefore;
      flow->ssh_tls.notAfter = flow->ndpi_flow->protos.stun_ssl.ssl.notAfter;
      snprintf(flow->ssh_tls.ja3_client, sizeof(flow->ssh_tls.ja3_client), "%s",
	       flow->ndpi_flow->protos.stun_ssl.ssl.ja3_client);
      snprintf(flow->ssh_tls.ja3_server, sizeof(flow->ssh_tls.ja3_server), "%s",
	       flow->ndpi_flow->protos.stun_ssl.ssl.ja3_server);
      flow->ssh_tls.server_unsafe_cipher = flow->ndpi_flow->protos.stun_ssl.ssl.server_unsafe_cipher;
      flow->ssh_tls.server_cipher = flow->ndpi_flow->protos.stun_ssl.ssl.server_cipher;
      memcpy(flow->ssh_tls.sha1_cert_fingerprint,
	     flow->ndpi_flow->l4.tcp.tls_sha1_certificate_fingerprint, 20);
    }
  }

  if(flow->detection_completed && (!flow->check_extra_packets)) {
    if(flow->detected_protocol.app_protocol == NDPI_PROTOCOL_UNKNOWN) {
      if(workflow->__flow_giveup_callback != NULL)
	workflow->__flow_giveup_callback(workflow, flow, workflow->__flow_giveup_udata);
    } else {
      if(workflow->__flow_detected_callback != NULL)
	workflow->__flow_detected_callback(workflow, flow, workflow->__flow_detected_udata);
    }

    ndpi_free_flow_info_half(flow);
  }
}

/* ****************************************************** */

/**
 * @brief Clear entropy stats if it meets prereq.
 */
static void
ndpi_clear_entropy_stats(struct ndpi_flow_info *flow) {
  if(flow->entropy.src2dst_pkt_count + flow->entropy.dst2src_pkt_count == max_num_packets_per_flow) {
    memcpy(&flow->last_entropy, &flow->entropy,  sizeof(struct ndpi_entropy));
    memset(&flow->entropy, 0x00, sizeof(struct ndpi_entropy));
  }
}

/* ****************************************************** */
/**
   Function to process the packet:
   determine the flow of a packet and try to decode it
   @return: 0 if success; else != 0

   @Note: ipsize = header->len - ip_offset ; rawsize = header->len
*/
static struct ndpi_proto packet_processing(struct ndpi_workflow * workflow,
					   const u_int64_t time,
					   u_int16_t vlan_id,
					   const struct ndpi_iphdr *iph,
					   struct ndpi_ipv6hdr *iph6,
					   u_int16_t ip_offset,
					   u_int16_t ipsize, u_int16_t rawsize,
					   const struct pcap_pkthdr *header,
					   const u_char *packet,
                                           struct timeval when) {
  struct ndpi_id_struct *src, *dst;
  struct ndpi_flow_info *flow = NULL;
  struct ndpi_flow_struct *ndpi_flow = NULL;
  u_int8_t proto;
  struct ndpi_tcphdr *tcph = NULL;
  struct ndpi_udphdr *udph = NULL;
  u_int16_t sport, dport, payload_len;
  u_int8_t *payload;
  u_int8_t src_to_dst_direction = 1;
  u_int8_t begin_or_end_tcp = 0;
  struct ndpi_proto nproto = { NDPI_PROTOCOL_UNKNOWN, NDPI_PROTOCOL_UNKNOWN };

  if(iph)
    flow = get_ndpi_flow_info(workflow, IPVERSION, vlan_id, iph, NULL,
			      ip_offset, ipsize,
			      ntohs(iph->tot_len) - (iph->ihl * 4),
			      &tcph, &udph, &sport, &dport,
			      &src, &dst, &proto,
			      &payload, &payload_len, &src_to_dst_direction, when);
  else
    flow = get_ndpi_flow_info6(workflow, vlan_id, iph6, ip_offset,
			       &tcph, &udph, &sport, &dport,
			       &src, &dst, &proto,
			       &payload, &payload_len, &src_to_dst_direction, when);

  if(flow != NULL) {
    struct timeval tdiff;

    workflow->stats.ip_packet_count++;
    workflow->stats.total_wire_bytes += rawsize + 24 /* CRC etc */,
      workflow->stats.total_ip_bytes += rawsize;
    ndpi_flow = flow->ndpi_flow;

    if((tcph != NULL) && (tcph->fin || tcph->rst || tcph->syn))
      begin_or_end_tcp = 1;

    if(flow->entropy.flow_last_pkt_time.tv_sec) {
      ndpi_timer_sub(&when, &flow->entropy.flow_last_pkt_time, &tdiff);
    
      if(flow->iat_flow) {
	u_int32_t ms = ndpi_timeval_to_milliseconds(tdiff);

	if(ms > 0)
	  ndpi_data_add_value(flow->iat_flow, ms);
      }
    }
    memcpy(&flow->entropy.flow_last_pkt_time, &when, sizeof(when));

    if(src_to_dst_direction) {
      if(flow->entropy.src2dst_last_pkt_time.tv_sec) {
	ndpi_timer_sub(&when, &flow->entropy.src2dst_last_pkt_time, &tdiff);

	if(flow->iat_c_to_s) {
	  u_int32_t ms = ndpi_timeval_to_milliseconds(tdiff);

	  ndpi_data_add_value(flow->iat_c_to_s, ms);
	}
      }

      ndpi_data_add_value(flow->pktlen_c_to_s, rawsize);
      flow->src2dst_packets++, flow->src2dst_bytes += rawsize;
      memcpy(&flow->entropy.src2dst_last_pkt_time, &when, sizeof(when));
    } else {
      if(flow->entropy.dst2src_last_pkt_time.tv_sec && (!begin_or_end_tcp)) {
	ndpi_timer_sub(&when, &flow->entropy.dst2src_last_pkt_time, &tdiff);

	if(flow->iat_s_to_c) {
	  u_int32_t ms = ndpi_timeval_to_milliseconds(tdiff);

	  ndpi_data_add_value(flow->iat_s_to_c, ms);
	}
      }

      ndpi_data_add_value(flow->pktlen_s_to_c, rawsize);
      flow->dst2src_packets++, flow->dst2src_bytes += rawsize;
      memcpy(&flow->entropy.dst2src_last_pkt_time, &when, sizeof(when));
    }

    if(enable_payload_analyzer && (payload_len > 0))
      ndpi_payload_analyzer(flow, src_to_dst_direction,
			    payload, payload_len,
			    workflow->stats.ip_packet_count);

    if(enable_joy_stats) {
      /* Update BD, distribution and mean. */
      ndpi_flow_update_byte_count(flow, payload, payload_len, src_to_dst_direction);
      ndpi_flow_update_byte_dist_mean_var(flow, payload, payload_len, src_to_dst_direction);
      /* Update SPLT scores for first 32 packets. */
      if((flow->entropy.src2dst_pkt_count+flow->entropy.dst2src_pkt_count) <= max_num_packets_per_flow) {
        if(flow->bidirectional)
          flow->entropy.score = ndpi_classify(flow->entropy.src2dst_pkt_len, flow->entropy.src2dst_pkt_time,
				     flow->entropy.dst2src_pkt_len, flow->entropy.dst2src_pkt_time,
				     flow->entropy.src2dst_start, flow->entropy.dst2src_start,
				     max_num_packets_per_flow, flow->src_port, flow->dst_port,
				     flow->src2dst_packets, flow->dst2src_packets,
				     flow->entropy.src2dst_opackets, flow->entropy.dst2src_opackets,
				     flow->entropy.src2dst_l4_bytes, flow->entropy.dst2src_l4_bytes, 1,
				     flow->entropy.src2dst_byte_count, flow->entropy.dst2src_byte_count);
       else
         flow->entropy.score = ndpi_classify(flow->entropy.src2dst_pkt_len, flow->entropy.src2dst_pkt_time,
				     NULL, NULL, flow->entropy.src2dst_start, flow->entropy.src2dst_start,
				     max_num_packets_per_flow, flow->src_port, flow->dst_port,
				     flow->src2dst_packets, 0,
				     flow->entropy.src2dst_opackets, 0,
				     flow->entropy.src2dst_l4_bytes, 0, 1,
				     flow->entropy.src2dst_byte_count, NULL);
      }
    }

    if(flow->first_seen == 0)
      flow->first_seen = time;

    flow->last_seen = time;

    /* Copy packets entropy if num packets count == 10 */
    ndpi_clear_entropy_stats(flow);

    if(!flow->has_human_readeable_strings) {
      u_int8_t skip = 0;

      if((proto == IPPROTO_TCP)
	 && (
	     (flow->detected_protocol.app_protocol == NDPI_PROTOCOL_TLS)
	     || (flow->detected_protocol.master_protocol == NDPI_PROTOCOL_TLS)
	     || (flow->detected_protocol.app_protocol == NDPI_PROTOCOL_SSH)
	     || (flow->detected_protocol.master_protocol == NDPI_PROTOCOL_SSH))
	 ) {
	if((flow->src2dst_packets+flow->dst2src_packets) < 10 /* MIN_NUM_ENCRYPT_SKIP_PACKETS */)
	  skip = 1;
      }

      if(!skip) {
	if(ndpi_has_human_readeable_string(workflow->ndpi_struct, (char*)packet, header->caplen,
					   human_readeable_string_len,
					   flow->human_readeable_string_buffer,
					   sizeof(flow->human_readeable_string_buffer)) == 1)
	  flow->has_human_readeable_strings = 1;
      }
    } else {
      if((proto == IPPROTO_TCP)
	 && (
	     (flow->detected_protocol.app_protocol == NDPI_PROTOCOL_TLS)
	     || (flow->detected_protocol.master_protocol == NDPI_PROTOCOL_TLS)
	     || (flow->detected_protocol.app_protocol == NDPI_PROTOCOL_SSH)
	     || (flow->detected_protocol.master_protocol == NDPI_PROTOCOL_SSH))
	 )
	flow->has_human_readeable_strings = 0;
    }
  } else { // flow is NULL
    workflow->stats.total_discarded_bytes++;
    return(nproto);
  }

  if(!flow->detection_completed) {
    u_int enough_packets =
      (((proto == IPPROTO_UDP) && ((flow->src2dst_packets + flow->dst2src_packets) > max_num_udp_dissected_pkts))
       || ((proto == IPPROTO_TCP) && ((flow->src2dst_packets + flow->dst2src_packets) > max_num_tcp_dissected_pkts))) ? 1 : 0;
    
#if 0
    printf("%s()\n", __FUNCTION__);  
#endif
  
    flow->detected_protocol = ndpi_detection_process_packet(workflow->ndpi_struct, ndpi_flow,
							    iph ? (uint8_t *)iph : (uint8_t *)iph6,
							    ipsize, time, src, dst);

    if(enough_packets || (flow->detected_protocol.app_protocol != NDPI_PROTOCOL_UNKNOWN)) {
      if((!enough_packets)
	 && ndpi_extra_dissection_possible(workflow->ndpi_struct, ndpi_flow))
	; /* Wait for certificate fingerprint */
      else {
	/* New protocol detected or give up */
	flow->detection_completed = 1;

#if 0
	/* Check if we should keep checking extra packets */
	if(ndpi_flow && ndpi_flow->check_extra_packets)
	  flow->check_extra_packets = 1;
#endif
	
	if(flow->detected_protocol.app_protocol == NDPI_PROTOCOL_UNKNOWN) {
	  u_int8_t proto_guessed;
	  
	  flow->detected_protocol = ndpi_detection_giveup(workflow->ndpi_struct, flow->ndpi_flow,
							  enable_protocol_guess, &proto_guessed);
	}
	
	process_ndpi_collected_info(workflow, flow);
      }
    }
  }

  return(flow->detected_protocol);
}

/* ****************************************************** */

struct ndpi_proto ndpi_workflow_process_packet(struct ndpi_workflow * workflow,
					       const struct pcap_pkthdr *header,
					       const u_char *packet) {
  /*
   * Declare pointers to packet headers
   */
  /* --- Ethernet header --- */
  const struct ndpi_ethhdr *ethernet;
  /* --- LLC header --- */
  const struct ndpi_llc_header_snap *llc;

  /* --- Cisco HDLC header --- */
  const struct ndpi_chdlc *chdlc;

  /* --- Radio Tap header --- */
  const struct ndpi_radiotap_header *radiotap;
  /* --- Wifi header --- */
  const struct ndpi_wifi_header *wifi;

  /* --- MPLS header --- */
  union mpls {
    uint32_t u32;
    struct ndpi_mpls_header mpls;
  } mpls;

  /** --- IP header --- **/
  struct ndpi_iphdr *iph;
  /** --- IPv6 header --- **/
  struct ndpi_ipv6hdr *iph6;

  struct ndpi_proto nproto = { NDPI_PROTOCOL_UNKNOWN, NDPI_PROTOCOL_UNKNOWN };

  /* lengths and offsets */
  u_int16_t eth_offset = 0;
  u_int16_t radio_len;
  u_int16_t fc;
  u_int16_t type = 0;
  int wifi_len = 0;
  int pyld_eth_len = 0;
  int check;
  u_int64_t time;
  u_int16_t ip_offset = 0, ip_len;
  u_int16_t frag_off = 0, vlan_id = 0;
  u_int8_t proto = 0;
  /*u_int32_t label;*/

  /* counters */
  u_int8_t vlan_packet = 0;

  /* Increment raw packet counter */
  workflow->stats.raw_packet_count++;

  /* setting time */
  time = ((uint64_t) header->ts.tv_sec) * TICK_RESOLUTION + header->ts.tv_usec / (1000000 / TICK_RESOLUTION);

  /* safety check */
  if(workflow->last_time > time) {
    /* printf("\nWARNING: timestamp bug in the pcap file (ts delta: %llu, repairing)\n", ndpi_thread_info[thread_id].last_time - time); */
    time = workflow->last_time;
  }
  /* update last time value */
  workflow->last_time = time;

  /*** check Data Link type ***/
  int datalink_type;

#ifdef USE_DPDK
  datalink_type = DLT_EN10MB;
#else
  datalink_type = (int)pcap_datalink(workflow->pcap_handle);
#endif

 datalink_check:
  switch(datalink_type) {
  case DLT_NULL:
    if(ntohl(*((u_int32_t*)&packet[eth_offset])) == 2)
      type = ETH_P_IP;
    else
      type = ETH_P_IPV6;

    ip_offset = 4 + eth_offset;
    break;

    /* Cisco PPP in HDLC-like framing - 50 */
  case DLT_PPP_SERIAL:
    chdlc = (struct ndpi_chdlc *) &packet[eth_offset];
    ip_offset = sizeof(struct ndpi_chdlc); /* CHDLC_OFF = 4 */
    type = ntohs(chdlc->proto_code);
    break;

    /* Cisco PPP - 9 or 104 */
  case DLT_C_HDLC:
  case DLT_PPP:
    chdlc = (struct ndpi_chdlc *) &packet[eth_offset];
    ip_offset = sizeof(struct ndpi_chdlc); /* CHDLC_OFF = 4 */
    type = ntohs(chdlc->proto_code);
    break;

    /* IEEE 802.3 Ethernet - 1 */
  case DLT_EN10MB:
    ethernet = (struct ndpi_ethhdr *) &packet[eth_offset];
    ip_offset = sizeof(struct ndpi_ethhdr) + eth_offset;
    check = ntohs(ethernet->h_proto);

    if(check <= 1500)
      pyld_eth_len = check;
    else if(check >= 1536)
      type = check;

    if(pyld_eth_len != 0) {
      llc = (struct ndpi_llc_header_snap *)(&packet[ip_offset]);
      /* check for LLC layer with SNAP extension */
      if(llc->dsap == SNAP || llc->ssap == SNAP) {
	type = llc->snap.proto_ID;
	ip_offset += + 8;
      }
      /* No SNAP extension - Spanning Tree pkt must be discarted */
      else if(llc->dsap == BSTP || llc->ssap == BSTP) {
	goto v4_warning;
      }
    }
    break;

    /* Linux Cooked Capture - 113 */
  case DLT_LINUX_SLL:
    type = (packet[eth_offset+14] << 8) + packet[eth_offset+15];
    ip_offset = 16 + eth_offset;
    break;

    /* Radiotap link-layer - 127 */
  case DLT_IEEE802_11_RADIO:
    radiotap = (struct ndpi_radiotap_header *) &packet[eth_offset];
    radio_len = radiotap->len;

    /* Check Bad FCS presence */
    if((radiotap->flags & BAD_FCS) == BAD_FCS) {
      workflow->stats.total_discarded_bytes +=  header->len;
      return(nproto);
    }

    /* Calculate 802.11 header length (variable) */
    wifi = (struct ndpi_wifi_header*)( packet + eth_offset + radio_len);
    fc = wifi->fc;

    /* check wifi data presence */
    if(FCF_TYPE(fc) == WIFI_DATA) {
      if((FCF_TO_DS(fc) && FCF_FROM_DS(fc) == 0x0) ||
	 (FCF_TO_DS(fc) == 0x0 && FCF_FROM_DS(fc)))
	wifi_len = 26; /* + 4 byte fcs */
    } else   /* no data frames */
      break;

    /* Check ether_type from LLC */
    llc = (struct ndpi_llc_header_snap*)(packet + eth_offset + wifi_len + radio_len);
    if(llc->dsap == SNAP)
      type = ntohs(llc->snap.proto_ID);

    /* Set IP header offset */
    ip_offset = wifi_len + radio_len + sizeof(struct ndpi_llc_header_snap) + eth_offset;
    break;

  case DLT_RAW:
    ip_offset = eth_offset = 0;
    break;

  default:
    /* printf("Unknown datalink %d\n", datalink_type); */
    return(nproto);
  }

  /* check ether type */
  switch(type) {
  case VLAN:
    vlan_id = ((packet[ip_offset] << 8) + packet[ip_offset+1]) & 0xFFF;
    type = (packet[ip_offset+2] << 8) + packet[ip_offset+3];
    ip_offset += 4;
    vlan_packet = 1;
    // double tagging for 802.1Q
    if(type == 0x8100) {
      vlan_id = ((packet[ip_offset] << 8) + packet[ip_offset+1]) & 0xFFF;
      type = (packet[ip_offset+2] << 8) + packet[ip_offset+3];
      ip_offset += 4;
    }
    break;
  case MPLS_UNI:
  case MPLS_MULTI:
    mpls.u32 = *((uint32_t *) &packet[ip_offset]);
    mpls.u32 = ntohl(mpls.u32);
    workflow->stats.mpls_count++;
    type = ETH_P_IP, ip_offset += 4;

    while(!mpls.mpls.s) {
      mpls.u32 = *((uint32_t *) &packet[ip_offset]);
      mpls.u32 = ntohl(mpls.u32);
      ip_offset += 4;
    }
    break;
  case PPPoE:
    workflow->stats.pppoe_count++;
    type = ETH_P_IP;
    ip_offset += 8;
    break;
  default:
    break;
  }

  workflow->stats.vlan_count += vlan_packet;

 iph_check:
  /* Check and set IP header size and total packet length */
  iph = (struct ndpi_iphdr *) &packet[ip_offset];

  /* just work on Ethernet packets that contain IP */
  if(type == ETH_P_IP && header->caplen >= ip_offset) {
    frag_off = ntohs(iph->frag_off);

    proto = iph->protocol;
    if(header->caplen < header->len) {
      static u_int8_t cap_warning_used = 0;

      if(cap_warning_used == 0) {
	if(!workflow->prefs.quiet_mode)
	  NDPI_LOG(0, workflow->ndpi_struct, NDPI_LOG_DEBUG, "\n\nWARNING: packet capture size is smaller than packet size, DETECTION MIGHT NOT WORK CORRECTLY\n\n");
	cap_warning_used = 1;
      }
    }
  }

  if(iph->version == IPVERSION) {
    ip_len = ((u_int16_t)iph->ihl * 4);
    iph6 = NULL;

    if(iph->protocol == IPPROTO_IPV6) {
      ip_offset += ip_len;
      goto iph_check;
    }

    if((frag_off & 0x1FFF) != 0) {
      static u_int8_t ipv4_frags_warning_used = 0;
      workflow->stats.fragmented_count++;

      if(ipv4_frags_warning_used == 0) {
	if(!workflow->prefs.quiet_mode)
	  NDPI_LOG(0, workflow->ndpi_struct, NDPI_LOG_DEBUG, "\n\nWARNING: IPv4 fragments are not handled by this demo (nDPI supports them)\n");
	ipv4_frags_warning_used = 1;
      }

      workflow->stats.total_discarded_bytes +=  header->len;
      return(nproto);
    }
  } else if(iph->version == 6) {
    iph6 = (struct ndpi_ipv6hdr *)&packet[ip_offset];
    proto = iph6->ip6_hdr.ip6_un1_nxt;
    ip_len = sizeof(struct ndpi_ipv6hdr);

    if(proto == IPPROTO_DSTOPTS /* IPv6 destination option */) {
      u_int8_t *options = (u_int8_t*)&packet[ip_offset+ip_len];
      proto = options[0];
      ip_len += 8 * (options[1] + 1);
    }

    iph = NULL;
  } else {
    static u_int8_t ipv4_warning_used = 0;

  v4_warning:
    if(ipv4_warning_used == 0) {
      if(!workflow->prefs.quiet_mode)
        NDPI_LOG(0, workflow->ndpi_struct, NDPI_LOG_DEBUG,
		 "\n\nWARNING: only IPv4/IPv6 packets are supported in this demo (nDPI supports both IPv4 and IPv6), all other packets will be discarded\n\n");
      ipv4_warning_used = 1;
    }
    workflow->stats.total_discarded_bytes +=  header->len;
    return(nproto);
  }

  if(workflow->prefs.decode_tunnels && (proto == IPPROTO_UDP)) {
    struct ndpi_udphdr *udp = (struct ndpi_udphdr *)&packet[ip_offset+ip_len];
    u_int16_t sport = ntohs(udp->source), dport = ntohs(udp->dest);

    if((sport == GTP_U_V1_PORT) || (dport == GTP_U_V1_PORT)) {
      /* Check if it's GTPv1 */
      u_int offset = ip_offset+ip_len+sizeof(struct ndpi_udphdr);
      u_int8_t flags = packet[offset];
      u_int8_t message_type = packet[offset+1];

      if((((flags & 0xE0) >> 5) == 1 /* GTPv1 */) &&
	 (message_type == 0xFF /* T-PDU */)) {

	ip_offset = ip_offset+ip_len+sizeof(struct ndpi_udphdr)+8; /* GTPv1 header len */
	if(flags & 0x04) ip_offset += 1; /* next_ext_header is present */
	if(flags & 0x02) ip_offset += 4; /* sequence_number is present (it also includes next_ext_header and pdu_number) */
	if(flags & 0x01) ip_offset += 1; /* pdu_number is present */

	iph = (struct ndpi_iphdr *) &packet[ip_offset];

	if(iph->version != IPVERSION) {
	  // printf("WARNING: not good (packet_id=%u)!\n", (unsigned int)workflow->stats.raw_packet_count);
	  goto v4_warning;
	}
      }
    } else if((sport == TZSP_PORT) || (dport == TZSP_PORT)) {
      /* https://en.wikipedia.org/wiki/TZSP */
      u_int offset = ip_offset+ip_len+sizeof(struct ndpi_udphdr);
      u_int8_t version = packet[offset];
      u_int8_t ts_type    = packet[offset+1];
      u_int16_t encapsulates = ntohs(*((u_int16_t*)&packet[offset+2]));

      if((version == 1) && (ts_type == 0) && (encapsulates == 1)) {
	u_int8_t stop = 0;

	offset += 4;

	while((!stop) && (offset < header->caplen)) {
	  u_int8_t tag_type = packet[offset];
	  u_int8_t tag_len;

	  switch(tag_type) {
	  case 0: /* PADDING Tag */
	    tag_len = 1;
	    break;
	  case 1: /* END Tag */
	    tag_len = 1, stop = 1;
	    break;
	  default:
	    tag_len = packet[offset+1];
	    break;
	  }

	  offset += tag_len;

	  if(offset >= header->caplen)
	    return(nproto); /* Invalid packet */
	  else {
	    eth_offset = offset;
	    goto datalink_check;
	  }
	}
      }
    }
  }

  /* process the packet */
  return(packet_processing(workflow, time, vlan_id, iph, iph6,
			   ip_offset, header->caplen - ip_offset,
			   header->caplen, header, packet, header->ts));
}

/* ********************************************************** */
/*       http://home.thep.lu.se/~bjorn/crc/crc32_fast.c       */
/* ********************************************************** */

static uint32_t crc32_for_byte(uint32_t r) {
  int j;
  for(j = 0; j < 8; ++j)
    r = ((r & 1) ? 0 : (uint32_t)0xEDB88320L) ^ r >> 1;
  return r ^ (uint32_t)0xFF000000L;
}

/* Any unsigned integer type with at least 32 bits may be used as
 * accumulator type for fast crc32-calulation, but unsigned long is
 * probably the optimal choice for most systems. */
typedef unsigned long accum_t;

static void init_tables(uint32_t* table, uint32_t* wtable) {
  size_t i, j, k, w;
  for(i = 0; i < 0x100; ++i)
    table[i] = crc32_for_byte(i);
  for(k = 0; k < sizeof(accum_t); ++k)
    for(i = 0; i < 0x100; ++i) {
      for(j = w = 0; j < sizeof(accum_t); ++j)
	w = table[(uint8_t)(j == k? w ^ i: w)] ^ w >> 8;
      wtable[(k << 8) + i] = w ^ (k? wtable[0]: 0);
    }
}

static void __crc32(const void* data, size_t n_bytes, uint32_t* crc) {
  static uint32_t table[0x100], wtable[0x100*sizeof(accum_t)];
  size_t n_accum = n_bytes/sizeof(accum_t);
  size_t i, j;
  if(!*table)
    init_tables(table, wtable);
  for(i = 0; i < n_accum; ++i) {
    accum_t a = *crc ^ ((accum_t*)data)[i];
    for(j = *crc = 0; j < sizeof(accum_t); ++j)
      *crc ^= wtable[(j << 8) + (uint8_t)(a >> 8*j)];
  }
  for(i = n_accum*sizeof(accum_t); i < n_bytes; ++i)
    *crc = table[(uint8_t)*crc ^ ((uint8_t*)data)[i]] ^ *crc >> 8;
}

u_int32_t ethernet_crc32(const void* data, size_t n_bytes) {
  u_int32_t crc = 0;
  __crc32(data, n_bytes, &crc);
  return crc;
}

/* *********************************************** */

#ifdef USE_DPDK

static const struct rte_eth_conf port_conf_default = {
						      .rxmode = { .max_rx_pkt_len = ETHER_MAX_LEN }
};

/* ************************************ */

int dpdk_port_init(int port, struct rte_mempool *mbuf_pool) {
  struct rte_eth_conf port_conf = port_conf_default;
  const u_int16_t rx_rings = 1, tx_rings = 1;
  int retval;
  u_int16_t q;

  /* 1 RX queue */
  retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);

  if(retval != 0)
    return retval;

  for(q = 0; q < rx_rings; q++) {
    retval = rte_eth_rx_queue_setup(port, q, RX_RING_SIZE, rte_eth_dev_socket_id(port), NULL, mbuf_pool);
    if(retval < 0)
      return retval;
  }

  for(q = 0; q < tx_rings; q++) {
    retval = rte_eth_tx_queue_setup(port, q, TX_RING_SIZE, rte_eth_dev_socket_id(port), NULL);
    if(retval < 0)
      return retval;
  }

  retval = rte_eth_dev_start(port);

  if(retval < 0)
    return retval;

  rte_eth_promiscuous_enable(port);

  return 0;
}

#endif
