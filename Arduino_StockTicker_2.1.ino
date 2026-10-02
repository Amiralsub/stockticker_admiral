// ============================================================================
// ESP32 STOCK TICKER - Affichage sur 2 pages (Yahoo Finance + Météo + Mode Éco)
// Matériel : ESP32 + Écran e-Paper 4.2 pouces Waveshare (Rev 2.2)
// Description : Récupère et affiche des cours boursiers, des cryptomonnaies 
//               et la météo avec gestion détaillée des erreurs HTTP et économie d'énergie.
//               Suivi visuel complet de toutes les étapes de démarrage au boot.
// ============================================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <time.h>
#include <esp_bt.h> // Nécessaire pour désactiver le Bluetooth (gain énergétique)

// ============================================================================
// 1. CONFIGURATION GÉNÉRALE & PARAMÈTRES RÉSEAU
// ============================================================================

const char* ssid = "plopplopplop";       // Nom du réseau WiFi
const char* password = "papapapa";     // Mot de passe du réseau WiFi

// Symboles boursiers affichés sur la Page 1 (Indices US/UE, Actions, etc.)
String page1Stocks[] = {"^GSPC", "^STOXX", "^IBEX", "GOOGL", "SAF.PA"};
int page1Count = 5;

// Symboles boursiers affichés sur la Page 2 (Actions diverses + Matières premières)
String page2Stocks[] = {"CEC.PA", "AI.PA", "ORA.PA", "TSLA", "BZ=F"};
int page2StockCount = 5;

// Cryptomonnaies affichées sur la Page 2
String page2Crypto[] = {"BTC-USD"};
int page2CryptoCount = 1;

// Intervalles de temps (en millisecondes)
const long dataRefreshInterval = 900000;  // 15 minutes (rafraîchissement global des API)
const long pageSwapInterval = 60000;      // 60 secondes (alternance Page 1 / Page 2)
const unsigned long yahooDelayMs = 15000; // 15 secondes de temporisation en régime nominal

// ============================================================================
// 2. CONFIGURATION DE L'ÉCRAN E-PAPER (Broches SPI)
// ============================================================================

#define EPD_CS    5
#define EPD_DC    17
#define EPD_RST   16
#define EPD_BUSY  4   

// Initialisation du driver pour l'écran 4.2 pouces noir et blanc
GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> 
  display(GxEPD2_420_GDEY042T81(EPD_CS, EPD_DC, EPD_RST, -1));

// ============================================================================
// 3. STRUCTURES DE DONNÉES ET VARIABLES GLOBALES
// ============================================================================

struct StockData {
  String symbol;
  float price;
  float change;
  float changePercent;
  bool isValid;
  bool isCrypto;
  String currency; // "$" ou "€"
  int failCount;   // Compteur d'échecs consécutifs pour les tentatives
};

StockData page1Data[5];
StockData page2StockData[5];
StockData page2CryptoData[2];

float outdoorTemp = 0.0;
bool weatherValid = false;

// Drapeaux et variables d'état pour la gestion des erreurs
bool wifiErrorState = false;
bool stockErrorState = false;
bool weatherErrorState = false;
int lastStockHttpCode = 200; // Stocke le dernier code HTTP boursier en cas d'erreur

unsigned long lastDataRefresh = 0;
unsigned long lastPageSwap = 0;
int currentPage = 0; // 0 = Page 1, 1 = Page 2

// Variables pour l'heure de la dernière mise à jour globale
unsigned long lastDataRefreshMillis = 0; 
struct tm lastUpdateTm = {0};
bool hasBeenUpdated = false;

// Configuration du serveur de temps (NTP) pour Paris
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;          // UTC+1 (Heure standard)
const int daylightOffset_sec = 3600;     // +1h en été (Heure d'été)

// ============================================================================
// 4. DÉCLARATIONS ANTICIPÉES (PROTOTYPES)
// ============================================================================
void logSerial(String message);
bool checkAndEnsureWiFi();
StockData getStockQuote(String symbol);
StockData getCryptoQuote(String symbol);
void updateWeather();
void updateAllData(bool isInitial = false);
bool isParisOpen();
bool isNewYorkOpen();
bool isRefreshAllowed();
void displayCurrentPage(bool usePartial);
void drawHeader();
void displayPage1(bool usePartial);
void displayPage2(bool usePartial);
void drawStock(StockData stock, int yPos);
void drawCrypto(StockData crypto, int yPos);
void drawFooter(int pageNum);
void showMessage(String title, String message);
void showError(String title, String message);

// Gestion dynamique de la fréquence du processeur (Économie d'énergie)
void setHighCPU() { setCpuFrequencyMhz(240); } // Pleine puissance (WiFi, API, Écran)
void setLowCPU()  { setCpuFrequencyMhz(80); }  // Économie d'énergie le reste du temps

// ============================================================================
// 5. FONCTION DE LOG AVEC HORODATAGE
// ============================================================================

void logSerial(String message) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char timeStr[15];
    strftime(timeStr, sizeof(timeStr), "[%H:%M:%S] ", &timeinfo);
    Serial.print(timeStr);
  } else {
    Serial.print("[--:--:--] ");
  }
  Serial.println(message);
}

// ============================================================================
// 6. INITIALISATION (SETUP)
// ============================================================================

void setup() {
  btStop();       // Désactive le module Bluetooth (inutilisé) pour économiser l'énergie
  setHighCPU();   // Passe à 240 MHz pour un démarrage rapide
  
  Serial.begin(115200);
  delay(1000);
  
  logSerial("=== Démarrage de l'ESP32 Stock Ticker ===");
  
  WiFi.mode(WIFI_STA); // Force explicitement le mode Station pour stabiliser le réseau
  
  display.init(115200);
  display.setRotation(3); // Orientation horizontale de l'écran
  display.setTextColor(GxEPD_BLACK);
  
  // Étape 1 : Démarrage matériel
  showMessage("BOOT [1/4]", "Initialisation materiel");
  
  // Étape 2 : Connexion WiFi
  showMessage("BOOT [2/4] WiFi", "Connexion à : " + String(ssid));
  
  if (checkAndEnsureWiFi()) {
    String ipStr = WiFi.localIP().toString();
    showMessage("BOOT [2/4] WiFi OK", "IP: " + ipStr);
    delay(800);
    
    // Étape 3 : Synchronisation NTP (Heure)
    showMessage("BOOT [3/4] Horloge", "Synchro serveur NTP...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    
    struct tm timeinfo;
    int ntpRetry = 0;
    while (!getLocalTime(&timeinfo) && ntpRetry < 10) {
      delay(500);
      ntpRetry++;
    }
    
    if (ntpRetry < 10) {
      char timeBuf[20];
      strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &timeinfo);
      showMessage("BOOT [3/4] NTP OK", "Heure: " + String(timeBuf));
    } else {
      showMessage("BOOT [3/4] NTP Warning", "Echec heure NTP");
    }
    delay(800);
    
    // Étape 4 : Chargement rapide pas-à-pas des API (isInitial = true)
    updateAllData(true);          
    
    // Affichage final de la première page
    displayCurrentPage(false);  
    
    logSerial("=== Configuration terminée ===");
  } else {
    wifiErrorState = true;
    showError("Erreur WiFi", "Echec connexion reseau");
  }
  
  setLowCPU(); 
}

// ============================================================================
// 7. BOUCLE PRINCIPALE (LOOP)
// ============================================================================

void loop() {
  unsigned long currentTime = millis();
  
  if (WiFi.status() != WL_CONNECTED) {
    setHighCPU();
    wifiErrorState = !checkAndEnsureWiFi();
    setLowCPU();
  } else {
    wifiErrorState = false;
  }
  
  if (isRefreshAllowed()) {
    
    if (currentTime - lastDataRefresh >= dataRefreshInterval) {
      setHighCPU();
      logSerial("\n--- Rafraîchissement planifié des données ---");
      // Régime nominal : temporisation activée (isInitial = false)
      updateAllData(false);
      displayCurrentPage(false); 
      lastDataRefresh = millis();
      lastPageSwap = millis();
      setLowCPU();
    }
    
    if (currentTime - lastPageSwap >= pageSwapInterval) {
      setHighCPU();
      logSerial("\n--- Changement de page d'affichage ---");
      currentPage = (currentPage == 0) ? 1 : 0;  
      lastPageSwap = currentTime;
      displayCurrentPage(true); 
      setLowCPU();
    }
    
  } else {
    lastDataRefresh = currentTime;
    lastPageSwap = currentTime;
  }
  
  delay(1000); 
}

// ============================================================================
// 8. GESTION DU RÉSEAU ET DES HORAIRES DE MARCHÉ
// ============================================================================

bool checkAndEnsureWiFi() {
  wl_status_t status = WiFi.status();
  
  if (status == WL_CONNECTED) {
    wifiErrorState = false;
    return true;
  }

  static unsigned long lastBeginAttempt = 0;
  unsigned long now = millis();

  if (now - lastBeginAttempt < 15000 && lastBeginAttempt != 0) {
    return (WiFi.status() == WL_CONNECTED);
  }

  logSerial("[WIFI] Connexion perdue ou inactive (Statut: " + String(status) + "). Tentative de reconnexion...");
  lastBeginAttempt = now;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }
  
  wifiErrorState = (WiFi.status() != WL_CONNECTED);
  if (!wifiErrorState) {
    logSerial("[WIFI] Reconnexion réussie !");
  }
  
  return !wifiErrorState;
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
  
  int hourNY = timeinfo.tm_hour - 6;
  if (hourNY < 0) hourNY += 24;
  int currentMinutesNY = hourNY * 60 + timeinfo.tm_min;
  return (currentMinutesNY >= (9 * 60 + 30) && currentMinutesNY < (16 * 60));
}

// ============================================================================
// 9. RÉCUPÉRATION DES DONNÉES (APIs HTTP)
// ============================================================================

void updateWeather() {
  logSerial("[MÉTÉO] Récupération de la météo...");
  if (!checkAndEnsureWiFi()) {
    weatherValid = false;
    weatherErrorState = true;
    return;
  }
  
  HTTPClient http;
  http.setTimeout(5000);
  http.begin("https://api.open-meteo.com/v1/forecast?latitude=44.84&longitude=-0.58&current=temperature_2m");
  
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error && !doc["current"]["temperature_2m"].isNull()) {
      outdoorTemp = doc["current"]["temperature_2m"].as<float>();
      weatherValid = true;
      weatherErrorState = false;
      char buf[50];
      sprintf(buf, "[MÉTÉO] Succès : %.1f\xB0C", outdoorTemp);
      logSerial(buf);
    } else {
      weatherValid = false;
      weatherErrorState = true;
      logSerial("[MÉTÉO] Erreur de parsing JSON");
    }
  } else {
    weatherValid = false;
    weatherErrorState = true;
    char buf[60];
    sprintf(buf, "[MÉTÉO] Échec - Erreur HTTP ou Timeout (Code: %d)", httpCode);
    logSerial(buf);
  }
  http.end();
}

void updateAllData(bool isInitial) {
  int failedStocks = 0;
  int totalSteps = page1Count + page2StockCount + page2CryptoCount + 1; // 5 + 5 + 1 + 1 = 12
  int currentStep = 1;

  // Page 1 - Actions / Indices
  for (int i = 0; i < page1Count; i++) {
    if (isInitial) {
      char stepStr[40];
      sprintf(stepStr, "BOOT [4/4] [%d/%d]", currentStep, totalSteps);
      showMessage(stepStr, "Req: " + page1Stocks[i]);
    }
    currentStep++;

    StockData newData = getStockQuote(page1Stocks[i]);
    if (newData.isValid) {
      page1Data[i] = newData;
      page1Data[i].failCount = 0; 
    } else {
      page1Data[i].failCount++;   
      if (page1Data[i].failCount >= 3) {
        page1Data[i].isValid = false; 
      }
      failedStocks++;
    }
    if (!isInitial) delay(yahooDelayMs); // Uniquement en régime nominal
  }

  // Page 2 - Actions / Mat. premières
  for (int i = 0; i < page2StockCount; i++) {
    if (isInitial) {
      char stepStr[40];
      sprintf(stepStr, "BOOT [4/4] [%d/%d]", currentStep, totalSteps);
      showMessage(stepStr, "Req: " + page2Stocks[i]);
    }
    currentStep++;

    StockData newData = getStockQuote(page2Stocks[i]);
    if (newData.isValid) {
      page2StockData[i] = newData;
      page2StockData[i].failCount = 0;
    } else {
      page2StockData[i].failCount++;
      if (page2StockData[i].failCount >= 3) {
        page2StockData[i].isValid = false;
      }
      failedStocks++;
    }
    if (!isInitial) delay(yahooDelayMs);
  }

  // Page 2 - Cryptomonnaies
  for (int i = 0; i < page2CryptoCount; i++) {
    if (isInitial) {
      char stepStr[40];
      sprintf(stepStr, "BOOT [4/4] [%d/%d]", currentStep, totalSteps);
      showMessage(stepStr, "Req Crypto: " + page2Crypto[i]);
    }
    currentStep++;

    StockData newData = getCryptoQuote(page2Crypto[i]);
    if (newData.isValid) {
      page2CryptoData[i] = newData;
      page2CryptoData[i].failCount = 0;
    } else {
      page2CryptoData[i].failCount++;
      if (page2CryptoData[i].failCount >= 3) {
        page2CryptoData[i].isValid = false;
      }
      failedStocks++;
    }
    if (!isInitial) delay(yahooDelayMs);
  }
  
  stockErrorState = (failedStocks > 0);

  // Météo
  if (isInitial) {
    char stepStr[40];
    sprintf(stepStr, "BOOT [4/4] [%d/%d]", currentStep, totalSteps);
    showMessage(stepStr, "Req: Meteo Open-Meteo");
  }
  updateWeather();

  if (getLocalTime(&lastUpdateTm)) {
    lastDataRefreshMillis = millis();
    hasBeenUpdated = true;
  }
}

StockData getStockQuote(String symbol) {
  StockData data;
  data.symbol = symbol;
  data.isValid = false;
  data.isCrypto = false;
  data.failCount = 0;
  
  data.currency = (symbol.endsWith(".PA") || symbol.endsWith(".EX") || symbol == "^STOXX" || symbol == "^IBEX") ? "€" : "$";
  
  if (!checkAndEnsureWiFi()) return data;
  
  HTTPClient http;
  http.setTimeout(6000);
  http.begin("https://query1.finance.yahoo.com/v8/finance/chart/" + symbol + "?interval=1d&range=2d");
  http.setUserAgent("Mozilla/5.0");
  
  int httpCode = http.GET();
  char buf[60];
  sprintf(buf, "[BOURSE] Symbole %s - Code HTTP: %d", symbol.c_str(), httpCode);
  logSerial(buf);
  
  if (httpCode == HTTP_CODE_OK) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      JsonArray result = doc["chart"]["result"];
      if (!result.isNull() && result.size() > 0) {
        JsonObject meta = result[0]["meta"];
        if (!meta.isNull() && !meta["regularMarketPrice"].isNull()) {
          data.price = meta["regularMarketPrice"].as<float>();
          float prevClose = meta["chartPreviousClose"] | meta["previousClose"] | data.price;
          data.change = data.price - prevClose;
          data.changePercent = (prevClose != 0) ? (data.change / prevClose) * 100.0 : 0.0;
          data.isValid = true;
        }
      }
    }
  } else {
    lastStockHttpCode = httpCode;
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

// ============================================================================
// 10. AFFICHAGE GRAPHIQUE SUR L'ÉCRAN E-PAPER
// ============================================================================

void displayCurrentPage(bool usePartial) {
  if (currentPage == 0) displayPage1(usePartial);
  else displayPage2(usePartial);
}

void drawHeader() {
  display.setFont(&FreeSansBold12pt7b);
  display.setCursor(5, 20);
  display.print("STOCK TICKER");
  
  display.setFont(&FreeSans9pt7b);
  display.setCursor(5, 38);
  
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char timeParis[10];
    strftime(timeParis, sizeof(timeParis), "%H:%M", &timeinfo);
    
    int hourNY = timeinfo.tm_hour - 6;
    if (hourNY < 0) hourNY += 24;
    char timeNY[10];
    sprintf(timeNY, "%d:%02d", hourNY, timeinfo.tm_min);
    
    String headerText = "Paris " + String(timeParis) + ":" + (isParisOpen() ? "ouvert" : "ferme") + 
                        " NY " + String(timeNY) + ":" + (isNewYorkOpen() ? "ouvert" : "ferme");
    display.print(headerText);
  } else {
    display.print(String("Paris:") + (isParisOpen() ? "ouvert" : "ferme") + String(" NY:") + (isNewYorkOpen() ? "ouvert" : "ferme"));
  }
  
  display.drawLine(0, 45, 300, 45, GxEPD_BLACK);
}

void displayPage1(bool usePartial) {
  if (usePartial) display.setPartialWindow(0, 0, 300, 400);
  else display.setFullWindow();
  
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawHeader();
    
    int yPos = 75;
    for (int i = 0; i < page1Count; i++) {
      drawStock(page1Data[i], yPos);
      yPos += 60;
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
    drawHeader();
    
    int yPos = 75;
    for (int i = 0; i < page2StockCount; i++) {
      drawStock(page2StockData[i], yPos);
      yPos += 50;
    }
    
    yPos += 5;
    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(5, yPos);
    display.print("CRYPTO WATCH");
    display.drawLine(0, yPos + 5, 300, yPos + 5, GxEPD_BLACK);
    yPos += 25;
    
    for (int i = 0; i < page2CryptoCount; i++) {
      drawCrypto(page2CryptoData[i], yPos);
      yPos += 45;
    }
    drawFooter(2);
  } while (display.nextPage());
}

void drawStock(StockData stock, int yPos) {
  display.setFont(&FreeSansBold18pt7b);
  display.setCursor(5, yPos);
  display.print(stock.symbol);
  
  if (!stock.isValid && stock.failCount >= 3) {
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(140, yPos);
    display.print("Err API Yahoo");
    return;
  }
  
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
  
  if (!crypto.isValid && crypto.failCount >= 3) {
    display.setFont(&FreeSans9pt7b);
    display.setCursor(140, yPos);
    display.print("Err API Crypto");
    return;
  }
  
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
  
  int arrowX = 295 - w - 18;
  if (crypto.changePercent > 0) {
    display.fillTriangle(arrowX, yPos-3, arrowX-5, yPos+3, arrowX+5, yPos+3, GxEPD_BLACK);
  } else if (crypto.changePercent < 0) {
    display.fillTriangle(arrowX-5, yPos-3, arrowX+5, yPos-3, arrowX, yPos+3, GxEPD_BLACK);
  }
}

// Dessine le pied de page sur une seule ligne (y = 378)
void drawFooter(int pageNum) {
  display.drawLine(0, 378, 300, 378, GxEPD_BLACK); // 378 au lieu de 365
  display.setFont();  
  
  int footerY = 393; // 393 au lieu de 378
  
  display.setCursor(5, footerY);
  
  if (wifiErrorState) {
    String wifiErrText = "Err WiFi: ";
    wl_status_t wStatus = WiFi.status();
    switch (wStatus) {
      case WL_NO_SSID_AVAIL:  wifiErrText += "No SSID"; break;
      case WL_CONNECT_FAILED: wifiErrText += "Failed"; break;
      case WL_CONNECTION_LOST:wifiErrText += "Lost"; break;
      case WL_DISCONNECTED:   wifiErrText += "Disconnected"; break;
      case WL_IDLE_STATUS:    wifiErrText += "Idle"; break;
      default:                wifiErrText += "Code " + String(wStatus); break;
    }
    display.print(wifiErrText);
  } else if (stockErrorState) {
    display.print("Err Yahoo: " + String(lastStockHttpCode));
  } else if (hasBeenUpdated) {
    unsigned long elapsedMinutes = (millis() - lastDataRefreshMillis) / 60000;
    if (elapsedMinutes < 1) {
      display.print("Last update: just now");
    } else if (elapsedMinutes < 60) {
      display.print("Last update: " + String(elapsedMinutes) + " min ago");
    } else {
      char updateTimeStr[10];
      strftime(updateTimeStr, sizeof(updateTimeStr), "%H:%M", &lastUpdateTm);
      display.print("Last update: " + String(updateTimeStr));
    }
  } else {
    display.print("Last update: --:--");
  }
  
  // 2. Droite : Température météo (°C) et numéro de page
  String rightStr = "";
  if (weatherErrorState || !weatherValid) {
    rightStr = "Err Meteo Pg." + String(pageNum) + "/2";
  } else {
    rightStr = String((int)round(outdoorTemp)) + "\xB0" + "C  Pg." + String(pageNum) + "/2";
  }
  
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(rightStr, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(295 - w, footerY);
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
    display.setCursor(20, 180);
    display.print("ERREUR :");
    display.setCursor(20, 210);
    display.print(title);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(20, 240);
    display.print(message);
  } while (display.nextPage());
}