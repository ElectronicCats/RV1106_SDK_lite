/*
 * CubeSat — Mission parameters (on-air and CCSDS mission parameters)
 */

#ifndef MISSION_H
#define MISSION_H

#include <stdint.h>

/* Firmware version */
#define FW_PATCH  0
#define FW_MINOR  0
#define FW_MAJOR  1

#define SPACECRAFT_ID  0x01

/* Uplink (radio0): always listening for TC */
#define UPLINK_FREQ     918000000U
#define UPLINK_SF       7
#define UPLINK_BW       125000U       /* BW125 */
#define UPLINK_CR       1       /* 4/5 */

/* Downlink (radio1): telemetry TX + TC response TX */
#define DOWNLINK_FREQ   916000000U
#define DOWNLINK_SF     7
#define DOWNLINK_BW     125000U       /* BW125 */
#define DOWNLINK_CR     1       /* 4/5 */
#define DOWNLINK_POWER  20

/* Timer intervals (ms) */
#define TELEM_INTERVAL_MS    10500U
#define SYNC_INTERVAL_MS     15000U
#define IDLE_INTERVAL_MS     20000U
#define BEACON_INTERVAL_MS   15000U   /* default, configurable via TC */

/* APIDS TC */
#define SPP_APID_TC_PING            0x01
#define SPP_APID_TC_RESETC          0x02
#define SPP_APID_TC_SEND_FW         0x03
#define SPP_APID_TC_SET_THRUSTER    0x04
#define SPP_APID_TC_SET_BEACON_RATE 0x05
#define SPP_APID_TC_BROADCAST_MSG   0x06
#define SPP_APID_TC_FLASH           0x07

/* APIDS TM */
#define SPP_APID_TM_PING            0x01
#define SPP_APID_TM_RESETC          0x02
#define SPP_APID_TM_SEND_FW         0x03
#define SPP_APID_TM_SET_THRUSTER    0x04
#define SPP_APID_TM_SET_BEACON_RATE 0x05
#define SPP_APID_TM_BROADCAST_MSG   0x06
#define SPP_APID_TM_FLASH           0x07
#define SPP_APID_TM_SEND_TM         0x08
#define SPP_APID_TM_UNKNOWN         0x09

#define MAX_PAYLOAD_CHUNK 128

#endif /* MISSION_H */
