// Arduino NetworkManager::hostByName calls the raw dns_clear_cache() when an
// interface first gets an IP (and after reconnects). Clearing pending lookups
// invokes their callbacks, including SNTP retry/UDP creation, so this must run
// inside the lwIP core. Protect the raw call, not the blocking DNS/HTTP request.
// Link wrapping keeps this workaround local to the firmware; the shared SDK
// and its TCPIP thread-safety assertions remain unchanged.
#if defined(ARDUINO_ARCH_ESP32)
#include <lwip/priv/tcpip_priv.h>
#include <lwip/sys.h>
#include <esp32-hal-log.h>

extern "C" void __real_dns_clear_cache(void);

namespace {
err_t clearDnsCacheInCore(tcpip_api_call_data*) {
    __real_dns_clear_cache();
    return ERR_OK;
}
}

extern "C" void __wrap_dns_clear_cache(void) {
    // A core callback can also clear the cache. Do not acquire a nonrecursive
    // core lock or wait on our own TCPIP thread in that case.
    if (sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER)) {
        __real_dns_clear_cache();
        return;
    }
    tcpip_api_call_data call{};
    // Synchronous dispatch also preserves clear-before-lookup ordering when
    // LWIP_TCPIP_CORE_LOCKING is disabled. No stack data outlives this call.
    const err_t result = tcpip_api_call(clearDnsCacheInCore, &call);
    if (result != ERR_OK) {
        log_w("DNS cache clear dispatch failed: %d", static_cast<int>(result));
    }
}
#endif
