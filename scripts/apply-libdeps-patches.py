"""Apply version-checked fixes to installed PlatformIO libraries.

ESPAsyncWebServer 3.12.0 allocates a fixed 2*MSS staging buffer for
asynchronous static-file responses. On no-PSRAM boards that allocation can
fail after MQTT and peripheral tasks have fragmented DRAM, leaving the
response truncated. This hook applies a small, idempotent fallback patch to
the pinned installed library. It refuses to guess when source markers change.

AsyncTCP 3.5.0 does not consume ``CONFIG_ASYNC_TCP_MAX_CONNECTIONS``. Without
an admission gate, a browser's initial connection burst can exhaust the small
8-bit DRAM heap on C3 and classic ESP32 targets before the Web middleware gets
a chance to reject work. The same hook installs a version-checked connection
gate in the pinned AsyncTCP source.

It can also be run directly:
    python scripts/apply-libdeps-patches.py --env esp32-F4R0
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
from typing import Callable, Tuple


class PatchError(RuntimeError):
    """Raised when a known source marker is missing or ambiguous."""


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def _write(path: Path, text: str) -> None:
    path.write_text(text.replace("\r\n", "\n"), encoding="utf-8", newline="\n")


def _replace_once(text: str, old: str, new: str, label: str) -> Tuple[str, bool]:
    if new in text:
        return text, False
    count = text.count(old)
    if count != 1:
        raise PatchError(f"{label}: expected one source marker, found {count}")
    return text.replace(old, new, 1), True


_UPSTREAM_FILE_FILL = """size_t AsyncFileResponse::_fillBuffer(uint8_t *data, size_t len) {
  return _content.read(data, len);
}"""

_LEGACY_FILE_FILL = """size_t AsyncFileResponse::_fillBuffer(uint8_t *data, size_t len) {
  size_t rd = _content.read(data, len);
  // LittleFS can transiently return zero while flash access is contended.
  // The response layer interprets zero as EOF, so retry briefly before
  // declaring a static file complete.
  if (len > 0 && rd == 0 && _content.position() < _content.size()) {
    for (uint8_t retry = 0; retry < 4 && rd == 0; ++retry) {
      delay(1);
      rd = _content.read(data, len);
    }
  }
  return rd;
}"""

_CURRENT_FILE_FILL = """size_t AsyncFileResponse::_fillBuffer(uint8_t *data, size_t len) {
  size_t rd = _content.read(data, len);
  // FastBee patch: a transient zero-read before EOF is not end-of-stream.
  // Retry briefly in place, then let the response layer retry on a later
  // ACK/poll instead of closing a response with bytes still outstanding.
  if (len > 0 && rd == 0 && _content.position() < _content.size()) {
    for (uint8_t retry = 0; retry < 4 && rd == 0; ++retry) {
      delay(1);
      rd = _content.read(data, len);
    }
    if (rd == 0 && _content.position() < _content.size()) {
      return RESPONSE_TRY_AGAIN;
    }
  }
  return rd;
}"""


_UPSTREAM_PENDING_BUFFER_ACCOUNTING = """        if (added_len != _send_buffer_len - _send_buffer_offset) {
          // we were not able to add entire buffer's content to tcp buffs, leave it for later
          // (this should not happen normally unless connection's TCP window suddenly changed from remote or mem pressure)
          _send_buffer_offset += added_len;
          break;
        } else {
          _send_buffer_len = _send_buffer_offset = 0;  // consider buffer empty
        }
        payloadlen += added_len;"""

_CURRENT_PENDING_BUFFER_ACCOUNTING = """        // FastBee patch: account bytes accepted by AsyncTCP before a partial
        // add breaks out. Otherwise the response consumes a send credit without
        // recording in-flight data and can never regain that credit from ACKs.
        payloadlen += added_len;
        if (added_len != _send_buffer_len - _send_buffer_offset) {
          // we were not able to add entire buffer's content to tcp buffs, leave it for later
          // (this should not happen normally unless connection's TCP window suddenly changed from remote or mem pressure)
          _send_buffer_offset += added_len;
          break;
        } else {
          _send_buffer_len = _send_buffer_offset = 0;  // consider buffer empty
        }"""

_UPSTREAM_HEADER_CREDIT_ACCOUNTING = """#if ASYNCWEBSERVER_USE_CHUNK_INFLIGHT
      _in_flight += pcb_written;
      --_in_flight_credit;  // take a credit
#endif"""

_CURRENT_HEADER_CREDIT_ACCOUNTING = """#if ASYNCWEBSERVER_USE_CHUNK_INFLIGHT
      // FastBee patch: zero progress must not consume the finite send credit.
      if (pcb_written) {
        _in_flight += pcb_written;
        if (_in_flight_credit) {
          --_in_flight_credit;  // take a credit only for bytes now in flight
        }
      }
#endif"""

_UPSTREAM_BODY_CREDIT_ACCOUNTING = """#if ASYNCWEBSERVER_USE_CHUNK_INFLIGHT
    _in_flight += payloadlen;
    --_in_flight_credit;  // take a credit
#endif"""

_CURRENT_BODY_CREDIT_ACCOUNTING = """#if ASYNCWEBSERVER_USE_CHUNK_INFLIGHT
    // FastBee patch: allocation failure, zero TCP space and transient
    // LittleFS RESPONSE_TRY_AGAIN all produce payloadlen == 0. Preserve the
    // credit so a later poll can retry instead of deadlocking the response.
    if (payloadlen) {
      _in_flight += payloadlen;
      if (_in_flight_credit) {
        --_in_flight_credit;  // take a credit only for bytes now in flight
      }
    }
#endif"""


_UPSTREAM_INFLIGHT_GUARD = """  if (_chunked || !_sendContentLength || (_sentLength > CONFIG_LWIP_TCP_WND_DEFAULT)) {
    if (!_in_flight_credit || (ASYNC_RESPONCE_BUFF_SIZE > request->client()->space())) {
      // async_ws_log_d("defer user call in_flight:%u, tcpwin:%u", _in_flight, request->client()->space());
      // take the credit back since we are ignoring this ack and rely on other inflight data acks
      if (len) {
        --_in_flight_credit;
      }
      return 0;
    }
  }"""

_LEGACY_INFLIGHT_GUARD = """  if (_chunked || !_sendContentLength || (_sentLength > CONFIG_LWIP_TCP_WND_DEFAULT)) {
    // FastBee patch: use the actual fallback buffer capacity for the in-flight
    // guard.  The upstream fixed 2*MSS check can defer forever after a
    // low-memory allocation falls back to a smaller buffer.
    const size_t tcpWindow = request->client()->space();
    const size_t responseBufferCap = _send_buffer_cap
      ? _send_buffer_cap
      : std::min<size_t>(ASYNC_RESPONCE_BUFF_SIZE, tcpWindow);
    if (!_in_flight_credit || (responseBufferCap > tcpWindow)) {
      // async_ws_log_d("defer user call in_flight:%u, tcpwin:%u", _in_flight, request->client()->space());
      // take the credit back since we are ignoring this ack and rely on other inflight data acks
      if (len) {
        --_in_flight_credit;
      }
      return 0;
    }
  }"""

_CURRENT_INFLIGHT_GUARD = """  if (_chunked || !_sendContentLength || (_sentLength > CONFIG_LWIP_TCP_WND_DEFAULT)) {
    // FastBee patch: use the actual fallback buffer capacity for new reads,
    // but always let an already-staged tail drain when TCP has room. Requiring
    // the full allocation size here can strand a partial add after its last ACK.
    const size_t tcpWindow = request->client()->space();
    const size_t responseBufferCap = _send_buffer_cap
      ? _send_buffer_cap
      : std::min<size_t>(ASYNC_RESPONCE_BUFF_SIZE, tcpWindow);
    const bool hasPendingSendBuffer =
      _send_buffer && (_send_buffer_len > _send_buffer_offset);

    // FastBee patch: writable TCP space is authoritative liveness evidence.
    // Recover one credit on a poll when an ACK event was lost/coalesced; the
    // real socket window still bounds every add(), so this cannot overfill it.
    if (!len && !_in_flight_credit && tcpWindow > 0) {
      _in_flight_credit = 1;
    }

    if (!_in_flight_credit ||
        (!hasPendingSendBuffer && responseBufferCap > tcpWindow)) {
      // async_ws_log_d("defer user call in_flight:%u, tcpwin:%u", _in_flight, request->client()->space());
      // take the credit back since we are ignoring this ack and rely on other inflight data acks
      if (len) {
        --_in_flight_credit;
      }
      return 0;
    }
  }"""


_UPSTREAM_FINAL_BUFFER_DRAIN = """          _send_buffer_len = _send_buffer_offset = 0;  // consider buffer empty
        }
      }

      auto tcp_win = request->client()->space();"""

_CURRENT_FINAL_BUFFER_DRAIN = """          _send_buffer_len = _send_buffer_offset = 0;  // consider buffer empty
        }
      }

      // FastBee patch: _sentLength counts bytes read from the source, not
      // bytes accepted by AsyncTCP.  Do not finish a known-length response
      // until the final staging buffer has been accepted in full; otherwise
      // the request closes with pending bytes and browsers report
      // ERR_CONTENT_LENGTH_MISMATCH under memory pressure.
      if (_sendContentLength && _sentLength == _contentLength) {
        _state = RESPONSE_END;
        break;
      }

      auto tcp_win = request->client()->space();"""

_UPSTREAM_EAGER_FINAL_COMPLETION = """          if (_sendContentLength && (_sentLength == _contentLength)) {
            // it was last piece of content
            _state = RESPONSE_END;
          }"""

_CURRENT_EAGER_FINAL_COMPLETION = """          // Do not mark RESPONSE_END here. The final bytes are staged but may
          // only be partially accepted by AsyncTCP; completion is checked at
          // the top of the next loop after the pending buffer drains."""


_ASYNCTCP_HEADER_PROTECTED = """protected:
  friend class AsyncTCP_detail;
  friend class AsyncServer;

  tcp_pcb *_pcb;"""

_ASYNCTCP_HEADER_PROTECTED_PATCHED = """protected:
  friend class AsyncTCP_detail;
  friend class AsyncServer;

  // FastBee patch: count only server-accepted clients so outbound MQTT and
  // other AsyncClient users do not consume the inbound Web connection budget.
  static uint32_t _fastbee_accepted_count;

  tcp_pcb *_pcb;"""

_ASYNCTCP_HEADER_CONNECT_PORT = """  uint16_t _connect_port;

  int8_t _close();"""

_ASYNCTCP_HEADER_CONNECT_PORT_PATCHED_LEGACY = """  uint16_t _connect_port;
  bool _fastbee_server_accepted;

  int8_t _close();"""

_ASYNCTCP_HEADER_CONNECT_PORT_PATCHED = """  uint16_t _connect_port;
  bool _fastbee_server_accepted;

  void _fastbee_release_accepted();
  int8_t _close();"""

_ASYNCTCP_CONSTRUCTOR_TAIL = """    _rx_timeout(0), _rx_last_ack(0), _ack_timeout(CONFIG_ASYNC_TCP_MAX_ACK_TIME), _connect_port(0) {
  _pcb = pcb;"""

_ASYNCTCP_CONSTRUCTOR_TAIL_PATCHED = """    _rx_timeout(0), _rx_last_ack(0), _ack_timeout(CONFIG_ASYNC_TCP_MAX_ACK_TIME), _connect_port(0), _fastbee_server_accepted(false) {
  _pcb = pcb;"""

_ASYNCTCP_CLIENT_SECTION = """/*
  Async TCP Client
 */

AsyncClient::AsyncClient(tcp_pcb *pcb)"""

_ASYNCTCP_CLIENT_SECTION_PATCHED_LEGACY = """/*
  Async TCP Client
 */

uint32_t AsyncClient::_fastbee_accepted_count = 0;

AsyncClient::AsyncClient(tcp_pcb *pcb)"""

_ASYNCTCP_CLIENT_SECTION_PATCHED = """/*
  Async TCP Client
 */

uint32_t AsyncClient::_fastbee_accepted_count = 0;

void AsyncClient::_fastbee_release_accepted() {
  if (__atomic_exchange_n(
        &_fastbee_server_accepted, false, __ATOMIC_ACQ_REL
      )) {
    __atomic_fetch_sub(&_fastbee_accepted_count, 1U, __ATOMIC_RELAXED);
  }
}

AsyncClient::AsyncClient(tcp_pcb *pcb)"""

_ASYNCTCP_DESTRUCTOR = """AsyncClient::~AsyncClient() {
  if (_pcb) {
    _close();
  }
}"""

_ASYNCTCP_DESTRUCTOR_PATCHED_LEGACY = """AsyncClient::~AsyncClient() {
  if (_fastbee_server_accepted) {
    __atomic_fetch_sub(&_fastbee_accepted_count, 1U, __ATOMIC_RELAXED);
    _fastbee_server_accepted = false;
  }
  if (_pcb) {
    _close();
  }
}"""

_ASYNCTCP_DESTRUCTOR_PATCHED = """AsyncClient::~AsyncClient() {
  _fastbee_release_accepted();
  if (_pcb) {
    _close();
  }
}"""

_ASYNCTCP_CLOSE = """int8_t AsyncClient::_close() {
  // ets_printf("X: 0x%08x\\n", (uint32_t)this);
  int8_t err = _tcp_close(&_pcb, this);"""

_ASYNCTCP_CLOSE_PATCHED = """int8_t AsyncClient::_close() {
  // Release the admission slot when closing starts; destruction may be
  // delayed indefinitely while user code still owns the AsyncClient.
  _fastbee_release_accepted();
  // ets_printf("X: 0x%08x\\n", (uint32_t)this);
  int8_t err = _tcp_close(&_pcb, this);"""

_ASYNCTCP_ABORT = """int8_t AsyncClient::abort() {
  int8_t err = _tcp_abort(&_pcb, this);"""

_ASYNCTCP_ABORT_PATCHED = """int8_t AsyncClient::abort() {
  // Explicit abort detaches tcp_err; release before callbacks or retained
  // request ownership can delay AsyncClient destruction indefinitely.
  _fastbee_release_accepted();
  int8_t err = _tcp_abort(&_pcb, this);"""

_ASYNCTCP_TCP_ERROR = """  // enqueue event to be processed in the async task for the user callback
  lwip_tcp_event_packet_t *e = new (std::nothrow) lwip_tcp_event_packet_t{LWIP_TCP_ERROR, client};"""

_ASYNCTCP_TCP_ERROR_PATCHED = """  // LwIP has already freed the PCB. Release the admission slot before the
  // event allocation so an OOM here cannot permanently consume the limit.
  if (client) {
    client->_fastbee_release_accepted();
  }

  // enqueue event to be processed in the async task for the user callback
  lwip_tcp_event_packet_t *e = new (std::nothrow) lwip_tcp_event_packet_t{LWIP_TCP_ERROR, client};"""

_ASYNCTCP_ACCEPT_HEAD = """int8_t AsyncTCP_detail::tcp_accept(void *arg, tcp_pcb *pcb, int8_t err) {
  if (!pcb) {
    async_tcp_log_e("_accept failed: pcb is NULL");
    return ERR_ABRT;
  }
  auto server = reinterpret_cast<AsyncServer *>(arg);"""

_ASYNCTCP_ACCEPT_HEAD_PATCHED = """int8_t AsyncTCP_detail::tcp_accept(void *arg, tcp_pcb *pcb, int8_t err) {
  if (!pcb) {
    async_tcp_log_e("_accept failed: pcb is NULL");
    return ERR_ABRT;
  }

  // FastBee patch: reject a connection before allocating AsyncClient,
  // AsyncWebServerRequest, response objects and pbufs. The configured limit is
  // target-specific (4 on C3/classic, 6 on C6, 8 on S3).
#if defined(ARDUINO) && !defined(LIBRETINY)
#ifndef FASTBEE_TCP_ACCEPT_MIN_FREE8
#define FASTBEE_TCP_ACCEPT_MIN_FREE8 8192U
#endif
#ifndef FASTBEE_TCP_ACCEPT_MIN_BLK8
#define FASTBEE_TCP_ACCEPT_MIN_BLK8 1536U
#endif
#ifndef CONFIG_ASYNC_TCP_MAX_CONNECTIONS
#define CONFIG_ASYNC_TCP_MAX_CONNECTIONS 4U
#endif
  const uint32_t free8 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t block8 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t accepted = __atomic_load_n(
    &AsyncClient::_fastbee_accepted_count, __ATOMIC_RELAXED
  );
  if (free8 < FASTBEE_TCP_ACCEPT_MIN_FREE8 ||
      block8 < FASTBEE_TCP_ACCEPT_MIN_BLK8 ||
      accepted >= CONFIG_ASYNC_TCP_MAX_CONNECTIONS) {
    static uint32_t last_reject_log_ms = 0;
    const uint32_t now_ms = millis();
    if (now_ms - last_reject_log_ms >= 1000U) {
      last_reject_log_ms = now_ms;
      async_tcp_log_e(
        "_accept rejected: free8=%u blk8=%u conn=%u (min %u/%u max %u)",
        (unsigned)free8, (unsigned)block8, (unsigned)accepted,
        (unsigned)FASTBEE_TCP_ACCEPT_MIN_FREE8,
        (unsigned)FASTBEE_TCP_ACCEPT_MIN_BLK8,
        (unsigned)CONFIG_ASYNC_TCP_MAX_CONNECTIONS
      );
    }
    tcp_abort(pcb);
    return ERR_ABRT;
  }
#endif

  auto server = reinterpret_cast<AsyncServer *>(arg);"""

_ASYNCTCP_ACCEPT_CLIENT = """    if (c && c->pcb()) {
      c->setNoDelay(server->_noDelay);

      lwip_tcp_event_packet_t *e = new (std::nothrow) lwip_tcp_event_packet_t{LWIP_TCP_ACCEPT, c};"""

_ASYNCTCP_ACCEPT_CLIENT_PATCHED = """    if (c && c->pcb()) {
      c->_fastbee_server_accepted = true;
      __atomic_fetch_add(&AsyncClient::_fastbee_accepted_count, 1U, __ATOMIC_RELAXED);
      c->setNoDelay(server->_noDelay);

      lwip_tcp_event_packet_t *e = new (std::nothrow) lwip_tcp_event_packet_t{LWIP_TCP_ACCEPT, c};"""

_ASYNCTCP_ACCEPT_EVENT_FAILURE = """      c->_pcb = nullptr;
      tcp_abort(pcb);
      async_tcp_log_e("_accept failed: couldn't accept client");"""

_ASYNCTCP_ACCEPT_EVENT_FAILURE_PATCHED_R11 = """      c->_pcb = nullptr;
      tcp_abort(pcb);
      c->_fastbee_release_accepted();
      delete c;
      async_tcp_log_e("_accept failed: couldn't accept client");"""

_ASYNCTCP_ACCEPT_EVENT_FAILURE_PATCHED = """      // This client was never published. Detach callbacks on the LwIP
      // thread before abort, otherwise tcp_error can queue its freed address.
      _reset_tcp_callbacks(pcb, c);
      c->_pcb = nullptr;
      tcp_abort(pcb);
      c->_fastbee_release_accepted();
      delete c;
      async_tcp_log_e("_accept failed: couldn't accept client");"""

_ASYNCTCP_ACCEPT_EVENT_FAILURE_PATCHED_LEGACY = """      c->_pcb = nullptr;
      tcp_abort(pcb);
      if (c->_fastbee_server_accepted) {
        __atomic_fetch_sub(&AsyncClient::_fastbee_accepted_count, 1U, __ATOMIC_RELAXED);
        c->_fastbee_server_accepted = false;
      }
      async_tcp_log_e("_accept failed: couldn't accept client");"""

_ASYNCTCP_ACCEPT_EVENT_FAILURE_PATCHED_COUNTER = """      c->_pcb = nullptr;
      tcp_abort(pcb);
      if (c->_fastbee_server_accepted) {
        __atomic_fetch_sub(&AsyncClient::_fastbee_accepted_count, 1U, __ATOMIC_RELAXED);
        c->_fastbee_server_accepted = false;
      }
      delete c;
      async_tcp_log_e("_accept failed: couldn't accept client");"""


_WEB_REQUEST_HEADER_TIMEOUT_RECOVERY = """  bool _sent = false;                            // response is sent
  bool _paused = false;                          // request is paused (request continuation)"""

_WEB_REQUEST_HEADER_TIMEOUT_RECOVERY_PATCHED = """  bool _sent = false;                            // response is sent
  bool _paused = false;                          // request is paused (request continuation)
  // FastBee patch: one bounded retry for a response whose ACK event was lost.
  bool _ackTimeoutRecoveryAttempted = false;"""

_WEB_REQUEST_ON_POLL = """    _response->_ack(this, 0, 0);
  }
}

void AsyncWebServerRequest::_onAck"""

_WEB_REQUEST_ON_POLL_PATCHED = """    _response->_ack(this, 0, 0);
    // FastBee patch: a poll can observe peer drain after a lost ACK event.
    if (_response->_finished()) _client->close();
  }
}

void AsyncWebServerRequest::_onAck"""

_WEB_REQUEST_ON_ACK = """void AsyncWebServerRequest::_onAck(size_t len, uint32_t time) {
  // os_printf("a:%u:%u\\n", len, time);
  if (!_response) {
    return;
  }"""

_ASYNCTCP_TCP_ERROR_PATCHED_POOLED = """  // LwIP has already freed the PCB. Release the admission slot before the
  // event allocation so an OOM here cannot permanently consume the limit.
  if (client) {
    client->_fastbee_release_accepted();
  }

  // Enqueue the callback even when heap allocation fails. The fixed pool is
  // reserved for error cleanup, so WebRequest disconnect handlers and static
  // response leases are not stranded until a watchdog fires.
  lwip_tcp_event_packet_t *e = new (std::nothrow) lwip_tcp_event_packet_t{LWIP_TCP_ERROR, client};
  if (!e) {
    e = _fastbee_alloc_error_event(client);
  }
  if (!e) {
    async_tcp_log_e("Failed to allocate error event packet; emergency pool exhausted");
    return;
  }"""

_ASYNCTCP_EVENT_PACKET_CONSTRUCTOR = """  inline lwip_tcp_event_packet_t(lwip_tcp_event_t _event, AsyncClient *_client) : next(nullptr), event(_event), client(_client){};"""

_ASYNCTCP_EVENT_PACKET_CONSTRUCTOR_PATCHED = """  inline lwip_tcp_event_packet_t() : next(nullptr), event(LWIP_TCP_ERROR), client(nullptr){};
  inline lwip_tcp_event_packet_t(lwip_tcp_event_t _event, AsyncClient *_client) : next(nullptr), event(_event), client(_client){};"""

_ASYNCTCP_QUEUE_GLOBALS = """static SimpleIntrusiveList<lwip_tcp_event_packet_t> _async_queue;
static TaskHandle_t _async_service_task_handle = NULL;"""

_ASYNCTCP_QUEUE_GLOBALS_PATCHED = """static SimpleIntrusiveList<lwip_tcp_event_packet_t> _async_queue;
static TaskHandle_t _async_service_task_handle = NULL;

// FastBee patch: tcp_error runs after LwIP has already destroyed the PCB, so
// losing its heap-allocated event also loses the only disconnect callback.
// Keep a bounded, allocation-free emergency pool sized above the maximum
// accepted Web clients; normal events still use the upstream heap path.
#ifndef FASTBEE_ASYNC_TCP_ERROR_EVENT_POOL_SIZE
#define FASTBEE_ASYNC_TCP_ERROR_EVENT_POOL_SIZE 12U
#endif
static lwip_tcp_event_packet_t
  _fastbee_error_event_pool[FASTBEE_ASYNC_TCP_ERROR_EVENT_POOL_SIZE];
static bool
  _fastbee_error_event_pool_used[FASTBEE_ASYNC_TCP_ERROR_EVENT_POOL_SIZE] = {};
static uint32_t _fastbee_error_event_pool_exhausted = 0;

static lwip_tcp_event_packet_t *_fastbee_alloc_error_event(AsyncClient *client) {
  for (size_t i = 0; i < FASTBEE_ASYNC_TCP_ERROR_EVENT_POOL_SIZE; ++i) {
    bool expected = false;
    if (__atomic_compare_exchange_n(
          &_fastbee_error_event_pool_used[i], &expected, true, false,
          __ATOMIC_ACQ_REL, __ATOMIC_RELAXED
        )) {
      auto *event = &_fastbee_error_event_pool[i];
      event->next = nullptr;
      event->event = LWIP_TCP_ERROR;
      event->client = client;
      return event;
    }
  }
  __atomic_fetch_add(
    &_fastbee_error_event_pool_exhausted, 1U, __ATOMIC_RELAXED
  );
  return nullptr;
}

static bool _fastbee_release_error_event(lwip_tcp_event_packet_t *event) {
  for (size_t i = 0; i < FASTBEE_ASYNC_TCP_ERROR_EVENT_POOL_SIZE; ++i) {
    if (event == &_fastbee_error_event_pool[i]) {
      event->next = nullptr;
      event->client = nullptr;
      __atomic_store_n(
        &_fastbee_error_event_pool_used[i], false, __ATOMIC_RELEASE
      );
      return true;
    }
  }
  return false;
}"""

_ASYNCTCP_FREE_EVENT = """static void _free_event(lwip_tcp_event_packet_t *evpkt) {
  if ((evpkt->event == LWIP_TCP_RECV) && (evpkt->recv.pb != nullptr)) {
    pbuf_free(evpkt->recv.pb);
  }
  delete evpkt;
}"""

_ASYNCTCP_FREE_EVENT_PATCHED = """static void _free_event(lwip_tcp_event_packet_t *evpkt) {
  if ((evpkt->event == LWIP_TCP_RECV) && (evpkt->recv.pb != nullptr)) {
    pbuf_free(evpkt->recv.pb);
  }
  if (_fastbee_release_error_event(evpkt)) {
    return;
  }
  delete evpkt;
}"""

_WEB_REQUEST_ON_ACK_PATCHED = """void AsyncWebServerRequest::_onAck(size_t len, uint32_t time) {
  // A real ACK proves that the peer is advancing. It arms one new recovery
  // opportunity for a later, independent dropped ACK event.
  if (len) {
    _ackTimeoutRecoveryAttempted = false;
  }
  // os_printf("a:%u:%u\\n", len, time);
  if (!_response) {
    return;
  }"""

_WEB_REQUEST_ON_TIMEOUT = """void AsyncWebServerRequest::_onTimeout(uint32_t time) {
  (void)time;
  // os_printf("TIMEOUT: %u, state: %s\\n", time, _client->stateToString());
  // We do not need to lock the shared pointer here as we do no work after closing the client
  _client->close();
}"""

_WEB_REQUEST_ON_TIMEOUT_PATCHED = """void AsyncWebServerRequest::_onTimeout(uint32_t time) {
  (void)time;
  std::shared_ptr<AsyncWebServerRequest> self = _this;

  // FastBee patch: AsyncTCP checks ACK timeout before its normal poll callback.
  // Give an unfinished, writable response one bounded recovery attempt, but
  // keep the connection only when the response actually queued new bytes.
  if (!_ackTimeoutRecoveryAttempted &&
      _response && !_response->_finished() &&
      _client && _client->canSend()) {
    _ackTimeoutRecoveryAttempted = true;
    if (_response->_ack(this, 0, 0) > 0) {
      return;
    }
  }

  if (_client) {
    _client->close();
  }
}"""


def _patch_file_zero_read(text: str) -> Tuple[str, bool]:
    """Install or upgrade the AsyncFileResponse transient zero-read fix."""
    if _CURRENT_FILE_FILL in text:
        return text, False

    candidates = [source for source in (_UPSTREAM_FILE_FILL, _LEGACY_FILE_FILL) if source in text]
    if len(candidates) != 1:
        raise PatchError(
            "file zero-read retry: expected one upstream/legacy source marker, "
            f"found {len(candidates)}"
        )
    return text.replace(candidates[0], _CURRENT_FILE_FILL, 1), True


def _patch_send_progress_accounting(text: str) -> Tuple[str, bool]:
    """Keep AsyncWebServer send credits aligned with bytes accepted by TCP."""
    changed = False
    replacements = (
        (
            _UPSTREAM_PENDING_BUFFER_ACCOUNTING,
            _CURRENT_PENDING_BUFFER_ACCOUNTING,
            "partial pending-buffer accounting",
        ),
        (
            _UPSTREAM_HEADER_CREDIT_ACCOUNTING,
            _CURRENT_HEADER_CREDIT_ACCOUNTING,
            "zero-progress header credit accounting",
        ),
        (
            _UPSTREAM_BODY_CREDIT_ACCOUNTING,
            _CURRENT_BODY_CREDIT_ACCOUNTING,
            "zero-progress body credit accounting",
        ),
    )
    for old, new, label in replacements:
        if new in text:
            continue
        text, did_change = _replace_once(text, old, new, label)
        changed = changed or did_change
    return text, changed


def _patch_inflight_liveness(text: str) -> Tuple[str, bool]:
    """Ensure pending bytes and writable polls cannot be stranded by credits."""
    if _CURRENT_INFLIGHT_GUARD in text:
        return text, False

    candidates = [
        source
        for source in (_UPSTREAM_INFLIGHT_GUARD, _LEGACY_INFLIGHT_GUARD)
        if source in text
    ]
    if len(candidates) != 1:
        raise PatchError(
            "in-flight liveness guard: expected one upstream/legacy source marker, "
            f"found {len(candidates)}"
        )
    return text.replace(candidates[0], _CURRENT_INFLIGHT_GUARD, 1), True


def _patch_final_buffer_completion(text: str) -> Tuple[str, bool]:
    """Keep a response alive until its final staged bytes reach AsyncTCP."""
    changed = False
    # Later upgrades insert terminal-chunk handling before this check and a
    # batch allowance after it; the known-length completion block stays intact.
    check_start = _CURRENT_FINAL_BUFFER_DRAIN.index("      // FastBee patch:")
    final_drain_contract = _CURRENT_FINAL_BUFFER_DRAIN[check_start:].rsplit("\n      auto tcp_win", 1)[0]
    final_peer_drain_contract = final_drain_contract.replace("_state = RESPONSE_END;", "_state = RESPONSE_WAIT_ACK;")
    if final_drain_contract not in text and final_peer_drain_contract not in text:
        text, did_change = _replace_once(
            text,
            _UPSTREAM_FINAL_BUFFER_DRAIN,
            _CURRENT_FINAL_BUFFER_DRAIN,
            "pending final-buffer drain",
        )
        changed = changed or did_change
    if _CURRENT_EAGER_FINAL_COMPLETION not in text:
        text, did_change = _replace_once(
            text,
            _UPSTREAM_EAGER_FINAL_COMPLETION,
            _CURRENT_EAGER_FINAL_COMPLETION,
            "eager final-buffer completion",
        )
        changed = changed or did_change
    return text, changed


def _patch_web_response_impl(path: Path) -> bool:
    text = _read(path)
    marker = "FastBee patch: dynamic send buffer"
    changed = False
    old = "  std::unique_ptr<std::array<uint8_t, ASYNC_RESPONCE_BUFF_SIZE> > _send_buffer;"
    new = (
        "  // FastBee patch: dynamic send buffer with a small-buffer fallback.\n"
        "  // A fixed 2*MSS allocation can fail on the no-PSRAM ESP32.\n"
        "  std::unique_ptr<uint8_t[]> _send_buffer;\n"
        "  size_t _send_buffer_cap{0};"
    )
    if marker not in text:
        text, changed = _replace_once(text, old, new, "WebResponseImpl.h send buffer")
    pending = "  size_t _fastbeePendingOutputBytes{0};"
    if text.count(pending) != 2:
        if pending in text:
            raise PatchError("WebResponseImpl.h partial pending output patch")
        anchor = "  size_t _writtenHeadersLength{0};"
        if text.count(anchor) != 2:
            raise PatchError("WebResponseImpl.h expected two response header counters")
        text = text.replace(anchor, anchor + "\n" + pending)
        changed = True
    if changed:
        _write(path, text)
    return changed


def _patch_bounded_response_batch(text: str) -> Tuple[str, bool]:
    marker = "FastBee patch: bound a low-DRAM response batch before TCP output"
    if marker in text:
        return text, False
    additions = [
        ('#include <utility>', '#include <utility>\n#if defined(ESP32)\n#include <esp_heap_caps.h>\n#endif'),
        ('  (void)time;\n  if (!_sourceValid()) {', '''  (void)time;
  // FastBee patch: bound a low-DRAM response batch before TCP output.
  // Queuing the entire TCP window before send() starves Wi-Fi's own TX
  // allocations even when the staging allocation itself still succeeds.
  // PSRAM cannot satisfy those internal allocations. Leave high-headroom
  // targets on the existing throughput path; ACKs advance constrained ones.
  const size_t responseBatchLimit =
#if defined(ESP32)
    (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 49152U ||
     heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 8192U)
      ? CONFIG_LWIP_TCP_MSS : 0;
#else
    0;
#endif
  if (!_sourceValid()) {'''),
        ('    do {\n      if (_send_buffer_len && _send_buffer) {', '''    do {
      if (responseBatchLimit && payloadlen >= responseBatchLimit) break;
      if (_send_buffer_len && _send_buffer) {'''),
        ('request->client()->add(reinterpret_cast<char *>(_send_buffer.get() + _send_buffer_offset), _send_buffer_len - _send_buffer_offset);',
         '''request->client()->add(reinterpret_cast<char *>(_send_buffer.get() + _send_buffer_offset),
            responseBatchLimit ? std::min(_send_buffer_len - _send_buffer_offset, responseBatchLimit - payloadlen)
                               : _send_buffer_len - _send_buffer_offset);'''),
        ('      auto tcp_win = request->client()->space();', '''      if (responseBatchLimit && payloadlen >= responseBatchLimit) break;
      auto tcp_win = request->client()->space();
      if (responseBatchLimit) tcp_win = std::min<size_t>(tcp_win, responseBatchLimit - payloadlen);'''),
        ('        for (size_t cap : kFallbackSizes) {', '''        for (size_t cap : kFallbackSizes) {
          if (responseBatchLimit && cap > responseBatchLimit) continue;'''),
    ]
    for old, new in additions:
        text, _ = _replace_once(text, old, new, "bounded response batch")
    return text, True


def _patch_web_responses(path: Path) -> bool:
    text = _read(path)
    changed = False
    replacements = [
        (
            "request->client()->add(reinterpret_cast<char *>(_send_buffer->data() + _send_buffer_offset), _send_buffer_len - _send_buffer_offset);",
            "request->client()->add(reinterpret_cast<char *>(_send_buffer.get() + _send_buffer_offset), _send_buffer_len - _send_buffer_offset);",
            "pending send buffer pointer",
        ),
        (
            """      if (!_send_buffer) {
        auto p = new (std::nothrow) std::array<uint8_t, ASYNC_RESPONCE_BUFF_SIZE>;
        if (p) {
          _send_buffer.reset(p);
          _send_buffer_len = _send_buffer_offset = 0;
        } else {
          break;  // OOM
        }
      }""",
            """      if (!_send_buffer) {
        // FastBee patch: try the normal buffer first, then progressively
        // smaller buffers so a low-memory device can still finish the page.
        static constexpr size_t kFallbackSizes[] = {ASYNC_RESPONCE_BUFF_SIZE, 1436, 768, 256};
        for (size_t cap : kFallbackSizes) {
          auto p = new (std::nothrow) uint8_t[cap];
          if (p) {
            _send_buffer.reset(p);
            _send_buffer_cap = cap;
            _send_buffer_len = _send_buffer_offset = 0;
            break;
          }
        }
        if (!_send_buffer) {
          break;  // Retry on the next ACK/poll when memory is available.
        }
      }""",
            "send buffer allocation fallback",
        ),
        (
            "_fillBufferAndProcessTemplates(_send_buffer->data() + 6, std::min(_send_buffer->size(), tcp_win) - 8)",
            "_fillBufferAndProcessTemplates(_send_buffer.get() + 6, std::min(_send_buffer_cap, tcp_win) - 8)",
            "chunked send buffer capacity",
        ),
        (
            "size_t maxLen = std::min(_send_buffer->size(), tcp_win);",
            "size_t maxLen = std::min(_send_buffer_cap, tcp_win);",
            "non-chunked send buffer capacity",
        ),
        (
            "_fillBufferAndProcessTemplates(_send_buffer->data(), maxLen)",
            "_fillBufferAndProcessTemplates(_send_buffer.get(), maxLen)",
            "non-chunked send buffer pointer",
        ),
    ]
    for old, new, label in replacements:
        if label in ("send buffer allocation fallback", "pending send buffer pointer") and "FastBee patch: bound a low-DRAM response batch before TCP output" in text:
            continue
        # The chunked read is expanded into a guarded form below.  Once that
        # form is present the original one-line marker is intentionally gone.
        if label == "chunked send buffer capacity" and "chunkCap = std::min" in text:
            continue
        if new in text:
            continue
        text, did_change = _replace_once(text, old, new, label)
        changed = changed or did_change

    text, did_change = _patch_file_zero_read(text)
    changed = changed or did_change

    text, did_change = _patch_send_progress_accounting(text)
    changed = changed or did_change

    text, did_change = _patch_inflight_liveness(text)
    changed = changed or did_change

    text, did_change = _patch_final_buffer_completion(text)
    changed = changed or did_change

    byte_replacements = [
        ("_send_buffer->data()[0]", "_send_buffer[0]", "chunk header byte 0"),
        ("_send_buffer->data()[1]", "_send_buffer[1]", "chunk header byte 1"),
        ("_send_buffer->data()[2]", "_send_buffer[2]", "chunk header byte 2"),
        ("_send_buffer->data()[3]", "_send_buffer[3]", "chunk header byte 3"),
        ("_send_buffer->data()[4]", "_send_buffer[4]", "chunk header byte 4"),
        ("_send_buffer->data()[5]", "_send_buffer[5]", "chunk header byte 5"),
        ("_send_buffer->at(readLen + 6)", "_send_buffer[readLen + 6]", "chunk trailer byte 0"),
        ("_send_buffer->at(readLen + 7)", "_send_buffer[readLen + 7]", "chunk trailer byte 1"),
    ]
    for old, new, label in byte_replacements:
        if new in text:
            continue
        text, did_change = _replace_once(text, old, new, label)
        changed = changed or did_change

    old_chunk_read = """        size_t const readLen =
          _fillBufferAndProcessTemplates(_send_buffer.get() + 6, std::min(_send_buffer_cap, tcp_win) - 8);  // reserve 8 bytes for chunk size data"""
    new_chunk_read = """        const size_t chunkCap = std::min(_send_buffer_cap, tcp_win);
        if (chunkCap <= 8) {
          break;
        }
        size_t const readLen =
          _fillBufferAndProcessTemplates(_send_buffer.get() + 6, chunkCap - 8);  // reserve 8 bytes for chunk size data"""
    if new_chunk_read not in text:
        text, did_change = _replace_once(text, old_chunk_read, new_chunk_read, "small TCP window guard")
        changed = changed or did_change

    old_reset = "      _send_buffer.reset();\n"
    new_reset = "      _send_buffer.reset();\n      _send_buffer_cap = 0;\n"
    if new_reset not in text:
        text, did_change = _replace_once(text, old_reset, new_reset, "send buffer release")
        changed = changed or did_change

    if "_send_buffer->" in text:
        raise PatchError("WebResponses.cpp still contains std::array send-buffer access")
    if "_send_buffer_cap" not in text:
        raise PatchError("WebResponses.cpp is missing dynamic capacity state")
    if "hasPendingSendBuffer" not in text:
        raise PatchError("WebResponses.cpp is missing pending-buffer liveness protection")
    if "writable TCP space is authoritative liveness evidence" not in text:
        raise PatchError("WebResponses.cpp is missing poll credit recovery")
    if "zero progress must not consume the finite send credit" not in text:
        raise PatchError("WebResponses.cpp is missing zero-progress header credit protection")
    if "credit so a later poll can retry instead of deadlocking" not in text:
        raise PatchError("WebResponses.cpp is missing zero-progress body credit protection")
    if "account bytes accepted by AsyncTCP before a partial" not in text:
        raise PatchError("WebResponses.cpp is missing partial-add byte accounting")
    if "ERR_CONTENT_LENGTH_MISMATCH under memory pressure" not in text:
        raise PatchError("WebResponses.cpp is missing pending final-buffer protection")
    if _UPSTREAM_EAGER_FINAL_COMPLETION in text:
        raise PatchError("WebResponses.cpp still completes before the final buffer drains")
    text, did_change = _patch_bounded_response_batch(text)
    changed = changed or did_change
    text, did_change = _patch_bounded_basic_response(text)
    changed = changed or did_change
    text, did_change = _patch_outstanding_response_budget(text)
    changed = changed or did_change
    text, did_change = _patch_basic_queued_accounting(text)
    changed = changed or did_change
    text, did_change = _patch_chunked_terminal_drain(text)
    changed = changed or did_change
    text, did_change = _patch_low_dram_staging_capacity(text)
    changed = changed or did_change
    text, did_change = _patch_pending_output_retry(text)
    changed = changed or did_change
    text, did_change = _patch_peer_drained_completion(text)
    changed = changed or did_change
    if changed:
        _write(path, text)
    return changed


def _patch_peer_drained_completion(text: str) -> Tuple[str, bool]:
    """Do not close a successful response while lwIP still owns body bytes."""
    marker = "FastBee patch: peer-drained completion"
    if text.count(marker) == 2:
        return text, False
    if marker in text:
        raise PatchError("WebResponses.cpp partial peer-drained completion")
    def _replace_once(source: str, old: str, new: str, label: str) -> Tuple[str, bool]:
        # Idempotence is checked for the whole upgrade above. New fragments
        # here may be empty or already occur elsewhere in the same function.
        if source.count(old) != 1:
            raise PatchError(f"{label}: expected one peer-drain upgrade anchor")
        return source.replace(old, new, 1), True
    for response_type, following in (
        ("AsyncBasicResponse", "AsyncAbstractResponse::AsyncAbstractResponse"),
        ("AsyncAbstractResponse", "size_t AsyncAbstractResponse::_readDataFromCacheOrContent"),
    ):
        start = text.find(f"size_t {response_type}::write_send_buffs(")
        end = text.find(following, start)
        if start < 0 or end <= start:
            raise PatchError("WebResponses.cpp missing peer-drain boundaries")
        body = text[start:end]
        body, _ = _replace_once(body, "  _ackedLength += len;", """  // Clamp callback accounting; completion uses actual TCP space below.
  _ackedLength += std::min(len, _writtenLength - std::min(_ackedLength, _writtenLength));""",
                                "bounded ACK accounting")
        is_abstract = response_type == "AsyncAbstractResponse"
        old_wait = ("  if (_state == RESPONSE_WAIT_ACK && !_send_buffer_len) _state = RESPONSE_END;"
                    if is_abstract else "  if (_state == RESPONSE_WAIT_ACK) _state = RESPONSE_END;")
        pending = " && !_send_buffer_len" if is_abstract else ""
        new_wait = """  // FastBee patch: peer-drained completion. A queued final byte is not
  // delivered yet. Real sndbuf recovery also handles lost/duplicate ACK
  // callbacks without trusting counters or extending the existing timeout.
  if (_state == RESPONSE_WAIT_ACK""" + pending + """) {
    if (request->client()->space() == TCP_SND_BUF) _state = RESPONSE_END;
    return 0;
  }"""
        # Change completion assignments before inserting the only real END.
        body, _ = _replace_once(body, old_wait, "  // FASTBEE_PEER_DRAIN_ENTRY", "peer-drain entry")
        if is_abstract:
            old_tail = """  // implicit check
  if (_state == RESPONSE_WAIT_ACK) {
    // we do not need to wait for any acks actually if we won't send any more data,
    // connection would be closed gracefully with last piece of data (in AsyncWebServerRequest::_onAck)
    _state = RESPONSE_END;
  }
"""
            body, _ = _replace_once(body, old_tail, "", "remove implicit abstract completion")
            body, _ = _replace_once(body, "    _writtenLength += pcb_written;\n    _writtenHeadersLength", "    _writtenHeadersLength",
                                    "count final header batch once")
            anchor = "    if (_writtenHeadersLength < _assembled_headers.length()) {"
            body, _ = _replace_once(body, anchor, anchor + "\n      _writtenLength += pcb_written;", "count partial header batch")
            # END is never used for staging or source completion now.
            body = body.replace("_state = RESPONSE_END;", "_state = RESPONSE_WAIT_ACK;")
            body, _ = _replace_once(body, """      if (_fastbeePendingOutputBytes && _state == RESPONSE_END) {
        _state = RESPONSE_WAIT_ACK;
      }
""", "", "remove obsolete failed-final transition")
        else:
            body, _ = _replace_once(body, """  // implicit complete
  if (_state == RESPONSE_WAIT_ACK && !_fastbeePendingOutputBytes) {
    _state = RESPONSE_END;
  }
""", "", "remove implicit basic completion")
            body, _ = _replace_once(body,
                "_state = _fastbeePendingOutputBytes ? RESPONSE_WAIT_ACK : RESPONSE_END;",
                "_state = RESPONSE_WAIT_ACK;", "basic peer wait")
        body = body.replace("  // FASTBEE_PEER_DRAIN_ENTRY", new_wait)
        text = text[:start] + body + text[end:]
    return text, True


def _patch_pending_output_retry(text: str) -> Tuple[str, bool]:
    """Retry queued output without adding bytes or reopening the send budget."""
    marker = "FastBee patch: retry only previously failed TCP output"
    if text.count(marker) == 2:
        return text, False
    if marker in text:
        raise PatchError("WebResponses.cpp partial pending output retry")
    for response_type, following in (
        ("AsyncBasicResponse", "AsyncAbstractResponse::AsyncAbstractResponse"),
        ("AsyncAbstractResponse", "size_t AsyncAbstractResponse::_readDataFromCacheOrContent"),
    ):
        start = text.find(f"size_t {response_type}::write_send_buffs(")
        end = text.find(following, start)
        if start < 0 or end <= start:
            raise PatchError("WebResponses.cpp missing output retry method boundaries")
        body = text[start:end]
        anchor = "  _ackedLength += len;"
        body, _ = _replace_once(body, anchor, anchor + """
  // FastBee patch: retry only previously failed TCP output. add() already
  // consumed sndbuf and source bytes, so do not add them again or require
  // another MSS of room. One retry per callback; the existing ACK timeout
  // still closes a stalled peer. A successful retry clears this obligation.
  if (_fastbeePendingOutputBytes) {
    if (!request->client()->send()) return 0;
    const size_t outputProgress = _fastbeePendingOutputBytes;
    _fastbeePendingOutputBytes = 0;
    return outputProgress;
  }""", "pending output before flow control")
        old = """      if (!request->client()->send()) {
        // something is wrong, what should we do here?
        request->client()->close();
        return 0;
      }
      return pcb_written;"""
        new = """      if (!request->client()->send()) {
        _fastbeePendingOutputBytes = pcb_written;
      }
      return pcb_written;"""
        body, _ = _replace_once(body, old, new, "partial header output retry")
        if response_type == "AsyncBasicResponse":
            body, _ = _replace_once(body, "    request->client()->send();", """    if (!request->client()->send()) {
      _fastbeePendingOutputBytes = payloadlen + pcb_written;
    }""", "basic pending output bytes")
            body, _ = _replace_once(body, "      _state = RESPONSE_END;", """      _state = _fastbeePendingOutputBytes ? RESPONSE_WAIT_ACK : RESPONSE_END;""",
                                    "basic failed final output")
            body = body.replace("if (_state == RESPONSE_WAIT_ACK) {",
                                "if (_state == RESPONSE_WAIT_ACK && !_fastbeePendingOutputBytes) {")
        else:
            body, _ = _replace_once(body, "    request->client()->send();", """    if (!request->client()->send()) {
      _fastbeePendingOutputBytes = payloadlen;
      if (_fastbeePendingOutputBytes && _state == RESPONSE_END) {
        _state = RESPONSE_WAIT_ACK;
      }
    }""", "abstract pending output bytes")
        text = text[:start] + body + text[end:]
    return text, True


def _patch_bounded_basic_response(text: str) -> Tuple[str, bool]:
    marker = "FastBee patch: bound basic JSON responses before TCP output"
    if marker in text:
        return text, False
    old = """size_t AsyncBasicResponse::write_send_buffs(AsyncWebServerRequest *request, size_t len, uint32_t time) {
  (void)time;"""
    new = old + """
  // FastBee patch: bound basic JSON responses before TCP output.
  const size_t responseBatchLimit =
#if defined(ESP32)
    (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 49152U ||
     heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 8192U)
      ? CONFIG_LWIP_TCP_MSS : 0;
#else
    0;
#endif"""
    text, _ = _replace_once(text, old, new, "bounded basic response budget")
    old = "request->client()->write(_content.c_str() + _sentLength, _content.length() - _sentLength);"
    new = """request->client()->write(_content.c_str() + _sentLength,
      responseBatchLimit ? std::min(_content.length() - _sentLength,
        responseBatchLimit > payloadlen ? responseBatchLimit - payloadlen : size_t(0))
        : _content.length() - _sentLength);"""
    text, _ = _replace_once(text, old, new, "bounded basic response write")
    return text, True


def _patch_outstanding_response_budget(text: str) -> Tuple[str, bool]:
    """Upgrade both existing per-callback limits to actual TCP in-flight limits."""
    marker = "FastBee patch: account outstanding TCP bytes across polls"
    if text.count(marker) == 2:
        return text, False
    if marker in text:
        raise PatchError("WebResponses.cpp has a partial outstanding response budget")
    if text.count("  _ackedLength += len;") != 2:
        raise PatchError("WebResponses.cpp expected two ACK accounting anchors")
    text = text.replace("  _ackedLength += len;", "")
    for response_type in ("AsyncBasicResponse", "AsyncAbstractResponse"):
        signature = f"size_t {response_type}::write_send_buffs(AsyncWebServerRequest *request, size_t len, uint32_t time) {{\n  (void)time;"
        text, _ = _replace_once(text, signature, signature + """
  // ACK accounting and completion must precede every flow-control return.
  _ackedLength += len;
  if (_state == RESPONSE_WAIT_ACK) _state = RESPONSE_END;
  if (_state == RESPONSE_END) return 0;""", "response ACK before flow control")
    replacements = [
        ("  const size_t responseBatchLimit =", "  size_t responseBatchLimit ="),
        ("  size_t payloadlen{0};", """  // FastBee patch: account outstanding TCP bytes across polls.
  // A per-callback cap alone still fills lwIP's whole send window when polls
  // arrive before ACKs. Use actual sndbuf space, not response ACK counters:
  // a dropped/coalesced callback must not strand bytes after lwIP freed them.
  if (responseBatchLimit) {
    const size_t tcpSpace = request->client()->space();
    const size_t outstanding = TCP_SND_BUF - std::min<size_t>(TCP_SND_BUF, tcpSpace);
    if (outstanding >= responseBatchLimit) return 0;
    responseBatchLimit -= outstanding;
  }
  size_t payloadlen{0};"""),
        ("request->client()->add(_assembled_headers.c_str() + _writtenHeadersLength, _assembled_headers.length() - _writtenHeadersLength);",
         """request->client()->add(_assembled_headers.c_str() + _writtenHeadersLength,
      responseBatchLimit ? std::min(_assembled_headers.length() - _writtenHeadersLength, responseBatchLimit)
                         : _assembled_headers.length() - _writtenHeadersLength);"""),
    ]
    for old, new in replacements:
        if text.count(old) != 2:
            raise PatchError("WebResponses.cpp outstanding budget expected two response anchors")
        text = text.replace(old, new)
    return text, True


def _patch_basic_queued_accounting(text: str) -> Tuple[str, bool]:
    marker = "FastBee patch: count queued bytes even when tcp_output fails"
    if marker in text:
        return text, False
    old = "size_t const pcb_written = request->client()->write(_content.c_str() + _sentLength,"
    new = """// FastBee patch: count queued bytes even when tcp_output fails.
    // AsyncClient::write returns zero when output fails after add succeeded.
    // The data is still queued: resubmitting it would duplicate JSON bytes.
    size_t const pcb_written = request->client()->add(_content.c_str() + _sentLength,"""
    text, _ = _replace_once(text, old, new, "basic response queued byte accounting")
    old = """        : _content.length() - _sentLength);
    _writtenLength += pcb_written;"""
    new = """        : _content.length() - _sentLength);
    // Flush headers even for an empty body or a header-only budget slice.
    request->client()->send();
    _writtenLength += pcb_written;"""
    text, _ = _replace_once(text, old, new, "basic response explicit flush")
    return text, True


def _patch_chunked_terminal_drain(text: str) -> Tuple[str, bool]:
    """Keep a partially queued chunked terminator alive until its buffer drains."""
    marker = "FastBee patch: drain the terminal chunk before completion"
    if marker in text:
        return text, False
    signature = "size_t AsyncAbstractResponse::write_send_buffs("
    following = "size_t AsyncAbstractResponse::_readDataFromCacheOrContent("
    start, end = text.find(signature), text.find(following)
    if start < 0 or end <= start:
        raise PatchError("WebResponses.cpp missing abstract response boundaries")
    body = text[start:end]
    replacements = [
        ("if (_state == RESPONSE_WAIT_ACK) _state = RESPONSE_END;",
         "if (_state == RESPONSE_WAIT_ACK && !_send_buffer_len) _state = RESPONSE_END;"),
        ("if (_state == RESPONSE_CONTENT) {",
         "if (_state == RESPONSE_CONTENT || _state == RESPONSE_WAIT_ACK) {"),
        ("      // FastBee patch: _sentLength counts bytes read from the source, not",
         """      // FastBee patch: drain the terminal chunk before completion.
      // WAIT_ACK holds the final framing bytes across partial/zero add calls.
      // Once drained, stop without serializing a second terminal chunk.
      if (_state == RESPONSE_WAIT_ACK) {
        _state = RESPONSE_END;
        break;
      }

      // FastBee patch: _sentLength counts bytes read from the source, not"""),
        ("""          if (!readLen) {
            // last chunk?
            _state = RESPONSE_END;""",
         """          if (!readLen) {
            // Last chunk is staged; it may still be only partially accepted.
            _state = RESPONSE_WAIT_ACK;"""),
    ]
    for old, new in replacements:
        body, _ = _replace_once(body, old, new, "chunked terminal drain")
    return text[:start] + body + text[end:], True


def _patch_low_dram_staging_capacity(text: str) -> Tuple[str, bool]:
    """Reserve less local staging memory without changing the TCP allowance."""
    marker = "FastBee patch: smaller staging allocation under existing DRAM pressure"
    if marker in text:
        return text, False
    old = "          if (responseBatchLimit && cap > responseBatchLimit) continue;"
    new = """          // FastBee patch: smaller staging allocation under existing DRAM pressure.
          // TCP may still accept one MSS per callback. Fill that allowance in
          // smaller pieces so the local staging array does not duplicate it.
          if (responseBatchLimit && cap > std::min<size_t>(responseBatchLimit, 768U)) continue;"""
    return _replace_once(text, old, new, "low-DRAM response staging capacity")


def _patch_web_request_header_text(text: str) -> Tuple[str, bool]:
    """Add per-request state for one bounded ACK-timeout recovery."""
    if _WEB_REQUEST_HEADER_TIMEOUT_RECOVERY_PATCHED in text:
        return text, False
    return _replace_once(
        text,
        _WEB_REQUEST_HEADER_TIMEOUT_RECOVERY,
        _WEB_REQUEST_HEADER_TIMEOUT_RECOVERY_PATCHED,
        "WebRequest ACK-timeout recovery state",
    )


def _patch_web_request_source_text(text: str) -> Tuple[str, bool]:
    """Recover once only when an ACK-timeout retry queues real bytes."""
    changed = False
    for old, new, label in (
        (
            _WEB_REQUEST_ON_POLL,
            _WEB_REQUEST_ON_POLL_PATCHED,
            "WebRequest close after polled peer drain",
        ),
        (
            _WEB_REQUEST_ON_ACK,
            _WEB_REQUEST_ON_ACK_PATCHED,
            "WebRequest real-ACK recovery reset",
        ),
        (
            _WEB_REQUEST_ON_TIMEOUT,
            _WEB_REQUEST_ON_TIMEOUT_PATCHED,
            "WebRequest bounded ACK-timeout recovery",
        ),
    ):
        if new in text:
            continue
        text, did_change = _replace_once(text, old, new, label)
        changed = changed or did_change

    required = (
        "_ackTimeoutRecoveryAttempted = false;",
        "!_ackTimeoutRecoveryAttempted",
        "_response && !_response->_finished()",
        "_client && _client->canSend()",
        "_response->_ack(this, 0, 0) > 0",
    )
    missing = [marker for marker in required if marker not in text]
    if missing:
        raise PatchError(
            "WebRequest ACK-timeout recovery patch is incomplete: "
            + ", ".join(missing)
        )
    return text, changed


def _patch_async_tcp_header_text(text: str) -> Tuple[str, bool]:
    """Install the server-accepted connection bookkeeping fields."""
    changed = False
    if _ASYNCTCP_HEADER_CONNECT_PORT_PATCHED_LEGACY in text:
        text = text.replace(
            _ASYNCTCP_HEADER_CONNECT_PORT_PATCHED_LEGACY,
            _ASYNCTCP_HEADER_CONNECT_PORT_PATCHED,
            1,
        )
        changed = True
    replacements = (
        (
            _ASYNCTCP_HEADER_PROTECTED,
            _ASYNCTCP_HEADER_PROTECTED_PATCHED,
            "AsyncTCP.h accepted-client counter",
        ),
        (
            _ASYNCTCP_HEADER_CONNECT_PORT,
            _ASYNCTCP_HEADER_CONNECT_PORT_PATCHED,
            "AsyncTCP.h accepted-client instance flag",
        ),
    )
    for old, new, label in replacements:
        if new in text:
            continue
        text, did_change = _replace_once(text, old, new, label)
        changed = changed or did_change
    return text, changed


def _patch_async_tcp_source_text(text: str) -> Tuple[str, bool]:
    """Install the 8-bit DRAM and inbound-connection admission gate."""
    changed = False

    # Upgrade the first field-test revision in place. PSRAM-capable targets
    # must measure internal DMA-capable DRAM rather than aggregate 8-bit heap.
    if "FastBee patch: reject a connection before allocating AsyncClient" in text:
        for old, new in (
            (
                "heap_caps_get_free_size(MALLOC_CAP_8BIT)",
                "heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)",
            ),
            (
                "heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)",
                "heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)",
            ),
            (
                _ASYNCTCP_ACCEPT_EVENT_FAILURE_PATCHED_LEGACY,
                _ASYNCTCP_ACCEPT_EVENT_FAILURE_PATCHED,
            ),
            (
                _ASYNCTCP_ACCEPT_EVENT_FAILURE_PATCHED_COUNTER,
                _ASYNCTCP_ACCEPT_EVENT_FAILURE_PATCHED,
            ),
            (
                _ASYNCTCP_ACCEPT_EVENT_FAILURE_PATCHED_R11,
                _ASYNCTCP_ACCEPT_EVENT_FAILURE_PATCHED,
            ),
            (
                _ASYNCTCP_CLIENT_SECTION_PATCHED_LEGACY,
                _ASYNCTCP_CLIENT_SECTION_PATCHED,
            ),
            (
                _ASYNCTCP_DESTRUCTOR_PATCHED_LEGACY,
                _ASYNCTCP_DESTRUCTOR_PATCHED,
            ),
            (
                _ASYNCTCP_TCP_ERROR_PATCHED,
                _ASYNCTCP_TCP_ERROR_PATCHED_POOLED,
            ),
        ):
            if old in text and new not in text:
                text = text.replace(old, new, 1)
                changed = True
    replacements = (
        (
            _ASYNCTCP_EVENT_PACKET_CONSTRUCTOR,
            _ASYNCTCP_EVENT_PACKET_CONSTRUCTOR_PATCHED,
            "AsyncTCP.cpp error-event default constructor",
        ),
        (
            _ASYNCTCP_QUEUE_GLOBALS,
            _ASYNCTCP_QUEUE_GLOBALS_PATCHED,
            "AsyncTCP.cpp fixed error-event pool",
        ),
        (
            _ASYNCTCP_FREE_EVENT,
            _ASYNCTCP_FREE_EVENT_PATCHED,
            "AsyncTCP.cpp pooled error-event release",
        ),
        (
            _ASYNCTCP_CLIENT_SECTION,
            _ASYNCTCP_CLIENT_SECTION_PATCHED,
            "AsyncTCP.cpp accepted-client counter",
        ),
        (
            _ASYNCTCP_CONSTRUCTOR_TAIL,
            _ASYNCTCP_CONSTRUCTOR_TAIL_PATCHED,
            "AsyncTCP.cpp accepted-client initialization",
        ),
        (
            _ASYNCTCP_DESTRUCTOR,
            _ASYNCTCP_DESTRUCTOR_PATCHED,
            "AsyncTCP.cpp accepted-client release",
        ),
        (
            _ASYNCTCP_CLOSE,
            _ASYNCTCP_CLOSE_PATCHED,
            "AsyncTCP.cpp close-path accepted-client release",
        ),
        (
            _ASYNCTCP_ABORT,
            _ASYNCTCP_ABORT_PATCHED,
            "AsyncTCP.cpp abort-path accepted-client release",
        ),
        (
            _ASYNCTCP_TCP_ERROR,
            _ASYNCTCP_TCP_ERROR_PATCHED_POOLED,
            "AsyncTCP.cpp error-path accepted-client release",
        ),
        (
            _ASYNCTCP_ACCEPT_HEAD,
            _ASYNCTCP_ACCEPT_HEAD_PATCHED,
            "AsyncTCP.cpp low-memory admission gate",
        ),
        (
            _ASYNCTCP_ACCEPT_CLIENT,
            _ASYNCTCP_ACCEPT_CLIENT_PATCHED,
            "AsyncTCP.cpp accepted-client increment",
        ),
        (
            _ASYNCTCP_ACCEPT_EVENT_FAILURE,
            _ASYNCTCP_ACCEPT_EVENT_FAILURE_PATCHED,
            "AsyncTCP.cpp accept-event rollback",
        ),
    )
    for old, new, label in replacements:
        if new in text:
            continue
        text, did_change = _replace_once(text, old, new, label)
        changed = changed or did_change

    required = (
        "FastBee patch: reject a connection before allocating AsyncClient",
        "heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)",
        "CONFIG_ASYNC_TCP_MAX_CONNECTIONS",
        "__atomic_fetch_add(&AsyncClient::_fastbee_accepted_count",
        "__atomic_exchange_n(",
        "void AsyncClient::_fastbee_release_accepted()",
        "client->_fastbee_release_accepted();",
        "FASTBEE_ASYNC_TCP_ERROR_EVENT_POOL_SIZE",
        "_fastbee_alloc_error_event(client)",
        "_fastbee_release_error_event(evpkt)",
        "emergency pool exhausted",
        "c->_fastbee_release_accepted();",
        _ASYNCTCP_ABORT_PATCHED,
        "_reset_tcp_callbacks(pcb, c);",
        "delete c;",
    )
    missing = [marker for marker in required if marker not in text]
    if missing:
        raise PatchError(
            "AsyncTCP.cpp admission patch is incomplete: " + ", ".join(missing)
        )
    return text, changed


def apply_to_async_tcp_library(
    library_dir: Path, *, log: Callable[[str], None] = print
) -> bool:
    """Patch the pinned AsyncTCP dependency, refusing unknown versions."""
    metadata_path = library_dir / "library.json"
    if not metadata_path.is_file():
        log(f"[FastBee] skip AsyncTCP: library.json missing at {library_dir}")
        return False
    try:
        metadata = json.loads(_read(metadata_path))
    except (OSError, ValueError) as exc:
        raise PatchError(f"cannot read {metadata_path}: {exc}") from exc

    version = str(metadata.get("version", ""))
    if version != "3.5.0":
        raise PatchError(
            f"AsyncTCP v{version}: admission patch is verified only for v3.5.0"
        )

    header_path = library_dir / "src" / "AsyncTCP.h"
    source_path = library_dir / "src" / "AsyncTCP.cpp"
    if not header_path.is_file() or not source_path.is_file():
        raise PatchError(f"AsyncTCP v{version}: expected source files are missing")

    original_header = _read(header_path)
    original_source = _read(source_path)
    header, header_changed = _patch_async_tcp_header_text(original_header)
    source, source_changed = _patch_async_tcp_source_text(original_source)
    if header_changed or source_changed:
        # Both transformations are validated before the first write. If the
        # second write fails, restore both originals so a later incremental
        # build cannot compile a half-patched header/source pair.
        try:
            if header_changed:
                _write(header_path, header)
            if source_changed:
                _write(source_path, source)
        except (OSError, UnicodeError) as exc:
            rollback_errors = []
            for path, original in (
                (header_path, original_header),
                (source_path, original_source),
            ):
                try:
                    _write(path, original)
                except (OSError, UnicodeError) as rollback_exc:
                    rollback_errors.append(f"{path.name}: {rollback_exc}")
            detail = f"; rollback failed ({', '.join(rollback_errors)})" if rollback_errors else ""
            raise PatchError(f"AsyncTCP patch write failed: {exc}{detail}") from exc
    changed = header_changed or source_changed
    log(
        f"[FastBee] AsyncTCP v{version}: "
        + ("connection admission patch applied" if changed else "connection admission patch already present")
    )
    return changed


def apply_to_library(library_dir: Path, *, log: Callable[[str], None] = print) -> bool:
    metadata_path = library_dir / "library.json"
    if not metadata_path.is_file():
        log(f"[FastBee] skip ESPAsyncWebServer: library.json missing at {library_dir}")
        return False
    try:
        metadata = json.loads(_read(metadata_path))
    except (OSError, ValueError) as exc:
        raise PatchError(f"cannot read {metadata_path}: {exc}") from exc

    version = str(metadata.get("version", ""))
    if version != "3.12.0":
        raise PatchError(
            f"ESPAsyncWebServer v{version}: response patch is verified only for v3.12.0"
        )

    src = library_dir / "src"
    impl = src / "WebResponseImpl.h"
    responses = src / "WebResponses.cpp"
    request_header = src / "ESPAsyncWebServer.h"
    request_source = src / "WebRequest.cpp"
    if not all(path.is_file() for path in (impl, responses, request_header, request_source)):
        raise PatchError(f"ESPAsyncWebServer v{version}: expected source files are missing")

    original_request_header = _read(request_header)
    original_request_source = _read(request_source)
    patched_request_header, request_header_changed = _patch_web_request_header_text(
        original_request_header
    )
    patched_request_source, request_source_changed = _patch_web_request_source_text(
        original_request_source
    )
    if request_header_changed or request_source_changed:
        try:
            if request_header_changed:
                _write(request_header, patched_request_header)
            if request_source_changed:
                _write(request_source, patched_request_source)
        except (OSError, UnicodeError) as exc:
            rollback_errors = []
            for path, original in (
                (request_header, original_request_header),
                (request_source, original_request_source),
            ):
                try:
                    _write(path, original)
                except (OSError, UnicodeError) as rollback_exc:
                    rollback_errors.append(f"{path.name}: {rollback_exc}")
            detail = (
                f"; rollback failed ({', '.join(rollback_errors)})"
                if rollback_errors else ""
            )
            raise PatchError(f"WebRequest patch write failed: {exc}{detail}") from exc

    changed = request_header_changed or request_source_changed
    changed = _patch_web_response_impl(impl) or changed
    changed = _patch_web_responses(responses) or changed
    log(
        f"[FastBee] ESPAsyncWebServer v{version}: "
        + ("response recovery patch applied" if changed else "response recovery patch already present")
    )
    return changed


def _resolve_libdeps_dir(project_dir: Path, configured) -> Path:
    """Resolve PlatformIO's libdeps directory without assuming .pio layout."""
    raw = str(configured or "").strip()
    if not raw or raw == "$PROJECT_LIBDEPS_DIR":
        return (project_dir / ".pio" / "libdeps").resolve()
    candidate = Path(os.path.expandvars(os.path.expanduser(raw)))
    if not candidate.is_absolute():
        candidate = project_dir / candidate
    return candidate.resolve()


def _standalone_libdeps_dir(project_dir: Path) -> Path:
    """Read libdeps_dir through PlatformIO when run outside SCons."""
    try:
        from platformio.project.config import ProjectConfig
    except ImportError:
        return _resolve_libdeps_dir(project_dir, None)
    config = ProjectConfig.get_instance(str(project_dir / "platformio.ini"))
    return _resolve_libdeps_dir(
        project_dir, config.get("platformio", "libdeps_dir")
    )


def _platformio_context():
    try:
        Import("env")  # type: ignore[name-defined, F821]
        pio_env = env.subst("$PIOENV")  # type: ignore[name-defined, F821]
        project_dir = Path(  # type: ignore[name-defined, F821]
            env.subst("$PROJECT_DIR")
        ).resolve()
        project_config = env.GetProjectConfig()  # type: ignore[name-defined, F821]
        configured_libdeps = project_config.get("platformio", "libdeps_dir")
        expanded_libdeps = env.subst(  # type: ignore[name-defined, F821]
            "$PROJECT_LIBDEPS_DIR"
        )
        if not expanded_libdeps or "$PROJECT_LIBDEPS_DIR" in expanded_libdeps:
            expanded_libdeps = configured_libdeps
        libdeps_dir = _resolve_libdeps_dir(project_dir, expanded_libdeps)
        return project_dir, pio_env, libdeps_dir
    except (NameError, RuntimeError, OSError):
        return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env", help="PlatformIO environment name")
    parser.add_argument("--project-dir", default=os.getcwd(), help="Project directory")
    args = parser.parse_args()

    context = _platformio_context()
    if context:
        project_dir, pio_env, libdeps_dir = context
    else:
        project_dir = Path(args.project_dir).resolve()
        if not args.env:
            parser.error("--env is required outside PlatformIO")
        pio_env = args.env
        libdeps_dir = _standalone_libdeps_dir(project_dir)

    try:
        dependency_root = libdeps_dir / pio_env
        apply_to_library(dependency_root / "ESPAsyncWebServer")
        apply_to_async_tcp_library(dependency_root / "AsyncTCP")
    except PatchError as exc:
        print(f"[FastBee] ERROR: {exc}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
else:
    _ctx = _platformio_context()
    if _ctx:
        _project_dir, _pio_env, _libdeps_dir = _ctx
        try:
            _dependency_root = _libdeps_dir / _pio_env
            apply_to_library(_dependency_root / "ESPAsyncWebServer")
            apply_to_async_tcp_library(_dependency_root / "AsyncTCP")
        except PatchError as exc:
            print(f"[FastBee] ERROR: {exc}")
            raise
