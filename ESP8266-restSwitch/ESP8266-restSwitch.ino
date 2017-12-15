#include <ESP8266WiFi.h>
#include <WiFiClient.h> 
#include <ESP8266WebServer.h>
#include <aREST.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>

//---------- function declarations for REST and webserver -------------
void handleNotFound();
void handleRoot();
void handleConfig();
void setupAP(const char* ssid_wifi, const char* password_wifi);
void closeConnection();
int switchPower(String command);
//---------- /function declarations -------------

#define MAX_SSID_LENGTH 32
#define MAX_WIFI_PASSWORD_LENGTH 64
#define MAX_STATICIP_LEN 16
#define GPIO_0 0
#define GPIO_2 2
#define WLAN_RECONNECT_TIMEOUT_S 60
// If defined the device will print out debug messages during
// operation.
#define DEBUG true

// Some libs are stuff may need some EEPROM space
// so we can optionally skip some bytes.
const uint8_t webConfigEEPROMStartAddress = 0;

// set the EEPROM data structure
struct EEPROM_storage {
  char ssid[MAX_SSID_LENGTH + 1]; // WIFI ssid + null
  char password[MAX_WIFI_PASSWORD_LENGTH + 1]; // WiFi password,  if empyt use OPEN, else use AUTO (WEP/WPA/WPA2) + null
  char staticIP[MAX_STATICIP_LEN + 1]; // staticIP, if empty use DHCP + null
} storage;
const int EEPROM_storageSize = sizeof(EEPROM_storage);

// Create aREST instance
aREST rest = aREST();

// Create an instance of the server
WiFiServer restServer(80);

// Create an instance of the server
ESP8266WebServer webServer(80);

boolean isInConfigMode = false;
boolean isSsidInEepromSet = false;

// Constants
const char* DEFAULT_SSID = "mySwitch";
const int POWER_CHECK_PIN = 5;
const int POWER_SWITCH_PIN = 2;

/**
 * Checks if we are still connected to the WLAN. If not we will try to reconnect
 * for WLAN_RECONNECT_TIMEOUT_S and if this does not succed we reboot the system
 * so if reconnect during startup does not work we will go into config mode.
 */
void checkConnection() {
  const int DELAY_MS = 500;
  uint8_t i = 0;
  while (WiFi.status() != WL_CONNECTED) {
    if(i * DELAY_MS >= WLAN_RECONNECT_TIMEOUT_S * 1000) {
      // Reboot in order to open config mode.
      // Maybe we need to perform some pin settings here to
      // prevent hanging.
      // see: https://github.com/esp8266/Arduino/issues/1017
      ESP.restart();
    }
    Serial.println("WiFi connection lost.");  
    i++;
    delay(500);
  }
}

/**
 * Sets the device into config mode.
 */
void setupConfigMode() {
  // Start AP and get ready to serve a config web page
  // we leave relay on to indicate config mode.
  WiFi.softAP(DEFAULT_SSID);

  webServer.begin();
}

void setupNormalMode() {
  // We are in normal usage.
  // We turn GPIO_0 to high to turn off the relay.
  digitalWrite(GPIO_0, HIGH);

  /* Explicitly set the ESP8266 to be a WiFi-client, otherwise, it by default,
     would try to act as both a client and an access-point and could cause
     network-issues with your other WiFi-devices on your WiFi-network. */
  WiFi.mode(WIFI_STA);
  WiFi.begin(storage.ssid, storage.password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");  
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
  
  // Set up mDNS responder:
  // - first argument is the domain name, in this example
  //   the fully-qualified domain name is "esp8266.local"
  // - second argument is the IP address to advertise
  //   we send our IP address on the WiFi network
  if (!MDNS.begin("esp8266switch")) {
    Serial.println("Error setting up MDNS responder!");
    while(1) {
      delay(1000);
    }
  }
  //Serial.println(F("mDNS responder started"));
  // Add service to MDNS-SD
  MDNS.addService("http", "tcp", 80);

  // Setup function handler to the rest server.
  rest.function("switch", switchPower);
  
  // Start the TCP/HTTP server
  restServer.begin();
  Serial.println("REST/HTTP server started");
}

void setup()
{
  delay(500);
  Serial.begin(115200);
  Serial.println();

  // We handle our own persistence of SSIDS + password.
  // see: https://arduino-esp8266.readthedocs.io/en/latest/esp8266wifi/generic-class.html#persistent
  WiFi.persistent (false);
  
  // Prepare the special pinout settings.
  pinMode(GPIO_0, OUTPUT);
  digitalWrite(GPIO_0, LOW); // make GPIO0 output low

  // Check the GPIO2 input to see if the push button is pressed connecting
  // it to GPIO0.
  isInConfigMode = (digitalRead(GPIO_2) == LOW);

  // Check if we have a eeprom setting.
  EEPROM.begin(EEPROM_storageSize);

  Serial.print("Reading ");
  Serial.print(byte(EEPROM_storageSize));
  Serial.print(" bytes from EEPROM.");
  
  uint8_t * byteStorageRead = (uint8_t *)&storage;
  for (size_t i = 0; i < EEPROM_storageSize; i++) {
    byteStorageRead[i] = EEPROM.read(webConfigEEPROMStartAddress + i);
  }
  EEPROM.end();

  // Check if configuration present.
  if (*storage.ssid == '\0') {
    isInConfigMode = true;
    isSsidInEepromSet = false;
  } else {
    isInConfigMode = false;
    isSsidInEepromSet = true;
  }

  if(isInConfigMode) {
    setupConfigMode();
  } else {
    // TODO hier checken ob man connecten kann, sonst in den config mode setzen.
    setupNormalMode();
  }
  
  // Basic setup work.
  // 0. Checke ob reset Schalter für 5 Sekunden gedrückt wird.
  // YES: Go to SETUP_WIFI
  // 1. Check if there is an eeprom setting for SSID
  // YES: Try to connect to this Network Goto CONNECT_WIFI
  // NO: set IS_WIFI_SETUP = false Goto SETUP_WIFI

  // CONNECT_WIFI
  // 1. Tries to connect to WIFI a certain times. Does it succeed?
  // YES: Start operation
  // NO: set IS_WIFI_SETUP = true GOTO SETUP_WIFI
  // WENN Connection verloren versuche re-connect für ~ 3 min. Wenn nicht verbunden, reboote.

  // SETUP_WIFI
  // 1. Starte im SoftAP Mode mit spezieller SSID.
  // 2. Zeige HTML Seite mit Dateneingabe für Netzwerk und öffne Port für direkte Konfiguration.
  // 3. Wenn IS_WIFI_SETUP = true dann alle 3 Minuten ein Reboot.
}

void loop()
{
  if(isInConfigMode) {
    // We are in config mode, handle HTTP requests for webpage.
    // TODO
  } else {
    // We are in normal operation.
    // Only check for rest calls.
    checkConnection();
    
    // Handle REST calls
    WiFiClient client = restServer.available();
    if (!client) {
      return;
    }
    while(!client.available()){
      delay(1);
    }
    rest.handle(client);
  }
}

/**
 * Switches the power of the relay. It also arms the 
 */
int switchPower(String command) {
  if(command.toInt() == 1) {
    // Switch on.
    digitalWrite(GPIO_0, HIGH);
  } else {
    // Switch off.
    digitalWrite(GPIO_0, LOW);
  }

  return 1;
}

