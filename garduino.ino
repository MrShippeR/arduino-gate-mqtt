/*
* Program na ovládání samonosné posuvné brány. Arduino je tady nadstavbou - dává pokyn k desce ovládající samotný pohon, že se má otevřít či zavřít. Arduino přináší možnost otevřít bránu přes internet,
* konkrétně přes webovou aplikaci Home Assistant. Home Assistant využívá protokolu MQTT pro komunikaci s Ardinem. Prostředníkem pro řízení komunikace je tady Mosquitto broker (server), který řídí komunikaci po
* protoku MQTT. 
*
* Použitý Hardware:
* https://pohonservis.cz/produkt/posuvna-samonosna-brana-vc-sloupku-a-pohonu--sb
* Arduino Uno rev 3
* Ethernet shield W5100
* ** Ethernet Shield využívá piny 11, 12 a 13 pro SPI a dále pin 10 pro CS signál W5100 a pin 4 pro CS signál slotu pro paměťové karty.
* Relay Shield V2.0 Deek-Robot.com
*
* Užitečná dokumentace:
* https://www.home-assistant.io/integrations/sensor.mqtt/
* https://www.home-assistant.io/integrations/switch.mqtt/
* https://github.com/256dpi/arduino-mqtt
* https://github.com/sstaub/Ticker
*
* Verze 1.0 09/2025
* marek@vach.cz
*/

#include <Ethernet.h>
#include <MQTT.h>

// values for network
byte mac[] = {0x02, 0x17, 0x3A, 0x4B, 0x5C, 0x6E};
byte ip[] = {192, 168, 20, 7};
const char* mqtt_server   = "192.168.0.8";
const char* mqtt_name     = "garduino";
const char* mqtt_password = "Drainpipe";

// MQTT communication variables
#define MAX_TOPIC_LEN 16
#define MAX_PAYLOAD_LEN 7
char received_topic[MAX_TOPIC_LEN];
char received_message[MAX_PAYLOAD_LEN];

EthernetClient ethClient;
MQTTClient mqqtClient;
unsigned long lastMillis = 0;

// HW pinout section
const int pin_sensor_mailbox = 2;
const int pin_motor_running = 3;
// Relay module **never use more relay then one at same time because of internal Arduino power supply will overload
// pin 4 reserved for chip select of SD card.
//const int pin_relay_1 = 5;    // unused
const int pin_relay_open = 6;
//const int pin_relay_3 = 7;    // unused
const int pin_limiter_closed = 9;
const int pin_limiter_opened = 8;
// pin 10 reserved for chip select of Ethernet W5100
const int pin_photocell_outside = A0;
const int pin_photocell_inside = A1;
const int pin_induction_loop = A2;
const int pin_input_open_pulse = A3;
const int pin_input_open_automatic = A4;
const int pin_home_ring = A5;

// MQTT topics
const char* topic_connect_status                 = "g/c";
const char* topic_gate_position                  = "g/p";
const char* topic_relay_open_pulse               = "g/o";
const char* topic_relay_open_automatic           = "g/oat";
const char* topic_mailbox                        = "d/m";
const char* topic_home_ring                      = "d/r";



void connectMqtt() {
  Serial.print("MQTT cnct...");
  while (!mqqtClient.connect(mqtt_name, mqtt_name, mqtt_password)) {
    Serial.print(".");
    delay(1000);
  }
  Serial.print("\ncnct to ");
  Serial.println(mqtt_server);

  mqqtClient.subscribe(topic_relay_open_pulse);
  mqqtClient.subscribe(topic_relay_open_automatic);
  mqttClient.publish(topic_connect_status, "online");
}



void messageReceived(String &topic, String &payload) {
  Serial.println("incoming: " + topic + " - " + payload);

  if (payload.length() >= MAX_PAYLOAD_LEN) {
    Serial.println(F("dropped MQTT payload!"));
    return;
  }

  topic.toCharArray(received_topic, MAX_TOPIC_LEN);
  payload.toCharArray(received_message, MAX_PAYLOAD_LEN);
}

void setup() {
  Serial.begin(9600);
  Ethernet.begin(mac, ip);

  mqqtClient.begin(mqtt_server, ethClient);
  mqqtClient.onMessage(messageReceived);
  mqttClient.setWill(topic_connect_status, "offline", true, 0);  // retained, QoS

  connectMqtt();
}

void loop() {
  mqqtClient.loop();

  if (!mqqtClient.connected()) {
    connectMqtt();
  }

  // publish a message roughly every 10 second.
  if (millis() - lastMillis > 10000) {
    lastMillis = millis();
    mqqtClient.publish("/hello", "world");
  }
}
