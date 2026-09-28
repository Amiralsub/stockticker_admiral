// ==========================================
// ESP32 STOCK TICKER - Two Page Display (Yahoo Finance + Weather + Sleep Schedule)
// Matériel : ESP32 + Écran e-Paper 4.2 pouces Waveshare (Rev 2.2)
// Fonctionnalités : Alternance de pages, devises dynamiques (€/$), heures Paris/NY, météo Bordeaux, mode nuit (sommeil).
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

// Identifiants du réseau WiFi
const char* ssid = "plopplopplop";
const char* password = "papapapa";

// Actions et indices de la Page 1 (Yahoo Finance : .PA pour Paris, .EX pour Xetra)
String page1Stocks[] = {"^GSPC", "^STOXX", "^IBEX", "GOOGL", "SAF.PA"};
int page1Count = 5;

// Actions de la Page 2
String page2Stocks[] = {"CEC.PA", "AI.PA", "ORA.PA", "TSLA", "BZ=F"};
int page2StockCount = 5;

// Cryptomonnaies de la Page 2 (Format Yahoo Finance avec suffixe -USD)
String page2Crypto[] = {"BTC-USD"};
int page2CryptoCount = 1;

// Intervalles de temps (en millisecondes)
const long dataRefreshInterval = 900000;  // 15 minutes = récupération des nouvelles données financières/météo
const long pageSwapInterval = 60000;      // 60 secondes = basculement d'affichage entre la page 1 et la page 2

// ==========================================
// 2. CONFIGURATION DE L'ÉCRAN E-PAPER
// ==========================================

#define EPD_CS    5
#define EPD_DC    17
#define EPD_RST   16
#define EPD_BUSY  4   

// Initialisation du contrôleur pour l'écran 4.2 pouces N&B (SSD1683 / GDEY042T81)
GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> 
  display(GxEPD2_420_GDEY042T81(EPD_CS, EPD_DC, EPD_RST, -1));

// ==========================================
// 3. STRUCTURES DE DONNÉES ET VARIABLES GLOBALES
// ==========================================

// Structure pour stocker les informations d'une action ou d'une crypto
struct StockData {
  String symbol;       // Nom du symbole
  float price;         // Prix actuel
  float change;        // Variation en valeur absolue
  float changePercent; // Variation en pourcentage
  bool isValid;        // Indique si les données ont bien été récupérées
  bool isCrypto;       // Indique si c'est une crypto
  String currency;     // Symbole de la devise (€ ou $)
};

// Tableaux de stockage des données pour les différentes pages
StockData page1Data[5];
StockData page2StockData[5];
StockData page2CryptoData[2];

// Variables pour la météo extérieure (Bordeaux)
float outdoorTemp = 0.0;
bool weatherValid = false;

// Chronomètres et index de pagination
unsigned long lastDataRefresh = 0;
unsigned long lastPageSwap = 0;
int currentPage = 0; // 0 pour Page 1, 1 pour Page 2

// ==========================================
// 4. CONFIGURATION DU FUSEAU HORAIRE (Paris)
// ==========================================

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;        // UTC+1 (Heure standard de Paris)
const int daylightOffset_sec = 3600;    // +1h supplémentaire en période d'heure d'été (automatique)

// ==========================================
// 5. DÉCLARATIONS ANTICIPÉES DES FONCTIONS
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
  
  // Initialisation de l'affichage e-Paper
  display.init(115200);
  display.setRotation(3);  // Orientation paysage
  display.setTextColor(GxEPD_BLACK);
  
  // Affichage d'un message d'attente
  showMessage("Connexion...", "Veuillez patienter");
  
  // Connexion initiale au réseau WiFi
  if (checkAndEnsureWiFi()) {
    // Synchronisation de l'heure via le serveur NTP pour Paris
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    
    showMessage("Connecte !", "Recuperation des donnees...");
    delay(2000);
    
    // Premier chargement des données et affichage initial complet
    updateAllData();
    displayCurrentPage(false);  
    
    Serial.println("=== Configuration terminée ===\n");
  } else {
    Serial.println("\n[ERREUR CRITIQUE] Échec persistant de la connexion WiFi au démarrage !");
    Serial.println("[WIFI LOG] Vérifiez que le SSID et le mot de passe sont corrects et que le routeur est à portée.");
    showError("Erreur WiFi", "Connexion impossible a SSID: " + String(ssid));
  }
}

// ==========================================
// 7. BOUCLE PRINCIPALE (LOOP)
// ==========================================

void loop() {
  unsigned long currentTime = millis();
  
  // Vérification continue de l'état du WiFi en arrière-plan
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n[WIFI LOG] ⚠️ Connexion perdue détectée dans la boucle principale ! Tentative de reconnexion immédiate...");
    if (!checkAndEnsureWiFi()) {
      Serial.println("[WIFI LOG] ❌ Échec de la reconnexion automatique. Nouvelle tentative au prochain cycle.");
      showError("Erreur WiFi", "Connexion perdue");
      delay(5000); // Pause avant de retenter pour éviter de saturer
      return;
    }
  }
  
  // Vérifie si l'on se trouve dans la plage horaire autorisée (09h00 - 22h30 heure de Paris)
  if (isRefreshAllowed()) {
    
    // Rafraîchissement des données (Bourse + Météo) toutes les 15 minutes
    if (currentTime - lastDataRefresh >= dataRefreshInterval) {
      Serial.println("\n--- Rafraîchissement planifié des données ---");
      updateAllData();
      displayCurrentPage(false);  // Rafraîchissement complet de l'écran
    }
    
    // Changement de page (Page 1 <-> Page 2) toutes les 60 secondes
    if (currentTime - lastPageSwap >= pageSwapInterval) {
      Serial.println("\n--- Changement de page d'affichage ---");
      currentPage = (currentPage == 0) ? 1 : 0;  
      lastPageSwap = currentTime;
      displayCurrentPage(true);  // Rafraîchissement partiel (plus rapide pour l'e-paper)
    }
    
  } else {
    // Hors des horaires de fonctionnement (la nuit), réinitialisation des compteurs pour relancer proprement à 9h00
    lastDataRefresh = currentTime;
    lastPageSwap = currentTime;
  }
  
  delay(1000); // Petite pause d'une seconde pour ne pas saturer le processeur de l'ESP32
}

// ==========================================
// 8. GESTION DU WIFI, HORAIRES ET MARCHÉS
// ==========================================

// Vérifie l'état du WiFi et tente de se reconnecter si nécessaire avec logs détaillés
bool checkAndEnsureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.println("[WIFI LOG] Connexion WiFi inactive ou perdue. Lancement de la procédure de reconnexion...");
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI LOG] ✓ Reconnexion WiFi réussie avec succès !");
    Serial.print("[WIFI LOG] Adresse IP obtenue : ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("\n[WIFI LOG] ❌ Échec critique : impossible de se reconnecter au point d'accès WiFi.");
    return false;
  }
}

// Détermine si le rafraîchissement est autorisé (entre 09:00 et 22:30 heure de Paris)
bool isRefreshAllowed() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return true; // Par sécurité si le NTP échoue temporairement
  }
  
  int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int startMinutes = 9 * 60;       // 09:00
  int endMinutes = 22 * 60 + 30;   // 22:30
  
  return (currentMinutes >= startMinutes && currentMinutes <= endMinutes);
}

// Vérifie si la bourse de Paris (Euronext) est ouverte (09:00 - 17:35 heure de Paris, du lundi au vendredi)
bool isParisOpen() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return false;
  
  int dayOfWeek = timeinfo.tm_wday; // 0 = Dimanche, 6 = Samedi
  if (dayOfWeek == 0 || dayOfWeek == 6) return false;
  
  int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  return (currentMinutes >= (9 * 60) && currentMinutes < (17 * 60 + 35));
}

// Vérifie si la bourse de New York (NYSE/NASDAQ) est ouverte (09:30 - 16:00 heure de NY, du lundi au vendredi)
bool isNewYorkOpen() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return false;
  
  int dayOfWeek = timeinfo.tm_wday; // 0 = Dimanche, 6 = Samedi
  if (dayOfWeek == 0 || dayOfWeek == 6) return false;
  
  int offsetNY = timeinfo.tm_isdst > 0 ? -6 : -6; 
  int hourNY = timeinfo.tm_hour + offsetNY;
  if (hourNY < 0) hourNY += 24;
  
  int currentMinutesNY = hourNY * 60 + timeinfo.tm_min;
  return (currentMinutesNY >= (9 * 60 + 30) && currentMinutesNY < (16 * 60));
}

// ==========================================
// 9. RÉCUPÉRATION DES DONNÉES (API)
// ==========================================

// Récupère la météo extérieure à Bordeaux via l'API Open-Meteo
void updateWeather() {
  Serial.println("[METEO LOG] Récupération de la météo pour Bordeaux...");
  if (!checkAndEnsureWiFi()) {
    Serial.println("[METEO LOG] ❌ Échec : Pas de connexion WiFi disponible pour la météo.");
    return;
  }
  
  HTTPClient http;
  http.setTimeout(5000);
  http.begin("http://api.open-meteo.com/v1/forecast?latitude=44.84&longitude=-0.58&current=temperature_2m");
  
  int httpCode = http.GET();
  Serial.printf("[METEO LOG] Code de réponse HTTP reçu : %d\n", httpCode);
  
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error && !doc["current"]["temperature_2m"].isNull()) {
      outdoorTemp = doc["current"]["temperature_2m"].as<float>();
      weatherValid = true;
      Serial.printf("[METEO LOG] ✓ Succès - Température Bordeaux : %.1f°C\n", outdoorTemp);
    } else {
      weatherValid = false;
      Serial.println("[METEO LOG] ❌ Échec - Erreur de décodage JSON ou valeur de température manquante.");
      showError("Erreur Météo", "Format JSON invalide");
    }
  } else {
    weatherValid = false;
    Serial.printf("[METEO LOG] ❌ Échec - Erreur HTTP (Code : %d)\n", httpCode);
    showError("Erreur Météo", "HTTP " + String(httpCode));
  }
  http.end();
}

// Lance la mise à jour de toutes les listes de valeurs (actions, cryptos, météo)
void updateAllData() {
  Serial.println("\n--- Début de la mise à jour globale ---");
  Serial.printf("[HEAP] Mémoire libre avant maj : %d octets\n", ESP.getFreeHeap());
  
  bool globalApiError = false;

  for (int i = 0; i < page1Count; i++) {
    page1Data[i] = getStockQuote(page1Stocks[i]);
    if (!page1Data[i].isValid) globalApiError = true;
    delay(1000); 
  }
  
  for (int i = 0; i < page2StockCount; i++) {
    page2StockData[i] = getStockQuote(page2Stocks[i]);
    if (!page2StockData[i].isValid) globalApiError = true;
    delay(1000);
  }
  
  for (int i = 0; i < page2CryptoCount; i++) {
    page2CryptoData[i] = getCryptoQuote(page2Crypto[i]);
    if (!page2CryptoData[i].isValid) globalApiError = true;
    delay(1000);
  }
  
  updateWeather();

  if (globalApiError) {
    Serial.println("[API LOG] ⚠️ Attention : Une ou plusieurs requêtes financières ont échoué lors de la mise à jour.");
  }
  
  Serial.printf("[HEAP] Mémoire libre après maj : %d octets\n", ESP.getFreeHeap());
  Serial.println("--- Fin de la mise à jour globale ---\n");
}

// Interroge l'API Yahoo Finance pour récupérer le cours d'une action de manière sécurisée
StockData getStockQuote(String symbol) {
  StockData data;
  data.symbol = symbol;
  data.isValid = false;
  data.isCrypto = false;
  
  if (symbol.endsWith(".PA") || symbol.endsWith(".EX") || symbol == "^STOXX" || symbol == "^IBEX") {
    data.currency = "€";
  } else {
    data.currency = "$";
  }
  
  if (!checkAndEnsureWiFi()) {
    Serial.printf("[API LOG] ❌ Échec pour %s : WiFi déconnecté au moment de la requête\n", symbol.c_str());
    return data;
  }
  
  Serial.printf("[API LOG] Envoi de la requête Yahoo pour le symbole : %s | Heap libre : %d octets\n", symbol.c_str(), ESP.getFreeHeap());
  
  HTTPClient http;
  String url = "https://query1.finance.yahoo.com/v8/finance/chart/" + symbol + "?interval=1d&range=2d";
  
  http.begin(url);
  http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
  http.setTimeout(6000); 
  
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error) {
      Serial.printf("[API LOG] ❌ Erreur de décodage JSON pour %s : %s\n", symbol.c_str(), error.c_str());
    } else {
      JsonArray result = doc["chart"]["result"];
      if (!result.isNull() && result.size() > 0) {
        JsonObject meta = result[0]["meta"];
        if (!meta.isNull() && !meta["regularMarketPrice"].isNull()) {
          float regularMarketPrice = meta["regularMarketPrice"].as<float>();
          float previousClose = meta["chartPreviousClose"] | meta["previousClose"] | regularMarketPrice;
          
          data.price = regularMarketPrice;
          data.change = data.price - previousClose;
          data.changePercent = (previousClose != 0) ? (data.change / previousClose) * 100.0 : 0.0;
          data.isValid = true;
          
          Serial.printf("[API LOG] ✓ Succès - %s : Prix=%.2f, Variation=%.2f (%+.2f%%)\n", 
                        symbol.c_str(), data.price, data.change, data.changePercent);
        } else {
          Serial.printf("[API LOG] ❌ Échec - Données 'meta' ou 'regularMarketPrice' absentes pour %s\n", symbol.c_str());
        }
      } else {
        Serial.printf("[API LOG] ❌ Échec - Tableau 'result' vide ou invalide pour %s\n", symbol.c_str());
      }
    }
  } else {
    Serial.printf("[API LOG] ❌ Échec - Erreur HTTP %d reçue de Yahoo pour le symbole %s\n", httpCode, symbol.c_str());
  }
  
  http.end();
  return data;
}

// Fonction spécifique pour traiter les cryptomonnaies
StockData getCryptoQuote(String symbol) {
  StockData data = getStockQuote(symbol);
  data.isCrypto = true;
  data.currency = "$";
  data.symbol = symbol;
  data.symbol.replace("-USD", ""); 
  return data;
}

// ==========================================
// 10. AFFICHAGE GRAPHIQUE SUR L'ÉCRAN E-PAPER
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
  
  int arrowX = 295 - w - 18;
  if (crypto.changePercent > 0) {
    display.fillTriangle(arrowX, yPos-3, arrowX-5, yPos+3, arrowX+5, yPos+3, GxEPD_BLACK);
  } else if (crypto.changePercent < 0) {
    display.fillTriangle(arrowX-5, yPos-3, arrowX+5, yPos-3, arrowX, yPos+3, GxEPD_BLACK);
  }
}

void drawFooter(int pageNum) {
  display.drawLine(0, 378, 300, 378, GxEPD_BLACK);
  display.setFont();  
  
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char timeParis[10];
    strftime(timeParis, sizeof(timeParis), "%H:%M", &timeinfo);
    
    int hourNY = timeinfo.tm_hour - 6;
    if (hourNY < 0) hourNY += 24;
    
    char timeNY[10];
    sprintf(timeNY, "%d:%02d", hourNY, timeinfo.tm_min);
    
    display.setCursor(5, 393);
    display.print(timeParis);
    display.print(" Paris (");
    display.print(timeNY);
    display.print(" NY)");
  }
  
  String rightStr = "";
  if (weatherValid) {
    rightStr = String((int)round(outdoorTemp)) + "C  Pg." + String(pageNum) + "/2";
  } else {
    rightStr = "Pg. " + String(pageNum) + "/2";
  }
  
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