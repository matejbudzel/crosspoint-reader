#pragma once

#include <cstdint>
#include <string>

class CrossPointNetworkManager;

enum class NetworkClaimMode {
  Foreground,
  Background,
};

enum class NetworkConnectResult {
  Connected,
  Busy,
  NoCredentials,
  Failed,
};

class NetworkSession {
 public:
  NetworkSession() = default;
  NetworkSession(const NetworkSession&) = delete;
  NetworkSession& operator=(const NetworkSession&) = delete;
  NetworkSession(NetworkSession&& other) noexcept;
  NetworkSession& operator=(NetworkSession&& other) noexcept;
  ~NetworkSession();

  bool isActive() const { return manager_ != nullptr; }
  const std::string& owner() const { return owner_; }

  NetworkConnectResult connectKnownNetwork(uint32_t timeoutPerNetworkMs = 15000);
  void disconnect();
  void release();

 private:
  friend class CrossPointNetworkManager;

  NetworkSession(CrossPointNetworkManager* manager, std::string owner);

  CrossPointNetworkManager* manager_ = nullptr;
  std::string owner_;
};

class CrossPointNetworkManager {
 public:
  static CrossPointNetworkManager& getInstance();

  NetworkSession claim(const char* owner, NetworkClaimMode mode = NetworkClaimMode::Foreground);
  bool isBusy() const;
  const std::string& currentOwner() const { return owner_; }
  std::string connectedSsid() const;

 private:
  friend class NetworkSession;

  CrossPointNetworkManager() = default;
  CrossPointNetworkManager(const CrossPointNetworkManager&) = delete;
  CrossPointNetworkManager& operator=(const CrossPointNetworkManager&) = delete;

  NetworkConnectResult connectKnownNetwork(const std::string& owner, uint32_t timeoutPerNetworkMs);
  void disconnect(const std::string& owner);
  void release(const std::string& owner);
  bool isExternallyActive() const;

  std::string owner_;
};

#define NETWORK_MANAGER CrossPointNetworkManager::getInstance()
