#ifndef ENV_SCHEDULER_RPC_RPC_HH
#define ENV_SCHEDULER_RPC_RPC_HH

// RPC layer unified header
//
// Provides:
//   pump::rpc::call()    — request-response sender
//   pump::rpc::notify()  — fire-and-forget notification sender
//   pump::rpc::serve()   — recv loop + dispatch pipeline
//   pump::rpc::service<> — static service registration (template specialization)
//   pump::rpc::make_channel() — channel factory
//   pump::rpc::raw_codec — default codec for trivially-copyable types

#include "common/message_type.hh"
#include "common/header.hh"
#include "common/error.hh"
#include "common/codec_concept.hh"
#include "common/config.hh"

#include "channel/pending_map.hh"
#include "channel/dispatcher.hh"
#include "channel/channel.hh"

#include "senders/call.hh"
#include "senders/notify.hh"
#include "senders/serve.hh"

#include "codec/raw_codec.hh"

#endif //ENV_SCHEDULER_RPC_RPC_HH
