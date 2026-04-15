# Parking Lock Gateway - Screen UART Protocol

## Scope

This document defines the UART text protocol between the STM32 gateway and the screen module.

- UART device alias: `screen_uart`
- UART in board DTS: `usart1`
- Baud rate: `115200`
- Line ending: `\r\n`
- Encoding: ASCII text

The STM32 gateway is responsible for:

- polling RS485 slave nodes
- executing `LOCK` and `GET`
- forwarding node telemetry to both screen and ESP8266

The ESP8266 is responsible for:

- Wi-Fi and MQTT/cloud
- config persistence
- network status reporting

## General Rules

- All frames are ASCII text terminated by `\r\n`.
- Fields are comma-separated.
- Free-form fields must not contain comma `,`, carriage return, or line feed.
- Screen-side network config text should avoid commas in SSID, password, username, and other fields.

## Gateway -> Screen

### 1) Boot Ready

```text
GATEWAY_READY\r\n
```

### 2) Telemetry Forwarding

Gateway forwards raw RS485 telemetry lines unchanged.

Example:

```text
REPORT,1,PERIODIC,T=26.00,H=55.00,FD=0,FA=1234,L=1,DE=0,AE=0,SE=0,RB=100,RL=5,RC=2,RO=0\r\n
```

### 3) RS485 Registration Events

Gateway forwards registration-related lines for diagnostics:

```text
REG,REQ,UID=<uid_hex>\r\n
REG,ASSIGN,<node_id>,UID=<uid_hex>\r\n
REG,ACK,<node_id>,UID=<uid_hex>\r\n
REG,NACK,NOADDR\r\n
```

### 4) Offline Event

```text
OFFLINE,<node_id>\r\n
```

Example:

```text
OFFLINE,2\r\n
```

### 5) ESP Status Forwarding

The gateway forwards ESP8266 status lines unchanged:

```text
READY,ESP,1\r\n
NETCFG,EMPTY\r\n
NETCFG,SAVED\r\n
WIFI,CONNECTING\r\n
WIFI,CONNECTED,192.168.1.20\r\n
MQTT,CONNECTED\r\n
MQTT,DISCONNECTED,timeout\r\n
INFO,ESP,boot_complete\r\n
ERR,ESP,broker_unreachable\r\n
```

## Screen -> Gateway

### 1) Set Lock

```text
LOCK,<node_id>,<0|1>\r\n
```

Examples:

```text
LOCK,1,1\r\n
LOCK,1,0\r\n
```

### 2) Request Poll

```text
GET,<node_id>\r\n
```

Example:

```text
GET,1\r\n
```

### 3) Save or Update Network Config

The screen sends plain text config directly:

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

### 5) Query Network Status

```text
NETSTAT\r\n
```

## Notes

- The gateway does not parse or store `NETCFG`; it forwards it to ESP8266 unchanged.
- The gateway periodically sends `REQ,0,DISCOVER` on RS485 and assigns addresses from `1` upward.
- Only registered node addresses are polled and accepted for `LOCK`/`GET` operations.
- The screen should render `READY`, `NETCFG`, `WIFI`, `MQTT`, `INFO`, `ERR`, and `REG` lines as status/diagnostic lines.
- For the full gateway <-> ESP protocol, see [ESP_UART_PROTOCOL.md](/D:/Projects/zephyr-ot-clean/app/parking_lock_gateway/ESP_UART_PROTOCOL.md:1).
