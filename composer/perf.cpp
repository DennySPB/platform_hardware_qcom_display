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
#include "android-base/properties.h"
#include <android-base/parsebool.h>
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

std::optional<std::string> waitForPropertyValue(const std::string& property, int64_t timeoutMs) {
    if (!android::base::WaitForPropertyCreation(property, std::chrono::milliseconds(timeoutMs))) {
        return std::nullopt;
    }
    std::string out = android::base::GetProperty(property, "unknown");
    if (out == "unknown") {
        return std::nullopt;
    }
    return std::make_optional(out);
}

namespace sdm {

constexpr float nsecsPerSec = std::chrono::nanoseconds(1s).count();
constexpr int64_t nsecsIdleHintTimeout = std::chrono::nanoseconds(100ms).count();

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
        mDeathRecipient(AIBinder_DeathRecipient_new(BinderDiedCallback)),
        mPowerHalExtAidl(nullptr),
        mPowerHalAidl(nullptr),
        mPowerHintSession(nullptr) {}

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

int32_t Perf::PowerHalHintWorker::connectPowerHal() {
    if (mPowerHalAidl && mPowerHalExtAidl) {
        return android::NO_ERROR;
    }
    const std::string kInstance = std::string(IPower::descriptor) + "/default";
    ndk::SpAIBinder pwBinder = ndk::SpAIBinder(AServiceManager_getService(kInstance.c_str()));
    mPowerHalAidl = IPower::fromBinder(pwBinder);

    if (!mPowerHalAidl) {
        ALOGE("failed to connect power HAL");
        return -EINVAL;
    }

    ndk::SpAIBinder pwExtBinder;
    AIBinder_getExtension(pwBinder.get(), pwExtBinder.getR());

    mPowerHalExtAidl = IPowerExt::fromBinder(pwExtBinder);
    if (!mPowerHalExtAidl) {
        mPowerHalAidl = nullptr;
        DLOGE("failed to connect power HAL extension");
        return -EINVAL;
    }

    AIBinder_linkToDeath(pwExtBinder.get(), mDeathRecipient.get(), reinterpret_cast<void *>(this));

    // ensure the hint session is recreated every time powerhal is recreated
    mPowerHintSession = nullptr;
    forceUpdateHints();

    ALOGI("connect power HAL extension successfully");
    return android::NO_ERROR;
}

int32_t Perf::PowerHalHintWorker::checkPowerHalExtHintSupport(const std::string &mode) {
    if (mode.empty() || connectPowerHal() != android::NO_ERROR) {
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
    if (mode.empty() || connectPowerHal() != android::NO_ERROR) {
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

int32_t Perf::PowerHalHintWorker::checkPowerHintSessionSupport() {
    std::scoped_lock lock(sSharedDisplayMutex);
    if (sSharedDisplayData.hintSessionSupported.has_value()) {
        mHintSessionSupportChecked = true;
        return *(sSharedDisplayData.hintSessionSupported);
    }

    if (connectPowerHal() != android::NO_ERROR) {
        ALOGW("Error connecting to the PowerHAL");
        return -EINVAL;
    }

    int64_t rate;
    // Try to get preferred rate to determine if it's supported
    auto ret = mPowerHalAidl->getHintSessionPreferredRate(&rate);

    int32_t out;
    if (ret.isOk()) {
        ALOGV("Power hint session is supported");
        out = android::NO_ERROR;
    } else if (ret.getExceptionCode() == EX_UNSUPPORTED_OPERATION) {
        ALOGW("Power hint session unsupported");
        out = -EOPNOTSUPP;
    } else {
        ALOGW("Error checking power hint status");
        out = -EINVAL;
    }

    mHintSessionSupportChecked = true;
    sSharedDisplayData.hintSessionSupported = out;
    return out;
}

int32_t Perf::PowerHalHintWorker::updateIdleHint(int64_t deadlineTime, bool forceUpdate) {
    int32_t ret = checkIdleHintSupport();
    if (ret != android::NO_ERROR) {
        return ret;
    }
    bool enableIdleHint =
            (deadlineTime < systemTime(SYSTEM_TIME_MONOTONIC) && CC_LIKELY(deadlineTime > 0));

    if (mIdleHintIsEnabled != enableIdleHint || forceUpdate) {
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

int32_t Perf::PowerHalHintWorker::sendActualWorkDuration() {
    Lock();
    if (mPowerHintSession == nullptr) {
        ALOGW("Cannot send actual work duration, power hint session not running");
        Unlock();
        return -EINVAL;
    }

    if (!needSendActualWorkDurationLocked()) {
        Unlock();
        return android::NO_ERROR;
    }

    if (mActualWorkDuration.has_value()) {
        mLastErrorSent = *mActualWorkDuration - mTargetWorkDuration;
    }

    std::vector<WorkDuration> hintQueue(std::move(mPowerHintQueue));
    mPowerHintQueue.clear();
    Unlock();

    ALOGV("Sending hint update batch");
    mLastActualReportTimestamp = systemTime(SYSTEM_TIME_MONOTONIC);
    auto ret = mPowerHintSession->reportActualWorkDuration(hintQueue);
    if (!ret.isOk()) {
        ALOGW("Failed to report power hint session timing:  %s %s", ret.getMessage(),
              ret.getDescription().c_str());
        if (ret.getExceptionCode() == EX_TRANSACTION_FAILED) {
            Lock();
            mPowerHalExtAidl = nullptr;
            Unlock();
        }
    }
    return ret.isOk() ? android::NO_ERROR : -EINVAL;
}

int32_t Perf::PowerHalHintWorker::updateTargetWorkDuration() {
    if (sNormalizeTarget) {
        return android::NO_ERROR;
    }

    if (mPowerHintSession == nullptr) {
        ALOGW("Cannot send target work duration, power hint session not running");
        return -EINVAL;
    }

    Lock();

    if (!needUpdateTargetWorkDurationLocked()) {
        Unlock();
        return android::NO_ERROR;
    }

    nsecs_t targetWorkDuration = mTargetWorkDuration;
    mLastTargetDurationReported = targetWorkDuration;
    Unlock();

    ALOGV("Sending target time: %lld ns", static_cast<long long>(targetWorkDuration));
    auto ret = mPowerHintSession->updateTargetWorkDuration(targetWorkDuration);
    if (!ret.isOk()) {
        ALOGW("Failed to send power hint session target:  %s %s", ret.getMessage(),
              ret.getDescription().c_str());
        if (ret.getExceptionCode() == EX_TRANSACTION_FAILED) {
            Lock();
            mPowerHalExtAidl = nullptr;
            Unlock();
        }
    }
    return ret.isOk() ? android::NO_ERROR : -EINVAL;
}

void Perf::PowerHalHintWorker::signalActualWorkDuration(nsecs_t actualDurationNanos) {
    ATRACE_CALL();

    if (!usePowerHintSession()) {
        return;
    }
    Lock();
    // convert to long long so "%lld" works correctly
    long long actualNs = actualDurationNanos, targetNs = mTargetWorkDuration;
    ALOGV("Sending actual work duration of: %lld on target: %lld with error: %lld", actualNs,
          targetNs, actualNs - targetNs);
    if (sTraceHintSessionData) {
        ATRACE_INT64("Measured duration", actualNs);
        ATRACE_INT64("Target error term", actualNs - targetNs);
    }

    WorkDuration work;
    work.timeStampNanos = systemTime();
    work.durationNanos = actualDurationNanos;
    if (sNormalizeTarget) {
        work.durationNanos += mLastTargetDurationReported - mTargetWorkDuration;
    }

    mPowerHintQueue.push_back(work);
    // store the non-normalized last value here
    mActualWorkDuration = actualDurationNanos;

    bool shouldSignal = needSendActualWorkDurationLocked();
    Unlock();
    if (shouldSignal) {
        Signal();
    }
}

void Perf::PowerHalHintWorker::signalTargetWorkDuration(nsecs_t targetDurationNanos) {
    if (!usePowerHintSession()) {
        return;
    }
    Lock();
    mTargetWorkDuration = targetDurationNanos - kTargetSafetyMargin.count();

    if (sTraceHintSessionData) ATRACE_INT64("Time target", mTargetWorkDuration);
    bool shouldSignal = false;
    if (!sNormalizeTarget) {
        shouldSignal = needUpdateTargetWorkDurationLocked();
        if (shouldSignal && mActualWorkDuration.has_value() && sTraceHintSessionData) {
            ATRACE_INT64("Target error term", *mActualWorkDuration - mTargetWorkDuration);
        }
    }
    Unlock();
    if (shouldSignal) {
        Signal();
    }
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
    bool useHintSession = usePowerHintSession();
    // if the tids have updated, we restart the session
    if (mTidsUpdated && useHintSession) mPowerHintSession = nullptr;
    bool needStartHintSession =
            (mPowerHintSession == nullptr) && useHintSession && !mBinderTids.empty();
    int ret = android::NO_ERROR;
    int64_t timeout = -1;
    if (!mNeedUpdateRefreshRateHint && !needUpdateIdleHintLocked(timeout) &&
        !needSendActualWorkDurationLocked() && !needStartHintSession &&
        !needUpdateTargetWorkDurationLocked()) {
        ret = WaitForSignalOrExitLocked(timeout);
    }

    // exit() signal received
    if (ret == -EINTR) {
        Unlock();
        return;
    }

    // store internal values so they are consistent after Unlock()
    // some defined earlier also might have changed during the wait
    useHintSession = usePowerHintSession();
    needStartHintSession = (mPowerHintSession == nullptr) && useHintSession && !mBinderTids.empty();

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
    mTidsUpdated = false;
    mNeedUpdateRefreshRateHint = false;

    bool forceUpdateIdleHint = mForceUpdateIdleHint;
    mForceUpdateIdleHint = false;

    Unlock();

    if (!mHintSessionSupportChecked) {
        checkPowerHintSessionSupport();
    }

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

    if (useHintSession) {
        if (needStartHintSession) {
            startHintSession();
        }
        sendActualWorkDuration();
        updateTargetWorkDuration();
    }
}

void Perf::PowerHalHintWorker::addBinderTid(pid_t tid) {
    Lock();
    if (mBinderTids.count(tid) != 0) {
        Unlock();
        return;
    }
    mTidsUpdated = true;
    mBinderTids.emplace(tid);
    Unlock();
    Signal();
}

void Perf::PowerHalHintWorker::removeBinderTid(pid_t tid) {
    Lock();
    if (mBinderTids.erase(tid) == 0) {
        Unlock();
        return;
    }
    mTidsUpdated = true;
    Unlock();
    Signal();

}

int32_t Perf::PowerHalHintWorker::startHintSession() {
    Lock();
    std::vector<int> tids(mBinderTids.begin(), mBinderTids.end());
    nsecs_t targetWorkDuration =
            sNormalizeTarget ? mLastTargetDurationReported : mTargetWorkDuration;
    // we want to stay locked during this one since it assigns "mPowerHintSession"
    auto ret = mPowerHalAidl->createHintSession(getpid(), static_cast<uid_t>(getuid()), tids,
                                                targetWorkDuration, &mPowerHintSession);
    if (!ret.isOk()) {
        ALOGW("Failed to start power hal hint session with error  %s %s", ret.getMessage(),
              ret.getDescription().c_str());
        if (ret.getExceptionCode() == EX_TRANSACTION_FAILED) {
            mPowerHalExtAidl = nullptr;
        }
        Unlock();
        return -EINVAL;
    } else {
        mLastTargetDurationReported = targetWorkDuration;
    }
    Unlock();
    return android::NO_ERROR;
}

bool Perf::PowerHalHintWorker::checkPowerHintSessionReady() {
    static constexpr const std::chrono::milliseconds maxFlagWaitTime = 20s;
    static const std::string propName =
            "debug.sf.adpf_cpu_hint_enable";
    static std::once_flag hintSessionFlag;
    // wait once for 20 seconds in another thread for the value to become available, or give up
    std::call_once(hintSessionFlag, [&] {
        std::thread hintSessionChecker([&] {
            std::optional<std::string> flagValue =
                    waitForPropertyValue(propName, maxFlagWaitTime.count());
            bool enabled = flagValue.has_value() &&
                    (android::base::ParseBool(flagValue->c_str()) == android::base::ParseBoolResult::kTrue);
            std::scoped_lock lock(sSharedDisplayMutex);
            sSharedDisplayData.hintSessionEnabled = enabled;
        });
        hintSessionChecker.detach();
    });
    std::scoped_lock lock(sSharedDisplayMutex);
    return sSharedDisplayData.hintSessionEnabled.has_value() &&
            sSharedDisplayData.hintSessionSupported.has_value();
}

bool Perf::PowerHalHintWorker::usePowerHintSession() {
    std::optional<bool> useSessionCached{mUsePowerHintSession.load()};
    if (useSessionCached.has_value()) {
        return *useSessionCached;
    }
    if (!checkPowerHintSessionReady()) return false;
    std::scoped_lock lock(sSharedDisplayMutex);
    bool out = *(sSharedDisplayData.hintSessionEnabled) &&
            (*(sSharedDisplayData.hintSessionSupported) == android::NO_ERROR);
    mUsePowerHintSession.store(out);
    return out;
}

bool Perf::PowerHalHintWorker::needUpdateTargetWorkDurationLocked() {
    if (!usePowerHintSession() || sNormalizeTarget) return false;
    // to disable the rate limiter we just use a max deviation of 1
    nsecs_t maxDeviation = sUseRateLimiter ? kAllowedDeviation.count() : 1;
    // report if the change in target from our last submission to now exceeds the threshold
    return abs(mTargetWorkDuration - mLastTargetDurationReported) >= maxDeviation;
}

bool Perf::PowerHalHintWorker::needSendActualWorkDurationLocked() {
    if (!usePowerHintSession() || mPowerHintQueue.size() == 0 || !mActualWorkDuration.has_value()) {
        return false;
    }
    if (!mLastErrorSent.has_value() ||
        (systemTime(SYSTEM_TIME_MONOTONIC) - mLastActualReportTimestamp) > kStaleTimeout.count()) {
        return true;
    }
    // to effectively disable the rate limiter we just use a max deviation of 1
    nsecs_t maxDeviation = sUseRateLimiter ? kAllowedDeviation.count() : 1;
    // report if the change in error term from our last submission to now exceeds the threshold
    return abs((*mActualWorkDuration - mTargetWorkDuration) - *mLastErrorSent) >= maxDeviation;
}

// track the tid of any thread that calls in and remove it on thread death
void Perf::PowerHalHintWorker::trackThisThread() {
    thread_local struct TidTracker {
        TidTracker(PowerHalHintWorker *worker) : mWorker(worker) {
            mTid = gettid();
            mWorker->addBinderTid(mTid);
        }
        ~TidTracker() { mWorker->removeBinderTid(mTid); }
        pid_t mTid;
        PowerHalHintWorker *mWorker;
    } tracker(this);
}

const bool Perf::PowerHalHintWorker::sTraceHintSessionData =
        android::base::GetBoolProperty(std::string("debug.hwc.trace_hint_sessions"), false);

const bool Perf::PowerHalHintWorker::sNormalizeTarget =
        android::base::GetBoolProperty(std::string("debug.hwc.normalize_hint_session_durations"), false);

const bool Perf::PowerHalHintWorker::sUseRateLimiter =
        android::base::GetBoolProperty(std::string("debug.hwc.use_rate_limiter"), true);

Perf::PowerHalHintWorker::SharedDisplayData
        Perf::PowerHalHintWorker::sSharedDisplayData;

std::mutex Perf::PowerHalHintWorker::sSharedDisplayMutex;


// we can cache the value once it is known to avoid the lock after boot
// we also only actually check once per frame to keep the value internally consistent
bool Perf::usePowerHintSession() {
    if (!mUsePowerHintSession.has_value() && mPowerHalHint.checkPowerHintSessionReady()) {
        mUsePowerHintSession = mPowerHalHint.usePowerHintSession();
    }
//    ALOGI("Using hint session: %d", mUsePowerHintSession);
    return mUsePowerHintSession.value_or(false);
}

}  // namespace sdm
