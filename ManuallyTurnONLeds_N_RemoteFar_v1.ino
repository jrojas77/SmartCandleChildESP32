#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <Preferences.h>
#include <ESPmDNS.h>

// -----------------------------
// Provisioning and auth defaults
// -----------------------------
static const char* AP_SSID = "CPW1";
static const char* AP_PASS = "12345678";
static const char* MDNS_HOSTNAME = "esp32-candle";
static const char* MDNS_NAME = "esp32-candle.local";
static const char* ADMIN_BOOTSTRAP_USER = "admin";
static const char* ADMIN_BOOTSTRAP_PASSWORD = "admin123";

static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
static const unsigned long API_SESSION_TTL_MS = 30UL * 60UL * 1000UL;

// EEPROM (kept for candle config)
#define EEPROM_SIZE 512
#define CONFIG_START 0

// LED & Sensor Pin Arrays
const int LED_PINS[] = {2, 18, 19, 23};
const int SIGNAL_PINS[] = {4, 5, 21, 22};
const int NUM_LEDS = sizeof(LED_PINS) / sizeof(LED_PINS[0]);

const String SIZE_BY_LED[] = {"S", "M", "S", "M"};

bool debugMode = true;
unsigned long ledTurnDuration = 10000;

// -----------------------------
// Global state
// -----------------------------
WebServer server(80);
Preferences prefs;

String statusRequest = "Idle";
String wifiModeLabel = "NONE";

String cfgSsid = "";
String cfgPass = "";
String cfgStoredIp = "";
String cfgAdminUser = "";
String cfgAdminPassword = "";
String cfgCallbackUrl = "";
bool cfgCredentialsValid = false;
bool cfgConnectNow = false;

String apiSessionToken = "";
unsigned long apiSessionExpiresAt = 0;

bool processingRequest = false;
int targetLedsToTurnOn = 0;
int ledsTurnedOn = 0;
int currentLedIndex = 0;
bool ledCurrentlyOn = false;
unsigned long ledOnStartTime = 0;

bool ledStates[NUM_LEDS] = {false};
unsigned long ledStartTimes[NUM_LEDS] = {0};

int requestedQuantity[3] = {0, 0, 0};

// -----------------------------
// Candle configuration struct
// -----------------------------
struct CandleConfig {
  char type[10];
  int quantity;
  int minutes;
  char range[10];
};

CandleConfig candleConfig[3];

// -----------------------------
// Helpers
// -----------------------------
void debugLog(const String& msg) {
  if (debugMode) Serial.println("[DEBUG] " + msg);
}

void setStatus(const String& s) {
  statusRequest = s;
  if (debugMode) Serial.println("[STATUS] " + s);
}

void countCandlesByState(int& s_on, int& s_off, int& m_on, int& m_off, int& l_on, int& l_off) {
  s_on = s_off = m_on = m_off = l_on = l_off = 0;
  for (int i = 0; i < NUM_LEDS; i++) {
    String type = SIZE_BY_LED[i];
    bool isOn = ledStates[i];
    if (type == "S") {
      if (isOn) s_on++; else s_off++;
    } else if (type == "M") {
      if (isOn) m_on++; else m_off++;
    } else if (type == "L") {
      if (isOn) l_on++; else l_off++;
    }
  }
}

void initDefaultConfig() {
  strcpy(candleConfig[0].type, "Small");
  candleConfig[0].quantity = 5;
  candleConfig[0].minutes = 1;
  strcpy(candleConfig[0].range, "0-4");

  strcpy(candleConfig[1].type, "Medium");
  candleConfig[1].quantity = 5;
  candleConfig[1].minutes = 1;
  strcpy(candleConfig[1].range, "5-9");

  strcpy(candleConfig[2].type, "Large");
  candleConfig[2].quantity = 5;
  candleConfig[2].minutes = 1;
  strcpy(candleConfig[2].range, "10-14");

  EEPROM.put(CONFIG_START, candleConfig);
  EEPROM.commit();
}

void readConfig() {
  EEPROM.get(CONFIG_START, candleConfig);
  if (candleConfig[0].type[0] == '\0' || candleConfig[0].type[0] == (char)0xFF) {
    initDefaultConfig();
    EEPROM.get(CONFIG_START, candleConfig);
  }
}

int getMinutesForType(const char* type) {
  for (int i = 0; i < 3; i++) {
    if (strcmp(candleConfig[i].type, type) == 0) {
      return candleConfig[i].minutes;
    }
  }
  return 1;
}

void loadProvisioningConfig() {
  prefs.begin("provision", false);

  cfgSsid = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");
  cfgStoredIp = prefs.getString("stored_ip", "");
  cfgCallbackUrl = prefs.getString("callback", "");
  cfgCredentialsValid = prefs.getBool("cred_valid", false);
  cfgConnectNow = prefs.getBool("connect_now", false);

  cfgAdminUser = prefs.getString("admin_user", "");
  cfgAdminPassword = prefs.getString("admin_pwd", "");

  if (cfgAdminUser.length() == 0 || cfgAdminPassword.length() == 0) {
    cfgAdminUser = ADMIN_BOOTSTRAP_USER;
    cfgAdminPassword = ADMIN_BOOTSTRAP_PASSWORD;
    prefs.putString("admin_user", cfgAdminUser);
    prefs.putString("admin_pwd", cfgAdminPassword);
    debugLog("Bootstrapped admin credentials");
  }
}

void saveWiFiConfig() {
  prefs.putString("ssid", cfgSsid);
  prefs.putString("pass", cfgPass);
  prefs.putString("stored_ip", cfgStoredIp);
  prefs.putString("callback", cfgCallbackUrl);
  prefs.putBool("cred_valid", cfgCredentialsValid);
  prefs.putBool("connect_now", cfgConnectNow);
}

void clearWiFiProvisioning() {
  cfgSsid = "";
  cfgPass = "";
  cfgStoredIp = "";
  cfgCallbackUrl = "";
  cfgCredentialsValid = false;
  cfgConnectNow = false;
  saveWiFiConfig();
}

void invalidateSession() {
  apiSessionToken = "";
  apiSessionExpiresAt = 0;
}

String getApiKeyFromHeaders() {
  if (server.hasHeader("X-Api-Key")) {
    String key = server.header("X-Api-Key");
    key.trim();
    return key;
  }

  if (server.hasHeader("Authorization")) {
    String auth = server.header("Authorization");
    if (auth.startsWith("Bearer ")) {
      auth = auth.substring(7);
      auth.trim();
      return auth;
    }
  }

  return "";
}

String getSessionTokenFromHeaders() {
  if (server.hasHeader("X-Session-Token")) {
    String token = server.header("X-Session-Token");
    token.trim();
    return token;
  }

  if (server.hasHeader("Authorization")) {
    String auth = server.header("Authorization");
    if (auth.startsWith("Session ")) {
      auth = auth.substring(8);
      auth.trim();
      return auth;
    }
  }

  return "";
}

bool isApiAuthorized() {
  String apiKey = getApiKeyFromHeaders();
  return apiKey.length() > 0 && apiKey == cfgAdminPassword;
}

bool isSessionAuthorized() {
  String token = getSessionTokenFromHeaders();
  if (token.length() == 0 || apiSessionToken.length() == 0) return false;
  if (token != apiSessionToken) return false;
  return (long)(millis() - apiSessionExpiresAt) < 0;
}

String generateSessionToken() {
  uint32_t r = (uint32_t)esp_random();
  uint32_t t = (uint32_t)millis();
  String token = String(t, HEX) + String(r, HEX);
  token.toUpperCase();
  return token;
}

void startApMode() {
  MDNS.end();
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(200);

  wifiModeLabel = "AP";
  debugLog("AP started. IP: " + WiFi.softAPIP().toString());
}

void startMdnsIfConnected() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    debugLog(String("mDNS started: ") + MDNS_NAME);
  } else {
    debugLog("mDNS start failed");
  }
}

void notifyCallbackUrl() {
  if (cfgCallbackUrl.length() == 0) return;
  if (!cfgCallbackUrl.startsWith("http://")) {
    debugLog("Callback URL must start with http://");
    return;
  }

  String url = cfgCallbackUrl.substring(7); // strip http://
  int slash = url.indexOf('/');
  String hostPort = (slash == -1) ? url : url.substring(0, slash);
  String path = (slash == -1) ? "/" : url.substring(slash);

  String host = hostPort;
  int port = 80;
  int colon = hostPort.indexOf(':');
  if (colon != -1) {
    host = hostPort.substring(0, colon);
    port = hostPort.substring(colon + 1).toInt();
  }

  if (host.length() == 0) {
    debugLog("Callback URL missing host");
    return;
  }

  StaticJsonDocument<192> doc;
  doc["ip"] = WiFi.localIP().toString();
  doc["ssid"] = WiFi.SSID();
  doc["mdns"] = MDNS_NAME;

  String body;
  serializeJson(doc, body);

  WiFiClient client;
  if (!client.connect(host.c_str(), port)) {
    debugLog("Callback connection failed");
    return;
  }

  client.print("POST "); client.print(path); client.println(" HTTP/1.1");
  client.print("Host: "); client.println(host);
  client.println("Content-Type: application/json");
  client.print("Content-Length: "); client.println(body.length());
  client.println("Connection: close");
  client.println();
  client.print(body);

  unsigned long start = millis();
  while (client.connected() && millis() - start < 1000) {
    while (client.available()) client.read();
  }
  client.stop();
  debugLog("Callback sent");
}

bool connectToConfiguredWiFi() {
  if (cfgSsid.length() == 0) return false;

  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_STA);

  if (cfgPass.length() == 0) {
    WiFi.begin(cfgSsid.c_str());
  } else {
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  }

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    debugLog("STA connect failed");
    return false;
  }

  IPAddress ip = WiFi.localIP();
  if (ip == IPAddress(0, 0, 0, 0)) {
    debugLog("STA got invalid DHCP IP");
    return false;
  }

  cfgStoredIp = ip.toString();
  saveWiFiConfig();
  wifiModeLabel = "STA";
  startMdnsIfConnected();
  notifyCallbackUrl();
  debugLog("STA connected: " + cfgStoredIp);
  return true;
}

bool validateWiFiCredentials(const String& ssid, const String& pass, String& outIp) {
  WiFi.mode(WIFI_AP_STA);

  if (pass.length() == 0) {
    WiFi.begin(ssid.c_str());
  } else {
    WiFi.begin(ssid.c_str(), pass.c_str());
  }

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(300);
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(false, true);
    return false;
  }

  IPAddress ip = WiFi.localIP();
  if (ip == IPAddress(0, 0, 0, 0)) {
    WiFi.disconnect(false, true);
    return false;
  }

  outIp = ip.toString();
  WiFi.disconnect(false, true);
  return true;
}

void sendJson(int code, const String& json) {
  server.send(code, "application/json", json);
}

void sendUnauthorized() {
  sendJson(401, "{\"error\":\"Unauthorized\"}");
}

// -----------------------------
// Provisioning API handlers
// -----------------------------
void handleApiLogin() {
  if (!server.hasArg("plain")) {
    sendJson(400, "{\"error\":\"Missing body\"}");
    return;
  }

  StaticJsonDocument<192> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    sendJson(400, "{\"error\":\"Invalid JSON\"}");
    return;
  }

  String user = doc["user"] | "";
  String password = doc["password"] | "";
  if (password.length() == 0) {
    password = doc["pwd"] | "";
  }

  bool validUser = user.length() > 0 && user == cfgAdminUser;
  bool validPwd = password.length() > 0 && password == cfgAdminPassword;
  if (!validUser || !validPwd) {
    sendJson(401, "{\"error\":\"Invalid credentials\"}");
    return;
  }

  apiSessionToken = generateSessionToken();
  apiSessionExpiresAt = millis() + API_SESSION_TTL_MS;

  StaticJsonDocument<192> response;
  response["status"] = "ok";
  response["sessionToken"] = apiSessionToken;
  response["expiresInSec"] = API_SESSION_TTL_MS / 1000UL;

  String payload;
  serializeJson(response, payload);
  sendJson(200, payload);
}

void handleApiStatus() {
  if (!isApiAuthorized()) {
    sendUnauthorized();
    return;
  }

  bool connected = WiFi.status() == WL_CONNECTED;

  StaticJsonDocument<320> doc;
  doc["mode"] = wifiModeLabel;
  doc["contract_version"] = "1.0";
  doc["connected"] = connected;
  doc["ssid"] = connected ? WiFi.SSID() : "";
  doc["ip"] = connected ? WiFi.localIP().toString() : "";
  doc["apIp"] = (wifiModeLabel == "AP") ? WiFi.softAPIP().toString() : "";
  doc["storedIp"] = cfgStoredIp;
  doc["mdns"] = MDNS_NAME;
  doc["credentialsValid"] = cfgCredentialsValid;
  doc["connectNow"] = cfgConnectNow;
  doc["status"] = connected ? (String("Connected to: ") + WiFi.SSID()) : "Not connected";

  String payload;
  serializeJson(doc, payload);
  sendJson(200, payload);
}

void handleApiSetWiFi() {
  if (!isApiAuthorized()) {
    sendUnauthorized();
    return;
  }

  if (!server.hasArg("plain")) {
    sendJson(400, "{\"error\":\"Missing body\"}");
    return;
  }

  StaticJsonDocument<320> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    sendJson(400, "{\"error\":\"Invalid JSON\"}");
    return;
  }

  String newSsid = doc["ssid"] | "";
  String newPass = doc["password"] | "";
  String callback = doc["callback_url"] | "";
  if (callback.length() == 0) {
    callback = doc["callback"] | "";
  }

  if (newSsid.length() == 0) {
    sendJson(400, "{\"error\":\"Missing ssid\"}");
    return;
  }

  String validatedIp;
  bool ok = validateWiFiCredentials(newSsid, newPass, validatedIp);
  if (!ok) {
    cfgCredentialsValid = false;
    cfgConnectNow = false;
    saveWiFiConfig();
    sendJson(200, "{\"status\":\"Invalid\",\"message\":\"Validation failed. Check SSID/password.\"}");
    return;
  }

  cfgSsid = newSsid;
  cfgPass = newPass;
  cfgStoredIp = validatedIp;
  cfgCallbackUrl = callback;
  cfgCredentialsValid = true;
  cfgConnectNow = false;
  saveWiFiConfig();

  String payload = String("{\"status\":\"Validated\",\"storedIp\":\"") + cfgStoredIp +
                   "\",\"message\":\"Rebooting to AP\",\"next\":\"POST /api/connectnow\",\"mdns\":\"" +
                   String(MDNS_NAME) + "\"}";
  sendJson(200, payload);

  invalidateSession();
  delay(200);
  ESP.restart();
}

void handleApiConnectNow() {
  if (!isSessionAuthorized()) {
    sendUnauthorized();
    return;
  }

  if (!cfgCredentialsValid) {
    sendJson(400, "{\"error\":\"Credentials not validated\"}");
    return;
  }

  cfgConnectNow = true;
  saveWiFiConfig();

  sendJson(200, "{\"status\":\"Connecting\"}");
  invalidateSession();
  delay(200);
  ESP.restart();
}

void handleApiResetWiFi() {
  if (!isSessionAuthorized()) {
    sendUnauthorized();
    return;
  }

  clearWiFiProvisioning();
  sendJson(200, "{\"status\":\"Resetting\",\"mode\":\"AP\"}");
  invalidateSession();
  delay(200);
  ESP.restart();
}

void handleApiReconfigure() {
  if (!isSessionAuthorized()) {
    sendUnauthorized();
    return;
  }

  clearWiFiProvisioning();
  sendJson(200, "{\"status\":\"Reconfiguring\",\"message\":\"Clearing credentials and rebooting\"}");
  invalidateSession();
  delay(200);
  ESP.restart();
}

// -----------------------------
// Existing LED API handlers
// -----------------------------
void handleTurnOnLEDs() {
  if (!server.hasArg("plain")) {
    sendJson(400, "{\"error\":\"Missing body\"}");
    return;
  }

  String body = server.arg("plain");
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    sendJson(400, "{\"error\":\"Invalid JSON\"}");
    return;
  }

  int requestedS = doc["Small"]["numberLeds"] | 0;
  int requestedM = doc["Medium"]["numberLeds"] | 0;
  int requestedL = doc["Large"]["numberLeds"] | 0;

  debugLog("Request received: S=" + String(requestedS) + ", M=" + String(requestedM) + ", L=" + String(requestedL));

  requestedQuantity[0] = requestedS;
  requestedQuantity[1] = requestedM;
  requestedQuantity[2] = requestedL;

  ledsTurnedOn = 0;
  currentLedIndex = 0;
  processingRequest = true;

  candleConfig[0].quantity = requestedS;
  candleConfig[1].quantity = requestedM;
  candleConfig[2].quantity = requestedL;

  ledTurnDuration = getMinutesForType("Small") * 30000;

  sendJson(200, "{\"status\":\"Order ready to be Processed\"}");

  setStatus("Ready to process LEDs: S=" + String(requestedS) + ", M=" + String(requestedM) + ", L=" + String(requestedL));
}

void handleAvailableLEDs() {
  StaticJsonDocument<128> doc;
  JsonObject root = doc.createNestedObject("Ledsl");
  root["on"] = ledCurrentlyOn ? 1 : 0;
  root["off"] = ledCurrentlyOn ? 0 : 1;

  String json;
  serializeJson(doc, json);
  sendJson(200, json);
}

void handleGetConfig() {
  StaticJsonDocument<512> doc;
  JsonArray arr = doc.createNestedArray("CandlesPerType");

  for (int i = 0; i < 3; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["type"] = candleConfig[i].type;
    obj["quantity"] = candleConfig[i].quantity;
    obj["minutes"] = candleConfig[i].minutes;
    obj["range"] = candleConfig[i].range;
  }

  String json;
  serializeJson(doc, json);
  sendJson(200, json);
}

void handleSetConfig() {
  if (!server.hasArg("plain")) {
    sendJson(400, "{\"error\":\"Missing body\"}");
    return;
  }

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error) {
    sendJson(400, "{\"error\":\"Invalid JSON\"}");
    return;
  }

  JsonArray arr = doc["CandlesPerType"].as<JsonArray>();
  for (int i = 0; i < 3 && i < (int)arr.size(); i++) {
    JsonObject obj = arr[i];
    strlcpy(candleConfig[i].type, obj["type"] | "", sizeof(candleConfig[i].type));
    candleConfig[i].quantity = obj["quantity"] | 0;
    candleConfig[i].minutes = obj["minutes"] | 0;
    strlcpy(candleConfig[i].range, obj["range"] | "", sizeof(candleConfig[i].range));
  }

  EEPROM.put(CONFIG_START, candleConfig);
  EEPROM.commit();
  sendJson(200, "{\"status\":\"Configuration Updated\"}");
}

void handleStatusRequest() {
  StaticJsonDocument<256> doc;

  doc["status"] = processingRequest
                    ? (statusRequest.startsWith("All LEDs") ? "Request Completed" : "In progress")
                    : "Waiting for new Orders";

  JsonObject numCandles = doc.createNestedObject("numCandles");

  int totalS = 0, onS = 0;
  int totalM = 0, onM = 0;
  int totalL = 0, onL = 0;

  for (int i = 0; i < NUM_LEDS; i++) {
    String type = SIZE_BY_LED[i];
    bool isOn = ledStates[i];

    if (type == "S") {
      totalS++;
      if (isOn) onS++;
    } else if (type == "M") {
      totalM++;
      if (isOn) onM++;
    } else if (type == "L") {
      totalL++;
      if (isOn) onL++;
    }
  }

  numCandles["Small"] = String(onS) + "/" + String(totalS - onS);
  numCandles["Medium"] = String(onM) + "/" + String(totalM - onM);
  numCandles["Large"] = String(onL) + "/" + String(totalL - onL);

  String json;
  serializeJson(doc, json);
  sendJson(200, json);
}

void handleGetAvailableCandle() {
  StaticJsonDocument<256> doc;

  int s_on, s_off, m_on, m_off, l_on, l_off;
  countCandlesByState(s_on, s_off, m_on, m_off, l_on, l_off);

  JsonObject available = doc.createNestedObject("available");
  available["Small"] = String(s_on) + "/" + String(s_off);
  available["Medium"] = String(m_on) + "/" + String(m_off);
  available["Large"] = String(l_on) + "/" + String(l_off);

  String json;
  serializeJson(doc, json);
  sendJson(200, json);
}

// -----------------------------
// Setup and loop
// -----------------------------
void setup() {
  Serial.begin(74880);

  for (int i = 0; i < NUM_LEDS; i++) {
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], LOW);
    pinMode(SIGNAL_PINS[i], INPUT_PULLUP);
  }

  EEPROM.begin(EEPROM_SIZE);
  readConfig();

  loadProvisioningConfig();

  if (cfgCredentialsValid && cfgConnectNow && cfgSsid.length() > 0) {
    if (!connectToConfiguredWiFi()) {
      cfgConnectNow = false;
      saveWiFiConfig();
      startApMode();
    }
  } else {
    startApMode();
  }

  const char* headerKeys[] = {"X-Api-Key", "Authorization", "X-Session-Token"};
  server.collectHeaders(headerKeys, 3);

  server.on("/api/login", HTTP_POST, handleApiLogin);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/wifi", HTTP_POST, handleApiSetWiFi);
  server.on("/api/connectnow", HTTP_POST, handleApiConnectNow);
  server.on("/api/connect-now", HTTP_POST, handleApiConnectNow);
  server.on("/api/resetwifi", HTTP_POST, handleApiResetWiFi);
  server.on("/api/reconfigure", HTTP_POST, handleApiReconfigure);

  server.on("/TurnonLEDs", HTTP_POST, handleTurnOnLEDs);
  server.on("/AvailableLEDs", HTTP_GET, handleAvailableLEDs);
  server.on("/Config", HTTP_GET, handleGetConfig);
  server.on("/Config", HTTP_POST, handleSetConfig);
  server.on("/Statusrequest", HTTP_GET, handleStatusRequest);
  server.on("/GetAvailableCandle", HTTP_GET, handleGetAvailableCandle);

  server.begin();
  debugLog("HTTP server started");
}

void loop() {
  server.handleClient();

  if (!processingRequest) return;

  bool anyActive = false;

  for (int i = 0; i < NUM_LEDS; i++) {
    int pinLED = LED_PINS[i];
    int pinSensor = SIGNAL_PINS[i];
    String ledSize = SIZE_BY_LED[i];

    int remainingQuota = 0;
    if (ledSize == "S") remainingQuota = candleConfig[0].quantity;
    else if (ledSize == "M") remainingQuota = candleConfig[1].quantity;
    else if (ledSize == "L") remainingQuota = candleConfig[2].quantity;

    if (ledStates[i]) {
      if (millis() - ledStartTimes[i] >= getMinutesForType(ledSize.c_str()) * 30000) {
        digitalWrite(pinLED, LOW);
        ledStates[i] = false;
        ledsTurnedOn++;

        if (ledSize == "S") candleConfig[0].quantity--;
        else if (ledSize == "M") candleConfig[1].quantity--;
        else if (ledSize == "L") candleConfig[2].quantity--;

        debugLog("LED [" + ledSize + "] at pin " + String(pinLED) + " turned OFF");
      } else {
        anyActive = true;
      }
    }

    if (!ledStates[i] && remainingQuota > 0 && digitalRead(pinSensor) == LOW) {
      digitalWrite(pinLED, HIGH);
      ledStates[i] = true;
      ledStartTimes[i] = millis();

      debugLog("Signal detected on pin " + String(pinSensor));
      debugLog("LED [" + ledSize + "] turned ON at pin " + String(pinLED));

      setStatus("LED ON [" + ledSize + "] - Total ON: " + String(ledsTurnedOn + 1));
    }
  }

  if (candleConfig[0].quantity <= 0 &&
      candleConfig[1].quantity <= 0 &&
      candleConfig[2].quantity <= 0 &&
      !anyActive) {
    processingRequest = false;
    setStatus("All LEDs processed");
  }
}
