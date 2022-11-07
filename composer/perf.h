/* Copyright (c) 2015, 2020, The Linux Foundataion. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above
*       copyright notice, this list of conditions and the following
*       disclaimer in the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of The Linux Foundation nor the names of its
*       contributors may be used to endorse or promote products derived
*       from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/

#ifndef __PERF_H__
#define __PERF_H__

#include <android/binder_auto_utils.h>
#include <core/sdm_types.h>
#include <utils/sys.h>

#include "hwc_display.h"
#include "worker.h"

#include <chrono>
#include <set>
#include <utils/Timers.h>

namespace composer_V2_4 = ::android::hardware::graphics::composer::V2_4;
using VsyncPeriodNanos = composer_V2_4::VsyncPeriodNanos;
using namespace std::chrono_literals;

template <size_t bufferSize>
struct RollingAverage {
    std::array<int64_t, bufferSize> buffer{0};
    int64_t total = 0;
    int64_t average;
    size_t elems = 0;
    size_t buffer_index = 0;
    void insert(int64_t newTime) {
        total += newTime - buffer[buffer_index];
        buffer[buffer_index] = newTime;
        buffer_index = (buffer_index + 1) % bufferSize;
        elems = std::min(elems + 1, bufferSize);
        average = total / elems;
    }
};

// Waits for a given property value, or returns std::nullopt if unavailable
std::optional<std::string> waitForPropertyValue(const std::string &property, int64_t timeoutMs);

namespace aidl {
namespace google {
namespace hardware {
namespace power {
namespace extension {
namespace pixel {

class IPowerExt;

} // namespace pixel
} // namespace extension
} // namespace power
} // namespace hardware
} // namespace google
} // namespace aidl

namespace aidl {
namespace android {
namespace hardware {
namespace power {

class IPower;
class IPowerHintSession;
class WorkDuration;

} // namespace power
} // namespace hardware
} // namespace android
} // namespace aidl

using WorkDuration = aidl::android::hardware::power::WorkDuration;

namespace sdm {

class Perf {

 public:
  // is the hint session both enabled and supported
  bool usePowerHintSession();
  int32_t checkPowerHalExtHintSupport(const std::string& mode);

  /* Display hint to notify power hal */
  class PowerHalHintWorker : public Worker {
  public:
      virtual ~PowerHalHintWorker();
      int Init();

      PowerHalHintWorker();
      void signalRefreshRate(HWC2::PowerMode powerMode, uint32_t vsyncPeriod);
      void signalIdle();
      void signalActualWorkDuration(nsecs_t actualDurationNanos);
      void signalTargetWorkDuration(nsecs_t targetDurationNanos);

      void addBinderTid(pid_t tid);
      void removeBinderTid(pid_t tid);

      bool signalStartHintSession();
      void trackThisThread();

      // is the hint session both enabled and supported
      bool usePowerHintSession();
      // is it known if the hint session is enabled + supported yet
      bool checkPowerHintSessionReady();

  protected:
      void Routine() override;

  private:
      static void BinderDiedCallback(void*);
      int32_t connectPowerHal();
      int32_t connectPowerHalExt();
      int32_t checkPowerHalExtHintSupport(const std::string& mode);
      int32_t sendPowerHalExtHint(const std::string& mode, bool enabled);
      int32_t checkRefreshRateHintSupport(int refreshRate);
      int32_t updateRefreshRateHintInternal(HWC2::PowerMode powerMode,
                                            uint32_t vsyncPeriod);
      int32_t sendRefreshRateHint(int refreshRate, bool enabled);
      void forceUpdateHints();
      int32_t checkIdleHintSupport();
      int32_t updateIdleHint(int64_t deadlineTime, bool forceUpdate);
      bool needUpdateIdleHintLocked(int64_t& timeout) REQUIRES(mutex_);

      // for adpf cpu hints
      int32_t sendActualWorkDuration();
      int32_t updateTargetWorkDuration();

      // Update checking methods
      bool needUpdateTargetWorkDurationLocked() REQUIRES(mutex_);
      bool needSendActualWorkDurationLocked() REQUIRES(mutex_);

      // is it known if the hint session is enabled + supported yet
      bool checkPowerHintSessionReadyLocked();
      // Hint session lifecycle management
      int32_t startHintSession();

      int32_t checkPowerHintSessionSupport();
      bool mNeedUpdateRefreshRateHint;
      // previous refresh rate
      int mPrevRefreshRate;
      // the refresh rate whose hint failed to be disabled
      int mPendingPrevRefreshRate;
      // support list of refresh rate hints
      std::map<int, bool> mRefreshRateHintSupportMap;
      bool mIdleHintIsEnabled;
      bool mForceUpdateIdleHint;
      int64_t mIdleHintDeadlineTime;
      // whether idle hint support is checked
      bool mIdleHintSupportIsChecked;
      // whether idle hint is supported
      bool mIdleHintIsSupported;
      HWC2::PowerMode mPowerModeState;
      VsyncPeriodNanos mVsyncPeriod;

      ndk::ScopedAIBinder_DeathRecipient mDeathRecipient;

      // for power HAL extension hints
      std::shared_ptr<aidl::google::hardware::power::extension::pixel::IPowerExt>
               mPowerHalExtAidl;

      // for normal power HAL hints
      std::shared_ptr<aidl::android::hardware::power::IPower> mPowerHalAidl;
      // Max amount the error term can vary without causing an actual value report,
      // as well as the target durations if not normalized
      static constexpr const std::chrono::nanoseconds kAllowedDeviation = 300us;
      // Target value used for initialization and normalization,
      // the actual value does not really matter
      static constexpr const std::chrono::nanoseconds kDefaultTarget = 50ms;
      // Whether to normalize all the actual values as error terms relative to a constant
      // target. This saves a binder call by not setting the target
      static const bool sNormalizeTarget;
      // Whether we should emit ATRACE_INT data for hint sessions
      static const bool sTraceHintSessionData;
      // Whether we use or disable the rate limiter for target and actual values
      static const bool sUseRateLimiter;
      std::shared_ptr<aidl::android::hardware::power::IPowerHintSession> mPowerHintSession;
      // queue of actual durations waiting to be reported
      std::vector<WorkDuration> mPowerHintQueue;
      // display-specific binder thread tids
      std::set<pid_t> mBinderTids;
      // indicates that the tid list has changed, so the session must be rebuilt
      bool mTidsUpdated = false;

      static std::mutex sSharedDisplayMutex;
      struct SharedDisplayData {
              std::optional<bool> hintSessionEnabled;
              std::optional<int32_t> hintSessionSupported;
      };
      // caches the output of usePowerHintSession to avoid sSharedDisplayMutex
      std::atomic<std::optional<bool>> mUsePowerHintSession{std::nullopt};
      // this lets us know if we can skip calling checkPowerHintSessionSupport
      bool mHintSessionSupportChecked = false;
      // used to indicate to all displays whether hint sessions are enabled/supported
      static SharedDisplayData sSharedDisplayData GUARDED_BY(sSharedDisplayMutex);
      // latest target that was signalled
      nsecs_t mTargetWorkDuration = kDefaultTarget.count();
      // last target duration reported to PowerHAL
      nsecs_t mLastTargetDurationReported = kDefaultTarget.count();
      // latest actual duration signalled
      std::optional<nsecs_t> mActualWorkDuration;
      // last error term reported to PowerHAL, used for rate limiting
      std::optional<nsecs_t> mLastErrorSent;
      // timestamp of the last report we sent, used to avoid stale sessions
      nsecs_t mLastActualReportTimestamp = 0;
      // amount of time after the last message was sent before the session goes stale
      // actually 100ms but we use 80 here to ideally avoid going stale
      static constexpr const std::chrono::nanoseconds kStaleTimeout = 80ms;
      // An adjustable safety margin which moves the "target" earlier to allow flinger to
      // go a bit over without dropping a frame, especially since we can't measure
      // the exact time HWC finishes composition so "actual" durations are measured
      // from the end of present() instead, which is a bit later.
      static constexpr const std::chrono::nanoseconds kTargetSafetyMargin = 2ms;
  };
      PowerHalHintWorker mPowerHalHint;

      std::optional<nsecs_t> mValidateStartTime;
      nsecs_t mPresentStartTime;
      std::optional<nsecs_t> mValidationDuration;
      // cached value used to skip evaluation once set
      std::optional<bool> mUsePowerHintSession;
      // tracks the time right before we start to wait for the fence
      std::optional<nsecs_t> mRetireFenceWaitTime;
      // tracks the time right after we finish waiting for the fence
      std::optional<nsecs_t> mRetireFenceAcquireTime;
      // tracks the expected present time of the last frame
      std::optional<nsecs_t> mLastTarget;
      // tracks the expected present time of the current frame
      nsecs_t mCurrentTarget;
      // set once at the start of composition to ensure consistency
      bool mUsePowerHints = false;
      nsecs_t getTarget();
      void updateAverages(nsecs_t endTime);
      std::optional<nsecs_t> getPredictedDuration(bool duringValidation);

};

}  // namespace sdm

#endif  // __PERF_H__
