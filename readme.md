# 🏓 Pong com ESP32 e AWS - TP de Computação Paralela

Trabalho prático da disciplina de **Computação Paralela** do curso de **Ciência da Computação**.

O projeto consiste em um jogo de Pong controlado por **dois ESP32** com acelerômetro **MMA8452**, que enviam comandos via **WebSocket** para um **servidor Python hospedado na AWS**, que por sua vez retransmite o estado do jogo para um **navegador** que renderiza a partida em tempo real.

---

## 📐 Arquitetura do Sistema

```
┌─────────────┐         ┌─────────────────┐         ┌──────────────┐
│  ESP32 #1   │         │                 │         │              │
│  (Jogador 1)│ ──────▶ │                 │ ──────▶ │  Navegador   │
│  + MMA8452  │  WS     │  Servidor AWS   │  WS     │  (HTML/JS)   │
│             │         │  (Python)       │         │              │
├─────────────┤         │                 │         │  Renderiza   │
│  ESP32 #2   │ ──────▶ │  - Recebe mov.  │         │  o jogo      │
│  (Jogador 2)│  WS     │  - Física       │         │              │
│  + MMA8452  │         │  - Broadcast    │         │              │
└─────────────┘         └─────────────────┘         └──────────────┘
```

**Fluxo:**
1. Os ESP32 leem o acelerômetro, filtram e enviam a velocidade (`speed: -100..100`) via WebSocket.
2. O servidor processa as velocidades, calcula a física do jogo (bola, colisões, pontuação) e envia o `state` para todos os navegadores conectados.
3. O navegador renderiza o estado no `<canvas>` a 60 FPS.

**Paralelismo no ESP32 (requisito da disciplina):**
- **Task `taskSensor`** (Núcleo 1): lê o acelerômetro, aplica filtro e coloca na fila.
- **Task `taskRede`** (Núcleo 0): mantém Wi-Fi + WebSocket e envia os dados.
- **Fila circular** protegida por **mutex** (produtor/consumidor).
- **Mutex no Serial** para evitar mensagens embaralhadas.

---

## 🔧 Pré-requisitos

### Hardware
- 2× ESP32 (qualquer modelo com Wi-Fi)
- 2× Acelerômetro MMA8452
- 2× Cabo micro USB **de dados** (não pode ser só de alimentação!)
- Jumpers para ligação I2C

### Software (no PC)
- [Arduino IDE](https://www.arduino.cc/en/software) (ou outra IDE compatível)
- Driver **CP210x** (para o Windows reconhecer o ESP32)
- Pacote do ESP32 na Arduino IDE (veja passo a passo abaixo)

### Software (no servidor)
- Conta na AWS (Free Tier serve)
- Instância EC2 rodando **Amazon Linux** com IP público
- Python 3.9+ e `pip3`

---

## 🚀 Parte 1: Configurando o ESP32 na Arduino IDE

### 1.1. Instalar o driver CP210x (Windows)

Nas versões recentes do Windows, o ESP32 pode não ser reconhecido. Instale o driver:

- Baixe em: https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers
- Extraia e execute o instalador.
- Reconecte o ESP32 e verifique no **Gerenciador de Dispositivos** se aparece como `Silicon Labs CP210x USB to UART Bridge (COMx)`.

> 💡 **Dica:** evite usar hub USB. Conecte o ESP32 direto na porta USB do notebook. Hubs compartilham energia com teclado/mouse e podem não alimentar o ESP32 corretamente.

### 1.2. Instalar o pacote do ESP32 na Arduino IDE

1. Abra a Arduino IDE.
2. Vá em **File → Preferences**.
3. No campo **Additional Boards Manager URLs**, cole:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
4. Vá em **Tools → Board → Boards Manager**.
5. Procure por `esp32` e instale o pacote **"esp32 by Espressif Systems"**.

### 1.3. Instalar a biblioteca WebSockets

1. Vá em **Sketch → Include Library → Manage Libraries**.
2. Procure por `WebSockets` e instale a **"WebSockets" by Markus Sattler**.

### 1.4. Testar a comunicação I2C (opcional, mas recomendado)

Antes de rodar o código principal, confirme que o ESP32 fala com o MMA8452:

1. Monte o circuito:
   | MMA8452 | ESP32 |
   |---------|-------|
   | VCC | 3.3V |
   | GND | GND |
   | SDA | GPIO 21 |
   | SCL | GPIO 22 |
2. Rode o exemplo **I2C Scanner** (disponível em vários tutoriais).
3. Anote o endereço retornado (normalmente `0x1D` ou `0x1C`).
4. Ajuste `#define MMA8452_ADDR` no código do projeto com o valor encontrado.
### 1.5. Gravar o código nos ESP32

1. Abra o arquivo `esp32_pong.ino`.
2. **Edite as configurações** no topo do arquivo:
   ```cpp
   const char* WIFI_SSID = "NOME_DA_SUA_REDE";
   const char* WIFI_PASS = "SENHA_DA_SUA_REDE";
   const char* WS_HOST   = "ec2-xx-xx-xx-xx.compute-1.amazonaws.com";
   const uint16_t WS_PORT = 8080;
   const char* WS_PATH   = "/ws";
   #define PLAYER_ID 1   // 1 para Jogador 1, 2 para Jogador 2
   ```
3. **Grave na primeira placa** com `PLAYER_ID 1`.
4. **Grave na segunda placa** com `PLAYER_ID 2`.

> ⚠️ **Importante:** As duas placas usam o **mesmo código**, mudando apenas o `PLAYER_ID`.

### 1.6. Verificar o funcionamento

Abra o **Monitor Serial** (115200 baud) e observe:

```
=== TP Computacao Paralela - ESP32 PONG ===
Jogador ID: 1
MMA8452 inicializado (+-2g, 100 Hz).
Calibrando sensor... NAO MEXA O CONTROLE!
Offset Y calibrado: -0.0234 g
Conectando ao Wi-Fi...
Wi-Fi OK. IP: 192.168.1.42
WebSocket CONECTADO. Enviando registro...
Registrado no servidor com sucesso!
[stats] enviadas=15 descartadas=0 ws=on reg=on
```

Se aparecer **`Registrado no servidor com sucesso!`**, o ESP32 está pronto. 🎉

---

## ☁️ Parte 2: Configurando o Servidor AWS

### 2.1. Criar a instância EC2

1. Acesse o [Console AWS](https://console.aws.amazon.com/ec2).
2. Clique em **Launch Instance**.
3. Configure:
   - **Name:** `pong-server`
   - **AMI:** Amazon Linux 2023 (Free Tier)
   - **Instance type:** `t2.micro` (Free Tier)
   - **Key pair:** crie ou use uma existente (guarde o `.pem`)
   - **Security Group:** libere as portas:
     - **SSH (22)** — Source: My IP
     - **Custom TCP (8080)** — Source: `0.0.0.0/0`
4. Clique em **Launch**.

> 💡 **Recomendação:** crie um **Elastic IP** e associe à instância para o IP não mudar quando ela reiniciar.
> Console AWS → **Elastic IPs → Allocate → Associate** com a instância.

### 2.2. Conectar via SSH

```bash
ssh -i caminho/para/sua-chave.pem ec2-user@SEU_IP_PUBLICO
```

### 2.3. Instalar Python e dependências

```bash
sudo dnf update -y
sudo dnf install -y python3 python3-pip
pip3 install aiohttp
```

### 2.4. Criar a estrutura do projeto no servidor

```bash
mkdir -p ~/pong-server/public
cd ~/pong-server
```

Envie os arquivos do seu PC para o servidor (rode **no seu PC**):

```bash
scp -i caminho/para/sua-chave.pem server.py ec2-user@SEU_IP:/home/ec2-user/pong-server/
scp -i caminho/para/sua-chave.pem index.html ec2-user@SEU_IP:/home/ec2-user/pong-server/public/
```

Ou crie direto no servidor com `nano`:

```bash
nano ~/pong-server/server.py      # cole o conteúdo
nano ~/pong-server/public/index.html  # cole o conteúdo
```

### 2.5. Rodar o servidor manualmente (teste)

```bash
cd ~/pong-server
python3 server.py
```

Saída esperada:

```
Servidor HTTP + WebSocket rodando em http://0.0.0.0:8080
  - HTML:      http://SEU_IP:8080/
  - WebSocket: ws://SEU_IP:8080/ws
```

### 2.6. Deixar o servidor rodando 24/7 com `systemd`

Para o servidor **não cair** quando você fechar o SSH:

```bash
sudo nano /etc/systemd/system/pong-server.service
```

Cole:

```ini
[Unit]
Description=Pong Server (Python + aiohttp)
After=network.target

[Service]
Type=simple
User=ec2-user
WorkingDirectory=/home/ec2-user/pong-server
ExecStart=/usr/bin/python3 /home/ec2-user/pong-server/server.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Ative e inicie:

```bash
sudo systemctl daemon-reload
sudo systemctl enable pong-server
sudo systemctl start pong-server
sudo systemctl status pong-server
```

Comandos úteis:

```bash
sudo systemctl restart pong-server   # reinicia
sudo systemctl stop pong-server      # para
sudo journalctl -u pong-server -f    # logs em tempo real
```

---

## 🌐 Parte 3: Testando o Jogo

### 3.1. Abrir o jogo no navegador

No seu PC, acesse:

```
http://SEU_IP_PUBLICO:8080/
```

> ⚠️ Use **`http://`** (não `https://`) e inclua a porta **`:8080`**.

### 3.2. Verificar as conexões

No terminal do servidor, você deve ver:

```
[NOVA CONEXAO] 177.55.227.221
[RECEBIDO] {"type":"register","role":"browser"}
[+] Navegador registrado
[NOVA CONEXAO] 189.93.232.48
[RECEBIDO] {"type":"register","role":"esp32","player":1}
[+] ESP32 registrado como Jogador 1
[NOVA CONEXAO] 189.93.232.49
[RECEBIDO] {"type":"register","role":"esp32","player":2}
[+] ESP32 registrado como Jogador 2
```

### 3.3. Iniciar a partida

1. Clique em **"Iniciar Jogo"** no navegador.
2. Incline os controles para mover as raquetes.
3. A raquete verde (esquerda) é o Jogador 1; a vermelha (direita) é o Jogador 2.

---

## 🛠️ Solução de Problemas

| Erro | Causa | Solução |
|------|-------|---------|
| `ERR_CONNECTION_REFUSED` | Servidor não está rodando | `sudo systemctl restart pong-server` |
| `HTTP 404 Not Found` | `index.html` não está em `public/` | Verifique `ls -la ~/pong-server/public/` |
| `Invalid method encountered` | Cliente usando HTTPS em porta HTTP | Use `http://` (não `https://`) |
| ESP32 mostra `ws=off` | Porta 8080 fechada no Security Group | Libere `Custom TCP 8080` com source `0.0.0.0/0` |
| ESP32 mostra `ws=on` mas `reg=off` | Formato da mensagem de registro errado | Verifique se o ESP32 envia `{"type":"register","role":"esp32","player":N}` |
| Raquete não se move | ESP32 envia formato antigo (`direction`) ou servidor espera `speed` | Atualize o ESP32 com o código novo (`speed`) |
| Jogo não renderiza | HTML antigo em cache | `Ctrl + Shift + R` no navegador |
| `Address already in use` | Outro processo na porta 8080 | `sudo lsof -i :8080` e `sudo kill -9 PID` |

---

## 📂 Estrutura de Arquivos

```
pong-server/                      (no servidor AWS)
├── server.py                     # Servidor WebSocket + HTTP
├── public/
│   └── index.html                # Interface web do jogo
└── (opcional) node_modules/      # se usar Node.js

esp32_pong/                       (no PC, Arduino IDE)
└── esp32_pong.ino                # Código do ESP32
```

---

## ⚙️ Parâmetros Ajustáveis

### No ESP32 (`esp32_pong.ino`)

| Parâmetro | Padrão | Descrição |
|-----------|--------|-----------|
| `PLAYER_ID` | 1 ou 2 | Define qual jogador é a placa |
| `N_AMOSTRAS_FILTRO` | 5 | Suavização do sinal (maior = mais suave, mais lento) |
| `DEAD_ZONE` | 0.08 | Zona morta em g (evita tremida parado) |
| `ESCALA_MAXIMA` | 0.7 | Inclinação máxima útil em g |
| `INTERVALO_ENVIO` | 50 | ms entre envios (20 Hz) |

### No servidor (`server.py`)

| Parâmetro | Padrão | Descrição |
|-----------|--------|-----------|
| `PADDLE_SPEED` | 5 | Velocidade geral das raquetes |
| `PADDLE_HEIGHT` | 100 | Altura da raquete em pixels |
| `BALL_SIZE` | 10 | Tamanho da bola |
| `FPS` | 60 | Quadros por segundo do loop de física |

---

## 🧪 Testes sem Hardware

Para testar o servidor **sem os ESP32**, use um cliente WebSocket de teste:

```bash
# Instale o wscat
sudo npm install -g wscat

# Conecte no servidor
wscat -c ws://SEU_IP:8080/ws
```

E envie manualmente:

```json
{"type":"register","role":"esp32","player":1}
{"type":"move","speed":100}
{"type":"move","speed":-100}
```

---

## 👥 Autores

- Luigi Fernando Alves 
**Disciplina:** Computação Paralela
**Curso:** Ciência da Computação
**Instituição:** [Nome da Instituição]
**Data:** [Mês/Ano]

---

## 📚 Referências

- [FreeRTOS no ESP32](https://www.freertos.org/)
- [Documentação do ESP32](https://docs.espressif.com/projects/arduino-esp32/en/latest/)
- [aiohttp - WebSocket Server](https://docs.aiohttp.org/en/stable/web_advanced.html#websocket)
- [AWS EC2 - Getting Started](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/EC2_GetStarted.html)
- [MMA8452Q Datasheet](https://www.nxp.com/docs/en/data-sheet/MMA8452Q.pdf)
```

---

## 📝 Como usar este README

1. **Copie** o conteúdo acima para um arquivo `README.md` na raiz do projeto.
2. **Substitua** os campos entre `[colchetes]` com as informações do seu grupo.
3. **Suba** para o GitHub ou entregue junto com o TP.

### Dica extra: gere um PDF bonito

Se a disciplina pedir em PDF, use o [Pandoc](https://pandoc.org/):

```bash
pandoc README.md -o README.pdf --pdf-engine=xelatex
```

Ou cole direto no **Google Docs** → **Arquivo → Fazer download → PDF**.

---

## 🎯 O que este README cobre

| Seção | Conteúdo |
|-------|----------|
| **Arquitetura** | Diagrama do fluxo, paralelismo no ESP32 |
| **Pré-requisitos** | Hardware, software, drivers |
| **ESP32** | Instalação do pacote, biblioteca, driver, gravação |
| **AWS** | Criação da EC2, SSH, dependências, `systemd` |
| **Testes** | Como abrir, o que verificar, o que esperar |
| **Troubleshooting** | Tabela com os erros mais comuns e soluções |
| **Parâmetros** | O que ajustar para calibrar |
| **Testes sem hardware** | Como simular com `wscat` |

