#include "CrossPointNetworkManager.h"

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <utility>
#include <vector>

#include "WifiCredentialStore.h"

namespace {
constexpr const char* LOG_TAG = "NET";

void configureStationMode() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(100);

  String mac = WiFi.macAddress();
  mac.replace(":", "");
  const String hostname = "CrossPoint-Reader-" + mac;
  WiFi.setHostname(hostname.c_str());
}

bool waitForConnection(uint32_t timeoutMs) {
  const unsigned long startedAt = millis();
  while (millis() - startedAt < timeoutMs) {
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
      return true;
    }
    if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
      return false;
    }
    delay(100);
  }
  return false;
}
}  // namespace

NetworkSession::NetworkSession(CrossPointNetworkManager* manager, std::string owner)
    : manager_(manager), owner_(std::move(owner)) {}

NetworkSession::NetworkSession(NetworkSession&& other) noexcept
    : manager_(other.manager_), owner_(std::move(other.owner_)) {
  other.manager_ = nullptr;
}

NetworkSession& NetworkSession::operator=(NetworkSession&& other) noexcept {
  if (this != &other) {
    release();
    manager_ = other.manager_;
    owner_ = std::move(other.owner_);
    other.manager_ = nullptr;
  }
  return *this;
}

NetworkSession::~NetworkSession() { release(); }

NetworkConnectResult NetworkSession::connectKnownNetwork(uint32_t timeoutPerNetworkMs) {
  if (!manager_) return NetworkConnectResult::Busy;
  return manager_->connectKnownNetwork(owner_, timeoutPerNetworkMs);
}

void NetworkSession::disconnect() {
  if (manager_) {
    manager_->disconnect(owner_);
  }
}

void NetworkSession::release() {
  if (manager_) {
    manager_->release(owner_);
    manager_ = nullptr;
  }
}

CrossPointNetworkManager& CrossPointNetworkManager::getInstance() {
  static CrossPointNetworkManager instance;
  return instance;
}

NetworkSession CrossPointNetworkManager::claim(const char* owner, NetworkClaimMode mode) {
  const char* requestedOwner = owner ? owner : "";
  if (owner_.empty() && isExternallyActive()) {
    LOG_INF(LOG_TAG, "Network busy outside manager; %s cannot claim", requestedOwner);
    return {};
  }

  if (!owner_.empty()) {
    if (mode == NetworkClaimMode::Background) {
      LOG_INF(LOG_TAG, "Network busy by %s; %s will defer", owner_.c_str(), requestedOwner);
    } else {
      LOG_INF(LOG_TAG, "Network busy by %s; %s cannot claim", owner_.c_str(), requestedOwner);
    }
    return {};
  }

  owner_ = requestedOwner;
  LOG_DBG(LOG_TAG, "Claimed by %s", owner_.c_str());
  return NetworkSession(this, owner_);
}

bool CrossPointNetworkManager::isBusy() const { return !owner_.empty() || isExternallyActive(); }

std::string CrossPointNetworkManager::connectedSsid() const {
  if (WiFi.status() != WL_CONNECTED) {
    return "";
  }
  return WiFi.SSID().c_str();
}

NetworkConnectResult CrossPointNetworkManager::connectKnownNetwork(const std::string& owner,
                                                                   uint32_t timeoutPerNetworkMs) {
  if (owner.empty() || owner != owner_) {
    return NetworkConnectResult::Busy;
  }

  WIFI_STORE.loadFromFile();
  const auto& credentials = WIFI_STORE.getCredentials();
  if (credentials.empty()) {
    LOG_INF(LOG_TAG, "No saved WiFi credentials");
    return NetworkConnectResult::NoCredentials;
  }

  std::vector<const WifiCredential*> candidates;
  candidates.reserve(credentials.size());

  const std::string& lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (!lastSsid.empty()) {
    const WifiCredential* last = WIFI_STORE.findCredential(lastSsid);
    if (last) {
      candidates.push_back(last);
    }
  }

  for (const auto& credential : credentials) {
    const bool alreadyAdded =
        std::any_of(candidates.begin(), candidates.end(),
                    [&credential](const WifiCredential* candidate) { return candidate->ssid == credential.ssid; });
    if (!alreadyAdded) {
      candidates.push_back(&credential);
    }
  }

  for (const WifiCredential* credential : candidates) {
    if (!credential || credential->ssid.empty()) {
      continue;
    }

    LOG_INF(LOG_TAG, "Connecting to saved WiFi: %s", credential->ssid.c_str());
    configureStationMode();
    if (!credential->password.empty()) {
      WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
    } else {
      WiFi.begin(credential->ssid.c_str());
    }

    if (waitForConnection(timeoutPerNetworkMs)) {
      WIFI_STORE.setLastConnectedSsid(credential->ssid);
      LOG_INF(LOG_TAG, "Connected to %s (%s)", credential->ssid.c_str(), WiFi.localIP().toString().c_str());
      return NetworkConnectResult::Connected;
    }

    LOG_INF(LOG_TAG, "Failed to connect to %s", credential->ssid.c_str());
    WiFi.disconnect(false);
    delay(100);
  }

  return NetworkConnectResult::Failed;
}

void CrossPointNetworkManager::disconnect(const std::string& owner) {
  if (owner.empty() || owner != owner_) {
    return;
  }

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    LOG_DBG(LOG_TAG, "Disconnecting WiFi for %s", owner.c_str());
    WiFi.disconnect(false);
    delay(30);
    WiFi.mode(WIFI_OFF);
  }
}

void CrossPointNetworkManager::release(const std::string& owner) {
  if (owner.empty() || owner != owner_) {
    return;
  }
  LOG_DBG(LOG_TAG, "Released by %s", owner.c_str());
  owner_.clear();
}

bool CrossPointNetworkManager::isExternallyActive() const {
  if (WiFi.getMode() == WIFI_MODE_NULL) {
    return false;
  }
  if (!owner_.empty()) {
    return false;
  }
  return true;
}
