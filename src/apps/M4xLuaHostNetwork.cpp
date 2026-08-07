#include "apps/M4xLuaHost.h"

#include <HTTPClient.h>
#include <NetworkClientSecure.h>

#include "debug/M4HeapAudit.h"

void M4xLuaHost::releaseNetworkSession() {
  M4HeapAudit::snapshot("net_release_before");

  // HTTPClient may retain connection/parser state; end it before destroying the
  // underlying TLS client. Keep Wi-Fi itself up so provider work can reconnect
  // later without paying association/DHCP cost.
  if (netHttp_) netHttp_->end();
  if (netTls_) netTls_->stop();
  netHttp_.reset();
  netTls_.reset();

  M4HeapAudit::snapshot("net_release_after");
}
