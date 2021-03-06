/**
 * Real Time Protocol Music Instrument Digital Interface Daemon
 * Copyright (C) 2019-2020 David Moreno Montero <dmoreno@coralbits.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <optional>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <rtpmidid/iobytes.hpp>
#include <rtpmidid/logger.hpp>
#include <rtpmidid/poller.hpp>
#include <rtpmidid/rtpclient.hpp>

using namespace std::chrono_literals;
using namespace rtpmidid;

rtpclient::rtpclient(std::string name) : peer(std::move(name)) {
  local_base_port = -1;
  remote_base_port = -1; // Not defined
  control_socket = -1;
  midi_socket = -1;
  //peer.initiator_id = rand();
  conn_event = std::nullopt;
  peer.send_event.connect([this](const io_bytes &data, rtppeer::port_e port) {
    this->sendto(data, port);
  });
}

rtpclient::~rtpclient() {
  // Say goodbye to any connected channels
  DEBUG("sending goodbyes");
  if (peer.status & rtppeer::status_e::MIDI_CONNECTED)
    peer.send_goodbye(rtppeer::MIDI_PORT);
  if (peer.status & rtppeer::status_e::CONTROL_CONNECTED)
    peer.send_goodbye(rtppeer::CONTROL_PORT);

  if (midi_socket >= 0) {
    poller.remove_fd(midi_socket);
    close(midi_socket);
  }
  if (control_socket >= 0) {
    poller.remove_fd(control_socket);
    close(control_socket);
  }
}

void rtpclient::connect_to(const std::string &address,
                           const std::string &port,
                           int local_port) {
  struct addrinfo hints;
  struct addrinfo *sockaddress_list = nullptr;
  char host[NI_MAXHOST], service[NI_MAXSERV];
  socklen_t peer_addr_len = NI_MAXHOST;

  control_socket = -1;
  midi_socket = -1;

  DEBUG("Try connect to service at {}:{} ({})", address, port, peer.remote_name);

  try {
    int res;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = 0;
    hints.ai_protocol = 0;

    res = getaddrinfo(address.c_str(), port.c_str(), &hints, &sockaddress_list);
    if (res < 0) {
      DEBUG("Error resolving address {}:{}", address, port);
      throw rtpmidid::exception("Can not resolve address {}:{}. {}", address,
                                port, strerror(errno));
    }
    // Get addr info may return several options, try them in order.
    // we asusme that if the ocntrol success to be created the midi will too.
    auto serveraddr = sockaddress_list;
    for (; serveraddr != nullptr; serveraddr = serveraddr->ai_next) {
      getnameinfo(serveraddr->ai_addr, peer_addr_len, host, NI_MAXHOST, service,
                  NI_MAXSERV, NI_NUMERICSERV);
      DEBUG("Try connect to resolved name: {}:{}", host, service);
      // remote_base_port = service;

      control_socket = socket(AF_INET6, SOCK_DGRAM, 0);
      if (control_socket <= 0) {
        continue;
      }
      int reuse = 1;
      if (setsockopt(control_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))) {
        close(control_socket);
        control_socket = -1;
        throw rtpmidid::exception("Could not make local control port reusable");
      }
      if (local_port >= 0) {
        struct sockaddr_in6 servaddr;
        bzero(&servaddr, sizeof(sockaddr_in6));
        servaddr.sin6_family = AF_INET6;
        servaddr.sin6_addr = in6addr_any;
        servaddr.sin6_port = htons(local_port);
        if (bind(control_socket, (struct sockaddr *)&servaddr, sizeof(servaddr))) {
          close(control_socket);
          control_socket = -1;
          throw rtpmidid::exception("Could not bind local control port");
        }
      }

      if (connect(control_socket, serveraddr->ai_addr,
                  serveraddr->ai_addrlen) == 0) {
        break;
      }
      close(control_socket);
    }
    if (!serveraddr) {
      DEBUG("Error opening control socket, port {}", port);
      control_socket = 0;
      throw rtpmidid::exception(
          "Can not open remote rtpmidi control socket. {}", strerror(errno));
    }
    DEBUG("Connected to resolved name: {}:{}", host, service);
    memcpy(&control_addr, serveraddr->ai_addr, sizeof(control_addr));

    struct sockaddr_in6 servaddr;
    socklen_t len = sizeof(servaddr);
    ::getsockname(control_socket, (struct sockaddr *)&servaddr, &len);
    local_base_port = ntohs(servaddr.sin6_port);

    DEBUG("Control port, local: {}, remote at {}:{}", local_base_port, host,
          service);

    poller.add_fd_in(control_socket,
                     [this](int) { this->data_ready(rtppeer::CONTROL_PORT); });

    midi_socket = socket(AF_INET6, SOCK_DGRAM, 0);
    if (midi_socket < 0) {
      throw rtpmidid::exception("Can not open MIDI socket. Out of sockets?");
    }
    // Reuse servaddr, just on next port
    remote_base_port = ntohs(((sockaddr_in *)serveraddr->ai_addr)->sin_port);
    ((sockaddr_in *)serveraddr->ai_addr)->sin_port =
        htons(remote_base_port + 1);

    servaddr.sin6_port = htons(local_base_port + 1);

    int reuse = 1;
    auto optret = setsockopt(midi_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (optret) {
      throw rtpmidid::exception("Could not make local midi port reusable");
    }
    
    auto ret = bind(midi_socket, (struct sockaddr *)&servaddr, len);
    if (ret < 0) {
      throw rtpmidid::exception("Could not bind to local midi port");
    }

    if (connect(midi_socket, serveraddr->ai_addr, serveraddr->ai_addrlen) < 0) {
      DEBUG("Error opening midi socket, port {}", port);
      throw rtpmidid::exception("Can not open remote rtpmidi MIDI socket. {}",
                                strerror(errno));
    }
    memcpy(&midi_addr, serveraddr->ai_addr, sizeof(midi_addr));
    ::getsockname(midi_socket, (struct sockaddr *)&servaddr, &len);
    auto midi_port = htons(servaddr.sin6_port);
    DEBUG("MIDI PORT at port {}", midi_port);

    poller.add_fd_in(midi_socket,
                     [this](int) { this->data_ready(rtppeer::MIDI_PORT); });
  } catch (const std::exception &excp) {
    ERROR("Error creating rtp client: {}", excp.what());
    if (control_socket >= 0) {
      poller.remove_fd(control_socket);
      ::close(control_socket);
      control_socket = -1;
    }
    if (midi_socket >= 0) {
      poller.remove_fd(midi_socket);
      ::close(midi_socket);
      midi_socket = -1;
    }
    if (sockaddress_list) {
      freeaddrinfo(sockaddress_list);
    }
    peer.disconnect_event(rtppeer::disconnect_reason_e::CANT_CONNECT);
    return;
  }
  if (sockaddress_list) {
    freeaddrinfo(sockaddress_list);
  }

  DEBUG("Connecting control port {} to {}:{} local_ssrc {:X}", local_base_port, host, service,
        peer.local_ssrc);

  // If not connected, connect now the MIDI port
  conn_event = peer.connected_event.connect(
      [this, address, port](const std::string &name, rtppeer::status_e status) {
        if (status == rtppeer::CONTROL_CONNECTED) {
          DEBUG("Connecting midi port {} to {}:{}, local ssrc {:X}", local_base_port + 1,
                address, remote_base_port + 1, peer.local_ssrc);
          peer.connect_to(rtppeer::MIDI_PORT);
        } else if (status == rtppeer::CONNECTED) {
          connected();
        }
      });

  peer.connect_to(rtppeer::CONTROL_PORT);

  connect_timer = poller.add_timer_event(20s, [this] {
    peer.connected_event.disconnect(conn_event.value());
    peer.disconnect_event(rtppeer::CONNECT_TIMEOUT);
  });
}

/**
 * Send the periodic latency and connection checks
 *
 * At first six times as received confirmation from other end. Then Every 10
 * secs. Check connected() function for actuall recall code.
 *
 * This just checks timeout and sends the ck.
 */
void rtpclient::connected() {
  connect_timer.disable();

  peer.ck_event.connect([this](float ms) {
    ck_timeout.disable();
    if (timerstate < 6) {
      timer_ck =
          poller.add_timer_event(250ms, [this] { send_ck0_with_timeout(); });
      timerstate++;
    } else {
      timer_ck =
          poller.add_timer_event(10s, [this] { send_ck0_with_timeout(); });
    }
  });
  send_ck0_with_timeout();
}

void rtpclient::send_ck0_with_timeout() {
  peer.send_ck0();
  ck_timeout = poller.add_timer_event(5s, [this] {
    peer.disconnect_event(rtppeer::disconnect_reason_e::CK_TIMEOUT);
  });
}

void rtpclient::sendto(const io_bytes &pb, rtppeer::port_e port) {
  auto peer_addr = (port == rtppeer::MIDI_PORT) ? midi_addr : control_addr;

  auto socket = rtppeer::MIDI_PORT == port ? midi_socket : control_socket;

  auto res = ::sendto(socket, pb.start, pb.size(), MSG_CONFIRM,
                      (const struct sockaddr *)&peer_addr, sizeof(peer_addr));

  if (res < 0 || static_cast<uint32_t>(res) != pb.size()) {
    throw exception("Could not send all data to {}:{}. Sent {}. {}",
                    peer.remote_name, remote_base_port, res, strerror(errno));
  }
}

void rtpclient::reset() {
  timerstate = 0;

  // Disable pending connect timer, or it may
  // fire before reinitializing the connection
  connect_timer.disable();
  
  // Disconnect prior attempts connection events
  // so an eventual connect doesn't fire multiple events
  if (conn_event.has_value()){
    peer.connected_event.disconnect(conn_event.value());
    conn_event = std::nullopt;
  }

  remote_base_port = 0;

  // Close ports to ensure they don't leak during retry
  if (midi_socket >= 0) {
    poller.remove_fd(midi_socket);
    close(midi_socket);
    midi_socket = 0;
  }
  if (control_socket >= 0) {
    poller.remove_fd(control_socket);
    close(control_socket);
    control_socket = 0;
  }
  peer.reset();
}

void rtpclient::data_ready(rtppeer::port_e port) {
  uint8_t raw[1500];
  struct sockaddr_in6 cliaddr;
  unsigned int len = sizeof(cliaddr);
  auto socket = port == rtppeer::CONTROL_PORT ? control_socket : midi_socket;
  auto n = recvfrom(socket, raw, 1500, MSG_DONTWAIT,
                    (struct sockaddr *)&cliaddr, &len);
  // DEBUG("Got some data from control: {}", n);
  if (n < 0) {
    throw exception("Error reading from rtppeer {}:{}", peer.remote_name,
                    remote_base_port);
  }

  auto buffer = io_bytes_reader(raw, n);
  peer.data_ready(std::move(buffer), port);
}
