#include "wifimanager.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <ETH.h>

#include "newproto.h"
#include "system.h"
#include "tag_db.h"
#include "udp.h"
#include "web.h"
#include "ips_display.h"
#include "tag_db.h"

uint8_t WifiManager::apClients = 0;
uint8_t x_buffer[100];
uint8_t x_position = 0;

#if defined(ETHERNET_PHY_POWER) && defined(ETHERNET_PHY_MDC) && defined(ETHERNET_PHY_MDIO) && defined(ETHERNET_PHY_TYPE) && defined(ETHERNET_CLK_MODE)
static bool eth_init = false;
static bool eth_connected = false;
static bool eth_ip_ok = false;
static long eth_timeout = 0;
#endif

WifiManager::WifiManager() {
    _reconnectIntervalCheck = 5000;
    _retryIntervalCheck = 5 * 60000;
    _connectionTimeout = 15000;

    _nextReconnectCheck = 0;
    _connected = false;
    _savewhensuccessfull = false;
    _ssid = "";
    _APstarted = false;
    wifiStatus = NOINIT;

    WiFi.onEvent(WiFiEvent);
    WiFiEventId_t eventID = WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        Serial.print("WiFi lost connection. Reason: ");
        Serial.println(info.wifi_sta_disconnected.reason);
    }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
}

void WifiManager::terminalLog(String text) {
    Serial.println(text);
#ifdef HAS_TFT
    TFTLog(text);
#endif
}

void WifiManager::poll() {

#if defined(ETHERNET_PHY_POWER) && defined(ETHERNET_PHY_MDC) && defined(ETHERNET_PHY_MDIO) && defined(ETHERNET_PHY_TYPE) && defined(ETHERNET_CLK_MODE)

    if (eth_connected) {
        wifiStatus = ETHERNET;
        if(!eth_ip_ok && eth_timeout != 0 && millis() - eth_timeout > 2000) {
            eth_timeout = 0;
            eth_connected = false;
        }
    } else if(!eth_connected && wifiStatus == ETHERNET) {
        wifiStatus = NOINIT;
        _APstarted = false;
        WiFi.mode(WIFI_STA);
        connectToWifi();
    }

#endif

    if (wifiStatus == AP && millis() > _nextReconnectCheck && _ssid != "") {
        if (apClients == 0) {
            terminalLog("Attempting to reconnect to WiFi.");
            logLine("Attempting to reconnect to WiFi.");
            _APstarted = false;
            wifiStatus = NOINIT;
            connectToWifi();
        } else {
            _nextReconnectCheck = millis() + _retryIntervalCheck;
        }
    }

    if (wifiStatus == CONNECTED && millis() > _nextReconnectCheck) {
        if (WiFi.status() != WL_CONNECTED) {
            _connected = false;
            terminalLog("WiFi connection lost. Attempting to reconnect.");
            logLine("WiFi connection lost. Attempting to reconnect.");
            WiFi.reconnect();
            waitForConnection();
        } else {
            _nextReconnectCheck = millis() + _reconnectIntervalCheck;
        }
    }

#ifndef HAS_USB

#ifdef ETHERNET_CLK_MODE
    if (!(ETHERNET_CLK_MODE == ETH_CLOCK_GPIO0_IN || ETHERNET_CLK_MODE == ETH_CLOCK_GPIO0_OUT)) {
#endif
    // ap_and_flasher has gpio0 in use as FLASHER_AP_POWER
    if (digitalRead(0) == LOW) {
        Serial.println("GPIO0 LOW");
        long starttime = millis();
        while (digitalRead(0) == LOW && millis() - starttime < 5000) {
        }
        if (digitalRead(0) == LOW) {
            Serial.println("Resetting WIFI settings");
            Preferences preferences;
            preferences.begin("wifi", false);
            preferences.putString("ssid", "");
            preferences.putString("pw", "");
            preferences.putString("ip", "");
            preferences.putString("mask", "");
            preferences.putString("gw", "");
            preferences.putString("dns", "");
            preferences.end();

            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            esp_wifi_init(&cfg);
            delay(2000);

            if (esp_wifi_restore() != ESP_OK) {
                Serial.println("WiFi is not initialized by esp_wifi_init ");
            } else {
                Serial.println("WiFi Configurations Cleared!");
            }
            delay(100);
            ESP.restart();
        }
    }
#ifdef ETHERNET_CLK_MODE
    }
#endif

#endif

    pollSerial();
}

void WifiManager::initEth() {
#if defined(ETHERNET_PHY_POWER) && defined(ETHERNET_PHY_MDC) && defined(ETHERNET_PHY_MDIO) && defined(ETHERNET_PHY_TYPE) && defined(ETHERNET_CLK_MODE)
    if(!eth_init) {
        eth_init = true;
        ETH.begin(
            ETH_PHY_ADDR,
            ETHERNET_PHY_POWER,
            ETHERNET_PHY_MDC,
            ETHERNET_PHY_MDIO,
            ETHERNET_PHY_TYPE,
            ETHERNET_CLK_MODE,
            false);
    }
#endif
}

bool WifiManager::connectToWifi() {
#if defined(ETHERNET_PHY_POWER) && defined(ETHERNET_PHY_MDC) && defined(ETHERNET_PHY_MDIO) && defined(ETHERNET_PHY_TYPE) && defined(ETHERNET_CLK_MODE)
    if (wifiStatus == ETHERNET || eth_connected)
        return true;
#endif

    Preferences preferences;
    preferences.begin("wifi", false);
    _ssid = preferences.getString("ssid", WiFi_SSID());
    _pass = preferences.getString("pw", WiFi_psk());
    if (_ssid == "") {
        terminalLog("No connection info saved");
        logLine("No connection information saved");
        startManagementServer();
        return false;
    }
    terminalLog("ssid: " + String(_ssid));

    String ip = preferences.getString("ip", "");
    String mask = preferences.getString("mask", "");
    String gw = preferences.getString("gw", "");
    String dns = preferences.getString("dns", "");

    if (ip.length() > 0) {
        IPAddress staticIP, subnetMask, gatewayIP, dnsIP;
        if (staticIP.fromString(ip) && subnetMask.fromString(mask) && gatewayIP.fromString(gw)) {
            if (dns.length() > 0) dnsIP.fromString(dns);
            WiFi.config(staticIP, gatewayIP, subnetMask, dnsIP);
            terminalLog("Setting static IP: " + ip);
        }
    }

    _connected = connectToWifi(_ssid, _pass, false);
    return _connected;
}

bool WifiManager::connectToWifi(String ssid, String pass, bool savewhensuccessfull) {
#if defined(ETHERNET_PHY_POWER) && defined(ETHERNET_PHY_MDC) && defined(ETHERNET_PHY_MDIO) && defined(ETHERNET_PHY_TYPE) && defined(ETHERNET_CLK_MODE)
    if (wifiStatus == ETHERNET)
        return true;
#endif

    _ssid = ssid;
    _pass = pass;
    _savewhensuccessfull = savewhensuccessfull;

    _APstarted = false;
    WiFi.disconnect(true, true);
    delay(100);
    WiFi.mode(WIFI_MODE_NULL);
    delay(100);
    WiFi.setHostname(buildHostname(ESP_MAC_WIFI_STA).c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(WIFI_PS_MIN_MODEM);

    terminalLog("Connecting to WiFi...");
    // logLine("Connecting to WiFi...");
    WiFi.persistent(savewhensuccessfull);
    WiFi.begin(_ssid.c_str(), _pass.c_str());
    _connected = waitForConnection();
    return _connected;
}

bool WifiManager::waitForConnection() {
#if defined(ETHERNET_PHY_POWER) && defined(ETHERNET_PHY_MDC) && defined(ETHERNET_PHY_MDIO) && defined(ETHERNET_PHY_TYPE) && defined(ETHERNET_CLK_MODE)
    if (wifiStatus == ETHERNET)
        return true;
#endif

    unsigned long timeout = millis() + _connectionTimeout;
    wifiStatus = WAIT_CONNECTING;

    while (WiFi.status() != WL_CONNECTED) {
        if (millis() > timeout) {
            terminalLog("!Unable to connect to WiFi");
            logLine("Unable to connect to WiFi");
            startManagementServer();
            return false;
        }
        vTaskDelay(250 / portTICK_PERIOD_MS);
    }

    if (_savewhensuccessfull) {
        Preferences preferences;
        preferences.begin("wifi", false);
        preferences.putString("ssid", _ssid);
        preferences.putString("pass", _pass);
        preferences.end();
        _savewhensuccessfull = false;
    }
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    IPAddress IP = WiFi.localIP();
    terminalLog("Connected!");
    // logLine("Connected!");
    _nextReconnectCheck = millis() + _reconnectIntervalCheck;
    wifiStatus = CONNECTED;
    return true;
}

void WifiManager::startManagementServer() {
    if (!_APstarted && wifiStatus != ETHERNET) {
        terminalLog("Starting config AP, ssid: OpenEPaperLink");
        logLine("Starting configuration AP, ssid OpenEPaperLink");
        WiFi.disconnect(true, true);
        delay(100);
        WiFi.mode(WIFI_AP);
        WiFi.softAP("OpenEPaperLink", "", 1, false);
        WiFi.softAPsetHostname("OpenEPaperLink");
        IPAddress IP = WiFi.softAPIP();
        terminalLog("Connect to it, visit http://" + String(IP.toString().c_str()) + "/setup");
        _APstarted = true;
        _nextReconnectCheck = millis() + _retryIntervalCheck;
        wifiStatus = AP;
    }
}

String WifiManager::buildHostname(esp_mac_type_t mac_type) {
    char hostname[32] = "OpenEpaperLink-";
    uint8_t mac[6];
    esp_read_mac(mac, mac_type);
    char lastTwoBytes[5];
    sprintf(lastTwoBytes, "%02X%02X", mac[4], mac[5]);
    strcat(hostname, lastTwoBytes);

    if (config.alias[0] != '\0') {
        int len = strlen(config.alias);
        int j = 0;
        for (int i = 0; i < len; i++) {
            char c = config.alias[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
                hostname[j] = c;
                j++;
            }
        }
        hostname[j] = '\0';
    }
    return String(hostname);
}

IPAddress WifiManager::localIP() {
    if (wifiStatus == ETHERNET) {
        return ETH.localIP();
    } else {
        return WiFi.localIP();
    }
}

String WifiManager::WiFi_SSID() {
    wifi_config_t conf;
    esp_wifi_get_config(WIFI_IF_STA, &conf);
    return String(reinterpret_cast<const char *>(conf.sta.ssid));
}

String WifiManager::WiFi_psk() {
    if (WiFiGenericClass::getMode() == WIFI_MODE_NULL) {
        return String();
    }
    wifi_config_t conf;
    esp_wifi_get_config(WIFI_IF_STA, &conf);
    return String(reinterpret_cast<char *>(conf.sta.password));
}

void WifiManager::pollSerial() {
    while (Serial.available() > 0) {
        char receivedChar = Serial.read();

        if (parse_improv_serial_byte(x_position, receivedChar, x_buffer, onCommandCallback, onErrorCallback)) {
            x_buffer[x_position++] = receivedChar;
            if (x_position > 100) {
                x_position = 0;
                Serial.println("buffer full!");
            }
        } else {
            x_position = 0;
        }
    }
}

void WifiManager::WiFiEvent(WiFiEvent_t event) {
    Serial.printf("[WiFi-event %d] ", event);
    String eventname="";

    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            eventname = "Connected to access point";
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            // eventname = "Disconnected from WiFi access point";
            break;
        case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE:
            eventname = "Authentication mode of access point has changed";
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            eventname = "Obtained IP address: " + String(WiFi.localIP().toString().c_str());
            init_udp();
            break;
        case ARDUINO_EVENT_WIFI_STA_LOST_IP:
            eventname = "Lost IP address and IP address is reset to 0";
            break;

        case ARDUINO_EVENT_WIFI_AP_START:
            // eventname = "WiFi access point started";
            break;
        case ARDUINO_EVENT_WIFI_AP_STOP:
            // eventname = "WiFi access point stopped";
            break;
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            apClients++;
            // eventname = "Client connected";
            break;
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            apClients--;
            // eventname = "Client disconnected";
            break;
        case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
            // eventname = "Assigned IP address to client";
            break;

#if defined(ETHERNET_PHY_POWER) && defined(ETHERNET_PHY_MDC) && defined(ETHERNET_PHY_MDIO) && defined(ETHERNET_PHY_TYPE) && defined(ETHERNET_CLK_MODE)

        case ARDUINO_EVENT_ETH_START:
            eventname = "ETH Started";
            //set eth hostname here
            ETH.setHostname(buildHostname(ESP_MAC_ETH).c_str());
            eth_timeout = 0;
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            eventname = "ETH Connected";
            WiFi.mode(WIFI_MODE_NULL);
            WiFi.disconnect();
            eth_connected = true;
            eth_timeout = millis();
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            if (ETH.fullDuplex()) {
                eventname = "ETH MAC: " + ETH.macAddress() + ", IPv4: " + ETH.localIP().toString() + ", FULL_DUPLEX, " + ETH.linkSpeed() + "Mbps";
            } else {
                eventname = "ETH MAC: " + ETH.macAddress() + ", IPv4: " + ETH.localIP().toString() + ", " + ETH.linkSpeed() + "Mbps";
            }
            eth_ip_ok = true;
            init_udp();
            eth_timeout = 0;
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            eventname = "ETH Disconnected";
            eth_connected = false;
            eth_ip_ok = false;
            eth_timeout = 0;
            break;
        case ARDUINO_EVENT_ETH_STOP:
            eventname = "ETH Stopped";
            eth_connected = false;
            eth_ip_ok = false;
            eth_timeout = 0;
            break;

#endif

        default:
            break;
    }
    if (eventname) terminalLog(eventname);
    // logLine("WiFi event [" + String(event) + "]: " + eventname);
}

// *** Improv
// https :  // github.com/jnthas/improv-wifi-demo

#define STR_IMPL(x) #x
#define STR(x) STR_IMPL(x)

#ifndef BUILD_ENV_NAME
#define BUILD_ENV_NAME unknown
#endif
#ifndef BUILD_TIME
#define BUILD_TIME 0
#endif
#ifndef BUILD_VERSION
#define BUILD_VERSION custom
#endif

std::vector<std::string> getLocalUrl() {
    return {String("http://" + WiFi.localIP().toString()).c_str()};
}

void onErrorCallback(improv::Error err) {
}

bool onCommandCallback(improv::ImprovCommand cmd) {
    switch (cmd.command) {
        case improv::Command::GET_CURRENT_STATE: {
            if ((WiFi.status() == WL_CONNECTED)) {
                set_state(improv::State::STATE_PROVISIONED);
                std::vector<uint8_t> data = improv::build_rpc_response(improv::GET_CURRENT_STATE, getLocalUrl(), false);
                send_response(data);
            } else {
                set_state(improv::State::STATE_AUTHORIZED);
            }
            break;
        }

        case improv::Command::WIFI_SETTINGS: {
            if (cmd.ssid.length() == 0) {
                set_error(improv::Error::ERROR_INVALID_RPC);
                break;
            }

            set_state(improv::STATE_PROVISIONING);

            ws.enable(false);
            refreshAllPending();
            saveDB("/current/tagDB.json");
            ws.closeAll();
            delay(100);
            if (wm.connectToWifi(String(cmd.ssid.c_str()), String(cmd.password.c_str()), true)) {
                Preferences preferences;
                preferences.begin("wifi", false);
                preferences.putString("ssid", cmd.ssid.c_str());
                preferences.putString("pw", cmd.password.c_str());
                preferences.end();
                ws.enable(true);

                set_state(improv::STATE_PROVISIONED);
                std::vector<uint8_t> data = improv::build_rpc_response(improv::WIFI_SETTINGS, getLocalUrl(), false);
                send_response(data);
            } else {
                set_state(improv::STATE_STOPPED);
                set_error(improv::Error::ERROR_UNABLE_TO_CONNECT);
            }

            break;
        }

        case improv::Command::GET_DEVICE_INFO: {
            std::vector<std::string> infos = {
                // Firmware name
                "OpenEPaperLink",
                // Firmware version
                STR(BUILD_VERSION),
                // Hardware chip/variant
                STR(BUILD_ENV_NAME),
                // Device name
                "Access Point"};
            std::vector<uint8_t> data = improv::build_rpc_response(improv::GET_DEVICE_INFO, infos, false);
            send_response(data);
            break;
        }

        case improv::Command::GET_WIFI_NETWORKS: {
            getAvailableWifiNetworks();
            break;
        }

        default: {
            set_error(improv::ERROR_UNKNOWN_RPC);
            return false;
        }
    }

    return true;
}

void getAvailableWifiNetworks() {
    int networkNum = WiFi.scanNetworks();

    for (int id = 0; id < networkNum; ++id) {
        std::vector<uint8_t> data = improv::build_rpc_response(
            improv::GET_WIFI_NETWORKS, {WiFi.SSID(id), String(WiFi.RSSI(id)), (WiFi.encryptionType(id) == WIFI_AUTH_OPEN ? "NO" : "YES")}, false);
        send_response(data);
        delay(1);
    }
    // final response
    std::vector<uint8_t> data =
        improv::build_rpc_response(improv::GET_WIFI_NETWORKS, std::vector<std::string>{}, false);
    send_response(data);
}

void set_state(improv::State state) {
    std::vector<uint8_t> data = {'I', 'M', 'P', 'R', 'O', 'V'};
    data.resize(11);
    data[6] = improv::IMPROV_SERIAL_VERSION;
    data[7] = improv::TYPE_CURRENT_STATE;
    data[8] = 1;
    data[9] = state;

    uint8_t checksum = 0x00;
    for (uint8_t d : data)
        checksum += d;
    data[10] = checksum;

    Serial.write(data.data(), data.size());
}

void send_response(std::vector<uint8_t> &response) {
    std::vector<uint8_t> data = {'I', 'M', 'P', 'R', 'O', 'V'};
    data.resize(9);
    data[6] = improv::IMPROV_SERIAL_VERSION;
    data[7] = improv::TYPE_RPC_RESPONSE;
    data[8] = response.size();
    data.insert(data.end(), response.begin(), response.end());

    uint8_t checksum = 0x00;
    for (uint8_t d : data)
        checksum += d;
    data.push_back(checksum);

    Serial.write(data.data(), data.size());
}

void set_error(improv::Error error) {
    std::vector<uint8_t> data = {'I', 'M', 'P', 'R', 'O', 'V'};
    data.resize(11);
    data[6] = improv::IMPROV_SERIAL_VERSION;
    data[7] = improv::TYPE_ERROR_STATE;
    data[8] = 1;
    data[9] = error;

    uint8_t checksum = 0x00;
    for (uint8_t d : data)
        checksum += d;
    data[10] = checksum;

    Serial.write(data.data(), data.size());
}

// **** improv ****

namespace improv {

ImprovCommand parse_improv_data(const std::vector<uint8_t> &data, bool check_checksum) {
    return parse_improv_data(data.data(), data.size(), check_checksum);
}

ImprovCommand parse_improv_data(const uint8_t *data, size_t length, bool check_checksum) {
    ImprovCommand improv_command;
    Command command = (Command)data[0];
    uint8_t data_length = data[1];

    if (data_length != length - 2 - check_checksum) {
        improv_command.command = UNKNOWN;
        return improv_command;
    }

    if (check_checksum) {
        uint8_t checksum = data[length - 1];

        uint32_t calculated_checksum = 0;
        for (uint8_t i = 0; i < length - 1; i++) {
            calculated_checksum += data[i];
        }

        if ((uint8_t)calculated_checksum != checksum) {
            improv_command.command = BAD_CHECKSUM;
            return improv_command;
        }
    }

    if (command == WIFI_SETTINGS) {
        uint8_t ssid_length = data[2];
        uint8_t ssid_start = 3;
        size_t ssid_end = ssid_start + ssid_length;

        uint8_t pass_length = data[ssid_end];
        size_t pass_start = ssid_end + 1;
        size_t pass_end = pass_start + pass_length;

        std::string ssid(data + ssid_start, data + ssid_end);
        std::string password(data + pass_start, data + pass_end);
        return {.command = command, .ssid = ssid, .password = password};
    }

    improv_command.command = command;
    return improv_command;
}

bool parse_improv_serial_byte(size_t position, uint8_t byte, const uint8_t *buffer,
                              std::function<bool(ImprovCommand)> &&callback, std::function<void(Error)> &&on_error) {
    if (position == 0)
        return byte == 'I';
    if (position == 1)
        return byte == 'M';
    if (position == 2)
        return byte == 'P';
    if (position == 3)
        return byte == 'R';
    if (position == 4)
        return byte == 'O';
    if (position == 5)
        return byte == 'V';

    if (position == 6)
        return byte == IMPROV_SERIAL_VERSION;

    if (position <= 8)
        return true;

    uint8_t type = buffer[7];
    uint8_t data_len = buffer[8];

    if (position <= 8 + data_len)
        return true;

    if (position == 8 + data_len + 1) {
        uint8_t checksum = 0x00;
        for (size_t i = 0; i < position; i++)
            checksum += buffer[i];

        if (checksum != byte) {
            on_error(ERROR_INVALID_RPC);
            return false;
        }

        if (type == TYPE_RPC) {
            auto command = parse_improv_data(&buffer[9], data_len, false);
            return callback(command);
        }
    }

    return false;
}

std::vector<uint8_t> build_rpc_response(Command command, const std::vector<std::string> &datum, bool add_checksum) {
    std::vector<uint8_t> out;
    uint32_t length = 0;
    out.push_back(command);
    for (const auto &str : datum) {
        uint8_t len = str.length();
        length += len + 1;
        out.push_back(len);
        out.insert(out.end(), str.begin(), str.end());
    }
    out.insert(out.begin() + 1, length);

    if (add_checksum) {
        uint32_t calculated_checksum = 0;

        for (uint8_t byte : out) {
            calculated_checksum += byte;
        }
        out.push_back(calculated_checksum);
    }
    return out;
}

std::vector<uint8_t> build_rpc_response(Command command, const std::vector<String> &datum, bool add_checksum) {
    std::vector<uint8_t> out;
    uint32_t length = 0;
    out.push_back(command);
    for (const auto &str : datum) {
        uint8_t len = str.length();
        length += len;
        out.push_back(len);
        out.insert(out.end(), str.begin(), str.end());
    }
    out.insert(out.begin() + 1, length);

    if (add_checksum) {
        uint32_t calculated_checksum = 0;

        for (uint8_t byte : out) {
            calculated_checksum += byte;
        }
        out.push_back(calculated_checksum);
    }
    return out;
}

}  // namespace improv
