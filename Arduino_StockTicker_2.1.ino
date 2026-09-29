// ==========================================
// ESP32 STOCK TICKER - Two Page Display (Optimized with JSON Filtering)
// Matériel : ESP32 + Écran e-Paper 4.2 pouces Waveshare (Rev 2.2)
// ==========================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <time.h>

// ==========================================
// 1. CONFIGURATION GÉNÉRALE
// ==========================================

const char* ssid = "ssid";
const char* password = "cle_wifi";

String page1Stocks[] = {"^GSPC", "^STOXX", "^IBEX", "GOOGL", "SAF.PA"};
int page1Count = 5;

String page2Stocks[] = {"CEC.PA", "AI.PA", "ORA.PA", "TSLA", "BZ=F"};
int page2StockCount = 5;

String page2Crypto[] = {"BTC-USD"};
int page2CryptoCount = 1;

const long dataRefreshInterval = 900000;  // 15 minutes
const long pageSwapInterval = 60000;      // 60 secondes

// ==========================================
// 2. CONFIGURATION DE L'ÉCRAN E-PAPER
// ==========================================

#define EPD_CS    5
#define EPD_DC    17
#define EPD_RST   16
#define EPD_BUSY  4   

GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> 
  display(GxEPD2_420_GDEY042T81(EPD_CS, EPD_DC, EPD_RST, -1));

// ==========================================
// 3. STRUCTURES DE DONNÉES ET VARIABLES GLOBALES
// ==========================================

struct StockData {
  String symbol;       
  float price;         
  float change;        
  float changePercent; 
  bool isValid;        
  bool isCrypto;       
  String currency;     
};

StockData page1Data[5];
StockData page2StockData[5];
StockData page2CryptoData[2];

float outdoorTemp = 0.0;
bool weatherValid = false;

unsigned long lastDataRefresh = 0;
unsigned long lastPageSwap = 0;
int currentPage = 0; 

// ==========================================
// 4. CONFIGURATION DU FUSEAU HORAIRE (Paris)
// ==========================================

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;        
const int daylightOffset_sec = 3600;    

// ==========================================
// 5. DÉCLARATIONS ANTICIPÉES
// ==========================================
bool checkAndEnsureWiFi();
StockData getStockQuote(String symbol);
StockData getCryptoQuote(String symbol);
void updateWeather();
void updateAllData();
bool isParisOpen();
bool isNewYorkOpen();
bool isRefreshAllowed();
void displayCurrentPage(bool usePartial);
void displayPage1(bool usePartial);
void displayPage2(bool usePartial);
void drawStock(StockData stock, int yPos);
void drawCrypto(StockData crypto, int yPos);
void drawFooter(int pageNum);
void showMessage(String title, String message);
void showError(String title, String message);

// ==========================================
// 6. INITIALISATION (SETUP)
// ==========================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=== Démarrage de l'ESP32 Stock Ticker ===");
  Serial.printf("[INIT] Heap libre initial : %d octets\n", ESP.getFreeHeap());
  
  display.init(115200);
  display.setRotation(3);  
  display.setTextColor(GxEPD_BLACK);
  
  showMessage("Connexion...", "Veuillez patienter");
  
  if (checkAndEnsureWiFi()) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    
    showMessage("Connecte !", "Recuperation des donnees...");
    delay(2000);
    
    updateAllData();
    displayCurrentPage(false);  
    
    Serial.println("=== Configuration terminée ===\n");
  } else {
    Serial.println("\n[ERREUR CRITIQUE] Échec persistant de la connexion WiFi au démarrage !");
    showError("Erreur WiFi", "Connexion impossible a SSID: " + String(ssid));
  }
}

// ==========================================
// 7. BOUCLE PRINCIPALE (LOOP)
// ==========================================

void loop() {
  unsigned long currentTime = millis();
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n[WIFI LOG] ⚠️ Connexion perdue ! Tentative de reconnexion...");
    if (!checkAndEnsureWiFi()) {
      showError("Erreur WiFi", "Connexion perdue");
      delay(5000); 
      return;
    }
  }
  
  if (isRefreshAllowed()) {
    if (currentTime - lastDataRefresh >= dataRefreshInterval) {
      Serial.println("\n--- Rafraîchissement planifié des données ---");
      updateAllData();
      displayCurrentPage(false);  
    }
    
    if (currentTime - lastPageSwap >= pageSwapInterval) {
      Serial.println("\n--- Changement de page d'affichage ---");
      currentPage = (currentPage == 0) ? 1 : 0;  
      lastPageSwap = currentTime;
      displayCurrentPage(true);  
    }
  } else {
    lastDataRefresh = currentTime;
    lastPageSwap = currentTime;
  }
  
  delay(1000); 
}

// ==========================================
// 8. GESTION DU WIFI ET MARCHÉS
// ==========================================

bool checkAndEnsureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI LOG] ✓ Reconnexion réussie ! IP : " + WiFi.localIP().toString());
    return true;
  }
  return false;
}

bool isRefreshAllowed() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return true;
  int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  return (currentMinutes >= (9 * 60) && currentMinutes <= (22 * 60 + 30));
}

bool isParisOpen() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return false;
  int dayOfWeek = timeinfo.tm_wday;
  if (dayOfWeek == 0 || dayOfWeek == 6) return false;
  int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  return (currentMinutes >= (9 * 60) && currentMinutes < (17 * 60 + 35));
}

bool isNewYorkOpen() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return false;
  int dayOfWeek = timeinfo.tm_wday;
  if (dayOfWeek == 0 || dayOfWeek == 6) return false;
  int hourNY = (timeinfo.tm_hour - 6 + 24) % 24; 
  int currentMinutesNY = hourNY * 60 + timeinfo.tm_min;
  return (currentMinutesNY >= (9 * 60 + 30) && currentMinutesNY < (16 * 60));
}

// ==========================================
// 9. RÉCUPÉRATION DES DONNÉES (API OPTIMISÉE)
// ==========================================

void updateWeather() {
  if (!checkAndEnsureWiFi()) return;
  
  HTTPClient http;
  http.setTimeout(5000);
  http.begin("http://api.open-meteo.com/v1/forecast?latitude=44.84&longitude=-0.58&current=temperature_2m");
  
  if (http.GET() == HTTP_CODE_OK) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getStream())) {
      if (!doc["current"]["temperature_2m"].isNull()) {
        outdoorTemp = doc["current"]["temperature_2m"].as<float>();
        weatherValid = true;
      }
    }
  }
  http.end();
}

void updateAllData() {
  Serial.println("\n--- Début de la mise à jour globale ---");
  Serial.printf("[HEAP] Mémoire libre avant maj : %d octets\n", ESP.getFreeHeap());
  
  for (int i = 0; i < page1Count; i++) {
    page1Data[i] = getStockQuote(page1Stocks[i]);
    delay(1000); 
  }
  
  for (int i = 0; i < page2StockCount; i++) {
    page2StockData[i] = getStockQuote(page2Stocks[i]);
    delay(1000);
  }
  
  for (int i = 0; i < page2CryptoCount; i++) {
    page2CryptoData[i] = getCryptoQuote(page2Crypto[i]);
    delay(1000);
  }
  
  updateWeather();
  Serial.printf("[HEAP] Mémoire libre après maj : %d octets\n", ESP.getFreeHeap());
  Serial.println("--- Fin de la mise à jour globale ---\n");
}

StockData getStockQuote(String symbol) {
  StockData data;
  data.symbol = symbol;
  data.isValid = false;
  data.isCrypto = false;
  data.currency = (symbol.endsWith(".PA") || symbol.endsWith(".EX") || symbol == "^STOXX" || symbol == "^IBEX") ? "€" : "$";
  
  if (!checkAndEnsureWiFi()) return data;
  
  HTTPClient http;
  String url = "https://query1.finance.yahoo.com/v8/finance/chart/" + symbol + "?interval=1d&range=2d";
  
  http.begin(url);
  http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
  http.setTimeout(6000); 
  
  if (http.GET() == HTTP_CODE_OK) {
    // 💡 OPTIMISATION : Utilisation d'un filtre JSON pour ne garder que le bloc "meta"
    // Cela réduit drastiquement la taille du document en mémoire RAM (Heap)
    JsonDocument filter;
    filter["chart"]["result"][0]["meta"]["regularMarketPrice"] = true;
    filter["chart"]["result"][0]["meta"]["chartPreviousClose"] = true;
    filter["chart"]["result"][0]["meta"]["previousClose"] = true;

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    
    if (!error) {
      JsonObject meta = doc["chart"]["result"][0]["meta"];
      if (!meta.isNull() && !meta["regularMarketPrice"].isNull()) {
        float regularMarketPrice = meta["regularMarketPrice"].as<float>();
        float previousClose = meta["chartPreviousClose"] | meta["previousClose"] | regularMarketPrice;
        
        data.price = regularMarketPrice;
        data.change = data.price - previousClose;
        data.changePercent = (previousClose != 0) ? (data.change / previousClose) * 100.0 : 0.0;
        data.isValid = true;
        
        Serial.printf("[API] ✓ %s : Prix=%.2f, Var=%.2f (%.2f%%)\n", symbol.c_str(), data.price, data.change, data.changePercent);
      }
    } else {
      Serial.printf("[API] ❌ Erreur JSON %s : %s\n", symbol.c_str(), error.c_str());
    }
  }
  
  http.end();
  return data;
}

StockData getCryptoQuote(String symbol) {
  StockData data = getStockQuote(symbol);
  data.isCrypto = true;
  data.currency = "$";
  data.symbol = symbol;
  data.symbol.replace("-USD", ""); 
  return data;
}

// ==========================================
// 10. AFFICHAGE GRAPHIQUE (Inchangé)
// ==========================================

void displayCurrentPage(bool usePartial) {
  if (currentPage == 0) displayPage1(usePartial);
  else displayPage2(usePartial);
}

void displayPage1(bool usePartial) {
  if (usePartial) display.setPartialWindow(0, 0, 300, 400);
  else display.setFullWindow();
  
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(5, 20);
    display.print("STOCK TICKER");
    
    display.setFont(&FreeSans9pt7b);
    display.setCursor(5, 38);
    String marketStatus = String("Paris:") + (isParisOpen() ? "ouvert" : "ferme") + String(" NY:") + (isNewYorkOpen() ? "ouvert" : "ferme");
    display.print(marketStatus);
    
    display.drawLine(0, 45, 300, 45, GxEPD_BLACK);
    
    int yPos = 75;
    for (int i = 0; i < page1Count; i++) {
      if (page1Data[i].isValid) {
        drawStock(page1Data[i], yPos);
        yPos += 60;
      }
    }
    drawFooter(1);
  } while (display.nextPage());
}

void displayPage2(bool usePartial) {
  if (usePartial) display.setPartialWindow(0, 0, 300, 400);
  else display.setFullWindow();
  
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(5, 20);
    display.print("STOCK TICKER");
    
    display.setFont(&FreeSans9pt7b);
    display.setCursor(5, 38);
    String marketStatus = String("Paris:") + (isParisOpen() ? "ouvert" : "ferme") + String(" NY:") + (isNewYorkOpen() ? "ouvert" : "ferme");
    display.print(marketStatus);
    
    display.drawLine(0, 45, 300, 45, GxEPD_BLACK);
    
    int yPos = 75;
    for (int i = 0; i < page2StockCount; i++) {
      if (page2StockData[i].isValid) {
        drawStock(page2StockData[i], yPos);
        yPos += 50;
      }
    }
    
    yPos += 5;
    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(5, yPos);
    display.print("CRYPTO WATCH");
    display.drawLine(0, yPos + 5, 300, yPos + 5, GxEPD_BLACK);
    yPos += 25;
    
    for (int i = 0; i < page2CryptoCount; i++) {
      if (page2CryptoData[i].isValid) {
        drawCrypto(page2CryptoData[i], yPos);
        yPos += 45;
      }
    }
    drawFooter(2);
  } while (display.nextPage());
}

void drawStock(StockData stock, int yPos) {
  display.setFont(&FreeSansBold18pt7b);
  display.setCursor(5, yPos);
  display.print(stock.symbol);
  
  String priceStr = String(stock.price, 2) + stock.currency;
  display.setFont(&FreeSansBold12pt7b);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(priceStr, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(295 - w, yPos);
  display.print(priceStr);
  
  yPos += 22;
  String changeStr = (stock.change > 0 ? "+" : "") + String(stock.change, 2) + stock.currency + " (" + (stock.change > 0 ? "+" : "") + String(stock.changePercent, 2) + "%)";
  display.setFont(&FreeSans9pt7b);
  display.getTextBounds(changeStr, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(295 - w, yPos);
  display.print(changeStr);
  
  int arrowX = 295 - w - 18;
  if (stock.changePercent > 0) {
    display.fillTriangle(arrowX, yPos-3, arrowX-5, yPos+3, arrowX+5, yPos+3, GxEPD_BLACK);
  } else if (stock.changePercent < 0) {
    display.fillTriangle(arrowX-5, yPos-3, arrowX+5, yPos-3, arrowX, yPos+3, GxEPD_BLACK);
  }
}

void drawCrypto(StockData crypto, int yPos) {
  display.setFont(&FreeSansBold12pt7b);
  display.setCursor(5, yPos);
  display.print(crypto.symbol);
  
  String priceStr = String(crypto.price, 2) + crypto.currency;
  display.setFont(&FreeSansBold12pt7b);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(priceStr, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(295 - w, yPos);
  display.print(priceStr);
  
  yPos += 18;
  String changeStr = (crypto.change > 0 ? "+" : "") + String(crypto.change, 2) + crypto.currency + " (" + (crypto.change > 0 ? "+" : "") + String(crypto.changePercent, 2) + "%)";
  display.setFont(&FreeSans9pt7b);
  display.getTextBounds(changeStr, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(295 - w, yPos);
  display.print(changeStr);
  
  int arrowString = 295 - w - 18;
  if (crypto.changePercent > 0) {
    display.fillTriangle(arrowString, yPos-3, arrowString-5, yPos+3, arrowString+5, yPos+3, GxEPD_BLACK);
  } else if (crypto.changePercent < 0) {
    display.fillTriangle(arrowString-5, yPos-3, arrowString+5, yPos-3, arrowString, yPos+3, GxEPD_BLACK);
  }
}

void drawFooter(int pageNum) {
  display.drawLine(0, 378, 300, 378, GxEPD_BLACK);
  display.setFont();  
  
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char timeParis[10];
    strftime(timeParis, sizeof(timeParis), "%H:%M", &timeinfo);
    int hourNY = (timeinfo.tm_hour - 6 + 24) % 24;
    char timeNY[10];
    sprintf(timeNY, "%d:%02d", hourNY, timeinfo.tm_min);
    
    display.setCursor(5, 393);
    display.print(String(timeParis) + " Paris (" + timeNY + " NY)");
  }
  
  String rightStr = weatherValid ? String((int)round(outdoorTemp)) + "C  Pg." + String(pageNum) + "/2" : "Pg. " + String(pageNum) + "/2";
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(rightStr, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(295 - w, 393);
  display.print(rightStr);
}

void showMessage(String title, String message) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(20, 180);
    display.print(title);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(20, 210);
    display.print(message);
  } while (display.nextPage());
}

void showError(String title, String message) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(20, 160);
    display.print("ERREUR :");
    display.setCursor(20, 190);
    display.print(title);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(20, 230);
    display.print(message);
  } while (display.nextPage());
}