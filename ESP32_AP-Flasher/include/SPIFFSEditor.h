#ifndef SPIFFSEditor_H_
#define SPIFFSEditor_H_
#ifdef W5500_ETH
  #include <AsyncWebServer_ESP32_SC_W5500.h>
#else
  #include <ESPAsyncWebServer.h>
#endif

class SPIFFSEditor: public AsyncWebHandler {
  private:
    fs::FS _fs;
    String _username;
    String _password; 
    bool _authenticated;
    uint32_t _startTime;
  public:
#ifdef ESP32
    SPIFFSEditor(const fs::FS& fs, const String& username=String(), const String& password=String());
#else
    SPIFFSEditor(const String& username=String(), const String& password=String(), const fs::FS& fs=SPIFFS);
#endif
    virtual bool canHandle(AsyncWebServerRequest *request) override final;
    virtual void handleRequest(AsyncWebServerRequest *request) override final;
    virtual void handleUpload(AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) override final;
    virtual bool isRequestHandlerTrivial() override final {return false;}
    virtual String listFilesRecursively(String path, bool recursive = false);
};

#endif
