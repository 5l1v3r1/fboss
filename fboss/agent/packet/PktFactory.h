// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include "fboss/agent/TxPacket.h"
#include "fboss/agent/packet/Ethertype.h"
#include "fboss/agent/types.h"

#include "fboss/agent/packet/EthHdr.h"
#include "fboss/agent/packet/IPv4Hdr.h"
#include "fboss/agent/packet/IPv6Hdr.h"
#include "fboss/agent/packet/MPLSHdr.h"
#include "fboss/agent/packet/UDPHeader.h"

namespace facebook {
namespace fboss {

class HwSwitch;

namespace utility {
class UDPDatagram {
 public:
  // read entire udp datagram, and populate payloads, useful to parse RxPacket
  explicit UDPDatagram(folly::io::Cursor& cursor);

  // set header fields, useful to construct TxPacket
  UDPDatagram(const UDPHeader& udpHdr, std::vector<uint8_t> payload)
      : udpHdr_(udpHdr), payload_(payload) {
    udpHdr_.length = udpHdr_.size() + payload_.size();
  }

  size_t length() const {
    return UDPHeader::size() + payload_.size();
  }

  UDPHeader header() const {
    return udpHdr_;
  }

  std::vector<uint8_t> payload() const {
    return payload_;
  }

  // construct TxPacket by encapsulating rabdom byte payload
  std::unique_ptr<facebook::fboss::TxPacket> getTxPacket(
      const HwSwitch* hw) const;

  void serialize(folly::io::RWPrivateCursor& cursor) const;

  bool operator==(const UDPDatagram& that) const {
    /* ignore checksum, */
    return std::tie(
               udpHdr_.srcPort, udpHdr_.dstPort, udpHdr_.length, payload_) ==
        std::tie(
               that.udpHdr_.srcPort,
               that.udpHdr_.dstPort,
               that.udpHdr_.length,
               that.payload_);
  }

 private:
  UDPHeader udpHdr_;
  std::vector<uint8_t> payload_{};
};

template <typename AddrT>
class IPPacket {
 public:
  using HdrT = std::conditional_t<
      std::is_same<AddrT, folly::IPAddressV4>::value,
      IPv4Hdr,
      IPv6Hdr>;
  // read entire ip packet, and populate payloads, useful to parse RxPacket
  explicit IPPacket(folly::io::Cursor& cursor);

  // set header fields, useful to construct TxPacket
  explicit IPPacket(const HdrT& hdr) : hdr_{hdr} {}

  IPPacket(const HdrT& hdr, UDPDatagram payload)
      : hdr_{hdr}, udpPayLoad_(payload) {
    if constexpr (std::is_same_v<HdrT, IPv4Hdr>) {
      hdr_.version = 4;
      hdr_.length = length();
    } else {
      hdr_.version = 6;
      hdr_.payloadLength = udpPayLoad_->length();
    }
  }

  size_t length() const {
    return hdr_.size() + (udpPayLoad_ ? udpPayLoad_->length() : 0);
  }

  HdrT header() const {
    return hdr_;
  }

  folly::Optional<UDPDatagram> payload() const {
    return udpPayLoad_;
  }

  // construct TxPacket by encapsulating udp payload
  std::unique_ptr<facebook::fboss::TxPacket> getTxPacket(
      const HwSwitch* hw) const;

  void serialize(folly::io::RWPrivateCursor& cursor) const;

  bool operator==(const IPPacket<AddrT>& that) const {
    return std::tie(hdr_, udpPayLoad_) == std::tie(that.hdr_, that.udpPayLoad_);
  }

 private:
  void setUDPCheckSum(folly::IOBuf* buffer) const;
  HdrT hdr_;
  folly::Optional<UDPDatagram> udpPayLoad_;
  // TODO: support TCP segment
};

using IPv4Packet = IPPacket<folly::IPAddressV4>;
using IPv6Packet = IPPacket<folly::IPAddressV6>;

class MPLSPacket {
 public:
  // read entire label stack, and populate payloads, useful to parse RxPacket
  explicit MPLSPacket(folly::io::Cursor& cursor);

  // set header fields, useful to construct TxPacket
  explicit MPLSPacket(MPLSHdr hdr) : hdr_(std::move(hdr)) {}
  template <typename AddrT>
  MPLSPacket(MPLSHdr hdr, IPPacket<AddrT> payload) : hdr_(std::move(hdr)) {
    setPayLoad(payload);
  }

  MPLSHdr header() const {
    return hdr_;
  }

  size_t length() const {
    return hdr_.size() +
        (v4PayLoad_ ? v4PayLoad_->length()
                    : (v6PayLoad_ ? v6PayLoad_->length() : 0));
  }

  folly::Optional<IPPacket<folly::IPAddressV4>> v4PayLoad() const {
    return v4PayLoad_;
  }

  folly::Optional<IPPacket<folly::IPAddressV6>> v6PayLoad() const {
    return v6PayLoad_;
  }

  // construct TxPacket by encapsulating l3 payload
  std::unique_ptr<facebook::fboss::TxPacket> getTxPacket(
      const HwSwitch* hw) const;

  void serialize(folly::io::RWPrivateCursor& cursor) const;

  bool operator==(const MPLSPacket& that) const {
    return std::tie(hdr_, v4PayLoad_, v6PayLoad_) ==
        std::tie(that.hdr_, that.v4PayLoad_, that.v6PayLoad_);
  }

 private:
  void setPayLoad(IPPacket<folly::IPAddressV6> payload) {
    v6PayLoad_.assign(payload);
  }

  void setPayLoad(IPPacket<folly::IPAddressV4> payload) {
    v4PayLoad_.assign(payload);
  }

  MPLSHdr hdr_{MPLSHdr::Label{0, 0, 0, 0}};
  folly::Optional<IPPacket<folly::IPAddressV4>> v4PayLoad_;
  folly::Optional<IPPacket<folly::IPAddressV6>> v6PayLoad_;
};

class EthFrame {
 public:
  // read entire ethernet frame, and populate payloads, useful to parse RxPacket
  explicit EthFrame(folly::io::Cursor& cursor);

  // set header fields, useful to construct TxPacket
  explicit EthFrame(EthHdr hdr) : hdr_(std::move(hdr)) {}

  EthFrame(EthHdr hdr, MPLSPacket payload) : hdr_(std::move(hdr)) {
    mplsPayLoad_.assign(std::move(payload));
  }

  EthFrame(EthHdr hdr, IPPacket<folly::IPAddressV4> payload)
      : hdr_(std::move(hdr)), v4PayLoad_(payload) {}

  EthFrame(EthHdr hdr, IPPacket<folly::IPAddressV6> payload)
      : hdr_(std::move(hdr)), v6PayLoad_(payload) {}

  EthHdr header() const {
    return hdr_;
  }

  size_t length() const {
    auto len = 0;
    if (v4PayLoad_) {
      len += v4PayLoad_->length();
    } else if (v6PayLoad_) {
      len += v6PayLoad_->length();
    } else if (mplsPayLoad_) {
      len += mplsPayLoad_->length();
    }
    len += EthHdr::SIZE;
    return len;
  }
  // construct TxPacket by encapsulating payload
  std::unique_ptr<facebook::fboss::TxPacket> getTxPacket(
      const HwSwitch* hw) const;

  folly::Optional<IPPacket<folly::IPAddressV4>> v4PayLoad() const {
    return v4PayLoad_;
  }

  folly::Optional<IPPacket<folly::IPAddressV6>> v6PayLoad() const {
    return v6PayLoad_;
  }

  folly::Optional<MPLSPacket> mplsPayLoad() const {
    return mplsPayLoad_;
  }

  void serialize(folly::io::RWPrivateCursor& cursor) const;

  bool operator==(const EthFrame& that) const {
    return std::tie(hdr_, v4PayLoad_, v6PayLoad_, mplsPayLoad_) ==
        std::tie(
               that.hdr_, that.v4PayLoad_, that.v6PayLoad_, that.mplsPayLoad_);
  }

 private:
  EthHdr hdr_;
  folly::Optional<IPPacket<folly::IPAddressV4>> v4PayLoad_;
  folly::Optional<IPPacket<folly::IPAddressV6>> v6PayLoad_;
  folly::Optional<MPLSPacket> mplsPayLoad_;
};
} // namespace utility
} // namespace fboss
} // namespace facebook