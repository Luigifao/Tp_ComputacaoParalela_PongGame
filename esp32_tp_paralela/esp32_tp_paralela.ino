// =============================================================
//  TP Computacao Paralela - ESP32 + MMA8452 + WebSocket
//  CONTROLADOR PRECISO DO PONG
//
//  Arquitetura:
//    - taskSensor (NUCLEO 1): le o acelerometro, filtra, aplica
//      zona morta e coloca a velocidade (-100 a +100) na fila.
//    - taskRede   (NUCLEO 0): mantem Wi-Fi + WebSocket e envia
//      as velocidades ao servidor.
//    - A fila e protegida por um MUTEX.
//    - O Serial tem seu proprio mutex.
//
//  Bibliotecas:
//    - Wire.h  e WiFi.h  (ja vem com o pacote ESP32)
//    - "WebSockets" by Markus Sattler
// =============================================================

#include <Wire.h>
#include <WiFi.h>
#include <WebSocketsClient.h>

// ---------------- CONFIGURACOES (EDITE AQUI) -----------------
const char* WIFI_SSID = "iPhone";
const char* WIFI_PASS = "12345678";

const char*    WS_HOST = "ec2-44-223-78-186.compute-1.amazonaws.com";

const uint16_t WS_PORT = 8080;
const char*    WS_PATH = "/ws";

// *** MUDE AQUI PARA CADA PLACA ***
#define PLAYER_ID 1   // 1 = Jogador 1 (esquerda), 2 = Jogador 2 (direita)

#define MMA8452_ADDR 0x1D
#define SDA_PIN      21
#define SCL_PIN      22

// ---------------- CONFIGURACOES DO CONTROLE ------------------
const int   N_AMOSTRAS_FILTRO = 5;       // tamanho do filtro de media movel
const float DEAD_ZONE         = 0.08;    // zona morta (evita micro-movimentos)
const float ESCALA_MAXIMA     = 0.7;     // inclinacao maxima util (em g)
const int   INTERVALO_ENVIO   = 50;      // ms entre envios (20 Hz)

// Tempos das tasks
const TickType_t PERIODO_LEITURA = pdMS_TO_TICKS(20);  // 50 leituras/s
const TickType_t PERIODO_ENVIO   = pdMS_TO_TICKS(10);  // checa a fila a cada 10 ms

// ---------------- REGISTRADORES DO MMA8452 -------------------
#define REG_OUT_X_MSB     0x01
#define REG_WHO_AM_I      0x0D
#define REG_XYZ_DATA_CFG  0x0E
#define REG_CTRL_REG1     0x2A

// ---------------- FILA CIRCULAR ------------------------------
#define TAM_FILA 32
int fila[TAM_FILA];
int inicioFila = 0;
int fimFila    = 0;
int qtdFila    = 0;

SemaphoreHandle_t mutexFila;
SemaphoreHandle_t mutexSerial;

volatile uint32_t amostrasDescartadas = 0;
volatile bool     wsConectado = false;
volatile bool     registrado  = false;

// Filtro de media movel
float bufferFiltro[N_AMOSTRAS_FILTRO] = {0};
int   idxFiltro = 0;

// Offset calibrado
float offsetY = 0.0;

WebSocketsClient webSocket;

// =============================================================
//  Log seguro (mutex no Serial)
// =============================================================
void logSeguro(const char* msg) {
  if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(50)) == pdTRUE) {
    Serial.println(msg);
    xSemaphoreGive(mutexSerial);
  }
}

// =============================================================
//  FILA CIRCULAR PROTEGIDA POR MUTEX
// =============================================================
bool filaInserir(int velocidade) {
  if (xSemaphoreTake(mutexFila, pdMS_TO_TICKS(5)) != pdTRUE) return false;

  if (qtdFila == TAM_FILA) {
    inicioFila = (inicioFila + 1) % TAM_FILA;
    qtdFila--;
    amostrasDescartadas++;
  }
  fila[fimFila] = velocidade;
  fimFila = (fimFila + 1) % TAM_FILA;
  qtdFila++;

  xSemaphoreGive(mutexFila);
  return true;
}

bool filaRemover(int& velocidade) {
  if (xSemaphoreTake(mutexFila, pdMS_TO_TICKS(5)) != pdTRUE) return false;

  bool ok = false;
  if (qtdFila > 0) {
    velocidade = fila[inicioFila];
    inicioFila = (inicioFila + 1) % TAM_FILA;
    qtdFila--;
    ok = true;
  }

  xSemaphoreGive(mutexFila);
  return ok;
}

// =============================================================
//  DRIVER DO MMA8452
// =============================================================
void mmaEscrever(uint8_t reg, uint8_t valor) {
  Wire.beginTransmission(MMA8452_ADDR);
  Wire.write(reg);
  Wire.write(valor);
  Wire.endTransmission();
}

uint8_t mmaLer(uint8_t reg) {
  Wire.beginTransmission(MMA8452_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)MMA8452_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

bool mmaIniciar() {
  uint8_t id = mmaLer(REG_WHO_AM_I);
  if (id != 0x2A) {
    Serial.printf("MMA8452 nao respondeu (WHO_AM_I = 0x%02X)\n", id);
    return false;
  }
  mmaEscrever(REG_CTRL_REG1, 0x00);
  mmaEscrever(REG_XYZ_DATA_CFG, 0x00);   // +-2g
  mmaEscrever(REG_CTRL_REG1, 0x19);      // 100 Hz, ativo
  return true;
}

bool mmaLerXYZ(int16_t& x, int16_t& y, int16_t& z) {
  Wire.beginTransmission(MMA8452_ADDR);
  Wire.write(REG_OUT_X_MSB);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)MMA8452_ADDR, (uint8_t)6) != 6) return false;

  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();

  x = (int16_t)((b[0] << 8) | b[1]) >> 4;
  y = (int16_t)((b[2] << 8) | b[3]) >> 4;
  z = (int16_t)((b[4] << 8) | b[5]) >> 4;
  return true;
}

// =============================================================
//  CALIBRACAO DO SENSOR
// =============================================================
void calibrarSensor() {
  Serial.println("Calibrando sensor... NAO MEXA O CONTROLE!");
  long soma = 0;
  int amostras = 100;

  for (int i = 0; i < amostras; i++) {
    int16_t x, y, z;
    if (mmaLerXYZ(x, y, z)) {
      soma += y;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  offsetY = (soma / (float)amostras) / 1024.0;
  Serial.printf("Offset Y calibrado: %.4f g\n", offsetY);
}

// =============================================================
//  FILTRO DE MEDIA MOVEL
// =============================================================
float filtroMediaMovel(float nova) {
  bufferFiltro[idxFiltro] = nova;
  idxFiltro = (idxFiltro + 1) % N_AMOSTRAS_FILTRO;

  float soma = 0;
  for (int i = 0; i < N_AMOSTRAS_FILTRO; i++) soma += bufferFiltro[i];
  return soma / N_AMOSTRAS_FILTRO;
}

// =============================================================
//  WI-FI E WEBSOCKET
// =============================================================
void conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  logSeguro("Conectando ao Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  char msg[64];
  snprintf(msg, sizeof(msg), "Wi-Fi OK. IP: %s", WiFi.localIP().toString().c_str());
  logSeguro(msg);
}

void eventoWebSocket(WStype_t tipo, uint8_t* payload, size_t tamanho) {
  switch (tipo) {
    case WStype_CONNECTED: {
      wsConectado = true;
      registrado  = false;
      logSeguro("WebSocket CONECTADO. Enviando registro...");
      char reg[64];
      snprintf(reg, sizeof(reg),
               "{\"type\":\"register\",\"role\":\"esp32\",\"player\":%d}",
               PLAYER_ID);
      webSocket.sendTXT(reg);
      break;
    }
    case WStype_DISCONNECTED:
      wsConectado = false;
      registrado  = false;
      logSeguro("WebSocket DESCONECTADO (tentando reconectar...)");
      break;
    case WStype_TEXT: {
      if (strstr((char*)payload, "\"registered\"") != NULL) {
        registrado = true;
        logSeguro("Registrado no servidor com sucesso!");
      }
      break;
    }
    default:
      break;
  }
}

// =============================================================
//  TASK 1 - SENSOR (PRODUTOR) - NUCLEO 1
// =============================================================
void taskSensor(void* parametro) {
  TickType_t ultimoDespertar = xTaskGetTickCount();

  for (;;) {
    int16_t x, y, z;
    if (mmaLerXYZ(x, y, z)) {
      // 1. Converte para g e remove offset
      float y_g = (y / 1024.0) - offsetY;

      // 2. Aplica filtro de media movel
      y_g = filtroMediaMovel(y_g);

      // 3. Aplica zona morta
      if (fabs(y_g) < DEAD_ZONE) {
        y_g = 0.0;
      }

      // 4. Normaliza para -1.0 a +1.0
      float vel = y_g / ESCALA_MAXIMA;
      vel = constrain(vel, -1.0, 1.0);

      // 5. Converte para inteiro (-100 a +100)
      int velocidade = (int)(vel * 100);

      filaInserir(velocidade);
    }
    vTaskDelayUntil(&ultimoDespertar, PERIODO_LEITURA);
  }
}

// =============================================================
//  TASK 2 - REDE (CONSUMIDOR) - NUCLEO 0
// =============================================================
void taskRede(void* parametro) {
  conectarWiFi();

  webSocket.begin(WS_HOST, WS_PORT, WS_PATH);
  webSocket.onEvent(eventoWebSocket);
  webSocket.setReconnectInterval(2000);

  char json[96];
  uint32_t ultimoRelatorio = millis();
  uint32_t enviadas = 0;
  int ultimaVelocidade = 0;
  uint32_t ultimoEnvio = 0;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      wsConectado = false;
      registrado  = false;
      conectarWiFi();
    }

    webSocket.loop();

    int velocidade;
    while (wsConectado && registrado && filaRemover(velocidade)) {
      uint32_t agora = millis();
      // So envia se mudou mais de 5 unidades OU passou tempo suficiente
      if (abs(velocidade - ultimaVelocidade) > 5 ||
          agora - ultimoEnvio > INTERVALO_ENVIO) {

        snprintf(json, sizeof(json),
                 "{\"type\":\"move\",\"speed\":%d}", velocidade);
        webSocket.sendTXT(json);

        ultimaVelocidade = velocidade;
        ultimoEnvio = agora;
        enviadas++;
      }
    }

    if (millis() - ultimoRelatorio > 5000) {
      char msg[96];
      snprintf(msg, sizeof(msg),
               "[stats] enviadas=%lu descartadas=%lu ws=%s reg=%s",
               (unsigned long)enviadas,
               (unsigned long)amostrasDescartadas,
               wsConectado ? "on" : "off",
               registrado  ? "on" : "off");
      logSeguro(msg);
      ultimoRelatorio = millis();
    }

    vTaskDelay(PERIODO_ENVIO);
  }
}

// =============================================================
//  SETUP / LOOP
// =============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== TP Computacao Paralela - ESP32 PONG ===");
  Serial.printf("Jogador ID: %d\n", PLAYER_ID);

  mutexFila   = xSemaphoreCreateMutex();
  mutexSerial = xSemaphoreCreateMutex();

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);

  if (!mmaIniciar()) {
    Serial.println("ERRO: confira ligacoes e o endereco I2C.");
  } else {
    Serial.println("MMA8452 inicializado (+-2g, 100 Hz).");
    calibrarSensor();
  }

  xTaskCreatePinnedToCore(taskSensor, "Sensor", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(taskRede,   "Rede",   8192, NULL, 1, NULL, 0);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}