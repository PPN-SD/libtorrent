#include "config.h"

#define __STDC_FORMAT_MACROS

#include "tracker_udp.h"

#include <cstdio>
#include <netdb.h>
#include <sys/types.h>

#include "globals.h"
#include "manager.h"
#include "net/address_list.h"
#include "rak/error_number.h"
#include "torrent/exceptions.h"
#include "torrent/connection_manager.h"
#include "torrent/download_info.h"
#include "torrent/poll.h"
#include "torrent/tracker_list.h"
#include "torrent/net/socket_address.h"
#include "torrent/net/resolver.h"
#include "torrent/utils/log.h"
#include "torrent/utils/option_strings.h"
#include "torrent/utils/thread.h"
#include "torrent/utils/uri_parser.h"

#include "thread_main.h"

#define LT_LOG(log_fmt, ...)                                            \
  lt_log_print_hash(LOG_TRACKER_REQUESTS, info().info_hash, "tracker_udp", log_fmt, __VA_ARGS__);

#define LT_LOG_DUMP(log_level, log_dump_data, log_dump_size, log_fmt, ...) \
  lt_log_print_hash_dump(LOG_TRACKER_DUMP, log_dump_data, log_dump_size, info().info_hash, "tracker_udp", log_fmt, __VA_ARGS__);

namespace torrent {

TrackerUdp::TrackerUdp(const TrackerInfo& info, int flags) :
  TrackerWorker(info, flags) {

  m_task_timeout.slot() = std::bind(&TrackerUdp::receive_timeout, this);
}

TrackerUdp::~TrackerUdp() {
  close_directly();
}

bool
TrackerUdp::is_busy() const {
  return get_fd().is_valid();
}

void
TrackerUdp::send_event(tracker::TrackerState::event_enum new_state) {
  LT_LOG("sending event : requester:%p state:%s url:%s",
         this, option_as_string(OPTION_TRACKER_EVENT, new_state), info().url.c_str());

  // TODO: Don't close fd for every new request.
  close_directly();

  hostname_type hostname;

  if (!parse_udp_url(info().url, hostname, m_port))
    return receive_failed("could not parse hostname or port");

  lock_and_set_latest_event(new_state);

  m_send_state = new_state;
  m_resolver_requesting = true;
  m_sending_announce = true;

  LT_LOG("resolving hostname : requester:%p address:%s", this, hostname.data());

  if ((m_inet_address == nullptr && m_inet6_address == nullptr) ||
      (cachedTime - m_time_last_resolved) > rak::timer::from_minutes(24 * 60) ||
      m_failed_since_last_resolved > 3) {

    // Currently discarding SOCK_DGRAM filter.
    thread_self->resolver()->resolve_both(this, hostname.data(), AF_UNSPEC,
                                          [this](c_sin_shared_ptr sin, c_sin6_shared_ptr sin6, int err) {
                                            receive_resolved(sin, sin6, err);
                                          });
    return;
  }

  start_announce();
}

void
TrackerUdp::send_scrape() {
  throw internal_error("Tracker type UDP does not support scrape.");
}

bool
TrackerUdp::parse_udp_url(const std::string& url, hostname_type& hostname, int& port) const {
  if (std::sscanf(url.c_str(), "udp://%1023[^:]:%i", hostname.data(), &port) == 2 && hostname[0] != '\0' &&
      port > 0 && port < (1 << 16))
    return true;

  if (std::sscanf(url.c_str(), "udp://[%1023[^]]]:%i", hostname.data(), &port) == 2 && hostname[0] != '\0' &&
      port > 0 && port < (1 << 16))
    return true;

  return false;
}

// TODO: Controller should not need to close the tracker when starting a new request.

void
TrackerUdp::close() {
  LT_LOG("closing event : requester:%p state:%s url:%s",
         this, option_as_string(OPTION_TRACKER_EVENT, state().latest_event()), info().url.c_str());

  close_directly();
}

void
TrackerUdp::disown() {
  LT_LOG("disowning tracker : requester:%p state:%s url:%s",
         this, option_as_string(OPTION_TRACKER_EVENT, state().latest_event()), info().url.c_str());

  close_directly();
}

void
TrackerUdp::close_directly() {
  LT_LOG("closing directly : requester:%p state:%s url:%s",
         this, option_as_string(OPTION_TRACKER_EVENT, state().latest_event()), info().url.c_str());

  if (m_resolver_requesting)
    thread_self->resolver()->cancel(this);

  m_resolver_requesting = false;
  m_sending_announce = false;

  m_read_buffer = nullptr;
  m_write_buffer = nullptr;

  priority_queue_erase(&taskScheduler, &m_task_timeout);

  if (!get_fd().is_valid())
    return;

  thread_self->poll()->remove_read(this);
  thread_self->poll()->remove_write(this);
  thread_self->poll()->remove_error(this);
  thread_self->poll()->close(this);

  get_fd().close();
  get_fd().clear();
}

tracker_enum
TrackerUdp::type() const {
  return TRACKER_UDP;
}

void
TrackerUdp::receive_failed(const std::string& msg) {
  m_failed_since_last_resolved++;

  close_directly();
  m_slot_failure(msg);
}

// TODO: Only resolve when we don't have a valid address, failed too many times or network change
// events.
void
TrackerUdp::receive_resolved(c_sin_shared_ptr& sin, c_sin6_shared_ptr& sin6, int err) {
  if (std::this_thread::get_id() != torrent::thread_main->thread_id())
    LT_LOG("invalid thread : requester:%p", this);

  LT_LOG("received resolved : requester:%p", this);

  if (!m_resolver_requesting)
    throw internal_error("TrackerUdp::receive_resolved() called but m_resolver_requesting is false.");

  m_resolver_requesting = false;

  if (err != 0) {
    LT_LOG("could not resolve hostname : requester:%p error:'%s'", this, gai_strerror(err));
    return receive_failed("could not resolve hostname : error:'" + std::string(gai_strerror(err)) + "'");
  }

  if (sin != nullptr) {
    m_inet_address = sin_copy(sin.get());
    sa_set_port((sockaddr*)m_inet_address.get(), m_port);
  } else {
    m_inet_address = nullptr;
  }

  if (sin6 != nullptr) {
    m_inet6_address = sin6_copy(sin6.get());
    sa_set_port((sockaddr*)m_inet6_address.get(), m_port);
  } else {
    m_inet6_address = nullptr;
  }

  m_time_last_resolved = cachedTime;
  m_failed_since_last_resolved = 0;

  start_announce();
}

void
TrackerUdp::receive_timeout() {
  if (m_task_timeout.is_queued())
    throw internal_error("TrackerUdp::receive_timeout() called but m_task_timeout is still scheduled.");

  if (--m_tries == 0) {
    receive_failed("unable to connect to UDP tracker");
    return;
  }

  priority_queue_insert(&taskScheduler, &m_task_timeout, (cachedTime + rak::timer::from_seconds(udp_timeout)).round_seconds());
  thread_self->poll()->insert_write(this);
}

void
TrackerUdp::start_announce() {
  if (!m_sending_announce)
    throw internal_error("TrackerUdp::start_announce() called but m_sending_announce is false.");

  m_sending_announce = false;

  // TODO: Properly select preferred protocol and on failure try the other one.

  if (m_inet_address != nullptr)
    m_current_address = (sockaddr*)m_inet_address.get();
  else if (m_inet6_address != nullptr)
    m_current_address = (sockaddr*)m_inet6_address.get();
  else
    throw internal_error("TrackerUdp::start_announce() called but both m_inet_address and m_inet6_address are nullptr.");

  LT_LOG("starting announce : requester:%p address:%s", this, sa_pretty_str(m_current_address).c_str());

  // TODO: Make each of these a separate error... at the very least separate open and bind.
  if (!get_fd().open_datagram() || !get_fd().set_nonblock()) {
    LT_LOG("could not open UDP socket : requester:%p", this);
    return receive_failed("could not open UDP socket");
  }

  auto bind_address = rak::socket_address::cast_from(manager->connection_manager()->bind_address());

  if (bind_address->is_bindable() && !get_fd().bind(*bind_address)) {
    LT_LOG("failed to bind socket to udp address : requester:%p address:%s error:'%s'",
           this, bind_address->pretty_address_str().c_str(), rak::error_number::current().c_str());
    return receive_failed("failed to bind socket to udp address '" + bind_address->pretty_address_str() +
                          "' with error '" + rak::error_number::current().c_str() + "'");
  }

  // TODO: Don't recreate buffers.
  m_read_buffer = std::make_unique<ReadBuffer>();
  m_write_buffer = std::make_unique<WriteBuffer>();

  prepare_connect_input();

  thread_self->poll()->open(this);
  thread_self->poll()->insert_read(this);
  thread_self->poll()->insert_write(this);
  thread_self->poll()->insert_error(this);

  m_tries = udp_tries;
  priority_queue_insert(&taskScheduler, &m_task_timeout, (cachedTime + rak::timer::from_seconds(udp_timeout)).round_seconds());
}

void
TrackerUdp::event_read() {
  rak::socket_address sa;

  int s = read_datagram(m_read_buffer->begin(), m_read_buffer->reserved(), &sa);

  if (s < 0)
    return;

  m_read_buffer->reset_position();
  m_read_buffer->set_end(s);

  LT_LOG("received reply (size:%d)", s);
  LT_LOG_DUMP(DEBUG, (const char*)m_read_buffer->begin(), s, "received reply", 0);

  if (s < 4)
    return;

  // Make sure sa is from the source we expected?

  // Do something with the content here.
  switch (m_read_buffer->read_32()) {
  case 0:
    if (m_action != 0 || !process_connect_output())
      return;

    prepare_announce_input();

    priority_queue_update(&taskScheduler, &m_task_timeout, (cachedTime + rak::timer::from_seconds(udp_timeout)).round_seconds());

    m_tries = udp_tries;
    thread_self->poll()->insert_write(this);
    return;

  case 1:
    if (m_action != 1 || !process_announce_output())
      return;

    return;

  case 3:
    if (!process_error_output())
      return;

    return;

  default:
    return;
  };
}

void
TrackerUdp::event_write() {
  if (m_write_buffer->size_end() == 0)
    throw internal_error("TrackerUdp::write() called but the write buffer is empty.");

  [[maybe_unused]] int s = write_datagram_sa(m_write_buffer->begin(), m_write_buffer->size_end(), m_current_address);

  thread_self->poll()->remove_write(this);
}

void
TrackerUdp::event_error() {
}

void
TrackerUdp::prepare_connect_input() {
  m_write_buffer->reset();
  m_write_buffer->write_64(m_connection_id = magic_connection_id);
  m_write_buffer->write_32(m_action = 0);
  m_write_buffer->write_32(m_transaction_id = random());

  LT_LOG_DUMP(DEBUG, m_write_buffer->begin(), m_write_buffer->size_end(),
              "prepare connect (id:%" PRIx32 ")", m_transaction_id);
}

void
TrackerUdp::prepare_announce_input() {
  m_write_buffer->reset();

  m_write_buffer->write_64(m_connection_id);
  m_write_buffer->write_32(m_action = 1);
  m_write_buffer->write_32(m_transaction_id = random());

  m_write_buffer->write_range(info().info_hash.begin(), info().info_hash.end());
  m_write_buffer->write_range(info().local_id.begin(), info().local_id.end());

  auto parameters = m_slot_parameters();

  m_write_buffer->write_64(parameters.completed_adjusted);
  m_write_buffer->write_64(parameters.download_left);
  m_write_buffer->write_64(parameters.uploaded_adjusted);
  m_write_buffer->write_32(m_send_state);

  const rak::socket_address* localAddress = rak::socket_address::cast_from(manager->connection_manager()->local_address());

  uint32_t local_addr = 0;

  if (localAddress->family() == rak::socket_address::af_inet)
    local_addr = localAddress->sa_inet()->address_n();

  m_write_buffer->write_32_n(local_addr);
  m_write_buffer->write_32(info().key);
  m_write_buffer->write_32(parameters.numwant);
  m_write_buffer->write_16(manager->connection_manager()->listen_port());

  if (m_write_buffer->size_end() != 98)
    throw internal_error("TrackerUdp::prepare_announce_input() ended up with the wrong size");

  LT_LOG_DUMP(DEBUG, m_write_buffer->begin(), m_write_buffer->size_end(),
              "prepare announce (state:%s id:%" PRIx32 " up_adj:%" PRIu64 " completed_adj:%" PRIu64 " left_adj:%" PRIu64 ")",
              option_as_string(OPTION_TRACKER_EVENT, m_send_state),
              m_transaction_id, parameters.uploaded_adjusted, parameters.completed_adjusted, parameters.download_left);
}

bool
TrackerUdp::process_connect_output() {
  if (m_read_buffer->size_end() < 16 ||
      m_read_buffer->read_32() != m_transaction_id)
    return false;

  m_connection_id = m_read_buffer->read_64();

  return true;
}

bool
TrackerUdp::process_announce_output() {
  if (m_read_buffer->size_end() < 20 ||
      m_read_buffer->read_32() != m_transaction_id)
    return false;

  {
    auto guard = lock_guard();

    state().set_normal_interval(m_read_buffer->read_32());
    state().set_min_interval(tracker::TrackerState::default_min_interval);

    state().m_scrape_incomplete = m_read_buffer->read_32(); // leechers
    state().m_scrape_complete   = m_read_buffer->read_32(); // seeders
    state().m_scrape_time_last  = rak::timer::current().seconds();
  }

  AddressList l;

  std::copy(reinterpret_cast<const SocketAddressCompact*>(m_read_buffer->position()),
            reinterpret_cast<const SocketAddressCompact*>(m_read_buffer->end() - m_read_buffer->remaining() % sizeof(SocketAddressCompact)),
            std::back_inserter(l));

  // Some logic here to decided on whetever we're going to close the
  // connection or not?
  close_directly();

  m_slot_success(std::move(l));

  return true;
}

bool
TrackerUdp::process_error_output() {
  if (m_read_buffer->size_end() < 8 ||
      m_read_buffer->read_32() != m_transaction_id)
    return false;

  receive_failed("received error message: " + std::string(m_read_buffer->position(), m_read_buffer->end()));
  return true;
}

}
