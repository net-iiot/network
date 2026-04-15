# Relatório BLE — Requisitos para cliente enviar dados na rede

## Objetivo

Este documento resume o que um cliente BLE precisa para:

1. Conectar ao firmware `NetworkMesh`.
2. Escrever dados no dispositivo.
3. Fazer esses dados entrarem no fluxo da rede (`mesh`/`gateway`/`server`).

---

## Visão geral do papel do BLE no projeto

- O firmware atua como **GATT Server BLE**.
- O BLE é usado como **porta de entrada local** para receber dados de um cliente (ex.: app Flutter).
- Após receber um payload válido, o firmware encaminha o pacote para a lógica de rede (`NetworkManager`), que decide o roteamento.

---

## Dados de descoberta e conexão BLE

## Nome do dispositivo

- Nome configurado: `NetworkMesh`.

> Observação: o advertising não inclui explicitamente o nome; para descoberta robusta, prefira filtrar por UUID de serviço.

## Service UUID (128-bit)

- `574D0001-AABB-CCDD-8899-102030405060`

## Descoberta com múltiplos nós (mesmo UUID)

- Todos os nós anunciam o mesmo Service UUID.
- Portanto, o cliente encontrará **vários dispositivos compatíveis** na varredura.
- A seleção do nó de destino deve ser feita pelo app (cliente), normalmente por:
  - **RSSI mais forte** (tende a ser o mais próximo);
  - filtros adicionais (ex.: MAC conhecido, último nó usado, estabilidade de sinal).

> Importante: RSSI mais alto geralmente indica maior proximidade, mas **não é garantia absoluta** (interferência, obstáculos e multipercurso podem afetar).

## Characteristics relevantes

- **RX JSON (escrita pelo cliente):**
  - UUID binário no código: `...0002...`
  - UUID textual correspondente: `574D0002-AABB-CCDD-8899-102030405060`
  - Propriedades: `WRITE` e `WRITE WITHOUT RESPONSE`

- **BUTTON (pacote binário de botão):**
  - UUID textual: `574D0004-AABB-CCDD-8899-102030405060`
  - Propriedades: `WRITE` e `WRITE WITHOUT RESPONSE`

---

## Fluxo para envio de dado (cliente -> rede)

1. Cliente conecta por BLE ao nó.
2. Cliente escreve um JSON na characteristic RX (`...0002...`).
3. Firmware tenta fazer parse desse JSON para `Protocol::Packet`.
4. Se parse for válido, chama `NetworkManager::handle_incoming(packet)`.
5. O `NetworkManager` encaminha conforme `dst` e estado do nó:
   - Para `gateway`/`server`, pode seguir via UART (nó de borda) ou mesh.
   - Para broadcast/eventos, pode ser retransmitido na mesh com regras de loop/TTL.

## Conexão e desconexão (comportamento atual)

- O firmware **não força desconexão automática** após o envio.
- Ou seja, após conectar, o link BLE tende a permanecer ativo até:
  - o cliente desconectar;
  - ocorrer perda de sinal/erro de link;
  - timeout interno da pilha BLE.
- Ao desconectar, o nó volta a anunciar (`advertising`) para novas conexões.

## Fluxo recomendado para app cliente (modo transacional)

Para reduzir consumo e evitar sessão BLE longa desnecessária:

1. Descobrir nós pelo Service UUID.
2. Escolher o nó (preferencialmente com melhor RSSI estável).
3. Conectar.
4. Escrever payload na RX characteristic.
5. Aguardar confirmação local do app (write concluído).
6. Desconectar explicitamente.

Esse fluxo implementa na prática: **"conecta -> envia -> desconecta"**.

---

## Contrato mínimo de payload JSON

Campos obrigatórios para parse:

- `type` (string): `REQUEST`, `RESPONSE` ou `EVENT`
- `src` (string): origem lógica
- `dst` (string): destino lógico

Campos recomendados para envio de dados:

- `method` (string), ex.: `DATA`
- `body` (objeto JSON ou string)
- `ttl` (opcional; padrão interno existe no protocolo)
- `trace` (opcional, recomendado para rastreabilidade)

### Exemplo mínimo válido

```json
{
  "type": "EVENT",
  "src": "app-mobile",
  "dst": "server",
  "method": "DATA",
  "body": {
    "temp": 25.3,
    "hum": 61
  }
}
```

---

## Regras e validações importantes

## Parse e estrutura

- Se `type`, `src` ou `dst` faltar (ou não for string), o pacote é rejeitado.
- Se JSON estiver malformado, o pacote é descartado.

## Roteamento e proteção de rede

- O roteamento respeita `dst` e tipo de nó (normal, borda, gateway).
- Em tráfego mesh, o sistema usa mecanismos de proteção:
  - decremento de `ttl`;
  - detecção de loop via `trace.path`.

## Segurança BLE

- Há configuração de segurança BLE e bonding (`SC_BOND`) no firmware.
- Endereços pareados podem ser persistidos em NVS.
- As characteristics estão com permissão de escrita (`WRITE`), sem exigir explicitamente escrita criptografada no ponto de definição da characteristic.
- Portanto, no cenário atual, o cliente pode operar em fluxo simples sem obrigar pareamento prévio em toda sessão (dependendo da política do sistema operacional do cliente).

---

## Limites e observações práticas

- Existe constante de referência para payload BLE (`kMaxBLEPayload = 512`), mas não há validação explícita de tamanho no ponto de escrita BLE antes do parse.
- Para maior robustez do cliente:
  - manter payloads compactos;
  - validar JSON localmente antes de enviar;
  - preencher `src/dst/method` de forma consistente com o roteamento esperado.

---

## Checklist rápido para cliente BLE

- Descobrir dispositivo pelo UUID de serviço `574D0001-AABB-CCDD-8899-102030405060`.
- Se houver vários nós, selecionar alvo por RSSI/filtro (não assumir escolha automática "correta" sem critério).
- Conectar ao GATT Server.
- Localizar characteristic RX `574D0002-AABB-CCDD-8899-102030405060`.
- Enviar JSON com `type`, `src`, `dst`.
- Desconectar explicitamente após envio, se o app usar modo transacional.
- Preferir `method` e `body` bem definidos.
- Verificar no app logs/retries em caso de desconexão ou rejeição por JSON inválido.

---

## Conclusão

Para um cliente conseguir enviar dado para a rede via BLE neste projeto, o essencial é:

1. Conectar ao serviço BLE correto.
2. Escrever um `Protocol::Packet` JSON válido na characteristic RX.
3. Respeitar os campos mínimos e as regras de roteamento (`dst`, `ttl`, coerência do payload).

Com isso, o firmware recebe, interpreta e integra o dado ao fluxo da malha.

