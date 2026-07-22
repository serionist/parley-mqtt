#ifndef PARLEY_TOPICS_H
#define PARLEY_TOPICS_H

namespace parley {

// The four consumer-supplied topic strings for one command type (protocol.md §3). Opaque; Parley
// imposes no naming scheme. The strings are borrowed — the consumer owns their lifetime (they must
// outlive the commander/handler). On a device these are typically string literals in flash.
struct ParleyTopics {
  const char* request;    // commander → handler: req, rak, cnl
  const char* heartbeat;  // handler → commander: hb
  const char* progress;   // handler → commander: prg
  const char* result;     // handler → commander: res

  ParleyTopics() : request(nullptr), heartbeat(nullptr), progress(nullptr), result(nullptr) {}

  ParleyTopics(const char* req, const char* hb, const char* prg, const char* res)
      : request(req), heartbeat(hb), progress(prg), result(res) {}
};

}  // namespace parley

#endif  // PARLEY_TOPICS_H
