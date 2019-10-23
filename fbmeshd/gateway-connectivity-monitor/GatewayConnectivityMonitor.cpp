/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "GatewayConnectivityMonitor.h"

#include <time.h>
#include <chrono>

#include <folly/FileUtil.h>

#include <fbmeshd/debugfs/DebugFsWriter.h>
#include <fbmeshd/gateway-connectivity-monitor/Socket.h>

static constexpr folly::StringPiece statPathPrefixTemplate{
    "fbmeshd.gateway_connectivity_monitor.{}"};

using namespace fbmeshd;

template <typename... Params>
void writeProcFs(
    folly::StringPiece value,
    const std::string& templateS,
    Params&&... params) {
  folly::writeFile(
      value,
      folly::sformat(templateS, std::forward<Params>(params)...).c_str(),
      O_WRONLY);
}

GatewayConnectivityMonitor::GatewayConnectivityMonitor(
    folly::EventBase* evb,
    Nl80211Handler& nlHandler,
    const std::string& monitoredInterface,
    std::vector<folly::SocketAddress> monitoredAddresses,
    std::chrono::seconds monitorInterval,
    std::chrono::seconds monitorSocketTimeout,
    unsigned int penalty,
    unsigned int suppressLimit,
    unsigned int reuseLimit,
    std::chrono::seconds halfLife,
    std::chrono::seconds maxSuppressLimit,
    unsigned int robustness,
    uint8_t setRootModeIfGate,
    Routing* routing,
    StatsClient& statsClient)
    : RouteDampener{evb,
                    penalty,
                    suppressLimit,
                    reuseLimit,
                    halfLife,
                    maxSuppressLimit},
      nlHandler_{nlHandler},
      monitoredInterface_{monitoredInterface},
      monitoredAddresses_{monitoredAddresses},
      monitorSocketTimeout_{monitorSocketTimeout},
      robustness_{robustness},
      setRootModeIfGate_{setRootModeIfGate},
      routing_{routing},
      statsClient_{statsClient} {
  // Disable reverse path filtering, i.e.
  // Do not drop packets from non-routable addresses on monitored interface
  writeProcFs("0", "/proc/sys/net/ipv4/conf/{}/rp_filter", monitoredInterface);
  writeProcFs("0", "/proc/sys/net/ipv4/conf/all/rp_filter");

  // Set timer to check routes
  connectivityCheckTimer_ = folly::AsyncTimeout::make(
      *evb, [ this, monitorInterval ]() mutable noexcept {
        checkRoutesAndAdvertise();
        connectivityCheckTimer_->scheduleTimeout(monitorInterval);
      });
  connectivityCheckTimer_->scheduleTimeout(monitorInterval);
}

bool GatewayConnectivityMonitor::probeWanConnectivityRobustly() {
  VLOG(8) << folly::sformat("GatewayConnectivityMonitor::{}()", __func__);
  for (size_t tryNum{0}; tryNum < robustness_; ++tryNum) {
    if (probeWanConnectivity()) {
      return true;
    }
  }
  return false;
}

bool GatewayConnectivityMonitor::probeWanConnectivity() {
  VLOG(8) << folly::sformat("GatewayConnectivityMonitor::{}()", __func__);
  Socket::Result result;
  bool connectionSucceeded = false;
  for (const auto& monitoredAddress : monitoredAddresses_) {
    Socket socket;
    if ((result = socket.connect(
             monitoredInterface_, monitoredAddress, monitorSocketTimeout_))
            .success) {
      VLOG(8) << "Successfully connected to " << monitoredAddress;
      connectionSucceeded = true;
      break;
    } else {
      VLOG(8) << "Failed to connect to " << monitoredAddress;
    }
  }

  if (connectionSucceeded) {
    VLOG(8) << "Probing WAN connectivity succeeded";
    statsClient_.incrementSumStat(
        "fbmeshd.gateway_connectivity_monitor.probe_wan_connectivity.success");
  } else {
    VLOG(8) << "Probing WAN connectivity failed";
    // If all connection attempts failed, report failure mode of the last one
    statsClient_.incrementSumStat(folly::sformat(
        "fbmeshd.gateway_connectivity_monitor.probe_wan_connectivity.failed.{}",
        result.errorMsg));
  }
  return connectionSucceeded;
}

void GatewayConnectivityMonitor::setStat(const std::string& path, int value) {
  VLOG(8) << folly::sformat("GatewayConnectivityMonitor::{}()", __func__);
  statsClient_.setAvgStat(folly::sformat(statPathPrefixTemplate, path), value);
}

void GatewayConnectivityMonitor::dampen() {
  VLOG(8) << folly::sformat("GatewayConnectivityMonitor::{}()", __func__);
  if (isGatewayActive_) {
    DebugFsWriter::writeDebugStat("is_gateway", "false");
    withdrawDefaultRoute();
  }
}

void GatewayConnectivityMonitor::undampen() {
  VLOG(8) << folly::sformat("GatewayConnectivityMonitor::{}()", __func__);
  if (isGatewayActive_) {
    DebugFsWriter::writeDebugStat("is_gateway", "true");
    advertiseDefaultRoute();
  }
}

void GatewayConnectivityMonitor::checkRoutesAndAdvertise() {
  VLOG(8) << folly::sformat("GatewayConnectivityMonitor::{}()", __func__);
  if (probeWanConnectivityRobustly()) {
    VLOG(8) << "Successfully probed wan connectivity";
    if (!isDampened()) {
      DebugFsWriter::writeDebugStat("is_gateway", "true");
      advertiseDefaultRoute();
    } else {
      LOG(INFO) << "Default route dampened, not advertising";
    }
    if (!isGatewayActive_) {
      flap();
    }
    isGatewayActive_ = true;
  } else {
    DebugFsWriter::writeDebugStat("is_gateway", "false");
    withdrawDefaultRoute();
    isGatewayActive_ = false;
  }
}

void GatewayConnectivityMonitor::advertiseDefaultRoute() {
  VLOG(8) << folly::sformat("GatewayConnectivityMonitor::{}()", __func__);
  if (setRootModeIfGate_ != 0) {
    nlHandler_.setRootMode(setRootModeIfGate_);
  }
  if (routing_) {
    routing_->setGatewayStatus(true);
  }
}

void GatewayConnectivityMonitor::withdrawDefaultRoute() {
  VLOG(8) << folly::sformat("GatewayConnectivityMonitor::{}()", __func__);
  if (setRootModeIfGate_ != 0) {
    nlHandler_.setRootMode(0);
  }
  if (routing_) {
    routing_->setGatewayStatus(false);
  }
}
