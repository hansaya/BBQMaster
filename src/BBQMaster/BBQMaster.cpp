
#include <Adafruit_ADS1015.h>
#include <DNSServer.h>
#include <OneWire.h>
// #include <WebServer.h>
// #include <WiFiManager.h>
//needed for library
#include <ESPAsyncWebServer.h>
#include <ESPAsyncWiFiManager.h>         //https://github.com/tzapu/WiFiManager

#include <Wire.h>
#include <ArduinoJson.h>
#include <SimpleList.h>
#include "DallasTemperature.h"

// Remote firmware update
#include <ArduinoOTA.h>

#include <FS.h>
#include <NtpClientLib.h>
#include <Time.h>
#include <TimeLib.h>
#include <WiFi.h>
#include "SPIFFS.h"

#define DEBUG

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
#define NTP_RETRY_INT 5000

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

unsigned long g_currentMillis = 0;
unsigned long g_prevMilForInputRead = 0;
unsigned long g_histPreviousMillis = 0;
unsigned long g_prevMilForNTPRetry = 0;
double g_batteryVoltage = 0;
bool g_ntpError = false;
short g_maxWorkingSensors = 0;

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

// Setup OTA
void setupOTA(char *hostname)
{
  // Config OTA updates
  ArduinoOTA.onStart([]() { Serial.println("Start"); });
  ArduinoOTA.onEnd([]() { Serial.println("\nEnd"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.begin();  
}

// Call back for saving the config.
void SaveConfigCallback () {
  Serial.println("Should save config");
  g_shouldSaveConfig = true;
}

// Manages wifi portal
void ManageWifi (bool reset_config = false)
{
//   WiFiManager wifiManager;
  AsyncWiFiManager wifiManager(&g_server, &dns);
  wifiManager.setConnectTimeout(60);
  wifiManager.setConfigPortalTimeout(60);
  wifiManager.setMinimumSignalQuality(10);
  wifiManager.setSaveConfigCallback(&SaveConfigCallback);
  
  if (reset_config)
    wifiManager.startConfigPortal("BBQ_Master");
  else
    wifiManager.autoConnect("BBQ_Master"); // This will hold the connection till it get a connection

  if (g_shouldSaveConfig)
  {
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

#ifdef DEBUG
   Serial.print ("AIN0: ");
   Serial.println (adc0);
   // Serial.print (" v: ");
   // Serial.println ((adc0 * 0.125) / 1000);
   // Serial.print ("AIN1: ");
   // Serial.print (adc1);
   // Serial.print (" v: ");
   // Serial.println ((adc1 * 0.125) / 1000);

  Serial.print ("Temp NTC 0 F: ");
  Serial.println (g_lastSenUpdate.m_sensors[0].m_tempF);
  Serial.print ("Temp NTC 1 F: ");
  Serial.println (g_lastSenUpdate.m_sensors[1].m_tempF);
  Serial.print ("Temp NTC 2 F: ");
  Serial.println (g_lastSenUpdate.m_sensors[2].m_tempF);
  Serial.print ("Temp NTC 3 F: ");
  Serial.println (g_lastSenUpdate.m_sensors[3].m_tempF);

   // Serial.print ("AIN2: ");
   // Serial.print (adc2);
   // Serial.print (" v: ");
   // Serial.println ((adc2 * 0.125) / 1000);
   // Serial.print ("AIN3: ");
   // Serial.print (adc3);
   // Serial.print (" v: ");
   // Serial.println ((adc3 * 0.125) / 1000);
   // Serial.println (" ");

   Serial.print ("Temp 0 F: ");
   Serial.println (tcp0);
   Serial.print ("Temp 1 F: ");
   Serial.println (tcp1);
   Serial.print ("Temp 2 F: ");
   Serial.println (tcp2);
   Serial.print ("Temp 3 F: ");
   Serial.println (tcp3);

   // Serial.print ("battery voltage: ");
   // Serial.print (g_batteryVoltage);
   // Serial.println ("v");
#endif
}

// Send the last sensor data reading in json format.
void SendMeasures (AsyncWebServerRequest *request)
{
   Serial.println ("Sending measurements!");
   DynamicJsonDocument json(1024);  // Current JSON static buffer
   // JsonObject& root = jsonBuffer.createObject ();
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

   char tempJson[2400];
   serializeJson(json, tempJson); // Export JSON object as a String
   request->send (200, "application/json", tempJson);  // Send history data to the web client
}


// Send the history in JSON format.
void SendHistory (AsyncWebServerRequest *request)
{
   Serial.println ("Sending History");
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

#ifdef DEBUG
   Serial.print ("size g_tempHist ");
   Serial.println (g_tempHist.size ());
#endif
}

void setup (void)
{
   Serial.begin (115200);

   // For battery voltage readings
   analogReadResolution (12);
   analogSetAttenuation (ADC_11db);  // For all pins

   // Temp probes
   g_ads.setGain (GAIN_ONE);  // 1x gain   +/- 4.096V  1 bit = 2mV      0.125mV
   g_ads.begin ();
   g_thermoCouples.begin ();
   g_thermoCouples.requestTemperatures ();
   g_thermoCouples.setResolution (12);

   // WIFI connect
   ManageWifi ();

   // OTA stuff
   setupOTA ("BBQ_Master");
   WiFi.setHostname("BBQ_Master");

   // Web server stuff
   if (!SPIFFS.begin ())
      Serial.println ("SPIFFS Mount failed");  // Problème avec le stockage SPIFFS - Serious problem with SPIFFS
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
   g_server.serveStatic ("/", SPIFFS, "/index.html");
   g_server.begin ();
   Serial.println ("HTTP server started");

   // Real time
   NTP.onNTPSyncEvent ([](NTPSyncEvent_t error) {
      if (error)
      {
         Serial.print ("Time Sync error: ");
         if (error == noResponse)
            Serial.println ("NTP server not reachable");
         else if (error == invalidAddress)
            Serial.println ("Invalid NTP server address");
         g_ntpError = true;
      }
      else
      {
         Serial.print ("Got NTP time: ");
         Serial.println (NTP.getTimeDateString (NTP.getLastNTPSync ()));
      }
   });
   // NTP Server, time offset, daylight
   NTP.begin ("pool.ntp.org", -1, true);
   NTP.setInterval (60);
}

void loop (void)
{
   g_currentMillis = millis ();  // Time now
   // Handle server requests
   ArduinoOTA.handle ();

   // Add a point to the history over time.
   if (g_currentMillis - g_histPreviousMillis > HIST_INT)
   {
      g_histPreviousMillis = g_currentMillis;
      AddDataPointToHistory ();
   }

   // If there is an error grabbing NTP time, try again.
   if (g_ntpError)
   {
      if (g_currentMillis - g_histPreviousMillis > NTP_RETRY_INT)
      {
         g_prevMilForNTPRetry = g_currentMillis;
         g_ntpError = false;
         NTP.getTime ();
      }
   }

   // Read the sensor data.
   if (g_currentMillis - g_prevMilForInputRead >= SENSOR_READ_INT)
   {
      g_prevMilForInputRead = g_currentMillis;
      ReadSensors ();
   }
}


