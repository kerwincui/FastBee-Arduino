---
title: TCP Connection Budget
order: 77
---

# ESP32 TCP Connection Budget

> lwIP TCP PCB pool management strategy to prevent pages from becoming unresponsive or SSE push loss.

## Background

ESP32's lwIP TCP PCB hard limit is **16** (same for all variants). Connection allocation follows:

```
TCP_TOTAL = SSE (persistent push) + HTTP (page/API requests)
```

## Per-Chip Connection Budget

| Chip | TCP Budget | SSE | HTTP | Exhaustion Threshold | 
|------|----------|-----|------|----------|
| ESP32 | 6 | 1 | 5 | 12 |
| ESP32-S3 | 8 | 2 | 6 | 14 |
| ESP32-C6 | 6 | 1 | 5 | 12 |
| ESP32-C3 | 4 | 1 | 3 | 10 |

> **Exhaustion threshold MUST be < 16** (lwIP hard limit), leaving 2-6 slots as buffer.

## Single User Scenario Analysis

| State | MQTT | SSE | HTTP | TIME_WAIT | Total PCB |
|------|------|-----|------|-----------|--------|
| Page load peak | 1 | 1 | 5-6 | 2-4 | 9-12 |
| Steady state | 1 | 1 | 0-2 | 1-2 | 3-6 |
| Multi-tab (S3) | 1 | 2 | 4-6 | 3-5 | 10-14 |

## Governance Measures

### TIME_WAIT Periodic Cleanup

`WebConfigManager` traverses the lwIP PCB list every 30 seconds, actively aborting timed-out TIME_WAIT connections.

### SSE Connection Limit

- ESP32-S3: `MAX_SSE_CLIENTS=2` (supports multiple tabs)
- Other chips: `MAX_SSE_CLIENTS=1` (single SSE connection)
- Reject new connections when slots are full

## Key Files

| File | Purpose |
|------|------|
| `include/core/ResourceProfile.h` | `TCP_TOTAL_BUDGET` / `TCP_SSE_BUDGET` constants |
| `include/network/handlers/SSERouteHandler.h` | `MAX_SSE_CLIENTS` slot count |
| `include/network/WebConfigManager.h` | `TCP_CONN_EXHAUSTION_THRESHOLD` |
| `src/network/WebConfigManager.cpp` | TIME_WAIT cleanup implementation |

> For complete description, see [`docs/tcp-connection-budget.md`](https://github.com/kerwincui/FastBee-Arduino/blob/master/docs/tcp-connection-budget.md).
