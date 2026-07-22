#ifndef PARLEY_CONNECTION_ADAPTER_H
#define PARLEY_CONNECTION_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

namespace parley {

// The transport port the consumer implements over their own MQTT client (design.md). The core never
// references a concrete client. All publishes use an EXPLICIT length (never a null-terminated overload
// — payloads are UTF-8 JSON, not C strings, and may not be safely null-terminated; CONVENTIONS.md).
//
// Delivery MAY be synchronous (in-process loopback) — the core completes every state transition and
// schedules its timers before it calls publish() (publish-comes-last), so a re-entrant inbound
// delivery from inside publish() always finds consistent state.
class ParleyConnectionAdapter {
 public:
  virtual ~ParleyConnectionAdapter() {}

  // Fire-and-forget publish with retain = false. `length` bytes at `payload`.
  virtual void publish(const char* topic, const uint8_t* payload, size_t length) = 0;

  // Subscribe with replace-semantics: one handler per topic (design.md). The core re-subscribes on
  // every connected() signal WITHOUT unsubscribing first, so an appending adapter would multiply-deliver.
  virtual void subscribe(const char* topic) = 0;

  virtual void unsubscribe(const char* topic) = 0;
};

}  // namespace parley

#endif  // PARLEY_CONNECTION_ADAPTER_H
