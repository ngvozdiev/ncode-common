#include "network.h"

#include <chrono>
#include <iomanip>
#include <limits>
#include <utility>

#include "../event_queue.h"
#include "../substitute.h"
#include "../logging.h"
#include "tcp.h"
#include "udp.h"

namespace nc {
namespace htsim {

static std::string GetSinkId(const net::FiveTuple& five_tuple) {
  return Substitute("sink_$0_port_$1_to_$2_port_$3_proto_$4",
                    net::IPToStringOrDie(five_tuple.ip_src()),
                    five_tuple.src_port().Raw(),
                    net::IPToStringOrDie(five_tuple.ip_dst()),
                    five_tuple.dst_port().Raw(), five_tuple.ip_proto().Raw());
}

static std::string GetGeneratorId(const net::FiveTuple& five_tuple) {
  return Substitute("generator_$0_port_$1_to_$2_port_$3_proto_$4",
                    net::IPToStringOrDie(five_tuple.ip_src()),
                    five_tuple.src_port().Raw(),
                    net::IPToStringOrDie(five_tuple.ip_dst()),
                    five_tuple.dst_port().Raw(), five_tuple.ip_proto().Raw());
}

const net::DevicePortNumber Device::kLoopbackPortNum =
    net::DevicePortNumber::Max();

void Device::HandlePacket(PacketPtr pkt) {
  if (pkt->size_bytes() == 0) {
    // The packet is an SSCP message.
    uint8_t type = pkt->five_tuple().ip_proto().Raw();
    if (type == SSCPAddOrUpdate::kSSCPAddOrUpdateType) {
      ++stats_.route_updates_seen;

      SSCPAddOrUpdate* add_or_update_message =
          static_cast<SSCPAddOrUpdate*>(pkt.get());
      matcher_.AddRule(add_or_update_message->TakeRule());

      if (add_or_update_message->tx_id() != SSCPMessage::kNoTxId &&
          replies_handler_ != nullptr) {
        auto reply = GetFreeList<SSCPAck>().New(
            ip_address_, pkt->five_tuple().ip_src(),
            event_queue_->CurrentTime(), add_or_update_message->tx_id());

        LOG(INFO) << "Will TX ACK " << reply->ToString();
        replies_handler_->HandlePacket(std::move(reply));
      }
    } else if (type == SSCPStatsRequest::kSSCPStatsRequestType) {
      SSCPStatsRequest* stats_request_message =
          static_cast<SSCPStatsRequest*>(pkt.get());

      auto reply = GetFreeList<SSCPStatsReply>().New(
          ip_address_, pkt->five_tuple().ip_src(), event_queue_->CurrentTime());
      matcher_.PopulateSSCPStats(stats_request_message->include_flow_counts(),
                                 reply.get());
      PostProcessStats(*stats_request_message, reply.get());

      CHECK(replies_handler_)
          << "Received stats request, but no output handler";
      replies_handler_->HandlePacket(std::move(reply));
    }

    return;
  }

  const net::FiveTuple& incoming_tuple = pkt->five_tuple();
  net::FiveTuple outgoing_tuple = incoming_tuple.Reverse();
  auto it = connections_.find(outgoing_tuple);
  if (it != connections_.end()) {
    it->second->HandlePacket(std::move(pkt));
    return;
  }

  // Will have to add a new sink for the incoming packets.
  Port* loopback_port = GetLoopbackPort();
  std::string sink_id = GetSinkId(incoming_tuple);

  std::unique_ptr<Connection> new_connection;
  net::IPProto ip_proto = incoming_tuple.ip_proto();
  if (ip_proto == net::kProtoUDP) {
    new_connection = make_unique<UDPSink>(sink_id, outgoing_tuple,
                                          loopback_port, event_queue_);
    LOG(INFO) << "Added UDP sink at " << id() << " for " << outgoing_tuple;
  } else if (ip_proto == net::kProtoTCP) {
    new_connection = make_unique<TCPSink>(sink_id, outgoing_tuple,
                                          loopback_port, event_queue_);
    LOG(INFO) << "Added TCP sink at " << id() << " for " << outgoing_tuple;
  } else {
    LOG(FATAL) << "Don't know how to create new connection for IP proto "
               << incoming_tuple.ip_proto().Raw();
  }

  Connection* raw_ptr = new_connection.get();
  connections_.emplace(outgoing_tuple, std::move(new_connection));
  raw_ptr->HandlePacket(std::move(pkt));
}

net::FiveTuple Device::PickSrcPortOrDie(
    const net::FiveTuple& tuple_with_no_src_port) {
  const net::FiveTuple& t = tuple_with_no_src_port;
  for (uint16_t i = 1; i < std::numeric_limits<uint16_t>::max(); ++i) {
    net::FiveTuple return_tuple(t.ip_src(), t.ip_dst(), t.ip_proto(),
                                net::AccessLayerPort(i), t.dst_port());
    if (connections_.find(return_tuple) == connections_.end()) {
      return return_tuple;
    }
  }

  CHECK(false) << "Out of src ports";
  return net::FiveTuple::kDefaultTuple;
}

net::FiveTuple Device::PrepareTuple(net::IPAddress dst_address,
                                    net::AccessLayerPort dst_port, bool tcp) {
  net::FiveTuple tuple(ip_address_, dst_address,
                       tcp ? net::kProtoTCP : net::kProtoUDP,
                       kWildAccessLayerPort, dst_port);
  tuple = PickSrcPortOrDie(tuple);
  return tuple;
}

TCPSource* Device::AddTCPGenerator(const TCPSourceConfig& tcp_config,
                                   net::IPAddress dst_address,
                                   net::AccessLayerPort dst_port) {
  net::FiveTuple tuple = PrepareTuple(dst_address, dst_port, true);
  Port* loopback_port = GetLoopbackPort();
  std::string gen_id = GetGeneratorId(tuple);

  auto new_connection = make_unique<TCPSource>(gen_id, tuple, tcp_config,
                                               loopback_port, event_queue_);

  CHECK(network_ != nullptr) << "Device not part of a network";
  network_->RegisterTCPSourceWithRetxTimer(new_connection.get());

  TCPSource* raw_ptr = new_connection.get();
  connections_.emplace(tuple, std::move(new_connection));

  LOG(INFO) << Substitute("Added TCP generator at $0 with 5-tuple $1", id_,
                          tuple.ToString());
  return raw_ptr;
}

UDPSource* Device::AddUDPGenerator(net::IPAddress dst_address,
                                   net::AccessLayerPort dst_port) {
  net::FiveTuple tuple = PrepareTuple(dst_address, dst_port, false);
  Port* loopback_port = GetLoopbackPort();
  std::string gen_id = GetGeneratorId(tuple);

  auto new_connection =
      make_unique<UDPSource>(gen_id, tuple, loopback_port, event_queue_);
  UDPSource* raw_ptr = new_connection.get();
  connections_.emplace(tuple, std::move(new_connection));
  return raw_ptr;
}

Device::Device(const std::string& id, net::IPAddress ip_address,
               EventQueue* event_queue)
    : DeviceInterface(id, ip_address, event_queue),
      matcher_("matcher_for_" + id),
      die_on_fail_to_match_(false) {}

Port::Port(net::DevicePortNumber number, DeviceInterface* device)
    : number_(number),
      parent_device_(device),
      out_handler_(nullptr),
      internal_(false) {}

void Port::HandlePacket(PacketPtr pkt) {
  parent_device_->HandlePacketFromPort(this, std::move(pkt));
}

void Port::SendPacketOut(PacketPtr pkt) {
  out_handler_->HandlePacket(std::move(pkt));
}

void Port::Connect(PacketHandler* out_handler) {
  if (out_handler == out_handler_) {
    return;
  }

  CHECK(out_handler_ == nullptr) << "Tried to connect port " << number_.Raw()
                                 << " twice on " << parent_device_->id();
  out_handler_ = out_handler;
}

void Port::Reconnect(PacketHandler* out_handler) {
  CHECK(out_handler_ != nullptr) << "Tried to reconnect an unconnected port";
  out_handler_ = out_handler;
}

Port* DeviceInterface::FindOrCreatePort(net::DevicePortNumber port_num) {
  auto it = port_number_to_port_.find(port_num);
  if (it != port_number_to_port_.end()) {
    return it->second.get();
  }

  std::unique_ptr<Port> port_ptr =
      std::unique_ptr<Port>(new Port(port_num, this));
  Port* port_naked_ptr = port_ptr.get();

  port_number_to_port_.emplace(port_num, std::move(port_ptr));
  return port_naked_ptr;
}

Port* DeviceInterface::NextAvailablePort() {
  for (uint32_t i = 1; i < net::DevicePortNumber::Max().Raw(); ++i) {
    auto port_number = net::DevicePortNumber(i);
    if (ContainsKey(port_number_to_port_, port_number)) {
      continue;
    }

    std::unique_ptr<Port> port_ptr =
        std::unique_ptr<Port>(new Port(port_number, this));
    Port* port_naked_ptr = port_ptr.get();
    port_number_to_port_.emplace(port_number, std::move(port_ptr));
    return port_naked_ptr;
  }

  LOG(FATAL) << "Out of port numbers";
  return nullptr;
}

void Device::HandlePacketFromPort(Port* input_port, PacketPtr pkt) {
  uint32_t pkt_size = pkt->size_bytes();
  stats_.packets_seen += 1;
  stats_.bytes_seen += pkt_size;

  if (pkt->five_tuple().ip_dst() == ip_address()) {
    stats_.packets_for_localhost += 1;
    stats_.bytes_for_localhost += pkt_size;
    HandlePacket(std::move(pkt));
    return;
  }

  const MatchRuleAction* action =
      matcher_.MatchOrNull(*pkt, input_port->number());
  if (action == nullptr) {
    stats_.packets_failed_to_match += 1;
    stats_.bytes_failed_to_match += pkt_size;

    // Packet will be dropped.
    if (die_on_fail_to_match_) {
      LOG(FATAL) << "Dropping packet " << pkt->ToString() << " at " << id();
    }

    return;
  }

  HandlePacketWithAction(input_port, std::move(pkt), action);
}

void Device::HandlePacketWithAction(Port* input_port, PacketPtr pkt,
                                    const MatchRuleAction* action) {
  CHECK(action != nullptr);
  if (action->tag() != kNullPacketTag) {
    pkt->set_tag(action->tag());
  }

  if (!pkt->preferential_drop() && action->preferential_drop()) {
    pkt->set_preferential_drop(true);
  }

  if (!pkt->DecrementTTL()) {
    LOG(FATAL) << "TTL exceeded at " << id() << " " << pkt->ToString();
  }

  const auto& it = port_number_to_port_.find(action->output_port());
  CHECK(it != port_number_to_port_.end())
      << "Unable to find port " << Substitute("$0", action->output_port().Raw())
      << " at " << id();

  Port* output_port = it->second.get();
  if (input_port->internal() && !output_port->internal() &&
      internal_external_observer_) {
    internal_external_observer_->ObservePacket(*pkt);
  } else if (!input_port->internal() && output_port->internal() &&
             external_internal_observer_) {
    external_internal_observer_->ObservePacket(*pkt);
  }

  output_port->SendPacketOut(std::move(pkt));
}

void DeviceInterface::AddInternalExternalObserver(PacketObserver* observer) {
  CHECK(internal_external_observer_ == nullptr ||
        internal_external_observer_ == observer);
  CHECK(observer != nullptr);
  internal_external_observer_ = observer;
}

void DeviceInterface::AddExternalInternalObserver(PacketObserver* observer) {
  CHECK(external_internal_observer_ == nullptr ||
        external_internal_observer_ == observer);
  CHECK(observer != nullptr);
  external_internal_observer_ = observer;
}

Network::Network(EventQueueTime tcp_retx_scan_period, EventQueue* event_queue)
    : SimComponent("network", event_queue) {
  tcp_retx_timer_ = make_unique<TCPRtxTimer>("tcp_retx_timer",
                                             tcp_retx_scan_period, event_queue);
}

void Network::AddDevice(DeviceInterface* device) {
  id_to_device_.emplace(device->id(), device);
  device->set_network(this);
}

void Network::AddLink(Queue* queue, Pipe* pipe, const std::string& src_id,
                      const std::string& dst_id,
                      nc::net::DevicePortNumber src_port_num,
                      nc::net::DevicePortNumber dst_port_num, bool internal) {
  CHECK(src_id != dst_id) << "Link source same as destination";

  DeviceInterface& src = FindDeviceOrDie(src_id);
  DeviceInterface& dst = FindDeviceOrDie(dst_id);

  Port* src_port = src.FindOrCreatePort(src_port_num);
  src_port->set_internal(internal);

  Port* dst_port = dst.FindOrCreatePort(dst_port_num);
  dst_port->set_internal(internal);

  // Connect the queue to the pipe and the source port to the queue
  src_port->Connect(queue);
  queue->Connect(pipe);
  pipe->Connect(dst_port);

  LOG(INFO) << Substitute("Added queue $0:$1 -> $2:$3.", src_id,
                          src_port_num.Raw(), dst_id, src_port_num.Raw());
  LOG(INFO) << Substitute("Added pipe $0:$1 -> $2:$3.", src_id,
                          src_port_num.Raw(), dst_id, src_port_num.Raw());
}

void Network::RegisterTCPSourceWithRetxTimer(TCPSource* src) {
  tcp_retx_timer_->RegisterTCPSource(src);
}

template <typename T>
static void PrintTimeDiff(std::ostream& out, T chrono_diff) {
  namespace sc = std::chrono;
  auto diff = sc::duration_cast<sc::milliseconds>(chrono_diff).count();
  auto const msecs = diff % 1000;
  diff /= 1000;
  auto const secs = diff % 60;
  diff /= 60;
  auto const mins = diff % 60;
  diff /= 60;
  auto const hours = diff % 24;
  diff /= 24;
  auto const days = diff;

  bool printed_earlier = false;
  if (days >= 1) {
    printed_earlier = true;
    out << days << (1 != days ? " days " : " day ");
  }
  if (printed_earlier || hours >= 1) {
    printed_earlier = true;
    out << hours << (1 != hours ? " hours " : " hour ");
  }
  if (printed_earlier || mins >= 1) {
    printed_earlier = true;
    out << mins << (1 != mins ? " minutes " : " minute ");
  }
  if (printed_earlier || secs >= 1) {
    printed_earlier = true;
    out << secs << (1 != secs ? " seconds " : " second ");
  }
  if (printed_earlier || msecs >= 1) {
    printed_earlier = true;
    out << msecs << (1 != msecs ? " milliseconds" : " millisecond");
  }
}

static std::chrono::milliseconds TimeNow() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::high_resolution_clock::now().time_since_epoch());
}

ProgressIndicator::ProgressIndicator(std::chrono::milliseconds update_period,
                                     nc::EventQueue* event_queue)
    : nc::EventConsumer("ProgressIndicator", event_queue),
      period_(event_queue->ToTime(update_period)),
      init_real_time_(TimeNow()) {
  EnqueueIn(period_);
}

void ProgressIndicator::HandleEvent() {
  double progress = event_queue()->Progress();
  CHECK(progress >= 0 && progress <= 1.0);
  std::cout << "\rProgress: " << std::setprecision(3) << (progress * 100.0)
            << "% ";

  auto real_time_delta = TimeNow() - init_real_time_;
  if (real_time_delta.count() > 0) {
    std::cout << "time remaining: ";
    auto remaining = std::chrono::milliseconds(static_cast<uint64_t>(
        real_time_delta.count() / progress * (1 - progress)));

    PrintTimeDiff(std::cout, remaining);
    std::cout << "                ";
  }

  std::cout << std::flush;
  EnqueueIn(period_);
}

}  // namespace htsim
}  // namespace ncode
