/*
 * CubeSat — CommandService: uplink RX (radio0) + TC dispatch.
 *
 * Patron identico a radio_service / sensor_service:
 *   - Endpoint rpmsg 0x4008 "rpmsg-command" para control desde Linux
 *   - RX continua en radio0 para recibir comandos TC via LoRa
 *   - Procesa los comandos TC en el MCU y responde por radio1 (downlink)
 *   - Envia eventos EVT_TC_RX a Linux cuando recibe un TC
 *   - Poll function en ping_echo.c
 */

#ifndef COMMAND_SERVICE_H
#define COMMAND_SERVICE_H

#include <stdint.h>
#include <stdbool.h>

#include "rpmsg_lite.h"

int  command_service_attach(struct rpmsg_lite_instance *inst);
int  command_service_init_default(void);
void command_service_init(void);
void command_service_poll(void);
void command_service_poll_flush(void);

/* Thruster state (leido por telemetry_service) */
uint8_t command_get_thruster0(void);
uint8_t command_get_thruster1(void);
uint32_t command_get_beacon_interval_ms(void);

#endif /* COMMAND_SERVICE_H */
