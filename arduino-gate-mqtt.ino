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
const char* topic_relay_open_pulse               = "g/r/o";
const char* topic_relay_open_automatic           = "g/r/oat";
const char* topic_photocell_outside              = "g/ph/out";
const char* topic_photocell_inside               = "g/ph/in";
const char* topic_induction_loop                 = "g/ind";
const char* topic_input_open_pulse               = "g/i/o";
const char* topic_input_open_automatic           = "g/i/oat";
const char* topic_mailbox                        = "d/mail";
const char* topic_home_ring                      = "d/ring";
const char* topic_gate_position                  = "g/pos";


// values for your network.
byte mac[]    = { 0x02, 0x17, 0x3A, 0x4B, 0x5C, 0x6E };
const char* mqtt_server      = "192.168.0.8";
const int   mqtt_port        = 1883;
const int   web_port_ha      = 8123;
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


// Variables to memorize last states
int last_sensor_mailbox;
int last_photocell_outside;
int last_photocell_inside;
int last_induction_loop;
int last_input_open_pulse;
int last_input_open_automatic;
int last_home_ring;
int last_gate_position;

int input_state;
int gate_position;



boolean reconnectEthernet() {
  Serial.println(F("IP from DHCP..."));
  Ethernet.begin(mac);  // if you want static, you must add second parameter IP
  delay(1500);

  if (Ethernet.localIP() != IPAddress(0,0,0,0)) {
    Serial.print(F("New IP "));
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

  Serial.println(F("Cnctg MQTT..."));
  mqttClient.connect(mqtt_name, mqtt_user, mqtt_password);
  delay(1500);

  if (mqttClient.connected()){
    Serial.print(F("MQTT cnctd to srv "));
    Serial.println(mqtt_server);

    MqttPeriodicReport();
    if ( millis() < 10000 )
      mqttClient.publish(topic_gate_position, returnGatePosition());
    

    mqttClient.subscribe(topic_relay_open_pulse);
    mqttClient.subscribe(topic_relay_open_automatic);
  } 
  else {
    Serial.print(F("error - MQTT lib code is: "));
    Serial.println(mqttClient.lastError());
  } 
} 


void MqttPeriodicReport() {
  mqttClient.publish(topic_connect_status, "online");
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
      Serial.println(F("MQTT failure."));

      if (!ethClient.connect(mqtt_server, web_port_ha)) {    // check if HA is online = LAN is working
        Serial.println(F("Ethernet failure."));
        if (reconnectEthernet()) {
          reconnectMQTT();
          return;
        }
        else {
          Serial.println(F("so skipping MQTT too."));
          return;
        }
      }
      else {
        Serial.println(F("Eth OK."));
        reconnectMQTT();
      }
    }
    else
      Serial.println(F("MQTT OK."));
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
  pinMode(pin_home_ring,      INPUT);

  
  last_photocell_outside    = digitalRead(pin_photocell_outside);
  last_photocell_inside     = digitalRead(pin_photocell_inside);
  last_induction_loop       = digitalRead(pin_induction_loop);
  last_input_open_pulse     = digitalRead(pin_input_open_pulse);
  last_input_open_automatic = digitalRead(pin_input_open_automatic);
  last_sensor_mailbox       = digitalRead(pin_sensor_mailbox);
  last_home_ring            = digitalRead(pin_home_ring);
  last_gate_position        = returnGatePosition();
}



int returnGatePosition() {    // 1=running, 2=closed, 3=opened, 4=unkown_position 5=auto_close
  if ( digitalRead(pin_motor_running) == 1 )
      return 3;
  else if ( digitalRead(pin_limiter_closed) == 1 )
      return 1;
  else if ( digitalRead(pin_limiter_opened) == 1 )
      return 2;
  else
      return 4;
}



void checkInputsForChanges() {
  const int key_count = 8;    // number of elements in array

  const int input_pins[key_count]    = {
                                        pin_photocell_outside, 
                                        pin_photocell_inside, 
                                        pin_induction_loop, 
                                        pin_input_open_pulse, 
                                        pin_input_open_automatic,
                                        pin_sensor_mailbox, 
                                        pin_home_ring,
                                        100
                                       };
  int* last_inputs[key_count]        = {
                                        &last_photocell_outside, 
                                        &last_photocell_inside, 
                                        &last_induction_loop, 
                                        &last_input_open_pulse, 
                                        &last_input_open_automatic,
                                        &last_sensor_mailbox,
                                        &last_home_ring,
                                        &last_gate_position
                                       };
  const char* mqtt_topics[key_count] = {
                                        topic_photocell_outside, 
                                        topic_photocell_inside, 
                                        topic_induction_loop, 
                                        topic_input_open_pulse, 
                                        topic_input_open_automatic,
                                        topic_mailbox, 
                                        topic_home_ring,
                                        topic_gate_position
                                       };

  for (int i = 0; i < key_count; i++) {
    if ( i == key_count - 1 )
      input_state = returnGatePosition();
    else
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



void clearMessages() {
  received_topic[0] = '\0';
  received_message[0] = '\0';
} 



void makeOpenGatePulse() {
  digitalWrite(pin_relay_open, HIGH);
  timer_turn_relays_off.start();
}



void makeOpenGateAutomatic() {

}



void turnRelaysOff() {
  digitalWrite(pin_relay_open, LOW);
}



Ticker timer_check_connectivity(checkAndRepairConnectivity, 120000);         // cals function every 120s
Ticker timer_maintain_ethernet(maintainEthernet, 1200000);                   // 20min
Ticker timer_maintain_mqtt(maintainMQTT, 1000);                              // 1s
Ticker timer_check_inputs(checkInputsForChanges, 100);                       // 0.1s
Ticker timer_mqtt_periodic_report(MqttPeriodicReport, 300000);               // 5min
Ticker timer_turn_relays_off(turnRelaysOff, 1000, 1);                        // 1s, repeated once


void setup() {
  Serial.begin(9600);
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
  timer_check_inputs.update();
  timer_mqtt_periodic_report.update();
  timer_turn_relays_off.update();

  if (received_message[0] != '\0') {
      Serial.print(F("Incoming MQTT: "));
      Serial.print(received_topic);
      Serial.print(F(": "));
      Serial.println(received_message);


      if (strcmp(received_topic, topic_relay_open_pulse) == 0) {
        makeOpenGatePulse();
      }

      clearMessages();
  }
}




