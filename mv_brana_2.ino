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

#include <SPI.h>
#include <Ethernet.h>
#include <PubSubClient.h>
#include "Ticker.h"


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

// MQTT topics
const char* topic_device_status                = "g/stat";
const char* topic_mailbox                      = "g/s/mail";
const char* topic_closed_limit                 = "g/s/cl_lim";
const char* topic_relay_close_set              = "g/rl/cl/set";
const char* topic_relay_accept_command         = "g/rl/ok";
const char* topic_relay_open_car_set           = "g/rl/op_car/set";
const char* topic_relay_open_pedestrian_set    = "g/rl/op_ped/set";
const char* topic_button_car                   = "g/b/car";
const char* topic_button_pedestrian            = "g/b/ped";
const char* topic_photocell_outside            = "g/s/pho_o";
const char* topic_photocell_inside             = "g/s/pho_i";
const char* topic_induction_loop               = "g/s/ind";
const char* topic_remote_open                  = "g/rm/op";
const char* topic_remote_close                 = "g/rm/cl";


// timers are defined at bottom before setup() function
int state_of_timer_end_relay_impulse = 0;


// values for your network.
byte mac[]    = { 0x02, 0x17, 0x3A, 0x4B, 0x5C, 0x6E };
const char* mqtt_server      = "192.168.0.2"; // used for MQTT server and for check connectivity = if webserver is running on same machine at port 80
const int   mqtt_port        = 1883;
const char* mqtt_name        = "garduino";
const char* mqtt_user        = "gate";
const char* mqtt_password    = "Drainpipe";

EthernetClient ethClient;
PubSubClient mqttClient(ethClient);




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

  digitalWrite(pin_relay_close,           LOW);
  digitalWrite(pin_relay_open_car,        LOW);
  digitalWrite(pin_relay_open_pedestrian, LOW);

  last_sensor_mailbox    = digitalRead(pin_sensor_mailbox);
  last_sensor_closed     = digitalRead(pin_sensor_closed);
  last_button_car        = digitalRead(pin_button_car);
  last_button_pedestrian = digitalRead(pin_button_pedestrian);
  last_photocell_outside = digitalRead(pin_photocell_outside);
  last_photocell_inside  = digitalRead(pin_photocell_inside);
  last_induction_loop    = digitalRead(pin_induction_loop);
  last_remote_open       = digitalRead(pin_remote_open);
  last_remote_close      = digitalRead(pin_remote_close);
}



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



boolean reconnectMQTT() {
  Serial.println("Connecting MQTT...");
  mqttClient.disconnect();
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(callback);
  mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(40);

  if (mqttClient.connect(mqtt_name, mqtt_user, mqtt_password, topic_device_status, 0, true, "offline")) { // display_name, username, password, LastWill_topic, QoS=0, retain=true, LastWill_message
    Serial.print("MQTT connected to broker at ");
    Serial.println(mqtt_server);

    
    mqttClient.loop();
    //mqttClient.subscribe(topic_relay_close_set);
    //mqttClient.subscribe(topic_relay_open_car_set);
    //mqttClient.subscribe(topic_relay_open_pedestrian_set);

    mqttReportCompleteStatus();
  }
  else {
    Serial.print("error - code 'pubsubmqttClient state' is: ");
    Serial.println(mqttClient.state());
  }
  return mqttClient.connected();
}



void maintainMQTT() {
  mqttClient.loop();
}



void checkAndRepairConnectivity() {
    if (!mqttClient.connected()) {
      Serial.print("Detected MQTT communication failure in main loop. Error code 'pubsubmqttClient state' is: ");
      Serial.println(mqttClient.state());

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



void mqttReportCompleteStatus() {
  mqttClient.publish(topic_device_status, "online");

  // here is 9x same logic, repeated for every input
  input_state = digitalRead(pin_sensor_mailbox);
    if (input_state == HIGH)
      mqttClient.publish(topic_mailbox, "1");
    else
      mqttClient.publish(topic_mailbox, "0");

  input_state = digitalRead(pin_sensor_closed);
    if (input_state == HIGH)
      mqttClient.publish(topic_closed_limit, "1");
    else
      mqttClient.publish(topic_closed_limit, "0");

  input_state = digitalRead(pin_button_car);
    if (input_state == HIGH)
      mqttClient.publish(topic_button_car, "1");
    else
      mqttClient.publish(topic_button_car, "0");

  input_state = digitalRead(pin_button_pedestrian);
    if (input_state == HIGH)
      mqttClient.publish(topic_button_pedestrian, "1");
    else
      mqttClient.publish(topic_button_pedestrian, "0");

  input_state = digitalRead(pin_photocell_outside);
    if (input_state == HIGH)
      mqttClient.publish(topic_photocell_outside, "1");
    else
      mqttClient.publish(topic_photocell_outside, "0");

  input_state = digitalRead(pin_photocell_inside);
    if (input_state == HIGH)
      mqttClient.publish(topic_photocell_inside, "1");
    else
      mqttClient.publish(topic_photocell_inside, "0");

  input_state = digitalRead(pin_induction_loop);
    if (input_state == HIGH)
      mqttClient.publish(topic_induction_loop, "1");
    else
      mqttClient.publish(topic_induction_loop, "0");

  input_state = digitalRead(pin_remote_open);
    if (input_state == HIGH)
      mqttClient.publish(topic_remote_open, "1");
    else
      mqttClient.publish(topic_remote_open, "0");

  input_state = digitalRead(pin_remote_close);
    if (input_state == HIGH)
      mqttClient.publish(topic_remote_close, "1");
    else
      mqttClient.publish(topic_remote_close, "0");
}



void mqttInfoDroppingSwitchCommands() {
  Serial.println("Skipping MQTT command. Already timing previous command of relay impulse. Only 1 relay can be active in same time.");
  //mqttClient.publish(topic_relay_accept_command, "0");
}



void scanInputsForChange() {
  // here is 9x same logic, repeated for every input
  input_state = digitalRead(pin_sensor_mailbox);
  if (input_state != last_sensor_mailbox) {
    last_sensor_mailbox = input_state;

    if (input_state == HIGH) {
      mqttClient.publish(topic_mailbox, "1");
    }
    else {
      mqttClient.publish(topic_mailbox, "0");
    }

    Serial.print("Mailbox: ");
    Serial.println(input_state);
  }


  input_state = digitalRead(pin_sensor_closed);
  if (input_state != last_sensor_closed) {
    last_sensor_closed = input_state;

    if (input_state == HIGH) {
      mqttClient.publish(topic_closed_limit, "1");
    }
    else {
      mqttClient.publish(topic_closed_limit, "0");
    }

    Serial.print("Sensor closed: ");
    Serial.println(input_state);
  }


  input_state = digitalRead(pin_button_car);
  if (input_state != last_button_car) {
    last_button_car = input_state;

    if (input_state == HIGH) {
      mqttClient.publish(topic_button_car, "1");
    }
    else {
      mqttClient.publish(topic_button_car, "0");
    }

    Serial.print("Button open for car: ");
    Serial.println(input_state);
  }


  input_state = digitalRead(pin_button_pedestrian);
  if (input_state != last_button_pedestrian) {
    last_button_pedestrian = input_state;

    if (input_state == HIGH) {
      mqttClient.publish(topic_button_pedestrian, "1");
    }
    else {
      mqttClient.publish(topic_button_pedestrian, "0");
    }

    Serial.print("Button open for pedestrian: ");
    Serial.println(input_state);
  }


  input_state = digitalRead(pin_photocell_outside);
  if (input_state != last_photocell_outside) {
    last_photocell_outside = input_state;

    if (input_state == HIGH) {
      mqttClient.publish(topic_photocell_outside, "1");
    }
    else {
      mqttClient.publish(topic_photocell_outside, "0");
    }

    Serial.print("Photocell outside: ");
    Serial.println(input_state);
  }


  input_state = digitalRead(pin_photocell_inside);
  if (input_state != last_photocell_inside) {
    last_photocell_inside = input_state;

    if (input_state == HIGH) {
      mqttClient.publish(topic_photocell_inside, "1");
    }
    else {
      mqttClient.publish(topic_photocell_inside, "0");
    }

    Serial.print("Photocell inside: ");
    Serial.println(input_state);
  }


  input_state = digitalRead(pin_induction_loop);
  if (input_state != last_induction_loop) {
    last_induction_loop = input_state;

    if (input_state == HIGH) {
      mqttClient.publish(topic_induction_loop, "1");
    }
    else {
      mqttClient.publish(topic_induction_loop, "0");
    }

    Serial.print("Induction loop: ");
    Serial.println(input_state);
  }


  input_state = digitalRead(pin_remote_open);
  if (input_state != last_remote_open) {
    last_remote_open = input_state;

    if (input_state == HIGH) {
      mqttClient.publish(topic_remote_open, "1");
    }
    else {
      mqttClient.publish(topic_remote_open, "0");
    }

    Serial.print("Remote controller open: ");
    Serial.println(input_state);
  }


  input_state = digitalRead(pin_remote_close);
  if (input_state != last_remote_close) {
    last_remote_close = input_state;

    if (input_state == HIGH) {
      mqttClient.publish(topic_remote_close, "1");
    }
    else {
      mqttClient.publish(topic_remote_close, "0");
    }

    Serial.print("Remote controller close: ");
    Serial.println(input_state);
  }
}






Ticker timer_check_connectivity(checkAndRepairConnectivity, 120000);         // cals function every 120s
Ticker timer_maintain_ethernet(maintainEthernet, 1200000);                   // 20min
Ticker timer_maintain_mqtt(maintainMQTT, 5000);                              // 5s
Ticker timer_scan_inputs_for_change(scanInputsForChange, 300);               // 300ms
Ticker timer_mqtt_report_complete_status(mqttReportCompleteStatus, 180000);  // 180s
Ticker timer_end_relay_impulse(switchRelaysOff, 2000, 1);                     // 2s



void callback(char* topic, byte* payload, unsigned int length) {

}



void switchRelayOn(char* serial_message, int relay_pin) {
  digitalWrite(relay_pin, HIGH);
  timer_end_relay_impulse.start();

  Serial.println(serial_message);
  //mqttClient.publish(topic_relay_accept_command, "1");
}


void switchRelaysOff() {
  digitalWrite(pin_relay_close, LOW);
  digitalWrite(pin_relay_open_car, LOW);
  digitalWrite(pin_relay_open_pedestrian, LOW);

  //mqttClient.publish(topic_relay_accept_command, "0");
}


void setup() {
  Serial.begin(9600);
  Serial.println();
  Serial.println("Starting Arduino gate control over HomeAssistant...");
  setupIoPins();

  reconnectEthernet();
  reconnectMQTT();

  timer_check_connectivity.start();
  timer_maintain_ethernet.start();
  timer_maintain_mqtt.start();
  timer_scan_inputs_for_change.start();
  timer_mqtt_report_complete_status.start();


}



void loop() {
  timer_check_connectivity.update();
  timer_maintain_ethernet.update();
  timer_maintain_mqtt.update();
  timer_scan_inputs_for_change.update();
  timer_mqtt_report_complete_status.update();
  timer_end_relay_impulse.update();

  if (state_of_timer_end_relay_impulse != timer_end_relay_impulse.state()) {
    state_of_timer_end_relay_impulse = timer_end_relay_impulse.state();     // defined this way because I cannot call timer.state() in function callback(). At that part of code timer is not defined yet. And defining timer must be abter timer callback function
    Serial.print("Ticker state: ");
    Serial.println(timer_end_relay_impulse.state());
  }

}
