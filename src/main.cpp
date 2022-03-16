#include <Arduino.h>
#include <Preferences.h>
#include "DHT.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>

#define NUM_REG 30
#define DHTPIN 32
#define DHTTYPE DHT22
#define FW_VERSION 20220128
#define DELAY_MQTT_SECONDS 300
#define MQTT_DIR "franciscogubbins.me"
#define MQTT_PORT 1883


DHT dht(DHTPIN, DHTTYPE);
WiFiManager wifimanager;
WiFiClient wClient;
WiFiClient updateWClient;
PubSubClient mClient(wClient);

uint32_t registros[NUM_REG] = {0};
String mac;
String baseTopicPub;
String baseTopicSub;
const char *fwUrlBase = "http://update.sistena.io/ota/";
bool firstTime = true;


void taskReadSensors(void * params);
void taskAct(void * params);
void taskLeds(void * params);
void taskMQTT(void * params);
void taskExcel(void * params);
void readMoisture(void);
void readDistanceSensor(void);
void readDHT();
bool checkForUpdates();
void update();
String formatMAC(void);
void callback(char *topic, uint8_t *payload, unsigned int length);

void setup() {
  // put your setup code here, to run once:
  Serial.begin(9600);
  disableCore0WDT();
  mac = formatMAC();
  baseTopicPub = String(mac + "/Pub/");
  baseTopicSub = String(mac + "/Sub/");
  pinMode(34, INPUT);
  pinMode(18, OUTPUT);
  pinMode(27, OUTPUT);
  pinMode(14, OUTPUT);
  pinMode(12, OUTPUT);
  dht.begin();
  xTaskCreatePinnedToCore(taskReadSensors, "sensores", 10000, NULL, 4, NULL, 1);
  xTaskCreatePinnedToCore(taskAct, "valvula", 10000, NULL, 4, NULL, 1);
  xTaskCreatePinnedToCore(taskLeds, "leds", 10000, NULL, 4, NULL, 1);
  xTaskCreatePinnedToCore(taskMQTT, "mqtt", 10000, NULL, 4, NULL, 0);
  xTaskCreatePinnedToCore(taskExcel, "excel", 10000, NULL, 4, NULL, 0);
}

void loop() {
  // put your main code here, to run repeatedly:
  delay(2000);
}

void taskReadSensors(void * params) {
  for (;;) {
    readMoisture();
    readDistanceSensor();
    readDHT();
    delay(100);
  }
}

void taskAct(void * params) {
  bool flag = true;
  for (;;) {
    if (registros[1] <= 20) {
      digitalWrite(18, 1); // Abrimos el relé
      registros[10] = 1;
      if (flag) {
        xTaskCreatePinnedToCore(taskLeds, "blink", 1000, 0, 3, NULL, 1);
        flag = false;
      }
    } else {
      registros[10] = 0;
      digitalWrite(18, 0); // Cerramos el relé
      flag = true;
    }
    delay(100);
  }
}

void taskLeds(void * params) {
  int turn = 1;
  for (;;) {
    uint32_t alarms = registros[20];
    unsigned long time = 0;
    Serial.println(alarms);
    if (alarms == 0) {
      digitalWrite(14, 1); // Verde, todo ok
      digitalWrite(12, 0);
      digitalWrite(27, 0);
    } else {
      if (((alarms & 1) == 1) && turn == 1) { // Mucho calor para la planta
        digitalWrite(14, 1);
        digitalWrite(12, 1);
        digitalWrite(27, 1);
      } else if (((alarms & 2) == 2) && turn == 2) { // Falta agua en el tanque
        digitalWrite(14, 1);
        digitalWrite(27, 1);
        digitalWrite(12, 0);
      } else if (((alarms & 4) == 4) && turn == 3) { // Falta agua en la planta
        digitalWrite(27, 1);
        digitalWrite(12, 0);
        digitalWrite(14, 0);
      } else if (((alarms & 8) == 8)  && turn == 4) { // No hay wifi
        digitalWrite(12, 1);
        digitalWrite(14, 0);
        digitalWrite(27, 0);
      } else if (((alarms & 16) == 16) && turn == 5) { // Actualizacion disponible
        digitalWrite(12, 1);
        digitalWrite(14, 1);
        digitalWrite(27, 0);
      } else if (((alarms & 32) == 32)  && turn == 6) { // No hay respuesta del servidor MQTT
        digitalWrite(12, 1);
        digitalWrite(27, 1);
        digitalWrite(14, 0);
      }
    }
    turn++;
    turn = turn % 7;
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void taskMQTT(void * params) {
  String APName = String("Planter-" + mac);
  unsigned long time = 0;
  wifimanager.setConnectTimeout(30);
  wifimanager.setConfigPortalTimeout(300);
  mClient.setServer(MQTT_DIR, MQTT_PORT);
  mClient.setCallback(callback);
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      registros[20] = registros[20] | 8; 
      if (wifimanager.autoConnect(APName.c_str(), "1234567890")) {
        Serial.println("Conectado a WIFI");
        if (firstTime) {
          update();
          firstTime = false;
        }
      }
    } else {
      registros[20] &= ~(1UL << 3);
      if (!mClient.connected()) {
        String willtopic = String(baseTopicPub + "status");
        registros[20] = registros[20] | 32;
        mClient.connect(mac.c_str(), NULL, NULL, willtopic.c_str(), 0, false, "disconnected", true);
        delay(3000);
        if (mClient.connected()) {
          mClient.publish(String(baseTopicPub + "status").c_str(), String("connected").c_str());
          mClient.publish(String(baseTopicPub + "version").c_str(), String(FW_VERSION).c_str());
        }
      } else {
        registros[20] &= ~(1UL << 5);
        if ((millis() - time) > (DELAY_MQTT_SECONDS * 1000)) {
          time = millis();
          for (int i = 0; i < NUM_REG; i++) {
            mClient.publish(String(baseTopicPub + i).c_str(), String(registros[i]).c_str());
          }
        }
        mClient.loop();
      }
    }
    delay(50);
  }
}

void readMoisture(void) {
  unsigned long sum;
  for (int i = 0; i < 20; i++) {
    sum += analogRead(34);
    delay(5);
  }
  registros[0] = sum / 20; // De esta forma evitamos interferencias que puedan entrar.
  float value = map(registros[0], 2432, 1, 0, 100);
  if (value >= 100) {
    registros[1] = 100;
  }
  else if (value <= 0) {
    registros[1] = 0;
  }
  else {
    registros[1] = value;
  }
  if (registros[1] <= 20) {
    registros[20] = registros[20] | 4;
  } else {
    registros[20] &= ~(1UL << 2);
  }
}

void readDistanceSensor(void) {
  unsigned long sum = 0;
  int pin = 2;
  for (int i = 0; i < 5; i++) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    delayMicroseconds(2);
    digitalWrite(pin, HIGH);
    delayMicroseconds(5);
    digitalWrite(pin,LOW);
    pinMode(pin,INPUT);
    sum += pulseIn(pin,HIGH);
  }
  registros[2] = sum / 5;
  registros[3] = registros[2] / 29.0 / 2.0;
  if (registros[3] > 60) {
    registros[20] = registros[20] | 2;
  } else {
    registros[20] &= ~(1UL << 1);
  }
}

void readDHT() {
  registros[4] = dht.readHumidity();
  registros[5] = dht.readTemperature();
  registros[6] = dht.computeHeatIndex(false);
  if (registros[6] > 100) {
    registros[20] = registros[20] | 1;
  } else {
    registros[20] &= ~(1UL << 0);
  }
}

bool checkForUpdates() {
  registros[20] = registros[20] | 16;
  return true;
} 

void update()
{
  String mac = formatMAC();
  String fwURL = String(fwUrlBase);
  fwURL.concat(mac);
  String fwVersionURL = fwURL;
  fwVersionURL.concat(".version");
  HTTPClient httpClient;
  httpClient.begin(updateWClient, fwVersionURL);
  int httpCode = httpClient.GET();
  if (httpCode == 200)
  {
    String newFWVersion = httpClient.getString();
    int newVersion = newFWVersion.toInt();
    if (newVersion > FW_VERSION)
    {
      String fwImageURL = fwURL;
      fwImageURL.concat(".bin");
      mClient.publish(String(baseTopicPub + "/update").c_str(), "Updating");
      delay(5000);
      t_httpUpdate_return ret = httpUpdate.update(updateWClient, fwImageURL); /// FOR ESP32 HTTP FOTA
      delay(5000);
      switch (ret)
      {
      case HTTP_UPDATE_FAILED:
        char currentString[64];
        sprintf(currentString, "\nHTTP_UPDATE_FAILD Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str()); /// FOR ESP32
        mClient.publish(String(baseTopicPub + "/update").c_str(), currentString);
        break;
      case HTTP_UPDATE_NO_UPDATES:
        break;
      }
    }
    else
    {
      char currentString[64];
      sprintf(currentString, "\nNot necessary to update. Actual v: %d\n", FW_VERSION);
      mClient.publish(String(baseTopicPub + "/update").c_str(), currentString);
    }
  }
  else
  {
    char currentString[64];
    sprintf(currentString, "\nVersión del firmware no alcanzable, Error HTTP: %d\n", httpCode);
    mClient.publish(String(baseTopicPub + "/update").c_str(), currentString);
  }
}


String formatMAC(void) {
  String mac = WiFi.macAddress();
  mac.remove(2, 1);
  mac.remove(4, 1);
  mac.remove(6, 1);
  mac.remove(8, 1);
  mac.remove(10, 1);
  return mac;
}

void callback(char *topic, uint8_t *payload, unsigned int length)
{
  char payloadToChar[50] = {0};
  String parsedTopic = String(topic);
  parsedTopic = parsedTopic.substring(baseTopicSub.length() + 1);
  int reg = parsedTopic.toInt();
  if (reg != 0) {
    if (reg >= 10 && reg < 20 ) {
      for (int i = 0; i < length; i++)
      {
        payloadToChar[i] = payload[i];
      }
      String parsedPayload = String(payloadToChar);
      registros[reg] = parsedPayload.toInt();
    }
  } else {
    if (parsedTopic == "restart") {
      ESP.restart();
    }
  }
}

void taskExcel(void * params) {
  WiFiClient *wifi_client = new WiFiClient();
  HTTPClient *http_client = new HTTPClient();
  while(true) {
    Serial.println("Sending data");
    String sample = "";
    for (int i = 0; i < NUM_REG; i++) {
      sample.concat(registros[i]);
      sample.concat(";");
    }
    http_client->begin(*wifi_client, String("http://franciscogubbins.me:3001/toExcel/" + formatMAC()));
    http_client->addHeader("content-type", "application/octet-stream");
    http_client->POST((uint8_t *)sample.c_str(), sample.length());
    http_client->end();
    Serial.println("Sent Data");
    vTaskDelay(1800 / portTICK_PERIOD_MS);
  }
}
