#include "server/drain_manager_impl.h"

#include <chrono>
#include <cstdint>
#include <functional>

#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/runtime/runtime.h"
#include "envoy/server/instance.h"

#include "common/common/assert.h"

namespace Envoy {
namespace Server {

DrainManagerImpl::DrainManagerImpl(Instance& server) : server_(server) {}

bool DrainManagerImpl::drainClose() const {
  // If we are actively HC failed, always drain close.
  //
  // TODO(mattklein123): In relation to x-envoy-immediate-health-check-fail, it would be better
  // if even in the case of server health check failure we had some period of drain ramp up. This
  // would allow the other side to fail health check for the host which will require some thread
  // jumps versus immediately start GOAWAY/connection thrashing.
  if (server_.healthCheckFailed()) {
    return true;
  }

  if (!draining()) {
    return false;
  }

  // We use the tick time as in increasing chance that we shutdown connections.
  return static_cast<uint64_t>(drain_time_completed_.count()) >
         (server_.random().random() % server_.options().drainTime().count());
}

void DrainManagerImpl::drainSequenceTick() {
  ENVOY_LOG(trace, "drain tick #{}", drain_time_completed_.count());
  ASSERT(drain_time_completed_ < server_.options().drainTime());
  drain_time_completed_ += std::chrono::seconds(1);

  if (drain_time_completed_ < server_.options().drainTime()) {
    drain_tick_timer_->enableTimer(std::chrono::milliseconds(1000));
  } else if (drain_sequence_completion_) {
    drain_sequence_completion_();
  }
}

void DrainManagerImpl::startDrainSequence(std::function<void()> completion) {
  drain_sequence_completion_ = completion;
  ASSERT(!drain_tick_timer_);
  drain_tick_timer_ = server_.dispatcher().createTimer([this]() -> void { drainSequenceTick(); });
  drainSequenceTick();
}

void DrainManagerImpl::startParentShutdownSequence() {
  ASSERT(!parent_shutdown_timer_);
  parent_shutdown_timer_ = server_.dispatcher().createTimer([this]() -> void {
    // Shut down the parent now. It should have already been draining.
    ENVOY_LOG(warn, "shutting down parent after drain");
    server_.hotRestart().terminateParent();
  });

  parent_shutdown_timer_->enableTimer(std::chrono::duration_cast<std::chrono::milliseconds>(
      server_.options().parentShutdownTime()));
}

} // namespace Server
} // namespace Envoy
