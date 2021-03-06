// Copyright 2004-present Facebook. All Rights Reserved.

#include "fboss/agent/hw/bcm/BcmIntf.h"
#include "fboss/agent/hw/bcm/BcmMultiPathNextHop.h"
#include "fboss/agent/hw/bcm/BcmPortTable.h"
#include "fboss/agent/hw/bcm/BcmRoute.h"
#include "fboss/agent/hw/bcm/tests/BcmLinkStateDependentTests.h"
#include "fboss/agent/hw/bcm/tests/BcmMplsTestUtils.h"
#include "fboss/agent/hw/bcm/tests/BcmTestRouteUtils.h"
#include "fboss/agent/hw/test/ConfigFactory.h"
#include "fboss/agent/state/RouteUpdater.h"
#include "fboss/agent/test/EcmpSetupHelper.h"

extern "C" {
#include <bcm/l3.h>
}
namespace facebook::fboss {

namespace {
using TestTypes = ::testing::Types<folly::IPAddressV4, folly::IPAddressV6>;
const LabelForwardingAction::LabelStack kStack0{
    101,
    102,
    103,
    104,
    105,
    106,
    107,
    108,
    109,
    110,
};
const LabelForwardingAction::LabelStack kStack1{
    201,
    202,
    203,
    204,
    205,
    206,
    207,
    208,
    209,
    210,
};

template <typename AddrT>
struct TestParameters {
  using PrefixT = typename Route<AddrT>::Prefix;
  PrefixT prefix; // prefix for route to remote dest
  AddrT nexthop; // next hop for that route
  const LabelForwardingAction::LabelStack* stack; // label stack to prefix
  bcm_mpls_label_t label; // labels advertised by "LDP" peer (OpenR adjacency)
};
} // namespace

template <typename AddrT>
class BcmLabelEdgeRouteTest : public BcmLinkStateDependentTests {
 public:
  using EcmpSetupTargetedPorts = utility::EcmpSetupTargetedPorts<AddrT>;
  using EcmpNextHop = utility::EcmpNextHop<AddrT>;
  using PrefixT = typename Route<AddrT>::Prefix;
  const static PrefixT kRoutePrefix;
  const static AddrT kRouteNextHopAddress;
  static auto constexpr kWidth = 4;

  cfg::SwitchConfig initialConfig() const override {
    std::vector<PortID> ports;
    for (auto i = 0; i < kWidth; i++) {
      ports.push_back(masterLogicalPortIds()[i]);
    }
    return utility::onePortPerVlanConfig(
        getHwSwitch(), ports, cfg::PortLoopbackMode::MAC);
  }

  TestParameters<AddrT> testParams(int i) {
    return kParams[i % 2];
  }

  void setupL3Route(
      ClientID client,
      AddrT network,
      uint8_t mask,
      AddrT nexthop,
      LabelForwardingAction::LabelStack stack = {}) {
    /* setup route to network/mask via nexthop,
      setup ip2mpls if stack is given, else setup ip route */
    std::optional<LabelForwardingAction> labelAction;
    if (!stack.empty()) {
      labelAction = LabelForwardingAction(
          LabelForwardingAction::LabelForwardingType::PUSH, std::move(stack));
    }
    auto state = getProgrammedState();
    RouteUpdater updater(state->getRouteTables());
    updater.addRoute(
        utility::kRouter0,
        network,
        mask,
        client,
        RouteNextHopEntry(
            UnresolvedNextHop(nexthop, ECMP_WEIGHT, labelAction),
            AdminDistance::MAX_ADMIN_DISTANCE));
    auto tables = updater.updateDone();
    tables->publish();
    auto newState = state->clone();
    newState->resetRouteTables(tables);
    applyNewState(newState);
  }

  void resolveLabeledNextHops(AddrT network, uint8_t mask) {
    // resolve neighbors of labeled egresses
    PrefixT prefix{network, mask};
    auto* ecmpHelper = ecmpHelpers_[prefix].get();
    auto state = getProgrammedState()->clone();
    applyNewState(ecmpHelper->resolveNextHops(state, labeledPorts_));
  }

  void resolveUnLabeledNextHops(AddrT network, uint8_t mask) {
    // resolve neighbors of unlabeled egresses
    PrefixT prefix{network, mask};
    auto* ecmpHelper = ecmpHelpers_[prefix].get();
    applyNewState(
        ecmpHelper->resolveNextHops(getProgrammedState(), unLabeledPorts_));
  }

  void unresolveLabeledNextHops(AddrT network, uint8_t mask) {
    // unresolve neighbors of labeled egresses
    PrefixT prefix{network, mask};
    auto* ecmpHelper = ecmpHelpers_[prefix].get();
    auto state = getProgrammedState()->clone();
    applyNewState(ecmpHelper->unresolveNextHops(state, labeledPorts_));
  }

  void unresolveUnLabeledNextHops(AddrT network, uint8_t mask) {
    // unresolve neighbors of unlabeled egresses
    PrefixT prefix{network, mask};
    auto* ecmpHelper = ecmpHelpers_[prefix].get();
    applyNewState(
        ecmpHelper->unresolveNextHops(getProgrammedState(), unLabeledPorts_));
  }

  std::map<PortDescriptor, LabelForwardingAction::LabelStack> port2LabelStacks(
      LabelForwardingAction::Label label) {
    std::map<PortDescriptor, LabelForwardingAction::LabelStack> result;
    for (auto port : labeledPorts_) {
      auto itr = result.emplace(port, LabelForwardingAction::LabelStack{});
      itr.first->second.push_back(label++);
    }
    for (auto port : unLabeledPorts_) {
      auto itr = result.emplace(port, LabelForwardingAction::LabelStack{});
    }
    return result;
  }

  flat_set<PortDescriptor> allPorts() {
    flat_set<PortDescriptor> result{unLabeledPorts_};
    result.merge(labeledPorts_);
    return result;
  }

  void setupEcmpForwarding(
      AddrT network,
      uint8_t mask,
      LabelForwardingAction::Label tunnelLabel) {
    std::map<PortDescriptor, LabelForwardingAction::LabelStack> stacks =
        port2LabelStacks(tunnelLabel);
    typename Route<AddrT>::Prefix prefix{network, mask};
    auto* ecmpHelper = ecmpHelpers_[prefix].get();
    if (!stacks.empty()) {
      applyNewState(ecmpHelper->setupIp2MplsECMPForwarding(
          getProgrammedState(), allPorts(), std::move(stacks), {prefix}));
    } else {
      applyNewState(ecmpHelper->setupECMPForwarding(
          getProgrammedState(), allPorts(), {prefix}));
    }
  }

  void setupECMPHelper(
      uint8_t unlabeledPaths,
      uint8_t labeledPaths,
      AddrT network,
      uint8_t mask) {
    // setup ecmp helper for network and mask, with labeled and unlabaled paths
    typename Route<AddrT>::Prefix prefix{network, mask};

    auto emplaced = ecmpHelpers_.emplace(std::make_pair(
        prefix,
        std::make_unique<EcmpSetupTargetedPorts>(
            getProgrammedState(), RouterID(0))));

    EXPECT_TRUE(emplaced.second);
    const auto& ecmpHelper = emplaced.first->second;
    auto ports = ecmpHelper->ecmpPortDescs(kWidth);
    for (auto i = 0; i < unlabeledPaths; i++) {
      unLabeledPorts_.insert(ports[i]);
    }
    for (auto i = unlabeledPaths; i < unlabeledPaths + labeledPaths; i++) {
      labeledPorts_.insert(ports[i]);
    }
  }

  boost::container::flat_set<PortDescriptor> labeledEgressPorts() {
    return labeledPorts_;
  }

  boost::container::flat_set<PortDescriptor> unLabeledEgressPorts() {
    return unLabeledPorts_;
  }

  void verifyLabeledNextHop(bcm_if_t egressId, bcm_mpls_label_t label) {
    // verify that egress id has given label attached
    utility::verifyLabeledEgress(egressId, label);
  }

  void verifyLabeledNextHopWithStack(
      PrefixT prefix,
      const LabelForwardingAction::LabelStack& tunnelStack) {
    auto* bcmRoute = this->getHwSwitch()->routeTable()->getBcmRoute(
        0, prefix.network, prefix.mask);
    auto egressId = bcmRoute->getEgressId();
    // verify that given egress is tunneled egress
    // its egress label must be tunnelLabel (top of stack)
    // rest of srack is from tunnel interface attached to egress
    utility::verifyTunneledEgress(egressId, tunnelStack);
  }

  void verifyLabeledMultiPathNextHopMemberWithStack(
      PrefixT prefix,
      int memberIndex,
      const LabelForwardingAction::LabelStack& tunnelStack,
      bool resolved) {
    auto* bcmRoute = this->getHwSwitch()->routeTable()->getBcmRoute(
        0, prefix.network, prefix.mask);
    ASSERT_NE(bcmRoute, nullptr);
    const auto* egr =
        dynamic_cast<const BcmEcmpEgress*>(bcmRoute->getNextHop()->getEgress());
    ASSERT_NE(egr, nullptr);
    const auto& nextHops = egr->paths();
    EXPECT_GT(nextHops.size(), memberIndex);
    auto i = 0;
    for (const auto nextHopId : nextHops) {
      if (i == memberIndex) {
        // verify that given egress is tunneled egress
        // its egress label must be tunnelLabel (top of stack)
        // rest of srack is from tunnel interface attached to egress
        if (resolved) {
          utility::verifyTunneledEgress(nextHopId, tunnelStack);
        } else {
          utility::verifyTunneledEgressToDrop(nextHopId, tunnelStack);
        }
      }
      i++;
    }
  }

  void verifyMultiPathNextHop(
      PrefixT prefix,
      const std::map<PortDescriptor, LabelForwardingAction::LabelStack>&
          stacks) {
    auto* bcmRoute = this->getHwSwitch()->routeTable()->getBcmRoute(
        0, prefix.network, prefix.mask);
    auto egressId = bcmRoute->getEgressId(); // ecmp egress id

    std::map<bcm_port_t, LabelForwardingAction::LabelStack> bcmPort2Stacks;
    for (auto& entry : stacks) {
      auto portDesc = entry.first;
      auto& stack = entry.second;
      auto bcmPort = this->getHwSwitch()->getPortTable()->getBcmPortId(
          portDesc.phyPortID());
      bcmPort2Stacks.emplace(bcmPort, stack);
    }

    utility::verifyLabeledMultiPathEgress(
        unLabeledPorts_.size(), labeledPorts_.size(), egressId, bcmPort2Stacks);
  }

  long getTunnelRefCount(
      InterfaceID intfID,
      const LabelForwardingAction::LabelStack& stack) {
    return getHwSwitch()
        ->getIntfTable()
        ->getBcmIntfIf(intfID)
        ->getLabeledTunnelRefCount(stack);
  }

  void verifyTunnelRefCounts(
      const AddrT& network,
      uint8_t mask,
      const PortDescriptor& port,
      const LabelForwardingAction::LabelStack& stack,
      long refCount) {
    PrefixT prefix{network, mask};
    auto helper = ecmpHelpers_[prefix].get();
    auto vlanID = helper->getVlan(port);
    EXPECT_TRUE(vlanID.has_value());
    if (stack.empty()) {
      EXPECT_EQ(
          refCount,
          getTunnelRefCount(
              getProgrammedState()
                  ->getVlans()
                  ->getVlan(vlanID.value())
                  ->getInterfaceID(),
              stack));
    } else {
      EXPECT_EQ(
          refCount,
          getTunnelRefCount(
              getProgrammedState()
                  ->getVlans()
                  ->getVlan(vlanID.value())
                  ->getInterfaceID(),
              LabelForwardingAction::LabelStack{stack.begin() + 1,
                                                stack.end()}));
    }
  }

 protected:
  std::map<PrefixT, std::unique_ptr<EcmpSetupTargetedPorts>> ecmpHelpers_;
  boost::container::flat_set<PortDescriptor> labeledPorts_;
  boost::container::flat_set<PortDescriptor> unLabeledPorts_;
  static const std::array<TestParameters<AddrT>, 2> kParams;
};

template <>
const std::array<TestParameters<folly::IPAddressV4>, 2> BcmLabelEdgeRouteTest<
    folly::IPAddressV4>::kParams{
    TestParameters<folly::IPAddressV4>{
        Route<folly::IPAddressV4>::Prefix{folly::IPAddressV4("101.102.103.0"),
                                          24},
        folly::IPAddressV4{"11.12.13.1"},
        &kStack0,
        1001},
    TestParameters<folly::IPAddressV4>{
        Route<folly::IPAddressV4>::Prefix{folly::IPAddressV4("201.202.203.0"),
                                          24},
        folly::IPAddressV4{"21.22.23.1"},
        &kStack1,
        2001}};

template <>
const std::array<TestParameters<folly::IPAddressV6>, 2>
    BcmLabelEdgeRouteTest<folly::IPAddressV6>::kParams{
        TestParameters<folly::IPAddressV6>{
            Route<folly::IPAddressV6>::Prefix{
                folly::IPAddressV6("101:102::103:0:0"),
                96},
            folly::IPAddressV6{"101:102::103:0:0:0:1"},
            &kStack0,
            1001},
        TestParameters<folly::IPAddressV6>{
            Route<folly::IPAddressV6>::Prefix{
                folly::IPAddressV6("201:202::203:0:0"),
                96},
            folly::IPAddressV6{"201:202::203:0:0:0:1"},
            &kStack1,
            2001}};

TYPED_TEST_CASE(BcmLabelEdgeRouteTest, TestTypes);

TYPED_TEST(BcmLabelEdgeRouteTest, OneLabel) {
  // setup nexthop with only one label
  // test that labeled egress is used
  // test that tunnel initiator is  not setup
  // test that route is setup to labeled egress
  auto params = this->testParams(0);
  this->setupECMPHelper(0, 1, params.nexthop, params.prefix.mask);
  auto setup = [=]() {
    this->setupL3Route(
        ClientID::BGPD,
        params.prefix.network,
        params.prefix.mask,
        params.nexthop); // unlabaled route from client
    this->resolveLabeledNextHops(params.nexthop, params.prefix.mask);
    this->setupEcmpForwarding(params.nexthop, params.prefix.mask, params.label);
    this->getProgrammedState();
  };
  auto verify = [=]() {
    auto* bcmRoute = this->getHwSwitch()->routeTable()->getBcmRoute(
        0, params.prefix.network, params.prefix.mask);
    auto egressId = bcmRoute->getEgressId();
    this->verifyLabeledNextHop(egressId, params.label);
    for (const auto& port : this->labeledEgressPorts()) {
      this->verifyTunnelRefCounts(
          params.nexthop,
          params.prefix.mask,
          port,
          LabelForwardingAction::LabelStack{},
          1);
    }
  };
  this->verifyAcrossWarmBoots(setup, verify);
}

TYPED_TEST(BcmLabelEdgeRouteTest, MaxLabels) {
  // setup nexthop with max labels
  // test that labeled egress is used
  // test that tunnel initiator is setup
  // test that route is setup to labeled egress
  // test that labeled egress is associated with tunnel
  auto maxSize = this->getHwSwitch()->getPlatform()->maxLabelStackDepth();
  auto params = this->testParams(0);
  this->setupECMPHelper(0, 1, params.nexthop, params.prefix.mask);
  auto setup = [=]() {
    // program l3 route with stack of size one less than maximum supported
    // additional label is adjacency label, which completes stack depth
    this->setupL3Route(
        ClientID::BGPD,
        params.prefix.network,
        params.prefix.mask,
        params.nexthop,
        LabelForwardingAction::LabelStack(
            params.stack->begin(), params.stack->begin() + maxSize - 1));
    this->resolveLabeledNextHops(params.nexthop, params.prefix.mask);
    this->setupEcmpForwarding(
        params.nexthop,
        params.prefix.mask,
        params.label); // apply adjacency label
    this->getProgrammedState();
  };
  auto verify = [=]() {
    // prepare expected stack
    // adjacency/tunnel label will be on top,
    // other labels will at the bottom of stack
    // the bottom most label in route's label stack will be attached to egress
    LabelForwardingAction::LabelStack stack{
        params.stack->begin(), params.stack->begin() + maxSize - 1};
    stack.push_back(params.label);
    this->verifyLabeledNextHopWithStack(params.prefix, stack);

    for (const auto& port : this->labeledEgressPorts()) {
      this->verifyTunnelRefCounts(
          params.nexthop, params.prefix.mask, port, stack, 1);
    }
  };
  this->verifyAcrossWarmBoots(setup, verify);
}

TYPED_TEST(BcmLabelEdgeRouteTest, ExceedMaxLabels) {
  // setup nexthop with stack exceeding labels
  // n
  auto maxSize = this->getHwSwitch()->getPlatform()->maxLabelStackDepth();
  auto params = this->testParams(0);
  this->setupECMPHelper(0, 1, params.nexthop, params.prefix.mask);
  auto setup = [=]() {
    this->setupL3Route(
        ClientID::BGPD,
        params.prefix.network,
        params.prefix.mask,
        params.nexthop,
        LabelForwardingAction::LabelStack(
            params.stack->begin(), params.stack->begin() + maxSize));
    this->resolveLabeledNextHops(params.nexthop, params.prefix.mask);
    this->setupEcmpForwarding(params.nexthop, params.prefix.mask, params.label);
  };
  EXPECT_THROW(setup(), FbossError);
}

TYPED_TEST(BcmLabelEdgeRouteTest, HalfPathsWithLabels) {
  // setup half next hops with labels and half without
  // test that labeled egress is used for labeled nexthops
  // test that unlabeled egress is used for unlabeled nexthops
  // test that tunnel initiator is setup correctly
  // test that labeled egress is associated with tunnel
  auto params = this->testParams(0);
  this->setupECMPHelper(1, 1, params.nexthop, params.prefix.mask);
  auto setup = [=]() {
    // program l3 route with stack of size one less than maximum supported
    // additional label is adjacency label, which completes stack depth
    this->setupL3Route(
        ClientID::BGPD,
        params.prefix.network,
        params.prefix.mask,
        params.nexthop);
    this->resolveUnLabeledNextHops(params.nexthop, params.prefix.mask);
    this->resolveLabeledNextHops(params.nexthop, params.prefix.mask);
    this->setupEcmpForwarding(params.nexthop, params.prefix.mask, params.label);
    this->getProgrammedState();
  };
  auto verify = [=]() {
    std::map<PortDescriptor, LabelForwardingAction::LabelStack> stacks;

    for (const auto unLabeledPort : this->unLabeledEgressPorts()) {
      stacks.emplace(unLabeledPort, LabelForwardingAction::LabelStack{});
    }

    for (const auto labeledPort : this->labeledEgressPorts()) {
      auto itr =
          stacks.emplace(labeledPort, LabelForwardingAction::LabelStack{});
      itr.first->second.push_back(params.label);
      this->verifyTunnelRefCounts(
          params.nexthop,
          params.prefix.mask,
          labeledPort,
          LabelForwardingAction::LabelStack{},
          1);
    }

    this->verifyMultiPathNextHop(params.prefix, stacks);
  };
  this->verifyAcrossWarmBoots(setup, verify);
}

TYPED_TEST(BcmLabelEdgeRouteTest, PathWithDifferentTunnelLabels) {
  // setup nexthops with common tunnel stack but different egress labels
  // test that labeled egress is used for labeled nexthops
  // test that only required tunnel initiators are set up
  // test that labeled egresses are associated with tunnel
  auto maxSize = this->getHwSwitch()->getPlatform()->maxLabelStackDepth();
  auto params = this->testParams(0);
  this->setupECMPHelper(0, 2, params.nexthop, params.prefix.mask);
  auto setup = [=]() {
    this->setupL3Route(
        ClientID::BGPD,
        params.prefix.network,
        params.prefix.mask,
        params.nexthop,
        LabelForwardingAction::LabelStack(
            params.stack->begin(), params.stack->begin() + maxSize - 1));
    this->resolveLabeledNextHops(params.nexthop, params.prefix.mask);
    this->setupEcmpForwarding(params.nexthop, params.prefix.mask, params.label);
    this->getProgrammedState();
  };
  auto verify = [=]() {
    std::map<PortDescriptor, LabelForwardingAction::LabelStack> stacks;
    auto i = 0;
    for (auto labeledPort : this->labeledEgressPorts()) {
      LabelForwardingAction::LabelStack stack{
          params.stack->begin(), params.stack->begin() + maxSize - 1};
      stack.push_back(params.label + i++);
      stacks.emplace(labeledPort, stack);

      this->verifyTunnelRefCounts(
          params.nexthop, params.prefix.mask, labeledPort, stack, 1);
    }
    this->verifyMultiPathNextHop(params.prefix, stacks);
  };
  this->verifyAcrossWarmBoots(setup, verify);
}

TYPED_TEST(BcmLabelEdgeRouteTest, PathsWithDifferentLabelStackSameTunnelLabel) {
  // setup nexthops with different tunnel stack but with same tunnel labels
  // test that labeled egress is used for labeled nexthops
  // test that only required tunnel initiators are set up
  // test that labeled egresses are associated with tunnel
  auto maxSize = this->getHwSwitch()->getPlatform()->maxLabelStackDepth();

  using ParamsT = TestParameters<TypeParam>;
  std::vector<ParamsT> params{
      this->testParams(0),
      this->testParams(1),
  };
  this->setupECMPHelper(0, 2, params[0].nexthop, params[0].prefix.mask);
  this->setupECMPHelper(0, 2, params[1].nexthop, params[1].prefix.mask);

  bcm_mpls_label_t tunnelLabel = 511;
  auto setup = [=]() {
    for (auto i = 0; i < params.size(); i++) {
      this->setupL3Route(
          ClientID::BGPD,
          params[i].prefix.network,
          params[i].prefix.mask,
          params[i].nexthop,
          LabelForwardingAction::LabelStack(
              params[i].stack->begin(),
              params[i].stack->begin() + maxSize - 1));
      this->resolveLabeledNextHops(params[i].nexthop, params[i].prefix.mask);

      this->setupEcmpForwarding(
          params[i].nexthop, params[i].prefix.mask, tunnelLabel);
    }
    this->getProgrammedState();
  };
  auto verify = [=]() {
    for (auto i = 0; i < params.size(); i++) {
      const auto& param = params[i];
      std::map<PortDescriptor, LabelForwardingAction::LabelStack> stacks;
      auto localTunnelLabel = tunnelLabel;

      for (auto labeledPort : this->labeledEgressPorts()) {
        LabelForwardingAction::LabelStack stack;
        auto pushStack = LabelForwardingAction::LabelStack(
            params[i].stack->begin(), params[i].stack->begin() + maxSize - 1);
        pushStack.push_back(localTunnelLabel);
        stacks.emplace(labeledPort, pushStack);
        localTunnelLabel += 1;
      }
      this->verifyMultiPathNextHop(param.prefix, stacks);
    }
  };
  this->verifyAcrossWarmBoots(setup, verify);
}

TYPED_TEST(BcmLabelEdgeRouteTest, PathsWithSameLabelStackDifferentTunnelLabel) {
  // setup nexthops with common tunnel stack but different egress labels
  // test that labeled egress is used for labeled nexthops
  // test that only required tunnel initiators are set up
  // test that labeled egresses are associated with tunnel
  auto maxSize = this->getHwSwitch()->getPlatform()->maxLabelStackDepth();

  using ParamsT = TestParameters<TypeParam>;
  std::vector<ParamsT> params{
      this->testParams(0),
      this->testParams(1),
  };
  this->setupECMPHelper(0, 2, params[0].nexthop, params[0].prefix.mask);
  this->setupECMPHelper(0, 2, params[1].nexthop, params[1].prefix.mask);

  auto setup = [=]() {
    for (auto i = 0; i < params.size(); i++) {
      this->setupL3Route(
          ClientID::BGPD,
          params[i].prefix.network,
          params[i].prefix.mask,
          params[i].nexthop,
          LabelForwardingAction::LabelStack(
              params[0].stack->begin(),
              params[0].stack->begin() + maxSize - 1));
      this->resolveLabeledNextHops(params[i].nexthop, params[i].prefix.mask);

      this->setupEcmpForwarding(
          params[i].nexthop, params[i].prefix.mask, params[i].label);
    }
    this->getProgrammedState();
  };
  auto verify = [=]() {
    for (auto i = 0; i < params.size(); i++) {
      const auto& param = params[i];
      std::map<PortDescriptor, LabelForwardingAction::LabelStack> stacks;
      auto j = 0;
      for (auto labeledPort : this->labeledEgressPorts()) {
        auto pushStack = LabelForwardingAction::LabelStack(
            params[0].stack->begin(), params[0].stack->begin() + maxSize - 1);
        pushStack.push_back(params[i].label + j);
        this->verifyTunnelRefCounts(
            params[i].nexthop,
            params[i].prefix.mask,
            labeledPort,
            pushStack,
            1);
        stacks.emplace(labeledPort, pushStack);
        j += 1;
      }
      this->verifyMultiPathNextHop(param.prefix, stacks);
    }
  };
  this->verifyAcrossWarmBoots(setup, verify);
}

TYPED_TEST(BcmLabelEdgeRouteTest, RoutesToSameNextHopWithDifferentStack) {
  // setup nexthops with common tunnel stack but different egress labels
  // test that labeled egress is used for labeled nexthops
  // test that only required tunnel initiators are set up
  // test that labeled egresses are associated with tunnel
  auto maxSize = this->getHwSwitch()->getPlatform()->maxLabelStackDepth();

  using ParamsT = TestParameters<TypeParam>;
  std::vector<ParamsT> params{
      this->testParams(0),
      this->testParams(1),
  };
  this->setupECMPHelper(0, 2, params[0].nexthop, params[0].prefix.mask);

  auto setup = [=]() {
    for (auto i = 0; i < params.size(); i++) {
      this->setupL3Route(
          ClientID::BGPD,
          params[i].prefix.network,
          params[i].prefix.mask,
          params[0].nexthop,
          LabelForwardingAction::LabelStack(
              params[i].stack->begin(),
              params[i].stack->begin() + maxSize - 1));
    }
    /* same next hop to 2 prefixes with different stacks */
    this->resolveLabeledNextHops(params[0].nexthop, params[0].prefix.mask);
    this->setupEcmpForwarding(
        params[0].nexthop, params[0].prefix.mask, params[0].label);
    this->getProgrammedState();
  };
  auto verify = [=]() {
    for (auto i = 0; i < params.size(); i++) {
      const auto& param = params[i];
      std::map<PortDescriptor, LabelForwardingAction::LabelStack> stacks;

      auto j = 0;
      for (auto labeledPort : this->labeledEgressPorts()) {
        LabelForwardingAction::LabelStack pushStack{
            params[i].stack->begin(), params[i].stack->begin() + maxSize - 1};
        pushStack.push_back(params[0].label + j);

        stacks.emplace(labeledPort, pushStack);
        j += 1;
      }
      this->verifyMultiPathNextHop(param.prefix, stacks);
    }
  };
  this->verifyAcrossWarmBoots(setup, verify);
}

TYPED_TEST(BcmLabelEdgeRouteTest, UnresolvedNextHops) {
  auto maxSize = this->getHwSwitch()->getPlatform()->maxLabelStackDepth();
  auto params = this->testParams(0);
  this->setupECMPHelper(
      0, 2, params.nexthop, params.prefix.mask); // two labeled ports

  auto setup = [=]() {
    this->setupL3Route(
        ClientID::BGPD,
        params.prefix.network,
        params.prefix.mask,
        params.nexthop,
        LabelForwardingAction::LabelStack(
            params.stack->begin(), params.stack->begin() + maxSize - 1));
    this->setupEcmpForwarding(params.nexthop, params.prefix.mask, params.label);
    this->getProgrammedState();
  };
  auto verify = [=]() {
    for (auto i = 0; i < 2; i++) {
      LabelForwardingAction::LabelStack stack{
          params.stack->begin(), params.stack->begin() + maxSize - 1};
      stack.push_back(params.label + i);
      this->verifyLabeledMultiPathNextHopMemberWithStack(
          params.prefix, i, stack, false);
    }
  };
  this->verifyAcrossWarmBoots(setup, verify);
}

TYPED_TEST(BcmLabelEdgeRouteTest, UnresolveResolvedNextHops) {
  auto maxSize = this->getHwSwitch()->getPlatform()->maxLabelStackDepth();
  auto params = this->testParams(0);
  this->setupECMPHelper(
      0, 2, params.nexthop, params.prefix.mask); // two labeled ports

  auto setup = [=]() {
    this->setupL3Route(
        ClientID::BGPD,
        params.prefix.network,
        params.prefix.mask,
        params.nexthop,
        LabelForwardingAction::LabelStack(
            params.stack->begin(), params.stack->begin() + maxSize - 1));
    this->setupEcmpForwarding(params.nexthop, params.prefix.mask, params.label);
    this->resolveLabeledNextHops(params.nexthop, params.prefix.mask);
    this->unresolveLabeledNextHops(params.nexthop, params.prefix.mask);
    this->getProgrammedState();
  };
  auto verify = [=]() {
    for (auto i = 0; i < 2; i++) {
      LabelForwardingAction::LabelStack stack{
          params.stack->begin(), params.stack->begin() + maxSize - 1};
      stack.push_back(params.label + i);
      this->verifyLabeledMultiPathNextHopMemberWithStack(
          params.prefix, i, stack, false);
    }
  };
  this->verifyAcrossWarmBoots(setup, verify);
}

TYPED_TEST(BcmLabelEdgeRouteTest, UnresolvedHybridNextHops) {
  auto maxSize = this->getHwSwitch()->getPlatform()->maxLabelStackDepth();
  auto params = this->testParams(0);
  this->setupECMPHelper(1, 1, params.nexthop, params.prefix.mask);

  auto setup = [=]() {
    this->setupL3Route(
        ClientID::BGPD,
        params.prefix.network,
        params.prefix.mask,
        params.nexthop,
        LabelForwardingAction::LabelStack(
            params.stack->begin(), params.stack->begin() + maxSize - 1));
    this->setupEcmpForwarding(params.nexthop, params.prefix.mask, params.label);
    this->resolveLabeledNextHops(params.nexthop, params.prefix.mask);
    this->resolveUnLabeledNextHops(params.nexthop, params.prefix.mask);
    this->unresolveLabeledNextHops(params.nexthop, params.prefix.mask);
    this->unresolveUnLabeledNextHops(params.nexthop, params.prefix.mask);
    this->getProgrammedState();
  };
  auto verify = [=]() {
    for (auto i = 0; i < 2; i++) {
      if (!i) {
        LabelForwardingAction::LabelStack stack{
            params.stack->begin(), params.stack->begin() + maxSize - 1};
        this->verifyLabeledMultiPathNextHopMemberWithStack(
            params.prefix, i, stack, false);
      } else {
        LabelForwardingAction::LabelStack stack{
            params.stack->begin(), params.stack->begin() + maxSize - 1};
        stack.push_back(params.label);
        this->verifyLabeledMultiPathNextHopMemberWithStack(
            params.prefix, i, stack, false);
      }
    }
  };
  this->verifyAcrossWarmBoots(setup, verify);
}

TYPED_TEST(BcmLabelEdgeRouteTest, UnresolvedAndResolvedNextHopMultiPathGroup) {
  auto maxSize = this->getHwSwitch()->getPlatform()->maxLabelStackDepth();
  auto params = this->testParams(0);
  this->setupECMPHelper(1, 1, params.nexthop, params.prefix.mask);

  auto setup = [=]() {
    this->setupL3Route(
        ClientID::BGPD,
        params.prefix.network,
        params.prefix.mask,
        params.nexthop,
        LabelForwardingAction::LabelStack(
            params.stack->begin(), params.stack->begin() + maxSize - 1));
    this->setupEcmpForwarding(params.nexthop, params.prefix.mask, params.label);
    this->resolveUnLabeledNextHops(params.nexthop, params.prefix.mask);
    this->getProgrammedState();
  };
  auto verify = [=]() {
    for (auto i = 0; i < 2; i++) {
      if (!i) {
        LabelForwardingAction::LabelStack stack{
            params.stack->begin(), params.stack->begin() + maxSize - 1};
        // resolved
        this->verifyLabeledMultiPathNextHopMemberWithStack(
            params.prefix, i, stack, true);
      } else {
        LabelForwardingAction::LabelStack stack{
            params.stack->begin(), params.stack->begin() + maxSize - 1};
        stack.push_back(params.label);
        // unresolved
        this->verifyLabeledMultiPathNextHopMemberWithStack(
            params.prefix, i, stack, false);
      }
    }
  };
  this->verifyAcrossWarmBoots(setup, verify);
}

TYPED_TEST(BcmLabelEdgeRouteTest, UpdateRouteLabels) {
  using ParamsT = TestParameters<TypeParam>;
  std::vector<ParamsT> params{
      this->testParams(0),
      this->testParams(1),
  };
  this->setupECMPHelper(0, 2, params[0].nexthop, params[0].prefix.mask);
  this->setupECMPHelper(0, 2, params[1].nexthop, params[1].prefix.mask);

  auto maxSize = this->getHwSwitch()->getPlatform()->maxLabelStackDepth();
  auto setup = [=]() {
    for (auto i = 0; i < 2; i++) {
      this->setupL3Route(
          ClientID::BGPD,
          params[i].prefix.network,
          params[i].prefix.mask,
          params[i].nexthop,
          LabelForwardingAction::LabelStack(
              params[i].stack->begin(),
              params[i].stack->begin() + maxSize - 1));
      this->resolveLabeledNextHops(params[i].nexthop, params[i].prefix.mask);
      this->setupEcmpForwarding(
          params[i].nexthop, params[i].prefix.mask, params[i].label);
    }
    // update label stack for prefix of param 1
    this->setupL3Route(
        ClientID::BGPD,
        params[1].prefix.network,
        params[1].prefix.mask,
        params[1].nexthop,
        LabelForwardingAction::LabelStack(
            params[0].stack->begin(), params[0].stack->begin() + maxSize - 1));
    this->getProgrammedState();
  };
  auto verify = [=]() {
    for (auto i = 0; i < 2; i++) {
      std::map<PortDescriptor, LabelForwardingAction::LabelStack> stacks;
      auto j = 0;
      for (auto labeledPort : this->labeledEgressPorts()) {
        LabelForwardingAction::LabelStack stack{
            params[0].stack->begin(), params[0].stack->begin() + maxSize - 1};
        stack.push_back(params[i].label + j);
        this->verifyTunnelRefCounts(
            params[i].nexthop, params[i].prefix.mask, labeledPort, stack, 1);
        stacks.emplace(labeledPort, stack);
        j++;
      }
      this->verifyMultiPathNextHop(params[i].prefix, stacks);
    }
  };
  this->verifyAcrossWarmBoots(setup, verify);
}

TYPED_TEST(BcmLabelEdgeRouteTest, UpdatePortLabel) {
  using ParamsT = TestParameters<TypeParam>;
  std::vector<ParamsT> params{
      this->testParams(0),
      this->testParams(1),
  };
  this->setupECMPHelper(0, 2, params[0].nexthop, params[0].prefix.mask);
  this->setupECMPHelper(0, 2, params[1].nexthop, params[1].prefix.mask);

  auto maxSize = this->getHwSwitch()->getPlatform()->maxLabelStackDepth();
  auto setup = [=]() {
    for (auto i = 0; i < 2; i++) {
      this->setupL3Route(
          ClientID::BGPD,
          params[i].prefix.network,
          params[i].prefix.mask,
          params[i].nexthop,
          LabelForwardingAction::LabelStack(
              params[i].stack->begin(),
              params[i].stack->begin() + maxSize - 1));
      this->resolveLabeledNextHops(params[i].nexthop, params[i].prefix.mask);
      this->setupEcmpForwarding(
          params[i].nexthop, params[i].prefix.mask, params[i].label);
    }

    //  update tunnel label for prefix 1
    this->setupL3Route(
        ClientID::BGPD,
        params[1].prefix.network,
        params[1].prefix.mask,
        params[0].nexthop,
        LabelForwardingAction::LabelStack(
            params[1].stack->begin(), params[1].stack->begin() + maxSize - 1));
    this->getProgrammedState();
  };
  auto verify = [=]() {
    for (auto i = 0; i < 2; i++) {
      std::map<PortDescriptor, LabelForwardingAction::LabelStack> stacks;
      auto j = 0;
      for (auto labeledPort : this->labeledEgressPorts()) {
        LabelForwardingAction::LabelStack stack{
            params[i].stack->begin(), params[i].stack->begin() + maxSize - 1};
        stack.push_back(params[0].label + j);
        stacks.emplace(labeledPort, stack);
        j++;
      }
      this->verifyMultiPathNextHop(params[i].prefix, stacks);
    }
  };
  this->verifyAcrossWarmBoots(setup, verify);
}

TYPED_TEST(BcmLabelEdgeRouteTest, RecursiveStackResolution) {
  using ParamsT = TestParameters<TypeParam>;

  std::vector<ParamsT> params{
      this->testParams(0),
      this->testParams(1),
  };

  auto maxSize = this->getHwSwitch()->getPlatform()->maxLabelStackDepth();
  auto halfSize = (maxSize >>= 1); // half label stack
  this->setupECMPHelper(0, 2, params[1].nexthop, params[1].nexthop.bitCount());

  auto setup = [=]() {
    this->setupL3Route(
        ClientID::BGPD,
        params[0].prefix.network,
        params[0].prefix.mask,
        params[0].nexthop,
        LabelForwardingAction::LabelStack(
            params[0].stack->begin(), params[0].stack->begin() + halfSize - 1));
    this->setupL3Route(
        ClientID::BGPD,
        params[0].nexthop,
        params[0].nexthop.bitCount(),
        params[1].nexthop,
        LabelForwardingAction::LabelStack(
            params[0].stack->begin() + halfSize,
            params[0].stack->begin() + maxSize - 1));
    this->setupEcmpForwarding(
        params[1].nexthop, params[1].nexthop.bitCount(), params[1].label);
    this->getProgrammedState();
  };
  auto verify = [=]() {
    std::map<PortDescriptor, LabelForwardingAction::LabelStack> stacks;
    auto j = 0;
    for (auto labeledPort : this->labeledEgressPorts()) {
      LabelForwardingAction::LabelStack stack{
          params[0].stack->begin(), params[0].stack->begin() + maxSize - 1};
      stack.push_back(params[1].label + j);
      this->verifyTunnelRefCounts(
          params[1].nexthop,
          params[1].nexthop.bitCount(),
          labeledPort,
          stack,
          1);
      stacks.emplace(labeledPort, stack);
      j++;
    }
    this->verifyMultiPathNextHop(params[0].prefix, stacks);
  };
}

TYPED_TEST(BcmLabelEdgeRouteTest, TunnelRefTest) {
  using ParamsT = TestParameters<TypeParam>;

  std::vector<ParamsT> params{
      this->testParams(0),
      this->testParams(1),
  };

  auto maxSize = this->getHwSwitch()->getPlatform()->maxLabelStackDepth();
  this->setupECMPHelper(0, 2, params[0].nexthop, params[0].nexthop.bitCount());
  this->setupECMPHelper(0, 2, params[1].nexthop, params[1].nexthop.bitCount());

  auto setup = [=]() {
    for (auto i = 0; i < 2; i++) {
      LabelForwardingAction::LabelStack stack{params[i].stack->begin(),
                                              params[i].stack->begin() + 1};
      std::copy(
          params[0].stack->begin() + 1,
          params[0].stack->begin() + maxSize - 1,
          std::back_inserter(stack));

      this->setupL3Route(
          ClientID::BGPD,
          params[i].prefix.network,
          params[i].prefix.mask,
          params[i].nexthop,
          stack);

      this->resolveLabeledNextHops(
          params[i].nexthop, params[i].nexthop.bitCount());
      this->setupEcmpForwarding(
          params[i].nexthop, params[i].nexthop.bitCount(), params[0].label);
    }
    this->getProgrammedState();
  };
  auto verify = [=]() {
    for (auto i = 0; i < 1; i++) {
      std::map<PortDescriptor, LabelForwardingAction::LabelStack> stacks;
      auto j = 0;
      for (auto labeledPort : this->labeledEgressPorts()) {
        LabelForwardingAction::LabelStack stack{
            params[0].stack->begin(), params[0].stack->begin() + maxSize - 1};
        stack.push_back(params[0].label + j);

        this->verifyTunnelRefCounts(
            params[i].nexthop,
            params[i].nexthop.bitCount(),
            labeledPort,
            stack,
            2);
        stacks.emplace(labeledPort, stack);
        j++;
      }
      this->verifyMultiPathNextHop(params[i].prefix, stacks);
    }
  };
  this->verifyAcrossWarmBoots(setup, verify);
}

} // namespace facebook::fboss
