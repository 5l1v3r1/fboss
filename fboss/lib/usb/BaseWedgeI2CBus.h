/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include "fboss/lib/usb/CP2112.h"
#include "fboss/lib/usb/TransceiverI2CApi.h"

#include <folly/Range.h>
#include <mutex>

namespace facebook::fboss {
/*
 * A small wrapper around CP2112 which is aware of the topology of wedge's QSFP
 * I2C bus, and can select specific QSFPs to query.
 */
class BaseWedgeI2CBus : public TransceiverI2CApi {
 public:
  explicit BaseWedgeI2CBus(std::unique_ptr<CP2112Intf> dev = nullptr) {
    dev_ = (dev) ? std::move(dev) : std::make_unique<CP2112>();
  }
  ~BaseWedgeI2CBus() override {}
  void open() override;
  void close() override;
  void moduleRead(
      unsigned int module,
      uint8_t i2cAddress,
      int offset,
      int len,
      uint8_t* buf) override;
  void moduleWrite(
      unsigned int module,
      uint8_t i2cAddress,
      int offset,
      int len,
      const uint8_t* buf) override;
  void read(uint8_t i2cAddress, int offset, int len, uint8_t* buf);
  void write(uint8_t i2cAddress, int offset, int len, const uint8_t* buf);

  bool isPresent(unsigned int module) override;
  void scanPresence(std::map<int32_t, ModulePresence>& presences) override;

  /* Platform function to count the i2c transactions in a platform. This
   * function gets the i2c controller stats and returns it in form of a vector
   * to the caller
   */
  std::vector<std::reference_wrapper<const I2cControllerStats>>
  getI2cControllerStats() override {
    std::vector<std::reference_wrapper<const I2cControllerStats>>
        i2cControllerCurrentStats;

    // Get the i2c controller platform stats from the controller class like
    // CP2112 class and return
    i2cControllerCurrentStats.push_back(dev_->getI2cControllerPlatformStats());

    return i2cControllerCurrentStats;
  }

 protected:
  enum : unsigned int {
    NO_PORT = 0,
  };

  virtual void initBus() = 0;
  virtual void selectQsfpImpl(unsigned int module) = 0;

  std::unique_ptr<CP2112Intf> dev_;
  unsigned int selectedPort_{NO_PORT};

 private:
  /*
   * Set the PCA9548 switches so that we can read from the selected QSFP
   * module.
   */
  void selectQsfp(unsigned int module);
  void unselectQsfp();

  // Forbidden copy constructor and assignment operator
  BaseWedgeI2CBus(BaseWedgeI2CBus const&) = delete;
  BaseWedgeI2CBus& operator=(BaseWedgeI2CBus const&) = delete;
};

} // namespace facebook::fboss
