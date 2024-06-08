#pragma once
#include <Arduino.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <WiFi.h>

enum WifiStatus {
    NOINIT,
    WAIT_CONNECTING,
    CONNECTED,
    WAIT_RECONNECT,
    AP,
    ETHERNET
};

class WifiManager {
   private:
    bool _connected;
    bool _savewhensuccessfull;
    int _reconnectIntervalCheck;
    int _retryIntervalCheck;
    int _connectionTimeout;
    String _ssid;
    String _pass;
    unsigned long _nextReconnectCheck;
    bool _APstarted;

    const int SERIAL_BUFFER_SIZE = 64;
    char serialBuffer[64];
    int serialIndex = 0;

    String WiFi_SSID();
    String WiFi_psk();

    bool waitForConnection();
    void pollSerial();
    static void terminalLog(String text);
    static String buildHostname(esp_mac_type_t mac_type);

   public:
    WifiManager();

    WifiStatus wifiStatus;
    static uint8_t apClients;
    bool connectToWifi();
    bool connectToWifi(String ssid, String pass, bool savewhensuccessfull);

    void startManagementServer();
    void poll();
    static void WiFiEvent(WiFiEvent_t event);
    void initEth();
    IPAddress localIP();
};

#endif

extern WifiManager wm;

// **** Improv Wi-Fi ****
// https://www.improv-wifi.com/
// https://github.com/jnthas/improv-wifi-demo
// http://www.apache.org/licenses/LICENSE-2.0

namespace improv {

enum Error : uint8_t {
    ERROR_NONE = 0x00,
    ERROR_INVALID_RPC = 0x01,
    ERROR_UNKNOWN_RPC = 0x02,
    ERROR_UNABLE_TO_CONNECT = 0x03,
    ERROR_NOT_AUTHORIZED = 0x04,
    ERROR_UNKNOWN = 0xFF,
};

enum State : uint8_t {
    STATE_STOPPED = 0x00,
    STATE_AWAITING_AUTHORIZATION = 0x01,
    STATE_AUTHORIZED = 0x02,
    STATE_PROVISIONING = 0x03,
    STATE_PROVISIONED = 0x04,
};

enum Command : uint8_t {
    UNKNOWN = 0x00,
    WIFI_SETTINGS = 0x01,
    IDENTIFY = 0x02,
    GET_CURRENT_STATE = 0x02,
    GET_DEVICE_INFO = 0x03,
    GET_WIFI_NETWORKS = 0x04,
    BAD_CHECKSUM = 0xFF,
};

static const uint8_t CAPABILITY_IDENTIFY = 0x01;
static const uint8_t IMPROV_SERIAL_VERSION = 1;

enum ImprovSerialType : uint8_t {
    TYPE_CURRENT_STATE = 0x01,
    TYPE_ERROR_STATE = 0x02,
    TYPE_RPC = 0x03,
    TYPE_RPC_RESPONSE = 0x04
};

struct ImprovCommand {
    Command command;
    std::string ssid;
    std::string password;
};

ImprovCommand parse_improv_data(const std::vector<uint8_t> &data, bool check_checksum = true);
ImprovCommand parse_improv_data(const uint8_t *data, size_t length, bool check_checksum = true);

bool parse_improv_serial_byte(size_t position, uint8_t byte, const uint8_t *buffer,
                              std::function<bool(ImprovCommand)> &&callback, std::function<void(Error)> &&on_error);

std::vector<uint8_t> build_rpc_response(Command command, const std::vector<std::string> &datum,
                                        bool add_checksum = true);
std::vector<uint8_t> build_rpc_response(Command command, const std::vector<String> &datum, bool add_checksum = true);

}  // namespace improv

void set_state(improv::State state);
void send_response(std::vector<uint8_t> &response);
void set_error(improv::Error error);
void getAvailableWifiNetworks();
bool onCommandCallback(improv::ImprovCommand cmd);
void onErrorCallback(improv::Error err);
