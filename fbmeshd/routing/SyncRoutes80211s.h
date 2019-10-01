/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>

#include <fbzmq/async/ZmqEventLoop.h>

#include <fbmeshd/rnl/NetlinkSocket.h>
#include <fbmeshd/routing/Routing.h>

namespace fbmeshd {

class SyncRoutes80211s : public fbzmq::ZmqEventLoop {
 public:
  SyncRoutes80211s(
      Routing* routing,
      std::unique_ptr<rnl::NetlinkProtocolSocket> nlProtocolSocket,
      folly::MacAddress nodeAddr,
      const std::string& interface);

  // This class should never be copied; remove default copy/move
  SyncRoutes80211s() = delete;
  ~SyncRoutes80211s() = default;
  SyncRoutes80211s(const SyncRoutes80211s&) = delete;
  SyncRoutes80211s(SyncRoutes80211s&&) = delete;
  SyncRoutes80211s& operator=(const SyncRoutes80211s&) = delete;
  SyncRoutes80211s& operator=(SyncRoutes80211s&&) = delete;

 private:
  void doSyncRoutes();

  Routing* routing_;
  folly::MacAddress nodeAddr_;
  const std::string& interface_;

  std::unique_ptr<fbzmq::ZmqTimeout> syncRoutesTimer_;
  rnl::NetlinkSocket netlinkSocket_;

  folly::Optional<std::pair<folly::MacAddress, uint32_t>> currentGate_;
  bool isGateBeforeRouteSync_{false};
};

} // namespace fbmeshd
