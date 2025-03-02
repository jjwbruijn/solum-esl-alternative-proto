#include "contentmanager.h"

// possibility to turn off, to save space if needed
#ifndef SAVE_SPACE
#define CONTENT_QR
#define CONTENT_RSS
#define CONTENT_BIGCAL
#define CONTENT_NFCLUT
#define CONTENT_DAYAHEAD
#define CONTENT_TIMESTAMP
#define CONTENT_BUIENRADAR
#define CONTENT_CAL
#endif
#define CONTENT_TAGCFG

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <MD5Builder.h>
#include <locale.h>
#ifdef CONTENT_RSS
#include <rssClass.h>
#endif
#include <TJpg_Decoder.h>
#include <time.h>

#include <map>

#include "commstructs.h"
#include "makeimage.h"
#include "newproto.h"
#include "storage.h"
#ifdef CONTENT_QR
#include "QRCodeGenerator.h"
#endif
#include "language.h"
#include "settings.h"
#include "system.h"
#include "tag_db.h"
#include "truetype.h"
#include "util.h"
#include "web.h"

// https://csvjson.com/json_beautifier

bool needRedraw(uint8_t contentMode, uint8_t wakeupReason) {
    // contentmode 26, timestamp
    if ((wakeupReason == WAKEUP_REASON_BUTTON1 || wakeupReason == WAKEUP_REASON_BUTTON2) && contentMode == 26) return true;
    return false;
}

void contentRunner() {
    if (config.runStatus == RUNSTATUS_STOP) return;

    time_t now;
    time(&now);

    for (tagRecord *taginfo : tagDB) {
        if (taginfo->RSSI &&
            (now >= taginfo->nextupdate || needRedraw(taginfo->contentMode, taginfo->wakeupReason)) &&
            config.runStatus == RUNSTATUS_RUN &&
            Storage.freeSpace() > 31000 && !util::isSleeping(config.sleepTime1, config.sleepTime2)) {
            drawNew(taginfo->mac, taginfo);
            taginfo->wakeupReason = 0;
        }

        if (taginfo->expectedNextCheckin > now - 10 && taginfo->expectedNextCheckin < now + 30 && taginfo->pendingIdle == 0 && taginfo->pendingCount == 0) {
            int32_t minutesUntilNextUpdate = (taginfo->nextupdate - now) / 60;
            if (minutesUntilNextUpdate > config.maxsleep) {
                minutesUntilNextUpdate = config.maxsleep;
            }
            if (util::isSleeping(config.sleepTime1, config.sleepTime2)) {
                struct tm timeinfo;
                getLocalTime(&timeinfo);
                struct tm nextSleepTimeinfo = timeinfo;
                nextSleepTimeinfo.tm_hour = config.sleepTime2;
                nextSleepTimeinfo.tm_min = 0;
                nextSleepTimeinfo.tm_sec = 0;
                time_t nextWakeTime = mktime(&nextSleepTimeinfo);
                if (nextWakeTime < now) nextWakeTime += 24 * 3600;
                minutesUntilNextUpdate = (nextWakeTime - now) / 60 - 2;
            }
            if (minutesUntilNextUpdate > 1 && (wsClientCount() == 0 || config.stopsleep == 0)) {
                taginfo->pendingIdle = minutesUntilNextUpdate * 60;
                if (taginfo->isExternal == false) {
                    prepareIdleReq(taginfo->mac, minutesUntilNextUpdate);
                }
            }
        }

        vTaskDelay(1 / portTICK_PERIOD_MS);  // add a small delay to allow other threads to run
    }
}

void checkVars() {
    JsonDocument cfgobj;
    for (tagRecord *tag : tagDB) {
        if (tag->contentMode == 19) {
            deserializeJson(cfgobj, tag->modeConfigJson);
            const String jsonfile = cfgobj["filename"].as<String>();
            if (!util::isEmptyOrNull(jsonfile)) {
                File file = contentFS->open(jsonfile, "r");
                if (file) {
                    const size_t fileSize = file.size();
                    std::unique_ptr<char[]> fileContent(new char[fileSize + 1]);
                    file.readBytes(fileContent.get(), fileSize);
                    file.close();
                    fileContent[fileSize] = '\0';
                    const char *contentPtr = fileContent.get();
                    for (const auto &entry : varDB) {
                        if (entry.second.changed && strstr(contentPtr, entry.first.c_str()) != nullptr) {
                            Serial.println("updating " + jsonfile + " because of var " + entry.first.c_str());
                            tag->nextupdate = 0;
                        }
                    }
                }
                file.close();
            }
        }
        if (tag->contentMode == 21) {
            if (varDB["ap_tagcount"].changed || varDB["ap_ip"].changed || varDB["ap_ch"].changed) {
                tag->nextupdate = 0;
            }
        }
    }
    for (const auto &entry : varDB) {
        if (entry.second.changed) varDB[entry.first].changed = false;
    }
}

/// @brief Draw a counter
/// @param mac Destination mac
/// @param taginfo Tag information
/// @param cfgobj Tag config as json object
/// @param filename Filename
/// @param imageParams Image parameters
/// @param nextupdate Next counter update
/// @param nextCheckin Next tag checkin
void drawCounter(const uint8_t mac[8], tagRecord *&taginfo, JsonObject &cfgobj, String &filename, imgParam &imageParams, const uint32_t nextupdate, const uint16_t nextCheckin) {
    int32_t counter = cfgobj["counter"].as<int32_t>();
    bool buttonPressed = false;
    if (buttonPressed) {  // fixme: button pressed
        counter = 0;
    }
    drawNumber(filename, counter, (int32_t)cfgobj["thresholdred"], taginfo, imageParams);
    taginfo->nextupdate = nextupdate;
    updateTagImage(filename, mac, (buttonPressed ? 0 : nextCheckin), taginfo, imageParams);
    cfgobj["counter"] = counter + 1;
}

void drawNew(const uint8_t mac[8], tagRecord *&taginfo) {
    time_t now;
    time(&now);

    const HwType hwdata = getHwType(taginfo->hwType);
    if (hwdata.bpp == 0) {
        taginfo->nextupdate = now + 300;
        Serial.println("No definition found for tag type " + String(taginfo->hwType));
        return;
    }

    uint8_t wifimac[8];
    WiFi.macAddress(wifimac);
    memset(&wifimac[6], 0, 2);

    const bool isAp = memcmp(mac, wifimac, 8) == 0;
    if ((taginfo->wakeupReason == WAKEUP_REASON_FIRSTBOOT || taginfo->wakeupReason == WAKEUP_REASON_WDT_RESET) && taginfo->contentMode == 0) {
        if (isAp) {
            taginfo->contentMode = 21;
            taginfo->nextupdate = 0;
        } else if (contentFS->exists("/tag_defaults.json")) {
            JsonDocument doc;
            fs::File tagDefaults = contentFS->open("/tag_defaults.json", "r");
            DeserializationError err = deserializeJson(doc, tagDefaults);
            if (!err) {
                if (doc["contentMode"].is<uint8_t>()) {
                    taginfo->contentMode = doc["contentMode"];
                }
                if (doc["modecfgjson"].is<String>()) {
                    taginfo->modeConfigJson = doc["modecfgjson"].as<String>();
                }
            }
            tagDefaults.close();
        }
    }

    char hexmac[17];
    mac2hex(mac, hexmac);
    String filename = "/temp/" + String(hexmac) + ".raw";
#ifdef HAS_TFT
    if (isAp) {
        filename = "direct";
    }
#endif

    JsonDocument doc;
    deserializeJson(doc, taginfo->modeConfigJson);
    JsonObject cfgobj = doc.as<JsonObject>();
    char buffer[64];

    wsLog("Updating " + String(hexmac));
    taginfo->nextupdate = now + 60;

    imgParam imageParams;
    imageParams.hwdata = hwdata;
    imageParams.width = hwdata.width;
    imageParams.height = hwdata.height;
    imageParams.bpp = hwdata.bpp;
    imageParams.rotatebuffer = hwdata.rotatebuffer;
    imageParams.shortlut = hwdata.shortlut;
    imageParams.highlightColor = getColor(String(hwdata.highlightColor));

    imageParams.hasRed = false;
    imageParams.dataType = DATATYPE_IMG_RAW_1BPP;
    imageParams.dither = 2;

    imageParams.invert = taginfo->invert;
    imageParams.symbols = 0;
    imageParams.rotate = taginfo->rotate;
    if (hwdata.zlib != 0 && taginfo->tagSoftwareVersion >= hwdata.zlib) {
        imageParams.zlib = 1;
    } else {
        imageParams.zlib = 0;
    }
#ifdef SAVE_SPACE
    imageParams.g5 = 0;
#else
    if (hwdata.g5 != 0 && taginfo->tagSoftwareVersion >= hwdata.g5) {
        imageParams.g5 = 1;
    } else {
        imageParams.g5 = 0;
    }
#endif

    imageParams.lut = EPD_LUT_NO_REPEATS;
    if (taginfo->lut == 2) imageParams.lut = EPD_LUT_FAST_NO_REDS;
    if (taginfo->lut == 3) imageParams.lut = EPD_LUT_FAST;
    time_t last_midnight = now - now % (24 * 60 * 60) + 3 * 3600;  // somewhere in the middle of the night
    if (imageParams.shortlut == SHORTLUT_DISABLED || taginfo->lastfullupdate < last_midnight || taginfo->lut == 1) {
        imageParams.lut = EPD_LUT_DEFAULT;
        taginfo->lastfullupdate = now;
    }

    int32_t interval = cfgobj["interval"].as<int>() * 60;
    if (interval == -1440 * 60) {
        interval = util::getMidnightTime() - now;
    } else if (interval < 0) {
        interval = -interval;
        unsigned int secondsUntilNext = (interval - (now % interval)) % interval;
        interval = secondsUntilNext;
    } else if (interval < 180)
        interval = 60 * 60;

    switch (taginfo->contentMode) {
        case 0:   // Not configured
        case 22:  // Static image
        case 23:  // Static image (advanced)
        case 24:  // External image
        case 25:  // Home Assistant
        {
            String configFilename = cfgobj["filename"].as<String>();
            if (!util::isEmptyOrNull(configFilename)) {
                if (!configFilename.startsWith("/")) {
                    configFilename = "/" + configFilename;
                }

                if (!contentFS->exists(configFilename)) {
                    taginfo->nextupdate = 3216153600;
                    wsErr("Not found: " + configFilename);
                    break;
                }

                imageParams.dither = cfgobj["dither"];

                imageParams.preload = cfgobj["preload"] && cfgobj["preload"] == "1";
                imageParams.preloadlut = cfgobj["preload_lut"];
                imageParams.preloadtype = cfgobj["preload_type"];

                jpg2buffer(configFilename, filename, imageParams);

                if (imageParams.hasRed && imageParams.lut == EPD_LUT_NO_REPEATS && imageParams.shortlut == SHORTLUT_ONLY_BLACK) {
                    imageParams.lut = EPD_LUT_DEFAULT;
                }

                if (imageParams.bpp == 3) {
                    imageParams.dataType = DATATYPE_IMG_RAW_3BPP;
                    Serial.println("datatype: DATATYPE_IMG_RAW_3BPP");
                } else if (imageParams.bpp == 4) {
                    imageParams.dataType = DATATYPE_IMG_RAW_4BPP;
                    Serial.println("datatype: DATATYPE_IMG_RAW_4BPP");
                } else if (imageParams.zlib) {
                    imageParams.dataType = DATATYPE_IMG_ZLIB;
                    Serial.println("datatype: DATATYPE_IMG_ZLIB");
                } else if (imageParams.g5) {
                    imageParams.dataType = DATATYPE_IMG_G5;
                    Serial.println("datatype: DATATYPE_IMG_G5");
                } else if (imageParams.hasRed) {
                    imageParams.dataType = DATATYPE_IMG_RAW_2BPP;
                    Serial.println("datatype: DATATYPE_IMG_RAW_2BPP");
                } else {
                    Serial.println("datatype: DATATYPE_IMG_RAW_1BPP");
                }

                struct imageDataTypeArgStruct arg = {0};
                // load parameters in case we do need to preload an image
                if (imageParams.preload) {
                    arg.preloadImage = 1;
                    arg.specialType = imageParams.preloadtype;
                    arg.lut = imageParams.preloadlut;
                } else {
                    arg.lut = imageParams.lut & 0x03;
                }

                if (prepareDataAvail(filename, imageParams.dataType, *((uint8_t *)&arg), mac, cfgobj["timetolive"].as<int>())) {
                    if (cfgobj["delete"].as<String>() == "1") {
                        contentFS->remove("/" + configFilename);
                    }
                } else {
                    wsErr("Error accessing " + filename);
                }
            } else {
                // configfilename is empty. Probably the tag needs to redisplay the image after a reboot.
                Serial.println("Resend static image");
                // fixme: doesn't work yet
                // prepareDataAvail(mac);
            }
            taginfo->nextupdate = 3216153600;
        } break;

        case 1:  // Today

            drawDate(filename, taginfo, imageParams);
            taginfo->nextupdate = util::getMidnightTime();
            updateTagImage(filename, mac, (taginfo->nextupdate - now) / 60 - 10, taginfo, imageParams);
            break;

        case 2:  // CountDays
            drawCounter(mac, taginfo, cfgobj, filename, imageParams, util::getMidnightTime(), 15);
            break;

        case 3:  // CountHours
            drawCounter(mac, taginfo, cfgobj, filename, imageParams, now + 3600, 5);
            break;

        case 4:  // Weather

            // https://open-meteo.com/
            // https://geocoding-api.open-meteo.com/v1/search?name=eindhoven
            // https://api.open-meteo.com/v1/forecast?latitude=52.52&longitude=13.41&current_weather=true
            // https://github.com/erikflowers/weather-icons

            drawWeather(filename, cfgobj, taginfo, imageParams);
            taginfo->nextupdate = now + interval;
            updateTagImage(filename, mac, interval / 60, taginfo, imageParams);
            break;

        case 8:  // Forecast

            drawForecast(filename, cfgobj, taginfo, imageParams);
            taginfo->nextupdate = now + interval;
            updateTagImage(filename, mac, interval / 60, taginfo, imageParams);
            break;

        case 5:  // Firmware

            filename = cfgobj["filename"].as<String>();
            if (!util::isEmptyOrNull(filename) && !cfgobj["#fetched"].as<bool>()) {
                File file = contentFS->open(filename, "r");
                if (file) {
                    if (file.find("<html")) {
                        file.close();
                        wsErr("User error flashing tag firmware: this is a html-file!");
                        cfgobj["#fetched"] = true;
                    } else {
                        file.close();
                        if (prepareDataAvail(filename, DATATYPE_FW_UPDATE, 0, mac, cfgobj["timetolive"].as<int>())) {
                            cfgobj["#fetched"] = true;
                        }
                    }
                }
                taginfo->nextupdate = 3216153600;
            } else {
                taginfo->nextupdate = now + 300;
            }
            break;

        case 7:  // ImageUrl

        {
            const int httpcode = getImgURL(filename, cfgobj["url"], (time_t)cfgobj["#fetched"], imageParams, String(hexmac));
            if (httpcode == 200) {
                taginfo->nextupdate = now + interval;
                updateTagImage(filename, mac, interval / 60, taginfo, imageParams);
                cfgobj["#fetched"] = now;
            } else if (httpcode == 304) {
                taginfo->nextupdate = now + interval;
            } else {
                taginfo->nextupdate = now + 300;
            }
            break;
        }

#ifdef CONTENT_RSS
        case 9:  // RSSFeed

            if (getRssFeed(filename, cfgobj["url"], cfgobj["title"], taginfo, imageParams)) {
                taginfo->nextupdate = now + interval;
                updateTagImage(filename, mac, interval / 60, taginfo, imageParams);
            } else {
                taginfo->nextupdate = now + 300;
            }
            break;
#endif

#ifdef CONTENT_QR
        case 10:  // QRcode:

            drawQR(filename, cfgobj["qr-content"], cfgobj["title"], taginfo, imageParams);
            taginfo->nextupdate = now + 12 * 3600;
            updateTagImage(filename, mac, 0, taginfo, imageParams);
            break;
#endif

#ifdef CONTENT_CAL
        case 11:  // Calendar:

            if (getCalFeed(filename, cfgobj, taginfo, imageParams)) {
                taginfo->nextupdate = now + interval;
                updateTagImage(filename, mac, interval / 60, taginfo, imageParams);
            } else {
                taginfo->nextupdate = now + 300;
            }
            break;
#endif

        case 12:  // RemoteAP

            taginfo->nextupdate = 3216153600;
            break;

        case 13:  // SegStatic

            sprintf(buffer, "%-4.4s%-2.2s%-4.4s", cfgobj["line1"].as<const char *>(), cfgobj["line2"].as<const char *>(), cfgobj["line3"].as<const char *>());
            taginfo->nextupdate = 3216153600;
            sendAPSegmentedData(mac, (String)buffer, 0x0000, false, (taginfo->isExternal == false));
            break;

#ifdef CONTENT_NFCLUT
        case 14:  // NFC URL

            taginfo->nextupdate = 3216153600;
            prepareNFCReq(mac, cfgobj["url"].as<const char *>());
            break;
#endif

#ifdef CONTENT_BUIENRADAR
        case 16:  // buienradar

        {
            const uint8_t refresh = drawBuienradar(filename, cfgobj, taginfo, imageParams);
            taginfo->nextupdate = now + refresh * 60;
            updateTagImage(filename, mac, refresh, taginfo, imageParams);
            break;
        }
#endif

        case 17:  // tag command
        {
            sendTagCommand(mac, cfgobj["cmd"].as<int>(), (taginfo->isExternal == false));
            taginfo->nextupdate = 3216153600;
            break;
        }

#ifdef CONTENT_TAGCFG
        case 18:  // tag config
        {
            prepareConfigFile(mac, cfgobj);
            taginfo->nextupdate = 3216153600;
            break;
        }
#endif

        case 19:  // json template
        {
            const String configFilename = cfgobj["filename"].as<String>();
            if (!util::isEmptyOrNull(configFilename)) {
                String configUrl = cfgobj["url"].as<String>();
                if (!util::isEmptyOrNull(configUrl)) {
                    JsonDocument json;
                    Serial.println("Get json url + file");

                    int index = configUrl.indexOf("{mac}");
                    if (index != -1) {
                        char macStr[17];
                        mac2hex(mac, macStr);
                        configUrl.replace("{mac}", macStr);
                    }

                    if (util::httpGetJson(configUrl, json, 1000)) {
                        taginfo->nextupdate = now + interval;
                        if (getJsonTemplateFileExtractVariables(filename, configFilename, json, taginfo, imageParams)) {
                            updateTagImage(filename, mac, interval / 60, taginfo, imageParams);
                        } else {
                            wsErr("error opening file " + configFilename);
                        }
                    } else {
                        taginfo->nextupdate = now + 600;
                    }

                } else {
                    const bool result = getJsonTemplateFile(filename, configFilename, taginfo, imageParams);
                    if (result) {
                        updateTagImage(filename, mac, interval, taginfo, imageParams);
                    } else {
                        wsErr("error opening file " + configFilename);
                    }
                    taginfo->nextupdate = 3216153600;
                }
            } else {
                const int httpcode = getJsonTemplateUrl(filename, cfgobj["url"], (time_t)cfgobj["#fetched"], String(hexmac), taginfo, imageParams);
                if (httpcode == 200) {
                    taginfo->nextupdate = now + interval;
                    updateTagImage(filename, mac, interval / 60, taginfo, imageParams);
                    cfgobj["#fetched"] = now;
                } else if (httpcode == 304) {
                    taginfo->nextupdate = now + interval;
                } else {
                    taginfo->nextupdate = now + 600;
                }
            }
            break;
        }

        case 20:  // display a copy
            break;

        case 21:  // ap info
            drawAPinfo(filename, cfgobj, taginfo, imageParams);
            updateTagImage(filename, mac, 0, taginfo, imageParams);
            taginfo->nextupdate = 3216153600;
            break;

#ifdef CONTENT_TIMESTAMP
        case 26:  // timestamp
            taginfo->nextupdate = 3216153600;
            drawTimestamp(filename, cfgobj, taginfo, imageParams);
            updateTagImage(filename, mac, 0, taginfo, imageParams);
            break;
#endif

#ifdef CONTENT_DAYAHEAD
        case 27:  // Day Ahead:

            if (getDayAheadFeed(filename, cfgobj, taginfo, imageParams)) {
                taginfo->nextupdate = now + (3600 - now % 3600);
                updateTagImage(filename, mac, 0, taginfo, imageParams);
            } else {
                taginfo->nextupdate = now + 300;
            }
            break;
#endif
        case 28:  // tag command
        {
            uint64_t newmac;
            sscanf(cfgobj["mac"].as<String>().c_str(), "%llx", &newmac);
            sendTagMac(mac, newmac, (taginfo->isExternal == false));
            taginfo->nextupdate = 3216153600;
            break;
        }
    }

    taginfo->modeConfigJson = doc.as<String>();
}

bool updateTagImage(String &filename, const uint8_t *dst, uint16_t nextCheckin, tagRecord *&taginfo, imgParam &imageParams) {
    if (taginfo->hwType == SOLUM_SEG_UK) {
        sendAPSegmentedData(dst, (String)imageParams.segments, imageParams.symbols, (imageParams.invert == 1), (taginfo->isExternal == false));
    } else {
        if (imageParams.hasRed && imageParams.lut == EPD_LUT_NO_REPEATS && imageParams.shortlut == SHORTLUT_ONLY_BLACK) {
            imageParams.lut = EPD_LUT_DEFAULT;
        }

        if (imageParams.bpp == 3) {
            imageParams.dataType = DATATYPE_IMG_RAW_3BPP;
            Serial.println("datatype: DATATYPE_IMG_RAW_3BPP");
        } else if (imageParams.bpp == 4) {
            imageParams.dataType = DATATYPE_IMG_RAW_4BPP;
            Serial.println("datatype: DATATYPE_IMG_RAW_4BPP");
        } else if (imageParams.zlib) {
            imageParams.dataType = DATATYPE_IMG_ZLIB;
            Serial.println("datatype: DATATYPE_IMG_ZLIB");
        } else if (imageParams.g5) {
            imageParams.dataType = DATATYPE_IMG_G5;
            Serial.println("datatype: DATATYPE_IMG_G5");
        } else if (imageParams.hasRed) {
            imageParams.dataType = DATATYPE_IMG_RAW_2BPP;
            Serial.println("datatype: DATATYPE_IMG_RAW_2BPP");
        }
        if (nextCheckin > 0x7fff) nextCheckin = 0;
        prepareDataAvail(filename, imageParams.dataType, imageParams.lut, dst, nextCheckin);
    }
    return true;
}

uint8_t processFontPath(String &font) {
    if (font == "") return 3;
    if (font == "glasstown_nbp_tf") return 1;
    if (font == "7x14_tf") return 1;
    if (font == "t0_14b_tf") return 1;
    if (font.indexOf('/') == -1) font = "/fonts/" + font;
    if (!font.startsWith("/")) font = "/" + font;
    if (font.endsWith(".vlw")) font = font.substring(0, font.length() - 4);
    if (font.endsWith(".ttf")) return 2;
    return 3;
}

void replaceVariables(String &format) {
    size_t startIndex = 0;
    size_t openBraceIndex, closeBraceIndex;

    time_t now;
    time(&now);
    struct tm timedef;
    localtime_r(&now, &timedef);
    char timeBuffer[80];
    strftime(timeBuffer, sizeof(timeBuffer), "%H:%M:%S", &timedef);
    setVarDB("ap_time", timeBuffer, false);

    while ((openBraceIndex = format.indexOf('{', startIndex)) != -1 &&
           (closeBraceIndex = format.indexOf('}', openBraceIndex + 1)) != -1) {
        const std::string variableName = format.substring(openBraceIndex + 1, closeBraceIndex).c_str();
        const std::string varKey = "{" + variableName + "}";
        const auto var = varDB.find(variableName);
        if (var != varDB.end()) {
            format.replace(varKey.c_str(), var->second.value);
        } else {
            format.replace(varKey.c_str(), "-");
        }
        startIndex = closeBraceIndex + 1;
    }
}

void drawString(TFT_eSprite &spr, String content, int16_t posx, int16_t posy, String font, byte align, uint16_t color, uint16_t size, uint16_t bgcolor) {
    // drawString(spr,"test",100,10,"bahnschrift30",TC_DATUM,TFT_RED);

    // backwards compitibility
    replaceVariables(content);
    if (font.startsWith("fonts/calibrib")) {
        String numericValueStr = font.substring(14);
        int calibriSize = numericValueStr.toInt();
        if (calibriSize != 30 && calibriSize != 16) {
            font = "Signika-SB.ttf";
            size = calibriSize;
        }
    }
    if (font == "glasstown_nbp_tf") {
        font = "tahoma9.vlw";
        posy -= 8;
    }
    if (font == "7x14_tf") {
        font = "REFSAN12.vlw";
        posy -= 10;
    }
    if (font == "t0_14b_tf") {
        font = "calibrib16.vlw";
        posy -= 11;
    }

    switch (processFontPath(font)) {
        case 2: {
            // truetype
            time_t t = millis();
            truetypeClass truetype = truetypeClass();
            void *framebuffer = spr.getPointer();
            truetype.setFramebuffer(spr.width(), spr.height(), spr.getColorDepth(), static_cast<uint8_t *>(framebuffer));
            File fontFile = contentFS->open(font, "r");
            if (!truetype.setTtfFile(fontFile)) {
                Serial.println("read ttf failed");
                return;
            }

            truetype.setCharacterSize(size);
            truetype.setCharacterSpacing(0);
            if (align == TC_DATUM) {
                posx -= truetype.getStringWidth(content) / 2;
            }
            if (align == TR_DATUM) {
                posx -= truetype.getStringWidth(content);
            }
            truetype.setTextBoundary(posx, spr.width(), spr.height());
            if (spr.getColorDepth() == 8) {
                truetype.setTextColor(spr.color16to8(color), spr.color16to8(color));
            } else {
                truetype.setTextColor(color, color);
            }
            truetype.textDraw(posx, posy, content);
            truetype.end();
        } break;
        case 3: {
            // vlw bitmap font
            spr.setTextDatum(align);
            if (font != "") spr.loadFont(font.substring(1), *contentFS);
            spr.setTextColor(color, bgcolor);
            spr.setTextWrap(false, false);
            spr.drawString(content, posx, posy);
            if (font != "") spr.unloadFont();
        }
    }
}

void drawTextBox(TFT_eSprite &spr, String &content, int16_t &posx, int16_t &posy, int16_t boxwidth, int16_t boxheight, String font, uint16_t color, uint16_t bgcolor, float lineheight, byte align) {
    replaceVariables(content);
    switch (processFontPath(font)) {
        case 2: {
            // truetype
            Serial.println("truetype font not implemented for drawTextBox");
        } break;
        case 3: {
            // vlw bitmap font
            // spr.drawRect(posx, posy, boxwidth, boxheight, TFT_BLACK);
            spr.setTextDatum(align);
            if (font != "") spr.loadFont(font.substring(1), *contentFS);
            spr.setTextWrap(false, false);
            spr.setTextColor(color, bgcolor);

            int length = content.length();
            int startPos = 0;
            int startPosY = posy;

            while (startPos < length && posy + spr.gFont.yAdvance <= startPosY + boxheight) {
                int endPos = startPos;
                bool hasspace = false;

                while (endPos < length && spr.textWidth(content.substring(startPos, endPos + 1).c_str()) <= boxwidth && content.charAt(endPos) != '\n') {
                    if (content.charAt(endPos) == ' ' || content.charAt(endPos) == '-') hasspace = true;
                    endPos++;
                }
                while (endPos < length && endPos > startPos && hasspace == true && content.charAt(endPos - 1) != ' ' && content.charAt(endPos - 1) != '-' && content.charAt(endPos) != '\n') {
                    endPos--;
                }
                spr.drawString(content.substring(startPos, endPos), posx, posy);
                posy += spr.gFont.yAdvance * lineheight;

                if (content.charAt(endPos) == '\n') endPos++;
                startPos = endPos;
                while (startPos < length && content.charAt(startPos) == ' ') {
                    startPos++;
                }
            }
            if (font != "") spr.unloadFont();
        }
    }
}

void initSprite(TFT_eSprite &spr, int w, int h, imgParam &imageParams) {
    spr.setColorDepth(16);
    spr.createSprite(w, h);
    if (spr.getPointer() == nullptr) {
        wsErr("low on memory. Fallback to 8bpp");
        util::printLargestFreeBlock();
        spr.setColorDepth(8);
        spr.createSprite(w, h);
    }
    if (spr.getPointer() == nullptr) {
        wsErr("low on memory. Fallback to 1bpp");
        util::printLargestFreeBlock();
        spr.setColorDepth(1);
        spr.setBitmapColor(TFT_WHITE, TFT_BLACK);
        imageParams.bufferbpp = 1;
        spr.createSprite(w, h);
    }
    if (spr.getPointer() == nullptr) {
        wsErr("Failed to create sprite");
    }
    spr.setRotation(3);
    spr.fillSprite(TFT_WHITE);
}

void drawDate(String &filename, tagRecord *&taginfo, imgParam &imageParams) {
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    const int month_number = timeinfo.tm_mon;
    const int year_number = timeinfo.tm_year + 1900;

    if (taginfo->hwType == SOLUM_SEG_UK) {
        sprintf(imageParams.segments, "%2d%2d%-2.2s%04d", timeinfo.tm_mday, month_number + 1, languageDays[timeinfo.tm_wday], year_number);
        imageParams.symbols = 0x04;
        return;
    }

    JsonDocument loc;
    getTemplate(loc, 1, taginfo->hwType);

    TFT_eSprite spr = TFT_eSprite(&tft);
    initSprite(spr, imageParams.width, imageParams.height, imageParams);

    const auto &date = loc["date"];
    const auto &weekday = loc["weekday"];
    if (date) {
        drawString(spr, languageDays[timeinfo.tm_wday], weekday[0], weekday[1], weekday[2], TC_DATUM, imageParams.highlightColor, weekday[3]);
        drawString(spr, String(timeinfo.tm_mday) + " " + languageMonth[timeinfo.tm_mon], date[0], date[1], date[2], TC_DATUM, TFT_BLACK, date[3]);
    } else {
        const auto &month = loc["month"];
        const auto &day = loc["day"];
        drawString(spr, languageDays[timeinfo.tm_wday], weekday[0], weekday[1], weekday[2], TC_DATUM, TFT_BLACK, weekday[3]);
        drawString(spr, String(languageMonth[timeinfo.tm_mon]), month[0], month[1], month[2], TC_DATUM, TFT_BLACK, month[3]);
        drawString(spr, String(timeinfo.tm_mday), day[0], day[1], day[2], TC_DATUM, imageParams.highlightColor, day[3]);
    }

    spr2buffer(spr, filename, imageParams);
    spr.deleteSprite();
}

void drawNumber(String &filename, int32_t count, int32_t thresholdred, tagRecord *&taginfo, imgParam &imageParams) {
    int32_t countTemp = count;
    count = abs(count);
    if (taginfo->hwType == SOLUM_SEG_UK) {
        imageParams.symbols = 0x00;
        if (count > 19999) {
            sprintf(imageParams.segments, "over  flow");
            return;
        } else if (count > 9999) {
            imageParams.symbols = 0x02;
            sprintf(imageParams.segments, "%04d", count - 10000);
        } else {
            sprintf(imageParams.segments, "%4d", count);
        }
        if (taginfo->contentMode == 3) {
            strcat(imageParams.segments, "  hour");
        } else {
            strcat(imageParams.segments, "  days");
        }
        return;
    }

    TFT_eSprite spr = TFT_eSprite(&tft);

    JsonDocument loc;
    getTemplate(loc, 2, taginfo->hwType);

    initSprite(spr, imageParams.width, imageParams.height, imageParams);
    uint16_t color = TFT_BLACK;
    if (countTemp > thresholdred) {
        color = imageParams.highlightColor;
    }
    String font = loc["fonts"][0].as<String>();
    uint8_t size = loc["fonts"][1].as<uint16_t>();
    if (count > 9) size = loc["fonts"][2].as<uint16_t>();
    if (count > 99) size = loc["fonts"][3].as<uint16_t>();
    if (count > 999) size = loc["fonts"][4].as<uint16_t>();
    if (count > 9999) size = loc["fonts"][5].as<uint16_t>();
    if (count > 99999) size = loc["fonts"][6].as<uint16_t>();
    drawString(spr, String(count), loc["xy"][0].as<uint16_t>(), loc["xy"][1].as<uint16_t>() - size / 1.8, font, TC_DATUM, color, size);

    spr2buffer(spr, filename, imageParams);
    spr.deleteSprite();
}

/// @brief Get a weather icon
/// @param id Icon identifier/index
/// @param isNight Use night icons (true) or not (false)
/// @return String reference to icon
const String getWeatherIcon(const uint8_t id, const bool isNight = false) {
    const String weatherIcons[] = {"\uf00d", "\uf00c", "\uf002", "\uf013", "\uf013", "\uf014", "", "", "\uf014", "", "",
                                   "\uf01a", "", "\uf01a", "", "\uf01a", "\uf017", "\uf017", "", "", "",
                                   "\uf019", "", "\uf019", "", "\uf019", "\uf015", "\uf015", "", "", "",
                                   "\uf01b", "", "\uf01b", "", "\uf01b", "", "\uf076", "", "", "\uf01a",
                                   "\uf01a", "\uf01a", "", "", "\uf064", "\uf064", "", "", "", "",
                                   "", "", "", "", "\uf01e", "\uf01d", "", "", "\uf01e"};
    if (isNight && id <= 2) {
        const String nightIcons[] = {"\uf02e", "\uf083", "\uf086"};
        return nightIcons[id];
    }
    return weatherIcons[id];
}

void drawWeather(String &filename, JsonObject &cfgobj, const tagRecord *taginfo, imgParam &imageParams) {
    wsLog("get weather");

    getLocation(cfgobj);

    const String lat = cfgobj["#lat"];
    const String lon = cfgobj["#lon"];
    const String tz = cfgobj["#tz"];
    String units = "";
    if (cfgobj["units"] == "1") {
        units += "&temperature_unit=fahrenheit&windspeed_unit=mph&precipitation_unit=inch";
    }

    JsonDocument doc;
    const bool success = util::httpGetJson("https://api.open-meteo.com/v1/forecast?latitude=" + lat + "&longitude=" + lon + "&current_weather=true&windspeed_unit=ms&timezone=" + tz + units, doc, 5000);
    if (!success) {
        return;
    }

    const auto &currentWeather = doc["current_weather"];
    const double temperature = currentWeather["temperature"].as<double>();
    float windspeed = currentWeather["windspeed"].as<float>();
    int windval = 0;
    const int winddirection = currentWeather["winddirection"].as<int>();
    const bool isNight = currentWeather["is_day"].as<int>() == 0;
    uint8_t weathercode = currentWeather["weathercode"].as<int>();
    if (weathercode > 40) weathercode -= 40;

    const uint8_t beaufort = windSpeedToBeaufort(windspeed);
    if (cfgobj["units"] != "1") {
        windval = beaufort;
    } else {
        windval = int(windspeed);
    }

    doc.clear();

    if (taginfo->hwType == SOLUM_SEG_UK) {
        const String weatherText[] = {"sun", "sun", "sun", "CLDY", "CLDY", "FOG", "", "", "FOG", "", "",
                                      "DRZL", "", "DRZL", "", "DRZL", "ice", "ice", "", "", "",
                                      "rain", "", "rain", "", "rain", "ice", "ice", "", "", "",
                                      "SNOW", "", "SNOW", "", "SNOW", "", "SNOW", "", "", "rain",
                                      "rain", "rain", "", "", "SNOW", "SNOW", "", "", "", "",
                                      "", "", "", "", "STRM", "HAIL", "", "", "HAIL"};
        if (temperature < -9.9) {
            sprintf(imageParams.segments, "%3d^%2d%-4.4s", static_cast<int>(temperature), windval, weatherText[weathercode].c_str());
            imageParams.symbols = 0x00;
        } else {
            sprintf(imageParams.segments, "%3d^%2d%-4.4s", static_cast<int>(temperature * 10), windval, weatherText[weathercode].c_str());
            imageParams.symbols = 0x04;
        }
        return;
    }

    getTemplate(doc, 4, taginfo->hwType);

    TFT_eSprite spr = TFT_eSprite(&tft);
    tft.setTextWrap(false, false);

    initSprite(spr, imageParams.width, imageParams.height, imageParams);
    const auto &location = doc["location"];
    drawString(spr, cfgobj["location"], location[0], location[1], location[2]);
    const auto &wind = doc["wind"];
    drawString(spr, String(windval), wind[0], wind[1], wind[2], TR_DATUM, (beaufort > 4 ? imageParams.highlightColor : TFT_BLACK));

    char tmpOutput[5];
    dtostrf(temperature, 2, 1, tmpOutput);
    const auto &temp = doc["temp"];
    drawString(spr, String(tmpOutput), temp[0], temp[1], temp[2], TL_DATUM, (temperature < 0 ? imageParams.highlightColor : TFT_BLACK));

    const int iconcolor = (weathercode == 55 || weathercode == 65 || weathercode == 75 || weathercode == 82 || weathercode == 86 || weathercode == 95 || weathercode == 96 || weathercode == 99)
                              ? imageParams.highlightColor
                              : TFT_BLACK;
    const auto &icon = doc["icon"];
    drawString(spr, getWeatherIcon(weathercode, isNight), icon[0], icon[1], "/fonts/weathericons.ttf", icon[3], iconcolor, icon[2]);
    const auto &dir = doc["dir"];
    drawString(spr, windDirectionIcon(winddirection), dir[0], dir[1], "/fonts/weathericons.ttf", TC_DATUM, TFT_BLACK, dir[2]);
    if (weathercode > 10) {
        const auto &umbrella = doc["umbrella"];
        drawString(spr, "\uf084", umbrella[0], umbrella[1], "/fonts/weathericons.ttf", TC_DATUM, imageParams.highlightColor, umbrella[2]);
    }

    spr2buffer(spr, filename, imageParams);
    spr.deleteSprite();
}

void drawForecast(String &filename, JsonObject &cfgobj, const tagRecord *taginfo, imgParam &imageParams) {
    wsLog("get weather");
    getLocation(cfgobj);

    String lat = cfgobj["#lat"];
    String lon = cfgobj["#lon"];
    String tz = cfgobj["#tz"];
    String units = "";
    if (cfgobj["units"] == "1") {
        units += "&temperature_unit=fahrenheit&windspeed_unit=mph&precipitation_unit=inch";
    }

    JsonDocument doc;
    const bool success = util::httpGetJson("https://api.open-meteo.com/v1/forecast?latitude=" + lat + "&longitude=" + lon + "&daily=weathercode,temperature_2m_max,temperature_2m_min,precipitation_sum,windspeed_10m_max,winddirection_10m_dominant&windspeed_unit=ms&timeformat=unixtime&timezone=" + tz + units, doc, 5000);
    if (!success) {
        return;
    }

    TFT_eSprite spr = TFT_eSprite(&tft);
    tft.setTextWrap(false, false);

    JsonDocument loc;
    getTemplate(loc, 8, taginfo->hwType);
    initSprite(spr, imageParams.width, imageParams.height, imageParams);

    const auto &location = loc["location"];
    drawString(spr, cfgobj["location"], location[0], location[1], location[2], TL_DATUM, TFT_BLACK);
    const auto &daily = doc["daily"];
    const auto &column = loc["column"];
    const int column1 = column[1].as<int>();
    const auto &day = loc["day"];
    const unsigned long utc_offset = doc["utc_offset_seconds"];
    for (uint8_t dag = 0; dag < column[0]; dag++) {
        const time_t weatherday = (daily["time"][dag].as<time_t>() + utc_offset);
        const struct tm *datum = localtime(&weatherday);

        drawString(spr, String(languageDaysShort[datum->tm_wday]), dag * column1 + day[0].as<int>(), day[1], day[2], TC_DATUM, TFT_BLACK);

        uint8_t weathercode = daily["weathercode"][dag].as<int>();
        if (weathercode > 40) weathercode -= 40;

        const int iconcolor = (weathercode == 55 || weathercode == 65 || weathercode == 75 || weathercode == 82 || weathercode == 86 || weathercode == 95 || weathercode == 96 || weathercode == 99)
                                  ? imageParams.highlightColor
                                  : TFT_BLACK;
        drawString(spr, getWeatherIcon(weathercode), loc["icon"][0].as<int>() + dag * column1, loc["icon"][1], "/fonts/weathericons.ttf", TC_DATUM, iconcolor, loc["icon"][2]);

        drawString(spr, windDirectionIcon(daily["winddirection_10m_dominant"][dag]), loc["wind"][0].as<int>() + dag * column1, loc["wind"][1], "/fonts/weathericons.ttf", TC_DATUM, TFT_BLACK, loc["icon"][2]);

        const int8_t tmin = round(daily["temperature_2m_min"][dag].as<double>());
        const int8_t tmax = round(daily["temperature_2m_max"][dag].as<double>());
        uint8_t wind;
        const int8_t beaufort = windSpeedToBeaufort(daily["windspeed_10m_max"][dag].as<double>());
        if (cfgobj["units"] == "1") {
            wind = daily["windspeed_10m_max"][dag].as<int>();
        } else {
            wind = beaufort;
        }

        if (loc["rain"]) {
            if (cfgobj["units"] == "0") {
                const int8_t rain = round(daily["precipitation_sum"][dag].as<double>());
                if (rain > 0) {
                    drawString(spr, String(rain) + "mm", dag * column1 + loc["rain"][0].as<int>(), loc["rain"][1], day[2], TC_DATUM, (rain > 10 ? imageParams.highlightColor : TFT_BLACK));
                }
            } else {
                double fRain = daily["precipitation_sum"][dag].as<double>();
                fRain = round(fRain * 100.0) / 100.0;
                if (fRain > 0.0) {
                    // inch, display if > .01 inches
                    drawString(spr, String(fRain) + "in", dag * column1 + loc["rain"][0].as<int>(), loc["rain"][1], day[2], TC_DATUM, (fRain > 0.5 ? imageParams.highlightColor : TFT_BLACK));
                }
            }
        }

        drawString(spr, String(tmin) + " ", dag * column1 + day[0].as<int>(), day[4], day[2], TR_DATUM, (tmin < 0 ? imageParams.highlightColor : TFT_BLACK));
        drawString(spr, String(" ") + String(tmax), dag * column1 + day[0].as<int>(), day[4], day[2], TL_DATUM, (tmax < 0 ? imageParams.highlightColor : TFT_BLACK));
        drawString(spr, String(wind), dag * column1 + column1 - 10, day[3], day[2], TR_DATUM, (beaufort > 5 ? imageParams.highlightColor : TFT_BLACK));
        if (dag > 0) {
            for (int i = loc["line"][0]; i < loc["line"][1]; i += 3) {
                spr.drawPixel(dag * column1, i, TFT_BLACK);
            }
        }
    }

    spr2buffer(spr, filename, imageParams);
    spr.deleteSprite();
}

int getImgURL(String &filename, String URL, time_t fetched, imgParam &imageParams, String MAC) {
    // https://images.klari.net/kat-bw29.jpg

    HTTPClient http;
    logLine("http getImgURL " + URL);
    http.begin(URL);
    http.addHeader("If-Modified-Since", formatHttpDate(fetched));
    http.addHeader("X-ESL-MAC", MAC);
    http.setTimeout(5000);  // timeout in ms
    const int httpCode = http.GET();
    if (httpCode == 200) {
        xSemaphoreTake(fsMutex, portMAX_DELAY);
        File f = contentFS->open("/temp/temp.jpg", "w");
        if (f) {
            http.writeToStream(&f);
            f.close();
            xSemaphoreGive(fsMutex);
            jpg2buffer("/temp/temp.jpg", filename, imageParams);
        } else {
            xSemaphoreGive(fsMutex);
        }
    } else {
        if (httpCode != 304) {
            wsErr("http " + URL + " " + String(httpCode));
        }
    }
    http.end();
    return httpCode;
}

#ifdef CONTENT_RSS
rssClass reader;
#endif

void replaceHTMLentities(String &text) {
    text.replace("&gt;", ">");
    text.replace("&lt;", "<");
    text.replace("&quot;", "\"");
    text.replace("&apos;", "'");
    text.replace("&amp;", "&");
}

void removeHTML(String &text) {
    int len = text.length();
    bool insideTag = false;
    int j = 0;
    for (int i = 0; i < len; ++i) {
        char c = text[i];
        if (c == '<') {
            insideTag = true;
        } else if (c == '>') {
            insideTag = false;
        } else if (!insideTag) {
            text[j++] = c;
        }
    }
    text.remove(j);
}

void stampTime(TFT_eSprite &spr) {
    time_t now;
    time(&now);
    char timeStr[24];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d  %H:%M:%S ", localtime(&now));
    drawString(spr, timeStr, spr.width() - 1, 12, "glasstown_nbp_tf", TR_DATUM, TFT_BLACK);
}

#ifdef CONTENT_RSS
bool getRssFeed(String &filename, String URL, String title, tagRecord *&taginfo, imgParam &imageParams) {
    // https://github.com/garretlab/shoddyxml2

    // http://feeds.feedburner.com/tweakers/nieuws
    // https://www.nu.nl/rss/Algemeen

    wsLog("get rss feed");

    const char *url = URL.c_str();
    const int rssTitleSize = 255;
    const int rssDescSize = 1000;

    TFT_eSprite spr = TFT_eSprite(&tft);

    JsonDocument loc;
    getTemplate(loc, 9, taginfo->hwType);
    initSprite(spr, imageParams.width, imageParams.height, imageParams);
    // stampTime(spr);

    if (util::isEmptyOrNull(title)) title = "RSS feed";
    drawString(spr, title, loc["title"][0], loc["title"][1], loc["title"][2], TL_DATUM, TFT_BLACK, loc["title"][3]);
    int16_t posx = loc["line"][0];
    int16_t posy = loc["line"][1];
    int n = reader.getArticles(url, rssTitleSize, rssDescSize, loc["items"]);

    float lineheight = loc["desc"][3].as<float>();
    for (int i = 0; i < n; i++) {
        // if (reader.titleData[i] != NULL && *reader.titleData[i] != NULL) {
        if (reader.titleData[i] != NULL) {
            String title = String(reader.titleData[i]);
            replaceHTMLentities(title);
            removeHTML(title);

            if (!util::isEmptyOrNull(title)) {
                drawTextBox(spr, title, posx, posy, imageParams.width - 2 * posx, 100, loc["line"][2], TFT_BLACK);
            }
        }
        // if (reader.descData[i] != NULL && *reader.descData[i] != NULL) {
        if (reader.descData[i] != NULL && loc["desc"][2] != "") {
            String desc = String(reader.descData[i]);
            replaceHTMLentities(desc);
            removeHTML(desc);

            if (!util::isEmptyOrNull(desc)) {
                posy += loc["desc"][0].as<int>();
                drawTextBox(spr, desc, posx, posy, imageParams.width - 2 * posx, 100, loc["desc"][2], TFT_BLACK, TFT_WHITE, lineheight);
                posy += loc["desc"][1].as<int>();
            }
        } else {
            posy += loc["desc"][1].as<int>();
        }
    }
    reader.clearItemData();

    spr2buffer(spr, filename, imageParams);
    spr.deleteSprite();

    return true;
}
#endif

char *epoch_to_display(time_t utc) {
    static char display[6];
    struct tm local_tm;
    localtime_r(&utc, &local_tm);
    time_t now;
    time(&now);
    struct tm now_tm;
    localtime_r(&now, &now_tm);
    if (local_tm.tm_year < now_tm.tm_year ||
        (local_tm.tm_year == now_tm.tm_year && local_tm.tm_mon < now_tm.tm_mon) ||
        (local_tm.tm_year == now_tm.tm_year && local_tm.tm_mon == now_tm.tm_mon && local_tm.tm_mday < now_tm.tm_mday) ||
        (local_tm.tm_hour == 0 && local_tm.tm_min == 0) ||
        difftime(utc, now) >= 86400) {
        strftime(display, sizeof(display), languageDateFormat[1].c_str(), &local_tm);
    } else {
        strftime(display, sizeof(display), "%H:%M", &local_tm);
    }
    return display;
}

#ifdef CONTENT_CAL
bool getCalFeed(String &filename, JsonObject &cfgobj, tagRecord *&taginfo, imgParam &imageParams) {
    // google apps scripts method to retrieve calendar
    // see https://github.com/OpenEPaperLink/OpenEPaperLink/wiki/Google-Apps-Scripts for description

    wsLog("get calendar");

    JsonDocument loc;
    getTemplate(loc, 11, taginfo->hwType);

    String URL = cfgobj["apps_script_url"].as<String>() + "?days=" + loc["days"].as<String>();

    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char dateString[40];
    strftime(dateString, sizeof(dateString), languageDateFormat[0].c_str(), &timeinfo);

    HTTPClient http;
    // logLine("http getCalFeed " + URL);
    http.begin(URL);
    http.setTimeout(10000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    int httpCode = http.GET();
    if (httpCode != 200) {
        wsErr("getCalFeed http error " + String(httpCode));
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, http.getString());
    if (error) {
        wsErr(error.c_str());
    }
    http.end();

    TFT_eSprite spr = TFT_eSprite(&tft);

    if (loc["rotate"] == 1) {
        int temp = imageParams.height;
        imageParams.height = imageParams.width;
        imageParams.width = temp;
        imageParams.rotatebuffer = 1 - (imageParams.rotatebuffer % 2);
        initSprite(spr, imageParams.width, imageParams.height, imageParams);
    } else {
        initSprite(spr, imageParams.width, imageParams.height, imageParams);
    }

    switch (loc["mode"].as<int>()) {
        case 0: {
            // appointment list
            String title = cfgobj["title"];
            if (util::isEmptyOrNull(title)) title = "Calendar";
            drawString(spr, title, loc["title"][0], loc["title"][1], loc["title"][2], TL_DATUM, TFT_BLACK);
            drawString(spr, dateString, loc["date"][0], loc["date"][1], loc["title"][2], TR_DATUM, TFT_BLACK);

            int n = doc.size();
            if (n > loc["items"]) n = loc["items"];
            for (int i = 0; i < n; i++) {
                const JsonObject &obj = doc[i];
                const String eventtitle = obj["title"];
                const time_t starttime = obj["start"];
                const time_t endtime = obj["end"];

                if (starttime <= now && endtime > now) {
                    spr.fillRect(loc["red"][0], loc["red"][1].as<int>() + i * loc["line"][2].as<int>(), loc["red"][2], loc["red"][3], imageParams.highlightColor);
                    drawString(spr, epoch_to_display(obj["start"]), loc["line"][0], loc["line"][1].as<int>() + i * loc["line"][2].as<int>(), loc["line"][3], TL_DATUM, TFT_WHITE, 0, imageParams.highlightColor);
                    drawString(spr, eventtitle, loc["line"][4], loc["line"][1].as<int>() + i * loc["line"][2].as<int>(), loc["line"][3], TL_DATUM, TFT_WHITE, 0, imageParams.highlightColor);
                } else {
                    drawString(spr, epoch_to_display(obj["start"]), loc["line"][0], loc["line"][1].as<int>() + i * loc["line"][2].as<int>(), loc["line"][3], TL_DATUM, TFT_BLACK, 0, TFT_WHITE);
                    drawString(spr, eventtitle, loc["line"][4], loc["line"][1].as<int>() + i * loc["line"][2].as<int>(), loc["line"][3], TL_DATUM, TFT_BLACK, 0, TFT_WHITE);
                }
            }
            break;
        }
        case 1: {
#ifdef CONTENT_BIGCAL
            // week view

            // gridparam:
            // 0: 10; offset top
            // 1: 20; offset cal block
            // 2: 30; offset left
            // 3: calibrib16.vlw; headers font
            // 4: tahoma9.vlw; appointments font
            // 5: 14; line height

            timeinfo.tm_hour = 0;
            timeinfo.tm_min = 0;
            timeinfo.tm_sec = 0;
            time_t midnightEpoch = mktime(&timeinfo);

            int calWidth = imageParams.width - 1;
            int calHeight = imageParams.height;
            int calDays = loc["days"];

            int colLeft = loc["gridparam"][2].as<int>();
            int colWidth = (calWidth - colLeft) / calDays;

            int calTop = loc["gridparam"][0].as<int>();
            int calBottom = calHeight;
            int calYOffset = loc["gridparam"][1].as<int>();
            int lineHeight = loc["gridparam"][5].as<int>();

            uint16_t backgroundLight = getColor("lightgray");
            uint16_t backgroundDark = getColor("darkgray");
            if (imageParams.hwdata.bpp >= 3) {
                backgroundLight = getColor("#BEFAFF");
                backgroundDark = getColor("#79FAFF");
            }

            // drawString(spr, String(timeinfo.tm_mday), calWidth / 2, -calHeight/5, "Signika-SB.ttf", TC_DATUM, imageParams.highlightColor, calHeight * 1.2);

            for (int i = 0; i < calDays; i++) {
                struct tm dayTimeinfo = *localtime(&midnightEpoch);
                dayTimeinfo.tm_mday += i;
                time_t dayEpoch = mktime(&dayTimeinfo);
                struct tm *dayInfo = localtime(&dayEpoch);

                int colStart = colLeft + i * colWidth;
                spr.drawLine(colStart, calTop, colStart, calBottom, TFT_BLACK);
                drawString(spr, String(languageDaysShort[dayInfo->tm_wday]) + " " + String(dayInfo->tm_mday), colStart + colWidth / 2, calTop, loc["gridparam"][3], TC_DATUM, TFT_BLACK);

                if (dayInfo->tm_wday == 0 || dayInfo->tm_wday == 6) {
                    spr.fillRect(colStart + 1, calTop + calYOffset, colWidth - 1, calHeight - 1, backgroundDark);
                } else {
                    spr.fillRect(colStart + 1, calTop + calYOffset, colWidth - 1, calHeight - 1, backgroundLight);
                }
            }

            int colStart = colLeft + calDays * colWidth;
            spr.drawLine(colStart, calTop, colStart, calBottom, TFT_BLACK);
            spr.drawLine(0, calTop + calYOffset, calWidth, calTop + calYOffset, TFT_BLACK);

            int minHour = 9;
            int maxHour = 17;

            int n = doc.size();
            int maxBlock = 0;
            int *block = new int[n];

            for (int i = 0; i < n; i++) {
                const JsonObject &obj = doc[i];
                String eventtitle = obj["title"];
                const time_t startdatetime = obj["start"];
                const time_t enddatetime = obj["end"];
                const bool isallday = obj["isallday"];
                const int calendarId = obj["calendar"];

                if (!isallday) {
                    localtime_r(&startdatetime, &timeinfo);
                    if (timeinfo.tm_hour < minHour) minHour = timeinfo.tm_hour;

                    localtime_r(&enddatetime, &timeinfo);
                    if (timeinfo.tm_hour > maxHour) maxHour = timeinfo.tm_hour;
                    if (timeinfo.tm_min > 1 && timeinfo.tm_hour + 1 > maxHour) maxHour = timeinfo.tm_hour + 1;
                } else {
                    int fulldaystart = constrain((startdatetime - midnightEpoch) / (24 * 3600), 0, calDays);
                    int fulldayend = constrain((enddatetime - midnightEpoch) / (24 * 3600), 0, calDays);
                    if (fulldaystart < calDays) {
                        int line = 1;
                        bool overlap = false;
                        do {
                            overlap = false;
                            for (int j = 0; j < i; j++) {
                                const JsonObject &obj2 = doc[j];
                                const bool isallday2 = obj2["isallday"];
                                const time_t startdatetime2 = obj2["start"];
                                const time_t enddatetime2 = obj2["end"];
                                if (startdatetime < enddatetime2 && enddatetime > startdatetime2 &&
                                    line == block[j] && isallday2) {
                                    overlap == true;
                                    line++;
                                }
                            }
                        } while (overlap == true);

                        int16_t eventX = colLeft + fulldaystart * colWidth + 3;
                        int16_t eventY = calTop + calYOffset + (line - 1) * lineHeight + 3;
                        uint16_t background = TFT_WHITE;
                        uint16_t border = TFT_BLACK;
                        uint16_t textcolor = TFT_BLACK;
                        if (loc["colors1"].is<JsonArray>() && loc["colors1"].size() > calendarId) {
                            background = getColor(loc["colors1"][calendarId]);
                            border = getColor(loc["colors2"][calendarId]);
                            if (loc["colors3"][calendarId]) textcolor = getColor(loc["colors3"][calendarId]);
                        }
                        spr.fillRect(eventX - 1, eventY - 2, colWidth * (fulldayend - fulldaystart) - 3, lineHeight - 1, background);
                        spr.drawRect(eventX - 2, eventY - 3, colWidth * (fulldayend - fulldaystart) - 1, lineHeight + 1, border);
                        drawTextBox(spr, eventtitle, eventX, eventY, colWidth * (fulldayend - fulldaystart) - 3, 15, loc["gridparam"][4], textcolor);

                        block[i] = line;
                        if (line > maxBlock) maxBlock = line;
                    }
                }

                const int starttime = timeinfo.tm_hour * 60 + timeinfo.tm_min;

                int hours = timeinfo.tm_hour;
                int minutes = timeinfo.tm_min;
            }
            calYOffset += maxBlock * lineHeight;

            spr.drawLine(0, calTop + calYOffset, calWidth, calTop + calYOffset, TFT_BLACK);
            int hourHeight = (calHeight - calYOffset - calTop) / (maxHour - minHour);
            for (int i = 0; i < (maxHour - minHour); i++) {
                spr.drawLine(0, calTop + calYOffset + i * hourHeight, colLeft + 10, calTop + calYOffset + i * hourHeight, TFT_BLACK);
                for (int j = 1; j <= calDays; j++) {
                    spr.drawLine(colLeft + j * colWidth - 5, calTop + calYOffset + i * hourHeight, colLeft + j * colWidth + 5, calTop + calYOffset + i * hourHeight, TFT_BLACK);
                }
                drawString(spr, String(minHour + i), colLeft - 2, calTop + calYOffset + i * hourHeight + 2, loc["gridparam"][3], TR_DATUM, TFT_BLACK);
            }
            spr.drawLine(0, calTop + calYOffset + (maxHour - minHour) * hourHeight, calWidth, calTop + calYOffset + (maxHour - minHour) * hourHeight, TFT_BLACK);

            for (int i = 0; i < n; i++) {
                const JsonObject &obj = doc[i];
                String eventtitle = obj["title"];
                const time_t startdatetime = obj["start"];
                const time_t enddatetime = obj["end"];
                const bool isallday = obj["isallday"];
                const int calendarId = obj["calendar"];

                if (!isallday) {
                    int fulldaystart = constrain((startdatetime - midnightEpoch) / (24 * 3600), 0, calDays);
                    int fulldayend = constrain((enddatetime - midnightEpoch) / (24 * 3600), 0, calDays);

                    for (int day = fulldaystart; day <= fulldayend; day++) {
                        localtime_r(&startdatetime, &timeinfo);
                        int starttime = timeinfo.tm_hour * 60 + timeinfo.tm_min - minHour * 60;
                        int duration = (enddatetime - startdatetime) / 60;
                        if (day > fulldaystart) {
                            starttime = 0;
                            localtime_r(&enddatetime, &timeinfo);
                            duration = timeinfo.tm_hour * 60 + timeinfo.tm_min - minHour * 60;
                        }
                        char formattedTime[6];
                        strftime(formattedTime, sizeof(formattedTime), "%H:%M", &timeinfo);
                        String formattedTimeString = String(formattedTime);

                        int indent = 1;
                        bool overlap = false;
                        do {
                            overlap = false;
                            for (int j = 0; j < i; j++) {
                                const JsonObject &obj2 = doc[j];
                                const bool isallday2 = obj2["isallday"];
                                const time_t startdatetime2 = obj2["start"];
                                const time_t enddatetime2 = obj2["end"];
                                if (startdatetime < enddatetime2 && enddatetime > startdatetime2 &&
                                    indent == block[j] && isallday2 == false) {
                                    overlap == true;
                                    indent++;
                                }
                            }
                        } while (overlap == true);

                        block[i] = indent;
                        int16_t eventX = colLeft + day * colWidth + (indent - 1) * 5;
                        int16_t eventY = calTop + calYOffset + (starttime * hourHeight / 60);
                        uint16_t background = TFT_WHITE;
                        uint16_t border = TFT_BLACK;
                        uint16_t textcolor = TFT_BLACK;
                        if (loc["colors1"].is<JsonArray>() && loc["colors1"].size() > calendarId) {
                            background = getColor(loc["colors1"][calendarId]);
                            border = getColor(loc["colors2"][calendarId]);
                            if (loc["colors3"][calendarId]) textcolor = getColor(loc["colors3"][calendarId]);
                        }
                        spr.fillRect(eventX + 2, eventY + 1, colWidth - 3, (duration * hourHeight / 60) - 1, background);
                        spr.drawRect(eventX + 1, eventY, colWidth - 1, (duration * hourHeight / 60) + 1, border);
                        eventX += 2;
                        eventY += 2;
                        if (day == fulldaystart) {
                            eventtitle = formattedTimeString + " " + String(eventtitle);
                            drawTextBox(spr, formattedTimeString, eventX, eventY, colWidth - 1, (duration * hourHeight / 60) - 1, loc["gridparam"][4], textcolor, TFT_WHITE, 1);
                            eventX++;
                            eventY = calTop + calYOffset + (starttime * hourHeight / 60) + 2;
                        } else {
                            eventtitle = obj["title"].as<String>();
                        }
                        drawTextBox(spr, eventtitle, eventX, eventY, colWidth - 1, (duration * hourHeight / 60) - 1, loc["gridparam"][4], textcolor, TFT_WHITE, 1);
                    }
                }
            }

            delete[] block;
#endif
            break;
        }
    }

    spr2buffer(spr, filename, imageParams);
    spr.deleteSprite();
    return true;
}
#endif

#ifdef CONTENT_DAYAHEAD
uint16_t getPercentileColor(const double *prices, int numPrices, double price, HwType hwdata) {
    const char *colorsDefault[] = {"black", "darkgray", "pink", "red"};
    const double boundariesDefault[] = {40.0, 80.0, 90.0};

    const char *colors3bpp[] = {"blue", "green", "yellow", "orange", "red"};
    const double boundaries3bpp[] = {20.0, 50.0, 70.0, 90.0};

    const char **colors;
    const double *boundaries;
    int numColors, numBoundaries;

    if (hwdata.bpp == 3 || hwdata.bpp == 4) {
        colors = colors3bpp;
        boundaries = boundaries3bpp;
        numColors = sizeof(colors3bpp) / sizeof(colors3bpp[0]);
        numBoundaries = sizeof(boundaries3bpp) / sizeof(boundaries3bpp[0]);
    } else {
        colors = colorsDefault;
        boundaries = boundariesDefault;
        numColors = sizeof(colorsDefault) / sizeof(colorsDefault[0]);
        numBoundaries = sizeof(boundariesDefault) / sizeof(boundariesDefault[0]);
        if (hwdata.highlightColor == 3) {
            colors[2] = "brown";
            colors[3] = "yellow";
        }
    }
    int colorIndex = numColors - 1;

    for (int i = 0; i < numBoundaries; i++) {
        if (price < prices[int(numPrices * boundaries[i] / 100.0)]) {
            colorIndex = i;
            break;
        }
    }
    return getColor(String(colors[constrain(colorIndex, 0, numColors - 1)]));
}

struct YAxisScale {
    double min;
    double max;
    double step;
};

double roundToNearest(double value, double factor) {
    return round(value / factor) * factor;
}

int mapDouble(double value, double fromMin, double fromMax, int toMin, int toMax) {
    return static_cast<int>(round((value - fromMin) / (fromMax - fromMin) * (toMax - toMin) + toMin));
}

YAxisScale calculateYAxisScale(double priceMin, double priceMax, int divisions) {
    double range = priceMax - priceMin;
    double stepSize = range / divisions;
    double orderOfMagnitude = pow(10, floor(log10(stepSize)));
    double significantDigit = stepSize / orderOfMagnitude;

    double roundedSignificantDigit;
    double thresholds[] = {1.0, 2.0, 5.0, 10.0};
    for (double threshold : thresholds) {
        if (significantDigit <= threshold) {
            roundedSignificantDigit = threshold;
            break;
        }
    }

    double roundedStepSize = roundedSignificantDigit * orderOfMagnitude;
    double newRange = roundedStepSize * divisions;
    double minY = floor(priceMin / roundedStepSize) * roundedStepSize;
    double maxY = minY + newRange;

    return {minY, maxY, roundedStepSize};
}

bool getDayAheadFeed(String &filename, JsonObject &cfgobj, tagRecord *&taginfo, imgParam &imageParams) {
    wsLog("get dayahead prices");

    JsonDocument loc;
    getTemplate(loc, 27, taginfo->hwType);

    // This is a link to a Google Apps Script script, which fetches (and caches) the tariff from https://transparency.entsoe.eu/
    // I made it available to provide easy access to the data, but please don't use this link in any projects other than OpenEpaperLink.
    String URL = "https://script.google.com/macros/s/AKfycbwMmeGAaPrWzVZrESSpmPmD--O132PzW_acnBsuEottKNATTqCRn6h8zN0Yts7S56ggsg/exec?country=" + cfgobj["country"].as<String>();

    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    char dateString[40];
    strftime(dateString, sizeof(dateString), languageDateFormat[0].c_str(), &timeinfo);

    HTTPClient http;
    http.begin(URL);
    http.setTimeout(10000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    int httpCode = http.GET();
    if (httpCode != 200) {
        wsErr("getDayAhead http error " + String(httpCode));
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, http.getString());
    if (error) {
        wsErr(error.c_str());
    }
    http.end();

    TFT_eSprite spr = TFT_eSprite(&tft);

    initSprite(spr, imageParams.width, imageParams.height, imageParams);

    int n = doc.size();

    int units = cfgobj["units"].as<int>();
    if (units == 0) units = 1;
    double tarifkwh;
    double tariftax = cfgobj["tarifftax"].as<double>();
    double minPrice = std::numeric_limits<double>::max();
    double maxPrice = std::numeric_limits<double>::lowest();
    double prices[n];

    JsonDocument doc2;
    JsonArray tariffArray;
    std::string tariffString = cfgobj["tariffkwh"].as<std::string>();
    if (tariffString.front() == '[') {
        if (deserializeJson(doc2, tariffString) == DeserializationError::Ok) {
            tariffArray = doc2.as<JsonArray>();
        } else {
            Serial.println("Error in tariffkwh array");
        }
    }
    for (int i = 0; i < n; i++) {
        const JsonObject &obj = doc[i];

        if (tariffArray.size() == 24) {
            const time_t item_time = obj["time"];
            struct tm item_timeinfo;
            localtime_r(&item_time, &item_timeinfo);

            tarifkwh = tariffArray[item_timeinfo.tm_hour].as<double>();
        } else {
            tarifkwh = cfgobj["tariffkwh"].as<double>();
        }
        prices[i] = (obj["price"].as<double>() / 10 + tarifkwh) * (1 + tariftax / 100) / units;
        minPrice = std::min(minPrice, prices[i]);
        maxPrice = std::max(maxPrice, prices[i]);
    }
    std::sort(prices, prices + n);

    YAxisScale yAxisScale = calculateYAxisScale(minPrice, maxPrice, loc["yaxis"][2].as<int>());
    minPrice = yAxisScale.min;

    uint16_t yAxisX = loc["yaxis"][1].as<int>();
    uint16_t yAxisY = loc["yaxis"][3].as<int>() | 9;
    uint16_t barBottom = loc["bars"][3].as<int>();

    for (double i = minPrice; i <= maxPrice; i += yAxisScale.step) {
        int y = mapDouble(i, minPrice, maxPrice, spr.height() - barBottom, spr.height() - barBottom - loc["bars"][2].as<int>());
        spr.drawLine(0, y, spr.width(), y, TFT_BLACK);
        if (loc["yaxis"][0]) drawString(spr, String(int(i * units)), yAxisX, y - yAxisY, loc["yaxis"][0], TL_DATUM, TFT_BLACK);
    }

    uint16_t barwidth = loc["bars"][1].as<int>() / n;
    uint16_t barheight = loc["bars"][2].as<int>() / (maxPrice - minPrice);
    uint16_t arrowY = 0;
    if (loc["bars"].size() >= 5) arrowY = loc["bars"][4].as<int>();
    uint16_t barX = loc["bars"][0].as<int>();
    double pricenow = std::numeric_limits<double>::quiet_NaN();
    bool showcurrent = true;
    if (cfgobj["showcurr"] && cfgobj["showcurr"] == "0") showcurrent = false;

    for (int i = 0; i < n; i++) {
        const JsonObject &obj = doc[i];
        const time_t item_time = obj["time"];
        struct tm item_timeinfo;
        localtime_r(&item_time, &item_timeinfo);

        if (tariffArray.size() == 24) {
            tarifkwh = tariffArray[item_timeinfo.tm_hour].as<double>();
        } else {
            tarifkwh = cfgobj["tariffkwh"].as<double>();
        }

        const double price = (obj["price"].as<double>() / 10 + tarifkwh) * (1 + tariftax / 100) / units;

        uint16_t barcolor = getPercentileColor(prices, n, price, imageParams.hwdata);
        uint16_t thisbarh = mapDouble(price, minPrice, maxPrice, 0, loc["bars"][2].as<int>());
        spr.fillRect(barX + i * barwidth, spr.height() - barBottom - thisbarh, barwidth - 1, thisbarh, barcolor);
        if (i % 2 == 0 && loc["time"][0]) {
            drawString(spr, String(item_timeinfo.tm_hour), barX + i * barwidth + barwidth / 3 + 1, spr.height() - barBottom + 3, loc["time"][0], TC_DATUM, TFT_BLACK);
        }

        if (now - item_time < 3600 && std::isnan(pricenow) && showcurrent) {
            spr.fillRect(barX + i * barwidth + (barwidth > 6 ? 3 : 1), 5 + arrowY, (barwidth > 6 ? barwidth - 6 : barwidth - 2), 10, imageParams.highlightColor);
            spr.fillTriangle(barX + i * barwidth, 15 + arrowY,
                             barX + i * barwidth + barwidth - 1, 15 + arrowY,
                             barX + i * barwidth + (barwidth - 1) / 2, 15 + barwidth + arrowY, imageParams.highlightColor);
            spr.drawLine(barX + i * barwidth + (barwidth - 1) / 2, 20 + barwidth + arrowY, barX + i * barwidth + (barwidth - 1) / 2, spr.height(), TFT_BLACK);
            pricenow = price;
        }
    }

    if (showcurrent) {
        if (barwidth < 5) {
            drawString(spr, String(timeinfo.tm_hour) + ":00", spr.width() / 2, 5, "calibrib16.vlw", TC_DATUM, TFT_BLACK, 30);
            drawString(spr, String(pricenow) + "/kWh", spr.width() / 2, 25, loc["head"][0], TC_DATUM, TFT_BLACK, 30);
        } else {
            drawString(spr, String(timeinfo.tm_hour) + ":00", barX, 5, loc["head"][0], TL_DATUM, TFT_BLACK, 30);
            drawString(spr, String(pricenow) + "/kWh", spr.width() - barX, 5, loc["head"][0], TR_DATUM, TFT_BLACK, 30);
        }
    }

    spr2buffer(spr, filename, imageParams);
    spr.deleteSprite();
    return true;
}
#endif

#ifdef CONTENT_QR
void drawQR(String &filename, String qrcontent, String title, tagRecord *&taginfo, imgParam &imageParams) {
    TFT_eSprite spr = TFT_eSprite(&tft);

    const char *text = qrcontent.c_str();
    QRCode qrcode;
    uint8_t version = findFittingVersion_text(ECC_MEDIUM, text);
    uint8_t qrcodeData[qrcode_getBufferSize(version)];
    // https://github.com/ricmoo/QRCode
    qrcode_initText(&qrcode, qrcodeData, version, ECC_MEDIUM, text);

    JsonDocument loc;
    getTemplate(loc, 10, taginfo->hwType);
    initSprite(spr, imageParams.width, imageParams.height, imageParams);
    drawString(spr, title, loc["title"][0], loc["title"][1], loc["title"][2], TC_DATUM, TFT_BLACK, loc["title"][3]);

    const int size = qrcode.size;
    const int dotsize = int((imageParams.height - loc["pos"][1].as<int>()) / size);
    const int xpos = loc["pos"][0].as<int>() - dotsize * size / 2;
    const int ypos = loc["pos"][1].as<int>() + (imageParams.height - loc["pos"][1].as<int>() - dotsize * size) / 2;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (qrcode_getModule(&qrcode, x, y)) {
                spr.fillRect(xpos + x * dotsize, ypos + y * dotsize, dotsize, dotsize, TFT_BLACK);
            }
        }
    }

    spr2buffer(spr, filename, imageParams);
    spr.deleteSprite();
}
#endif

#ifdef CONTENT_BUIENRADAR
uint8_t drawBuienradar(String &filename, JsonObject &cfgobj, tagRecord *&taginfo, imgParam &imageParams) {
    uint8_t refresh = 60;
    wsLog("get buienradar");

    getLocation(cfgobj);
    HTTPClient http;

    String lat = cfgobj["#lat"];
    String lon = cfgobj["#lon"];
    // logLine("http drawBuienradar");
    http.begin("https://gadgets.buienradar.nl/data/raintext/?lat=" + lat + "&lon=" + lon);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(5000);
    int httpCode = http.GET();

    if (httpCode == 200) {
        TFT_eSprite spr = TFT_eSprite(&tft);

        JsonDocument loc;
        getTemplate(loc, 16, taginfo->hwType);
        initSprite(spr, imageParams.width, imageParams.height, imageParams);

        tft.setTextWrap(false, false);

        String response = http.getString();

        drawString(spr, cfgobj["location"], loc["location"][0], loc["location"][1], loc["location"][2]);

        const auto &bars = loc["bars"];
        const auto &cols = loc["cols"];
        const int cols0 = cols[0].as<int>();
        const int cols1 = cols[1].as<int>();
        const int cols2 = cols[2].as<int>();
        const String cols3 = cols[3].as<String>();
        const int bars0 = bars[0].as<int>();
        const int bars1 = bars[1].as<int>();
        const int bars2 = bars[2].as<int>();

        float factor = (float)bars1 / 111;
        for (int i = 0; i < imageParams.width; i += 4) {
            int yCoordinates[] = {1, 20, 29, 39, 49, 55, 59};
            for (int y : yCoordinates) {
                spr.drawPixel(i, bars1 - (y * factor), TFT_BLACK);
            }
        }

        drawString(spr, "Buienradar", loc["title"][0], loc["title"][1], loc["title"][2]);

        for (int i = 0; i < 24; i++) {
            const int startPos = i * 10;
            uint8_t value = response.substring(startPos, startPos + 3).toInt();
            const String timestring = response.substring(startPos + 4, startPos + 9);
            const int minutes = timestring.substring(3).toInt();
            if (value < 70) {
                value = 70;
            } else if (value > 180) {
                value = 180;
            }
            if (value > 70) {
                if (i < 12) {
                    refresh = 10;
                } else if (refresh > 10) {
                    refresh = 20;
                }
            }
            value = value - 70;

            spr.fillRect(i * cols2 + bars0, bars1 - (value * factor), bars2, value * factor, (value > 50 ? imageParams.highlightColor : TFT_BLACK));

            if (minutes % 15 == 0) {
                drawString(spr, timestring, i * cols2 + cols0, cols1, cols3);
            }
        }

        spr2buffer(spr, filename, imageParams);
        spr.deleteSprite();
    } else {
        wsErr("Buitenradar http " + String(httpCode));
    }
    http.end();
    return refresh;
}
#endif

void drawAPinfo(String &filename, JsonObject &cfgobj, tagRecord *&taginfo, imgParam &imageParams) {
    if (taginfo->hwType == SOLUM_SEG_UK) {
        imageParams.symbols = 0x00;
        sprintf(imageParams.segments, "");
        return;
    }

    TFT_eSprite spr = TFT_eSprite(&tft);
    JsonDocument loc;
    uint8_t screenCurrentOrientation = 0;
    getTemplate(loc, 21, taginfo->hwType);

    initSprite(spr, imageParams.width, imageParams.height, imageParams);
    const JsonArray jsonArray = loc.as<JsonArray>();
    for (const JsonVariant &elem : jsonArray) {
        drawElement(elem, spr, imageParams, screenCurrentOrientation);
    }

    spr2buffer(spr, filename, imageParams);
    spr.deleteSprite();
}

#ifdef CONTENT_TIMESTAMP
void drawTimestamp(String &filename, JsonObject &cfgobj, tagRecord *&taginfo, imgParam &imageParams) {
    Serial.println("make Timestamp");
    time_t now;
    time(&now);
    struct tm timeinfo;

    JsonDocument loc;
    getTemplate(loc, 1, taginfo->hwType);

    TFT_eSprite spr = TFT_eSprite(&tft);
    initSprite(spr, imageParams.width, imageParams.height, imageParams);

    if (!cfgobj["#init"]) {
        Serial.println("init");
        // init preload images

        char hexmac[17];
        mac2hex(taginfo->mac, hexmac);

        String filename2 = "/temp/" + String(hexmac) + "-2.raw";
        drawString(spr, cfgobj["button1"].as<String>(), spr.width() / 2, 40, "calibrib30.vlw", TC_DATUM, TFT_BLACK);
        drawString(spr, "Well done!", spr.width() / 2, 90, "calibrib30.vlw", TC_DATUM, TFT_BLACK);
        spr2buffer(spr, filename2, imageParams);

        if (imageParams.zlib) {
            imageParams.dataType = DATATYPE_IMG_ZLIB;
        } else if (imageParams.g5) {
            imageParams.dataType = DATATYPE_IMG_G5;
        }

        struct imageDataTypeArgStruct arg = {0};
        arg.preloadImage = 1;
        arg.specialType = 17;  // button 2
        arg.lut = 0;

        prepareDataAvail(filename2, imageParams.dataType, *((uint8_t *)&arg), taginfo->mac, 5 | 0x8000);

        spr.fillRect(0, 0, spr.width(), spr.height(), TFT_WHITE);

        filename2 = "/temp/" + String(hexmac) + "-3.raw";
        drawString(spr, cfgobj["button2"].as<String>(), spr.width() / 2, 40, "calibrib30.vlw", TC_DATUM, TFT_BLACK);
        drawString(spr, "Well done!", spr.width() / 2, 90, "calibrib30.vlw", TC_DATUM, TFT_BLACK);
        spr2buffer(spr, filename2, imageParams);

        arg.preloadImage = 1;
        arg.specialType = 16;  // button 1
        arg.lut = 0;
        prepareDataAvail(filename2, imageParams.dataType, *((uint8_t *)&arg), taginfo->mac, 5 | 0x8000);

        cfgobj["#init"] = "1";
    }
    uint8_t offsety = 0;
    uint8_t buttony = 145;
    spr.fillRect(0, 0, spr.width(), spr.height(), TFT_WHITE);
    if (imageParams.rotate == 2) {
        offsety = 25;
        buttony = 10;
        spr.fillTriangle(spr.width() - 27, spr.height() - 160, spr.width() - 37, spr.height() - 160, spr.width() - 32, spr.height() - 165, TFT_BLACK);
        spr.fillTriangle(spr.width() - 127, spr.height() - 160, spr.width() - 117, spr.height() - 160, spr.width() - 122, spr.height() - 165, TFT_BLACK);
        drawString(spr, cfgobj["button1"], spr.width() - 32, buttony, "calibrib16.vlw", TC_DATUM, TFT_BLACK);
        drawString(spr, cfgobj["button2"], spr.width() - 122, buttony, "calibrib16.vlw", TC_DATUM, TFT_BLACK);
    } else {
        spr.fillTriangle(27, 160, 37, 160, 32, 165, TFT_BLACK);
        spr.fillTriangle(127, 160, 117, 160, 122, 165, TFT_BLACK);
        drawString(spr, cfgobj["button1"], 32, buttony, "calibrib16.vlw", TC_DATUM, TFT_BLACK);
        drawString(spr, cfgobj["button2"], 122, buttony, "calibrib16.vlw", TC_DATUM, TFT_BLACK);
    }
    drawString(spr, cfgobj["title"], spr.width() / 2, 10 + offsety, "calibrib30.vlw", TC_DATUM, TFT_BLACK);
    spr.drawLine(0, 40 + offsety, spr.width(), 40 + offsety, TFT_BLACK);

    uint8_t mode = cfgobj["mode"].as<int>();
    switch (taginfo->wakeupReason) {
        case WAKEUP_REASON_BUTTON2:
            Serial.println("button 1");
            cfgobj["last1"] = now;
            if (mode == 0) {
                // 1 timestamp
                cfgobj["last2"] = cfgobj["button1"].as<String>();
            }
            break;
        case WAKEUP_REASON_BUTTON1:
            Serial.println("button 2");
            if (mode == 0) {
                // 1 timestamp
                cfgobj["last1"] = now;
                cfgobj["last2"] = cfgobj["button2"].as<String>();
            } else {
                cfgobj["last2"] = now;
                // 2 timestamps
            }
            break;
    }

    char dateString1[40];
    uint32_t nextaction = cfgobj["nextaction"].as<uint32_t>();
    String dateformat = languageDateFormat[0] + " %H:%M";
    time_t timestamp = cfgobj["last1"].as<uint32_t>();
    localtime_r(&timestamp, &timeinfo);
    strftime(dateString1, sizeof(dateString1), dateformat.c_str(), &timeinfo);
    if (timestamp == 0) strcpy(dateString1, "never");

    if (mode == 0) {
        drawString(spr, "last:", 10, 50 + offsety, "calibrib16.vlw", TL_DATUM, TFT_BLACK);
        drawString(spr, dateString1, spr.width() / 2, 50 + offsety, "bahnschrift30.vlw", TC_DATUM, TFT_BLACK);
        drawString(spr, cfgobj["last2"].as<String>(), spr.width() / 2, 80 + offsety, "bahnschrift30.vlw", TC_DATUM, TFT_BLACK);

        if (nextaction > 0 && timestamp > 0) {
            timestamp += nextaction * 24 * 3600;
            if (timestamp < taginfo->nextupdate) taginfo->nextupdate = timestamp;
            localtime_r(&timestamp, &timeinfo);
            strftime(dateString1, sizeof(dateString1), languageDateFormat[0].c_str(), &timeinfo);

            drawString(spr, "next:", 10, 115 + offsety, "calibrib16.vlw", TL_DATUM, TFT_BLACK);
            drawString(spr, dateString1, 50, 115 + offsety, "calibrib16.vlw", TL_DATUM, timestamp < now ? imageParams.highlightColor : TFT_BLACK);
        }
    } else {
        drawString(spr, cfgobj["button1"].as<String>(), 10, 50 + offsety, "calibrib16.vlw", TL_DATUM, TFT_BLACK);
        drawString(spr, dateString1, 20, 67 + offsety, "fonts/bahnschrift20", TL_DATUM, TFT_BLACK);

        if (nextaction > 0 && timestamp > 0) {
            timestamp += nextaction * 24 * 3600;
            if (timestamp < taginfo->nextupdate) taginfo->nextupdate = timestamp;
            localtime_r(&timestamp, &timeinfo);
            strftime(dateString1, sizeof(dateString1), languageDateFormat[0].c_str(), &timeinfo);

            drawString(spr, "next", 200, 50 + offsety, "calibrib16.vlw", TL_DATUM, TFT_BLACK);
            drawString(spr, dateString1, 210, 67 + offsety, "fonts/bahnschrift20", TL_DATUM, timestamp < now ? imageParams.highlightColor : TFT_BLACK);
        }

        char dateString2[40];
        time_t timestamp = cfgobj["last2"].as<uint32_t>();
        localtime_r(&timestamp, &timeinfo);
        strftime(dateString2, sizeof(dateString2), dateformat.c_str(), &timeinfo);
        if (timestamp == 0) strcpy(dateString2, "never");

        drawString(spr, cfgobj["button2"].as<String>(), 10, 90 + offsety, "calibrib16.vlw", TL_DATUM, TFT_BLACK);
        drawString(spr, dateString2, 20, 107 + offsety, "fonts/bahnschrift20", TL_DATUM, TFT_BLACK);

        if (nextaction > 0 && timestamp > 0) {
            timestamp += nextaction * 24 * 3600;
            localtime_r(&timestamp, &timeinfo);
            strftime(dateString2, sizeof(dateString2), languageDateFormat[0].c_str(), &timeinfo);

            drawString(spr, "next", 200, 90 + offsety, "calibrib16.vlw", TL_DATUM, TFT_BLACK);
            drawString(spr, dateString2, 210, 107 + offsety, "fonts/bahnschrift20", TL_DATUM, timestamp < now ? imageParams.highlightColor : TFT_BLACK);
        }
    }

    spr2buffer(spr, filename, imageParams);
    spr.deleteSprite();
}
#endif

bool getJsonTemplateFile(String &filename, String jsonfile, tagRecord *&taginfo, imgParam &imageParams) {
    if (jsonfile.c_str()[0] != '/') {
        jsonfile = "/" + jsonfile;
    }
    File file = contentFS->open(jsonfile, "r");
    if (file) {
        drawJsonStream(file, filename, taginfo, imageParams);
        file.close();
        // contentFS->remove(jsonfile);
        return true;
    }
    return false;
}

/// @brief Extract a variable with the given path from the given json
/// @note Float and double values are rounded to 2 decimal places
/// @param json Json document
/// @param path Path in form of a.b.1.c
/// @return Value as string
String extractValueFromJson(JsonDocument &json, const String &path) {
    JsonVariant currentObj = json.as<JsonVariant>();
    char *segment = strtok(const_cast<char *>(path.c_str()), ".");

    while (segment != NULL) {
        if (currentObj.is<JsonObject>()) {
            currentObj = currentObj.as<JsonObject>()[segment];
        } else if (currentObj.is<JsonArray>()) {
            int index = atoi(segment);
            currentObj = currentObj.as<JsonArray>()[index];
        } else {
            Serial.printf("Invalid JSON structure at path segment: %s\r\n", segment);
            return "";
        }
        segment = strtok(NULL, ".");
    }

    if (!currentObj.is<int>() && currentObj.is<float>()) {
        return String(currentObj.as<float>(), 2);
    }

    return currentObj.as<String>();
}

/// @brief Replaces json placeholders ({.a.b.1.c}) with variables
class DataInterceptor : public Stream {
   private:
    /// @brief Stream being wrapped
    Stream &_stream;
    /// @brief Json containing variables
    JsonDocument &_variables;
    /// @brief Parsing buffer
    String _buffer;
    /// @brief Buffer size
    const size_t _bufferSize = 32;

   public:
    DataInterceptor(Stream &stream, JsonDocument &variables)
        : _stream(stream), _variables(variables) {
    }

    int available() override {
        return _buffer.length() + _stream.available();
    }

    int read() override {
        findAndReplace();

        if (_buffer.length() > 0) {
            const int data = _buffer[0];
            _buffer.remove(0, 1);
            return data;
        }

        return -1;  // No more data
    }

    int peek() override {
        findAndReplace();
        return _buffer.length() ? _buffer[0] : -1;
    }

    size_t write(uint8_t data) override {
        return _stream.write(data);
    }

   private:
    /// @brief Fill buffer, find and replace json variables
    void findAndReplace() {
        unsigned int len;
        while ((len = _buffer.length()) < _bufferSize) {
            const int data = _stream.read();
            if (data == -1) {
                break;  // No more data to read
            }
            _buffer += (char)data;
        }

        if (len < 4) {
            // There are no variables with less than 4 characters
            return;
        }

        int endIndex = findVar(_buffer, 0);
        if (endIndex == -1) {
            return;
        }

        const String varCleaned = _buffer.substring(1, endIndex - 1);
        String replacement = extractValueFromJson(_variables, varCleaned);

        // Check for operator and second variable
        if (endIndex + 3 < len) {
            const char op = _buffer[endIndex];
            if ((op == '*' || op == '/' || op == '+' || op == '-')) {
                const int endIndex2 = findVar(_buffer, endIndex + 1);
                if (endIndex2 != -1) {
                    const String var2Cleaned = _buffer.substring(endIndex + 2, endIndex2 - 1);
                    const float v2 = extractValueFromJson(_variables, var2Cleaned).toFloat();
                    endIndex = endIndex2;

                    if (op == '*') {
                        replacement = String(replacement.toFloat() * v2, 0);
                    } else if (op == '/') {
                        replacement = abs(v2) > 0.0f ? String(replacement.toFloat() / v2, 0) : "0";
                    } else if (op == '+') {
                        replacement = String(replacement.toFloat() + v2, 0);
                    } else if (op == '-') {
                        replacement = String(replacement.toFloat() - v2, 0);
                    }
                }
            }
        }

        _buffer = replacement + _buffer.substring(endIndex);
    }

    /// @brief Find a var at given start index
    /// @param buffer Buffer to search in
    /// @param index Index to look at
    /// @return Endindex
    int findVar(const String &buffer, const int index) {
        if (buffer[index] != '{' || buffer[index + 1] != '.') {
            return -1;
        }

        return buffer.indexOf("}", index + 2) + 1;
    }
};

bool getJsonTemplateFileExtractVariables(String &filename, String jsonfile, JsonDocument &variables, tagRecord *&taginfo, imgParam &imageParams) {
    if (jsonfile.c_str()[0] != '/') {
        jsonfile = "/" + jsonfile;
    }
    File file = contentFS->open(jsonfile, "r");
    if (file) {
        auto interceptor = DataInterceptor(file, variables);
        drawJsonStream(interceptor, filename, taginfo, imageParams);
        file.close();
        // contentFS->remove(jsonfile);
        return true;
    }
    return false;
}

int getJsonTemplateUrl(String &filename, String URL, time_t fetched, String MAC, tagRecord *&taginfo, imgParam &imageParams) {
    HTTPClient http;
    http.useHTTP10(true);
    logLine("http getJsonTemplateUrl " + URL);
    http.begin(URL);
    http.addHeader("If-Modified-Since", formatHttpDate(fetched));
    http.addHeader("X-ESL-MAC", MAC);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(5000);
    const int httpCode = http.GET();
    if (httpCode == 200) {
        drawJsonStream(http.getStream(), filename, taginfo, imageParams);
    } else {
        if (httpCode != 304) {
            wsErr("http " + URL + " status " + String(httpCode));
        }
    }
    http.end();
    return httpCode;
}

void drawJsonStream(Stream &stream, String &filename, tagRecord *&taginfo, imgParam &imageParams) {
    TFT_eSprite spr = TFT_eSprite(&tft);
    initSprite(spr, imageParams.width, imageParams.height, imageParams);
    uint8_t screenCurrentOrientation = 0;
    JsonDocument doc;
    if (stream.find("[")) {
        do {
            DeserializationError error = deserializeJson(doc, stream);
            if (error) {
                wsErr("json error " + String(error.c_str()));
                break;
            } else {
                drawElement(doc.as<JsonObject>(), spr, imageParams, screenCurrentOrientation);
                doc.clear();
            }
        } while (stream.findUntil(",", "]"));
    }

    spr2buffer(spr, filename, imageParams);
    spr.deleteSprite();
}

void rotateBuffer(uint8_t rotation, uint8_t &currentOrientation, TFT_eSprite &spr, imgParam &imageParams) {
    rotation = rotation % 4;               // First of all, let's be sure that the rotation have a valid value (0, 1, 2 or 3)
    if (rotation != currentOrientation) {  // If we have a rotation to do, let's do it
        int stepToDo = (currentOrientation - rotation + 4) % 4;
        // 2: upside down
        // 3: 270° rotation
        // 1: 90° rotation

        if (abs(stepToDo) == 2) {                                        // If we have to do a 180° rotation:
            TFT_eSprite sprCpy = TFT_eSprite(&tft);                      // We create a new sprite that will act as a buffer
            initSprite(sprCpy, spr.width(), spr.height(), imageParams);  // initialisation of the new sprite
            spr.pushRotated(&sprCpy, 180, TFT_WHITE);                    // We fill the new sprite with the old one rotated by 180°
            spr.fillSprite(TFT_WHITE);                                   // We fill the old one in white as anything that's white will be ignored by the pushRotated function
            sprCpy.pushRotated(&spr, 0, TFT_WHITE);                      // We copy the buffer sprite to the main one
            sprCpy.deleteSprite();                                       // We delete the buffer sprite to avoid memory leak
        } else {
            int angle = (stepToDo == 3) ? 270 : 90;
            TFT_eSprite sprCpy = TFT_eSprite(&tft);
            initSprite(sprCpy, spr.height(), spr.width(), imageParams);
            spr.pushRotated(&sprCpy, angle, TFT_WHITE);
            spr.deleteSprite();
            initSprite(spr, sprCpy.width(), sprCpy.height(), imageParams);
            sprCpy.pushToSprite(&spr, 0, 0);
            sprCpy.deleteSprite();
            imageParams.rotatebuffer = 1 - (imageParams.rotatebuffer % 2);
        }
        currentOrientation = rotation;
    }
}

TFT_eSprite sprDraw = TFT_eSprite(&tft);
bool spr_draw(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
    sprDraw.pushImage(x, y, w, h, bitmap);
    return 1;
}
void drawElement(const JsonObject &element, TFT_eSprite &spr, imgParam &imageParams, uint8_t &currentOrientation) {
    if (element["text"].is<JsonArray>()) {
        const JsonArray &textArray = element["text"];
        const uint16_t align = textArray[5] | 0;
        const uint16_t size = textArray[6] | 0;
        const String bgcolorstr = textArray[7].as<String>();
        const uint16_t bgcolor = (bgcolorstr.length() > 0) ? getColor(bgcolorstr) : TFT_WHITE;
        drawString(spr, textArray[2], textArray[0].as<int>(), textArray[1].as<int>(), textArray[3], align, getColor(textArray[4]), size, bgcolor);
    } else if (element["textbox"].is<JsonArray>()) {
        // posx, posy, width, height, text, font, color, lineheight, align  
        const JsonArray &textArray = element["textbox"];
        float lineheight = textArray[7].as<float>();
        if (lineheight == 0) lineheight = 1;
        int16_t posx = textArray[0] | 0;
        int16_t posy = textArray[1] | 0;
        String text = textArray[4];
        const uint16_t align = textArray[8] | 0;
        drawTextBox(spr, text, posx, posy, textArray[2], textArray[3], textArray[5], getColor(textArray[6]), TFT_WHITE, lineheight, align);
    } else if (element["box"].is<JsonArray>()) {
        const JsonArray &boxArray = element["box"];
        spr.fillRect(boxArray[0].as<int>(), boxArray[1].as<int>(), boxArray[2].as<int>(), boxArray[3].as<int>(), getColor(boxArray[4]));
        if (boxArray.size()>=7) {
            for (int i=0; i < boxArray[6].as<int>(); i++) {
                spr.drawRect(boxArray[0].as<int>() + i, boxArray[1].as<int>() + i, boxArray[2].as<int>() - 2 * i, boxArray[3].as<int>() - 2 * i, getColor(boxArray[5]));
            }
        }
    } else if (element["rbox"].is<JsonArray>()) {
        const JsonArray &rboxArray = element["rbox"];
        spr.fillRoundRect(rboxArray[0].as<int>(), rboxArray[1].as<int>(), rboxArray[2].as<int>(), rboxArray[3].as<int>(), rboxArray[4].as<int>(), getColor(rboxArray[5]));
        if (rboxArray.size() >= 8) {
            for (int i = 0; i < rboxArray[7].as<int>(); i++) {
                spr.drawRoundRect(rboxArray[0].as<int>() + i, rboxArray[1].as<int>() + i, rboxArray[2].as<int>() - 2 * i, rboxArray[3].as<int>() - 2 * i, rboxArray[4].as<int>() - i / 1.41, getColor(rboxArray[6]));
                if (i > 0) {
                    spr.drawRoundRect(rboxArray[0].as<int>() + i - 1, rboxArray[1].as<int>() + i, rboxArray[2].as<int>() - 2 * i + 2, rboxArray[3].as<int>() - 2 * i, rboxArray[4].as<int>() - i / 1.41, getColor(rboxArray[6]));
                }
            }
        }
    } else if (element["line"].is<JsonArray>()) {
        const JsonArray &lineArray = element["line"];
        spr.drawLine(lineArray[0].as<int>(), lineArray[1].as<int>(), lineArray[2].as<int>(), lineArray[3].as<int>(), getColor(lineArray[4]));
    } else if (element["triangle"].is<JsonArray>()) {
        const JsonArray &lineArray = element["triangle"];
        spr.fillTriangle(lineArray[0].as<int>(), lineArray[1].as<int>(), lineArray[2].as<int>(), lineArray[3].as<int>(), lineArray[4].as<int>(), lineArray[5].as<int>(), getColor(lineArray[6]));
    } else if (element["circle"].is<JsonArray>()) {
        const JsonArray &circleArray = element["circle"];
        spr.fillCircle(circleArray[0].as<int>(), circleArray[1].as<int>(), circleArray[2].as<int>(), getColor(circleArray[3]));
        if (circleArray.size() >= 6) {
            for (int i = 0; i < circleArray[5].as<int>(); i++) {
                spr.drawCircle(circleArray[0].as<int>(), circleArray[1].as<int>(), circleArray[2].as<int>() - i, getColor(circleArray[4]));
                if (i > 0) {
                    spr.drawCircle(circleArray[0].as<int>(), circleArray[1].as<int>(), circleArray[2].as<int>() - i - 0.5, getColor(circleArray[4]));
                }
            }
        }
    } else if (element["image"].is<JsonArray>()) {
        const JsonArray &imgArray = element["image"];

        TJpgDec.setSwapBytes(true);
        TJpgDec.setJpgScale(1);
        TJpgDec.setCallback(spr_draw);
        uint16_t w = 0, h = 0;
        String filename = imgArray[0];
        if (filename[0] != '/') {
            filename = "/" + filename;
        }
        TJpgDec.getFsJpgSize(&w, &h, filename, *contentFS);
        if (w == 0 && h == 0) {
            wsErr("invalid jpg");
            return;
        }
        Serial.println("jpeg conversion " + String(w) + "x" + String(h));
        sprDraw.setColorDepth(16);
        sprDraw.createSprite(w, h);
        if (sprDraw.getPointer() == nullptr) {
            wsErr("Failed to create sprite in contentmanager");
        } else {
            TJpgDec.drawFsJpg(0, 0, filename, *contentFS);
            sprDraw.pushToSprite(&spr, imgArray[1].as<int>(), imgArray[2].as<int>());
            sprDraw.deleteSprite();
        }

    } else if (element["rotate"].is<uint8_t>()) {
        uint8_t rotation = element["rotate"].as<int>();
        rotateBuffer(rotation, currentOrientation, spr, imageParams);
    }
}

uint16_t getColor(const String &color) {
    if (color == "0" || color == "white") return TFT_WHITE;
    if (color == "1" || color == "" || color == "black") return TFT_BLACK;
    if (color == "2" || color == "red") return TFT_RED;
    if (color == "3" || color == "yellow") return TFT_YELLOW;
    if (color == "4" || color == "lightgray") return 0xBDF7;
    if (color == "5" || color == "darkgray") return TFT_DARKGREY;
    if (color == "6" || color == "pink") return 0xFBCF;
    if (color == "7" || color == "brown") return 0x8400;
    if (color == "8" || color == "green") return TFT_GREEN;
    if (color == "9" || color == "blue") return TFT_BLUE;
    if (color == "10" || color == "orange") return 0xFBE0;
    uint16_t r, g, b;
    if (color.length() == 7 && color[0] == '#' &&
        sscanf(color.c_str(), "#%2hx%2hx%2hx", &r, &g, &b) == 3) {
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }
    return TFT_WHITE;
}

char *formatHttpDate(const time_t t) {
    static char buf[40];
    struct tm *timeinfo;
    timeinfo = localtime(&t);                 // Get the local time
    const time_t utcTime = mktime(timeinfo);  // Convert to UTC
    timeinfo = gmtime(&utcTime);
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", timeinfo);
    return buf;
}

String urlEncode(const char *msg) {
    constexpr const char *hex = "0123456789ABCDEF";
    String encodedMsg = "";

    while (*msg != '\0') {
        if (
            ('a' <= *msg && *msg <= 'z') || ('A' <= *msg && *msg <= 'Z') || ('0' <= *msg && *msg <= '9') || *msg == '-' || *msg == '_' || *msg == '.' || *msg == '~') {
            encodedMsg += *msg;
        } else {
            encodedMsg += '%';
            encodedMsg += hex[(unsigned char)*msg >> 4];
            encodedMsg += hex[*msg & 0xf];
        }
        msg++;
    }
    return encodedMsg;
}

int windSpeedToBeaufort(const float windSpeed) {
    constexpr const float speeds[] = {0.3, 1.5, 3.3, 5.5, 8, 10.8, 13.9, 17.2, 20.8, 24.5, 28.5, 32.7};
    constexpr const int numSpeeds = sizeof(speeds) / sizeof(speeds[0]);
    int beaufort = 0;
    for (int i = 0; i < numSpeeds; i++) {
        if (windSpeed >= speeds[i]) {
            beaufort = i + 1;
        }
    }
    return beaufort;
}

String windDirectionIcon(const int degrees) {
    const String directions[] = {"\uf044", "\uf043", "\uf048", "\uf087", "\uf058", "\uf057", "\uf04d", "\uf088"};
    int index = (degrees + 22) / 45;
    if (index >= 8) {
        index = 0;
    }
    return directions[index];
}

void getLocation(JsonObject &cfgobj) {
    const String lat = cfgobj["#lat"];
    const String lon = cfgobj["#lon"];

    if (util::isEmptyOrNull(lat) || util::isEmptyOrNull(lon)) {
        wsLog("get location");
        JsonDocument filter;
        filter["results"][0]["latitude"] = true;
        filter["results"][0]["longitude"] = true;
        filter["results"][0]["timezone"] = true;
        JsonDocument doc;
        if (util::httpGetJson("https://geocoding-api.open-meteo.com/v1/search?name=" + urlEncode(cfgobj["location"]) + "&count=1", doc, 5000, &filter)) {
            cfgobj["#lat"] = doc["results"][0]["latitude"].as<String>();
            cfgobj["#lon"] = doc["results"][0]["longitude"].as<String>();
            cfgobj["#tz"] = doc["results"][0]["timezone"].as<String>();
        }
    }
}

#ifdef CONTENT_NFCLUT
void prepareNFCReq(const uint8_t *dst, const char *url) {
    uint8_t *data;
    size_t len = strlen(url);
    data = new uint8_t[len + 8];

    // TLV
    data[0] = 0x03;  // NDEF message (TLV type)
    data[1] = 4 + len + 1;
    // ndef record
    data[2] = 0xD1;
    data[3] = 0x01;     // well known record type
    data[4] = len + 1;  // payload length
    data[5] = 0x55;     // payload type (URI record)
    data[6] = 0x00;     // URI identifier code (no prepending)

    memcpy(data + 7, reinterpret_cast<const uint8_t *>(url), len);
    len = 7 + len;
    data[len] = 0xFE;
    len = 1 + len;
    prepareDataAvail(data, len, DATATYPE_NFC_RAW_CONTENT, dst);
}
#endif

#ifdef CONTENT_TAGCFG
void prepareConfigFile(const uint8_t *dst, const JsonObject &config) {
    struct tagsettings tagSettings;
    tagSettings.settingsVer = 1;
    tagSettings.enableFastBoot = config["fastboot"].as<int>();
    tagSettings.enableRFWake = config["rfwake"].as<int>();
    tagSettings.enableTagRoaming = config["tagroaming"].as<int>();
    tagSettings.enableScanForAPAfterTimeout = config["tagscanontimeout"].as<int>();
    tagSettings.enableLowBatSymbol = config["showlowbat"].as<int>();
    tagSettings.enableNoRFSymbol = config["shownorf"].as<int>();
    tagSettings.customMode = 0;
    tagSettings.fastBootCapabilities = 0;
    tagSettings.minimumCheckInTime = 1;
    tagSettings.fixedChannel = config["fixedchannel"].as<int>();
    tagSettings.batLowVoltage = config["lowvoltage"].as<int>();
    prepareDataAvail((uint8_t *)&tagSettings, sizeof(tagSettings), 0xA8, dst);
}
#endif

void getTemplate(JsonDocument &json, const uint8_t id, const uint8_t hwtype) {
    JsonDocument filter;
    JsonDocument doc;

    const String idstr = String(id);
    constexpr const char *templateKey = "template";

    char filename[20];
    snprintf(filename, sizeof(filename), "/tagtypes/%02X.json", hwtype);
    File jsonFile = contentFS->open(filename, "r");

    if (jsonFile) {
        filter[templateKey][idstr] = true;
        filter["usetemplate"] = true;
        const DeserializationError error = deserializeJson(doc, jsonFile, DeserializationOption::Filter(filter));
        jsonFile.close();
        if (!error && doc[templateKey].is<JsonVariant>() && doc[templateKey][idstr].is<JsonVariant>()) {
            json.set(doc[templateKey][idstr]);
            return;
        }
        if (!error && doc["usetemplate"].is<uint8_t>()) {
            getTemplate(json, id, doc["usetemplate"]);
            return;
        }
        Serial.println("json error in " + String(filename));
        Serial.println(error.c_str());
    } else {
        Serial.println("Failed to open " + String(filename));
    }
}
