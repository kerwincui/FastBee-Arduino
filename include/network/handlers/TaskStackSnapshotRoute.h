#pragma once
class AsyncWebServer;
class WebHandlerContext;
namespace FastBee {
void setupTaskStackSnapshotRoute(AsyncWebServer* server, WebHandlerContext* context);
}
