#pragma once

// Pin map LilyGO T-ETH-Lite ESP32S3 (ESP32-S3-WROOM-1 N16R8 + W5500).
// Valori presi da LilyGO-T-ETH-Series/examples/*/utilities.h

#define ETH_MISO_PIN 11
#define ETH_MOSI_PIN 12
#define ETH_SCLK_PIN 10
#define ETH_CS_PIN 9
#define ETH_INT_PIN 13
#define ETH_RST_PIN 14
#define ETH_PHY_ADDR 1

#define SD_MISO_PIN 5
#define SD_MOSI_PIN 6
#define SD_SCLK_PIN 7
#define SD_CS_PIN 42

// Bus Sheltr (protocollo 1.6): default sui pin liberi del connettore P2.
// Sono riconfigurabili dall'interfaccia web (Configurazione -> Bus).
#define SHELTR_BUS_TX_DEFAULT 17
#define SHELTR_BUS_RX_DEFAULT 18
#define SHELTR_BUS_DE_DEFAULT -1  // -1 = TTL / RS232, altrimenti pin DE/RE per RS485

#ifndef SHELTR_FW_VERSION
#define SHELTR_FW_VERSION "0.0.0-dev"
#endif

#ifndef SHELTR_BOARD_NAME
#define SHELTR_BOARD_NAME "T-ETH-Lite-ESP32S3"
#endif
