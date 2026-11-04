ESP32 MQTT LED example, Arduino ESP32 core 3.3.12, PubSubClient 2.8.
GPIO26 -> resistor/LED -> GND. Reports logical GPIO output, not physical load.

Copy secrets.h.example to secrets.h inside FireflyESP32Led and enter local
Wi-Fi / Firefly LAN / broker credentials. secrets.h is ignored by Git.
No cloud, new broker or automatic Firefly broker activation is required.

arduino-cli compile --fqbn esp32:esp32:esp32 FireflyESP32Led
arduino-cli upload --fqbn esp32:esp32:esp32 --port /dev/ttyUSB0 FireflyESP32Led

Command topic: firefly/esp32/led/set, exact ON or OFF, retain=false.
State topic: firefly/esp32/led/state, exact ON or OFF, retain=false.
State is sent after connection, after an accepted command and every 15 seconds
while connected. Periodic readback never writes GPIO or publishes a command.
Failed state sends are paced at 15 seconds. Reconnection reports current output;
it does not replay a previous command. Existing network-loss fail-safe turns
the LED off; this behavior is unchanged.

Do not retain commands. PubSubClient callback does not expose the retained flag.
Never use a previous broker-retained ON/OFF message as fresh device evidence.
Firefly needs an explicitly configured observation binding for the state topic.
