/*
   Libraries needed
*/
//For temp & humidity sensor
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>


//Normal Mode
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
//Config. Mode
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <FS.h>

/*
   Configurable Settings

   Connect CONFIG_PIN(Default GPIO_03[RX]) to GND during startup to enter Config. Mode:
      1. Connect to 'ESP_Button' WiFi Access Point, with the password 'wifibutton'
      2. Visit http://192.168.4.1 to open the configuration page, or  http://192.168.4.1/edit to see the filesystem.
      3. After seeting your values, click on the 'Save' button then the 'Restart'
      4. Your button is now configured.
*/
String ssid;
String password;
String host;
String url;
int wc_p;                    // max. time in seconds to connect to wifi, before giving up
int gr_p;                    // max. times of attemps to perform GET request, before giving up
int reporting_interval_mins; // number of minutes between reporting (aka, duration of deep sleep)
bool s_vcc;                  // whether to send VCC voltage as a parameter in the url request
bool s_temp;                 // whether to send temperature from DHT11 as a parameter in the url request
bool s_humidity;             // whether to send humidity from DHT11 as a parameter in the url request
bool is_ip;                  // whether host adress is IP
String vcc_parm;             // parameter to pass VCC voltage by
String temp_parm;            // parameter to pass temperature by
String humidity_parm;        // parameter to pass humidity by

// Temp/Humidity sensor setup
#define DHT11_PIN 0
DHT dht(DHT11_PIN, DHT11);

/*
   System Variables
*/
//#define NOT_DEBUG //wether to enable debug or to show indication lights instead
//Normal Mode
int failCount = 0;
ADC_MODE(ADC_VCC);
bool su_mode = true;
//Config. Mode
#define CONFIG_PIN 3

ESP8266WebServer server(80);
File fsUploadFile;
const char *APssid = "ESP_Sensor";
const char *APpass = "wifisensor";

void gotoSleep() {
  ESP.deepSleep(reporting_interval_mins * 60 * 1000000);
}
  
void setup()
{
  //start serial monitor, SPIFFS and Config. Pin
  Serial.begin(115200);
  pinMode(CONFIG_PIN, INPUT_PULLUP);
  delay(10);

  // Read sensor values
  dht.begin();
  Serial.print("Temp C  : ");
  Serial.println(String(dht.readTemperature()));
  Serial.print("Humidity: ");
  Serial.println(String(dht.readHumidity()));

  SPIFFS.begin();
  Serial.println();
  Serial.println("ESP Booting...");
  Serial.println("SPIFFS Content: ");
  {
    Dir dir = SPIFFS.openDir("/");
    while (dir.next())
    {
      String fileName = dir.fileName();
      size_t fileSize = dir.fileSize();
      Serial.printf("FS File: %s, size: %s\n", fileName.c_str(), formatBytes(fileSize).c_str());
    }
    Serial.printf("\n");
  }

  //read and parse config.json from SPIFFS
  readConfig();

  //read Config. Pin
  su_mode = !digitalRead(CONFIG_PIN);
  if (su_mode)
  {

    //start Config. Mode
#ifdef NOT_DEBUG
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
#endif
    Serial.println("Entering Config. Mode!");

    //start WiFi Access Point
    Serial.println("Configuring access point...");
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(APssid, APpass);

    IPAddress myIP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(myIP);

    //start HTTP server

    //edit pages
    server.on("/list", HTTP_GET, handleFileList);
    server.on("/edit", HTTP_GET, []() {
      if (!handleFileRead("/edit.htm"))
        server.send(404, "text/plain", "FileNotFound");
    });
    server.on("/edit", HTTP_PUT, handleFileCreate);
    server.on("/edit", HTTP_DELETE, handleFileDelete);
    server.on("/edit", HTTP_POST, []() {
      server.send(200, "text/plain", "");
    }, handleFileUpload);

    //pages from SPIFFS
    server.onNotFound([]() {
      if (!handleFileRead(server.uri()))
      {
        server.send(404, "text/plain", "FileNotFound");
      }
    });

    server.begin();
    Serial.println("HTTP server started");
  }
  else
  {
    //start Normal Mode

    //connect to WiFi
    Serial.println();
    Serial.print("Connecting to ");
    Serial.println(ssid);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    while (WiFi.status() != WL_CONNECTED)
    {
      delay(500);
      Serial.print(".");
      //if we are taking too long to connect to WiFi give up.
      failCount++;
      if (failCount == wc_p * 2)
      {
        Serial.println("Session Terminated. Giving up after 21 tries connecting to WiFi.");
        Serial.println("entering deep sleep");
        delay(20);
        fail();
        gotoSleep();
      }
    }

    Serial.println("");
    Serial.println("WiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    failCount = 0;
  }
}

void addQueryParam(String &url, String queryName, String queryValue) {
  if (url.indexOf("?") > 0) {
    url += "&";
  } else {
    url += "?";
  }
  url += queryName;
  url += "=";
  url += queryValue;
}

void loop()
{
  if (su_mode)
  {
    server.handleClient();
  }
  else
  {
    //if we have tried too many times to make a GET Request give up.
    ++failCount;
    if (failCount == gr_p + 1)
    {
      Serial.println("Session Terminated. Giving up after 10 tries doing GET Request.");
      Serial.println("entering deep sleep");
      delay(20);
      fail();
      gotoSleep();
    }

    Serial.print("Try: ");
    Serial.println(failCount);
    Serial.print("connecting to ");
    Serial.println(host);

    //try to connect to the host with TCP
    WiFiClient client;
    const int httpPort = 80;
    if (is_ip)
    {
      IPAddress addr;
      if (addr.fromString(host))
      {
        if (!client.connect(addr, httpPort))
        {
          //try again if the connection fails.
          Serial.println("connection to IP failed");
          delay(10);
          return;
        }
      }
      else
      {
        Serial.println("failed to convert IP String to IP Address.");
        while (1)
          ;
      }
    }
    else
    {
      if (!client.connect(host.c_str(), httpPort))
      {
        //try again if the connection fails.
        Serial.println("connection failed");
        delay(10);
        return;
      }
    }

    //create the URI for the request
    if (s_vcc) {
      uint32_t getVcc = ESP.getVcc();
      String vccVol = String((getVcc / 1000U) % 10) + "." + String((getVcc / 100U) % 10) + String((getVcc / 10U) % 10) + String((getVcc / 1U) % 10);
      addQueryParam(url, vcc_parm, vccVol);
    }
    if (s_temp) {
      addQueryParam(url, temp_parm, String(dht.readTemperature()));
    }
    if (s_humidity) {
      addQueryParam(url, humidity_parm, String(dht.readHumidity()));
    }

    //request url to server
    Serial.print("Requesting URL: ");
    Serial.println(url);

    client.print(String("GET ") + url + " HTTP/1.1\r\n" +
                 "Host: " + host.c_str() + "\r\n" +
                 "Connection: close\r\n\r\n");

    unsigned long timeout = millis();
    while (client.available() == 0)
    {
      if (millis() - timeout > 60000)
      {
        //give up if the server takes too long to reply.
        Serial.println(">>> Client Timeout !");
        client.stop();
        //return;
        Serial.println("entering deep sleep");
        gotoSleep();
      }
    }

    //print response to serial
    while (client.available())
    {
      String line = client.readStringUntil('\r');
      Serial.print(line);
    }
    //finish request and close connections
    client.stop();
    Serial.println();
    Serial.println("closing connection");

    //enter Deep Sleep
    Serial.println("entering deep sleep");
    delay(100);
    yay();
    gotoSleep();
  }
}

/*
   Universal Functions
*/

void fail()
{
  //something has gone wrong, blink an indicator on the LED.
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  delay(250);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(250);
  digitalWrite(LED_BUILTIN, LOW);
  delay(250);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(250);
  digitalWrite(LED_BUILTIN, LOW);
  delay(250);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(250);
  digitalWrite(LED_BUILTIN, LOW);
  delay(250);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(250);
  digitalWrite(LED_BUILTIN, LOW);
  delay(250);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(250);
  digitalWrite(LED_BUILTIN, LOW);
  delay(500);
  digitalWrite(LED_BUILTIN, HIGH);
}

void yay()
{
  //everything worked, blink an indicator on the LED.
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  delay(1500);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(250);
  digitalWrite(LED_BUILTIN, LOW);
  delay(350);
  digitalWrite(LED_BUILTIN, HIGH);
}

void readConfig()
{
  //read config.json and load configuration to variables.
  File configFile = SPIFFS.open("/config.jsn", "r");
  if (!configFile)
  {
    Serial.println("Failed to open config file");
    while (1)
      ;
  }
  size_t size = configFile.size();
  
  Serial.print("Config File Size: ");
  Serial.println(String (size));
  
  if (size > 1024)
  {
    Serial.println("Config file size is too large");
    while (1)
      ;
  }
  std::unique_ptr<char[]> buf(new char[size]);
  configFile.readBytes(buf.get(), size);
  StaticJsonBuffer<450> jsonBuffer;
  JsonObject &json = jsonBuffer.parseObject(buf.get());
  if (!json.success())
  {
    Serial.println("Failed to parse config file");
    while (1)
      ;
  }

  ssid = (const char *)json["ssid"];
  password = (const char *)json["pass"];
  host = (const char *)json["host"];
  url = (const char *)json["uri"];
  wc_p = json["wc_p"];
  gr_p = json["gr_p"];
  reporting_interval_mins = json["reporting_interval_mins"];
  s_vcc = json["s_vcc"];
  s_temp = json["s_temp"];
  s_humidity = json["s_humidity"];
  is_ip = json["is_ip"];
  vcc_parm = (const char *)json["vcc_p"];
  temp_parm = (const char *)json["temp_p"];
  humidity_parm = (const char *)json["humidity_p"];

  Serial.println("Parsed JSON Config.");
  Serial.print("Loaded ssid: ");
  Serial.println(ssid);
  Serial.print("Loaded password: ");
  Serial.println(password);
  Serial.print("Loaded host: ");
  Serial.println(host);
  Serial.print("Loaded IsIP: ");
  Serial.println(is_ip);
  Serial.print("Loaded uri: ");
  Serial.println(url);
  Serial.print("Loaded WiFi Connect Persistance: ");
  Serial.println(wc_p);
  Serial.print("Loaded GET Request Persistance: ");
  Serial.println(gr_p);
  Serial.print("Loaded Reporting Interval (mins): ");
  Serial.println(reporting_interval_mins);
  Serial.print("Loaded Send VCC: ");
  Serial.println(s_vcc);
  Serial.print("Loaded VCC Param.: ");
  Serial.println(vcc_parm);
  Serial.print("Loaded Send Temp: ");
  Serial.println(s_temp);
  Serial.print("Loaded Temp Param.: ");
  Serial.println(temp_parm);
  Serial.print("Loaded Send Humidity: ");
  Serial.println(s_humidity);
  Serial.print("Loaded Humidity Param.: ");
  Serial.println(humidity_parm);
  Serial.println();
}

/*
   Config. Mode Functions
*/

//edit functions
String formatBytes(size_t bytes)
{
  if (bytes < 1024)
  {
    return String(bytes) + "B";
  }
  else if (bytes < (1024 * 1024))
  {
    return String(bytes / 1024.0) + "KB";
  }
  else if (bytes < (1024 * 1024 * 1024))
  {
    return String(bytes / 1024.0 / 1024.0) + "MB";
  }
  else
  {
    return String(bytes / 1024.0 / 1024.0 / 1024.0) + "GB";
  }
}

String getContentType(String filename)
{
  if (server.hasArg("download"))
    return "application/octet-stream";
  else if (filename.endsWith(".htm"))
    return "text/html";
  else if (filename.endsWith(".html"))
    return "text/html";
  else if (filename.endsWith(".css"))
    return "text/css";
  else if (filename.endsWith(".js"))
    return "application/javascript";
  else if (filename.endsWith(".png"))
    return "image/png";
  else if (filename.endsWith(".gif"))
    return "image/gif";
  else if (filename.endsWith(".jpg"))
    return "image/jpeg";
  else if (filename.endsWith(".ico"))
    return "image/x-icon";
  else if (filename.endsWith(".xml"))
    return "text/xml";
  else if (filename.endsWith(".pdf"))
    return "application/x-pdf";
  else if (filename.endsWith(".zip"))
    return "application/x-zip";
  else if (filename.endsWith(".gz"))
    return "application/x-gzip";
  return "text/plain";
}

bool handleFileRead(String path)
{
  Serial.println("handleFileRead: " + path);
  if ((path == "/") && (server.argName(0) == "restart" && server.arg(0) == "true"))
  {
    Serial.println(F("requested reset from admin page!"));
    server.send(200, "text/plain", "Restarting!");
#ifdef NOT_DEBUG
    digitalWrite(LED_BUILTIN, HIGH);
#endif
    delay(200);
    wdt_reset();
    ESP.restart();
    while (1)
    {
      wdt_reset();
    }
    //WiFi.forceSleepBegin(); wdt_reset(); ESP.restart(); while (1)wdt_reset();
  }
  if (path.endsWith("/"))
    path += "index.htm";
  String contentType = getContentType(path);
  String pathWithGz = path + ".gz";
  if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path))
  {
    if (SPIFFS.exists(pathWithGz))
      path += ".gz";
    File file = SPIFFS.open(path, "r");
    size_t sent = server.streamFile(file, contentType);
    file.close();
    return true;
  }
  return false;
}

void handleFileUpload()
{
  if (server.uri() != "/edit")
    return;
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START)
  {
    String filename = upload.filename;
    if (!filename.startsWith("/"))
      filename = "/" + filename;
    Serial.print("handleFileUpload Name: ");
    Serial.println(filename);
    fsUploadFile = SPIFFS.open(filename, "w");
    filename = String();
  }
  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    //Serial.print("handleFileUpload Data: "); Serial.println(upload.currentSize);
    if (fsUploadFile)
      fsUploadFile.write(upload.buf, upload.currentSize);
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    if (fsUploadFile)
      fsUploadFile.close();
    Serial.print("handleFileUpload Size: ");
    Serial.println(upload.totalSize);
  }
}

void handleFileDelete()
{
  if (server.args() == 0)
    return server.send(500, "text/plain", "BAD ARGS");
  String path = server.arg(0);
  Serial.println("handleFileDelete: " + path);
  if (path == "/")
    return server.send(500, "text/plain", "BAD PATH");
  if (!SPIFFS.exists(path))
    return server.send(404, "text/plain", "FileNotFound");
  SPIFFS.remove(path);
  server.send(200, "text/plain", "");
  path = String();
}

void handleFileCreate()
{
  if (server.args() == 0)
    return server.send(500, "text/plain", "BAD ARGS");
  String path = server.arg(0);
  Serial.println("handleFileCreate: " + path);
  if (path == "/")
    return server.send(500, "text/plain", "BAD PATH");
  if (SPIFFS.exists(path))
    return server.send(500, "text/plain", "FILE EXISTS");
  File file = SPIFFS.open(path, "w");
  if (file)
    file.close();
  else
    return server.send(500, "text/plain", "CREATE FAILED");
  server.send(200, "text/plain", "");
  path = String();
}

void handleFileList()
{
  if (!server.hasArg("dir"))
  {
    server.send(500, "text/plain", "BAD ARGS");
    return;
  }

  String path = server.arg("dir");
  Serial.println("handleFileList: " + path);
  Dir dir = SPIFFS.openDir(path);
  path = String();

  String output = "[";
  while (dir.next())
  {
    File entry = dir.openFile("r");
    if (output != "[")
      output += ',';
    bool isDir = false;
    output += "{\"type\":\"";
    output += (isDir) ? "dir" : "file";
    output += "\",\"name\":\"";
    output += String(entry.name()).substring(1);
    output += "\"}";
    entry.close();
  }

  output += "]";
  server.send(200, "text/json", output);
}
