#ifndef MESH_PB_H
#define MESH_PB_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The Meshtastic client API on the wire: which field number carries
 * what, and with which wire type. Only what sw/apps/mesh reads or
 * writes. See docs/mesh_app.md, "Clean-room and licensing".
 *
 * These are interoperability facts, written here by hand. Nothing in
 * this file is copied from the Meshtastic .proto files (GPL-3.0) and
 * nothing is generated from them. The names are ours; where a name
 * matches the schema's it is because it names the same thing.
 *
 * Each field's wire type is given as the ZPB_* its value must arrive
 * with. mesh_proto.c rejects a whole message when a known field arrives
 * with any other: in a stream with no CRC, that is the commonest
 * symptom of a frame shifted by a lost byte (docs/mesh_app.md, "Risks").
 *
 * Checked against the firmware's published schema of 2026-09
 * (protobufs 51028ca). Newer fields are skipped, so a newer firmware
 * needs no change here unless a field used below changes meaning.
 */

// -- ToRadio: what we send --
#define TR_PACKET			1	// LEN  MeshPacket
#define TR_WANT_CONFIG		3	// VAR  nonce
#define TR_DISCONNECT		4	// VAR  bool
#define TR_HEARTBEAT		7	// LEN  Heartbeat (empty is fine)

// -- FromRadio: what the node sends --
#define FR_ID				1	// VAR
#define FR_PACKET			2	// LEN  MeshPacket
#define FR_MY_INFO			3	// LEN  MyNodeInfo
#define FR_NODE_INFO		4	// LEN  NodeInfo
#define FR_CONFIG			5	// LEN  Config
#define FR_LOG_RECORD		6	// LEN
#define FR_CONFIG_COMPLETE	7	// VAR  the nonce we sent
#define FR_REBOOTED			8	// VAR  bool
#define FR_MODULE_CONFIG	9	// LEN
#define FR_CHANNEL			10	// LEN  Channel
#define FR_QUEUE_STATUS		11	// LEN  QueueStatus
#define FR_METADATA			13	// LEN  DeviceMetadata
#define FR_NOTIFICATION		16	// LEN  ClientNotification

// -- MeshPacket --
#define MP_FROM				1	// I32  fixed32
#define MP_TO				2	// I32  fixed32
#define MP_CHANNEL			3	// VAR
#define MP_DECODED			4	// LEN  Data
#define MP_ENCRYPTED		5	// LEN  (we could not decrypt it either)
#define MP_ID				6	// I32  fixed32
#define MP_RX_TIME			7	// I32  fixed32, the node's epoch seconds
#define MP_RX_SNR			8	// I32  float
#define MP_HOP_LIMIT		9	// VAR
#define MP_WANT_ACK			10	// VAR  bool
#define MP_RX_RSSI			12	// VAR  int32: negative, so ten bytes
#define MP_HOP_START		15	// VAR
#define MP_PKI_ENCRYPTED	17	// VAR  bool: a direct message, end to end

// -- Data (MeshPacket.decoded) --
#define DA_PORTNUM			1	// VAR
#define DA_PAYLOAD			2	// LEN
#define DA_REQUEST_ID		6	// I32  fixed32: which packet this answers
#define DA_REPLY_ID			7	// I32  fixed32
#define DA_EMOJI			8	// I32  fixed32: a tapback, not a message

// Port numbers: what a Data payload is.
#define PORT_TEXT			1
#define PORT_POSITION		3
#define PORT_NODEINFO		4	// payload is a User
#define PORT_ROUTING		5
#define PORT_TELEMETRY		67

// -- Routing (payload of PORT_ROUTING) --
#define RT_ERROR			3	// VAR  RT_ERR_*
#define RT_ERR_NONE			0
#define RT_ERR_NO_ROUTE		1
#define RT_ERR_GOT_NAK		2
#define RT_ERR_TIMEOUT		3
#define RT_ERR_NO_INTERFACE	4
#define RT_ERR_MAX_RETRANSMIT 5
#define RT_ERR_NO_CHANNEL	6
#define RT_ERR_TOO_LARGE	7
#define RT_ERR_NO_RESPONSE	8
#define RT_ERR_DUTY_CYCLE	9
#define RT_ERR_PKI_FAILED	34
#define RT_ERR_PKI_UNKNOWN_KEY 35

// -- MyNodeInfo --
#define MI_MY_NODE_NUM		1	// VAR

// -- NodeInfo --
#define NI_NUM				1	// VAR
#define NI_USER				2	// LEN  User
#define NI_POSITION			3	// LEN  Position
#define NI_SNR				4	// I32  float
#define NI_LAST_HEARD		5	// I32  fixed32
#define NI_DEVICE_METRICS	6	// LEN  DeviceMetrics
#define NI_HOPS_AWAY		9	// VAR
#define NI_IS_FAVORITE		10	// VAR  bool

// -- User --
#define US_ID				1	// LEN  "!a1b2c3d4"
#define US_LONG_NAME		2	// LEN  up to 39 bytes
#define US_SHORT_NAME		3	// LEN  up to 4 bytes
#define US_HW_MODEL			5	// VAR
#define US_ROLE				7	// VAR

// -- Position --
#define PO_LAT_I			1	// I32  sfixed32, degrees * 1e7
#define PO_LON_I			2	// I32  sfixed32
#define PO_ALTITUDE			3	// VAR  int32, metres
#define PO_TIME				4	// I32  fixed32

// -- DeviceMetrics (NodeInfo.device_metrics; Telemetry.device_metrics) --
#define DM_BATTERY			1	// VAR  percent; 101 means powered
#define DM_VOLTAGE			2	// I32  float

// -- Telemetry (payload of PORT_TELEMETRY) --
#define TE_DEVICE_METRICS	2	// LEN  DeviceMetrics

// -- Channel --
#define CH_INDEX			1	// VAR
#define CH_SETTINGS			2	// LEN  ChannelSettings
#define CH_ROLE				3	// VAR  CH_ROLE_*
#define CS_NAME				3	// LEN  up to 11 bytes; empty on a default primary
#define CH_ROLE_DISABLED	0
#define CH_ROLE_PRIMARY		1
#define CH_ROLE_SECONDARY	2

// -- Config: only what naming a channel and sending need --
#define CF_LORA				6	// LEN  LoRaConfig
#define LC_USE_PRESET		1	// VAR  bool
#define LC_MODEM_PRESET		2	// VAR  0 LONG_FAST .. 16 MEDIUM_TURBO
#define LC_REGION			7	// VAR  0 means unset: the radio will not transmit
#define LC_HOP_LIMIT		8	// VAR  what our own packets are sent with

// -- DeviceMetadata --
#define MD_FIRMWARE_VERSION	1	// LEN
#define MD_HW_MODEL			9	// VAR

// -- ClientNotification: the node telling the user something --
#define CN_MESSAGE			4	// LEN

// -- QueueStatus --
#define QS_FREE				2	// VAR
#define QS_MAXLEN			3	// VAR

// Addresses.
#define MESH_BROADCAST		0xffffffffu

// The largest Data.payload the firmware accepts, so the longest text.
#define MESH_PAYLOAD_MAX	233

#endif
