#include <WiFi.h>
#include <PubSubClient.h>
#include "DHT20.h"
#include "Wire.h"
#include <ArduinoJson.h>
#include <Update.h>
#include <SHA256.h>



const char* WIFI_SSID = "LamCoffee";
const char* WIFI_PASS = "999999999";

const char* MQTT_SERVER = "app.coreiot.io";
const int MQTT_PORT = 1883;
const char* ACCESS_TOKEN = "YWbAVwOqlX1AUG3CHBgy";

DHT20 dht20;
WiFiClient espClient;
PubSubClient client(espClient);
SHA256 sha256;


struct firmware_info {
  String version;
  int size_total;
  int size_downloaded = 0;
  String checksum;

};

bool notUpdated = true;
bool wifiConnected = false;
bool mqttConnected = false;
bool new_version = false;
int firmware_request_id = 1;
int chunkIndex = 0;
int chunk_size = 8192; // 8KB
bool get_fw_data = true;
bool exception = false;
firmware_info current_fw_info;



void callback(char* topic, byte* payload, unsigned int length) {
  
  if (strstr(topic, "v1/devices/me/attributes") != NULL) 
  {
    String messageTemp;
    for (unsigned int i = 0; i < length; i++) 
    {
      messageTemp += (char)payload[i];
    }
    Serial.print("Received [");
    Serial.print(topic);
    Serial.print("]: ");
    Serial.println(messageTemp);
    
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, messageTemp);
    if (!error && doc.containsKey("fw_version") && !new_version) 
    {
      
      String version = doc["fw_version"];
      String checksum = doc["fw_checksum"];
      current_fw_info.version = version;
      current_fw_info.size_total = doc["fw_size"];
      current_fw_info.checksum = checksum;
      new_version = true;
      Serial.println("New firmware available");
      Serial.println("Update info:\n\tVersion: " + current_fw_info.version + 
                     "\n\tSize: " + String(current_fw_info.size_total) + 
                     " bytes\n\tChecksum: " + current_fw_info.checksum + 
                     "\n\tChecksum algorithm: SHA256");   
      
    }
  }
  else if (strstr(topic, "v2/fw/response") != NULL) 
  {
    size_t written = Update.write((uint8_t *)payload, length);
    if (written != length) 
    {
        exception = true; 
        Serial.println("Install failed (written != length)");          
    }
    else 
    {
        sha256.update((uint8_t *)payload, length);
        current_fw_info.size_downloaded += length; 
        chunkIndex += 1;
        get_fw_data = true;   
        Serial.println("Write success: " + String(current_fw_info.size_downloaded * 100.0 / current_fw_info.size_total) + "%");
        String msg = "{\"ota_progress\":" + String(current_fw_info.size_downloaded * 100.0 / current_fw_info.size_total) + "%"
                   + ", \"ota_size_downloaded\":" + String(current_fw_info.size_downloaded) + "}";
        client.publish("v1/devices/me/telemetry", msg.c_str());
    }
  }
}


void InitWiFi() {
  Serial.print("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("Wifi connected!");

}

const bool reconnect() {
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    return true;
  }
  InitWiFi();
  return true;
}

void wifiTask(void *pvParameters) {
  while (1) {
    if (notUpdated) {
      wifiConnected = reconnect();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void serverTask(void *pvParameters) {
  while (1) {
    if (!wifiConnected) {
      mqttConnected = false;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (notUpdated && !client.connected()) 
    {
      mqttConnected = false;
      Serial.print("Connecting to server...");
      client.setServer(MQTT_SERVER, MQTT_PORT);
      client.setCallback(callback);
      if (!client.connect("ESP32_Client", ACCESS_TOKEN, NULL)) {
        Serial.println("Failed to connect");
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
      Serial.println("MQTT connected!");
      client.subscribe("v1/devices/me/attributes"); 
      client.subscribe("v2/fw/response/+/chunk/+");
      mqttConnected = true;
    }
 
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void SensorTask(void *pvParameters) {
  Wire.begin();
  dht20.begin();
  while (1) {
    if (!mqttConnected) 
    {
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    dht20.read();
    float temp = dht20.getTemperature();
    float humi = dht20.getHumidity();

    if (isnan(temp) || isnan(humi)) {
      Serial.println("Failed to read from DHT20 sensor!");
    } else {
      Serial.print("Temperature: ");
      Serial.print(temp);
      Serial.print(" °C, Humidity: ");
      Serial.print(humi);
      Serial.println(" %");
      String payload = "{\"temperature\":" + String(temp) + ", \"humidity\":" + String(humi) + "}";
      if (client.publish("v1/devices/me/telemetry", payload.c_str())) {
        Serial.println("Data sent: " + payload);
      } else {
        Serial.println("Failed to send data!");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}

void callbackLoopTask(void *pvParameters) {
  while (1) 
  {
    if (mqttConnected) 
    {
      client.loop();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}


String bytesToHex(const unsigned char* bytes, size_t length) 
{
  const char hexDigits[] = "0123456789abcdef";
  String hexString;
  hexString.reserve(length * 2); 
  for (size_t i = 0; i < length; ++i) {
      unsigned char byte = bytes[i];
      hexString += hexDigits[byte >> 4]; 
      hexString += hexDigits[byte & 0x0F]; 
  }

  return hexString;
}

bool verifyChecksum(String checksum)
{
  String payload = "{\"fw_state\":\"VERIFIED\"}";    
  client.publish("v1/devices/me/telemetry", payload.c_str());
  vTaskDelay(pdMS_TO_TICKS(3000));
  unsigned char hash[32];
  sha256.finalize(hash, sizeof(hash));
  String computedChecksum = bytesToHex(hash, sizeof(hash));

  if (computedChecksum == checksum) {
      Serial.println("Checksum matches -> Installing...");
      return true;
  } else {
      Serial.println("Checksum doesn't match.");
      return false;
  }

}

void otaTask(void * pvParameters) {
  while (1) 
  {
    if(!wifiConnected || !mqttConnected) 
    {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (!new_version) 
    {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    
    if (!Update.begin(current_fw_info.size_total)) 
    {               
        Serial.println("Cannot start downloading firmware");
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
    }

    String payload_downloading = "{\"fw_state\":\"DOWNLOADING\"}";
    client.publish("v1/devices/me/telemetry", payload_downloading.c_str());
    Serial.println("Start downloading firmware...");

    while (chunkIndex < ((current_fw_info.size_total / chunk_size) + 1)) 
    {
        if (get_fw_data && new_version) 
        {
          String topic = String("v2/fw/request/") + firmware_request_id + "/chunk/" + chunkIndex;
          client.publish(topic.c_str(), String(chunk_size).c_str());
          get_fw_data = false;
        }
        vTaskDelay(pdMS_TO_TICKS(3000));  
        if (exception) 
        {
          Update.abort();
          chunkIndex = 0;
          firmware_request_id = 1;
          current_fw_info.size_downloaded = 0;
          get_fw_data = true;
          exception = false;
          sha256.reset(); 
          vTaskDelay(pdMS_TO_TICKS(10));
          continue;
        }   
    }
  
    String payload_downloaded = "{\"fw_state\":\"DOWNLOADED\"}";
    client.publish("v1/devices/me/telemetry", payload_downloaded.c_str());   
    Serial.println("Downloading firmware successfully -> Start verifying checksum...");  
    
    if (!verifyChecksum(current_fw_info.checksum)) 
    {
        Update.abort();
        chunkIndex = 0;
        firmware_request_id = 1;
        current_fw_info.size_downloaded = 0;
        get_fw_data = true;
        exception = false;
        sha256.reset(); 
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
    }        
       
    if (!Update.end(true)) 
    {
        String payload = "{\"fw_state\":\"FAILED\"}";
        client.publish("v1/devices/me/telemetry", payload.c_str());
        Serial.println("Installing firmware failed");
        chunkIndex = 0;
        firmware_request_id = 1;
        current_fw_info.size_downloaded = 0;
        get_fw_data = true;
        exception = false;
        sha256.reset(); 
        vTaskDelay(pdMS_TO_TICKS(10));  
        continue;
    }
    
    String payload_installed = "{\"fw_state\":\"INSTALLED\"}";
    client.publish("v1/devices/me/telemetry", payload_installed.c_str());   
    notUpdated = false;
    Serial.println("Install success -> Rebooting...");           
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP.restart();
    
  }
}

void setup() {
  Serial.begin(115200);

  InitWiFi();
  xTaskCreate(wifiTask, "WiFi Task", 2048, NULL, 1, NULL);
  xTaskCreate(serverTask, "Server Task", 2048, NULL, 2, NULL);
  xTaskCreate(SensorTask, "Sensor Task", 4096, NULL, 2, NULL);
  xTaskCreate(callbackLoopTask, "Callback Loop Task", 4096, NULL, 2, NULL);
  xTaskCreate(otaTask, "OTA Task", 2048, NULL, 2, NULL);
}

void loop() {
  
}