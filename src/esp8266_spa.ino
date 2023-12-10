// https://github.com/ccutrer/balboa_worldwide_app/blob/master/doc/protocol.md
// Reference:https://github.com/ccutrer/balboa_worldwide_app/wiki

// Please install the needed dependencies:
// CircularBuffer
// PubSubClient

// TODO:
// HomeAssistant autodiscover - mostly done
// Configuration handling
// Proper states (rather than just ON/OFF)
// OTA update from Firebase
// Settings
// Implement Existing Client Request/Response
// Handle non-CTS Client IDs

// +12V RED
// GND  BLACK
// A    YELLOW
// B    WHITE
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Ticker.h>
#include <config.h>

#define AUTO_TX false  // if your chip needs to pull D1 high/low set this to false
#define SAVE_CONN true // save the ip details above to local filesystem

#define STRON String("ON").c_str()
#define STROFF String("OFF").c_str()

#define TX485_Rx D3 // find a way to skip this
#define TX485_Tx D4
#define RLY1 D7
#define RLY2 D8
#define loopcountmax 10000
#define pubsub_refresh_ms 100000 // 100 Seconds

#include <ESP8266WiFi.h>
#include <CircularBuffer.h>
CircularBuffer<uint8_t, 35> Q_in;
CircularBuffer<uint8_t, 35> Q_out;

#include <ESP8266WebServer.h> // Local WebServer used to serve the configuration portal
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ArduinoOTA.h>
ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;

#include <PubSubClient.h> // MQTT client
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

extern uint8_t crc8();
extern void ID_request();
extern void ID_ack();
extern void rs485_send();

bool actual_message;

uint8_t x, i, j;

uint8_t last_state_crc = 0x00;
uint8_t send = 0x00;
uint8_t settemp = 0x00;
uint8_t id = 0x00;

unsigned long lastrx = 0;
unsigned long lastota = 0;
// unsigned long looptimer = 0;
// unsigned int loopcount = 0;
unsigned long pubsub_refresh = 0;

// String looptimer_str;
String send_str;

// char looptimer_mqtt[50];
char send_mqtt[50];
char have_config = 0;      // stages: 0-> want it; 1-> requested it; 2-> got it; 3-> further processed it
char have_faultlog = 0;    // stages: 0-> want it; 1-> requested it; 2-> got it; 3-> further processed it
char faultlog_minutes = 0; // temp logic so we only get the fault log once per 5 minutes
char ip_settings = 0;      // stages: 0-> want it; 1-> requested it; 2-> got it; 3-> further processed it

struct
{
  uint8_t jet1 : 1;
  uint8_t jet2 : 1;
  uint8_t blower : 1;
  uint8_t light : 1;
  uint8_t restmode : 1;
  uint8_t highrange : 1;
  uint8_t padding : 2;
  uint8_t hour : 5;
  uint8_t minutes : 6;
} SpaState;

struct
{
  uint8_t pump1 : 2; // this could be 1=1 speed; 2=2 speeds
  uint8_t pump2 : 2;
  uint8_t pump3 : 2;
  uint8_t pump4 : 2;
  uint8_t pump5 : 2;
  uint8_t pump6 : 2;
  uint8_t light1 : 1;
  uint8_t light2 : 1;
  uint8_t circ : 1;
  uint8_t blower : 1;
  uint8_t mister : 1;
  uint8_t aux1 : 1;
  uint8_t aux2 : 1;
} SpaConfig;

struct
{
  uint8_t totEntry : 5;
  uint8_t currEntry : 5;
  uint8_t faultCode : 6;
  String faultMessage;
  uint8_t daysAgo : 8;
  uint8_t hour : 5;
  uint8_t minutes : 6;
} SpaFaultLog;

void _yield()
{
  yield();
  mqtt.loop();
  httpServer.handleClient();
}

void print_msg(CircularBuffer<uint8_t, 35> &data)
{
  String s;
  // for (i = 0; i < (Q_in[1] + 2); i++) {
  for (i = 0; i < data.size(); i++)
  {
    x = Q_in[i];
    if (x < 0x0A)
      s += "0";
    s += String(x, HEX);
    s += " ";
  }
  mqtt.publish("Spa/node/msg", s.c_str());
  _yield();
}

void decodeFault()
{
  SpaFaultLog.totEntry = Q_in[5];
  SpaFaultLog.currEntry = Q_in[6];
  SpaFaultLog.faultCode = Q_in[7];
  switch (SpaFaultLog.faultCode)
  { // this is a inelegant way to do it, a lookup table would be better
  case 15:
    SpaFaultLog.faultMessage = "Sensors are out of sync";
    break;
  case 16:
    SpaFaultLog.faultMessage = "The water flow is low";
    break;
  case 17:
    SpaFaultLog.faultMessage = "The water flow has failed";
    break;
  case 18:
    SpaFaultLog.faultMessage = "The settings have been reset";
    break;
  case 19:
    SpaFaultLog.faultMessage = "Priming Mode";
    break;
  case 20:
    SpaFaultLog.faultMessage = "The clock has failed";
    break;
  case 21:
    SpaFaultLog.faultMessage = "The settings have been reset";
    break;
  case 22:
    SpaFaultLog.faultMessage = "Program memory failure";
    break;
  case 26:
    SpaFaultLog.faultMessage = "Sensors are out of sync -- Call for service";
    break;
  case 27:
    SpaFaultLog.faultMessage = "The heater is dry";
    break;
  case 28:
    SpaFaultLog.faultMessage = "The heater may be dry";
    break;
  case 29:
    SpaFaultLog.faultMessage = "The water is too hot";
    break;
  case 30:
    SpaFaultLog.faultMessage = "The heater is too hot";
    break;
  case 31:
    SpaFaultLog.faultMessage = "Sensor A Fault";
    break;
  case 32:
    SpaFaultLog.faultMessage = "Sensor B Fault";
    break;
  case 34:
    SpaFaultLog.faultMessage = "A pump may be stuck on";
    break;
  case 35:
    SpaFaultLog.faultMessage = "Hot fault";
    break;
  case 36:
    SpaFaultLog.faultMessage = "The GFCI test failed";
    break;
  case 37:
    SpaFaultLog.faultMessage = "Standby Mode (Hold Mode)";
    break;
  default:
    SpaFaultLog.faultMessage = "Unknown error";
    break;
  }
  SpaFaultLog.daysAgo = Q_in[8];
  SpaFaultLog.hour = Q_in[9];
  SpaFaultLog.minutes = Q_in[10];

  mqtt.publish("Spa/fault/Entries", String(SpaFaultLog.totEntry).c_str());
  mqtt.publish("Spa/fault/Entry", String(SpaFaultLog.currEntry).c_str());
  mqtt.publish("Spa/fault/Code", String(SpaFaultLog.faultCode).c_str());
  mqtt.publish("Spa/fault/Message", SpaFaultLog.faultMessage.c_str());
  mqtt.publish("Spa/fault/DaysAgo", String(SpaFaultLog.daysAgo).c_str());
  mqtt.publish("Spa/fault/Hours", String(SpaFaultLog.hour).c_str());
  mqtt.publish("Spa/fault/Minutes", String(SpaFaultLog.minutes).c_str());
  have_faultlog = 2;
}

void resetFaultLog()
{
  have_faultlog = 0;
}

Ticker faultlogTimer(resetFaultLog, 300000); // 5 minutes

void decodeSettings()
{
  mqtt.publish("Spa/config/status", "Got config");
  SpaConfig.pump1 = Q_in[5] & 0x03;
  SpaConfig.pump2 = (Q_in[5] & 0x0C) >> 2;
  SpaConfig.pump3 = (Q_in[5] & 0x30) >> 4;
  SpaConfig.pump4 = (Q_in[5] & 0xC0) >> 6;
  SpaConfig.pump5 = (Q_in[6] & 0x03);
  SpaConfig.pump6 = (Q_in[6] & 0xC0) >> 6;
  SpaConfig.light1 = (Q_in[7] & 0x03);
  SpaConfig.light2 = (Q_in[7] >> 2) & 0x03;
  SpaConfig.circ = ((Q_in[8] & 0x80) != 0);
  SpaConfig.blower = ((Q_in[8] & 0x03) != 0);
  SpaConfig.mister = ((Q_in[9] & 0x30) != 0);
  SpaConfig.aux1 = ((Q_in[9] & 0x01) != 0);
  SpaConfig.aux2 = ((Q_in[9] & 0x02) != 0);
  mqtt.publish("Spa/config/pumps1", String(SpaConfig.pump1).c_str());
  mqtt.publish("Spa/config/pumps2", String(SpaConfig.pump2).c_str());
  mqtt.publish("Spa/config/pumps3", String(SpaConfig.pump3).c_str());
  mqtt.publish("Spa/config/pumps4", String(SpaConfig.pump4).c_str());
  mqtt.publish("Spa/config/pumps5", String(SpaConfig.pump5).c_str());
  mqtt.publish("Spa/config/pumps6", String(SpaConfig.pump6).c_str());
  mqtt.publish("Spa/config/light1", String(SpaConfig.light1).c_str());
  mqtt.publish("Spa/config/light2", String(SpaConfig.light2).c_str());
  mqtt.publish("Spa/config/circ", String(SpaConfig.circ).c_str());
  mqtt.publish("Spa/config/blower", String(SpaConfig.blower).c_str());
  mqtt.publish("Spa/config/mister", String(SpaConfig.mister).c_str());
  mqtt.publish("Spa/config/aux1", String(SpaConfig.aux1).c_str());
  mqtt.publish("Spa/config/aux2", String(SpaConfig.aux2).c_str());
  have_config = 2;
}

void decodeState()
{
  String s;
  double d = 0.0;
  double c = 0.0;

  // DEBUG for finding meaning:
  // print_msg(Q_in);

  // 25:Flag Byte 20 - Set Temperature
  d = Q_in[25] / 2;
  if (Q_in[25] % 2 == 1)
    d += 0.5;
  mqtt.publish("Spa/target_temp/state", String(d, 2).c_str());

  // 7:Flag Byte 2 - Actual temperature
  if (Q_in[7] != 0xFF)
  {
    d = Q_in[7] / 2;
    if (Q_in[7] % 2 == 1)
      d += 0.5;
    if (c > 0)
    {
      if ((d > c * 1.2) || (d < c * 0.8))
        d = c; // remove spurious readings greater or less than 20% away from previous read
    }

    mqtt.publish("Spa/temperature/state", String(d, 2).c_str());
    c = d;
  }
  else
  {
    d = 0;
  }
  // REMARK Move upper publish to HERE to get 0 for unknown temperature

  // 8:Flag Byte 3 Hour & 9:Flag Byte 4 Minute => Time
  if (Q_in[8] < 10)
    s = "0";
  else
    s = "";
  SpaState.hour = Q_in[8];
  s = String(Q_in[8]) + ":";
  if (Q_in[9] < 10)
    s += "0";
  s += String(Q_in[9]);
  SpaState.minutes = Q_in[9];
  mqtt.publish("Spa/time/state", s.c_str());

  // 10:Flag Byte 5 - Heating Mode
  switch (Q_in[10])
  {
  case 0:
    mqtt.publish("Spa/heatingmode/state", STRON); // Ready
    mqtt.publish("Spa/heat_mode/state", "heat");  // Ready
    SpaState.restmode = 0;
    break;
  case 3: // Ready-in-Rest
    SpaState.restmode = 0;
    break;
  case 1:
    mqtt.publish("Spa/heatingmode/state", STROFF); // Rest
    mqtt.publish("Spa/heat_mode/state", "off");    // Rest
    SpaState.restmode = 1;
    break;
  }

  // 15:Flags Byte 10 / Heat status, Temp Range
  d = bitRead(Q_in[15], 4);
  if (d == 0)
    mqtt.publish("Spa/heatstate/state", STROFF);
  else if (d == 1 || d == 2)
    mqtt.publish("Spa/heatstate/state", STRON);

  d = bitRead(Q_in[15], 2);
  if (d == 0)
  {
    mqtt.publish("Spa/highrange/state", STROFF); // LOW
    SpaState.highrange = 0;
  }
  else if (d == 1)
  {
    mqtt.publish("Spa/highrange/state", STRON); // HIGH
    SpaState.highrange = 1;
  }

  // 16:Flags Byte 11
  if (bitRead(Q_in[16], 1) == 1)
  {
    mqtt.publish("Spa/jet_1/state", STRON);
    SpaState.jet1 = 1;
  }
  else
  {
    mqtt.publish("Spa/jet_1/state", STROFF);
    SpaState.jet1 = 0;
  }

  if (bitRead(Q_in[16], 3) == 1)
  {
    mqtt.publish("Spa/jet_2/state", STRON);
    SpaState.jet2 = 1;
  }
  else
  {
    mqtt.publish("Spa/jet_2/state", STROFF);
    SpaState.jet2 = 0;
  }

  // 18:Flags Byte 13
  if (bitRead(Q_in[18], 1) == 1)
    mqtt.publish("Spa/circ/state", STRON);
  else
    mqtt.publish("Spa/circ/state", STROFF);

  if (bitRead(Q_in[18], 2) == 1)
  {
    mqtt.publish("Spa/blower/state", STRON);
    SpaState.blower = 1;
  }
  else
  {
    mqtt.publish("Spa/blower/state", STROFF);
    SpaState.blower = 0;
  }
  // 19:Flags Byte 14
  if (Q_in[19] == 0x03)
  {
    mqtt.publish("Spa/light/state", STRON);
    SpaState.light = 1;
  }
  else
  {
    mqtt.publish("Spa/light/state", STROFF);
    SpaState.light = 0;
  }

  last_state_crc = Q_in[Q_in[1]];

  // Publish own relay states
  s = "OFF";
  if (digitalRead(RLY1) == LOW)
    s = "ON";
  mqtt.publish("Spa/relay_1/state", s.c_str());

  s = "OFF";
  if (digitalRead(RLY2) == LOW)
    s = "ON";
  mqtt.publish("Spa/relay_2/state", s.c_str());
}

///////////////////////////////////////////////////////////////////////////////

void hardreset()
{
  ESP.wdtDisable();
  while (1)
  {
  };
}

/// Resets MQTT Connection information and makes sure everything issubcribed to
//////////////////////////////////////////////////////////////////////////////////

void mqttpubsub()
{

  // ONLY DO THE FOLLOWING IF have_config == true otherwise it will not work
  String Payload;

  mqtt.publish("Spa/node/state", "ON");
  mqtt.publish("Spa/node/debug", "RECONNECT");
  // mqtt.publish("Spa/node/debug", String(millis()).c_str());
  // mqtt.publish("Spa/node/debug", String(oldstate).c_str());
    mqtt.publish("Spa/node/flashsize", String(ESP.getFlashChipRealSize()).c_str());
  mqtt.publish("Spa/node/chipid", String(ESP.getChipId()).c_str());
  mqtt.publish("Spa/node/speed", String(ESP.getCpuFreqMHz()).c_str());

  // ... and resubscribe

  // FIrst Unsubscribe from everything to make sure there isn't any issues
  mqtt.unsubscribe("Spa/command");
  mqtt.unsubscribe("Spa/target_temp/set");
  mqtt.unsubscribe("Spa/heatingmode/set");
  mqtt.unsubscribe("Spa/heat_mode/set");
  mqtt.unsubscribe("Spa/highrange/set");
  mqtt.unsubscribe("Spa/ping");
  mqtt.unsubscribe("Spa/jet_1/set");
  mqtt.unsubscribe("Spa/jet_2/set");
  mqtt.unsubscribe("Spa/blower/set");
  mqtt.unsubscribe("Spa/light/set");
  mqtt.unsubscribe("Spa/relay_1/set");
  mqtt.unsubscribe("Spa/relay_2/set");

  mqtt.subscribe("Spa/command");
  mqtt.subscribe("Spa/target_temp/set");
  mqtt.subscribe("Spa/heatingmode/set");
  mqtt.subscribe("Spa/heat_mode/set");
  mqtt.subscribe("Spa/highrange/set");
  mqtt.subscribe("Spa/ping");

  if (have_config == 2)
  {
    // OPTIONAL ELEMENTS
    if (SpaConfig.pump1 != 0)
    {
      mqtt.subscribe("Spa/jet_1/set");
    }
    if (SpaConfig.pump2 != 0)
    {
      mqtt.subscribe("Spa/jet_2/set");
    }
    if (SpaConfig.blower)
    {
      mqtt.subscribe("Spa/blower/set");
    }
    if (SpaConfig.light1)
    {
      mqtt.subscribe("Spa/light/set");
    }
  }

  mqtt.subscribe("Spa/relay_1/set");
  mqtt.subscribe("Spa/relay_2/set");

  // not sure what this is
  last_state_crc = 0x00;

  // done with config
  have_config = 3;
}

Ticker pubsubTimer(mqttpubsub, pubsub_refresh_ms);

void reconnect()
{

  mqtt.disconnect();

  if (!mqtt.connected())
  {
    // Attempt to connect
    if (BROKER_PASS == "")
    {
      // connection =
      mqtt.connect(String(String("Spa") + String(millis())).c_str());
    }
    else
    {
      // connection =
      mqtt.connect("Spa1", BROKER_LOGIN.c_str(), BROKER_PASS.c_str());
    }

    if (have_config == 3)
    {
      have_config = 2; // we have disconnected, let's republish our configuration
    }
  }
  mqtt.setBufferSize(1024); // increase pubsubclient buffer size
}

/// MQTT Ping //////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////

unsigned long lastMQTTPing = millis();

void ping()
{
  mqtt.publish("Spa/ping", "ping");
}

Ticker pingTimer(ping, 1000);

/// Monitor Status Of Data From SPA //////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////

unsigned long loopCount = 0;
unsigned long loopTimer = millis();
unsigned long lastStatus = millis();
unsigned long lastCommandRequest = millis();
unsigned long startupTime = millis() / 1000;

void monitorStatus()
{

  unsigned long loopTimerDifference = millis() - loopTimer;

  if (loopCount > 0)
  {
    float loopTime = static_cast<float>(loopTimerDifference) / static_cast<float>(loopCount);

    mqtt.publish("Spa/debug/loopTime", String(loopTime).c_str());
  }
  else
  {
    mqtt.publish("Spa/debug/loopTime", "0");
  }
  unsigned long timeSinceLastStatus = millis() - lastStatus;
  unsigned long timeSinceLastCommandRequest = millis() - lastCommandRequest;
  unsigned long timeSinceLastMQTTPing = millis() - lastMQTTPing;
  unsigned long uptime = (millis() / 1000) - startupTime;

  mqtt.publish("Spa/debug/loopTimerDifference", String(loopTimerDifference).c_str());
  mqtt.publish("Spa/debug/loopCount", String(loopCount).c_str());
  mqtt.publish("Spa/debug/timeSinceLastStatus", String(timeSinceLastStatus).c_str());
  mqtt.publish("Spa/debug/timeSinceLastCommandRequest", String(timeSinceLastCommandRequest).c_str());
  mqtt.publish("Spa/debug/timeSinceLastMQTTPing", String(timeSinceLastMQTTPing).c_str());
  mqtt.publish("Spa/debug/uptime", String(uptime).c_str());
  mqtt.publish("Spa/node/id", String(id).c_str());

  // If WIFI isn't connected, then restart
  if (WiFi.status() != WL_CONNECTED)
  {
    mqtt.publish("Spa/debug/error", "Wifi Failed");
    ESP.restart();
    hardreset();
  }

  // If MQTT isn't connected or we haven't received a ping in 10 seconds, then restart mqtt
  if (!mqtt.connected() || timeSinceLastMQTTPing > 10000)
  {
    mqtt.publish("Spa/debug/error", "MQTT Disconnected");
    reconnect();
  }

  // If there isn't a command requestin 10 seconds then reset client id
  if (timeSinceLastCommandRequest > 10000 && id != 0)
  {
    mqtt.publish("Spa/debug/error", "Requesting New Client ID");
    id = 0;
  }

  // If no commands are received, or MQTT Ping isn't working then restart
  if (timeSinceLastCommandRequest > 100000 || timeSinceLastMQTTPing > 100000)
  {
    mqtt.publish("Spa/debug/error", "No Commands Received");
    ESP.restart();
    hardreset();
  }

  loopCount = 0;
  loopTimer = millis();
}

Ticker statusMonitor(monitorStatus, 1000);

/// SEND COMMS CONFIGURATION REGULARLY////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
void sendCommsConfiguration()
{
  mqtt.publish("Spa/debug/network/wifiSSID", WIFI_SSID.c_str());
  mqtt.publish("Spa/debug/network/ipAddress", WiFi.localIP().toString().c_str());
  mqtt.publish("Spa/debug/network/wifiStrength", String(WiFi.RSSI()).c_str());
}

Ticker commsUpdateTimer(sendCommsConfiguration, 1000);

// function called when a MQTT message arrived
void callback(char *p_topic, byte *p_payload, unsigned int p_length)
{
  // concat the payload into a string
  String payload;
  for (uint8_t i = 0; i < p_length; i++)
  {
    payload.concat((char)p_payload[i]);
  }
  String topic = String(p_topic);

  _yield();

  // handle message topic
  if (topic.equals("Spa/ping"))
  {
    lastMQTTPing = millis();
  }

  else if (topic.startsWith("Spa/relay_"))
  {
    bool newstate = 0;

    if (payload.equals("ON"))
      newstate = LOW;
    else if (payload.equals("OFF"))
      newstate = HIGH;

    if (topic.charAt(10) == '1')
    {
      pinMode(RLY1, INPUT);
      delay(25);
      pinMode(RLY1, OUTPUT);
      digitalWrite(RLY1, newstate);
    }
    else if (topic.charAt(10) == '2')
    {
      pinMode(RLY2, INPUT);
      delay(25);
      pinMode(RLY2, OUTPUT);
      digitalWrite(RLY2, newstate);
    }
  }
  else if (topic.equals("Spa/command"))
  {
    if (payload.equals("reset"))
      hardreset();
  }
  else if (topic.equals("Spa/heatingmode/set"))
  {
    if (payload.equals("ON") && SpaState.restmode == 1)
      send = 0x51; // ON = Ready; OFF = Rest
    else if (payload.equals("OFF") && SpaState.restmode == 0)
      send = 0x51;
  }
  else if (topic.equals("Spa/heat_mode/set"))
  {
    if (payload.equals("heat") && SpaState.restmode == 1)
      send = 0x51; // ON = Ready; OFF = Rest
    else if (payload.equals("off") && SpaState.restmode == 0)
      send = 0x51;
  }
  else if (topic.equals("Spa/light/set"))
  {
    if (payload.equals("ON") && SpaState.light == 0)
      send = 0x11;
    else if (payload.equals("OFF") && SpaState.light == 1)
      send = 0x11;
  }
  else if (topic.equals("Spa/jet_1/set"))
  {
    if (payload.equals("ON") && SpaState.jet1 == 0)
      send = 0x04;
    else if (payload.equals("OFF") && SpaState.jet1 == 1)
      send = 0x04;
  }
  else if (topic.equals("Spa/jet_2/set"))
  {
    if (payload.equals("ON") && SpaState.jet2 == 0)
      send = 0x05;
    else if (payload.equals("OFF") && SpaState.jet2 == 1)
      send = 0x05;
  }
  else if (topic.equals("Spa/blower/set"))
  {
    if (payload.equals("ON") && SpaState.blower == 0)
      send = 0x0C;
    else if (payload.equals("OFF") && SpaState.blower == 1)
      send = 0x0C;
  }
  else if (topic.equals("Spa/highrange/set"))
  {
    if (payload.equals("ON") && SpaState.highrange == 0)
      send = 0x50; // ON = High, OFF = Low
    else if (payload.equals("OFF") && SpaState.highrange == 1)
      send = 0x50;
  }
  else if (topic.equals("Spa/target_temp/set"))
  {
    // Get new set temperature
    double d = payload.toDouble();
    if (d > 0)
      d *= 2; // Convert to internal representation
    settemp = d;
    send = 0xff;
  }
}

///////////////////////////////////////////////////////////////////////////////

void setup()
{
  DynamicJsonDocument jsonSettings(1024);

  String error_msg = "";

  if (LittleFS.begin())
  {

    if (SAVE_CONN == true)
    {
      File f = LittleFS.open("/ip.txt", "w");
      if (!f)
      {
        error_msg = "failed to create file";
      }

      jsonSettings["WIFI_SSID"] = WIFI_SSID;
      jsonSettings["WIFI_PASSWORD"] = WIFI_PASSWORD;
      jsonSettings["BROKER"] = BROKER;
      jsonSettings["BROKER_LOGIN"] = BROKER_LOGIN;
      jsonSettings["BROKER_PASS"] = BROKER_PASS;

      if (serializeJson(jsonSettings, f) == 0)
      {
        error_msg = "failed to write file";
      }

      f.close();
    }

    jsonSettings["WIFI_SSID"] = "";
    jsonSettings["WIFI_PASSWORD"] = "";
    jsonSettings["BROKER"] = "";
    jsonSettings["BROKER_LOGIN"] = "";
    jsonSettings["BROKER_PASS"] = "";

    if (ip_settings == 0)
    {
      ip_settings = 1;
      // read the settings from filesystem, if empty, put in AP mode

      File file = LittleFS.open("/ip.txt", "r");
      if (!file)
      {
        error_msg = "could not open file for reading";
      }
      else
      {
        deserializeJson(jsonSettings, file);
        // Filesystem methods assuming it all went well

        // now I have them - NOTE: PROBABLY NEED TO CHECK THEM!!!!!
        ip_settings = 2;
        WIFI_SSID = jsonSettings["WIFI_SSID"].as<String>();
        WIFI_PASSWORD = jsonSettings["WIFI_PASSWORD"].as<String>();
        BROKER = jsonSettings["BROKER"].as<String>();
        BROKER_LOGIN = jsonSettings["BROKER_LOGIN"].as<String>();
        BROKER_PASS = jsonSettings["BROKER_PASS"].as<String>();
        error_msg = "Successfully read the configuration";
      }
      file.close();
    }
  }
  else
  {
    error_msg = "Could not mount fs";
  }

  LittleFS.end();

  // Begin RS485 in listening mode -> no longer required with new RS485 chip
  if (AUTO_TX)
  {
  }
  else
  {
    pinMode(TX485_Rx, OUTPUT);
    digitalWrite(TX485_Rx, LOW);
    pinMode(TX485_Tx, OUTPUT);
    digitalWrite(TX485_Tx, LOW);
  }

  // pinMode(LED_BUILTIN, OUTPUT);

  pinMode(RLY1, OUTPUT);
  digitalWrite(RLY1, HIGH);
  pinMode(RLY2, OUTPUT);
  digitalWrite(RLY2, HIGH);

  // Spa communication, 115.200 baud 8N1
  Serial.begin(115200);

  // give Spa time to wake up after POST
  for (uint8_t i = 0; i < 5; i++)
  {
    delay(1000);
    yield();
  }

  Q_in.clear();
  Q_out.clear();

  WiFi.setOutputPower(20.5); // this sets wifi to highest power
  WiFi.begin(WIFI_SSID.c_str(), WIFI_PASSWORD.c_str());
  unsigned long timeout = millis() + 10000;

  while (WiFi.status() != WL_CONNECTED && millis() < timeout)
  {
    yield();
  }

  // Reset because of no connection
  if (WiFi.status() != WL_CONNECTED)
  {
    hardreset();
  }

  httpUpdater.setup(&httpServer, "admin", "");
  httpServer.begin();
  MDNS.begin("Spa");
  MDNS.addService("http", "tcp", 80);

  mqtt.setServer(BROKER.c_str(), 1883);
  mqtt.setCallback(callback);
  mqtt.setKeepAlive(10);
  mqtt.setSocketTimeout(20);
  mqtt.setBufferSize(1024);
  mqtt.connect("Spa1", BROKER_LOGIN.c_str(), BROKER_PASS.c_str());

  mqtt.publish("Spa/node/debug", "MQTT Connected");

  ArduinoOTA.begin();

  commsUpdateTimer.start();
  statusMonitor.start();
  pingTimer.start();
  pubsubTimer.start();
  faultlogTimer.start();
  mqttpubsub();
}

void loop()
{
  loopCount = loopCount + 1;
  ArduinoOTA.handle();
  commsUpdateTimer.update();
  statusMonitor.update();
  pingTimer.update();
  pubsubTimer.update();
  faultlogTimer.update();

  _yield();

  // Read from Spa RS485
  if (Serial.available())
  {
    x = Serial.read();
    Q_in.push(x);
    // DEBUG: mqtt.publish("Spa/rcv", String(x).c_str()); _yield();

    // Drop until SOF is seen
    if (Q_in.first() != 0x7E)
      Q_in.clear();
    lastrx = millis();
  }

  // Double SOF-marker, drop last one
  if (Q_in[1] == 0x7E && Q_in.size() > 1)
    Q_in.pop();

  // Complete package
  // if (x == 0x7E && Q_in[0] == 0x7E && Q_in[1] != 0x7E) {
  if (x == 0x7E && Q_in.size() > 2)
  {
    // print_msg();

    // Unregistered or yet in progress
    if (id == 0)
    {
      if (Q_in[2] == 0xFE)
        print_msg(Q_in);

      // FE BF 02:got new client ID
      if (Q_in[2] == 0xFE && Q_in[4] == 0x02)
      {
        id = Q_in[5];
        if (id > 0x2F)
          id = 0x2F;

        ID_ack();
      }

      // FE BF 00:Any new clients?
      if (Q_in[2] == 0xFE && Q_in[4] == 0x00)
      {
        ID_request();
      }
    }
    else if (Q_in[2] == id && Q_in[4] == 0x06)
    { // we have an ID, do clever stuff
      // id BF 06:Ready to Send

      lastCommandRequest = millis();
      actual_message = false;

      if (send == 0xff)
      {
        // 0xff marks dirty temperature for now
        Q_out.push(id);
        Q_out.push(0xBF);
        Q_out.push(0x20);
        Q_out.push(settemp);
        actual_message = true;
      }
      else if (send == 0x00)
      {
        if (have_config == 0)
        { // Get configuration of the hot tub
          Q_out.push(id);
          Q_out.push(0xBF);
          Q_out.push(0x22);
          Q_out.push(0x00);
          Q_out.push(0x00);
          Q_out.push(0x01);
          mqtt.publish("Spa/config/status", "Getting config");
          have_config = 1;
          actual_message = true;
        }
        else if (have_faultlog == 0)
        { // Get the fault log
          Q_out.push(id);
          Q_out.push(0xBF);
          Q_out.push(0x22);
          Q_out.push(0x20);
          Q_out.push(0xFF);
          Q_out.push(0x00);
          have_faultlog = 1;
          actual_message = true;
        }
        else
        {
          // A Nothing to Send message is sent by a client immediately after a Clear to Send message if the client has no messages to send.
          Q_out.push(id);
          Q_out.push(0xBF);
          Q_out.push(0x07);
          // actual_message = true;
        }
      }
      else
      {
        // Send toggle commands
        Q_out.push(id);
        Q_out.push(0xBF);
        Q_out.push(0x11);
        Q_out.push(send);
        Q_out.push(0x00);
        actual_message = true;
      }

      rs485_send();
      if (actual_message)
      {
        // print_msg(Q_out);
        send_str = String(send);
        send_str.toCharArray(send_mqtt, send_str.length() + 1);
      }

      send = 0x00;
    }
    else if (Q_in[2] == id && Q_in[4] == 0x2E)
    {
      if (last_state_crc != Q_in[Q_in[1]])
      {
        decodeSettings();
      }
    }
    else if (Q_in[2] == id && Q_in[4] == 0x28)
    {
      if (last_state_crc != Q_in[Q_in[1]])
      {
        decodeFault();
      }
    }
    else if (Q_in[2] == 0xFF && Q_in[4] == 0x13)
    { // FF AF 13:Status Update - Packet index offset 5
      if (last_state_crc != Q_in[Q_in[1]])
      {
        lastStatus = millis();
        decodeState();
      }
    }
    else
    {
      // DEBUG for finding meaning
      // if (Q_in[2] & 0xFE || Q_in[2] == id)
      // print_msg(Q_in);
    }

    // Clean up queue
    _yield();
    Q_in.clear();
  }
}
