#include "limelight_ec/mini_json.hpp"

#include <soem/soem.h>

#if LIMELIGHT_EC_WITH_WPILIB
#include <networktables/BooleanTopic.h>
#include <networktables/DoubleTopic.h>
#include <networktables/NetworkTableInstance.h>
#include <networktables/StringArrayTopic.h>
#include <networktables/StringTopic.h>
#include <wpi/DataLogBackgroundWriter.h>
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace limelight_ec {
namespace {

constexpr const char* kConfigPath = "/etc/ec-configuration/ec-configuration.json";
constexpr const char* kSocketPath = "/var/run/ethercat_maindevice.sock";
constexpr const char* kLogDirectory = "/var/log/limelight-ethercat";
constexpr uint32_t kIpcMagic = 0x4C4C4543U;
constexpr uint16_t kIpcVersion = 1;
constexpr size_t kIpcMaxPayloadBytes = 8192;
constexpr int64_t kDefaultCyclePeriodUs = 5000;
constexpr int kSubDeviceMonitorTimeoutUs = 500;

std::atomic_bool g_running{true};

enum class MainDeviceState {
  Starting,
  WaitingForLink,
  Initializing,
  SafeOperational,
  Operational,
  Error,
  Stopping
};

std::string ToString(MainDeviceState state) {
  switch (state) {
    case MainDeviceState::Starting:
      return "Starting";
    case MainDeviceState::WaitingForLink:
      return "WaitingForLink";
    case MainDeviceState::Initializing:
      return "Initializing";
    case MainDeviceState::SafeOperational:
      return "SafeOperational";
    case MainDeviceState::Operational:
      return "OP";
    case MainDeviceState::Error:
      return "Error";
    case MainDeviceState::Stopping:
      return "Stopping";
  }
  return "Unknown";
}

struct InterfaceConfig {
  std::string logicalName;
  std::string physicalInterface;
  bool enabled = true;
};

struct DaemonConfig {
  std::vector<InterfaceConfig> interfaces;
  bool allowRestrictedInterfaces = false;
  int64_t cyclePeriodUs = kDefaultCyclePeriodUs;
  std::string nt4Server;
  int nt4Team = 0;
  std::string logDirectory = kLogDirectory;
};

struct SubDeviceSnapshot {
  std::string logicalInterface;
  int index = 0;
  std::string name;
  int state = 0;
  int statusCode = 0;
};

struct RuntimeSnapshot {
  MainDeviceState aggregateState = MainDeviceState::Starting;
  std::vector<std::string> activeLogicalInterfaces;
  std::vector<SubDeviceSnapshot> subDevices;
  double cpuUtilization = 0.0;
  double maxCycleJitterUs = 0.0;
  bool preemptRtAvailable = false;
};

uint64_t MonotonicMicros() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
         static_cast<uint64_t>(ts.tv_nsec / 1000L);
}

std::string TimestampForFile() {
  time_t now = time(nullptr);
  tm tmValue{};
  localtime_r(&now, &tmValue);
  std::ostringstream stream;
  stream << std::put_time(&tmValue, "%Y%m%d-%H%M%S");
  return stream.str();
}

void AddMicros(timespec* ts, int64_t micros) {
  ts->tv_nsec += static_cast<long>((micros % 1000000) * 1000);
  ts->tv_sec += static_cast<time_t>(micros / 1000000);
  while (ts->tv_nsec >= 1000000000L) {
    ts->tv_nsec -= 1000000000L;
    ++ts->tv_sec;
  }
}

std::string ReadFileToString(const std::string& path) {
  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("unable to open " + path);
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

bool IsRestrictedInterface(const std::string& physicalInterface) {
  return physicalInterface == "eth0" || physicalInterface == "wlan0" ||
         physicalInterface == "usb0";
}

bool IsLinkActive(const std::string& physicalInterface) {
  struct ifaddrs* addresses = nullptr;
  if (getifaddrs(&addresses) != 0) {
    return false;
  }

  bool active = false;
  for (const ifaddrs* cursor = addresses; cursor != nullptr;
       cursor = cursor->ifa_next) {
    if (cursor->ifa_name == nullptr || physicalInterface != cursor->ifa_name) {
      continue;
    }
    const unsigned int flags = cursor->ifa_flags;
    active = (flags & IFF_UP) != 0U && (flags & IFF_RUNNING) != 0U;
    break;
  }
  freeifaddrs(addresses);
  return active;
}

bool DetectPreemptRt() {
  std::ifstream realtime("/sys/kernel/realtime");
  std::string value;
  if (realtime && std::getline(realtime, value)) {
    if (value == "1") {
      return true;
    }
  }

  std::ifstream version("/proc/version");
  std::string text;
  if (version && std::getline(version, text)) {
    return text.find("PREEMPT_RT") != std::string::npos ||
           text.find("PREEMPT RT") != std::string::npos;
  }
  return false;
}

void ApplyPreemptRtOptimizationsIfAvailable(bool available) {
  if (!available) {
    return;
  }

  mlockall(MCL_CURRENT | MCL_FUTURE);

  sched_param param{};
  param.sched_priority = 55;
  pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  CPU_SET(3, &cpus);
  pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
}

DaemonConfig LoadConfig(const std::string& path) {
  Json root = Json::Parse(ReadFileToString(path));
  if (!root.isObject()) {
    throw std::runtime_error("configuration root must be a JSON object");
  }

  DaemonConfig config;
  config.allowRestrictedInterfaces =
      root.boolValue("allow_restricted_interfaces", false);
  config.cyclePeriodUs =
      std::max<int64_t>(1000, root.integerValue("cycle_period_us",
                                               kDefaultCyclePeriodUs));
  config.nt4Server = root.stringValue("nt4_server", "");
  config.nt4Team = static_cast<int>(root.integerValue("nt4_team", 0));
  config.logDirectory = root.stringValue("log_directory", kLogDirectory);

  const Json* interfaces = root.find("interfaces");
  if (interfaces == nullptr || !interfaces->isArray()) {
    throw std::runtime_error("configuration must contain an interfaces array");
  }

  std::set<std::string> logicalNames;
  std::set<std::string> physicalNames;
  for (const Json& item : interfaces->array()) {
    if (!item.isObject()) {
      throw std::runtime_error("each interface entry must be a JSON object");
    }

    InterfaceConfig interfaceConfig;
    interfaceConfig.logicalName = item.stringValue("logical_name");
    interfaceConfig.physicalInterface = item.stringValue("physical_interface");
    interfaceConfig.enabled = item.boolValue("enabled", true);

    if (interfaceConfig.logicalName.empty() ||
        interfaceConfig.physicalInterface.empty()) {
      throw std::runtime_error(
          "interface entries require logical_name and physical_interface");
    }
    if (!config.allowRestrictedInterfaces &&
        IsRestrictedInterface(interfaceConfig.physicalInterface)) {
      throw std::runtime_error("restricted interface " +
                               interfaceConfig.physicalInterface +
                               " requires allow_restricted_interfaces");
    }
    if (!logicalNames.insert(interfaceConfig.logicalName).second) {
      throw std::runtime_error("duplicate logical interface name " +
                               interfaceConfig.logicalName);
    }
    if (!physicalNames.insert(interfaceConfig.physicalInterface).second) {
      throw std::runtime_error("duplicate physical interface " +
                               interfaceConfig.physicalInterface);
    }
    if (interfaceConfig.enabled) {
      config.interfaces.emplace_back(std::move(interfaceConfig));
    }
  }

  if (config.interfaces.empty()) {
    throw std::runtime_error("no enabled interfaces configured");
  }
  return config;
}

class EventLog final {
 public:
  explicit EventLog(std::string directory) : directory_(std::move(directory)) {
    mkdir(directory_.c_str(), 0755);
#if LIMELIGHT_EC_WITH_WPILIB
    dataLog_ = std::make_unique<wpi::log::DataLogBackgroundWriter>(
        directory_, "ethercat-maindevice-" + TimestampForFile() + ".wpilog",
        0.25, "Limelight Systemcore EtherCAT MainDevice");
    eventEntry_ = dataLog_->Start("/EtherCAT/Events", "string");
    stateEntry_ = dataLog_->Start("/EtherCAT/MainDeviceState", "string");
    jitterEntry_ = dataLog_->Start("/EtherCAT/CycleJitterUs", "double");
#else
    fallback_.open(directory_ + "/ethercat-maindevice-" + TimestampForFile() +
                       ".log",
                   std::ios::app);
#endif
  }

  void Event(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::cerr << message << '\n';
#if LIMELIGHT_EC_WITH_WPILIB
    dataLog_->AppendString(eventEntry_, message, 0);
#else
    if (fallback_) {
      fallback_ << message << '\n';
    }
#endif
  }

  void State(const std::string& state) {
    std::lock_guard<std::mutex> lock(mutex_);
#if LIMELIGHT_EC_WITH_WPILIB
    dataLog_->AppendString(stateEntry_, state, 0);
#else
    if (fallback_) {
      fallback_ << "state=" << state << '\n';
    }
#endif
  }

  void Jitter(double jitterUs) {
    std::lock_guard<std::mutex> lock(mutex_);
#if LIMELIGHT_EC_WITH_WPILIB
    dataLog_->AppendDouble(jitterEntry_, jitterUs, 0);
#else
    if (fallback_) {
      fallback_ << "cycle_jitter_us=" << jitterUs << '\n';
    }
#endif
  }

 private:
  std::string directory_;
  std::mutex mutex_;
#if LIMELIGHT_EC_WITH_WPILIB
  std::unique_ptr<wpi::log::DataLogBackgroundWriter> dataLog_;
  int eventEntry_ = 0;
  int stateEntry_ = 0;
  int jitterEntry_ = 0;
#else
  std::ofstream fallback_;
#endif
};

class Telemetry final {
 public:
  Telemetry(const DaemonConfig& config, EventLog& log)
      : config_(config), log_(log) {
#if LIMELIGHT_EC_WITH_WPILIB
    instance_ = nt::NetworkTableInstance::GetDefault();
    if (!config_.nt4Server.empty()) {
      instance_.StartClient4("limelight-ethercat-maindevice");
      instance_.SetServer(config_.nt4Server.c_str());
    } else if (config_.nt4Team > 0) {
      instance_.StartClient4("limelight-ethercat-maindevice");
      instance_.SetServerTeam(config_.nt4Team);
    } else {
      instance_.StartServer();
    }

    auto table = instance_.GetTable("EtherCAT");
    statePub_ = table->GetStringTopic("MainDeviceState").Publish();
    activePub_ =
        table->GetStringArrayTopic("ActiveLogicalInterfaces").Publish();
    cpuPub_ = table->GetDoubleTopic("CpuUtilization").Publish();
    jitterPub_ = table->GetDoubleTopic("CycleJitterUs").Publish();
    preemptRtPub_ = table->GetBooleanTopic("PreemptRtAvailable").Publish();
    subDeviceNamesPub_ =
        table->GetStringArrayTopic("SubDevices/Names").Publish();
    subDeviceStatesPub_ =
        table->GetStringArrayTopic("SubDevices/States").Publish();
    subDeviceIndicesPub_ =
        table->GetStringArrayTopic("SubDevices/Indices").Publish();
    subDeviceStatusCodesPub_ =
        table->GetStringArrayTopic("SubDevices/StatusCodes").Publish();
    subDeviceInterfacesPub_ =
        table->GetStringArrayTopic("SubDevices/LogicalInterfaces").Publish();
#endif
  }

  ~Telemetry() { Stop(); }

  void Start() {
    worker_ = std::thread([this] { Run(); });
  }

  void Stop() {
    running_.store(false);
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void PublishSnapshot(RuntimeSnapshot snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = std::move(snapshot);
  }

 private:
  void Run() {
    uint64_t previousWallUs = MonotonicMicros();
    uint64_t previousCpuUs = ThreadCpuMicros();

    while (running_.load() && g_running.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));

      RuntimeSnapshot snapshot;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = snapshot_;
      }

      const uint64_t nowWallUs = MonotonicMicros();
      const uint64_t nowCpuUs = ThreadCpuMicros();
      if (nowWallUs > previousWallUs) {
        snapshot.cpuUtilization =
            100.0 * static_cast<double>(nowCpuUs - previousCpuUs) /
            static_cast<double>(nowWallUs - previousWallUs);
      }
      previousWallUs = nowWallUs;
      previousCpuUs = nowCpuUs;

#if LIMELIGHT_EC_WITH_WPILIB
      statePub_.Set(ToString(snapshot.aggregateState));
      activePub_.Set(snapshot.activeLogicalInterfaces);
      cpuPub_.Set(snapshot.cpuUtilization);
      jitterPub_.Set(snapshot.maxCycleJitterUs);
      preemptRtPub_.Set(snapshot.preemptRtAvailable);

      std::vector<std::string> names;
      std::vector<std::string> states;
      std::vector<std::string> indices;
      std::vector<std::string> statusCodes;
      std::vector<std::string> logicalInterfaces;
      for (const auto& item : snapshot.subDevices) {
        names.emplace_back(item.name.empty() ? "SubDevice" +
                                                   std::to_string(item.index)
                                             : item.name);
        indices.emplace_back(std::to_string(item.index));
        states.emplace_back("0x" + Hex16(item.state));
        statusCodes.emplace_back("0x" + Hex16(item.statusCode));
        logicalInterfaces.emplace_back(item.logicalInterface);
      }
      subDeviceNamesPub_.Set(names);
      subDeviceStatesPub_.Set(states);
      subDeviceIndicesPub_.Set(indices);
      subDeviceStatusCodesPub_.Set(statusCodes);
      subDeviceInterfacesPub_.Set(logicalInterfaces);
#endif

      if (lastState_ != snapshot.aggregateState) {
        lastState_ = snapshot.aggregateState;
        log_.State(ToString(lastState_));
      }
      log_.Jitter(snapshot.maxCycleJitterUs);
    }
  }

  static uint64_t ThreadCpuMicros() {
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
           static_cast<uint64_t>(ts.tv_nsec / 1000L);
  }

  static std::string Hex16(int value) {
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setw(4) << std::setfill('0')
           << (value & 0xFFFF);
    return stream.str();
  }

  const DaemonConfig& config_;
  EventLog& log_;
  std::atomic_bool running_{true};
  std::thread worker_;
  std::mutex mutex_;
  RuntimeSnapshot snapshot_;
  MainDeviceState lastState_ = MainDeviceState::Starting;
#if LIMELIGHT_EC_WITH_WPILIB
  nt::NetworkTableInstance instance_;
  nt::StringPublisher statePub_;
  nt::StringArrayPublisher activePub_;
  nt::DoublePublisher cpuPub_;
  nt::DoublePublisher jitterPub_;
  nt::BooleanPublisher preemptRtPub_;
  nt::StringArrayPublisher subDeviceNamesPub_;
  nt::StringArrayPublisher subDeviceStatesPub_;
  nt::StringArrayPublisher subDeviceIndicesPub_;
  nt::StringArrayPublisher subDeviceStatusCodesPub_;
  nt::StringArrayPublisher subDeviceInterfacesPub_;
#endif
};

#pragma pack(push, 1)
struct IpcPacketHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t type;
  uint32_t sequence;
  uint16_t logicalNameBytes;
  uint16_t reserved;
  uint32_t payloadBytes;
};
#pragma pack(pop)

struct IpcPacket {
  uint16_t type = 0;
  uint32_t sequence = 0;
  std::string logicalName;
  std::vector<uint8_t> payload;
};

class IpcServer final {
 public:
  explicit IpcServer(EventLog& log) : log_(log) {}

  ~IpcServer() { Stop(); }

  void Start() {
    SetupSocket();
    worker_ = std::thread([this] { Run(); });
  }

  void Stop() {
    running_.store(false);
    if (listenFd_ >= 0) {
      close(listenFd_);
      listenFd_ = -1;
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    for (int fd : clients_) {
      close(fd);
    }
    clients_.clear();
    unlink(kSocketPath);
  }

  std::vector<IpcPacket> DrainPackets() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<IpcPacket> packets;
    while (!packets_.empty()) {
      packets.emplace_back(std::move(packets_.front()));
      packets_.pop_front();
    }
    return packets;
  }

 private:
  void Run() {
    while (running_.load() && g_running.load()) {
      pollfd fds[65]{};
      fds[0].fd = listenFd_;
      fds[0].events = POLLIN;
      size_t count = 1;
      for (int fd : clients_) {
        fds[count].fd = fd;
        fds[count].events = POLLIN | POLLHUP | POLLERR;
        ++count;
      }

      const int pollResult = poll(fds, static_cast<nfds_t>(count), 100);
      if (pollResult <= 0) {
        continue;
      }

      if ((fds[0].revents & POLLIN) != 0) {
        AcceptClients();
      }

      std::vector<int> closed;
      for (size_t i = 1; i < count; ++i) {
        if ((fds[i].revents & (POLLHUP | POLLERR)) != 0) {
          closed.emplace_back(fds[i].fd);
          continue;
        }
        if ((fds[i].revents & POLLIN) != 0) {
          if (!ReadPacket(fds[i].fd)) {
            closed.emplace_back(fds[i].fd);
          }
        }
      }

      for (int fd : closed) {
        close(fd);
        clients_.erase(std::remove(clients_.begin(), clients_.end(), fd),
                       clients_.end());
      }
    }
  }

  void SetupSocket() {
    unlink(kSocketPath);
    listenFd_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC,
                       0);
    if (listenFd_ < 0) {
      throw std::runtime_error("unable to create IPC socket");
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);

    if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      throw std::runtime_error("unable to bind IPC socket");
    }
    chmod(kSocketPath, 0660);
    if (listen(listenFd_, 16) < 0) {
      throw std::runtime_error("unable to listen on IPC socket");
    }
    log_.Event(std::string("IPC socket active at ") + kSocketPath);
  }

  void AcceptClients() {
    while (clients_.size() < 64) {
      const int fd = accept4(listenFd_, nullptr, nullptr,
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          log_.Event("IPC accept failed");
        }
        break;
      }
      clients_.emplace_back(fd);
    }
  }

  bool ReadPacket(int fd) {
    std::array<uint8_t, sizeof(IpcPacketHeader) + kIpcMaxPayloadBytes> buffer{};
    const ssize_t received = recv(fd, buffer.data(), buffer.size(), 0);
    if (received == 0) {
      return false;
    }
    if (received < 0) {
      return errno == EAGAIN || errno == EWOULDBLOCK;
    }
    if (static_cast<size_t>(received) < sizeof(IpcPacketHeader)) {
      return true;
    }

    IpcPacketHeader header{};
    memcpy(&header, buffer.data(), sizeof(header));
    if (header.magic != kIpcMagic || header.version != kIpcVersion ||
        header.payloadBytes > kIpcMaxPayloadBytes) {
      log_.Event("IPC packet rejected");
      return true;
    }

    const size_t required = sizeof(IpcPacketHeader) + header.logicalNameBytes +
                            header.payloadBytes;
    if (required > static_cast<size_t>(received) || required > buffer.size()) {
      log_.Event("IPC packet length mismatch");
      return true;
    }

    IpcPacket packet;
    packet.type = header.type;
    packet.sequence = header.sequence;
    const auto* data = buffer.data() + sizeof(IpcPacketHeader);
    packet.logicalName.assign(reinterpret_cast<const char*>(data),
                              header.logicalNameBytes);
    packet.payload.assign(data + header.logicalNameBytes,
                          data + header.logicalNameBytes +
                              header.payloadBytes);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      packets_.emplace_back(std::move(packet));
      while (packets_.size() > 256) {
        packets_.pop_front();
      }
    }
    return true;
  }

  EventLog& log_;
  std::atomic_bool running_{true};
  std::thread worker_;
  int listenFd_ = -1;
  std::vector<int> clients_;
  std::mutex mutex_;
  std::deque<IpcPacket> packets_;
};

class NetlinkMonitor final {
 public:
  NetlinkMonitor(std::vector<InterfaceConfig> interfaces, EventLog& log)
      : interfaces_(std::move(interfaces)), log_(log) {
    for (const auto& item : interfaces_) {
      linkActive_[item.logicalName] = IsLinkActive(item.physicalInterface);
    }
  }

  ~NetlinkMonitor() { Stop(); }

  void Start() {
    OpenSocket();
    worker_ = std::thread([this] { Run(); });
  }

  void Stop() {
    running_.store(false);
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  bool LinkActive(const std::string& logicalName) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = linkActive_.find(logicalName);
    return it != linkActive_.end() && it->second;
  }

 private:
  void Run() {
    while (running_.load() && g_running.load()) {
      pollfd pfd{};
      pfd.fd = fd_;
      pfd.events = POLLIN;
      const int result = poll(&pfd, 1, 500);
      if (result > 0 && (pfd.revents & POLLIN) != 0) {
        DrainMessages();
      }
      RefreshAll();
    }
  }

  void OpenSocket() {
    fd_ = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC,
                 NETLINK_ROUTE);
    if (fd_ < 0) {
      throw std::runtime_error("unable to create rtnetlink socket");
    }

    sockaddr_nl addr{};
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = RTMGRP_LINK;
    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      throw std::runtime_error("unable to bind rtnetlink socket");
    }
  }

  void DrainMessages() {
    std::array<char, 8192> buffer{};
    while (true) {
      const ssize_t length = recv(fd_, buffer.data(), buffer.size(), 0);
      if (length < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          log_.Event("rtnetlink receive failed");
        }
        break;
      }
      int remaining = static_cast<int>(length);
      for (nlmsghdr* header = reinterpret_cast<nlmsghdr*>(buffer.data());
           NLMSG_OK(header, remaining);
           header = NLMSG_NEXT(header, remaining)) {
        if (header->nlmsg_type == RTM_NEWLINK ||
            header->nlmsg_type == RTM_DELLINK) {
          RefreshAll();
        }
      }
    }
  }

  void RefreshAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& item : interfaces_) {
      const bool current = IsLinkActive(item.physicalInterface);
      auto it = linkActive_.find(item.logicalName);
      if (it == linkActive_.end() || it->second != current) {
        linkActive_[item.logicalName] = current;
        log_.Event("Link " + std::string(current ? "restored" : "dropped") +
                   " for " + item.logicalName);
      }
    }
  }

  std::vector<InterfaceConfig> interfaces_;
  EventLog& log_;
  std::atomic_bool running_{true};
  std::thread worker_;
  int fd_ = -1;
  mutable std::mutex mutex_;
  std::map<std::string, bool> linkActive_;
};

class EthercatBus final {
 public:
  EthercatBus(InterfaceConfig config, EventLog& log)
      : config_(std::move(config)), log_(log) {
    memset(&context_, 0, sizeof(context_));
  }

  ~EthercatBus() { Shutdown(); }

  const std::string& LogicalName() const { return config_.logicalName; }
  MainDeviceState State() const { return state_; }
  bool Operational() const { return state_ == MainDeviceState::Operational; }

  void Step(bool linkActive, const std::vector<IpcPacket>& packets) {
    if (!linkActive) {
      if (socketOpen_) {
        log_.Event("Transitioning " + config_.logicalName +
                   " to Error because the physical link is unavailable");
        Shutdown();
      }
      state_ = MainDeviceState::Error;
      return;
    }

    if (!socketOpen_ || state_ == MainDeviceState::Error ||
        state_ == MainDeviceState::WaitingForLink) {
      const uint64_t nowUs = MonotonicMicros();
      if (nowUs < nextInitializeAttemptUs_) {
        return;
      }
      nextInitializeAttemptUs_ = nowUs + 1000000ULL;
      Initialize();
      if (state_ == MainDeviceState::Operational) {
        nextInitializeAttemptUs_ = 0;
      }
    }

    if (state_ == MainDeviceState::Operational) {
      ApplyPackets(packets);
      ExchangeProcessData();
    }
  }

  std::vector<SubDeviceSnapshot> Snapshot() const {
    std::vector<SubDeviceSnapshot> result;
    if (!socketOpen_) {
      return result;
    }

    for (int index = 1; index <= context_.slavecount; ++index) {
      const ec_slavet& item = context_.slavelist[index];
      SubDeviceSnapshot snapshot;
      snapshot.logicalInterface = config_.logicalName;
      snapshot.index = index;
      snapshot.name = item.name;
      snapshot.state = item.state;
      snapshot.statusCode = item.ALstatuscode;
      result.emplace_back(std::move(snapshot));
    }
    return result;
  }

 private:
  void Initialize() {
    Shutdown();
    state_ = MainDeviceState::Initializing;
    log_.Event("Initializing " + config_.logicalName + " on " +
               config_.physicalInterface);

    memset(&context_, 0, sizeof(context_));
    workingCounterMisses_ = 0;
    expectedWorkingCounter_ = 0;
    if (!ecx_init(&context_, const_cast<char*>(config_.physicalInterface.c_str()))) {
      state_ = MainDeviceState::Error;
      log_.Event("SOEM socket open failed for " + config_.logicalName);
      return;
    }
    socketOpen_ = true;

    if (ecx_config_init(&context_) <= 0) {
      state_ = MainDeviceState::Error;
      log_.Event("No SubDevices found for " + config_.logicalName);
      ShutdownSocketOnly();
      return;
    }

    ec_groupt* group = &context_.grouplist[groupIndex_];
    ecx_config_map_group(&context_, ioMap_.data(), groupIndex_);
    expectedWorkingCounter_ = (group->outputsWKC * 2) + group->inputsWKC;
    ecx_configdc(&context_);
    state_ = MainDeviceState::SafeOperational;

    ecx_statecheck(&context_, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);
    RoundTrip();

    context_.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&context_, 0);
    for (int attempt = 0; attempt < 10; ++attempt) {
      RoundTrip();
      ecx_statecheck(&context_, 0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE / 10);
      if (context_.slavelist[0].state == EC_STATE_OPERATIONAL) {
        state_ = MainDeviceState::Operational;
        log_.Event(config_.logicalName + " transitioned to OP with " +
                   std::to_string(context_.slavecount) + " SubDevices");
        return;
      }
    }

    ecx_readstate(&context_);
    for (int index = 1; index <= context_.slavecount; ++index) {
      const ec_slavet& item = context_.slavelist[index];
      if (item.state != EC_STATE_OPERATIONAL) {
        log_.Event(config_.logicalName + " SubDevice " +
                   std::to_string(index) + " state=0x" +
                   Hex16(item.state) + " status=0x" +
                   Hex16(item.ALstatuscode));
      }
    }
    state_ = MainDeviceState::Error;
  }

  void ExchangeProcessData() {
    const int workingCounter = ecx_receive_processdata(&context_, EC_TIMEOUTRET);
    if (workingCounter < expectedWorkingCounter_) {
      ++workingCounterMisses_;
      if (workingCounterMisses_ > 2) {
        MonitorSubDevices();
      }
    } else {
      workingCounterMisses_ = 0;
    }
    ecx_mbxhandler(&context_, 0, 4);
    ecx_send_processdata(&context_);
  }

  void MonitorSubDevices() {
    ec_groupt* group = &context_.grouplist[groupIndex_];
    group->docheckstate = FALSE;
    ecx_readstate(&context_);

    bool repaired = true;
    for (int index = 1; index <= context_.slavecount; ++index) {
      ec_slavet* item = &context_.slavelist[index];
      if (item->group != groupIndex_ ||
          item->state == EC_STATE_OPERATIONAL) {
        continue;
      }

      repaired = false;
      group->docheckstate = TRUE;
      if (item->state == (EC_STATE_SAFE_OP + EC_STATE_ERROR)) {
        item->state = (EC_STATE_SAFE_OP + EC_STATE_ACK);
        ecx_writestate(&context_, index);
      } else if (item->state == EC_STATE_SAFE_OP) {
        item->state = EC_STATE_OPERATIONAL;
        if (item->mbxhandlerstate == ECT_MBXH_LOST) {
          item->mbxhandlerstate = ECT_MBXH_CYCLIC;
        }
        ecx_writestate(&context_, index);
      } else if (item->state > EC_STATE_NONE) {
        if (ecx_reconfig_slave(&context_, index, kSubDeviceMonitorTimeoutUs) >=
            EC_STATE_PRE_OP) {
          item->islost = FALSE;
        }
      } else if (!item->islost) {
        ecx_statecheck(&context_, index, EC_STATE_OPERATIONAL, EC_TIMEOUTRET);
        if (item->state == EC_STATE_NONE) {
          item->islost = TRUE;
          item->mbxhandlerstate = ECT_MBXH_LOST;
          if (item->Ibytes != 0) {
            memset(item->inputs, 0x00, item->Ibytes);
          }
        }
      }

      if (item->islost) {
        if (item->state <= EC_STATE_INIT) {
          if (ecx_recover_slave(&context_, index, kSubDeviceMonitorTimeoutUs)) {
            item->islost = FALSE;
          }
        } else {
          item->islost = FALSE;
        }
      }
    }

    if (repaired || !group->docheckstate) {
      log_.Event(config_.logicalName + " SubDevices are responding in OP");
      workingCounterMisses_ = 0;
    }
  }

  void ApplyPackets(const std::vector<IpcPacket>& packets) {
    ec_groupt* group = &context_.grouplist[groupIndex_];
    if (group->outputs == nullptr || group->Obytes == 0) {
      return;
    }

    for (const auto& packet : packets) {
      if (packet.logicalName != config_.logicalName || packet.type != 1) {
        continue;
      }
      const size_t bytes =
          std::min(packet.payload.size(), static_cast<size_t>(group->Obytes));
      memcpy(group->outputs, packet.payload.data(), bytes);
    }
  }

  int RoundTrip() {
    ecx_send_processdata(&context_);
    return ecx_receive_processdata(&context_, EC_TIMEOUTRET);
  }

  void Shutdown() {
    if (!socketOpen_) {
      return;
    }
    state_ = MainDeviceState::Stopping;
    context_.slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(&context_, 0);
    ShutdownSocketOnly();
  }

  void ShutdownSocketOnly() {
    if (socketOpen_) {
      ecx_close(&context_);
      socketOpen_ = false;
    }
  }

  static std::string Hex16(int value) {
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setw(4) << std::setfill('0')
           << (value & 0xFFFF);
    return stream.str();
  }

  InterfaceConfig config_;
  EventLog& log_;
  ecx_contextt context_{};
  std::array<uint8_t, 4096> ioMap_{};
  uint8_t groupIndex_ = 0;
  int expectedWorkingCounter_ = 0;
  int workingCounterMisses_ = 0;
  bool socketOpen_ = false;
  MainDeviceState state_ = MainDeviceState::Starting;
  uint64_t nextInitializeAttemptUs_ = 0;
};

class EthercatMainDeviceDaemon final {
 public:
  EthercatMainDeviceDaemon(DaemonConfig config, EventLog& log)
      : config_(std::move(config)),
        log_(log),
        telemetry_(config_, log_),
        ipc_(log_),
        netlink_(config_.interfaces, log_) {
    preemptRtAvailable_ = DetectPreemptRt();
    for (const auto& item : config_.interfaces) {
      buses_.emplace_back(std::make_unique<EthercatBus>(item, log_));
    }
  }

  void Run() {
    telemetry_.Start();
    ipc_.Start();
    netlink_.Start();

    worker_ = std::thread([this] { CycleLoop(); });
    worker_.join();

    netlink_.Stop();
    ipc_.Stop();
    telemetry_.Stop();
  }

 private:
  void CycleLoop() {
    ApplyPreemptRtOptimizationsIfAvailable(preemptRtAvailable_);

    timespec next{};
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (g_running.load()) {
      AddMicros(&next, config_.cyclePeriodUs);
      const int sleepResult =
          clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
      if (sleepResult != 0 && sleepResult != EINTR) {
        log_.Event("cycle sleep returned error " + std::to_string(sleepResult));
      }

      timespec now{};
      clock_gettime(CLOCK_MONOTONIC, &now);
      const int64_t jitterNs =
          (static_cast<int64_t>(now.tv_sec) - static_cast<int64_t>(next.tv_sec)) *
              1000000000LL +
          (static_cast<int64_t>(now.tv_nsec) -
           static_cast<int64_t>(next.tv_nsec));
      maxCycleJitterUs_ =
          std::max(maxCycleJitterUs_, std::abs(static_cast<double>(jitterNs)) /
                                          1000.0);

      const std::vector<IpcPacket> packets = ipc_.DrainPackets();
      RuntimeSnapshot snapshot;
      snapshot.preemptRtAvailable = preemptRtAvailable_;
      snapshot.maxCycleJitterUs = maxCycleJitterUs_;

      MainDeviceState aggregate = MainDeviceState::Operational;
      for (const auto& bus : buses_) {
        const bool linkActive = netlink_.LinkActive(bus->LogicalName());
        bus->Step(linkActive, packets);
        if (bus->Operational()) {
          snapshot.activeLogicalInterfaces.emplace_back(bus->LogicalName());
        }
        auto subDevices = bus->Snapshot();
        snapshot.subDevices.insert(snapshot.subDevices.end(),
                                   subDevices.begin(), subDevices.end());
        aggregate = Combine(aggregate, bus->State());
      }
      snapshot.aggregateState = aggregate;
      telemetry_.PublishSnapshot(std::move(snapshot));
    }

    for (auto& bus : buses_) {
      bus.reset();
    }
  }

  static MainDeviceState Combine(MainDeviceState current,
                                 MainDeviceState incoming) {
    return StateSeverity(incoming) > StateSeverity(current) ? incoming
                                                            : current;
  }

  static int StateSeverity(MainDeviceState state) {
    switch (state) {
      case MainDeviceState::Error:
        return 50;
      case MainDeviceState::Initializing:
      case MainDeviceState::SafeOperational:
        return 40;
      case MainDeviceState::WaitingForLink:
        return 30;
      case MainDeviceState::Starting:
      case MainDeviceState::Stopping:
        return 20;
      case MainDeviceState::Operational:
        return 10;
    }
    return 0;
  }

  DaemonConfig config_;
  EventLog& log_;
  Telemetry telemetry_;
  IpcServer ipc_;
  NetlinkMonitor netlink_;
  bool preemptRtAvailable_ = false;
  std::vector<std::unique_ptr<EthercatBus>> buses_;
  std::thread worker_;
  double maxCycleJitterUs_ = 0.0;
};

void SignalHandler(int) {
  g_running.store(false);
}

}  // namespace
}  // namespace limelight_ec

int main(int argc, char** argv) {
  signal(SIGINT, limelight_ec::SignalHandler);
  signal(SIGTERM, limelight_ec::SignalHandler);

  const std::string configPath =
      argc > 1 ? std::string(argv[1]) : std::string(limelight_ec::kConfigPath);

  try {
    limelight_ec::DaemonConfig config = limelight_ec::LoadConfig(configPath);
    limelight_ec::EventLog log(config.logDirectory);
    log.Event("Limelight Systemcore EtherCAT MainDevice daemon starting");
    log.Event(std::string("PREEMPT_RT ") +
              (limelight_ec::DetectPreemptRt() ? "available" : "unavailable"));

    limelight_ec::EthercatMainDeviceDaemon daemon(std::move(config), log);
    daemon.Run();
    log.Event("Limelight Systemcore EtherCAT MainDevice daemon stopped");
  } catch (const std::exception& ex) {
    std::cerr << "fatal: " << ex.what() << '\n';
    return 1;
  }
  return 0;
}
