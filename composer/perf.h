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

namespace composer_V2_4 = ::android::hardware::graphics::composer::V2_4;
using VsyncPeriodNanos = composer_V2_4::VsyncPeriodNanos;

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


namespace sdm {

class Perf {

 public:
  DisplayError Init();
  void updateRefreshRateHint(HWC2::PowerMode powerMode, uint32_t vsyncPeriod);
  void signalIdle();

 private:
  int32_t checkPowerHalExtHintSupport(const std::string& mode);

  /* Display hint to notify power hal */
  class PowerHalHintWorker : public Worker {
  public:
      virtual ~PowerHalHintWorker();
      int Init();

      PowerHalHintWorker();
      void signalRefreshRate(HWC2::PowerMode powerMode, uint32_t vsyncPeriod);
      void signalIdle();
  protected:
      void Routine() override;

  private:
      static void BinderDiedCallback(void*);
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
      // for power HAL extension hints
      std::shared_ptr<aidl::google::hardware::power::extension::pixel::IPowerExt>
               mPowerHalExtAidl;
      ndk::ScopedAIBinder_DeathRecipient mDeathRecipient;
  };
      PowerHalHintWorker mPowerHalHint;
};

}  // namespace sdm

#endif  // __PERF_H__
