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
*
* Instalace knihven:
* Nutno doinstalovat knihovnu Ticker ve verzi 4.0 nebo novější z GitHub Repozitáře - https://github.com/sstaub/Ticker
*
* Verze 1.0 09/2025
* marek@vach.cz
*/

#include <Ethernet.h>
#include <MQTT.h>
#include "Ticker.h"

// MQTT topics
const char* topic_connect_status                 = "g/connect";
const char* topic_mailbox                        = "g/s/mail";
const char* topic_gate_state                     = "g/state";
const char* topic_relay_close_set                = "g/r/cl/set";
const char* topic_relay_close_response           = "g/r/cl/resp";
const char* topic_relay_open_car_set             = "g/r/op_car/set";
const char* topic_relay_open_car_response        = "g/r/op_car/resp";
const char* topic_relay_open_pedestrian_set      = "g/r/op_ped/set";
const char* topic_relay_open_pedestrian_response = "g/r/op_ped/resp";
const char* topic_log_action                     = "g/l/action";

// values for your network.
byte mac[]    = { 0x02, 0x17, 0x3A, 0x4B, 0x5C, 0x6E };
const char* mqtt_server      = "192.168.0.2"; // used for MQTT server and for check connectivity = if webserver is running on same machine at port 80
const int   mqtt_port        = 1883;
const char* mqtt_name        = "garduino";
const char* mqtt_user        = "gate";
const char* mqtt_password    = "Drainpipe";

EthernetClient ethClient;
MQTTClient mqttClient(128);

// MQTT communication variables
String received_topic = ""; 
String received_message = "";
String log_message = "";


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

const int pin_remote_open = A3;
const int pin_remote_close = A4;
// A5 pin free for future use


// Variables to memorize last states
int last_sensor_mailbox;
int last_sensor_closed;
int last_button_car;
int last_button_pedestrian;
int last_photocell_outside;
int last_photocell_inside;
int last_induction_loop;
int last_remote_open;
int last_remote_close;

int input_state;




boolean reconnectEthernet() {
  Serial.println("Obtaining IP adress form DHCP...");
  Ethernet.begin(mac);  // if you want static, you must add second parameter IP
  delay(1500);

  if (Ethernet.localIP() != IPAddress(0,0,0,0)) {
    Serial.print("Success! IP adress is ");
    Serial.println(Ethernet.localIP());
    return true;
  }
  else {
    Serial.println("error");
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

  Serial.println("Connecting MQTT...");
  mqttClient.connect(mqtt_name, mqtt_user, mqtt_password);
  delay(1500);

  if (mqttClient.connected()){
    Serial.print("MQTT connected to server ");
    Serial.println(mqtt_server);

    mqttClient.publish(topic_connect_status, "online");
    mqttClient.subscribe(topic_relay_close_set);
    mqttClient.subscribe(topic_relay_open_car_set);
    mqttClient.subscribe(topic_relay_open_pedestrian_set);
  } 
  else {
    Serial.print("error - MQTT library code is: ");
    Serial.println(mqttClient.lastError());
  } 
} 


void messageReceived(String &topic, String &payload) {
  received_topic = topic;
  received_message = payload;
}


void maintainMQTT() {
  mqttClient.loop();
}


void checkAndRepairConnectivity() {
    if (!mqttClient.connected()) {
      Serial.println("Detected MQTT communication failure in main loop.");

      if (!ethClient.connect(mqtt_server, 80)) {   // based on my constalation I have webserver on same machine running on port 80. If you don't, you must change way to know if ethernet is working
        Serial.println("Detected Ethernet communication failure in main loop.");
        if (reconnectEthernet()) {
          reconnectMQTT();
          return;
        }
        else {
          Serial.println("Ethernet failed to recover, skipping reconecting of MQTT...");
          return;
        }
      }
      else {
        Serial.println("Check Ethernet connection - OK.");
        reconnectMQTT();
      }
    }
    else
      Serial.println("Periodic MQTT connection check - OK.");
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

  pinMode(pin_remote_open,  INPUT_PULLUP);
  pinMode(pin_remote_close, INPUT_PULLUP);

  //turnRelaysOff();
  digitalWrite(pin_relay_close,           LOW);
  digitalWrite(pin_relay_open_car,        LOW);
  digitalWrite(pin_relay_open_pedestrian, LOW);
/*
  last_sensor_mailbox    = digitalRead(pin_sensor_mailbox);
  last_sensor_closed     = digitalRead(pin_sensor_closed);
  last_button_car        = digitalRead(pin_button_car);
  last_button_pedestrian = digitalRead(pin_button_pedestrian);
  last_photocell_outside = digitalRead(pin_photocell_outside);
  last_photocell_inside  = digitalRead(pin_photocell_inside);
  last_induction_loop    = digitalRead(pin_induction_loop);
  last_remote_open       = digitalRead(pin_remote_open);
  last_remote_close      = digitalRead(pin_remote_close);
*/
}

void turnRelaysOff() {
  digitalWrite(pin_relay_close,           LOW);
  digitalWrite(pin_relay_open_car,        LOW);
  digitalWrite(pin_relay_open_pedestrian, LOW);
  Serial.println("Všechna relé vypnuta.");
}


Ticker timer_check_connectivity(checkAndRepairConnectivity, 120000);         // cals function every 120s
Ticker timer_maintain_ethernet(maintainEthernet, 1200000);                   // 20min
Ticker timer_maintain_mqtt(maintainMQTT, 3000);                              // 3s
//Ticker timer_relays_off(turnRelaysOff, 5000, 1);                             // 2s repeat 1x



void setup() {
  Serial.begin(9600);
  Serial.println("Starting Arduino gate control over HomeAssistant...");
  setupIoPins();

  reconnectEthernet();
  reconnectMQTT();

  timer_check_connectivity.start();
  timer_maintain_ethernet.start();
  timer_maintain_mqtt.start();

}

void loop() {
  timer_check_connectivity.update();
  timer_maintain_ethernet.update();
  timer_maintain_mqtt.update();
  //timer_relays_off.update();

  if (received_message != ""){
    Serial.println("Incoming MQTT: " + received_topic + " - " + received_message);
/*
    if (timer_relays_off.state() == RUNNING) {
        Serial.println("Detekován tikající timer, nelze provést!");
    }
*/
    if (received_topic == topic_relay_close_set) {
        mqttClient.publish(topic_relay_close_response, "1");
        // exec relay
        Serial.println("Zapínám relé pro zavření a timer pro vypnutí.");
        //timer_relays_off.start();
        mqttClient.publish(topic_relay_close_response, "0"); 
    }

    if (received_topic == topic_relay_open_car_set) {
        mqttClient.publish(topic_relay_open_car_response, "1");
        // exec relay
        mqttClient.publish(topic_relay_open_car_response, "0"); 
    }

    if (received_topic == topic_relay_open_pedestrian_set) {
        mqttClient.publish(topic_relay_open_pedestrian_response, "1");
        // exec relay
        mqttClient.publish(topic_relay_open_pedestrian_response, "0"); 
    }

    received_topic = "";
    received_message = "";
  } 

}
