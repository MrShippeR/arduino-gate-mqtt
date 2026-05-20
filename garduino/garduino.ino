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
* https://github.com/256dpi/arduino-mqtt lwmqtt
*
* Verze 1.0 09/2025
* marek@vach.cz
*/

#include <Ethernet.h>
#include <MQTT.h>
#include <EEPROM.h>


// HW pinout section
const int pin_sensor_mailbox = 2;
const int pin_motor_running = 3;
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

// values for network
byte mac[] = {0x02, 0x17, 0x3A, 0x4B, 0x5C, 0x6E};
const char* mqtt_server   = "192.168.0.8";
const char* mqtt_name     = "garduino";
const char* mqtt_password = "Drainpipe";

// MQTT communication variables
#define MAX_TOPIC_LEN 16
#define MAX_PAYLOAD_LEN 7
char received_topic[MAX_TOPIC_LEN];
char received_message[MAX_PAYLOAD_LEN];

// MQTT topics
const char* topic_connect_status                 = "g/c";
const char* topic_gate_position                  = "g/p";
const char* topic_relay_open_pulse               = "g/o";
const char* topic_relay_open_automatic           = "g/oat";
const char* topic_input_open_automatic           = "g/i/oat";
const char* topic_setting_loop_autoopen_set      = "g/i/s";
const char* topic_setting_loop_autoopen_info     = "g/i/i";
const char* topic_mailbox                        = "d/m";
const char* topic_home_ring                      = "d/r";

// Defining classes
EthernetClient ethClient;
MQTTClient mqttClient;

// Global constants
const unsigned long period_mqtt_msg = 30000; // 30s
const unsigned long period_relay_pulse = 1000; // 1s
const unsigned long period_max_waiting_autoclose = 600000; // 10min 600000
const unsigned long period_delay_autoclose = 3000; // 3s
const unsigned long period_fast_scan_inputs = 250; // ms
const unsigned long period_slow_scan_inputs = 1100; // 1,1s
const unsigned long period_message_clear_way = 2000; // 2s
const unsigned long period_mqtt_reconnected = 3000; // 3s
const char* gate_positions_texts[] = {
                                      "v pohybu",
                                      "zavreno",
                                      "otevreno",
                                      "otevreno pro prujezd",
                                      "mezipoloha",
                                      "nedefinovano"
};
const char* text_changed_state_to = " changed state to ";
const char* text_autoclose_canceled = "Autoclose canceled.";

// Global variables
byte index_gate_position = 5;
unsigned long timing_for_periodic_mqtt_msg = 0;
unsigned long timing_for_relay_pulse = 0;
bool relay_open_active = 0;
unsigned long timing_for_cancel_autoclose = 0;
bool autoclose_activated = 0;
bool autoclose_planned_close_signal = 0;
unsigned long timing_delay_autoclose = 0;
unsigned long timing_fast_scan_inputs = 0;
unsigned long timing_slow_scan_inputs = 0;
unsigned long timing_message_clear_way = 0;
bool setting_loop_autoopen;
unsigned long timing_mqtt_reconnected = 0;

// Variables to memorize last states
int last_sensor_mailbox;
int last_home_ring;
int last_input_open_automatic;
int last_induction_loop;



void connectMqtt() {
  Serial.print(F("MQTT connect"));
  mqttClient.disconnect();
  Ethernet.maintain();

  mqttClient.begin(mqtt_server, ethClient);
  mqttClient.onMessage(messageReceived);
  mqttClient.setWill(topic_connect_status, "offline", true, 0);  // retained, QoS
  mqttClient.setKeepAlive(30);
  mqttClient.setTimeout(40);

  int repeats = 10;
  while (!mqttClient.connect(mqtt_name, mqtt_name, mqtt_password) && repeats > 0) {
    Serial.print(F("."));
    repeats = repeats - 1;
    delay(2000);
  }

  if (mqttClient.connected()){
    timing_mqtt_reconnected = millis();
    Serial.print(F("ed to "));
    Serial.println(mqtt_server);

    mqttClient.subscribe(topic_relay_open_pulse);
    mqttClient.subscribe(topic_relay_open_automatic);
    mqttClient.subscribe(topic_setting_loop_autoopen_set);
  }
  else
    Serial.println(F("error"));

}



void messageReceived(String &topic, String &payload) {
  if (payload.length() >= MAX_PAYLOAD_LEN) {
    Serial.println(F("dropped MQTT payload!"));
    return;
  }
  topic.toCharArray(received_topic, MAX_TOPIC_LEN);
  payload.toCharArray(received_message, MAX_PAYLOAD_LEN);
}



void clearMessages() {
  received_topic[0] = '\0';
  received_message[0] = '\0';
} 



byte returnGatePosition() {    // returns index to parse word in variable gatePosition[]
  if ( digitalRead(pin_motor_running) == 1 )
      return 0;
  else if ( digitalRead(pin_limiter_closed) == 0 && digitalRead(pin_limiter_opened) == 1 )
      return 1;
  else if ( digitalRead(pin_limiter_closed) == 1 && digitalRead(pin_limiter_opened) == 0 && autoclose_activated == 0 )
      return 2;
  else if ( digitalRead(pin_limiter_closed) == 1 && digitalRead(pin_limiter_opened) == 0 && autoclose_activated == 1 )
      return 3;
  else if ( digitalRead(pin_limiter_closed) == 1 && digitalRead(pin_limiter_opened) == 1 && digitalRead(pin_motor_running) == 0 )
      return 4;
  else
      return 5;
}



void makeOpenGatePulse() {
  digitalWrite(pin_relay_open, HIGH);
  relay_open_active = 1;
  timing_for_relay_pulse = millis();
}



void makeOpenGateAutomatic() {
  if ( autoclose_activated == 1 ) {
    Serial.println(text_autoclose_canceled);
    autoclose_activated = 0;
    autoclose_planned_close_signal = 0;
    return;
  }

  if (index_gate_position > 3) {
    Serial.println(F("Not accepted!"));
    return;
  }

  if (digitalRead(pin_limiter_closed) == 0 || autoclose_planned_close_signal == 1)
    makeOpenGatePulse();

  autoclose_activated = 1;
  timing_for_cancel_autoclose = millis();
  Serial.println(F("10min started."));
}



void fastScanInputs() {
  if (digitalRead(pin_home_ring) != last_home_ring ) {
    last_home_ring = digitalRead(pin_home_ring);
    Serial.print(topic_home_ring);
    Serial.print(text_changed_state_to);
    Serial.println(last_home_ring);
    mqttClient.publish (topic_home_ring, last_home_ring);
  }

  if (returnGatePosition() != index_gate_position ) {
    index_gate_position = returnGatePosition();
    mqttClient.publish(topic_gate_position, gate_positions_texts[index_gate_position]);
  } 

  if (digitalRead(pin_induction_loop) != last_induction_loop) {
    last_induction_loop = digitalRead(pin_induction_loop);
    Serial.print(F("Ind"));
    Serial.print(text_changed_state_to);
    Serial.println(last_induction_loop);

    if (setting_loop_autoopen == 1 && digitalRead(pin_limiter_closed) == 0 && last_induction_loop == 1 && autoclose_activated == 0)
      makeOpenGateAutomatic();
    
  }
}



void slowScanInputs() {
  if (digitalRead(pin_sensor_mailbox) != last_sensor_mailbox ) {
    last_sensor_mailbox = digitalRead(pin_sensor_mailbox);
    Serial.print(topic_mailbox);
    Serial.print(text_changed_state_to);
    Serial.println(last_sensor_mailbox);
    mqttClient.publish (topic_mailbox, last_sensor_mailbox);
  }

  if (digitalRead(pin_input_open_automatic) != last_input_open_automatic) {
    last_input_open_automatic = digitalRead(pin_input_open_automatic);
    Serial.print(topic_input_open_automatic);
    Serial.print(text_changed_state_to);
    Serial.println(last_input_open_automatic);
    mqttClient.publish (topic_input_open_automatic, last_input_open_automatic);
    makeOpenGateAutomatic();
  }
}



void setupIoPins() {
  pinMode(pin_motor_running,  INPUT);
  pinMode(pin_relay_open, OUTPUT);
  digitalWrite(pin_relay_open, LOW);
  
  pinMode(pin_limiter_closed, INPUT);
  pinMode(pin_limiter_opened, INPUT);

  pinMode(pin_photocell_outside, INPUT);
  pinMode(pin_photocell_inside,  INPUT);
  pinMode(pin_induction_loop,    INPUT);

  pinMode(pin_input_open_pulse,     INPUT);
  pinMode(pin_input_open_automatic, INPUT);

  pinMode(pin_sensor_mailbox, INPUT_PULLUP);
  pinMode(pin_home_ring, INPUT);
}



void setup() {
  setupIoPins();
  last_sensor_mailbox = digitalRead(pin_sensor_mailbox);
  last_home_ring = digitalRead(pin_home_ring);
  index_gate_position = returnGatePosition();
  last_input_open_automatic = digitalRead(pin_input_open_automatic);
  last_induction_loop = digitalRead(pin_induction_loop);
  delay(3000);
  
  Serial.begin(9600);
  Serial.print(F("'\nGarduino starting with IP "));
  Ethernet.begin(mac);
  Serial.println(Ethernet.localIP());
  delay(500);
  connectMqtt();

  setting_loop_autoopen = EEPROM.read(0);
}



void loop() {
  mqttClient.loop();

  if (!mqttClient.connected()) {
    connectMqtt();
  }

  // timers
  if ( millis() - timing_for_periodic_mqtt_msg > period_mqtt_msg ) {
    timing_for_periodic_mqtt_msg = millis();
    mqttClient.publish(topic_connect_status, "online");
    index_gate_position = returnGatePosition();
    mqttClient.publish(topic_gate_position, gate_positions_texts[index_gate_position]);
    mqttClient.publish(topic_setting_loop_autoopen_info, setting_loop_autoopen ? "1" : "0");
  }

  if ( millis() - timing_fast_scan_inputs > period_fast_scan_inputs )
    fastScanInputs();
  
  if ( millis() - timing_slow_scan_inputs > period_slow_scan_inputs )
    slowScanInputs();

  if ( (millis() - timing_for_relay_pulse > period_relay_pulse) && relay_open_active == 1 ) {
    relay_open_active = 0;
    digitalWrite(pin_relay_open, LOW);
  }

  if ( (millis() - timing_for_cancel_autoclose > period_max_waiting_autoclose) && autoclose_activated == 1 ) {
    autoclose_activated = 0;  
    autoclose_planned_close_signal = 0;
    Serial.println(text_autoclose_canceled);
    index_gate_position = returnGatePosition();
    mqttClient.publish(topic_gate_position, gate_positions_texts[index_gate_position]);
  }

  if ( (millis() - timing_delay_autoclose > period_delay_autoclose) && autoclose_activated == 1 && autoclose_planned_close_signal == 1 ) {
    
    if ( millis() - timing_message_clear_way > period_message_clear_way ) {
      timing_message_clear_way = millis();
      Serial.println(F("Waiting for clear way."));
    }
    if ( digitalRead(pin_induction_loop) == 0 && digitalRead(pin_photocell_outside) == 0 && digitalRead(pin_photocell_inside) == 0 ) {
      autoclose_activated = 0;
      makeOpenGatePulse();
      autoclose_planned_close_signal = 0;
    }
  }

  // processing of MQTT tasks
  if ( millis() - timing_mqtt_reconnected < period_mqtt_reconnected )
    clearMessages();    // drop false-positive commands after reconnect
  
  if (received_message[0] != '\0') {
      Serial.print(F("Incoming MQTT: "));
      Serial.print(received_topic);
      Serial.print(F(": "));
      Serial.println(received_message);


      if (strcmp(received_topic, topic_relay_open_pulse) == 0) {
        makeOpenGatePulse();
      }

      if (strcmp(received_topic, topic_relay_open_automatic) == 0) {
        makeOpenGateAutomatic();
      }

      if (strcmp(received_topic, topic_setting_loop_autoopen_set) == 0) {
        if (received_message[0] == '1')
          setting_loop_autoopen = 1;
        else
          setting_loop_autoopen = 0;

        EEPROM.update(0, setting_loop_autoopen);
        Serial.print(F("EEPROM "));
        Serial.print(topic_setting_loop_autoopen_set);
        Serial.print(F(" updated to "));
        Serial.println(setting_loop_autoopen);
        mqttClient.publish(topic_setting_loop_autoopen_info, setting_loop_autoopen ? "1" : "0");
      }
      

      clearMessages();
  }


  // autoclose logic
  if ( autoclose_activated == 1 && digitalRead(pin_induction_loop) == 1 && autoclose_planned_close_signal == 0 ) {
    autoclose_activated = 0;
    autoclose_planned_close_signal = 1;
    Serial.println(F("Vehicle on loop."));
  }
    
  if ( autoclose_activated == 0 && digitalRead(pin_induction_loop) == 0 && autoclose_planned_close_signal == 1 ) {
    autoclose_activated = 1;
    timing_delay_autoclose = millis();
    Serial.println(F("Delay started."));
  }
    
  
  







}
