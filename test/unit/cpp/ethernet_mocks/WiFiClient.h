#pragma once
class Client { public: virtual ~Client() = default; };
class WiFiClient : public Client { public: void stop() {} };
