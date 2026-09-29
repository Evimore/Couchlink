#include "discovery.h"

#include "log.h"

#ifdef _WIN32
  #include <windows.h>

  #include <chrono>
  #include <condition_variable>
  #include <mutex>
#endif

namespace couchlink::discovery {

  std::string instance_label(const std::string &name) {
    std::string label;
    for (char c : name) {
      label.push_back(c == '.' ? '-' : c);
    }
    if (label.size() > 63) {
      std::size_t cut = 63;
      while (cut > 0 && (static_cast<unsigned char>(label[cut]) & 0xC0) == 0x80) {
        --cut;  // don't split a UTF-8 sequence
      }
      label.resize(cut);
    }
    return label.empty() ? "couchlink-host" : label;
  }

#ifdef _WIN32

  namespace {
    // The DNS-SD API is in dnsapi.dll since Windows 10 1809. It is loaded at
    // run time, so older Windows still runs couchlink-host (without discovery)
    // and the build does not depend on the SDK version's windns.h.
    constexpr ULONG kRequestVersion1 = 1;  // DNS_QUERY_REQUEST_VERSION1
    constexpr DWORD kRequestPending = 9506;  // DNS_REQUEST_PENDING

    using RegisterComplete = VOID(WINAPI *)(DWORD status, PVOID context, PVOID instance);

    struct RegisterRequest {  // DNS_SERVICE_REGISTER_REQUEST
      ULONG version;
      ULONG interface_index;
      PVOID instance;
      RegisterComplete callback;
      PVOID context;
      HANDLE credentials;
      BOOL unicast_enabled;
    };

    using ConstructInstance = PVOID(WINAPI *)(
      PCWSTR service_name, PCWSTR host_name, PVOID ip4, PVOID ip6, WORD port, WORD priority, WORD weight,
      DWORD property_count, PCWSTR *keys, PCWSTR *values
    );
    using FreeInstance = VOID(WINAPI *)(PVOID instance);
    using Register = DWORD(WINAPI *)(RegisterRequest *request, PVOID cancel);

    std::wstring widen(const std::string &text) {
      if (text.empty()) {
        return {};
      }
      const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
      std::wstring out(static_cast<std::size_t>(length), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
      return out;
    }

    template<typename T>
    T load(HMODULE module, const char *name) {
      // Casting through void (*)() keeps GCC's -Wcast-function-type quiet.
      return reinterpret_cast<T>(reinterpret_cast<void (*)()>(GetProcAddress(module, name)));
    }
  }  // namespace

  struct Advertiser::Impl {
    HMODULE dnsapi = nullptr;
    ConstructInstance construct = nullptr;
    FreeInstance free_instance = nullptr;
    Register register_service = nullptr;
    Register deregister_service = nullptr;

    PVOID instance = nullptr;
    RegisterRequest request {};

    std::mutex mutex;
    std::condition_variable changed;
    bool deregistered = false;

    static VOID WINAPI on_registered(DWORD status, PVOID context, PVOID instance) {
      auto *self = static_cast<Impl *>(context);
      if (instance != nullptr) {
        self->free_instance(instance);
      }
      if (status == ERROR_SUCCESS) {
        log::info("discovery: Couchlink apps on this network can now find this PC");
      } else {
        log::warn("discovery: announcing this PC failed (error ", status, "); enter its address in Couchlink instead");
      }
    }

    static VOID WINAPI on_deregistered(DWORD, PVOID context, PVOID instance) {
      auto *self = static_cast<Impl *>(context);
      if (instance != nullptr) {
        self->free_instance(instance);
      }
      std::lock_guard lock(self->mutex);
      self->deregistered = true;
      self->changed.notify_all();
    }

    ~Impl() {
      if (instance != nullptr) {
        free_instance(instance);
      }
      if (dnsapi != nullptr) {
        FreeLibrary(dnsapi);
      }
    }
  };

  Advertiser::Advertiser() = default;

  Advertiser::~Advertiser() {
    stop();
  }

  bool Advertiser::start(const std::string &name, std::uint16_t port) {
    stop();
    auto impl = std::make_unique<Impl>();
    impl->dnsapi = LoadLibraryW(L"dnsapi.dll");
    if (impl->dnsapi != nullptr) {
      impl->construct = load<ConstructInstance>(impl->dnsapi, "DnsServiceConstructInstance");
      impl->free_instance = load<FreeInstance>(impl->dnsapi, "DnsServiceFreeInstance");
      impl->register_service = load<Register>(impl->dnsapi, "DnsServiceRegister");
      impl->deregister_service = load<Register>(impl->dnsapi, "DnsServiceDeRegister");
    }
    if (!impl->construct || !impl->free_instance || !impl->register_service || !impl->deregister_service) {
      log::info("discovery: not available on this Windows version; enter this PC's address in Couchlink");
      return false;
    }

    wchar_t computer[256] = {};
    DWORD computer_length = 256;
    if (!GetComputerNameExW(ComputerNameDnsHostname, computer, &computer_length) || computer_length == 0) {
      log::warn("discovery: could not read this PC's host name");
      return false;
    }
    const std::wstring service = widen(instance_label(name)) + L"." + widen(kServiceType) + L".local";
    const std::wstring host = std::wstring(computer) + L".local";
    PCWSTR keys[] = {L"v"};
    PCWSTR values[] = {L"1"};
    impl->instance = impl->construct(service.c_str(), host.c_str(), nullptr, nullptr, port, 0, 0, 1, keys, values);
    if (impl->instance == nullptr) {
      log::warn("discovery: could not describe the service");
      return false;
    }

    impl->request.version = kRequestVersion1;
    impl->request.instance = impl->instance;
    impl->request.callback = &Impl::on_registered;
    impl->request.context = impl.get();
    const DWORD status = impl->register_service(&impl->request, nullptr);
    if (status != kRequestPending) {
      log::warn("discovery: announcing this PC failed (error ", status, "); enter its address in Couchlink instead");
      return false;
    }
    log::debug("discovery: announcing '", instance_label(name), "' as ", kServiceType, " on port ", port);
    impl_ = impl.release();
    return true;
  }

  void Advertiser::stop() {
    if (impl_ == nullptr) {
      return;
    }
    Impl *impl = impl_;
    impl_ = nullptr;
    impl->request.callback = &Impl::on_deregistered;
    if (impl->deregister_service(&impl->request, nullptr) != kRequestPending) {
      delete impl;
      return;
    }
    std::unique_lock lock(impl->mutex);
    if (impl->changed.wait_for(lock, std::chrono::seconds(2), [impl] { return impl->deregistered; })) {
      lock.unlock();
      delete impl;
    }
    // Otherwise Windows may still call back into impl: leave it allocated.
  }

#else

  struct Advertiser::Impl {};

  Advertiser::Advertiser() = default;

  Advertiser::~Advertiser() = default;

  bool Advertiser::start(const std::string &, std::uint16_t) {
    log::debug("discovery: not built in on this platform; enter this PC's address in Couchlink");
    return false;
  }

  void Advertiser::stop() {}

#endif

}  // namespace couchlink::discovery
