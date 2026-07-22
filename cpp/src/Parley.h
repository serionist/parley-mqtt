#ifndef PARLEY_H
#define PARLEY_H

// Parley — request/response RPC over MQTT (C++ / ESP32-Arduino-first). Umbrella header: include this
// to get the full public API. The wire contract is docs/protocol.md; the cross-language design is
// docs/design.md. Statically allocated, no exceptions, no RTTI dependence, tick-driven.

#include "ParleyCallOptions.h"
#include "ParleyCodec.h"
#include "ParleyCommandHandler.h"
#include "ParleyCommander.h"
#include "ParleyConfig.h"
#include "ParleyConnectionAdapter.h"
#include "ParleyError.h"
#include "ParleyExecution.h"
#include "ParleyIdGenerator.h"
#include "ParleyTopics.h"

#endif  // PARLEY_H
