#include <DHT.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"

// ── Pinos ──────────────────────────────────────────────────────
#define DHT_PIN        32
#define DHT_TYPE       DHT11

// Pinos do módulo microSD (mesma fiação do ESP32-microSD-Card-Wiring-Diagram.png)
#define SD_CS          5
#define SD_SCK         18
#define SD_MISO        19
#define SD_MOSI        23
#define LOG_FILE       "/datalog.txt"
#define LOG_TMP_FILE   "/datalog_tmp.txt"

// ── Hotspot de configuração ────────────────────────────────────
#define AP_SSID "Colmeia-Setup"
#define AP_PASS ""               // aberto — sem senha para facilitar acesso

// Intervalos
#define SENSOR_INTERVAL_MS      2000    // frequência de leitura do DHT
#define WIFI_RETRY_INTERVAL_MS  30000   // intervalo entre tentativas de reconexão

// ── HiveMQ Cloud ───────────────────────────────────────────────
const char* mqttHost = "2cd4e9f8396443f9bf9c16820fac480f.s1.eu.hivemq.cloud";
const int   mqttPort = 8883;
const char* mqttUser = "rustServer";
const char* mqttPass = "Petterson67";
const char* topic    = "sensors/leitura";

// ── Objetos ────────────────────────────────────────────────────
DHT dht(DHT_PIN, DHT_TYPE);
WiFiClientSecure espClient;
PubSubClient client(espClient);
WebServer server(80);
DNSServer dnsServer;          // responde QUALQUER domínio com o IP do ESP32
Preferences prefs;

// ── Estado ─────────────────────────────────────────────────────
bool sdReady = false;
bool hasCredentials = false;  // true se já existe SSID salvo no NVS
bool apMode = false;          // true enquanto o portal de configuração está ativo em paralelo
bool pendingSDData = false;   // true se existem leituras no SD ainda não enviadas ao servidor
unsigned long lastWifiAttempt = 0;
unsigned long lastSensorRead = 0;

// ─────────────────────────────────────────────────────────────
//  Páginas HTML do portal
// ─────────────────────────────────────────────────────────────
const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Colmeia — Configurar WiFi</title>
  <style>
    body { font-family: sans-serif; max-width: 360px; margin: 60px auto; padding: 0 16px; }
    h2   { color: #f0a500; }
    input { width: 100%; padding: 8px; margin: 6px 0 16px; box-sizing: border-box; border: 1px solid #ccc; border-radius: 4px; }
    button { background: #f0a500; color: #fff; border: none; padding: 10px 24px; border-radius: 4px; cursor: pointer; font-size: 1rem; }
  </style>
</head>
<body>
  <h2> BeeDuino</h2>
  <p>Conecte o ESP32 à sua rede WiFi.</p>
  <p style="color:#666;font-size:0.9rem;">O sensor já está gravando leituras no cartão SD mesmo sem WiFi configurado.</p>
  <form action="/save" method="POST">
    <label>Nome da rede (SSID)</label>
    <input name="ssid" type="text" placeholder="MinhaRede" required>
    <label>Senha</label>
    <input name="pass" type="password" placeholder="••••••••">
    <button type="submit">Salvar e conectar</button>
  </form>
</body>
</html>
)rawliteral";

const char SAVED_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8">
  <title>Salvo!</title>
  <style>body{font-family:sans-serif;text-align:center;margin-top:80px;}</style>
</head>
<body>
  <h2>Credenciais salvas!</h2>
  <p>O ESP32 vai reiniciar e se conectar à rede.</p>
  <p>Você pode fechar esta página.</p>
</body>
</html>
)rawliteral";

// ─────────────────────────────────────────────────────────────
//  Rotas do WebServer
// ─────────────────────────────────────────────────────────────
void setupRoutes() {
  // Formulário principal
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", PORTAL_HTML);
  });

  // Salva credenciais no NVS e reinicia
  server.on("/save", HTTP_POST, []() {
    String newSsid = server.arg("ssid");
    String newPass = server.arg("pass");

    if (newSsid.length() == 0) {
      server.send(400, "text/plain", "SSID nao pode ser vazio.");
      return;
    }

    prefs.begin("wifi", false);
    prefs.putString("ssid", newSsid);
    prefs.putString("pass", newPass);
    prefs.end();

    server.send_P(200, "text/html", SAVED_HTML);
    Serial.println("[WiFi] Credenciais salvas. Reiniciando...");
    delay(1500);
    ESP.restart();
  });

  // ── Captive-portal hooks ───────────────────────────────────
  auto redirect = []() {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString(), true);
    server.send(302, "text/plain", "");
  };

  // Android / ChromeOS
  server.on("/generate_204",          HTTP_GET, redirect);
  server.on("/gen_204",               HTTP_GET, redirect);
  // Windows / Edge
  server.on("/ncsi.txt",              HTTP_GET, redirect);
  server.on("/connecttest.txt",       HTTP_GET, redirect);
  server.on("/redirect",              HTTP_GET, redirect);
  // Apple / iOS / macOS
  server.on("/hotspot-detect.html",   HTTP_GET, redirect);
  server.on("/library/test/success.html", HTTP_GET, redirect);
  server.on("/success.txt",           HTTP_GET, redirect);
  // Qualquer outra URL não mapeada
  server.onNotFound(redirect);
}

// ─────────────────────────────────────────────────────────────
//  Portal de configuração — roda em SEGUNDO PLANO (não bloqueia)
//  para que a leitura do sensor e o log no SD continuem normalmente
//  mesmo enquanto o WiFi ainda não foi configurado.
// ─────────────────────────────────────────────────────────────
void startConfigPortalBackground() {
  Serial.println("[WiFi] Sem credenciais salvas — portal de configuração ativo em paralelo.");
  Serial.println("[WiFi] O sensor continua lendo e gravando no SD normalmente.");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("[WiFi] Hotspot: \"%s\"  IP: %s\n", AP_SSID, apIP.toString().c_str());

  dnsServer.start(53, "*", apIP);
  setupRoutes();
  server.begin();

  apMode = true;
}

// ─────────────────────────────────────────────────────────────
//  Conexão WiFi com credenciais salvas no NVS
//
//  Se não houver credenciais salvas, NÃO bloqueia: abre o portal em
//  segundo plano e deixa o setup() seguir para a leitura/log no SD.
//  Se houver credenciais mas a rede estiver fora do ar, também segue
//  em frente offline — o loop() tenta reconectar periodicamente.
// ─────────────────────────────────────────────────────────────
void connectWiFi() {
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();

  if (ssid.length() == 0) {
    hasCredentials = false;
    startConfigPortalBackground();
    return;
  }

  hasCredentials = true;
  Serial.printf("[WiFi] Conectando a \"%s\"", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Conectado: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WiFi] Rede indisponível no momento — seguindo offline (log no SD).");
  }
}

// ─────────────────────────────────────────────────────────────
//  Cartão SD
// ─────────────────────────────────────────────────────────────
bool setupSD() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS)) {
    Serial.println("[SD] Falha ao montar o cartão SD.");
    return false;
  }
  if (SD.cardType() == CARD_NONE) {
    Serial.println("[SD] Nenhum cartão SD encontrado.");
    return false;
  }

  if (!SD.exists(LOG_FILE)) {
    File f = SD.open(LOG_FILE, FILE_WRITE);
    if (f) {
      f.println("millis_ms,temp_c,umidade_pct");
      f.close();
    }
    pendingSDData = false;
  } else {
    // Verifica se já existem leituras pendentes de um boot anterior
    File f = SD.open(LOG_FILE, FILE_READ);
    if (f) {
      f.readStringUntil('\n'); // pula o cabeçalho
      pendingSDData = f.available();
      f.close();
      if (pendingSDData) {
        Serial.println("[SD] Há leituras pendentes de um boot anterior — serão enviadas assim que houver conexão.");
      }
    }
  }

  Serial.println("[SD] Cartão montado com sucesso.");
  return true;
}

void logToSD(float t, float h) {
  if (!sdReady) {
    Serial.println("[SD] Log ignorado — cartão indisponível.");
    return;
  }

  File f = SD.open(LOG_FILE, FILE_APPEND);
  if (!f) {
    Serial.println("[SD] Falha ao abrir arquivo de log.");
    return;
  }

  f.printf("%lu,%.1f,%.1f\n", millis(), t, h);
  f.close();
  pendingSDData = true;

  Serial.printf("[SD] Leitura salva localmente: %lu ms, %.1f°C, %.1f%%\n", millis(), t, h);
}

// ─────────────────────────────────────────────────────────────
//  Envia ao servidor as leituras que ficaram guardadas no SD
//  enquanto o ESP32 estava offline, uma de cada vez. Só remove do
//  cartão o que foi confirmado como publicado; se a conexão cair no
//  meio do envio, o restante fica guardado para a próxima tentativa.
// ─────────────────────────────────────────────────────────────
void flushSDLogs() {
  if (!sdReady || !pendingSDData) return;
  if (!SD.exists(LOG_FILE)) { pendingSDData = false; return; }

  File f = SD.open(LOG_FILE, FILE_READ);
  if (!f) {
    Serial.println("[SD] Falha ao abrir log para sincronizar.");
    return;
  }

  String header = f.readStringUntil('\n');

  SD.remove(LOG_TMP_FILE);
  File tmp;
  bool tmpOpen = false;
  bool allSent = true;
  int sentCount = 0;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    if (!allSent) {
      // já houve falha antes — só preserva o restante no arquivo temporário
      if (!tmpOpen) { tmp = SD.open(LOG_TMP_FILE, FILE_WRITE); tmpOpen = true; }
      if (tmpOpen) tmp.println(line);
      continue;
    }

    int c1 = line.indexOf(',');
    int c2 = line.indexOf(',', c1 + 1);
    if (c1 < 0 || c2 < 0) continue; // linha corrompida — descarta

    unsigned long loggedAt = line.substring(0, c1).toInt();
    float logT = line.substring(c1 + 1, c2).toFloat();
    float logH = line.substring(c2 + 1).toFloat();

    JsonDocument payload;
    payload["tempDHT"]  = logT;
    payload["umidade"]  = logH;
    payload["offline"]  = true;
    payload["loggedAt"] = loggedAt;

    char buffer[250];
    serializeJson(payload, buffer, sizeof(buffer));

    bool ok = client.publish(topic, buffer, false); // retain=false: são leituras históricas
    client.loop();

    if (ok) {
      sentCount++;
      delay(50); // dá um respiro pro broker
    } else {
      Serial.println("[SD] Falha ao enviar leitura antiga — pausando sincronização.");
      allSent = false;
      if (!tmpOpen) { tmp = SD.open(LOG_TMP_FILE, FILE_WRITE); tmpOpen = true; }
      if (tmpOpen) tmp.println(line); // guarda essa linha que falhou também
    }
  }

  f.close();
  if (tmpOpen) tmp.close();

  SD.remove(LOG_FILE);
  File nf = SD.open(LOG_FILE, FILE_WRITE);
  if (nf) {
    nf.println(header);
    if (!allSent) {
      File rf = SD.open(LOG_TMP_FILE, FILE_READ);
      if (rf) {
        while (rf.available()) {
          nf.println(rf.readStringUntil('\n'));
        }
        rf.close();
      }
    }
    nf.close();
  }
  SD.remove(LOG_TMP_FILE);

  pendingSDData = !allSent;

  if (sentCount > 0) {
    Serial.printf("[SD] %d leitura(s) antiga(s) sincronizada(s) com o servidor.\n", sentCount);
  }
  if (!allSent) {
    Serial.println("[SD] Ainda há leituras pendentes — serão retomadas na próxima conexão.");
  }
}

// ─────────────────────────────────────────────────────────────
//  MQTT — tentativa única (não bloqueia o loop indefinidamente)
// ─────────────────────────────────────────────────────────────
bool tryConnectMQTT() {
  if (client.connected()) return true;

  String clientId = "ESP32-" + String(random(0xffff), HEX);
  Serial.print("[MQTT] Conectando ao broker HiveMQ...");

  if (client.connect(clientId.c_str(), mqttUser, mqttPass)) {
    Serial.println("conectado!");
    return true;
  }

  Serial.printf("falhou, rc=%d\n", client.state());
  return false;
}

// ─────────────────────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  dht.begin();
  sdReady = setupSD();

  connectWiFi(); // não bloqueia mais quando não há credenciais salvas

  espClient.setInsecure();
  client.setServer(mqttHost, mqttPort);
}

// ─────────────────────────────────────────────────────────────
//  Loop
// ─────────────────────────────────────────────────────────────
void loop() {
  // Mantém o portal de configuração respondendo, se estiver ativo
  if (apMode) {
    dnsServer.processNextRequest();
    server.handleClient();
  }

  // Mantém a conexão MQTT viva quando já está conectado
  if (client.connected()) {
    client.loop();
  }

  // Espera o intervalo de leitura sem travar o portal/MQTT acima
  if (millis() - lastSensorRead < SENSOR_INTERVAL_MS) {
    return;
  }
  lastSensorRead = millis();

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (isnan(t) || isnan(h)) {
    Serial.println("Erro ao ler DHT!");
    return;
  }

  Serial.printf("Temp DHT: %.1f°C | Umidade: %.1f%%\n", t, h);

  bool wifiOk = (WiFi.status() == WL_CONNECTED);

  // Só tenta reconectar periodicamente se já existirem credenciais salvas
  if (hasCredentials && !wifiOk && millis() - lastWifiAttempt > WIFI_RETRY_INTERVAL_MS) {
    Serial.println("[WiFi] Tentando reconectar...");
    WiFi.reconnect();
    lastWifiAttempt = millis();

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 5000) {
      delay(200);
    }
    wifiOk = (WiFi.status() == WL_CONNECTED);
  }

  bool online = wifiOk && tryConnectMQTT();

  if (online) {
    JsonDocument payload;
    payload["tempDHT"] = t;
    payload["umidade"] = h;

    char buffer[250];
    serializeJson(payload, buffer, sizeof(buffer));

    bool ok = client.publish(topic, buffer, true);
    Serial.printf("Publicado em '%s': %s [%s]\n\n", topic, buffer, ok ? "OK" : "FALHOU");

    if (!ok) {
      logToSD(t, h); // publish falhou mesmo online -> guarda local também
    } else if (pendingSDData) {
      flushSDLogs(); // aproveita a conexão pra mandar o que ficou pendente no SD
    }
  } else {
    Serial.println("Sem conexão — salvando leitura no cartão SD.");
    logToSD(t, h);
  }
}
