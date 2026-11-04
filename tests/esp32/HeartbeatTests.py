"""Execute the actual example sketch with fake GPIO/network; no USB or secrets."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="firefly-esp32-heartbeat-") as temporary:
    work = Path(temporary)
    (work / "Arduino.h").write_text('''#pragma once
#include <cstdint>
#include <vector>
using byte = unsigned char;
constexpr int HIGH=1, LOW=0, OUTPUT=1;
inline unsigned long instant=1;
inline int gpio=0, writes=0;
inline unsigned long millis(){return instant;}
inline void digitalWrite(int,int v){gpio=v;++writes;}
inline int digitalRead(int){return gpio;}
inline void pinMode(int,int){}
inline void delay(int){}
struct SerialType {void begin(int){};void println(const char*){};void println(int){};void print(const char*){};template<class... A>void printf(const char*,A...){};};
inline SerialType Serial;
struct Esp {unsigned long long getEfuseMac(){return 1;}};
inline Esp ESP;
''')
    (work / "WiFi.h").write_text('''#pragma once
constexpr int WIFI_STA=1, WL_CONNECTED=3;
struct WiFiClient {};
struct Wifi {int connection=WL_CONNECTED;int status(){return connection;}void mode(int){};void setAutoReconnect(bool){};void begin(const char*,const char*){};int localIP(){return 0;}};
inline Wifi WiFi;
''')
    (work / "PubSubClient.h").write_text('''#pragma once
#include <string>
#include <vector>
struct Packet {std::string topic,payload;bool retain;};
struct PubSubClient {
 bool live=false, accepts=true;std::vector<Packet> packets;
 PubSubClient(WiFiClient&){};bool connected(){return live;}
 bool connect(const char*,const char*,const char*){live=true;return true;}
 bool subscribe(const char*,int){return true;}void disconnect(){live=false;}
 void setServer(const char*,int){};template<class T>void setCallback(T){};
 void setKeepAlive(int){};void setSocketTimeout(int){};int state(){return 0;};void loop(){}
 bool publish(const char*t,const char*p,bool r){packets.push_back({t,p,r});return accepts;}
};
''')
    (work / "secrets.h").write_text('#define WIFI_SSID "test"\n#define WIFI_PASSWORD "test"\n#define MQTT_HOST "127.0.0.1"\n#define MQTT_PORT 1883\n#define MQTT_USERNAME "test"\n#define MQTT_PASSWORD "test"\n')
    (work / "test.cpp").write_text('''#include "FireflyESP32Led.ino"
#include <cassert>
#include <limits>
int main(){
 setup();loop();assert(mqtt.packets.size()==1 && mqtt.packets.back().payload=="OFF");
 const int startWrites=writes;
 instant+=14999;loop();assert(mqtt.packets.size()==1);
 ++instant;loop();assert(mqtt.packets.size()==2 && writes==startWrites);
 byte on[]={'O','N'};onMessage(const_cast<char*>(TOPIC_SET),on,2);
 assert(mqtt.packets.size()==3 && gpio==HIGH && mqtt.packets.back().payload=="ON");
 const int onWrites=writes;
 instant+=15000;loop();assert(mqtt.packets.size()==4 && writes==onWrites && gpio==HIGH);
 byte invalid[]={'O','N','!'};onMessage(const_cast<char*>(TOPIC_SET),invalid,3);
 assert(mqtt.packets.size()==4 && writes==onWrites);
 mqtt.accepts=false;instant+=15000;loop();const auto failed=mqtt.packets.size();
 for(int i=0;i<1000;++i)loop();assert(mqtt.packets.size()==failed);
 mqtt.accepts=true;instant=std::numeric_limits<unsigned long>::max()-100;
 publishCurrentState();instant=14900;loop();assert(mqtt.packets.size()==failed+2);
 for(const auto&p:mqtt.packets)assert(p.topic==TOPIC_STATE && !p.retain);
 mqtt.disconnect();loop();assert(mqtt.packets.back().payload=="OFF");
}
''')
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-I", str(work), "-I",
                    str(root / "examples/esp32-mqtt-led/FireflyESP32Led"), str(work / "test.cpp"),
                    "-o", str(work / "test")], check=True, timeout=30)
    subprocess.run([str(work / "test")], check=True, timeout=5)
print("PASS actual ESP32 sketch: periodic state without GPIO writes, command control, no retained/command publish, failed-send pacing, rollover/reconnect")
