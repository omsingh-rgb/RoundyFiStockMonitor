#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>

#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>

#define BUFFERSIZE 50
#define NO_VERT_MARKERS 5
#define NO_HORIZ_MARKERS 5
#define STARTX 58
#define STARTY 179
#define HEIGHT 115
#define WIDTH 156
#define MIN_RES 0


#if defined(DISPLAY_DEV_KIT)
Arduino_GFX *gfx = create_default_Arduino_GFX();
#else /* !defined(DISPLAY_DEV_KIT) */
Arduino_DataBus *bus = new Arduino_ESP8266SPI(2 /* DC */, 15 /* CS */);
Arduino_GFX *gfx = new Arduino_GC9A01(bus, 16 /* RST */, 0 /* rotation */, true /* IPS */);
#endif /* !defined(DISPLAY_DEV_KIT) */

// Configuration for fallback access point 
// if Wi-Fi connection fails.
const char * AP_ssid = "MONITOR";
const char * AP_password = "123456789";
IPAddress AP_IP = IPAddress(192,168,0,1);
IPAddress AP_subnet = IPAddress(255,255,255,0);

// Wi-Fi connection parameters.
// It will be read from the flash during setup.
struct ConfigData  {
  char wifi_ssid[BUFFERSIZE];
  char wifi_password[BUFFERSIZE];
  char stock[BUFFERSIZE]; 
  int timeDelay;
  int noPoints;   
  char cstr_terminator = 0;
};
ConfigData configData;

// Web server for editing configuration.
// 80 is the default http port.
ESP8266WebServer server(80);

typedef struct graph {
  float points[BUFFERSIZE]; 
  int length; 
  float min; 
  float max; 
} Graph_t; 

Graph_t g;
bool found = false;
bool connected = false;

// Init stuff for Stock Scraper
HTTPClient http;
WiFiClientSecure client;

String companyName = String("");
String currency = String(""); 
String open = String(""); 
float price = 0;

float calculateMin(float *ps, int l){
  float min = ps[0];
  for (int i = 0 ; i < l; i ++){
    if (min > ps[i]){
      min = ps[i];
    }
  }
  return min;
}

float calculateMax(float *ps, int l){
  float max = ps[0];
  for (int i = 0 ; i < l; i ++){
    if (max < ps[i]){
      max = ps[i];
    }
  }
  return max;
}

void addPoint(Graph_t *g, float point){
  if (g->length >= configData.noPoints){
    g->length = configData.noPoints;
    for (int i = 0; i < configData.noPoints - 1; i ++){
      g->points[i] = g->points[i + 1];
    }
    g->points[configData.noPoints - 1] = point;
  } else {
    g->length += 1;
    g->points[g->length - 1] = point; 
  }

  g->min = calculateMin(g->points, g->length) - 0.25;
  if (g->min < 0){
    g->min = 0;
  } 

  g->max = calculateMax(g->points, g->length) + 0.25; 
}

void drawPoints(float *points, int length, float min, float max){
  float range = max - min;

  if (range < MIN_RES) {
    min = max - MIN_RES;
    if (min < 0){
      min = 0;
      max = MIN_RES;
    }
    range = max - min; 
  }

  int noPixelsPerHoriz = WIDTH/configData.noPoints; 
  float noPixelsPerDiff = HEIGHT/range; 

  for(int i = 0; i < length; i ++ ){
    float thisx = i * noPixelsPerHoriz + STARTX;
    float thisy = STARTY - (points[i] - min) * noPixelsPerDiff ;
    gfx->fillCircle(thisx, thisy, 2, RED);

    if (i != 0 ){
      float thatx = (i - 1) * noPixelsPerHoriz + STARTX;
      float thaty = STARTY - (points[i - 1] - min) * noPixelsPerDiff ;
      gfx->drawLine(thatx, thaty, thisx, thisy, ORANGE);
    }
  }
}

void drawGrid(float min, float max){

  float range = max - min;

  if (range < MIN_RES) {
    min = max - MIN_RES;
    if (min < 0){
      min = 0;
      max = MIN_RES;
    }
    range = max - min; 
  }

  gfx->drawLine(STARTX, STARTY, STARTX + WIDTH, STARTY, WHITE);
  gfx->drawLine(STARTX, STARTY, STARTX, STARTY - HEIGHT, WHITE);
  gfx->setTextColor(WHITE);


  int vertspace =  HEIGHT / NO_VERT_MARKERS;
  gfx->setTextSize(1);

  for (int i = 0; i < NO_VERT_MARKERS; i ++){
    int thisSpace = vertspace * (i + 1);
    gfx->drawLine(STARTX, STARTY - thisSpace, STARTX - 7, STARTY - thisSpace, WHITE);
    gfx->setCursor(STARTX - 43, STARTY - thisSpace); 
    int val = (int) ((range * ((i + 1.0)/NO_VERT_MARKERS) + min) * 10);
    gfx->println(String(val/10.0)); 
  }

  int horizspace =  WIDTH / NO_HORIZ_MARKERS;

  for (int i = 0; i < NO_HORIZ_MARKERS; i ++){
    int thisSpace = horizspace * (i + 1);
    gfx->drawLine(STARTX + thisSpace, STARTY, STARTX + thisSpace, STARTY + 7, WHITE);
  }

  gfx->setCursor(STARTX - 10, STARTY + 7);
  gfx->println(String((int) min));
}

void setup() {
  Serial.begin(9600);
  Serial.println("Booting...");

  gfx->begin();

  EEPROM.begin(512);
  readconfigData();

  gfx->fillScreen(BLACK);
  gfx->setCursor(20,110); 
  gfx->setTextSize(3);
  gfx->setTextColor(WHITE);
  gfx->println("CONECTING...");      

  ppConfig(configData);

  if (!connectToWiFi()) {
    setUpAccessPoint();
    errorAP();
  } else {
    connected = true;
  }

  setUpWebServer();

  if (connected){
    g.length = 0;
    g.min = 0; 
    g.max = 0;

    client.setInsecure();

    gfx->fillScreen(BLACK);

    pinMode(0, INPUT);
  }
}

void ppConfig(ConfigData data){
  Serial.printf("SSID : %s\n", data.wifi_ssid);
  Serial.printf("Password : %s\n", data.wifi_password);
  Serial.printf("Stock : %s\n", data.stock);
  Serial.printf("Time Delay : %i\n", data.timeDelay);
  Serial.printf("No Points : %i\n", data.noPoints);
}

void errorAP(){
  
  found = false;
  gfx->fillScreen(ORANGE);

  gfx->setTextSize(3);

  gfx->setCursor(20,70); 
  gfx->setTextColor(WHITE);
  gfx->println("CONFIG AT");

  gfx->setTextSize(2);

  gfx->setCursor(20,100);
  gfx->println("SSID: " + String(AP_ssid));

  gfx->setCursor(20,120);
  gfx->println("Pass: " + String(AP_password));

  gfx->setCursor(20,140);
  gfx->println(WiFi.softAPIP());  
}

void errorScreen(int code){
  found = false;
  
  gfx->fillScreen(RED);
  gfx->setTextSize(3);

  gfx->setCursor(20,70); 
  gfx->setTextColor(WHITE);
  gfx->println("ERROR");
  
  gfx->setCursor(20,100); 
  gfx->setTextSize(2);
  gfx->println("CODE: " + String(code));
  
  delay(1000 * configData.timeDelay * 2);
}

void readconfigData() {
  // Read wifi conf from flash
  for (int i=0; i<sizeof(configData); i++) {
    ((char *)(&configData))[i] = char(EEPROM.read(i));
  }
  
  // Make sure that there is a 0 
  // that terminatnes the c string
  // if memory is not initalized yet.
  configData.cstr_terminator = 0;

  if (configData.noPoints < 0) {
    configData.noPoints = 10;
  }

}

void writeconfigData() {
  for (int i=0; i<sizeof(configData); i++) {
    EEPROM.write(i, ((char *)(&configData))[i]);
  }
  EEPROM.commit();
}

bool connectToWiFi() {
  Serial.printf("Connecting to '%s'\n", configData.wifi_ssid);
  Serial.printf("With Password %s\n", configData.wifi_password);

  WiFi.mode(WIFI_STA);
  WiFi.begin(configData.wifi_ssid, configData.wifi_password);

  if (WiFi.waitForConnectResult() == WL_CONNECTED) {
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("Connection Failed!");
    return false;
  }
}

void setUpAccessPoint() {
    Serial.println("Setting up access point.");
    Serial.printf("SSID: %s\n", AP_ssid);
    Serial.printf("Password: %s\n", AP_password);

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(AP_IP, AP_IP, AP_subnet);
    if (WiFi.softAP(AP_ssid, AP_password)) {
      Serial.print("Ready. Access point IP: ");
      Serial.println(WiFi.softAPIP());
    } else {
      Serial.println("Setting up access point failed!");
    }
}

void setUpWebServer() {
  server.on("/", handleWebServerRequest);
  server.begin();
}

void handleWebServerRequest() {
  bool save = false;
  bool reboot = false;

  if (server.hasArg("ssid") && server.hasArg("password")) {
    char buffer[BUFFERSIZE];

    server.arg("ssid").toCharArray(
        buffer,
        sizeof(buffer));

    if (strcmp(buffer, configData.wifi_ssid) != 0){
      strcpy(configData.wifi_ssid, buffer);
      save = true;
      reboot = true;
    }
  }

  if (server.hasArg("password")){
    char buffer[BUFFERSIZE];

    server.arg("password").toCharArray(
        buffer,
        sizeof(buffer));

    if (strcmp(buffer, configData.wifi_password) != 0){
      strcpy(configData.wifi_password, buffer);
      save = true;
      reboot = true;
    }
  }

  if (server.hasArg("stock")) {
      char buffer[BUFFERSIZE];

      server.arg("stock").toCharArray(
        buffer,
        sizeof(buffer));

    if (strcmp(buffer, configData.stock) != 0){
      strcpy(configData.stock, buffer);
      save = true;
      g.length = 0;
    }
  }

  //TODO SOME INPUT VALIDATION

  if (server.hasArg("timeDelay")) {
      char buffer[BUFFERSIZE];
      server.arg("timeDelay").toCharArray(
        buffer,
        sizeof(buffer));
      
      configData.timeDelay = atoi(buffer);
      save = true;
      
  }

  if (server.hasArg("noPoints")) {
      char buffer[BUFFERSIZE];
      server.arg("noPoints").toCharArray(
        buffer,
        sizeof(buffer));
      
      int tmp = atoi(buffer);

      if (tmp != configData.noPoints){
        configData.noPoints = tmp;        
        save = true;
        g.length = 0;
      } 
      
  }

  ppConfig(configData);

  String message = "";
  message += "<!DOCTYPE html>";
  message += "<html>";
  message += "<head>";
  message += "<title>ESP8266 conf</title>";
  message += "</head>";
  message += "<body>";

  if (save) {
    message += "<div><h1>CONFIG Saved!</h1></div>";
    writeconfigData();
  } 

  if (reboot) {
    message += "<div><h1>REBOOTING</h1></div>";
  }

  message += "<h1>Wi-Fi conf</h1>";
  message += "<form action='/' method='POST'>";
  message += "<div>SSID:</div>";
  message += "<div><input type='text' name='ssid' value='" + String(configData.wifi_ssid) + "'/></div>";
  message += "<div>Password:</div>";
  message += "<div><input type='text' name='password' value='" + String(configData.wifi_password) + "'/></div>";
  message += "<div>Stock:</div>";
  message += "<div><input type='text' name='stock' value='" + String(configData.stock) + "'/></div>";
  message += "<div>Delay(Seconds):</div>";
  message += "<div><input type='text' name='timeDelay' value='" + String(configData.timeDelay) + "'/></div>";
  message += "<div>Points Shown:</div>";
  message += "<div><input type='text' name='noPoints' value='" + String(configData.noPoints) + "'/></div>";
  message += "<div><input type='submit' value='Save'/></div>";
  message += "</form>";
  message += "</body>";
  message += "</html>";

  server.send(200, "text/html", message);

  if (save && reboot) {
    Serial.println("Conf saved. Rebooting...");
    delay(1000);
    ESP.restart();
  }
}

void updateStockScreen(){
  gfx->fillScreen(BLACK);  

  drawGrid(g.min, g.max);
  drawPoints(g.points, g.length, g.min, g.max); 

  gfx->setTextSize(2); 
  gfx->setTextColor(WHITE);

  gfx->setCursor(60,20); 
  gfx->println(companyName);

  gfx->setCursor(60,45); 
  gfx->println(currency + " - " + String(price));

  gfx->setCursor(50,200);  
  gfx->print("OPEN - " + String());

  gfx->setTextColor(GREEN);
  gfx->print(open);

  http.begin(client, "https://cloud.iexapis.com/stable/stock/" + String(configData.stock) +  "/quote?token=pk_923055743dde40fe8e5f6bef679182a3");
  int httpCode = http.GET();

  String payload = "{}";

  const size_t capacity = JSON_OBJECT_SIZE(56) + 1230;
  DynamicJsonDocument root(capacity);

  if (httpCode > 0){
    Serial.println(String(httpCode));
    payload = http.getString();

    DeserializationError error = deserializeJson(root, payload);

    // Test if parsing succeeds.
    if (!error) {

      companyName = String(root["companyName"]);
      currency = String(root["currency"]);
      open = String(root["isUSMarketOpen"]);      

      addPoint(&g, root["latestPrice"]);     
      price = root["latestPrice"];
      found = true; 

    } else {
      errorScreen(httpCode);
    }
  } else{
    errorScreen(httpCode);
  }
}

void showInfo(){
  gfx->fillScreen(ORANGE);

  gfx->setTextSize(3);

  gfx->setCursor(20,70); 
  gfx->setTextColor(WHITE);
  gfx->println("Info");
  
  gfx->setTextSize(2);
  gfx->setCursor(20,100);
  gfx->println("SSID: " + String(configData.wifi_ssid));

  gfx->setCursor(20,120);
  gfx->println("Stock: " + String(configData.stock));

  gfx->setCursor(20,140);
  gfx->println(WiFi.localIP());  
}

void loop() {
  server.handleClient();

  if (connected){
    if (digitalRead(0) == LOW){
      showInfo();
    } else {
      updateStockScreen();
    }
    if (found) delay(1000 * configData.timeDelay);
  }
}