# 🧩 WetzelMesh — IoT Mesh Network Framework

**WetzelMesh** é uma arquitetura modular desenvolvida em **C++ sobre ESP-IDF**, projetada para criar uma **rede mesh híbrida BLE + UART** capaz de transmitir dados entre dispositivos IoT (nós) e um gateway central conectado a um servidor.  

A rede é totalmente **extensível**, permitindo que dispositivos ESP32, sensores e até aplicativos Flutter participem da malha, trocando pacotes JSON padronizados com roteamento dinâmico e comunicação bidirecional.

---

## 🚀 Visão Geral da Arquitetura

A rede WetzelMesh é composta por três camadas principais:

```
[ Servidor Backend ]
        ▲
        │ (MQTT/HTTP)
   ┌────┴─────┐
   │ Gateway  │  ⇄ UART ⇄  Nó Raiz
   └────┬─────┘
        │ BLE Mesh
┌───────┼────────┬────────┐
│       │         │        │
Node1  Node2    Node3   Flutter Plugin
```

### Componentes

| Componente | Descrição |
|-------------|------------|
| **Gateway ESP32** | Conectado à rede Wi-Fi ou LAN; recebe dados da malha BLE via UART e encaminha ao servidor. |
| **Nodes BLE** | Nós intermediários que formam a malha; retransmitem mensagens entre si e até o gateway. |
| **NetworkManager** | Gerencia os vizinhos BLE, mantém a tabela de roteamento e executa broadcast. |
| **Router** | Decide automaticamente se o pacote vai via BLE, UART ou broadcast. |
| **Protocol** | Define a estrutura padronizada de mensagens da rede (JSON unificado). |
| **Flutter Plugin** | Permite que um aplicativo móvel participe da malha BLE como um nó (enviando e recebendo pacotes JSON). |

---

## ⚙️ Requisitos do Ambiente

### 1️⃣ Ferramentas necessárias

| Ferramenta | Versão recomendada | Descrição |
|-------------|--------------------|------------|
| **ESP-IDF** | v5.5.1 ou superior | Framework principal da Espressif |
| **VS Code + Espressif IDF Extension** | Última versão | IDE e plugin oficial para desenvolvimento |
| **Python** | 3.10 ou 3.11 | Necessário para scripts e dependências do IDF |
| **Git** | 2.39 ou superior | Para clonar e gerenciar componentes externos |
| **Ninja** | (instalado pelo IDF) | Sistema de build utilizado por padrão |

> 💡 No Windows, instale o ESP-IDF via **Espressif IDE Installer**, que configura automaticamente todos os paths, toolchains e variáveis de ambiente.

---

### 2️⃣ Dependências adicionais (instaladas automaticamente)

WetzelMesh utiliza alguns componentes integrados ao ESP-IDF:

- **Bluetooth (BLE)** — via `esp_bt` e `esp_ble_mesh`  
- **UART Driver** — `esp_driver_uart`  
- **FreeRTOS** — para multitarefa e gerenciamento de eventos  
- **cJSON** — para manipulação de pacotes JSON  
- **NVS Flash** — armazenamento de configuração persistente  

> ⚠️ Certifique-se de que o **Bluetooth** esteja ativado no `menuconfig`:
>
> ```bash
> idf.py menuconfig
> ```
>
> Vá até:  
> `Component config → Bluetooth → Enable Bluetooth Controller and BLE`

---

## 🧱 Estrutura do Projeto

```
wetzel-mesh/
├── CMakeLists.txt
├── main/
│   └── main.cpp
└── components/
    ├── protocol/
    │   ├── protocol.cpp
    │   ├── include/protocol.hpp
    │   └── CMakeLists.txt
    ├── router/
    │   ├── router.cpp
    │   ├── include/router.hpp
    │   └── CMakeLists.txt
    ├── gateway/
    │   ├── gateway.cpp
    │   ├── include/gateway.hpp
    │   └── CMakeLists.txt
    ├── ble_transport/
    │   ├── ble_transport.cpp
    │   ├── include/ble_transport.hpp
    │   └── CMakeLists.txt
    ├── network_manager/
    │   ├── network_manager.cpp
    │   ├── include/network_manager.hpp
    │   └── CMakeLists.txt
    ├── json_codec/
    │   ├── json_codec.cpp
    │   ├── include/json_codec.hpp
    │   └── CMakeLists.txt
    └──led_manager/
        ├── led_manager.cpp
        ├── include/led_manager.hpp 
        └── CMakeLists.txt   
```

### 📂 Descrição dos componentes

| Diretório | Responsabilidade |
|------------|------------------|
| **protocol/** | Define o formato do pacote (headers, corpo, rotas, tipo). |
| **router/** | Decide automaticamente o caminho do pacote. |
| **gateway/** | Faz a ponte via UART entre a malha BLE e o servidor. |
| **ble_transport/** | Implementa o transporte BLE Mesh e callbacks. |
| **network_manager/** | Gerencia vizinhos BLE e rotas locais. |
| **json_codec/** | Codifica/decodifica mensagens JSON entre dispositivos. |
| **main/** | Ponto de entrada do firmware (`app_main`). |

---

## 🧠 Estrutura do Protocolo

Cada mensagem WetzelMesh segue um **JSON padronizado**,  
para garantir compatibilidade entre todos os dispositivos (ESP, sensor, app Flutter e servidor).

```json
{
  "type": "request",
  "route": {
    "src": "node-01",
    "dst": "gateway"
  },
  "method": "POST",
  "endpoint": "/api/telemetry",
  "body": {
    "temperature": 24.7,
    "humidity": 62.5,
    "voltage": 3.78
  }
}
```

O `Router` interpreta esses campos e decide o trajeto automaticamente.

---

## 🔄 Fluxo de Comunicação

### 📡 Envio de Dados (Node → Gateway → Servidor)

1. Sensor envia leitura para seu **Node BLE**.  
2. O **Router** do node detecta que o destino é `"gateway"`.  
3. Pacote é transmitido via BLE pela malha até o **gateway BLE**.  
4. O **Gateway UART** envia a requisição ao servidor HTTP/MQTT.  
5. O servidor responde; o gateway encapsula e devolve pela malha.

### 🔁 Recebimento (Servidor → Gateway → Node)

1. Servidor envia comando (JSON).  
2. Gateway o repassa via UART → Router.  
3. Router determina o destino (`node-xx`) e envia via BLE.

---

## 🧰 Configuração Inicial do Projeto

1️⃣ **Criar o projeto:**

```bash
idf.py create-project wetzel-mesh
```

2️⃣ **Clonar este repositório dentro do diretório:**

```bash
git clone https://github.com/seuprojeto/wetzel-mesh.git
```

3️⃣ **Selecionar a placa (exemplo: ESP32):**

```bash
idf.py set-target esp32
```

4️⃣ **Compilar e gravar:**

```bash
idf.py build flash monitor
```

5️⃣ **Limpar build (se necessário):**

```bash
idf.py fullclean
```

---

## 🔌 Configuração de UART entre Node e Gateway

- **TX:** GPIO 13  
- **RX:** GPIO 15  
- **Baudrate:** 115200  
- **Protocolo:** 8N1 (8 bits, sem paridade, 1 stop bit)  

> No gateway, o UART é utilizado para receber pacotes BLE encapsulados e encaminhar ao servidor via Wi-Fi.

---

## 📱 Integração com Flutter

O **plugin Flutter (WetzelMesh BLE Plugin)** permitirá:

- Descoberta automática de nós próximos (BLE scan);
- Handshake e autenticação por token;
- Envio de pacotes JSON padronizados;
- Recebimento e decodificação dos pacotes de resposta.

> Essa integração será desenvolvida na **Fase 7**, utilizando o pacote `flutter_reactive_ble` e a mesma estrutura JSON definida em `protocol`.

---

## 🧩 Próximas Etapas do Projeto

| Fase | Descrição | Status |
|------|------------|--------|
| **1** | Estrutura base do projeto e build ESP-IDF | ✅ |
| **2** | Implementação de `Protocol` e `JSON Codec` | ✅ |
| **3** | Camadas BLE Transport e UART Gateway | ✅ |
| **4** | Network Manager (descoberta e vizinhos) | ✅ |
| **5** | Roteamento inteligente (`Router`) | ✅ |
| **6** | Integração final e teste da malha BLE↔UART | ✅ |
| **7** | Plugin Flutter + Handshake BLE | 🔜 |
| **8** | Envio de dados reais ao servidor backend | 🔜 |

---

## 📚 Licença

Este projeto é distribuído sob a **licença MIT**.  
Você é livre para usar, modificar e redistribuir, desde que mantenha os créditos originais.

---

## 👨‍💻 Autor

**Bruno Santos**  
Desenvolvedor Full-Stack & IoT Engineer  
📧 <bruno.santos@empresa.com>  
📍 Wetzel Automação Industrial — 2025  
