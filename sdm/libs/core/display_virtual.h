/*
* Copyright (c) 2014 - 2020, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without modification, are permitted
* provided that the following conditions are met:
*    * Redistributions of source code must retain the above copyright notice, this list of
*      conditions and the following disclaimer.
*    * Redistributions in binary form must reproduce the above copyright notice, this list of
*      conditions and the following disclaimer in the documentation and/or other materials provided
*      with the distribution.
*    * Neither the name of The Linux Foundation nor the names of its contributors may be used to
*      endorse or promote products derived from this software without specific prior written
*      permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
* NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
* OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
* STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef __DISPLAY_VIRTUAL_H__
#define __DISPLAY_VIRTUAL_H__

#include <private/hw_info_types.h>
#include "display_base.h"

namespace sdm {

class HWVirtualInterface;

class DisplayVirtual : public DisplayBase {
 public:
  DisplayVirtual(DisplayEventHandler *event_handler, HWInfoInterface *hw_info_intf,
                 BufferAllocator *buffer_allocator, CompManager *comp_manager);
  DisplayVirtual(int32_t display_id, DisplayEventHandler *event_handler,
                 HWInfoInterface *hw_info_intf, BufferAllocator *buffer_allocator,
                 CompManager *comp_manager);
  virtual DisplayError Init();
  virtual DisplayError Prepare(LayerStack *layer_stack);
  virtual DisplayError GetNumVariableInfoConfigs(uint32_t *count);
  virtual DisplayError GetConfig(uint32_t index, DisplayConfigVariableInfo *variable_info);
  virtual DisplayError GetActiveConfig(uint32_t *index);
  virtual DisplayError SetActiveConfig(uint32_t index) {
    return kErrorNotSupported;
  }
  virtual DisplayError SetActiveConfig(DisplayConfigVariableInfo *variable_info);
  virtual DisplayError SetMixerResolution(uint32_t width, uint32_t height) {
    return kErrorNotSupported;
  }
  virtual DisplayError SetPanelLuminanceAttributes(float min_lum, float max_lum);
  virtual DisplayError SetVSyncState(bool enable) {
    return kErrorNotSupported;
  }
  virtual DisplayError SetRefreshRate(uint32_t refresh_rate, bool final_rate, bool idle_screen) {
    return kErrorNotSupported;
  }
  virtual DisplayError SetDetailEnhancerData(const DisplayDetailEnhancerData &de_data) {
    return kErrorNotSupported;
  }
  virtual DisplayError ValidateGPUTargetParams() {
    // TODO(user): Validate GPU target for virtual display when query display attributes
    // on virtual display is functional.
    return kErrorNone;
  }
  virtual DisplayError SetColorTransform(const uint32_t length, const double *color_transform) {
    return kErrorNone;
  }
  virtual DisplayError GetColorModeCount(uint32_t *mode_count);
  virtual DisplayError colorSamplingOn();
  virtual DisplayError colorSamplingOff();
  virtual DisplayError TeardownConcurrentWriteback(void) { return kErrorNotSupported; }
  virtual DisplayError SetExpectedPresentTime(int64_t expectedPresentTime) {
    return kErrorNotSupported;
  }

 protected:
  float set_max_lum_ = -1.0;
  float set_min_lum_ = -1.0;
};

}  // namespace sdm

#endif  // __DISPLAY_VIRTUAL_H__

