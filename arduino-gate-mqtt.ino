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
#include "Ticker.h"



// MQTT topics
const char* topic_connect_status                 = "g/connect";
const char* topic_mailbox                        = "g/mail";
const char* topic_gate_mode                      = "g/mode";          // 0 = Opened, 1 = Closed, 2 = Opening, 3 = Closing
const char* topic_relay_close_set                = "g/cl/set";
const char* topic_relay_close_response           = "g/cl/resp";
const char* topic_relay_open_car_set             = "g/op_car/set";
const char* topic_relay_open_car_response        = "g/op_car/resp";
const char* topic_relay_open_pedestrian_set      = "g/op_ped/set";
const char* topic_relay_open_pedestrian_response = "g/op_ped/resp";
const char* topic_button_car                     = "g/b/car";
const char* topic_button_pedestrian              = "g/b/ped";
const char* topic_photocell_outside              = "g/ph/out";
const char* topic_photocell_inside               = "g/ph/in";
const char* topic_induction_loop                 = "g/ind";
const char* topic_radio_open                     = "g/r/op";
const char* topic_radio_close                    = "g/r/cl";



// values for your network.
byte mac[]    = { 0x02, 0x17, 0x3A, 0x4B, 0x5C, 0x6E };
const char* mqtt_server      = "192.168.0.2"; // used for MQTT server and for check connectivity = if webserver is running on same machine at port 80
const int   mqtt_port        = 1883;
const char* mqtt_name        = "garduino";
const char* mqtt_user        = "gate";
const char* mqtt_password    = "Drainpipe";

EthernetClient ethClient;
MQTTClient mqttClient;



// MQTT communication variables
#define MAX_TOPIC_LEN 16
#define MAX_PAYLOAD_LEN 7
char received_topic[MAX_TOPIC_LEN];
char received_message[MAX_PAYLOAD_LEN];



// HW pinout section
const int pin_sensor_mailbox = 2;
const int pin_sensor_closed = 3;

// Relay module **never use more relay then one at same time because of internal Arduino power supply will overload
// pin 4 reserved for chip select of SD card.
const int pin_relay_close = 5;
const int pin_relay_open_car = 6;
const int pin_relay_open_pedestrian = 7;

const int pin_button_car = 8;
const int pin_button_pedestrian = 9;
// pin 10 reserved for chip select of Ethernet W5100

const int pin_photocell_outside = A0;
const int pin_photocell_inside = A1;
const int pin_induction_loop = A2;

const int pin_radio_open = A3;
const int pin_radio_close = A4;
// A5 pin free for future use



// Variables to memorize last states
int last_sensor_mailbox;
int last_sensor_closed;
int last_button_car;
int last_button_pedestrian;
int last_photocell_outside;
int last_photocell_inside;
int last_induction_loop;
int last_radio_open;
int last_radio_close;

int input_state;

// Timing for gate mode view
const unsigned long time_of_gate_moving = 7000;  // Time in millis for blocking override topic_gate_mode when is Opening or Closing.
bool gate_is_moving = false;  // Control variable to block updating of pin_sensor_closed for time given line upper.


boolean reconnectEthernet() {
  Serial.println(F("Obtaining IP adress form DHCP..."));
  Ethernet.begin(mac);  // if you want static, you must add second parameter IP
  delay(1500);

  if (Ethernet.localIP() != IPAddress(0,0,0,0)) {
    Serial.print(F("Success! IP adress is "));
    Serial.println(Ethernet.localIP());
    return true;
  }
  else {
    Serial.println(F("error"));
    return false;
  }
}


void maintainEthernet() {
  Ethernet.maintain();
}


void reconnectMQTT() {
  mqttClient.disconnect();
  mqttClient.begin(mqtt_server, mqtt_port, ethClient);
  mqttClient.onMessage(messageReceived);
  mqttClient.setWill(topic_connect_status, "offline", true, 0);  // retained, QoS
  mqttClient.setKeepAlive(30);
  mqttClient.setTimeout(40);

  Serial.println(F("Connecting MQTT..."));
  mqttClient.connect(mqtt_name, mqtt_user, mqtt_password);
  delay(1500);

  if (mqttClient.connected()){
    Serial.print(F("MQTT connected to server "));
    Serial.println(mqtt_server);

    // mqttClient.publish(topic_connect_status, "online");
    MqttPeriodicReport();

    mqttClient.subscribe(topic_relay_close_set);
    mqttClient.subscribe(topic_relay_open_car_set);
    mqttClient.subscribe(topic_relay_open_pedestrian_set);
  } 
  else {
    Serial.print(F("error - MQTT library code is: "));
    Serial.println(mqttClient.lastError());
  } 
} 


void MqttPeriodicReport() {
  mqttClient.publish(topic_connect_status, "online");

  input_state = digitalRead( pin_sensor_mailbox );
  mqttClient.publish (topic_mailbox, input_state ? "1" : "0");

  if (gate_is_moving == false) {
    input_state = digitalRead( pin_sensor_closed );
    mqttClient.publish (topic_gate_mode, input_state ? "1" : "0");
  }
}


void messageReceived(String &topic, String &payload) {
  if ( millis() < 10000 ) // Fixing bug when arduino became online in Home Assistant it trigger all switches (open_car, open_pedestrian, close) to ON for a moment. 
    return;               // This will drop incomming signals in first 10s of arduino program.
  
  topic.toCharArray(received_topic, MAX_TOPIC_LEN);
  payload.toCharArray(received_message, MAX_PAYLOAD_LEN);
}


void maintainMQTT() {
  mqttClient.loop();
}


void checkAndRepairConnectivity() {
    if (!mqttClient.connected()) {
      Serial.println(F("Detected MQTT communication failure in main loop."));

      if (!ethClient.connect(mqtt_server, 80)) {   // based on my constalation I have webserver on same machine running on port 80. If you don't, you must change way to know if ethernet is working
        Serial.println(F("Detected Ethernet communication failure in main loop."));
        if (reconnectEthernet()) {
          reconnectMQTT();
          return;
        }
        else {
          Serial.println(F("Ethernet failed to recover, skipping reconecting of MQTT..."));
          return;
        }
      }
      else {
        Serial.println(F("Check Ethernet connection - OK."));
        reconnectMQTT();
      }
    }
    else
      Serial.println(F("Periodic MQTT connection check - OK."));
}


void setupIoPins() {
  pinMode(pin_sensor_mailbox, INPUT_PULLUP);
  pinMode(pin_sensor_closed,  INPUT_PULLUP);

  pinMode(pin_relay_close,           OUTPUT);
  pinMode(pin_relay_open_car,        OUTPUT);
  pinMode(pin_relay_open_pedestrian, OUTPUT);

  pinMode(pin_button_car,        INPUT_PULLUP);
  pinMode(pin_button_pedestrian, INPUT_PULLUP);

  pinMode(pin_photocell_outside, INPUT_PULLUP);
  pinMode(pin_photocell_inside,  INPUT_PULLUP);
  pinMode(pin_induction_loop,    INPUT_PULLUP);

  pinMode(pin_radio_open,  INPUT_PULLUP);
  pinMode(pin_radio_close, INPUT_PULLUP);

  turnRelaysOff();

  last_sensor_mailbox    = digitalRead(pin_sensor_mailbox);
  last_sensor_closed     = digitalRead(pin_sensor_closed);
  last_button_car        = digitalRead(pin_button_car);
  last_button_pedestrian = digitalRead(pin_button_pedestrian);
  last_photocell_outside = digitalRead(pin_photocell_outside);
  last_photocell_inside  = digitalRead(pin_photocell_inside);
  last_induction_loop    = digitalRead(pin_induction_loop);
  last_radio_open        = digitalRead(pin_radio_open);
  last_radio_close       = digitalRead(pin_radio_close);
}


void checkInputsForChanges() {
  const int input_pins[9]    = {
                                pin_sensor_mailbox, 
                                pin_sensor_closed, 
                                pin_button_car, 
                                pin_button_pedestrian, 
                                pin_photocell_outside, 
                                pin_photocell_inside, 
                                pin_induction_loop, 
                                pin_radio_open, 
                                pin_radio_close
                              };
  int* last_inputs[9]        = {
                                &last_sensor_mailbox, 
                                &last_sensor_closed, 
                                &last_button_car, 
                                &last_button_pedestrian, 
                                &last_photocell_outside, 
                                &last_photocell_inside, 
                                &last_induction_loop, 
                                &last_radio_open, 
                                &last_radio_close
                              };
  const char* mqtt_topics[9] = {
                                topic_mailbox, 
                                topic_gate_mode, 
                                topic_button_car, 
                                topic_button_pedestrian, 
                                topic_photocell_outside, 
                                topic_photocell_inside, 
                                topic_induction_loop, 
                                topic_radio_open, 
                                topic_radio_close
                              };

  for (int i = 0; i < 9; i++) {
    if ( i == 1 && gate_is_moving )
      continue;

    input_state = digitalRead ( input_pins[i] );

    if ( input_state != *last_inputs[i] ) {
      *last_inputs[i] = input_state;
      mqttClient.publish (mqtt_topics[i], input_state ? "1" : "0");

      Serial.print( mqtt_topics[i] );
      Serial.print(F(" changed state to: "));
      Serial.println( input_state );
      
    }
  }
}


void turnRelaysOff() {
  digitalWrite(pin_relay_close,           LOW);
  digitalWrite(pin_relay_open_car,        LOW);
  digitalWrite(pin_relay_open_pedestrian, LOW);

  mqttClient.publish(topic_relay_close_response, "0");
  mqttClient.publish(topic_relay_open_car_response, "0");
  mqttClient.publish(topic_relay_open_pedestrian_response, "0");
}


void gateIsMoving() {
  gate_is_moving = false;
  last_sensor_closed = 2; // force refresh of input in next comparing cycle
}


void clearMessages() {
  received_topic[0] = '\0';
  received_message[0] = '\0';
} 


Ticker timer_check_connectivity(checkAndRepairConnectivity, 120000);         // cals function every 120s
Ticker timer_maintain_ethernet(maintainEthernet, 1200000);                   // 20min
Ticker timer_maintain_mqtt(maintainMQTT, 1000);                              // 1s
Ticker timer_relays_off(turnRelaysOff, 2000, 1);                             // 2s repeated 1x
Ticker timer_check_inputs(checkInputsForChanges, 1000);                      // 1s
Ticker timer_mqtt_periodic_report(MqttPeriodicReport, 300000);               // 5min
Ticker timer_gate_is_moving(gateIsMoving, time_of_gate_moving, 1);           // 7s repeated 1x


void setup() {
  Serial.begin(9600);
  Serial.println(F("Starting Arduino gate control over HomeAssistant..."));
  setupIoPins();

  reconnectEthernet();
  reconnectMQTT();

  timer_check_connectivity.start();
  timer_maintain_ethernet.start();
  timer_maintain_mqtt.start();
  timer_check_inputs.start();
  timer_mqtt_periodic_report.start();
}


void loop() {
  timer_check_connectivity.update();
  timer_maintain_ethernet.update();
  timer_maintain_mqtt.update();
  timer_relays_off.update();
  timer_check_inputs.update();
  timer_mqtt_periodic_report.update();
  timer_gate_is_moving.update();

  if (received_message[0] != '\0') {
      Serial.print(F("Incoming MQTT: "));
      Serial.print(received_topic);
      Serial.print(F(" - "));
      Serial.println(received_message);

      if (timer_relays_off.state() == RUNNING) {
        Serial.println(F("Dropping, one relay is already running!"));
        clearMessages();
      } 

      if (strcmp(received_topic, topic_relay_close_set) == 0) {
          mqttClient.publish(topic_relay_close_response, "1");
          mqttClient.publish(topic_gate_mode, "3"); 
          digitalWrite(pin_relay_close, HIGH);
          timer_relays_off.start();
          timer_gate_is_moving.start();
          gate_is_moving = true;
      }

      if (strcmp(received_topic, topic_relay_open_car_set) == 0) {
          mqttClient.publish(topic_relay_open_car_response, "1");
          mqttClient.publish(topic_gate_mode, "2"); 
          digitalWrite(pin_relay_open_car, HIGH);
          timer_relays_off.start();
          timer_gate_is_moving.start();
          gate_is_moving = true;
      }

      if (strcmp(received_topic, topic_relay_open_pedestrian_set) == 0) {
          mqttClient.publish(topic_relay_open_pedestrian_response, "1");
          mqttClient.publish(topic_gate_mode, "2"); 
          digitalWrite(pin_relay_open_pedestrian, HIGH);
          timer_relays_off.start();
          timer_gate_is_moving.start();
          gate_is_moving = true;
      }

      clearMessages();
  }
}




