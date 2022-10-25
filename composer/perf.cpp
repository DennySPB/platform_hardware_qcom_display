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

#include <android/binder_manager.h>
#include <cutils/properties.h>
#include <errno.h>
#include <dlfcn.h>
#include <utils/debug.h>
#include "perf.h"
#include "hwc_debugger.h"
#include <system/graphics.h>

#include <math.h>
#include <utils/constants.h>
#include <utils/utils.h>
#include <utils/debug.h>
#include <utils/Timers.h>

#include <aidl/android/hardware/power/IPower.h>
#include <aidl/google/hardware/power/extension/pixel/IPowerExt.h>

using ::aidl::android::hardware::power::IPower;
using ::aidl::google::hardware::power::extension::pixel::IPowerExt;
using namespace std::chrono_literals;

#define __CLASS__ "Perf"

namespace sdm {

constexpr float nsecsPerSec = std::chrono::nanoseconds(1s).count();
constexpr int64_t nsecsIdleHintTimeout = std::chrono::nanoseconds(100ms).count();

DisplayError Perf::Init() {
  mPowerHalHint.Init();
  return kErrorNone;
}

Perf::PowerHalHintWorker::PowerHalHintWorker()
      : Worker("DisplayHints", HAL_PRIORITY_URGENT_DISPLAY),
        mNeedUpdateRefreshRateHint(false),
        mPrevRefreshRate(0),
        mPendingPrevRefreshRate(0),
        mIdleHintIsEnabled(false),
        mForceUpdateIdleHint(false),
        mIdleHintDeadlineTime(0),
        mIdleHintSupportIsChecked(false),
        mIdleHintIsSupported(false),
        mPowerModeState(HWC2::PowerMode::Off),
        mVsyncPeriod(16666666),
        mPowerHalExtAidl(nullptr),
        mDeathRecipient(AIBinder_DeathRecipient_new(BinderDiedCallback)) {}

Perf::PowerHalHintWorker::~PowerHalHintWorker() {
    Exit();
}

int Perf::PowerHalHintWorker::Init() {
    return InitWorker();
}

void Perf::PowerHalHintWorker::BinderDiedCallback(void *cookie) {
    ALOGE("PowerHal is died");
    auto powerHint = reinterpret_cast<PowerHalHintWorker *>(cookie);
    powerHint->forceUpdateHints();
}

int32_t Perf::PowerHalHintWorker::connectPowerHalExt() {
    if (mPowerHalExtAidl) {
        return android::NO_ERROR;
    }
    const std::string kInstance = std::string(IPower::descriptor) + "/default";
    ndk::SpAIBinder pwBinder = ndk::SpAIBinder(AServiceManager_getService(kInstance.c_str()));
    ndk::SpAIBinder pwExtBinder;
    AIBinder_getExtension(pwBinder.get(), pwExtBinder.getR());

    mPowerHalExtAidl = IPowerExt::fromBinder(pwExtBinder);
    if (!mPowerHalExtAidl) {
        DLOGE("failed to connect power HAL extension");
        return -EINVAL;
    }

    AIBinder_linkToDeath(pwExtBinder.get(), mDeathRecipient.get(), reinterpret_cast<void *>(this));

    forceUpdateHints();

    ALOGI("connect power HAL extension successfully");
    return android::NO_ERROR;
}

int32_t Perf::PowerHalHintWorker::checkPowerHalExtHintSupport(const std::string &mode) {
    if (mode.empty() || connectPowerHalExt() != android::NO_ERROR) {
        return -EINVAL;
    }
    bool isSupported = false;
    auto ret = mPowerHalExtAidl->isModeSupported(mode.c_str(), &isSupported);
    if (!ret.isOk()) {
        DLOGE("failed to check power HAL extension hint: mode=%s", mode.c_str());
        if (ret.getExceptionCode() == EX_TRANSACTION_FAILED) {
            /*
             * PowerHAL service may crash due to some reasons, this could end up
             * binder transaction failure. Set nullptr here to trigger re-connection.
             */
            DLOGE("binder transaction failed for power HAL extension hint");
            mPowerHalExtAidl = nullptr;
            return -ENOTCONN;
        }
        return -EINVAL;
    }
    if (!isSupported) {
        DLOGW("power HAL extension hint is not supported: mode=%s", mode.c_str());
        return -EOPNOTSUPP;
    }
    DLOGI("power HAL extension hint is supported: mode=%s", mode.c_str());
    return android::NO_ERROR;
}

int32_t Perf::PowerHalHintWorker::sendPowerHalExtHint(const std::string &mode,
                                                               bool enabled) {
    if (mode.empty() || connectPowerHalExt() != android::NO_ERROR) {
        return -EINVAL;
    }
    auto ret = mPowerHalExtAidl->setMode(mode.c_str(), enabled);
    if (!ret.isOk()) {
        DLOGE("failed to send power HAL extension hint: mode=%s, enabled=%d", mode.c_str(),
              enabled);
        if (ret.getExceptionCode() == EX_TRANSACTION_FAILED) {
            /*
             * PowerHAL service may crash due to some reasons, this could end up
             * binder transaction failure. Set nullptr here to trigger re-connection.
             */
            DLOGE("binder transaction failed for power HAL extension hint");
            mPowerHalExtAidl = nullptr;
            return -ENOTCONN;
        }
        return -EINVAL;
    }
    return android::NO_ERROR;
}

int32_t Perf::PowerHalHintWorker::checkRefreshRateHintSupport(int refreshRate) {
    int32_t ret = android::NO_ERROR;
    const auto its = mRefreshRateHintSupportMap.find(refreshRate);
    if (its == mRefreshRateHintSupportMap.end()) {
        /* check new hint */
        std::string refreshRateHintStr = "REFRESH_" + std::to_string(refreshRate) + "FPS";
        ret = checkPowerHalExtHintSupport(refreshRateHintStr);
        if (ret == android::NO_ERROR || ret == -EOPNOTSUPP) {
            mRefreshRateHintSupportMap[refreshRate] = (ret == android::NO_ERROR);
            DLOGI("cache refresh rate hint %s: %d", refreshRateHintStr.c_str(), !ret);
        } else {
            DLOGE("failed to check the support of refresh rate hint, ret %d", ret);
        }
    } else {
        /* check existing hint */
        if (!its->second) {
            ret = -EOPNOTSUPP;
        }
    }
    return ret;
}

int32_t Perf::PowerHalHintWorker::sendRefreshRateHint(int refreshRate, bool enabled) {
    std::string hintStr = "REFRESH_" + std::to_string(refreshRate) + "FPS";
    int32_t ret = sendPowerHalExtHint(hintStr, enabled);
    if (ret == -ENOTCONN) {
        /* Reset the hints when binder failure occurs */
        mPrevRefreshRate = 0;
        mPendingPrevRefreshRate = 0;
    }
    return ret;
}

int32_t Perf::PowerHalHintWorker::updateRefreshRateHintInternal(
        HWC2::PowerMode powerMode, VsyncPeriodNanos vsyncPeriod) {
    int32_t ret = android::NO_ERROR;
    /* We should disable pending hint before other operations */
    if (mPendingPrevRefreshRate) {
        ret = sendRefreshRateHint(mPendingPrevRefreshRate, false);
        if (ret == android::NO_ERROR) {
            mPendingPrevRefreshRate = 0;
        } else {
            return ret;
        }
    }
    if (powerMode != HWC2::PowerMode::On) {
        if (mPrevRefreshRate) {
            ret = sendRefreshRateHint(mPrevRefreshRate, false);
            // DLOGI("RefreshRate hint = %d disabled", mPrevRefreshRate);
            if (ret == android::NO_ERROR) {
                mPrevRefreshRate = 0;
            }
        }
        return ret;
    }
    /* TODO: add refresh rate buckets, tracked in b/181100731 */
    int refreshRate = static_cast<int>(round(nsecsPerSec / vsyncPeriod * 0.1f) * 10);
    if (mPrevRefreshRate == refreshRate) {
        return android::NO_ERROR;
    }
    ret = checkRefreshRateHintSupport(refreshRate);
    if (ret != android::NO_ERROR) {
        return ret;
    }
    /*
     * According to PowerHAL design, while switching to next refresh rate, we
     * have to enable the next hint first, then disable the previous one so
     * that the next hint can take effect.
     */
    ret = sendRefreshRateHint(refreshRate, true);
    // DLOGI("RefreshRate hint = %d enabled", refreshRate);
    if (ret != android::NO_ERROR) {
        return ret;
    }
    if (mPrevRefreshRate) {
        ret = sendRefreshRateHint(mPrevRefreshRate, false);
        if (ret != android::NO_ERROR) {
            if (ret != -ENOTCONN) {
                /*
                 * We may fail to disable the previous hint and end up multiple
                 * hints enabled. Save the failed hint as pending hint here, we
                 * will try to disable it first while entering this function.
                 */
                mPendingPrevRefreshRate = mPrevRefreshRate;
                mPrevRefreshRate = refreshRate;
            }
            return ret;
        }
    }
    mPrevRefreshRate = refreshRate;
    return ret;
}

int32_t Perf::PowerHalHintWorker::checkIdleHintSupport(void) {
    int32_t ret = android::NO_ERROR;
    Lock();
    if (mIdleHintSupportIsChecked) {
        ret = mIdleHintIsSupported ? android::NO_ERROR : -EOPNOTSUPP;
        Unlock();
        return ret;
    }
    Unlock();
    ret = checkPowerHalExtHintSupport("DISPLAY_IDLE");
    Lock();
    if (ret == android::NO_ERROR) {
        mIdleHintIsSupported = true;
        mIdleHintSupportIsChecked = true;
        DLOGI("display idle hint is supported");
    } else if (ret == -EOPNOTSUPP) {
        mIdleHintSupportIsChecked = true;
        DLOGI("display idle hint is unsupported");
    } else {
        DLOGW("failed to check the support of display idle hint, ret %d", ret);
    }
    Unlock();
    return ret;
}

int32_t Perf::PowerHalHintWorker::updateIdleHint(int64_t deadlineTime, bool forceUpdate) {
    int32_t ret = checkIdleHintSupport();
    if (ret != android::NO_ERROR) {
        return ret;
    }
    bool enableIdleHint =
            (deadlineTime < systemTime(SYSTEM_TIME_MONOTONIC) && CC_LIKELY(deadlineTime > 0));

    if (mIdleHintIsEnabled != enableIdleHint) {
        // DLOGI("idle hint = %d", enableIdleHint);
        ret = sendPowerHalExtHint("DISPLAY_IDLE", enableIdleHint);
        if (ret == android::NO_ERROR) {
            mIdleHintIsEnabled = enableIdleHint;
        }
    }
    return ret;
}

void Perf::PowerHalHintWorker::forceUpdateHints(void) {
    Lock();
    mPrevRefreshRate = 0;
    mNeedUpdateRefreshRateHint = true;
    if (mIdleHintSupportIsChecked && mIdleHintIsSupported) {
        mForceUpdateIdleHint = true;
    }

    Unlock();

    Signal();
}

void Perf::PowerHalHintWorker::signalRefreshRate(HWC2::PowerMode powerMode,
                                                          VsyncPeriodNanos vsyncPeriod) {
    Lock();
    mPowerModeState = powerMode;
    mVsyncPeriod = vsyncPeriod;
    mNeedUpdateRefreshRateHint = true;
    Unlock();
    Signal();
}

void Perf::PowerHalHintWorker::signalIdle() {
    Lock();
    if (mIdleHintSupportIsChecked && !mIdleHintIsSupported) {
        Unlock();
        return;
    }
    mIdleHintDeadlineTime = static_cast<uint64_t>(systemTime(SYSTEM_TIME_MONOTONIC) + nsecsIdleHintTimeout);
    Unlock();
    Signal();
}

bool Perf::PowerHalHintWorker::needUpdateIdleHintLocked(int64_t &timeout) {
    if (!mIdleHintIsSupported) {
        return false;
    }

    int64_t currentTime = systemTime(SYSTEM_TIME_MONOTONIC);
    bool shouldEnableIdleHint =
            (mIdleHintDeadlineTime < currentTime) && CC_LIKELY(mIdleHintDeadlineTime > 0);
    if (mIdleHintIsEnabled != shouldEnableIdleHint || mForceUpdateIdleHint) {
        return true;
    }

    timeout = mIdleHintDeadlineTime - currentTime;
    return false;
}

void Perf::PowerHalHintWorker::Routine() {
    Lock();
    int ret = android::NO_ERROR;
    int64_t timeout = -1;
    if (!mNeedUpdateRefreshRateHint && !needUpdateIdleHintLocked(timeout)) {
        ret = WaitForSignalOrExitLocked(timeout);
    }
    if (ret == -EINTR) {
        Unlock();
        return;
    }
    bool needUpdateRefreshRateHint = mNeedUpdateRefreshRateHint;
    int64_t deadlineTime = mIdleHintDeadlineTime;
    HWC2::PowerMode powerMode = mPowerModeState;
    VsyncPeriodNanos vsyncPeriod = mVsyncPeriod;
    /*
     * Clear the flags here instead of clearing them after calling the hint
     * update functions. The flags may be set by signals after Unlock() and
     * before the hint update functions are done. Thus we may miss the newest
     * hints if we clear the flags after the hint update functions work without
     * errors.
     */
    mNeedUpdateRefreshRateHint = false;

    bool forceUpdateIdleHint = mForceUpdateIdleHint;
    mForceUpdateIdleHint = false;

    Unlock();
    updateIdleHint(deadlineTime, forceUpdateIdleHint);
    if (needUpdateRefreshRateHint) {
        int32_t rc = updateRefreshRateHintInternal(powerMode, vsyncPeriod);
        if (rc != android::NO_ERROR && rc != -EOPNOTSUPP) {
            Lock();
            if (mPowerModeState == HWC2::PowerMode::On) {
                /* Set the flag to trigger update again for next loop */
                mNeedUpdateRefreshRateHint = true;
            }
            Unlock();
        }
    }
}

void Perf::updateRefreshRateHint(HWC2::PowerMode powerMode,
                                            uint32_t vsyncPeriod) {
    mPowerHalHint.signalRefreshRate(powerMode, vsyncPeriod);
}

void Perf::signalIdle() {
    mPowerHalHint.signalIdle();
}

}  // namespace sdm
