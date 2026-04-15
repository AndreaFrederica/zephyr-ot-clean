# Parking Lock Gateway - ESP8266 UART Protocol

## Scope

This document defines the UART text protocol between the STM32 gateway and the ESP8266 module.

- UART device alias: `wifi_uart`
- UART in board DTS: `usart2`
- Baud rate: `115200`
- Line ending: `\r\n`
- Encoding: ASCII text

This protocol assumes:

- STM32 handles RS485 polling and lock control
- ESP8266 handles Wi-Fi, MQTT, cloud reconnect, and persistent storage
- both sides use one-line text frames
- configuration and status fields are plain text and do not contain `,`, `\r`, or `\n`

## General Rules

- All frames are ASCII text terminated by `\r\n`.
- Fields are comma-separated.
- Free-form fields must not contain comma `,`, carriage return, or line feed.
- `REPORT,...` and `OFFLINE,...` are raw application lines and must be preserved unchanged by the ESP8266.

## Gateway -> ESP8266

### 1) Boot Ready

Gateway sends a boot-ready line once after startup:

```text
READY,GATEWAY,1\r\n
```

`1` is the gateway-side protocol version.

### 2) Telemetry/Event Uplink

Gateway forwards raw node lines unchanged.

Examples:

```text
REPORT,1,POLL,T=26.00,H=55.00,FD=1,FA=3289,L=0,UID=6714285050678849\r\n
OFFLINE,2\r\n
REG,REQ,UID=12AB34CD56EF7788\r\n
REG,ASSIGN,1,UID=12AB34CD56EF7788\r\n
REG,ACK,1,UID=12AB34CD56EF7788\r\n
```

The ESP8266 firmware should publish or store these lines as needed.

### 3) Network Config Update

Gateway forwards screen-issued network config unchanged:

```text
NETCFG,<ssid>,<wifi_password>,<mqtt_host>,<mqtt_port>,<client_id>,<mqtt_username>,<mqtt_password>\r\n
```

Example:

```text
NETCFG,MyWiFi,12345678,broker.emqx.io,1883,gateway-001,user01,pass01\r\n
```

Field rules:

- all fields are plain text
- fields must not contain `,`, `\r`, or `\n`
- empty `wifi_password`, `mqtt_username`, and `mqtt_password` are allowed

### 4) Clear Network Config

```text
NETCLR\r\n
```

ESP8266 should erase its persisted network configuration.

### 5) Query Network Status

```text
NETSTAT\r\n
```

ESP8266 should reply with plain text status lines.

### 6) Command ACK

When the ESP8266 sends a cloud-originated command to the gateway, the gateway responds with:

```text
ACK,<command_id>,<sent|failed>,<code>\r\n
```

Examples:

```text
ACK,7f3c8f2a,sent,rs485_written\r\n
ACK,7f3c8f2a,failed,bad_node\r\n
ACK,7f3c8f2a,failed,bad_value\r\n
ACK,7f3c8f2a,failed,bus_busy\r\n
```

`code` must be a simple token without commas.

`sent` means the gateway has accepted the command and written it to the RS485 bus. It does not mean a slave node has finished the action.

`bad_node` also covers unregistered node addresses.

## ESP8266 -> Gateway

### 1) Boot Ready

ESP8266 should announce when its firmware is ready:

```text
READY,ESP,1\r\n
```

### 2) Cloud/ESP Status

ESP8266 reports its own state with one-line plain text status messages. The gateway forwards these lines to the screen unchanged.

Recommended formats:

```text
NETCFG,EMPTY\r\n
NETCFG,LOADED\r\n
NETCFG,SAVED\r\n
NETCFG,CLEARED\r\n
NETCFG,ERROR,bad_field\r\n
WIFI,CONNECTING\r\n
WIFI,CONNECTED,192.168.1.20\r\n
WIFI,DISCONNECTED,auth_fail\r\n
MQTT,CONNECTING\r\n
MQTT,CONNECTED\r\n
MQTT,DISCONNECTED,timeout\r\n
INFO,ESP,boot_complete\r\n
ERR,ESP,broker_unreachable\r\n
```

All detail fields must avoid commas.

### 3) Downlink Control Commands

Commands originating from cloud/MQTT are sent to the gateway as:

```text
CMD,<command_id>,LOCK,<node_id>,<0|1>\r\n
CMD,<command_id>,GET,<node_id>\r\n
```

Examples:

```text
CMD,7f3c8f2a,LOCK,1,1\r\n
CMD,7f3c8f2a,GET,3\r\n
```

The gateway validates the command, writes it to RS485 if valid, and replies with `ACK,...`.

### 4) Optional Link Check

ESP8266 may send:

```text
PING,ESP\r\n
```

Gateway replies:

```text
PONG,GATEWAY\r\n
```

## Required Enum Tokens

### ACK `code`

Recommended values:

- `rs485_written`
- `bad_node`
- `bad_value`
- `bad_action`
- `bus_busy`

### NETCFG status

- `EMPTY`
- `LOADED`
- `SAVED`
- `CLEARED`
- `ERROR,<code>`

### WIFI status

- `CONNECTING`
- `CONNECTED,<ip>`
- `DISCONNECTED,<reason>`

### MQTT status

- `CONNECTING`
- `CONNECTED`
- `DISCONNECTED,<reason>`

## Registration/Discovery Notes

- Gateway periodically sends `REQ,0,DISCOVER` on RS485.
- Unassigned slave nodes respond with `REG,REQ,UID=<uid_hex>`.
- Gateway assigns addresses from `1` upward using `SET,0,ADDR,<uid_hex>,<node_id>`.
- Slave confirms with `REG,ACK,<node_id>,UID=<uid_hex>`.
- Gateway only polls registered node addresses.

## Recommended ESP8266 Behavior

- Persist `NETCFG` locally in ESP flash.
- On boot, load config and automatically reconnect Wi-Fi/MQTT.
- Publish raw upstream lines to cloud as-is.
- Convert cloud downlink messages into `CMD,...` lines for the gateway.
- Forward connection status to STM32 using `NETCFG/...`, `WIFI/...`, `MQTT/...`, `INFO/...`, and `ERR/...`.
