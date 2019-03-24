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
#include "fboss/agent/gen-cpp2/switch_config_types.h"
#include "fboss/agent/types.h"

#include <folly/MacAddress.h>

#include <vector>


extern "C" {
#include <opennsl/port.h>
}
/*
 * This utility is to provide utils for bcm test.
 */
namespace facebook {
namespace fboss {
namespace utility {
/*
 * Use vlan 1000, as the base vlan for ports in configs generated here.
 * Anything except 0, 1 would actually work fine. 0 because
 * its reserved, and 1 because BRCM uses that as default VLAN.
 * So for example if we use VLAN 1, BRCM will also add cpu port to
 * that vlan along with our configured ports. This causes unnecessary
 * confusion for our tests.
 */
auto constexpr kBaseVlanId = 1000;
/*
 * Default VLAN
 */
auto constexpr kDefaultVlanId = 1;

folly::MacAddress kLocalCpuMac();

cfg::SwitchConfig onePortConfig(int unit, opennsl_port_t port);
cfg::SwitchConfig oneL3IntfConfig(
    int unit,
    opennsl_port_t port,
    cfg::PortLoopbackMode lbMode = cfg::PortLoopbackMode::NONE);
cfg::SwitchConfig oneL3IntfNoIPAddrConfig(
    int unit, opennsl_port_t port,
    cfg::PortLoopbackMode lbMode = cfg::PortLoopbackMode::NONE);
cfg::SwitchConfig oneL3IntfTwoPortConfig(int unit,
    opennsl_port_t port1, opennsl_port_t port2,
    cfg::PortLoopbackMode lbMode = cfg::PortLoopbackMode::NONE);
cfg::SwitchConfig oneL3IntfNPortConfig(
    int unit,
    const std::vector<opennsl_port_t>& ports,
    cfg::PortLoopbackMode lbMode = cfg::PortLoopbackMode::NONE,
    bool interfaceHasSubnet=true);

cfg::SwitchConfig onePortPerVlanConfig(
    int unit,
    const std::vector<opennsl_port_t>& ports,
    cfg::PortLoopbackMode lbMode = cfg::PortLoopbackMode::NONE,
    bool interfaceHasSubnet=true);

cfg::SwitchConfig twoL3IntfConfig(int unit,
    opennsl_port_t port1, opennsl_port_t port2);
cfg::SwitchConfig multiplePortSingleVlanConfig(
    int unit,
    const std::vector<opennsl_port_t>& ports);
} // namespace utility
} // namespace fboss
} // namespace facebook