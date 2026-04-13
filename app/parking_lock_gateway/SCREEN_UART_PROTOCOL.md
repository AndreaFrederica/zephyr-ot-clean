# Parking Lock Gateway - Screen UART Protocol

## Scope

This document defines the UART text protocol between the gateway and screen module.

- UART device alias: `screen_uart`
- UART in board DTS: `usart1`
- Baud rate: `115200`
- Line ending: `\r\n`
- Encoding: ASCII text

## Gateway -> Screen

### 1) Boot Ready

When gateway boots successfully:

```text
GATEWAY_READY\r\n
```

### 2) Telemetry Forwarding

Gateway forwards raw RS485 telemetry line from slave node to screen.

Example:

```text
REPORT,1,PERIODIC,T=26.00,H=55.00,FD=0,FA=1234,L=1,DE=0,AE=0,SE=0,RB=100,RL=5,RC=2,RO=0\r\n
```

Common fields:

- `REPORT,<node_id>,<reason>`
- `T=<temp_C>`
- `H=<humi_%>`
- `FD=<flame_digital>`
- `FA=<flame_mv>`
- `L=<lock_state>` (0 unlock, 1 lock)

### 3) Offline Event

When a node has no report for timeout window:

```text
OFFLINE,<node_id>\r\n
```

Example:

```text
OFFLINE,2\r\n
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

## Notes

- Commands are comma-separated and case-sensitive.
- Each command must be one complete line ending with `\r\n`.
- Gateway currently does not define ACK/NACK message to screen for command parsing errors.
