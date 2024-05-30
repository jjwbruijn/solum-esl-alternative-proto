#include "tag_db.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>

#include <unordered_map>
#include <vector>

#include "language.h"
#include "storage.h"
#include "util.h"

#define STR_IMPL(x) #x
#define STR(x) STR_IMPL(x)

std::vector<tagRecord*> tagDB;
std::unordered_map<std::string, varStruct> varDB;
std::unordered_map<int, HwType> hwdata = {};

Config config;

tagRecord* tagRecord::findByMAC(const uint8_t mac[8]) {
    for (tagRecord* tag : tagDB) {
        if (memcmp(tag->mac, mac, 8) == 0 && tag->version == 0) {
            return tag;
        }
    }
    return nullptr;
}

bool deleteRecord(const uint8_t mac[8], bool allVersions) {
    for (uint32_t c = 0; c < tagDB.size(); c++) {
        tagRecord* tag = tagDB.at(c);
        if (memcmp(tag->mac, mac, 8) == 0 && (allVersions || tag->version == 0)) {
            if (tag->data != nullptr) {
                free(tag->data);
            }
            tag->data = nullptr;
            delete tagDB[c];
            tagDB.erase(tagDB.begin() + c);
            return true;
        }
    }
    return false;
}

void mac2hex(const uint8_t* mac, char* hexBuffer) {
    sprintf(hexBuffer, "%02X%02X%02X%02X%02X%02X%02X%02X",
            mac[7], mac[6], mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
}

bool hex2mac(const String& hexString, uint8_t* mac) {
    size_t hexLength = hexString.length();
    if (hexLength != 12 && hexLength != 16) {
        return false;
    }
    if (hexLength / 2 == 6) {
        mac[6] = 0;
        mac[7] = 0;
        return (sscanf(hexString.c_str(), "%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX",
                       &mac[5], &mac[4], &mac[3], &mac[2], &mac[1], &mac[0]) == 6);
    } else {
        return (sscanf(hexString.c_str(), "%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX",
                       &mac[7], &mac[6], &mac[5], &mac[4], &mac[3], &mac[2], &mac[1], &mac[0]) == 8);
    }
}

String tagDBtoJson(const uint8_t mac[8], uint8_t startPos) {
    DynamicJsonDocument doc(5000);
    JsonArray tags = doc.createNestedArray("tags");

    for (uint32_t c = startPos; c < tagDB.size(); ++c) {
        const tagRecord* taginfo = tagDB.at(c);

        const bool select = !mac || memcmp(taginfo->mac, mac, 8) == 0;
        if (select && taginfo->version == 0) {
            JsonObject tag = tags.createNestedObject();
            fillNode(tag, taginfo);
            if (mac) {
                break;
            }
        }

        if (doc.capacity() - doc.memoryUsage() < doc.memoryUsage() / (c + 1) + 500) {
            doc["continu"] = c + 1;
            break;
        }
    }

    return doc.as<String>();
}

void fillNode(JsonObject& tag, const tagRecord* taginfo) {
    char hexmac[17];
    mac2hex(taginfo->mac, hexmac);
    tag["mac"] = String(hexmac);
    char hex[33];
    for (uint8_t i = 0; i < 16; i++) {
        sprintf(hex + (i * 2), "%02x", taginfo->md5[i]);
    }
    tag["hash"] = (String)hex;
    tag["lastseen"] = taginfo->lastseen;
    tag["nextupdate"] = taginfo->nextupdate;
    tag["nextcheckin"] = taginfo->expectedNextCheckin;
    tag["pending"] = taginfo->pendingCount;
    tag["alias"] = taginfo->alias;
    tag["contentMode"] = taginfo->contentMode;
    tag["LQI"] = taginfo->LQI;
    tag["RSSI"] = taginfo->RSSI;
    tag["temperature"] = taginfo->temperature;
    tag["batteryMv"] = taginfo->batteryMv;
    tag["hwType"] = taginfo->hwType;
    tag["wakeupReason"] = taginfo->wakeupReason;
    tag["capabilities"] = taginfo->capabilities;
    tag["modecfgjson"] = taginfo->modeConfigJson;
    tag["isexternal"] = taginfo->isExternal;
    tag["apip"] = taginfo->apIp.toString();
    tag["rotate"] = taginfo->rotate;
    tag["lut"] = taginfo->lut;
    tag["invert"] = taginfo->invert;
    tag["updatecount"] = taginfo->updateCount;
    tag["updatelast"] = taginfo->updateLast;
    tag["ch"] = taginfo->currentChannel;
    tag["ver"] = taginfo->tagSoftwareVersion;
}

void saveDB(const String& filename) {
    DynamicJsonDocument doc(2500);

    const long t = millis();

    xSemaphoreTake(fsMutex, portMAX_DELAY);

    fs::File existingFile = contentFS->open(filename, "r");
    if (existingFile) {
        existingFile.close();
        vTaskDelay(pdMS_TO_TICKS(100));
        String backupFilename = filename + ".bak";
        if (!contentFS->rename(filename.c_str(), backupFilename.c_str())) {
            xSemaphoreGive(fsMutex);
            logLine("error renaming tagDB to .bak");
            wsErr("error renaming tagDB to .bak");
            xSemaphoreTake(fsMutex, portMAX_DELAY);
        }
    }

    fs::File file = contentFS->open(filename, "w");
    if (!file) {
        Serial.println("saveDB: Failed to open file for writing");
        xSemaphoreGive(fsMutex);
        return;
    }

    file.write('[');
    for (size_t c = 0; c < tagDB.size(); c++) {
        const tagRecord* taginfo = tagDB.at(c);
        doc.clear();

        if (taginfo->version == 0) {
            JsonObject tag = doc.createNestedObject();
            fillNode(tag, taginfo);
            if (c > 0) {
                file.write(',');
            }
            serializeJsonPretty(doc, file);
        }
    }
    file.write(']');

    file.close();
    xSemaphoreGive(fsMutex);
    Serial.println("DB saved " + String(millis() - t) + "ms");
}

bool loadDB(const String& filename) {
    Serial.println("reading DB from " + String(filename));
    const long t = millis();

    fs::File readfile = contentFS->open(filename, "r");
    if (!readfile) {
        Serial.println("loadDB: Failed to open file");
        return false;
    }

    time_t now;
    time(&now);
    bool parsing = true;

    if (readfile.find("[")) {
        DynamicJsonDocument doc(1000);
        while (parsing) {
            DeserializationError err = deserializeJson(doc, readfile);
            if (!err) {
                JsonObject tag = doc[0];
                String dst = tag["mac"].as<String>();
                uint8_t mac[8];
                if (hex2mac(dst, mac)) {
                    tagRecord* taginfo = tagRecord::findByMAC(mac);
                    if (taginfo == nullptr) {
                        taginfo = new tagRecord;
                        memcpy(taginfo->mac, mac, sizeof(taginfo->mac));
                        tagDB.push_back(taginfo);
                    }
                    String md5 = tag["hash"].as<String>();
                    if (md5.length() >= 32) {
                        for (uint8_t i = 0; i < 16; i++) {
                            taginfo->md5[i] = strtoul(md5.substring(i * 2, i * 2 + 2).c_str(), NULL, 16);
                        }
                    }
                    taginfo->lastseen = (uint32_t)tag["lastseen"];
                    taginfo->nextupdate = (uint32_t)tag["nextupdate"];
                    taginfo->expectedNextCheckin = (uint32_t)tag["nextcheckin"];
                    if (taginfo->expectedNextCheckin < now) {
                        taginfo->expectedNextCheckin = now + 1800;
                    }
                    taginfo->pendingCount = 0;
                    taginfo->alias = tag["alias"].as<String>();
                    taginfo->contentMode = tag["contentMode"];
                    taginfo->LQI = tag["LQI"];
                    taginfo->RSSI = tag["RSSI"];
                    taginfo->temperature = tag["temperature"];
                    taginfo->batteryMv = tag["batteryMv"];
                    taginfo->hwType = (uint8_t)tag["hwType"];
                    taginfo->wakeupReason = tag["wakeupReason"];
                    taginfo->capabilities = tag["capabilities"];
                    taginfo->modeConfigJson = tag["modecfgjson"].as<String>();
                    taginfo->isExternal = tag["isexternal"].as<bool>();
                    taginfo->apIp.fromString(tag["apip"].as<String>());
                    taginfo->rotate = tag["rotate"] | 0;
                    taginfo->lut = tag["lut"] | 0;
                    taginfo->invert = tag["invert"] | 0;
                    taginfo->updateCount = tag["updatecount"] | 0;
                    taginfo->updateLast = tag["updatelast"] | 0;
                    taginfo->currentChannel = tag["ch"] | 0;
                    taginfo->tagSoftwareVersion = tag["ver"] | 0;
                }
            } else {
                Serial.print(F("deserializeJson() failed: "));
                Serial.println(err.c_str());
                parsing = false;
                readfile.close();
                return false;
            }
            parsing = parsing && readfile.find(",");
        }
    } else {
        readfile.close();
        return false;
    }

    readfile.close();
    Serial.println("loadDB took " + String(millis() - t) + "ms");
    return true;
}

void destroyDB() {
    Serial.println("destroying DB");
    util::printHeap();
    for (tagRecord*& tag : tagDB) {
        if (tag->data != nullptr) {
            free(tag->data);
        }
        tag->data = nullptr;
        delete tag;
    }
    tagDB.clear();
    util::printHeap();
}

uint32_t getTagCount() {
    uint32_t temp = 0;
    return getTagCount(temp, temp);
}

uint32_t getTagCount(uint32_t& timeoutcount, uint32_t& lowbattcount) {
    uint32_t tagcount = 0;
    time_t now;
    time(&now);
    for (const tagRecord* taginfo : tagDB) {
        if (!taginfo->isExternal) tagcount++;
        const int32_t timeout = now - taginfo->lastseen;
        if (taginfo->expectedNextCheckin < 3600) {
            // not initialised, timeout if not seen last 10 minutes
            if (timeout > 600) timeoutcount++;
        } else if (now - taginfo->expectedNextCheckin > 600) {
            // expected checkin is behind, timeout if not seen last 10 minutes
            if (timeout > 600) timeoutcount++;
        }
        if (taginfo->batteryMv < 2400 && taginfo->batteryMv != 0 && taginfo->batteryMv != 1337) lowbattcount++;
    }
    return tagcount;
}

void clearPending(tagRecord* taginfo) {
    taginfo->filename = String();
    if (taginfo->data != nullptr) {
        // check if this is the last copy of the buffer
        int datacount = 0;
        for (const tagRecord* tag : tagDB) {
            if (tag->data == taginfo->data) {
                datacount++;
            }
        }
        if (datacount == 1) {
            free(taginfo->data);
        }
        taginfo->data = nullptr;
    }
}

void initAPconfig() {
    DynamicJsonDocument APconfig(768);
    File configFile = contentFS->open("/current/apconfig.json", "r");
    if (configFile) {
        DeserializationError error = deserializeJson(APconfig, configFile);
        if (error) {
            configFile.close();
            Serial.println("failed to read apconfig.json. Using default config");
            Serial.println(error.c_str());
        }
        configFile.close();
    }
    config.channel = APconfig.containsKey("channel") ? APconfig["channel"] : 0;
    config.subghzchannel = APconfig.containsKey("subghzchannel") ? APconfig["subghzchannel"] : 0;
    if (APconfig["alias"]) strlcpy(config.alias, APconfig["alias"], sizeof(config.alias));
    config.led = APconfig.containsKey("led") ? APconfig["led"] : 255;
    config.tft = APconfig.containsKey("tft") ? APconfig["tft"] : 255;
    config.language = APconfig.containsKey("language") ? APconfig["language"] : 0;
    config.maxsleep = APconfig.containsKey("maxsleep") ? APconfig["maxsleep"] : 10;
    config.stopsleep = APconfig.containsKey("stopsleep") ? APconfig["stopsleep"] : 1;
    config.preview = APconfig.containsKey("preview") ? APconfig["preview"] : 1;
    config.nightlyreboot = APconfig.containsKey("nightlyreboot") ? APconfig["nightlyreboot"] : 1;
    config.lock = APconfig.containsKey("lock") ? APconfig["lock"] : 0;
    config.sleepTime1 = APconfig.containsKey("sleeptime1") ? APconfig["sleeptime1"] : 0;
    config.sleepTime2 = APconfig.containsKey("sleeptime2") ? APconfig["sleeptime2"] : 0;
    config.ble = APconfig.containsKey("ble") ? APconfig["ble"] : 0;
    config.discovery = APconfig.containsKey("discovery") ? APconfig["discovery"] : 0;
#ifdef BLE_ONLY
    config.ble = true;
#endif
    // default wifi power 8.5 dbM
    // see https://github.com/espressif/arduino-esp32/blob/master/libraries/WiFi/src/WiFiGeneric.h#L111
    config.wifiPower = APconfig.containsKey("wifipower") ? APconfig["wifipower"] : 34;
    config.repo = APconfig.containsKey("repo") ? APconfig["repo"].as<String>() : String("OpenEPaperLink/OpenEPaperLink");
    config.env = APconfig.containsKey("env") ? APconfig["env"].as<String>() : String(STR(BUILD_ENV_NAME));
    if (APconfig["timezone"]) {
        strlcpy(config.timeZone, APconfig["timezone"], sizeof(config.timeZone));
    } else {
        strlcpy(config.timeZone, "CET-1CEST,M3.5.0,M10.5.0/3", sizeof(config.timeZone));
    }
}

void saveAPconfig() {
    xSemaphoreTake(fsMutex, portMAX_DELAY);
    fs::File configFile = contentFS->open("/current/apconfig.json", "w");
    DynamicJsonDocument APconfig(500);
    APconfig["channel"] = config.channel;
    APconfig["subghzchannel"] = config.subghzchannel;
    APconfig["alias"] = config.alias;
    APconfig["led"] = config.led;
    APconfig["tft"] = config.tft;
    APconfig["language"] = config.language;
    APconfig["maxsleep"] = config.maxsleep;
    APconfig["stopsleep"] = config.stopsleep;
    APconfig["preview"] = config.preview;
    APconfig["nightlyreboot"] = config.nightlyreboot;
    APconfig["lock"] = config.lock;
    APconfig["wifipower"] = config.wifiPower;
    APconfig["timezone"] = config.timeZone;
    APconfig["sleeptime1"] = config.sleepTime1;
    APconfig["sleeptime2"] = config.sleepTime2;
    APconfig["ble"] = config.ble;
    APconfig["repo"] = config.repo;
    APconfig["env"] = config.env;
    APconfig["discovery"] = config.discovery;
    serializeJsonPretty(APconfig, configFile);
    configFile.close();
    xSemaphoreGive(fsMutex);
}

HwType getHwType(const uint8_t id) {
    auto it = hwdata.find(id);
    if (it != hwdata.end()) {
        return it->second;
    } else {
        char filename[20];
        snprintf(filename, sizeof(filename), "/tagtypes/%02X.json", id);
        Serial.printf("read %s\r\n", filename);
        File jsonFile = contentFS->open(filename, "r");

        if (jsonFile) {
            StaticJsonDocument<150> filter;
            filter["width"] = true;
            filter["height"] = true;
            filter["rotatebuffer"] = true;
            filter["bpp"] = true;
            filter["shortlut"] = true;
            filter["zlib_compression"] = true;
            filter["g5_compression"] = true;
            filter["highlight_color"] = true;
            filter["colortable"] = true;
            StaticJsonDocument<1000> doc;
            DeserializationError error = deserializeJson(doc, jsonFile, DeserializationOption::Filter(filter));
            jsonFile.close();
            if (error) {
                Serial.println("json error in " + String(filename));
                Serial.println(error.c_str());
            } else {
                HwType& hwType = hwdata[id];
                hwType.id = id;
                hwType.width = doc["width"];
                hwType.height = doc["height"];
                hwType.rotatebuffer = doc["rotatebuffer"];
                hwType.bpp = doc["bpp"];
                hwType.shortlut = doc["shortlut"];
                if (doc.containsKey("zlib_compression")) {
                    hwType.zlib = strtol(doc["zlib_compression"], nullptr, 16);
                } else {
                    hwType.zlib = 0;
                }
                if (doc.containsKey("g5_compression")) {
                    hwType.g5 = strtol(doc["g5_compression"], nullptr, 16);
                } else {
                    hwType.g5 = 0;
                }
                hwType.highlightColor = doc.containsKey("highlight_color") ? doc["highlight_color"].as<uint16_t>() : 2;
                JsonObject colorTable = doc["colortable"];
                for (auto kv : colorTable) {
                    JsonArray color = kv.value();
                    Color c;
                    c.r = color[0];
                    c.g = color[1];
                    c.b = color[2];
                    hwType.colortable.push_back(c);
                }
                return hwdata.at(id);
            }
        }
        return {0, 0, 0, 0, 0, 0, 0};
    }
}

bool setVarDB(const std::string& key, const String& value, const bool notify) {
    auto it = varDB.find(key);
    if (it == varDB.end()) {
        varStruct newVar;
        newVar.value = value;
        newVar.changed = notify;
        varDB[key] = newVar;
        return true;
    }

    if (it->second.value != value) {
        it->second.value = value;
        it->second.changed = notify;
        return true;
    } else {
        return false;
    }
}

String getBaseName(const String& filename) {
    // int lastDotIndex = filename.lastIndexOf('.');
    // return lastDotIndex != -1 ? filename.substring(0, lastDotIndex) : filename;
    return filename.substring(0, 16);
}

void cleanupCurrent() {
    // clean unknown previews
    Serial.println("Cleaning up temporary files");
    File dir = contentFS->open("/current");
    File file = dir.openNextFile();
    while (file) {
        String filename = file.name();
        uint8_t mac[8];
        if (hex2mac(getBaseName(filename), mac)) {
            bool found = false;
            for (tagRecord* record : tagDB) {
                if (memcmp(record->mac, mac, 8) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found || filename.endsWith(".pending")) {
                filename = file.path();
                file.close();
                Serial.println("remove " + filename);
                contentFS->remove(filename);
            }
        }
        file = dir.openNextFile();
    }
    dir.close();

    dir = contentFS->open("/temp");
    file = dir.openNextFile();
    while (file) {
        String filename = file.name();
        filename = file.path();
        file.close();
        contentFS->remove(filename);
        file = dir.openNextFile();
    }
    dir.close();
}

void pushTagInfo(tagRecord* taginfo) {
    tagRecord* taginfo2 = new tagRecord(*taginfo);
    taginfo2->version = 1;
    tagDB.push_back(taginfo2);
}

void popTagInfo(const uint8_t mac[8]) {
    for (tagRecord* tag : tagDB) {
        if (memcmp(tag->mac, mac, 8) == 0 && tag->version == 1) {
            deleteRecord(mac, false);
            tag->version = 0;
        }
    }
}
