#include "SPIFFSEditor.h"

#include <FS.h>

#define SPIFFS_MAXLENGTH_FILEPATH 32

SPIFFSEditor::SPIFFSEditor(const fs::FS &fs, const String &username, const String &password)
    : _fs(fs), _username(username), _password(password), _authenticated(false), _startTime(0) {
}

bool SPIFFSEditor::canHandle(AsyncWebServerRequest *request) const {
    if (request->url().equalsIgnoreCase("/edit")) {
        if (request->method() == HTTP_GET) {
            if (request->hasParam("list")) {
                return true;
            }
            if (request->hasParam("edit")) {
                request->_tempFile = _fs.open("/" + request->arg("edit"), "r");
                if (!request->_tempFile) {
                    return false;
                }
                if (request->_tempFile.isDirectory()) {
                    request->_tempFile.close();
                    return false;
                }
            }
            if (request->hasParam("download")) {
                request->_tempFile = _fs.open("/" + request->arg("download"), "r");
                if (!request->_tempFile) {
                    return false;
                }
                if (request->_tempFile.isDirectory()) {
                    request->_tempFile.close();
                    return false;
                }
            }
            return true;
        } else if (request->method() == HTTP_POST || request->method() == HTTP_DELETE || request->method() == HTTP_PUT) {
            return true;
        }
    }
    return false;
}

String SPIFFSEditor::listFilesRecursively(String path, bool recursive) {
    if (recursive && (path == "//www" || path == "//tagtypes" || path == "//current")) return "";

    File dir = _fs.open(path);
    String output = "";

    File file = dir.openNextFile();
    bool isFirstFile = true;

    while (file) {
        if (file.isDirectory()) {
            if (recursive) {
                String subDirPath = String(path + "/" + file.name());
                String subDirOutput = listFilesRecursively(subDirPath, true);
                output += subDirOutput;
            } else {
                output += ",{\"type\":\"dir\",\"name\":\"" + String(file.name()) + "\"}";
            }
        } else {
            if (recursive) {
                output += ",{\"type\":\"file\",\"name\":\"" + path.substring(2) + "/" + String(file.name()) + "\",\"size\":" + file.size() + "}";
            } else {
                output += ",{\"type\":\"file\",\"name\":\"" + String(file.name()) + "\",\"size\":" + file.size() + "}";
            }
        }
        file = dir.openNextFile();
    }
    dir.close();
    return output;
}

void SPIFFSEditor::handleRequest(AsyncWebServerRequest *request) {
    if (_username.length() && _password.length() && !request->authenticate(_username.c_str(), _password.c_str())) {
        return request->requestAuthentication();
    }

    if (request->method() == HTTP_GET) {
        if (request->hasParam("list")) {
            const String path = request->getParam("list")->value();
            String output = "[" + listFilesRecursively(path, request->hasParam("recursive")).substring(1) + "]";
            request->send(200, "application/json", output);
        } else if (request->hasParam("edit") || request->hasParam("download")) {
            request->send(request->_tempFile, request->_tempFile.name(), String(), request->hasParam("download"));
        } else {
            const char *buildTime = __DATE__ " " __TIME__ " GMT";
            if (request->header("If-Modified-Since").equals(buildTime)) {
                request->send(304);
            } else {
                AsyncWebServerResponse *response = request->beginResponse(_fs, "/www/edit.html", "text/html");
                response->addHeader("Last-Modified", buildTime);
                request->send(response);
            }
        }
    } else if (request->method() == HTTP_DELETE) {
        if (request->hasParam("path", true)) {
            _fs.remove("/" + request->getParam("path", true)->value());
            request->send(200, "", "DELETE: " + request->getParam("path", true)->value());
        } else {
            request->send(404);
        }
    } else if (request->method() == HTTP_POST) {
        String filename = request->getParam("data", true, true)->value();
        if (filename.c_str()[0] != '/') {
            filename = "/" + filename;
        }
        if (request->hasParam("data", true, true) && _fs.exists(filename)) {
            request->send(200, "", "UPLOADED: " + request->getParam("data", true, true)->value());
        } else {
            request->send(500);
        }
    } else if (request->method() == HTTP_PUT) {
        if (request->hasParam("path", true)) {
            String filename = request->getParam("path", true)->value();
            if (_fs.exists(filename)) {
                request->send(200);
            } else {
                if (filename.c_str()[0] != '/') {
                    filename = "/" + filename;
                }
                fs::File f = _fs.open(filename, "w");
                if (f) {
                    f.write((uint8_t)0x00);
                    f.close();
                    request->send(200, "", "CREATE: " + filename);
                } else {
                    request->send(500);
                }
            }
        } else {
            request->send(400);
        }
    }
}

void SPIFFSEditor::handleUpload(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final) {
    if (!index) {
        if (!_username.length() || request->authenticate(_username.c_str(), _password.c_str())) {
            _authenticated = true;
            if (filename.c_str()[0] != '/') {
                request->_tempFile = _fs.open("/" + filename, "w");
            } else {
                int lastSlash = filename.lastIndexOf('/');
                if (lastSlash != -1) {
                    String folderPath = filename.substring(0, lastSlash);
                    if (!_fs.exists(folderPath)) {
                        _fs.mkdir(folderPath);
                    }
                }
                request->_tempFile = _fs.open(filename, "w");
            }
            _startTime = millis();
        }
    }
    if (_authenticated && request->_tempFile) {
        if (len) {
            request->_tempFile.write(data, len);
        }
        if (final) {
            request->_tempFile.close();
        }
    }
}
