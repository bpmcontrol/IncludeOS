// This file is a part of the IncludeOS unikernel - www.includeos.org
//
// Copyright 2015 Oslo and Akershus University College of Applied Sciences
// and Alfred Bratterud
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#define DEBUG  // Allow debugging
#define DEBUG2 // Allow debugging

#include <vector>

#include <net/inet4.hpp>
#include <net/ip4/arp.hpp>
#include <net/ip4/packet_arp.hpp>
#include <statman>

using namespace std::chrono;

namespace net {

  // Initialize
  Arp::Arp(Stack& inet) noexcept:
  requests_rx_    {Statman::get().create(Stat::UINT32, inet.ifname() + ".arp.requests_rx").get_uint32()},
  requests_tx_    {Statman::get().create(Stat::UINT32, inet.ifname() + ".arp.requests_tx").get_uint32()},
  replies_rx_     {Statman::get().create(Stat::UINT32, inet.ifname() + ".arp.replies_rx").get_uint32()},
  replies_tx_     {Statman::get().create(Stat::UINT32, inet.ifname() + ".arp.replies_tx").get_uint32()},
  inet_           {inet},
  mac_            (inet.link_addr())
  {}

  void Arp::receive(Packet_ptr pckt) {
    debug("<ARP handler> got %i bytes of data\n", pckt->size());

    header* hdr = reinterpret_cast<header*>(pckt->layer_begin());

    /// cache entry
    this->cache(hdr->sipaddr, hdr->shwaddr);

    /// always try to ship waiting packets when someone talks
    auto waiting = waiting_packets_.find(hdr->sipaddr);
    if (waiting != waiting_packets_.end()) {
      debug("<Arp> Had a packet waiting for this IP. Sending\n");
      transmit(std::move(waiting->second), hdr->sipaddr);
      waiting_packets_.erase(waiting);
    }

    switch(hdr->opcode) {
    case H_request: {
      // Stat increment requests received
      requests_rx_++;

      debug("<Arp> %s is looking for %s\n",
             hdr->sipaddr.str().c_str(),
             hdr->dipaddr.str().c_str());

      if (hdr->dipaddr == inet_.ip_addr()) {

        // The packet is for us. Respond.
        arp_respond(hdr, inet_.ip_addr());

      } else if (proxy_ and proxy_(hdr->dipaddr)){

        // The packet is for an IP to which we know a route
        arp_respond(hdr, hdr->dipaddr);

      } else {

        // Drop
        debug2("\t NO MATCH for My IP (%s). DROP!\n",
               inet_.ip_addr().str().c_str());
      }
      break;
    }
    case H_reply: {
      // Stat increment replies received
      replies_rx_++;
      debug("\t ARP REPLY: %s belongs to %s (waiting: %u)\n",
             hdr->sipaddr.str().c_str(), hdr->shwaddr.str().c_str(), waiting_packets_.size());
      break;
    }
    default:
      debug2("\t UNKNOWN OPCODE\n");
      break;
    } //< switch(hdr->opcode)

  }

  void Arp::cache(IP4::addr ip, MAC::Addr mac) {
    debug("<Arp> Caching IP %s for %s\n", ip.str().c_str(), mac.str().c_str());

    auto entry = cache_.find(ip);

    if (entry != cache_.end()) {
      debug2("Cached entry found: %s recorded @ %llu. Updating timestamp\n",
             entry->second.mac().str().c_str(), entry->second.timestamp());

      if (entry->second.mac() != mac) {
        cache_.erase(entry);
        cache_[ip] = mac;
      } else {
        entry->second.update();
      }

    } else {
      cache_[ip] = mac; // Insert
      if (UNLIKELY(not flush_timer_.is_running())) {
        flush_timer_.start(flush_interval_);
      }
    }
  }


  void Arp::arp_respond(header* hdr_in, IP4::addr ack_ip) {
    debug2("\t IP Match. Constructing ARP Reply\n");

    // Stat increment replies sent
    replies_tx_++;

    // Populate ARP-header
    auto res = static_unique_ptr_cast<PacketArp>(inet_.create_packet());
    res->init(mac_, ack_ip, hdr_in->sipaddr);

    res->set_dest_mac(hdr_in->shwaddr);
    res->set_opcode(H_reply);

    debug("\t IP: %s is at My Mac: %s\n",
           res->source_ip().str().c_str(), res->source_mac().str().c_str());

    MAC::Addr dest = hdr_in->shwaddr;
    debug("<ARP -> physical> Sending response to %s. Linklayer begin: buf + %i \n",
          dest.str().c_str(), res->layer_begin() - res->buf() );

    linklayer_out_(std::move(res), dest, Ethertype::ARP);
  }

  void Arp::transmit(Packet_ptr pckt, IP4::addr next_hop) {

    Expects(pckt->size());

    debug2("<ARP -> physical> Transmitting %u bytes to %s\n",
          (uint32_t) pckt->size(), next_hop.str().c_str());

    MAC::Addr dest_mac;

    if (next_hop == IP4::ADDR_BCAST) {
      dest_mac = MAC::BROADCAST;
    } else {

#ifdef ARP_PASSTHROUGH
      extern MAC::Addr linux_tap_device;
      dest_mac = linux_tap_device;
#else
      // If we don't have a cached IP, perform address resolution
      auto cache_entry = cache_.find(next_hop);
      if (UNLIKELY(cache_entry == cache_.end())) {
        debug("<ARP> No cache entry for IP %s.  Resolving. \n", next_hop.to_string().c_str());
        await_resolution(std::move(pckt), next_hop);
        return;
      }

      // Get MAC from cache
      dest_mac = cache_[next_hop].mac();
#endif

      debug("<ARP> Found cache entry for IP %s -> %s \n",
            next_hop.to_string().c_str(), dest_mac.to_string().c_str());
    }

    // Move chain to linklayer
    linklayer_out_(std::move(pckt), dest_mac, Ethertype::IP4);
  }


  void Arp::resolve_waiting()
  {

    debug("<Arp> resolve timer doing sweep\n");

    if (waiting_packets_.empty()) {
      debug("<Arp> Noting to do - stopping resolve timer\n");
      resolve_timer_.stop();
      return;
    }

    for (auto& packet_it : waiting_packets_) {
      arp_resolver_(packet_it.first);
    }

    resolve_timer_.start(1s);

  }


  void Arp::await_resolution(Packet_ptr pckt, IP4::addr next_hop) {
    auto queue =  waiting_packets_.find(next_hop);
    debug("<ARP await> Waiting for resolution of %s\n", next_hop.str().c_str());
    if (queue != waiting_packets_.end()) {
      debug("\t * Packets already queueing for this IP\n");
      queue->second->chain(std::move(pckt));
    } else {
      debug("\t *This is the first packet going to that IP\n");
      waiting_packets_.emplace(std::make_pair(next_hop, std::move(pckt)));

      // Try resolution immediately
      arp_resolver_(next_hop);

      // Retry later
      resolve_timer_.start(1s);
    }
  }

  void Arp::arp_resolve(IP4::addr next_hop) {
    debug("<ARP RESOLVE> %s\n", next_hop.str().c_str());

    auto req = static_unique_ptr_cast<PacketArp>(inet_.create_packet());
    req->init(mac_, inet_.ip_addr(), next_hop);

    req->set_dest_mac(MAC::BROADCAST);
    req->set_opcode(H_request);

    // Stat increment requests sent
    requests_tx_++;

    linklayer_out_(std::move(req), MAC::BROADCAST, Ethertype::ARP);
  }


  void Arp::flush_expired()
  {
    debug("<ARP> Flushing expired entries\n");
    std::vector<IP4::addr> expired;
    for (auto ent : cache_) {
      if (ent.second.expired()) {
        expired.push_back(ent.first);
      }
    }

    for (auto ip : expired) {
      cache_.erase(ip);
    }

    if (not cache_.empty()) {
      flush_timer_.start(flush_interval_);
    }
  }

} //< namespace net
