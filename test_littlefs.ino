// ============================================
// FICHIER: SmartHome.ino - TEMPS RÉEL + AUTH
// ============================================

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <LittleFS.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <FS.h>

// Configuration WiFi
const char* ssid = "VOTRE_SSID";
const char* password = "VOTRE_MOT_DE_PASSE";

// Configuration DHT11
#define DHTPIN 4
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// Configuration LEDs
#define LED1_PIN 5
#define LED2_PIN 6

// Configuration Alarme
#define ALARM_PIN 7

// Configuration capteur de gaz
#define GAS_SENSOR_PIN 1
#define GAS_SENSOR_DIGITAL_PIN 2

// Serveur Web et WebSocket
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

// Variables pour stocker les données
float temperature = 0.0;
float humidity = 0.0;
float gasLevel = 0.0;
bool gasDetected = false;     
bool led1State = false;
bool led2State = false;
bool alarmState = false;
bool alarmManualOverride = false;  // Variable pour le contrôle manuel
unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL = 500;

const int MAX_GAS_HISTORY = 20; // Historique des mesures de gaz 
float gasHistory[MAX_GAS_HISTORY] = {0};
int gasHistoryIndex = 0;
bool gasHistoryFull = false;

// Authentification des clients WebSocket
const int MAX_WS_CLIENTS = 10; //Maximum 10 clients
struct WSClient {
    bool authenticated;
    char email[100];
} wsClients[MAX_WS_CLIENTS];

// ===== DÉCLARATIONS DES PROTOTYPES =====
void toggleLED1();
void toggleLED2();
void toggleAlarm();
void broadcastSensorData();
void addGasToHistory(float value);
bool authenticateUser(const char* email, const char* password);
bool registerUser(const char* name, const char* email, const char* password);
bool deleteUser(const char* email, const char* password);
bool userExists(const char* email);
String getUserName(const char* email);
String getGasHistoryJSON();
void updateSensors();
void handleWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
void handleRoot();
void handleSensorData();
void handleGasHistory();
void handleLEDControl();
void handleAlarmControl();
void handleCORS();

// --- Fonctions de Gestion des Utilisateurs ---

bool userExists(const char* email) {
    File file = LittleFS.open("/users.json", "r");
    if (!file) return false;

    StaticJsonDocument<2000> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) return false;

    JsonArray users = doc["users"];
    for (JsonObject user : users) {
        if (strcmp(user["email"], email) == 0) {
            return true;
        }
    }
    return false;
}

bool authenticateUser(const char* email, const char* password) {
    File file = LittleFS.open("/users.json", "r");
    if (!file) return false;

    StaticJsonDocument<2000> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) return false;

    JsonArray users = doc["users"];
    for (JsonObject user : users) {
        if (strcmp(user["email"], email) == 0 && strcmp(user["password"], password) == 0) {
            return true;
        }
    }
    return false;
}

bool registerUser(const char* name, const char* email, const char* password) {
    if (userExists(email)) {
        return false;
    }

    File file = LittleFS.open("/users.json", "r");
    StaticJsonDocument<2000> doc;

    if (file) {
        deserializeJson(doc, file);
        file.close();
    } else {
        doc["users"] = JsonArray();
    }

    JsonObject newUser = doc["users"].createNestedObject();
    newUser["id"] = millis();
    newUser["name"] = name;
    newUser["email"] = email;
    newUser["password"] = password;
    newUser["createdAt"] = millis();

    file = LittleFS.open("/users.json", "w");
    serializeJson(doc, file);
    file.close();

    return true;
}

bool deleteUser(const char* email, const char* password) {
    // Vérifier que l'utilisateur existe et que le mot de passe est correct
    if (!authenticateUser(email, password)) {
        Serial.printf(" Suppression: authentification échouée pour %s\n", email);
        return false;
    }

    File file = LittleFS.open("/users.json", "r");
    if (!file) {
        Serial.println(" Erreur: impossible d'ouvrir users.json");
        return false;
    }

    StaticJsonDocument<2000> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        Serial.println(" Erreur: JSON parsing error");
        return false;
    }

    JsonArray users = doc["users"];
    int userIndex = -1;

    // Trouver l'index de l'utilisateur à supprimer
    for (int i = 0; i < users.size(); i++) {
        if (strcmp(users[i]["email"], email) == 0) {
            userIndex = i;
            break;
        }
    }

    if (userIndex == -1) {
        Serial.printf(" Suppression: utilisateur %s non trouvé\n", email);
        return false;
    }

    // Supprimer l'utilisateur du tableau
    users.remove(userIndex);
    Serial.printf(" Suppression: utilisateur %s supprimé du tableau\n", email);

    // Écrire le fichier mis à jour
    file = LittleFS.open("/users.json", "w");
    if (!file) {
        Serial.println(" Erreur: impossible de créer users.json");
        return false;
    }

    serializeJson(doc, file);
    file.close();

    Serial.printf(" Compte supprimé avec succès: %s\n", email);
    return true;
}

String getUserName(const char* email) {
    File file = LittleFS.open("/users.json", "r");
    if (!file) return ""; //Si le fichier n'existe pas ou ne peut s'ouvrir, retourne une chaîne vide

    StaticJsonDocument<2000> doc;
    deserializeJson(doc, file);
    file.close();

    JsonArray users = doc["users"];
    for (JsonObject user : users) {
        if (strcmp(user["email"], email) == 0) {
            return String((const char*)user["name"]);
        }
    }
    return ""; //Retourne une chaîne vide si l'email n'est pas trouvé
}

// --- Fonctions Utilitaires ---

void addGasToHistory(float value) {
    gasHistory[gasHistoryIndex] = value;
    gasHistoryIndex = (gasHistoryIndex + 1) % MAX_GAS_HISTORY; // pour revenir à 0 après la dernière position
    if (gasHistoryIndex == 0) {  // Si l'index revient à 0, cela signifie qu'on a fait un tour complet
        gasHistoryFull = true;
    }
}

String getGasHistoryJSON() {
    StaticJsonDocument<300> doc;
    JsonArray array = doc.createNestedArray("history");

    int startIdx = gasHistoryFull ? gasHistoryIndex : 0;
    int count = gasHistoryFull ? MAX_GAS_HISTORY : gasHistoryIndex;

    for (int i = 0; i < count; i++) {
        int idx = (startIdx + i) % MAX_GAS_HISTORY;
        array.add(round(gasHistory[idx] * 10) / 10.0);
    }

    String response;
    serializeJson(doc, response);
    return response; //  récupérer l'historique 
}

void updateSensors() {
    if (millis() - lastUpdate >= UPDATE_INTERVAL) {
        temperature = dht.readTemperature();
        humidity = dht.readHumidity();

        int rawGasValue = analogRead(GAS_SENSOR_PIN);
        gasLevel = (rawGasValue / 4095.0) * 100.0;

        addGasToHistory(gasLevel);
        // Lecture valeur numérique (pour déclencher l'alarme)
        bool previousGasDetected = gasDetected;  // Sauvegarder l'état précédent
        gasDetected = !digitalRead(GAS_SENSOR_DIGITAL_PIN);
        
        // Gestion de l'alarme
        if (gasDetected && !previousGasDetected) {
            // NOUVELLE DÉTECTION DE GAZ (transition 0→1)
            Serial.println(" NOUVELLE DÉTECTION DE GAZ!");
            
            if (!alarmManualOverride) {
                // Si pas de contrôle manuel, activer l'alarme
                Serial.println(" Alarme activée automatiquement (nouvelle détection)!");
                alarmState = true;
                digitalWrite(ALARM_PIN, HIGH);
            } else {
                // Si contrôle manuel actif, ne pas activer mais signaler
                Serial.println(" Contrôle manuel actif - alarme non activée");
            }
        }
        else if (!gasDetected && previousGasDetected) {
            // GAZ DISPARU (transition 1→0)
            Serial.println(" Gaz disparu");
            
            // Si pas de contrôle manuel, éteindre l'alarme
            if (!alarmManualOverride && alarmState) {
                Serial.println(" Alarme désactivée automatiquement");
                alarmState = false;
                digitalWrite(ALARM_PIN, LOW);
            }
            
            // Réinitialiser le contrôle manuel quand le gaz disparaît
            alarmManualOverride = false;
        }
        else if (gasDetected && previousGasDetected && !alarmManualOverride && !alarmState) {
            // Gaz toujours présent mais alarme éteinte (mode auto) → l'activer
            Serial.println(" Gaz toujours présent - activation alarme");
            alarmState = true;
            digitalWrite(ALARM_PIN, HIGH);
        }

        if (!isnan(temperature) && !isnan(humidity)) {
            Serial.printf("[T:%.1f°C | H:%.1f%% | G:%.1f%% | Gaz:%s | Alarme:%s | Mode:%s]\n", 
                         temperature, humidity, gasLevel, 
                         gasDetected ? "OUI" : "NON",
                         alarmState ? "ON" : "OFF",
                         alarmManualOverride ? "MANUEL" : "AUTO");            
            broadcastSensorData();
        }

        lastUpdate = millis();
    }
}
void toggleLED1() {
    led1State = !led1State;
    digitalWrite(LED1_PIN, led1State ? HIGH : LOW);
    Serial.printf("LED1: %s (Pin %d)\n", led1State ? "ON" : "OFF", LED1_PIN);
}

void toggleLED2() {
    led2State = !led2State;
    digitalWrite(LED2_PIN, led2State ? HIGH : LOW);
    Serial.printf("LED2: %s (Pin %d)\n", led2State ? "ON" : "OFF", LED2_PIN);
}

void toggleAlarm() {
    // Basculer l'état de l'alarme
    alarmState = !alarmState;
    digitalWrite(ALARM_PIN, alarmState ? HIGH : LOW);
    
    if (!alarmState) {
        // Si l'utilisateur désactive l'alarme
        alarmManualOverride = true;  // Activer le contrôle manuel
        Serial.printf("ALARM: DÉSACTIVÉE MANUELLEMENT - Restera éteinte jusqu'à nouvelle détection (Pin %d)\n", ALARM_PIN);
    } 
    else {
        // Si l'utilisateur active l'alarme
        alarmManualOverride = false;  // Retour au mode automatique
        Serial.printf("ALARM: ACTIVÉE MANUELLEMENT - Retour mode automatique (Pin %d)\n", ALARM_PIN);
    }
}
void broadcastSensorData() {
    StaticJsonDocument<300> doc;

    doc["temperature"] = (isnan(temperature)) ? 0 : round(temperature * 10) / 10.0;
    doc["humidity"] = (isnan(humidity)) ? 0 : round(humidity * 10) / 10.0;
    doc["gas"] = round(gasLevel * 10) / 10.0;
    doc["led1"] = led1State;
    doc["led2"] = led2State;
    doc["alarm"] = alarmState;
    doc["alarmManual"] = alarmManualOverride;  // Ajout du mode
    doc["gasDetected"] = gasDetected;  // Ajout de la détection
    doc["status"] = "online";
    doc["timestamp"] = millis();

    String response;
    serializeJson(doc, response); // Convertit l'objet JSON en chaîne de caractères



    webSocket.broadcastTXT(response); // Envoie à tous les clients WebSocket connectés
    Serial.printf(" Broadcast: %s\n", response.c_str()); // Capteurs → Microcontrôleur → JSON → WebSocket → les clients
}

// --- Gestion des événements WebSocket ---
// Gèrer les différents types d'événements WebSocket pour le client "num"
void handleWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            Serial.printf("[%u] Déconnecté\n", num);
            wsClients[num].authenticated = false;
            memset(wsClients[num].email, 0, sizeof(wsClients[num].email));
            break;

        case WStype_CONNECTED: {
            IPAddress ip = webSocket.remoteIP(num);
            Serial.printf("[%u] Connecté à %d.%d.%d.%d\n", num, ip[0], ip[1], ip[2], ip[3]);
            wsClients[num].authenticated = false;
            break;
        }

        case WStype_TEXT: {
            String message = String((char *)payload);
            Serial.printf("[%u] Message: %s\n", num, message.c_str());

            StaticJsonDocument<300> doc;
            DeserializationError error = deserializeJson(doc, message);

            if (error) {
                webSocket.sendTXT(num, "{\"error\":\"JSON parsing error\"}");
                Serial.printf("[%u] Erreur JSON\n", num);
                break;
            }

            String action = doc.containsKey("action") ? String(doc["action"]) : "";
            Serial.printf("[%u] Action: %s\n", num, action.c_str());

            // === LOGIN ===
            if (action == "login") {
                const char* email = doc["email"];
                const char* password = doc["password"];

                Serial.printf("[%u] Login attempt: %s\n", num, email);

                if (authenticateUser(email, password)) {
                    wsClients[num].authenticated = true;
                    strcpy(wsClients[num].email, email);
                    
                    String userName = getUserName(email);
                    String response = "{\"authenticated\":true,\"userName\":\"" + userName + "\"}";
                    webSocket.sendTXT(num, response);
                    
                    broadcastSensorData();
                    Serial.printf("[%u] Authentifié: %s\n", num, email);
                } else {
                    webSocket.sendTXT(num, "{\"authenticated\":false,\"error\":\"Invalid credentials\"}");
                    Serial.printf("[%u] Login échoué\n", num);
                }
                break;
            }

            // === SIGNUP ===
            if (action == "signup") {
                const char* name = doc["name"];
                const char* email = doc["email"];
                const char* password = doc["password"];

                Serial.printf("[%u] Signup: %s (%s)\n", num, email, name);

                if (registerUser(name, email, password)) {
                    webSocket.sendTXT(num, "{\"registered\":true,\"message\":\"Account created\"}");
                    Serial.printf("[%u] Utilisateur créé: %s\n", num, email);
                } else {
                    webSocket.sendTXT(num, "{\"registered\":false,\"error\":\"Email already exists\"}");
                    Serial.printf("[%u] Email déjà existant\n", num);
                }
                break;
            }

            // === DELETE ACCOUNT ===
            if (action == "delete_account") {
                const char* email = doc["email"];
                const char* password = doc["password"];

                Serial.printf("[%u] Delete account attempt: %s\n", num, email);

                if (deleteUser(email, password)) {
                    wsClients[num].authenticated = false;
                    memset(wsClients[num].email, 0, sizeof(wsClients[num].email));
                    
                    webSocket.sendTXT(num, "{\"deleted\":true,\"message\":\"Account deleted successfully\"}");
                    Serial.printf("[%u] Compte supprimé: %s\n", num, email);
                } else {
                    webSocket.sendTXT(num, "{\"deleted\":false,\"error\":\"Invalid credentials or account not found\"}");
                    Serial.printf("[%u] Suppression échouée\n", num);
                }
                break;
            }
            // === CHANGE PASSWORD ===
            if (action == "change_password") {
                const char* email = doc["email"];
                const char* currentPassword = doc["currentPassword"];
                const char* newPassword = doc["newPassword"];
                
                Serial.printf("[%u] Changement mot de passe: %s\n", num, email);
                
                // Ouvrir le fichier users.json
                File file = LittleFS.open("/users.json", "r");
                if (!file) {
                    webSocket.sendTXT(num, "{\"passwordChanged\":false,\"error\":\"Server error: cannot open file\"}");
                    Serial.printf("[%u] Erreur: impossible d'ouvrir users.json\n", num);
                    break;
                }
                
                StaticJsonDocument<2000> doc;
                DeserializationError error = deserializeJson(doc, file);
                file.close();
                
                if (error) {
                    webSocket.sendTXT(num, "{\"passwordChanged\":false,\"error\":\"JSON parsing error\"}");
                    Serial.printf("[%u] Erreur: JSON parsing error\n", num);
                    break;
                }
                
                JsonArray users = doc["users"];
                bool userFound = false;
                bool updateSuccessful = false;
                
                // Chercher l'utilisateur et vérifier l'ancien mot de passe
                for (JsonObject user : users) {
                    if (strcmp(user["email"], email) == 0) {
                        userFound = true;
                        
                        // Vérifier l'ancien mot de passe
                        if (strcmp(user["password"], currentPassword) != 0) {
                            webSocket.sendTXT(num, "{\"passwordChanged\":false,\"error\":\"Current password incorrect\"}");
                            Serial.printf("[%u] Erreur: mot de passe actuel incorrect pour %s\n", num, email);
                            break;
                        }
                        
                        // Mettre à jour le mot de passe
                        user["password"] = newPassword;
                        updateSuccessful = true;
                        
                        // Sauvegarder les modifications
                        file = LittleFS.open("/users.json", "w");
                        if (!file) {
                            webSocket.sendTXT(num, "{\"passwordChanged\":false,\"error\":\"Server error: cannot save file\"}");
                            Serial.printf("[%u] Erreur: impossible d'écrire users.json\n", num);
                            break;
                        }
                        
                        serializeJson(doc, file);
                        file.close();
                        
                        webSocket.sendTXT(num, "{\"passwordChanged\":true,\"message\":\"Password updated successfully\"}");
                        Serial.printf("[%u] Mot de passe mis à jour pour: %s\n", num, email);
                        break;
                    }
                }
                
                if (!userFound) {
                    webSocket.sendTXT(num, "{\"passwordChanged\":false,\"error\":\"User not found\"}");
                    Serial.printf("[%u] Erreur: utilisateur %s non trouvé\n", num, email);
                }
                break;
            }














            
            // === CONTRÔLE (AUTHENTIFICATION REQUISE) ===
            if (!wsClients[num].authenticated) {
                webSocket.sendTXT(num, "{\"error\":\"Unauthorized\"}");
                Serial.printf("[%u] Non authentifié\n", num);
                break;
            }

            // Toggle LED1
            if (action == "toggle_led1") {
                Serial.printf("[%u] LED1 toggle\n", num);
                toggleLED1();
                broadcastSensorData();
            } 
            // Toggle LED2
            else if (action == "toggle_led2") {
                Serial.printf("[%u] LED2 toggle\n", num);
                toggleLED2();
                broadcastSensorData();
            } 
            // Toggle Alarm
            else if (action == "toggle_alarm") {
                Serial.printf("[%u] Alarm toggle\n", num);
                toggleAlarm();
                broadcastSensorData();
            }
            else {
                Serial.printf("[%u] Action inconnue: %s\n", num, action.c_str());
            }
            break;
        }

        default:
            break;
    }
}

// --- Routes du Serveur Web ---

void handleRoot() {
    if (LittleFS.exists("/index.html")) {
        File file = LittleFS.open("/index.html", "r");
        server.streamFile(file, "text/html");
        file.close();
    } else {
        Serial.println(" ERREUR: index.html introuvable!");
        server.send(404, "text/plain", "index.html not found");
    }
}

void handleSensorData() {
    StaticJsonDocument<300> doc;
    doc["temperature"] = (isnan(temperature)) ? 0 : round(temperature * 10) / 10.0;
    doc["humidity"] = (isnan(humidity)) ? 0 : round(humidity * 10) / 10.0;
    doc["gas"] = round(gasLevel * 10) / 10.0;
    doc["gasDetected"] = gasDetected;  // Ajout de la détection numérique
    doc["led1"] = led1State;
    doc["led2"] = led2State;
    doc["alarm"] = alarmState;
    doc["status"] = "online";
    doc["timestamp"] = millis();

    String response;
    serializeJson(doc, response);

    server.sendHeader("Content-Type", "application/json");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", response);
}

void handleGasHistory() {
    server.sendHeader("Content-Type", "application/json");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", getGasHistoryJSON());
}

void handleLEDControl() {
    if (server.hasArg("led")) {
        String ledNum = server.arg("led");
        if (ledNum == "1") {
            toggleLED1();
            server.send(200, "application/json", "{\"led1\":" + String(led1State ? "true" : "false") + "}");
        } else if (ledNum == "2") {
            toggleLED2();
            server.send(200, "application/json", "{\"led2\":" + String(led2State ? "true" : "false") + "}");
        }
    }
}

void handleAlarmControl() {
    toggleAlarm();
    server.send(200, "application/json", "{\"alarm\":" + String(alarmState ? "true" : "false") + "}");
}

void handleCORS() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.send(204);
}

// --- Setup et Loop ---

void setup() {
    // 1. OPTIMISATION CPU : Réduire à 80MHz dès le début
    setCpuFrequencyMhz(80);
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n\n  === Domotia SmartHome === \n");

    // Initialisation des broches
    dht.begin();
    pinMode(LED1_PIN, OUTPUT);
    pinMode(LED2_PIN, OUTPUT);
    pinMode(ALARM_PIN, OUTPUT);
    pinMode(GAS_SENSOR_DIGITAL_PIN, INPUT);  // Broche numérique du capteur
    digitalWrite(LED1_PIN, LOW);
    digitalWrite(LED2_PIN, LOW);
    digitalWrite(ALARM_PIN, LOW);
    
    Serial.println(" Pins initialisées");

    // Connexion WiFi
    Serial.printf(" Connexion WiFi: %s\n", ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n WiFi connecté!");
        Serial.print(" IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\n WiFi: Erreur de connexion");
    }
    WiFi.setSleep(true); 
    Serial.println(" Mode économie d'énergie WiFi activé");
    // Initialisation LittleFS
    if (!LittleFS.begin()) {
        Serial.println(" Erreur LittleFS");
    } else {
        if (!LittleFS.exists("/users.json")) {
            File file = LittleFS.open("/users.json", "w");
            file.print("{\"users\":[]}");
            file.close();
            Serial.println(" users.json créé");
        }
    }

    // Routes du serveur web
    server.on("/", handleRoot);
    server.on("/api/sensor", handleSensorData);
    server.on("/api/led", handleLEDControl);
    server.on("/api/alarm", handleAlarmControl);
    server.on("/api/gas-history", handleGasHistory);
    server.on("/api/sensor", HTTP_OPTIONS, handleCORS);
    server.on("/api/led", HTTP_OPTIONS, handleCORS);
    server.on("/api/alarm", HTTP_OPTIONS, handleCORS);
    server.on("/api/gas-history", HTTP_OPTIONS, handleCORS);

    // Démarrage du serveur web
    server.begin();
    Serial.println(" Serveur web: port 80");

    // Démarrage du serveur WebSocket
    webSocket.begin();
    webSocket.onEvent(handleWebSocketEvent);
    Serial.println(" WebSocket: port 81");
    Serial.print(" ws://");
    Serial.print(WiFi.localIP());
    Serial.println(":81\n");
}

void loop() {
    server.handleClient();
    webSocket.loop();
    updateSensors();
    delay(50);
}
