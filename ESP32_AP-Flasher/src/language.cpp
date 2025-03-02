#include "language.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>

#include "settings.h"
#include "storage.h"
#include "tag_db.h"

String languageDaysShort[7];
String languageDays[7];
String languageMonth[12];
String languageDateFormat[5];

int currentLanguage = 0;

void updateLanguageFromConfig() {
    int tempLang = config.language;
    if (tempLang < 0 || tempLang > 11) {
        Serial.println("Language not supported");
        return;
    }
    currentLanguage = tempLang;

    File file = contentFS->open("/languages.json", "r");
    if (!file) {
        Serial.println("Failed to open languages.json file");
        return;
    }

    JsonDocument doc;
    JsonDocument filter;
    filter[String(currentLanguage)] = true;
    const DeserializationError error = deserializeJson(doc, file, DeserializationOption::Filter(filter));
    file.close();
    if (error) {
        Serial.print("Failed to parse JSON: ");
        Serial.println(error.c_str());
        return;
    }
    JsonObject languageObject = doc[String(currentLanguage)];
    for (int i = 0; i < 7; ++i) {
        languageDaysShort[i] = languageObject["daysShort"][i].as<String>();
        languageDays[i] = languageObject["days"][i].as<String>();
    }
    for (int i = 0; i < 12; ++i) {
        languageMonth[i] = languageObject["months"][i].as<String>();
    }
    for (int i = 0; i < languageObject["date_format"].size(); i++) {
        languageDateFormat[i] = languageObject["date_format"][i].as<String>();
    }
}

