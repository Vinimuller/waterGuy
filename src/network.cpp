#include "network.h"
#include "telegram.h"
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>

enum NetState {
  NET_OK,             // last check passed; waiting for next interval
  NET_DISCONNECTING,  // WiFi.disconnect() called; waiting briefly before reconnect
  NET_RECONNECTING,   // WiFi.begin() called; polling status
  NET_TESTING,        // associated again; running upstream test
  NET_COOLDOWN,       // failed; wait a minute then retry
};

static NetState      state            = NET_OK;
static unsigned long stateEntered     = 0;
static unsigned long lastCheck        = 0;
static unsigned long outageStartMills = 0;
static String        netSsid;
static String        netPassword;

const unsigned long CHECK_INTERVAL    = 60000; // 1 min between health checks
const unsigned long DISCONNECT_DELAY  = 1500;  // pause after WiFi.disconnect(true)
const unsigned long RECONNECT_TIMEOUT = 10000; // give up associating after 10s


static void enter(NetState s) {
  state = s;
  stateEntered = millis();
}


// 3s blocking HTTP GET. Called at most once per minute in the steady state
// and once per recovery attempt, so it does not meaningfully impact the
// 10s irrigation reconcile tick.
static bool hasInternet() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(client, "http://clients3.google.com/generate_204");
  int code = http.GET();
  http.end();

  return code == 204;
}


void networkBegin(const String& ssid, const String& password) {
  WiFi.persistent(false);   // don't write credentials to flash on every begin()
  netSsid      = ssid;
  netPassword  = password;
  state        = NET_OK;
  stateEntered = millis();
  lastCheck    = millis();
}


static String formatDuration(unsigned long ms) {
  unsigned long totalSec = ms / 1000;
  unsigned long h = totalSec / 3600;
  unsigned long m = (totalSec % 3600) / 60;
  unsigned long s = totalSec % 60;
  String out = "";
  if (h > 0)            out += String(h) + "h ";
  if (h > 0 || m > 0)   out += String(m) + "m ";
  out += String(s) + "s";
  return out;
}


void networkLoop() {
  unsigned long now = millis();

  switch (state) {
    case NET_OK:
      if (now - lastCheck >= CHECK_INTERVAL) {
        lastCheck = now;
        if (!hasInternet()) {
          Serial.println("Internet check failed; cycling WiFi");
          outageStartMills = now;
          WiFi.disconnect(true);
          enter(NET_DISCONNECTING);
        }
      }
      break;

    case NET_DISCONNECTING:
      if (now - stateEntered >= DISCONNECT_DELAY) {
        Serial.println("Reconnecting WiFi");
        WiFi.mode(WIFI_STA);
        WiFi.begin(netSsid.c_str(), netPassword.c_str());
        enter(NET_RECONNECTING);
      }
      break;

    case NET_RECONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi associated; testing internet");
        enter(NET_TESTING);
      } else if (now - stateEntered >= RECONNECT_TIMEOUT) {
        Serial.println("Reconnect timeout; cooling down");
        enter(NET_COOLDOWN);
      }
      break;

    case NET_TESTING:
      if (hasInternet()) {
        Serial.println("Internet restored");
        telegramSend("🟢 Internet restored after " +
                     formatDuration(now - outageStartMills));
        lastCheck = now;
        enter(NET_OK);
      } else {
        Serial.println("Still no internet; cooling down");
        enter(NET_COOLDOWN);
      }
      break;

    case NET_COOLDOWN:
      if (now - stateEntered >= CHECK_INTERVAL) {
        WiFi.disconnect(true);
        enter(NET_DISCONNECTING);
      }
      break;
  }
}
