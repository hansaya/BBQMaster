#include <FS.h>
#include <SPIFFS.h>
#include <Adafruit_ADS1015.h>
#include <DNSServer.h>
#include <OneWire.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncWiFiManager.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <SimpleList.h>
#include <DallasTemperature.h>
#include <ArduinoOTA.h>
#include <NtpClientLib.h>
#include <PubSubClient.h>

// #define DEBUG_SERIAL
// #define DEBUG_TELNET
// #define DEBUG_EXTRA_OUTPUT

#define I2C_SDA_PIN 23
#define I2C_SCL_PIN 22
#define BATTERY_V_PIN 35

#define BAT_RATIO 0.0017240449438202
#define NTC_MAX_ADC 27550.00
#define NTC_REF_RES 62000
#define NTC_SMP_TMP 25.81
#define NTC_SMP_RES 52250
#define NTC_BCOEFFICIENT 4200 //3950
#define HIST_SIZE 20
#define HIST_INT 5000
#define SENSOR_READ_INT 1500
#define MQTT_PUBLISH_PERIOD 10000

// Macros for debugging
#ifdef DEBUG_TELNET
   #define     DEBUG_TELNET_PORT 23
   WiFiServer  telnetServer(DEBUG_TELNET_PORT);
   WiFiClient  telnetClient;
   #define     DEBUG_PRINT(x)    telnetClient.print(x)
   #define     DEBUG_PRINT_WITH_FMT(x, fmt)    telnetClient.printf(x, fmt)
   #define     DEBUG_PRINTLN(x)  telnetClient.println(x)
   #define     DEBUG_PRINTLN_WITH_FMT(x, fmt)  telnetClient.println(x, fmt)
#elif defined(DEBUG_SERIAL)
   #define     DEBUG_PRINT(x)    Serial.print(x)
   #define     DEBUG_PRINT_WITH_FMT(x, fmt)    Serial.printf(x, fmt)
   #define     DEBUG_PRINTLN(x)  Serial.println(x)
   #define     DEBUG_PRINTLN_WITH_FMT(x, fmt)  Serial.println(x, fmt)
#else
   #define     DEBUG_PRINT(x)
   #define     DEBUG_PRINT_WITH_FMT(x, fmt)
   #define     DEBUG_PRINTLN(x)
   #define     DEBUG_PRINTLN_WITH_FMT(x, fmt)
#endif

Adafruit_ADS1115 g_ads (0x49); /* Use this for the 16-bit version */
AsyncWebServer g_server (80);
OneWire g_oneWireTemp (17);
DallasTemperature g_thermoCouples (&g_oneWireTemp);
DNSServer dns;

// MAX31850KATB+-ND
DeviceAddress g_sensor1 = {0x3B, 0x45, 0x21, 0x18, 0x00, 0x00, 0x00, 0x2D};
DeviceAddress g_sensor2 = {0x3B, 0xF1, 0x22, 0x18, 0x00, 0x00, 0x00, 0xB8};
DeviceAddress g_sensor3 = {0x3B, 0xF3, 0x22, 0x18, 0x00, 0x00, 0x00, 0xD6};
DeviceAddress g_sensor4 = {0x3B, 0x43, 0x21, 0x18, 0x00, 0x00, 0x00, 0x9F};

double g_batteryVoltage = 0;
bool g_ntpError = false;
short g_maxWorkingSensors = 0;

// Host name of the device.
const char* g_hostName= "BBQ_Master";

// MQTT stuff
WiFiClient espClient;
PubSubClient g_mqttClient(espClient);

// MQTT configs
char g_mqtt_server[40] = "example.com";
char g_mqtt_port[6] = "1883";
char g_topicMQTTHeader[22 + 11];
char g_topicAvailability[22 + 11 + 10];
char g_topicState[22 + 11 + 10];
char g_topicMQTTDiscover[22 + 11 + 10];

// Representation of one temperature probe
struct Sensor
{
   bool m_ind;
   double m_tempF;
   String m_name;

public:
   Sensor (bool ind, double tempF, String name) : m_ind (ind), m_tempF (tempF), m_name (name) {}
   Sensor () : m_ind (false), m_tempF (0), m_name ("N/A") {}
};

// Data point completed with multiple probes
struct DataPoint
{
   int m_time;
   Sensor m_sensors[8];
};

// Current data set
DataPoint g_lastSenUpdate;

// Saving the history for the histogram
SimpleList<DataPoint> g_tempHist;

// Flag for saving data from wifi portal
bool g_shouldSaveConfig = false;

// Handle telnet debuging
#if defined(DEBUG_TELNET)
void handleTelnet(void) {
  if (telnetServer.hasClient()) {
    if (!telnetClient || !telnetClient.connected()) {
      if (telnetClient) {
        telnetClient.stop();
      }
      telnetClient = telnetServer.available();
    } else {
      telnetServer.available().stop();
    }
  }
}
#endif

// Setup OTA
void setupOTA(const char *hostname)
{
  // Config OTA updates
  ArduinoOTA.onStart([]() { DEBUG_PRINTLN("Start"); });
  ArduinoOTA.onEnd([]() { DEBUG_PRINTLN("\nEnd"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) { DEBUG_PRINT_WITH_FMT("Progress: %u%%\r", (progress / (total / 100))); });
  ArduinoOTA.onError([](ota_error_t error) {
    DEBUG_PRINT_WITH_FMT("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) DEBUG_PRINTLN("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) DEBUG_PRINTLN("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) DEBUG_PRINTLN("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) DEBUG_PRINTLN("Receive Failed");
    else if (error == OTA_END_ERROR) DEBUG_PRINTLN("End Failed");
  });
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.begin();  
}

// Saving the config to SPIFF
void SaveConfig () {
  DynamicJsonDocument json (256);
  json["mqtt_server"] = g_mqtt_server;
  json["mqtt_port"] = g_mqtt_port;

  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
    DEBUG_PRINTLN("failed to open config file for writing");
  }
  serializeJson(json, configFile);
  configFile.close();
}

// Reading the config from SPIFF
void ReadConfig ()
{
  if (SPIFFS.begin()) {
      DEBUG_PRINTLN("SPIFFS started");
      if (SPIFFS.exists("/config.json")) {
         //file exists, reading and loading
         DEBUG_PRINTLN("reading config file");
         File configFile = SPIFFS.open("/config.json", "r");
         if (configFile) {
            DEBUG_PRINTLN("opened config file");
            size_t size = configFile.size();
            // Allocate a buffer to store contents of the file.
            std::unique_ptr<char[]> buf(new char[size]);

            configFile.readBytes(buf.get(), size);
            DynamicJsonDocument json (size+1);
            DeserializationError error = deserializeJson(json, buf.get());
            if (!error) {
               DEBUG_PRINT("parsed json config: ");
               serializeJson(json, Serial);
               DEBUG_PRINTLN();
               strcpy(g_mqtt_server, json["mqtt_server"]);
               strcpy(g_mqtt_port, json["mqtt_port"]);

            } else {
               DEBUG_PRINTLN("failed to load json config");
               SaveConfig ();
            }
         }
      }
      else
      {
         SaveConfig ();
      }
  } else {
      DEBUG_PRINTLN("Formatting the flash...");
      SPIFFS.format();
      DEBUG_PRINTLN("SPIFFS Mount failed");
  }
}

// Call back for saving the config.
void SaveConfigCallback () {
  DEBUG_PRINTLN("Should save config");
  g_shouldSaveConfig = true;
}

// Manages wifi portal
void ManageWifi (bool reset_config = false)
{
  // The extra parameters to be configured (can be either global or just in the setup)
  AsyncWiFiManagerParameter custom_mqtt_server("server", "MQTT server", g_mqtt_server, 39);
  AsyncWiFiManagerParameter custom_mqtt_port("port", "MQTT port", g_mqtt_port, 5);

  AsyncWiFiManager wifiManager(&g_server, &dns);
  wifiManager.setConnectTimeout(60);
  wifiManager.setConfigPortalTimeout(60);
  wifiManager.setMinimumSignalQuality(10);
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.setSaveConfigCallback(&SaveConfigCallback);
  
  if (reset_config)
    wifiManager.startConfigPortal(g_hostName);
  else
    wifiManager.autoConnect(g_hostName); // This will hold the connection till it get a connection

  if (g_shouldSaveConfig)
  {
      strncpy(g_mqtt_server, custom_mqtt_server.getValue(), 39);
      strncpy(g_mqtt_port, custom_mqtt_port.getValue(), 5);
      SaveConfig ();
      g_shouldSaveConfig = false;
  }
}

// Calculate NTC temp using steinhart equation
float NTCTemp (int16_t adcValue)
{
  if (adcValue < NTC_MAX_ADC)
  {
    float steinhart;
    // Resistance calculation
    steinhart = (NTC_MAX_ADC / adcValue) - 1;
    steinhart = NTC_REF_RES / steinhart;

    // Rest of the steinhart equation
    steinhart = steinhart / NTC_SMP_RES;
    steinhart = log(steinhart);                  // ln(R/Ro)
    steinhart /= NTC_BCOEFFICIENT;                   // 1/B * ln(R/Ro)
    steinhart += 1.0 / (NTC_SMP_TMP + 273.15); // + (1/To)
    steinhart = 1.0 / steinhart;                 // Invert
    steinhart -= 273.15;
    return DallasTemperature::toFahrenheit(steinhart);
  }
  else
    return 0.0;
}

// Check whether temperature is valid.
bool IsValidTemp (float temp)
{
   if (temp <= 32.0 || temp > 1000.00)
      return false;
   else
      return true;
}

// Read all the sensors
void ReadSensors ()
{

   int16_t adc0, adc1, adc2, adc3;
   float ntc0, ntc1, ntc2, ntc3;
   // NTC readings
   adc0 = g_ads.readADC_SingleEnded (0);
   adc1 = g_ads.readADC_SingleEnded (1);
   adc2 = g_ads.readADC_SingleEnded (2);
   adc3 = g_ads.readADC_SingleEnded (3);

   ntc0 = NTCTemp (adc0);
   ntc1 = NTCTemp (adc1);
   ntc2 = NTCTemp (adc2);
   ntc3 = NTCTemp (adc3);

   float tcp0, tcp1, tcp2, tcp3;
   tcp0 = g_thermoCouples.getTempF (g_sensor1);
   tcp1 = g_thermoCouples.getTempF (g_sensor2);
   tcp2 = g_thermoCouples.getTempF (g_sensor3);
   tcp3 = g_thermoCouples.getTempF (g_sensor4);

   long int tps = now ();
   if (tps > 0)
   {
      g_lastSenUpdate.m_time = tps;
      g_lastSenUpdate.m_sensors[0] = Sensor (IsValidTemp (ntc0), ntc0, "NTC1");
      g_lastSenUpdate.m_sensors[1] = Sensor (IsValidTemp (ntc1), ntc1, "NTC2");
      g_lastSenUpdate.m_sensors[2] = Sensor (IsValidTemp (ntc2), ntc2, "NTC3");
      g_lastSenUpdate.m_sensors[3] = Sensor (IsValidTemp (ntc3), ntc3, "NTC4");

      // Thermocouple readings
      g_lastSenUpdate.m_sensors[4] = Sensor (IsValidTemp (tcp0), tcp0, "TC1");
      g_lastSenUpdate.m_sensors[5] = Sensor (IsValidTemp (tcp1), tcp1, "TC2");
      g_lastSenUpdate.m_sensors[6] = Sensor (IsValidTemp (tcp2), tcp2, "TC3");
      g_lastSenUpdate.m_sensors[7] = Sensor (IsValidTemp (tcp3), tcp3, "TC4");
   }

   g_batteryVoltage = analogRead (BATTERY_V_PIN) * BAT_RATIO;

   // Request thermo couple data
   g_thermoCouples.setWaitForConversion(false);  // makes it async
   g_thermoCouples.requestTemperatures();
   g_thermoCouples.setWaitForConversion(true);

#ifdef DEBUG_EXTRA_OUTPUT
   DEBUG_PRINT("AIN0: ");
   DEBUG_PRINTLN(adc0);

   DEBUG_PRINT("Temp NTC 0 F: ");
   DEBUG_PRINTLN(g_lastSenUpdate.m_sensors[0].m_tempF);
   DEBUG_PRINT("Temp NTC 1 F: ");
   DEBUG_PRINTLN(g_lastSenUpdate.m_sensors[1].m_tempF);
   DEBUG_PRINT("Temp NTC 2 F: ");
   DEBUG_PRINTLN(g_lastSenUpdate.m_sensors[2].m_tempF);
   DEBUG_PRINT("Temp NTC 3 F: ");
   DEBUG_PRINTLN(g_lastSenUpdate.m_sensors[3].m_tempF);

   DEBUG_PRINT("Temp 0 F: ");
   DEBUG_PRINTLN(tcp0);
   DEBUG_PRINT("Temp 1 F: ");
   DEBUG_PRINTLN(tcp1);
   DEBUG_PRINT("Temp 2 F: ");
   DEBUG_PRINTLN(tcp2);
   DEBUG_PRINT("Temp 3 F: ");
   DEBUG_PRINTLN(tcp3);

#endif
}

// Send the last sensor data reading in json format.
DynamicJsonDocument jsonSensorData ()
{
   DynamicJsonDocument json(1024);  // Current JSON static buffer
   json["bat"] = g_batteryVoltage;
   json["t"] = g_lastSenUpdate.m_time;
   JsonArray sensors = json.createNestedArray ("sensors");

   for (int i = 0; i < 8; ++i)
   {
      JsonObject tempS = sensors.createNestedObject ();
      tempS["i"] = g_lastSenUpdate.m_sensors[i].m_ind;
      tempS["v"] = int(g_lastSenUpdate.m_sensors[i].m_tempF);
      tempS["n"] = g_lastSenUpdate.m_sensors[i].m_name;
   }

   return json;
}


// Send the last sensor data reading in json format.
void SendMeasures (AsyncWebServerRequest *request)
{
   char tempJson[1024];
   serializeJson(jsonSensorData (), tempJson);
   request->send (200, "application/json", tempJson);  // Send history data to the web client
}


// Send the history in JSON format.
void SendHistory (AsyncWebServerRequest *request)
{
   DEBUG_PRINTLN("Sending History");
   DynamicJsonDocument json(5560 + 100);  // Current JSON static buffer
   JsonArray hist = json.createNestedArray ("hist");

   for (SimpleList<DataPoint>::iterator it = g_tempHist.begin (); it != g_tempHist.end (); ++it)
   {
      JsonObject item = hist.createNestedObject ();
      item["t"] = it->m_time;
      JsonArray jsonSens = item.createNestedArray ("sensors");

      for (int i = 0; i < 8; i++)
      {
         if (it->m_sensors[i].m_ind)
         {
            JsonObject sensor = jsonSens.createNestedObject ();
            sensor["n"] = it->m_sensors[i].m_name;
            sensor["v"] = int(it->m_sensors[i].m_tempF);  // Lose the decimal places for space saving.
         }
      }
   }
   char tempJson[5560 + 100];
   serializeJson(json, tempJson); // Export JSON object as a String
   request->send (200, "application/json", tempJson);  // Send history data to the web client
}

// Add a data point to the history
void AddDataPointToHistory ()
{
  short validSensors = 0;
  bool validData = false;
  for (int i = 0; i < 8; i++)
  {
     if (g_lastSenUpdate.m_sensors[i].m_ind)
     {
       validSensors++;
       validData = true;
     }
  }
  // Clear the history if the count does not match.
  if (g_maxWorkingSensors != validSensors)
  {
    g_tempHist.clear ();
    g_maxWorkingSensors = validSensors;
  }

  if (validData)
  {
   g_tempHist.push_back (g_lastSenUpdate);
   if (g_tempHist.size () > HIST_SIZE)
   {
      g_tempHist.erase (g_tempHist.begin ());
   }
  }

#ifdef DEBUG_EXTRA_OUTPUT
   DEBUG_PRINT ("size g_tempHist ");
   DEBUG_PRINTLN(g_tempHist.size ());
#endif
}

void publishToMQTT(const char* p_topic,const char* p_payload) {
  if (g_mqttClient.publish(p_topic, p_payload, true)) {
    DEBUG_PRINT(F("INFO: MQTT message published successfully, topic: "));
    DEBUG_PRINT(p_topic);
    DEBUG_PRINT(F(", payload: "));
    DEBUG_PRINTLN(p_payload);
  } else {
    DEBUG_PRINTLN(F("ERROR: MQTT message not published, either connection lost, or message too large. Topic: "));
    DEBUG_PRINT(p_topic);
    DEBUG_PRINT(F(" , payload: "));
    DEBUG_PRINTLN(p_payload);
  }
}

// Function that publishes birthMessage
void publishAvailability() 
{
   g_mqttClient.publish(g_topicAvailability, "online", true);
}

// Function that publishes availability of each sensor
void publishSensorAvailability() 
{
   for (int i = 0; i < 8; i++)
   {
      char availabilitySensorTopic[22 + 11 + 20];
      snprintf(availabilitySensorTopic, 22 + 11 + 20, "%s/%s/avail", g_topicMQTTHeader, g_lastSenUpdate.m_sensors[i].m_name.c_str());
      if (g_lastSenUpdate.m_sensors[i].m_ind)
         publishToMQTT(availabilitySensorTopic, "online");
      else
         publishToMQTT(availabilitySensorTopic, "offline");
   }
}

// publish MQTT discovery config to let hassio auto discover the sensors.
void publishDiscovery(void) 
{
   // Create json config for battery level.
   StaticJsonDocument<220> root;
   root["dev_cla"] = "battery";
   char batName[30];
   snprintf(batName, 30, "%s Battery.", g_hostName);
   root["name"] = batName;
   root["avty_t"] = g_topicAvailability;
   root["stat_t"] = g_topicState;
   root["unit_of_meas"] = "V";
   root["val_tpl"] = "{{value_json.bat}}";
   char outgoingJsonBuffer[220];
   serializeJson(root, outgoingJsonBuffer);
   publishToMQTT(g_topicMQTTDiscover, outgoingJsonBuffer);

   // Create json config for each sensor.
   for (int i = 0; i < 8; i++)
   {
      char sensorTopicHeader[22 + 11 + 20];
      snprintf(sensorTopicHeader, 22 + 11 + 20, "%s/%s/config", g_topicMQTTHeader, g_lastSenUpdate.m_sensors[i].m_name.c_str());

      StaticJsonDocument<250> sensorRoot;
      sensorRoot["dev_cla"] = "temperature";
      char sensorName[30];
      snprintf(sensorName, 30, "%s %s Temperature.", g_hostName, g_lastSenUpdate.m_sensors[i].m_name.c_str());
      sensorRoot["name"] = sensorName;
      char availabilitySensorTopic[50];
      snprintf(availabilitySensorTopic, 22 + 11 + 20, "%s/%s/avail", g_topicMQTTHeader, g_lastSenUpdate.m_sensors[i].m_name.c_str());
      sensorRoot["avty_t"] = availabilitySensorTopic;
      sensorRoot["stat_t"] = g_topicState;
      sensorRoot["unit_of_meas"] = "°F";
      String jsonTemplate = "{{value_json.sensors[" + String(i) + "].v}}";
      sensorRoot["val_tpl"] = jsonTemplate;
      sensorRoot["exp_aft"] = MQTT_PUBLISH_PERIOD/1000 + 5;

      char sensorDisBuffer[250];
      serializeJson(sensorRoot, sensorDisBuffer);
      publishToMQTT(sensorTopicHeader, sensorDisBuffer);
   }
}

// MQTT Connect.
void connectToMqtt() 
{
  DEBUG_PRINT("Connecting to MQTT with client id ");
  String clientId = "BBQMaster_";
  clientId += String(random(0xffff), HEX);
  DEBUG_PRINT(clientId);
  DEBUG_PRINTLN("...");
  // Attempt to connect
  if (g_mqttClient.connect(clientId.c_str(), "", "", g_topicAvailability, 0, true, "offline"))
  {
      publishAvailability();
      publishDiscovery();
  }
  else
  {
      DEBUG_PRINT("Failed to connect to MQTT! ");
      DEBUG_PRINT(g_mqttClient.state());
      DEBUG_PRINTLN(" Trying again in 5 seconds");
  }
}

// Publish MQTT states
void publishMqtt()
{
   char tempJson[1024];
   serializeJson(jsonSensorData (), tempJson);
   publishToMQTT (g_topicState, tempJson);
   publishSensorAvailability();
}

void setup (void)
{
   Serial.begin(115200);

   // For battery voltage readings
   analogReadResolution (12);
   analogSetAttenuation (ADC_11db);  // For all pins

   // Temp probes
   g_ads.setGain (GAIN_ONE);  // 1x gain   +/- 4.096V  1 bit = 2mV      0.125mV
   g_ads.begin ();
   g_thermoCouples.begin ();
   g_thermoCouples.requestTemperatures ();
   g_thermoCouples.setResolution (12);

   // Read mqtt config from SPIFF
   ReadConfig ();

   // WIFI connect
   WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
   WiFi.setHostname(g_hostName);
   WiFi.mode(WIFI_STA);
   ManageWifi ();

#if defined(DEBUG_TELNET)
   telnetServer.begin();
   telnetServer.setNoDelay(true);
#endif
   DEBUG_PRINT("Starting ");
   DEBUG_PRINTLN(g_hostName);

   // OTA stuff
   setupOTA (g_hostName);

   // Web server stuff
   if (!SPIFFS.begin ())
      DEBUG_PRINTLN("SPIFFS Mount failed");  // Problème avec le stockage SPIFFS - Serious problem with SPIFFS
   g_server.on ("/measures.json", SendMeasures);
   g_server.on ("/history.json", SendHistory);

   g_server.serveStatic ("/js/bootstrap.min.js", SPIFFS, "/js/bootstrap.min.js", "max-age=86400");
   g_server.serveStatic ("/js/jquery.min.js", SPIFFS, "/js/jquery-3.3.1.min.js", "max-age=86400");
   g_server.serveStatic ("/js/Chart.min.js", SPIFFS, "/js/Chart.min.js", "max-age=86400");
   g_server.serveStatic ("/js/moment.min.js", SPIFFS, "/js/moment.min.js", "max-age=86400");
   g_server.serveStatic ("/js/popper.min.js", SPIFFS, "/js/popper.min.js", "max-age=86400");
   g_server.serveStatic ("/css/custom.css", SPIFFS, "/css/custom.css");
   g_server.serveStatic ("/css/cover.css", SPIFFS, "/css/cover.css", "max-age=86400");
   g_server.serveStatic ("/css/bootstrap.min.css", SPIFFS, "/css/bootstrap.min.css", "max-age=86400");
   g_server.serveStatic ("/img/logo.png", SPIFFS, "/img/android-icon-72x72.png", "max-age=86400");
   g_server.serveStatic ("/favicon.ico", SPIFFS, "/img/favicon.ico", "max-age=86400");
   g_server.serveStatic ("/index.html", SPIFFS, "/index.html");
   g_server.serveStatic ("/", SPIFFS, "/").setDefaultFile("index.html");
   g_server.begin ();
   DEBUG_PRINTLN("HTTP server started");

   // Real time
   NTP.onNTPSyncEvent ([](NTPSyncEvent_t error) {
      if (error)
      {
         if (error == noResponse)
            DEBUG_PRINTLN("NTP server not reachable");
         else if (error == invalidAddress)
            DEBUG_PRINTLN("Invalid NTP server address");
         g_ntpError = true;
      }
      else
      {
         DEBUG_PRINT("Got NTP time: ");
         DEBUG_PRINTLN(NTP.getTimeDateString (NTP.getLastNTPSync ()));
      }
   });
   // NTP Server, time offset, daylight
   NTP.begin ("pool.ntp.org", -1, true);
   NTP.setInterval (240);

   // MQTT Config
   snprintf(g_topicMQTTHeader, 22 + 11, "homeassistant/sensor/%s", g_hostName);
   snprintf(g_topicAvailability, 22 + 11 + 10, "%s/avail",g_topicMQTTHeader);
   snprintf(g_topicState, 22 + 11 + 10, "%s/state", g_topicMQTTHeader);
   snprintf(g_topicMQTTDiscover, 22 + 11 + 10, "%s/config", g_topicMQTTHeader);
   g_mqttClient.setServer(g_mqtt_server, atoi(g_mqtt_port));
   g_mqttClient.setBufferSize(512);
   randomSeed(micros());
}

void loop (void)
{
// handle the Telnet connection
#if defined(DEBUG_TELNET)
  handleTelnet();
#endif

   unsigned long currentMillis = millis ();  // Time now
   // Handle server requests
   ArduinoOTA.handle ();

   // Connect to MQTT server
   if (!g_mqttClient.connected())
   {
      static unsigned long mqttConnectWaitPeriod;
      if (currentMillis - mqttConnectWaitPeriod >= 5000)
      {
         mqttConnectWaitPeriod = currentMillis;
         connectToMqtt ();
      }
   }   
   g_mqttClient.loop();

   // Add a point to the history over time.
   static unsigned long histPreviousMillis;
   if (currentMillis - histPreviousMillis > HIST_INT)
   {
      histPreviousMillis = currentMillis;
      AddDataPointToHistory ();
   }

   // Read the sensor data.
   static unsigned long prevMilForInputRead;
   if (currentMillis - prevMilForInputRead >= SENSOR_READ_INT)
   {
      prevMilForInputRead = currentMillis;
      ReadSensors ();
   }

   // Send MQTT state.
   static unsigned long prevMilForMqttPublish;
   if (currentMillis - prevMilForMqttPublish >= MQTT_PUBLISH_PERIOD)
   {
      prevMilForMqttPublish = currentMillis;
      publishMqtt ();
   }
}


