# 🧩 WetzelMesh — IoT Mesh Network Framework

**WetzelMesh** é uma arquitetura modular desenvolvida em **C++ sobre ESP-IDF**, projetada para criar uma **rede mesh híbrida BLE + ESP-NOW + UART** capaz de transmitir dados entre dispositivos IoT (nós) e um gateway central conectado a um servidor.  

A rede é totalmente **extensível**, permitindo que dispositivos ESP32, sensores e até aplicativos Flutter participem da malha, trocando pacotes JSON padronizados com roteamento dinâmico, comunicação bidirecional e **rastreabilidade completa** para visualização e mapeamento da rede.

---

## 🚀 Visão Geral da Arquitetura

A rede WetzelMesh é composta por três camadas principais:

```
[ Servidor Backend / Microserviço ]
        ▲
        │ (HTTP/MQTT)
   ┌────┴─────┐
   │ Gateway  │  ⇄ UART ⇄  Border Node (Nó Raiz)
   └────┬─────┘
        │ ESP-NOW Mesh
┌───────┼────────┬────────┐
│       │         │        │
Node1  Node2    Node3   Flutter Plugin
```

### Componentes Principais

| Componente | Descrição |
|-------------|------------|
| **Gateway ESP32** | Conectado à rede Wi-Fi; recebe dados da malha via UART e encaminha ao servidor. Gera dados de teste e inicia o fluxo de comunicação. |
| **Border Node** | Nó especial conectado ao gateway via UART; recebe dados do gateway e os reencaminha para a rede mesh ESP-NOW. |
| **Nodes Mesh** | Nós intermediários que formam a malha ESP-NOW; retransmitem mensagens entre si com delay de 1 segundo por node. |
| **NetworkManager** | Gerencia os vizinhos ESP-NOW, mantém a tabela de roteamento e executa broadcast de HELLO. |
| **Router** | Decide automaticamente se o pacote vai via ESP-NOW, UART ou broadcast. Atualiza informações de rastreabilidade. |
| **Protocol** | Define a estrutura padronizada de mensagens da rede (JSON unificado) com rastreabilidade completa. |
| **LedManager** | Gerencia LEDs indicadores: Gateway (UART/Server), Nodes (UART/Mesh) - piscam quando há tráfego. |

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
- **ESP-NOW** — comunicação mesh sem fio de baixo consumo
- **UART Driver** — `esp_driver_uart`  
- **FreeRTOS** — para multitarefa e gerenciamento de eventos  
- **cJSON** — para manipulação de pacotes JSON  
- **NVS Flash** — armazenamento de configuração persistente  
- **ESP Timer** — para timestamps precisos de rastreabilidade

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
│   ├── main.cpp
│   └── Kconfig.projbuild
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
    │   ├── http_context.cpp
    │   ├── include/gateway.hpp
    │   ├── include/http_context.hpp
    │   └── CMakeLists.txt
    ├── border_uart/
    │   ├── border_uart.cpp
    │   ├── include/border_uart.hpp
    │   └── CMakeLists.txt
    ├── espnow_transport/
    │   ├── espnow_transport.cpp
    │   ├── include/espnow_transport.hpp
    │   └── CMakeLists.txt
    ├── ble_transport/
    │   ├── ble_transport.cpp
    │   ├── include/ble_transport.hpp
    │   └── CMakeLists.txt
    ├── network_manager/
    │   ├── network_manager.cpp
    │   ├── include/network_manager.hpp
    │   └── CMakeLists.txt
    ├── led_manager/
    │   ├── led_manager.cpp
    │   ├── include/led_manager.hpp
    │   └── CMakeLists.txt
    ├── test_packet_generator/
    │   ├── test_packet_generator.cpp
    │   ├── include/test_packet_generator.hpp
    │   └── CMakeLists.txt
    ├── chunk_manager/
    │   ├── chunk_manager.cpp
    │   ├── include/chunk_manager.hpp
    │   └── CMakeLists.txt
    └── json_codec/
        ├── json_codec.cpp
        ├── include/json_codec.hpp
        └── CMakeLists.txt
```

### 📂 Descrição dos componentes

| Diretório | Responsabilidade |
|------------|------------------|
| **protocol/** | Define o formato do pacote (headers, corpo, rotas, tipo) e estruturas de rastreabilidade completa. |
| **router/** | Decide automaticamente o caminho do pacote e atualiza informações de trace (path, hop_count). |
| **gateway/** | Faz a ponte via UART entre a malha ESP-NOW e o servidor. Gera dados de teste a cada 5 segundos. |
| **border_uart/** | Gerencia comunicação UART entre gateway e border node. Handshake e reencaminhamento para mesh. |
| **espnow_transport/** | Implementa o transporte ESP-NOW Mesh, callbacks de recebimento e atualização de RSSI. |
| **ble_transport/** | Implementa transporte BLE para identificação de nodes e geração de node_id único. |
| **network_manager/** | Gerencia vizinhos ESP-NOW, rotas locais e tarefas de descoberta (HELLO). |
| **led_manager/** | Controla LEDs indicadores de status e tráfego (UART, Mesh, Server). |
| **test_packet_generator/** | Gera pacotes de teste para validação da rede (modo TESTE e modo REAL). |
| **chunk_manager/** | Gerencia fragmentação de mensagens grandes em chunks. |
| **main/** | Ponto de entrada do firmware (`app_main`) e configuração via menuconfig. |

---

## 🧠 Estrutura do Protocolo

Cada mensagem WetzelMesh segue um **JSON padronizado e extensível**, garantindo compatibilidade entre todos os dispositivos (ESP, sensor, app Flutter e servidor) e fornecendo **rastreabilidade completa** para mapeamento e visualização da rede.

### 📦 Estrutura Base do Pacote

```json
{
  "type": "EVENT",
  "src": "gateway",
  "dst": "border",
  "method": "DATA",
  "endpoint": "",
  "status": 0,
  "body": {
    "temp": 25,
    "hum": 65,
    "seq": 1
  },
  "request_id": "",
  "is_chunk": false,
  "chunk_id": 0,
  "chunk_total": 0,
  "chunk_index": 0
}
```

### 🔍 Campos de Rastreabilidade (Trace)

Cada pacote inclui informações completas de rastreamento para mapeamento da rede:

```json
{
  "trace": {
    "path": ["gateway", "border", "node-01", "node-02"],
    "hop_count": 3,
    "created_at_ms": 1234567890,
    "received_at_ms": 1234567893,
    "packet_id": "550e8400-e29b-41d4-a716-446655440000",
    "hop_history": [
      {
        "node_id": "gateway",
        "node_name": "Gateway Principal",
        "timestamp_ms": 1234567890,
        "rssi": 0,
        "transport": "UART"
      },
      {
        "node_id": "border",
        "node_name": "",
        "timestamp_ms": 1234567891,
        "rssi": -45,
        "transport": "UART"
      },
      {
        "node_id": "node-01",
        "node_name": "",
        "timestamp_ms": 1234567892,
        "rssi": -60,
        "transport": "MESH"
      },
      {
        "node_id": "node-02",
        "node_name": "",
        "timestamp_ms": 1234567893,
        "rssi": -65,
        "transport": "MESH"
      }
    ]
  }
}
```

#### 📊 Detalhamento dos Campos de Trace

| Campo | Tipo | Descrição |
|-------|------|-----------|
| **path** | `string[]` | Array sequencial de node IDs que o pacote percorreu: `["gateway", "border", "node-01", ...]` |
| **hop_count** | `uint32` | Número total de saltos (transmissões) que o pacote realizou |
| **created_at_ms** | `uint64` | Timestamp (milissegundos) de quando o pacote foi criado no gateway |
| **received_at_ms** | `uint64` | Timestamp de quando o pacote foi recebido no node atual |
| **packet_id** | `string` | ID único estilo UUID para rastreamento individual do pacote |
| **hop_history** | `HopInfo[]` | Histórico detalhado de cada hop com informações completas |

#### 🔗 Estrutura HopInfo

Cada entrada em `hop_history` contém:

```json
{
  "node_id": "node-01",
  "node_name": "Sala 1",
  "timestamp_ms": 1234567892,
  "rssi": -60,
  "transport": "MESH"
}
```

| Campo | Descrição |
|-------|-----------|
| **node_id** | ID técnico do node (ex: "node-01", "gateway", "border") |
| **node_name** | Nome amigável do node (ex: "Sala 1", "Corredor") - preenchido pelo microserviço |
| **timestamp_ms** | Timestamp exato de quando o pacote passou por este node |
| **rssi** | Força do sinal recebido (dBm) - útil para mapear qualidade de conexão |
| **transport** | Tipo de transporte usado: "UART", "MESH" (ESP-NOW), "BLE" |

### 🗺️ Estrutura de Topologia e Mapeamento

Cada pacote pode incluir informações de topologia da rede para visualização:

```json
{
  "topology": {
    "node_id": "node-01",
    "node_name": "Sala 1",
    "has_gateway": false,
    "gateway_id": "",
    "neighbors": [
      {
        "node_id": "border",
        "rssi": -60,
        "last_seen_ms": 1234567890
      },
      {
        "node_id": "node-02",
        "rssi": -65,
        "last_seen_ms": 1234567885
      }
    ],
    "connectivity": {
      "connections": [
        {
          "from_node_id": "node-01",
          "to_node_id": "border",
          "rssi": -60,
          "last_communication_ms": 1234567890,
          "packet_count": 42,
          "is_direct": true
        },
        {
          "from_node_id": "node-01",
          "to_node_id": "node-02",
          "rssi": -65,
          "last_communication_ms": 1234567885,
          "packet_count": 38,
          "is_direct": true
        }
      ]
    },
    "node_info": {
      "node_id": "node-01",
      "node_name": "Sala 1",
      "node_type": "node",
      "capabilities": ["sensor_temp", "sensor_hum"],
      "first_seen_ms": 1234560000,
      "last_seen_ms": 1234567890,
      "battery_level": 85,
      "position_x": 10.5,
      "position_y": 20.3,
      "metadata": {
        "location": "Sala Principal",
        "room": "101"
      }
    }
  }
}
```

#### 📋 Detalhamento da Topologia

| Estrutura | Descrição |
|-----------|-----------|
| **TopologyInfo** | Informações de topologia do node que enviou o pacote |
| **NeighborInfo** | Lista de vizinhos diretos com RSSI e último contato |
| **ConnectivityMatrix** | Matriz completa de quem pode comunicar com quem |
| **Connection** | Conexão entre dois nodes com estatísticas (RSSI, packet_count, is_direct) |
| **NodeInfo** | Informações completas do node (tipo, capacidades, posição, bateria, metadados) |

### 🛣️ Metadados de Roteamento

```json
{
  "next_hop": "node-02",
  "routing_strategy": "flooding",
  "ttl": 8
}
```

| Campo | Descrição |
|-------|-----------|
| **next_hop** | Próximo node no caminho (preenchido pelo router) |
| **routing_strategy** | Estratégia de roteamento: "flooding", "shortest_path", etc. |
| **ttl** | Time To Live - decrementado a cada hop para evitar loops infinitos |

---

## 🔄 Fluxo de Comunicação e Teste

### 📡 Fluxo de Dados no Teste

O sistema implementa um fluxo de teste específico para validação:

1. **Gateway gera dados a cada 5 segundos**
   - Cria pacote com `packet_id` único
   - Preenche `trace.created_at_ms` e `trace.path` inicial
   - Adiciona primeiro `hop_history` (gateway → UART)
   - Envia via UART para border node
   - **LED do gateway pisca** (GPIO 26)

2. **Border Node recebe via UART**
   - Adiciona ao `trace.path` e incrementa `hop_count`
   - Adiciona `hop_history` (border → UART)
   - **LED do border node pisca** (GPIO 26 - recebimento)
   - Aguarda **1 segundo com LED aceso** (GPIO 25)
   - Reencaminha para mesh ESP-NOW
   - **LED do border node pisca** (GPIO 25 - envio mesh)

3. **Nodes Mesh recebem e repassam**
   - Cada node adiciona ao `trace.path` e incrementa `hop_count`
   - Adiciona `hop_history` com RSSI recebido
   - Verifica TTL (descarta se expirado)
   - **LED do node pisca** (GPIO 25 - recebimento)
   - Aguarda **1 segundo com LED aceso** (GPIO 25)
   - Reencaminha para próximo node
   - **LED do node pisca** (GPIO 25 - envio)

4. **Geração contínua**
   - Gateway gera novo pacote a cada 5 segundos **independentemente** do anterior
   - Múltiplos pacotes podem estar na rede simultaneamente
   - Cada pacote mantém seu próprio `trace` completo

### 🎯 Sequência Visual dos LEDs

```
A cada 5 segundos:
Gateway LED (GPIO 26) pisca
  ↓
Border Node LED (GPIO 26) pisca → LED (GPIO 25) aceso por 1s → LED (GPIO 25) pisca
  ↓
Node 1 LED (GPIO 25) pisca → LED aceso por 1s → LED pisca
  ↓
Node 2 LED (GPIO 25) pisca → LED aceso por 1s → LED pisca
  ↓
...
```

---

## 🗺️ Sistema de Rastreabilidade e Mapeamento

### 📊 Como Funciona

O sistema de rastreabilidade é **automático** e **transparente**:

1. **Gateway inicia o trace**
   - Gera `packet_id` único (UUID-like)
   - Registra `created_at_ms`
   - Inicia `path` com `["gateway"]`
   - Adiciona primeiro `hop_history`

2. **Cada node atualiza automaticamente**
   - Adiciona seu `node_id` ao `path`
   - Incrementa `hop_count`
   - Registra `received_at_ms`
   - Adiciona entrada em `hop_history` com timestamp, RSSI e transport
   - Decrementa `ttl` (descarta se chegar a 0)

3. **Informações de topologia**
   - Cada node inclui sua lista de vizinhos em `topology.neighbors`
   - Matriz de conectividade pode ser construída coletando informações de múltiplos pacotes
   - Microserviço pode atribuir `node_name` e `position_x/y` baseado no mapeamento

### 🎨 Visualização no App

Com essas informações, o app pode:

#### 1. **Desenhar o Grafo da Rede**

```javascript
// Exemplo de uso no app
const nodes = extractNodesFromPackets(packets);
const edges = extractConnectionsFromTopology(packets);

// nodes = [
//   {id: "gateway", name: "Gateway Principal", x: 0, y: 0},
//   {id: "border", name: "Node Borda", x: 10, y: 0},
//   {id: "node-01", name: "Sala 1", x: 20, y: 10}
// ]

// edges = [
//   {from: "gateway", to: "border", rssi: -45, packetCount: 100},
//   {from: "border", to: "node-01", rssi: -60, packetCount: 95}
// ]
```

#### 2. **Mostrar Caminho Percorrido por Cada Pacote**

```javascript
// Visualizar rota completa
const packetTrace = packet.trace;
console.log(`Pacote ${packetTrace.packet_id} percorreu:`);
packetTrace.path.forEach((nodeId, index) => {
  const hop = packetTrace.hop_history[index];
  console.log(`  ${index + 1}. ${nodeId} (${hop.transport}) - RSSI: ${hop.rssi}dBm`);
});
```

#### 3. **Exibir Estatísticas de Rede**

- **Latência**: `received_at_ms - created_at_ms` por hop
- **Qualidade de Conexão**: RSSI médio por conexão
- **Throughput**: `packet_count` por conexão
- **Topologia**: Grafo completo de quem pode falar com quem

#### 4. **Mapeamento Automático**

- Coletar todos os pacotes recebidos
- Extrair `topology.connectivity.connections`
- Construir matriz completa de conectividade
- Identificar caminhos mais usados
- Detectar nós críticos (alta centralidade)

---

## 🔌 Configuração de Hardware

### UART entre Gateway e Border Node

| Parâmetro | Gateway | Border Node |
|-----------|---------|-------------|
| **TX Pin** | GPIO 17 | GPIO 15 |
| **RX Pin** | GPIO 16 | GPIO 13 |
| **Baudrate** | 115200 | 115200 |
| **Protocolo** | 8N1 | 8N1 |
| **Buffer Size** | 2048 bytes | 2048 bytes |

> ⚠️ **Importante**: Conecte TX do Gateway → RX do Border Node e RX do Gateway ← TX do Border Node

### LEDs Indicadores

| Device | GPIO 25 | GPIO 26 |
|--------|---------|---------|
| **Gateway** | Server Status (ON=desconectado, OFF=conectado, pisca=tráfego) | UART Status (ON=desconectado, OFF=conectado, pisca=tráfego) |
| **Node** | Mesh Status (ON=não conectado, OFF=conectado, pisca=tráfego, aceso=segurando dado) | UART Status (ON=desabilitado, OFF=habilitado, pisca=tráfego) |

---

## 🧰 Configuração Inicial do Projeto

### 1️⃣ Configurar ESP-IDF

```bash
# Instalar ESP-IDF (se ainda não instalado)
# Windows: Use Espressif IDE Installer
# Linux/Mac: Siga instruções em https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/

# Verificar instalação
idf.py --version
```

### 2️⃣ Clonar e Configurar Projeto

```bash
# Clonar repositório
git clone https://github.com/seuprojeto/wetzel-mesh.git
cd wetzel-mesh

# Configurar target (ESP32, ESP32-S2, etc.)
idf.py set-target esp32

# Configurar via menuconfig
idf.py menuconfig
```

### 3️⃣ Configurações do Menuconfig

No `menuconfig`, configure:

```
Wetzel Mesh
  ├─ Este build é GATEWAY? (y/n)
  ├─ Gateway
  │  ├─ WiFi SSID
  │  ├─ WiFi Password
  │  └─ Server URL
  └─ Test Mode
     └─ Token Hold Time (ms)
```

### 4️⃣ Compilar e Gravar

```bash
# Compilar
idf.py build

# Gravar no dispositivo
idf.py flash

# Monitorar serial
idf.py monitor

# Ou tudo de uma vez
idf.py build flash monitor
```

### 5️⃣ Limpar Build (se necessário)

```bash
idf.py fullclean
idf.py build
```

### 5️⃣ Exibir Mac

```bash
C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\esptool.exe --port COM6 read_mac
```

---

## 📱 Integração com Microserviço e App

### 🔄 Fluxo Gateway → Microserviço

O gateway envia todos os pacotes recebidos para o microserviço, que:

1. **Analisa o trace completo**
   - Extrai `trace.path` e `trace.hop_history`
   - Identifica novos nodes e conexões
   - Calcula latência e qualidade de conexão

2. **Constrói matriz de conectividade**
   - Agrega informações de múltiplos pacotes
   - Identifica quem pode falar com quem
   - Calcula estatísticas (RSSI médio, packet_count, etc.)

3. **Atribui nomes e posições**
   - Mapeia `node_id` → `node_name` (ex: "node-01" → "Sala 1")
   - Define `position_x` e `position_y` para visualização
   - Retorna informações atualizadas para o gateway

4. **Envia para o app**
   - App conecta ao microserviço via WebSocket/HTTP
   - Recebe topologia completa em tempo real
   - Desenha grafo da rede com posições e conexões

### 📊 Exemplo de API do Microserviço

```json
// POST /api/packets - Gateway envia pacote recebido
{
  "packet": { /* pacote completo com trace */ },
  "timestamp": 1234567890
}

// GET /api/topology - App solicita topologia
Response: {
  "nodes": [
    {
      "node_id": "gateway",
      "node_name": "Gateway Principal",
      "position": {"x": 0, "y": 0},
      "type": "gateway"
    },
    {
      "node_id": "border",
      "node_name": "Node Borda",
      "position": {"x": 10, "y": 0},
      "type": "border"
    }
  ],
  "connections": [
    {
      "from": "gateway",
      "to": "border",
      "rssi_avg": -45,
      "packet_count": 1000,
      "latency_avg_ms": 50
    }
  ]
}
```

---

## 🧩 Próximas Etapas do Projeto

| Fase | Descrição | Status |
|------|------------|--------|
| **1** | Estrutura base do projeto e build ESP-IDF | ✅ |
| **2** | Implementação de `Protocol` e `JSON Codec` | ✅ |
| **3** | Camadas ESP-NOW Transport e UART Gateway | ✅ |
| **4** | Network Manager (descoberta e vizinhos) | ✅ |
| **5** | Roteamento inteligente (`Router`) | ✅ |
| **6** | Integração final e teste da malha ESP-NOW↔UART | ✅ |
| **7** | Sistema de rastreabilidade e mapeamento | ✅ |
| **8** | Integração com microserviço para mapeamento | 🔜 |
| **9** | Plugin Flutter + Handshake BLE | 🔜 |
| **10** | App de visualização da rede | 🔜 |

---

## 📚 Referências Técnicas

### Estruturas de Dados Principais

#### `Protocol::Packet`

Estrutura completa do pacote com todos os campos de rastreabilidade, topologia e roteamento.

#### `Protocol::TraceInfo`

Informações de rastreamento: path, hop_count, timestamps, packet_id, hop_history.

#### `Protocol::TopologyInfo`

Informações de topologia: node_id, node_name, neighbors, connectivity matrix, node_info.

#### `Protocol::ConnectivityMatrix`

Matriz de conectividade: lista de conexões entre nodes com estatísticas.

#### `Protocol::NodeInfo`

Informações completas do node: tipo, capacidades, posição, bateria, metadados.

### Funções Helper

- `Protocol::generate_packet_id()` - Gera ID único estilo UUID para rastreamento
- `Protocol::generate_request_id()` - Gera ID único para correlacionar request/response
- `Protocol::serialize()` - Serializa Packet para JSON
- `Protocol::parse()` - Parse JSON para Packet

---

## 🐛 Debug e Troubleshooting

### LEDs não piscam

- Verifique conexão física dos pinos UART
- Confirme que border node está enviando PING
- Verifique logs no monitor serial

### Pacotes não chegam

- Verifique TTL (time to live) - pode estar expirando
- Confirme que nodes estão na mesma rede ESP-NOW
- Verifique RSSI nas conexões (valores muito baixos indicam problema)

### Trace não está sendo preenchido

- Verifique se gateway está gerando `packet_id` e `created_at_ms`
- Confirme que router está atualizando `path` e `hop_count`
- Verifique logs para ver se há erros de parse

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
