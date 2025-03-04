
//
// Generic TuyaMCU information
//
/*
There are two versions of TuyaMCU that I am aware of.
TuyaMCU version 3, the one that is supported by Tasmota and documented here:

TuyaMCU version 0, aka low power protocol, documented here:
(Tuya IoT Development PlatformProduct DevelopmentLow-Code Development (MCU)Wi-Fi for Low-PowerSerial Port Protocol)
https://developer.tuya.com/en/docs/iot/tuyacloudlowpoweruniversalserialaccessprotocol?id=K95afs9h4tjjh
*/

#include "../new_common.h"
#include "../new_pins.h"
#include "../new_cfg.h"
// Commands register, execution API and cmd tokenizer
#include "../cmnds/cmd_public.h"
#include "../logging/logging.h"
#include "drv_tuyaMCU.h"
#include "drv_uart.h"
#include <time.h>
#include "drv_ntp.h"


#define TUYA_CMD_HEARTBEAT     0x00
#define TUYA_CMD_QUERY_PRODUCT 0x01
#define TUYA_CMD_MCU_CONF      0x02
#define TUYA_CMD_WIFI_STATE    0x03
#define TUYA_CMD_WIFI_RESET    0x04
#define TUYA_CMD_WIFI_SELECT   0x05
#define TUYA_CMD_SET_DP        0x06
#define TUYA_CMD_STATE         0x07
#define TUYA_CMD_QUERY_STATE   0x08
#define TUYA_CMD_SET_TIME      0x1C

void TuyaMCU_RunFrame();





//=============================================================================
//dp data point type
//=============================================================================
#define         DP_TYPE_RAW                     0x00        //RAW type
#define         DP_TYPE_BOOL                    0x01        //bool type
#define         DP_TYPE_VALUE                   0x02        //value type
#define         DP_TYPE_STRING                  0x03        //string type
#define         DP_TYPE_ENUM                    0x04        //enum type
#define         DP_TYPE_BITMAP                  0x05        //fault type


const char *TuyaMCU_GetDataTypeString(int dpId){
	if(DP_TYPE_RAW == dpId)
		return "DP_TYPE_RAW";
	if(DP_TYPE_BOOL == dpId)
		return "DP_TYPE_BOOL";
	if(DP_TYPE_VALUE == dpId)
		return "DP_TYPE_VALUE";
	if(DP_TYPE_STRING == dpId)
		return "DP_TYPE_STRING";
	if(DP_TYPE_ENUM == dpId)
		return "DP_TYPE_ENUM";
	if(DP_TYPE_BITMAP == dpId)
		return "DP_TYPE_BITMAP";
	return "DP_ERROR";
}
// from http_fns.  should move to a utils file.
extern unsigned char hexbyte( const char* hex );




const char *TuyaMCU_GetCommandTypeLabel(int t) {
	if(t == TUYA_CMD_HEARTBEAT)
		return "Hearbeat";
	if(t == TUYA_CMD_QUERY_PRODUCT)
		return "QueryProductInformation";
	if(t == TUYA_CMD_MCU_CONF)
		return "MCUconf";
	if(t == TUYA_CMD_WIFI_STATE)
		return "WiFiState";
	if(t == TUYA_CMD_WIFI_RESET)
		return "WiFiReset";
	if(t == TUYA_CMD_WIFI_SELECT)
		return "WiFiSelect";
	if(t == TUYA_CMD_SET_DP)
		return "SetDP";
	if(t == TUYA_CMD_STATE)
		return "State";
	if(t == TUYA_CMD_QUERY_STATE)
		return "QueryState";
	if(t == TUYA_CMD_SET_TIME)
		return "SetTime";
	return "Unknown";
}
typedef struct rtcc_s {
	uint8_t       second;
	uint8_t       minute;
	uint8_t       hour;
	uint8_t       day_of_week;               // sunday is day 1
	uint8_t       day_of_month;
	uint8_t       month;
	char          name_of_month[4];
	uint16_t      day_of_year;
	uint16_t      year;
	uint32_t      days;
	uint32_t      valid;
} rtcc_t;

typedef struct tuyaMCUMapping_s {
	// internal Tuya variable index
	int fnId;
	// target channel
	int channel;
	// data point type (one of the DP_TYPE_xxx defines)
	int dpType;
	// store last channel value to avoid sending it again
	int prevValue;
	// TODO
	//int mode;
	// list
	struct tuyaMCUMapping_s *next;
} tuyaMCUMapping_t;

tuyaMCUMapping_t *g_tuyaMappings = 0;

/**
 * Dimmer range
 *
 * Map OpenBK7231T_App's dimmer range of 0..100 to the dimmer range used by TuyMCU.
 * Use tuyaMCU_setDimmerrange command to set range used by TuyaMCU.
 */
// minimum dimmer value as reported by TuyaMCU dimmer
int g_dimmerRangeMin = 0;
// maximum dimmer value as reported by TuyaMCU dimmer
int g_dimmerRangeMax = 100;

// serial baud rate used to communicate with the TuyaMCU
int g_baudRate = 9600;

tuyaMCUMapping_t *TuyaMCU_FindDefForID(int fnId) {
	tuyaMCUMapping_t *cur;

	cur = g_tuyaMappings;
	while(cur) {
		if(cur->fnId == fnId)
			return cur;
		cur = cur->next;
	}
	return 0;
}

tuyaMCUMapping_t *TuyaMCU_FindDefForChannel(int channel) {
	tuyaMCUMapping_t *cur;

	cur = g_tuyaMappings;
	while(cur) {
		if(cur->channel == channel)
			return cur;
		cur = cur->next;
	}
	return 0;
}

void TuyaMCU_MapIDToChannel(int fnId, int dpType, int channel) {
	tuyaMCUMapping_t *cur;

	cur = TuyaMCU_FindDefForID(fnId);

	if(cur == 0) {
		cur = (tuyaMCUMapping_t*)malloc(sizeof(tuyaMCUMapping_t));
		cur->fnId = fnId;
		cur->dpType = dpType;
		cur->prevValue = 0;
		cur->next = g_tuyaMappings;
		g_tuyaMappings = cur;
	}

	cur->channel = channel;
}


// header version command lenght data checksum
// 55AA		00		00		0000   xx	00

#define MIN_TUYAMCU_PACKET_SIZE (2+1+1+2+1)
int UART_TryToGetNextTuyaPacket(byte *out, int maxSize) {
	int cs;
	int len, i;
	int c_garbage_consumed = 0;
	byte a, b, version, command, lena, lenb;
	char printfSkipDebug[256];
	char buffer2[8];

	printfSkipDebug[0] = 0;

	cs = UART_GetDataSize();

	if(cs < MIN_TUYAMCU_PACKET_SIZE) {
		return 0;
	}
	// skip garbage data (should not happen)
	while(cs > 0) {
		a = UART_GetNextByte(0);
		b = UART_GetNextByte(1);
		if(a != 0x55 || b != 0xAA) {
			UART_ConsumeBytes(1);
			if(c_garbage_consumed + 2 < sizeof(printfSkipDebug)) {
				sprintf(buffer2,"%02X ",a);
				strcat_safe(printfSkipDebug,buffer2,sizeof(printfSkipDebug));
			}
			c_garbage_consumed++;
			cs--;
		} else {
			break;
		}
	}
	if(c_garbage_consumed > 0){
		addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"Consumed %i unwanted non-header byte in Tuya MCU buffer\n", c_garbage_consumed);
		addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"Skipped data (part) %s\n", printfSkipDebug);
	}
	if(cs < MIN_TUYAMCU_PACKET_SIZE) {
		return 0;
	}
	a = UART_GetNextByte(0);
	b = UART_GetNextByte(1);
	if(a != 0x55 || b != 0xAA) {
		return 0;
	}
	version = UART_GetNextByte(2);
	command = UART_GetNextByte(3);
	lena = UART_GetNextByte(4); // hi
	lenb = UART_GetNextByte(5); // lo
	len = lenb | lena >> 8;
	// now check if we have received whole packet
	len += 2 + 1 + 1 + 2 + 1; // header 2 bytes, version, command, lenght, chekcusm
	if(cs >= len) {
		int ret;
		// can packet fit into the buffer?
		if(len <= maxSize) {
			for(i = 0; i < len; i++) {
				out[i] = UART_GetNextByte(i);
			}
			ret = len;
		} else {
			addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU packet too large, %i > %i\n", len, maxSize);
			ret = 0;
		}
		// consume whole packet (but don't touch next one, if any)
		UART_ConsumeBytes(len);
		return ret;
	}
	return 0;
}




// append header, len, everything, checksum
void TuyaMCU_SendCommandWithData(byte cmdType, byte *data, int payload_len) {
	int i;

	byte check_sum = (0xFF + cmdType + (payload_len >> 8) + (payload_len & 0xFF));
	UART_InitUART(g_baudRate);
	UART_SendByte(0x55);
	UART_SendByte(0xAA);
	UART_SendByte(0x00);         // version 00
	UART_SendByte(cmdType);         // version 00
	UART_SendByte(payload_len >> 8);      // following data length (Hi)
	UART_SendByte(payload_len & 0xFF);    // following data length (Lo)
	for(i = 0; i < payload_len; i++) {
		byte b = data[i];
		check_sum += b;
		UART_SendByte(b);
	}
	UART_SendByte(check_sum);
}

void TuyaMCU_SendState(uint8_t id, uint8_t type, uint8_t* value)
{
  uint16_t payload_len = 4;
  uint8_t payload_buffer[8];
  payload_buffer[0] = id;
  payload_buffer[1] = type;
  switch (type) {
    case DP_TYPE_BOOL:
    case DP_TYPE_ENUM:
      payload_len += 1;
      payload_buffer[2] = 0x00;
      payload_buffer[3] = 0x01;
      payload_buffer[4] = value[0];
      break;
    case DP_TYPE_VALUE:
      payload_len += 4;
      payload_buffer[2] = 0x00;
      payload_buffer[3] = 0x04;
      payload_buffer[4] = value[3];
      payload_buffer[5] = value[2];
      payload_buffer[6] = value[1];
      payload_buffer[7] = value[0];
      break;

  }

  TuyaMCU_SendCommandWithData(TUYA_CMD_SET_DP, payload_buffer, payload_len);
}

void TuyaMCU_SendBool(uint8_t id, bool value)
{
  TuyaMCU_SendState(id, DP_TYPE_BOOL, (uint8_t*)&value);
}

void TuyaMCU_SendValue(uint8_t id, uint32_t value)
{
  TuyaMCU_SendState(id, DP_TYPE_VALUE, (uint8_t*)(&value));
}

void TuyaMCU_SendEnum(uint8_t id, uint32_t value)
{
  TuyaMCU_SendState(id, DP_TYPE_ENUM, (uint8_t*)(&value));
}

static uint16_t convertHexStringtoBytes (uint8_t * dest, char src[], uint16_t src_len){
  if (NULL == dest || NULL == src || 0 == src_len){
    return 0;
  }

  char hexbyte[3];
  hexbyte[2] = 0;
  uint16_t i;

  for (i = 0; i < src_len; i++) {
    hexbyte[0] = src[2*i];
    hexbyte[1] = src[2*i+1];
    dest[i] = strtol(hexbyte, NULL, 16);
  }

  return i;
}

void TuyaMCU_SendHexString(uint8_t id, char data[]) {

  uint16_t len = strlen(data)/2;
  uint16_t payload_len = 4 + len;
  uint8_t payload_buffer[payload_len];
  payload_buffer[0] = id;
  payload_buffer[1] = DP_TYPE_STRING;
  payload_buffer[2] = len >> 8;
  payload_buffer[3] = len & 0xFF;

  (void) convertHexStringtoBytes(&payload_buffer[4], data, len);

  TuyaMCU_SendCommandWithData(TUYA_CMD_SET_DP, payload_buffer, payload_len);
}

void TuyaMCU_SendString(uint8_t id, char data[]) {

  uint16_t len = strlen(data);
  uint16_t payload_len = 4 + len;
  uint8_t payload_buffer[payload_len];
  payload_buffer[0] = id;
  payload_buffer[1] = DP_TYPE_STRING;
  payload_buffer[2] = len >> 8;
  payload_buffer[3] = len & 0xFF;

  for (uint16_t i = 0; i < len; i++) {
    payload_buffer[4+i] = data[i];
  }

  TuyaMCU_SendCommandWithData(TUYA_CMD_SET_DP, payload_buffer, payload_len);
}

void TuyaMCU_SendRaw(uint8_t id, char data[]) {
  char* beginPos = strchr(data, 'x');
  if(!beginPos) {
    beginPos = strchr(data, 'X');
  }
  if(!beginPos) {
    beginPos = data;
  } else {
    beginPos += 1;
  }
  uint16_t strSize = strlen(beginPos);
  uint16_t len = strSize/2;
  uint16_t payload_len = 4 + len;
  uint8_t payload_buffer[payload_len];
  payload_buffer[0] = id;
  payload_buffer[1] = DP_TYPE_RAW;
  payload_buffer[2] = len >> 8;
  payload_buffer[3] = len & 0xFF;

  (void) convertHexStringtoBytes(&payload_buffer[4], beginPos, len);

  TuyaMCU_SendCommandWithData(TUYA_CMD_SET_DP, payload_buffer, payload_len);
}

void TuyaMCU_Send_SetTime(struct tm *pTime) {
	byte payload_buffer[8];
	byte tuya_day_of_week;

	if (pTime->tm_wday == 1) {
		tuya_day_of_week = 7;
	} else {
		tuya_day_of_week = pTime->tm_wday-1;
	}

	payload_buffer[0] = 0x01;
	payload_buffer[1] = pTime->tm_year % 100;
	payload_buffer[2] = pTime->tm_mon;
	payload_buffer[3] = pTime->tm_mday;
	payload_buffer[4] = pTime->tm_hour;
	payload_buffer[5] = pTime->tm_min;
	payload_buffer[6] = pTime->tm_sec;
	payload_buffer[7] = tuya_day_of_week; //1 for Monday in TUYA Doc

	TuyaMCU_SendCommandWithData(TUYA_CMD_SET_TIME, payload_buffer, 8);
}
struct tm * TuyaMCU_Get_NTP_Time() {
	int g_time;
	struct tm * ptm;

	g_time = NTP_GetCurrentTime();
	addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"MCU time to set: %i\n", g_time);
	ptm = gmtime(&g_time);
	addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"ptime ->gmtime => tm_hour: %i\n",ptm->tm_hour );
	addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"ptime ->gmtime => tm_min: %i\n", ptm->tm_min );

	return ptm;
}
// 
int TuyaMCU_Fake_Hex(const void *context, const char *cmd, const char *args, int cmdFlags) {
	//const char *args = CMD_GetArg(1);
	//byte rawData[128];
	//int curCnt;

	//curCnt = 0;
	if(!(*args)) {
		addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_Fake_Hex: requires 1 argument (hex string, like FFAABB00CCDD\n");
		return -1;
	}
	while(*args) {
		byte b;
		b = hexbyte(args);

		//rawData[curCnt] = b;
		//curCnt++;
		//if(curCnt>=sizeof(rawData)) {
		//	addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_Fake_Hex: sorry, given string is too long\n");
		//	return -1;
		//}

		UART_AppendByteToCircularBuffer(b);

		args += 2;
	}
	return 1;
}
void TuyaMCU_Send_RawBuffer(byte *data, int len) {
	int i;

	for(i = 0; i < len; i++) {
		UART_SendByte(data[i]);
	}
}
//battery-powered water sensor with TyuaMCU request to get somo response
// uartSendHex 55AA0001000000 - this will get reply:
//Info:TuyaMCU:TuyaMCU_ParseQueryProductInformation: received {"p":"j53rkdu55ydc0fkq","v":"1.0.0"}
//
// and this will get states: 0x55 0xAA 0x00 0x02 0x00 0x01 0x04 0x06
// uartSendHex 55AA000200010406
/*Info:MAIN:Time 143, free 88864, MQTT 1, bWifi 1, secondsWithNoPing -1, socks 2/38
Info:TuyaMCU:TUYAMCU received: 55 AA 00 08 00 0C 00 02 02 02 02 02 02 01 04 00 01 00 25 

Info:TuyaMCU:TuyaMCU_ProcessIncoming: processing V0 command 8 with 19 bytes

Info:TuyaMCU:TuyaMCU_V0_ParseRealTimeWithRecordStorage: processing dpId 1, dataType 4-DP_TYPE_ENUM and 1 data bytes

Info:TuyaMCU:TuyaMCU_V0_ParseRealTimeWithRecordStorage: raw data 1 byte: 
Info:GEN:No change in channel 1 (still set to 0) - ignoring
*/

int TuyaMCU_Send_Hex(const void *context, const char *cmd, const char *args, int cmdFlags) {
	//const char *args = CMD_GetArg(1);
	if(!(*args)) {
		addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_Send_Hex: requires 1 argument (hex string, like FFAABB00CCDD\n");
		return -1;
	}
	while(*args) {
		byte b;
		b = hexbyte(args);

		UART_SendByte(b);

		args += 2;
	}
	return 1;
}

int TuyaMCU_LinkTuyaMCUOutputToChannel(const void *context, const char *cmd, const char *args, int cmdFlags) {
	int dpId;
	const char *dpTypeString;
	int dpType;
	int channelID;

	// linkTuyaMCUOutputToChannel dpId varType channelID
	// linkTuyaMCUOutputToChannel 1 val 1
	Tokenizer_TokenizeString(args);

	if(Tokenizer_GetArgsCount() < 3) {
		addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_LinkTuyaMCUOutputToChannel: requires 3 arguments (dpId, dpType, channelIndex)\n");
		return -1;
	}
	dpId = Tokenizer_GetArgInteger(0);
	dpTypeString = Tokenizer_GetArg(1);
	if(!stricmp(dpTypeString,"bool")) {
		dpType = DP_TYPE_BOOL;
	} else if(!stricmp(dpTypeString,"val")) {
		dpType = DP_TYPE_VALUE;
	} else if(!stricmp(dpTypeString,"str")) {
		dpType = DP_TYPE_STRING;
	} else if(!stricmp(dpTypeString,"enum")) {
		dpType = DP_TYPE_ENUM;
	} else if(!stricmp(dpTypeString,"raw")) {
		dpType = DP_TYPE_RAW;
	} else {
		if(strIsInteger(dpTypeString)) {
			dpType = atoi(dpTypeString);
		} else {
			addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_LinkTuyaMCUOutputToChannel: %s is not a valid var type\n",dpTypeString);
			return -1;
		}
	}
	channelID = Tokenizer_GetArgInteger(2);

	TuyaMCU_MapIDToChannel(dpId, dpType, channelID);

	return 1;
}

int TuyaMCU_Send_SetTime_Current(const void *context, const char *cmd, const char *args, int cmdFlags) {

	TuyaMCU_Send_SetTime(TuyaMCU_Get_NTP_Time());

	return 1;
}
int TuyaMCU_Send_SetTime_Example(const void *context, const char *cmd, const char *args, int cmdFlags) {
	struct tm testTime;

	testTime.tm_year = 2012;
	testTime.tm_mon = 7;
	testTime.tm_mday = 15;
	testTime.tm_wday = 4;
	testTime.tm_hour = 6;
	testTime.tm_min = 54;
	testTime.tm_sec = 32;

	TuyaMCU_Send_SetTime(&testTime);
	return 1;
}

void TuyaMCU_Send(byte *data, int size) {
	int i;
    unsigned char check_sum;

	check_sum = 0;
	for(i = 0; i < size; i++) {
		byte b = data[i];
		check_sum += b;
		UART_SendByte(b);
	}
	UART_SendByte(check_sum);

	addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"\nWe sent %i bytes to Tuya MCU\n",size+1);
}

int TuyaMCU_SetDimmerRange(const void *context, const char *cmd, const char *args, int cmdFlags) {
	Tokenizer_TokenizeString(args);

	if(Tokenizer_GetArgsCount() < 2) {
		addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"tuyaMcu_setDimmerRange: requires 2 arguments (dimmerRangeMin, dimmerRangeMax)\n");
		return -1;
	}

	g_dimmerRangeMin = Tokenizer_GetArgInteger(0);
	g_dimmerRangeMax = Tokenizer_GetArgInteger(1);

	return 1;
}

int TuyaMCU_SendHeartbeat(const void *context, const char *cmd, const char *args, int cmdFlags) {
	TuyaMCU_SendCommandWithData(TUYA_CMD_HEARTBEAT, NULL, 0);

	return 1;
}

int TuyaMCU_SendQueryProductInformation(const void *context, const char *cmd, const char *args, int cmdFlags) {
	TuyaMCU_SendCommandWithData(TUYA_CMD_QUERY_PRODUCT, NULL, 0);

	return 1;
}
int TuyaMCU_SendQueryState(const void *context, const char *cmd, const char *args, int cmdFlags) {
	TuyaMCU_SendCommandWithData(TUYA_CMD_QUERY_STATE, NULL, 0);

	return 1;
}

int TuyaMCU_SendStateCmd(const void *context, const char *cmd, const char *args, int cmdFlags) {
	int dpId;
	int dpType;
	int value;

	Tokenizer_TokenizeString(args);

	if(Tokenizer_GetArgsCount() < 3) {
		addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"tuyaMcu_sendState: requires 3 arguments (dpId, dpType, value)\n");
		return -1;
	}

	dpId = Tokenizer_GetArgInteger(0);
	dpType = Tokenizer_GetArgInteger(1);
	value = Tokenizer_GetArgInteger(2);

	TuyaMCU_SendState(dpId, dpType, (uint8_t *)&value);

	return 1;
}

int TuyaMCU_SendMCUConf(const void *context, const char *cmd, const char *args, int cmdFlags) {
	TuyaMCU_SendCommandWithData(TUYA_CMD_MCU_CONF, NULL, 0);

	return 1;
}

// ntp_timeZoneOfs 2
// addRepeatingEvent 10 -1 uartSendHex 55AA0008000007
// setChannelType 1 temperature_div10
// setChannelType 2 humidity
// linkTuyaMCUOutputToChannel 1 1
// linkTuyaMCUOutputToChannel 2 2
// addRepeatingEvent 10 -1 tuyaMcu_sendCurTime
//
// same as above but merged
// backlog ntp_timeZoneOfs 2; addRepeatingEvent 10 -1 uartSendHex 55AA0008000007; setChannelType 1 temperature_div10; setChannelType 2 humidity; linkTuyaMCUOutputToChannel 1 1; linkTuyaMCUOutputToChannel 2 2; addRepeatingEvent 10 -1 tuyaMcu_sendCurTime
//
void TuyaMCU_ApplyMapping(int fnID, int value) {
	tuyaMCUMapping_t *mapping;
	int mappedValue = value;

	// find mapping (where to save received data)
	mapping = TuyaMCU_FindDefForID(fnID);

	if(mapping == 0){	
		addLogAdv(LOG_DEBUG, LOG_FEATURE_TUYAMCU,"TuyaMCU_ApplyMapping: id %i with value %i is not mapped\n", fnID, value);
		return;
	}

	// map value depending on channel type
	switch(CHANNEL_GetType(mapping->channel))
	{
	case ChType_Dimmer:
		// map TuyaMCU's dimmer range to OpenBK7231T_App's dimmer range 0..100
		mappedValue = ((value - g_dimmerRangeMin) * 100) / (g_dimmerRangeMax - g_dimmerRangeMin);
		break;
	default:
		break;
	}

	if (value != mappedValue) {
		addLogAdv(LOG_DEBUG, LOG_FEATURE_TUYAMCU,"TuyaMCU_ApplyMapping: mapped value %d (TuyaMCU range) to %d (OpenBK7321T_App range)\n", value, mappedValue);
	}

	mapping->prevValue = mappedValue;

	CHANNEL_Set(mapping->channel,mappedValue,0);
}

void TuyaMCU_OnChannelChanged(int channel, int iVal) {
	tuyaMCUMapping_t *mapping;
	int mappediVal = iVal;

	// find mapping
	mapping = TuyaMCU_FindDefForChannel(channel);

	if(mapping == 0){
		return;
	}

	// this might be a callback from CHANNEL_Set in TuyaMCU_ApplyMapping. If we should set exactly the
	// same value, skip it
	if (mapping->prevValue == iVal) {
		return;
	}

	// map value depending on channel type
	switch(CHANNEL_GetType(mapping->channel))
	{
	case ChType_Dimmer:
		// map OpenBK7231T_App's dimmer range 0..100 to TuyaMCU's dimmer range
		mappediVal = (((g_dimmerRangeMax - g_dimmerRangeMin) * iVal) / 100) + g_dimmerRangeMin;
		break;
	default:
		break;
	}

	if (iVal != mappediVal) {
		addLogAdv(LOG_DEBUG, LOG_FEATURE_TUYAMCU,"TuyaMCU_OnChannelChanged: mapped value %d (OpenBK7321T_App range) to %d (TuyaMCU range)\n", iVal, mappediVal);
	}

	// send value to TuyaMCU
	switch(mapping->dpType)
	{
	case DP_TYPE_BOOL:
		TuyaMCU_SendBool(mapping->fnId, mappediVal != 0);
		break;

	case DP_TYPE_ENUM:
		TuyaMCU_SendEnum(mapping->fnId, mappediVal);
		break;

	case DP_TYPE_VALUE:
		TuyaMCU_SendValue(mapping->fnId, mappediVal);
		break;

	default:
		addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_OnChannelChanged: channel %d: unsupported data point type %d-%s\n", channel, mapping->dpType, TuyaMCU_GetDataTypeString(mapping->dpType));
		break;
	}
}

void TuyaMCU_ParseQueryProductInformation(const byte *data, int len) {
	char name[256];
	int useLen;

	useLen = len-1;
	if(useLen > sizeof(name)-1)
		useLen = sizeof(name)-1;
	memcpy(name,data,useLen);
	name[useLen] = 0;

	addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_ParseQueryProductInformation: received %s\n", name);
}
// Protocol version - 0x00 (not 0x03)
// Used for battery powered devices, eg. door sensor.
// Packet ID: 0x08


//55AA 00 08 000C  00 02 02 02 02 02 02 01 01 00 01 01 23
//Head v0 ID lengh bV YY MM DD HH MM SS				   CHKSUM
// after that, there are status data uniys
// 01	01	 0001   01
// dpId Type Len	Value
void TuyaMCU_V0_ParseRealTimeWithRecordStorage(const byte *data, int len) {
	int ofs;
	int sectorLen;
	int fnId;
	int dataType;

	data[0]; // bDateValid
	data[1]; //  year
	data[2]; //  month
	data[3]; //  day
	data[4]; //  hour
	data[5]; //  minute
	data[6]; //  second

	ofs = 7;

	while(ofs + 4 < len) {
		sectorLen = data[ofs + 2] << 8 | data[ofs + 3];
		fnId = data[ofs];
		dataType = data[ofs+1];
		addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_V0_ParseRealTimeWithRecordStorage: processing dpId %i, dataType %i-%s and %i data bytes\n",
			fnId, dataType, TuyaMCU_GetDataTypeString(dataType),sectorLen);


		if(sectorLen == 1) {
			int iVal = (int)data[ofs+4];
			addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_V0_ParseRealTimeWithRecordStorage: raw data 1 byte: %c\n",iVal);
			// apply to channels
			TuyaMCU_ApplyMapping(fnId,iVal);
		}
		if(sectorLen == 4) {
			int iVal = data[ofs + 4] << 24 | data[ofs + 5] << 16 | data[ofs + 6] << 8 | data[ofs + 7];
			addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_V0_ParseRealTimeWithRecordStorage: raw data 4 int: %i\n",iVal);
			// apply to channels
			TuyaMCU_ApplyMapping(fnId,iVal);
		}

		// size of header (type, datatype, len 2 bytes) + data sector size
		ofs += (4+sectorLen);
	}
}
void TuyaMCU_ParseStateMessage(const byte *data, int len) {
	int ofs;
	int sectorLen;
	int fnId;
	int dataType;

	ofs = 0;

	while(ofs + 4 < len) {
		sectorLen = data[ofs + 2] << 8 | data[ofs + 3];
		fnId = data[ofs];
		dataType = data[ofs+1];
		addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_ParseStateMessage: processing dpId %i, dataType %i-%s and %i data bytes\n",
			fnId, dataType, TuyaMCU_GetDataTypeString(dataType),sectorLen);


		if(sectorLen == 1) {
			int iVal = (int)data[ofs+4];
			addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_ParseStateMessage: raw data 1 byte: %c\n",iVal);
			// apply to channels
			TuyaMCU_ApplyMapping(fnId,iVal);
		}
		if(sectorLen == 4) {
			int iVal = data[ofs + 4] << 24 | data[ofs + 5] << 16 | data[ofs + 6] << 8 | data[ofs + 7];
			addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_ParseStateMessage: raw data 4 int: %i\n",iVal);
			// apply to channels
			TuyaMCU_ApplyMapping(fnId,iVal);
		}

		// size of header (type, datatype, len 2 bytes) + data sector size
		ofs += (4+sectorLen);
	}

}
#define TUYA_V0_CMD_PRODUCTINFORMATION		0x01
#define TUYA_V0_CMD_NETWEORKSTATUS			0x02
#define TUYA_V0_CMD_RESETWIFI				0x03
#define TUYA_V0_CMD_RESETWIFI_AND_SEL_CONF	0x04
#define TUYA_V0_CMD_REALTIMESTATUS			0x05
#define TUYA_V0_CMD_RECORDSTATUS			0x08

void TuyaMCU_ProcessIncoming(const byte *data, int len) {
	int checkLen;
	int i;
	byte checkCheckSum;
	byte cmd;
	byte version;

	if(data[0] != 0x55 || data[1] != 0xAA) {
		addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_ProcessIncoming: discarding packet with bad ident and len %i\n",len);
		return;
	}
	version = data[2];
	checkLen = data[5] | data[4] >> 8;
	checkLen = checkLen + 2 + 1 + 1 + 2 + 1;
	if(checkLen != len) {
		addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_ProcessIncoming: discarding packet bad expected len, expected %i and got len %i\n",checkLen,len);
		return;
	}
	checkCheckSum = 0;
	for(i = 0; i < len-1; i++) {
		checkCheckSum += data[i];
	}
	if(checkCheckSum != data[len-1]) {
		addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_ProcessIncoming: discarding packet bad expected checksum, expected %i and got checksum %i\n",(int)data[len-1],(int)checkCheckSum);
		return;
	}
	cmd = data[3];
	addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_ProcessIncoming[ver=%i]: processing command %i (%s) with %i bytes\n",version,cmd,TuyaMCU_GetCommandTypeLabel(cmd),len);
	switch(cmd)
	{
	case TUYA_CMD_STATE:
		TuyaMCU_ParseStateMessage(data+6,len-6);
		break;
	case TUYA_CMD_SET_TIME:
		addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_ProcessIncoming: received TUYA_CMD_SET_TIME, so sending back time");
		TuyaMCU_Send_SetTime(TuyaMCU_Get_NTP_Time());
		break;
		// 55 AA 00 01 00 ${"p":"e7dny8zvmiyhqerw","v":"1.0.0"}$
		// tuyaMcu_fakeHex 55AA000100247B2270223A226537646E79387A766D69796871657277222C2276223A22312E302E30227D24
	case TUYA_CMD_QUERY_PRODUCT:
		TuyaMCU_ParseQueryProductInformation(data+6,len-6);
		break;
		// this name seems invalid for Version 0 of TuyaMCU
	case TUYA_CMD_QUERY_STATE:
		if(version == 0) {
			// 0x08 packet for version 0 (not 0x03) of TuyaMCU
			TuyaMCU_V0_ParseRealTimeWithRecordStorage(data+6,len-6);
		} else {

		}
		break;
	default:
		addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_ProcessIncoming: unhandled type %i",cmd);
		break;
		}
}
int TuyaMCU_FakePacket(const void *context, const char *cmd, const char *args, int cmdFlags) {
	byte packet[256];
	int c = 0;
	if(!(*args)) {
		addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_FakePacket: requires 1 argument (hex string, like FFAABB00CCDD\n");
		return -1;
	}
	while(*args) {
		byte b;
		b = hexbyte(args);

		if(sizeof(packet)>c+1) {
			packet[c] = b;
			c++;
		}
		args += 2;
	}
	TuyaMCU_ProcessIncoming(packet,c);
	return 1;
}
void TuyaMCU_RunFrame() {
	byte data[128];
	char buffer_for_log[256];
	char buffer2[4];
	int len, i;

	//addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"UART ring buffer state: %i %i\n",g_recvBufIn,g_recvBufOut);

	len = UART_TryToGetNextTuyaPacket(data,sizeof(data));
	if(len > 0) {
		buffer_for_log[0] = 0;
		for(i = 0; i < len; i++) {
			sprintf(buffer2,"%02X ",data[i]);
			strcat_safe(buffer_for_log,buffer2,sizeof(buffer_for_log));
		}
		addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TUYAMCU received: %s\n", buffer_for_log);
		TuyaMCU_ProcessIncoming(data,len);
	}
}


int TuyaMCU_SetBaudRate(const void *context, const char *cmd, const char *args, int cmdFlags) {
	Tokenizer_TokenizeString(args);

	if(Tokenizer_GetArgsCount() < 1) {
		addLogAdv(LOG_INFO, LOG_FEATURE_TUYAMCU,"TuyaMCU_SetBaudRate: requires 1 arguments (baudRate)\n");
		return -1;
	}

	g_baudRate = Tokenizer_GetArgInteger(0);
	
	return 1;
}


void TuyaMCU_Init()
{
	UART_InitUART(g_baudRate);
	UART_InitReceiveRingBuffer(256);
	// uartSendHex 55AA0008000007
	CMD_RegisterCommand("tuyaMcu_testSendTime","",TuyaMCU_Send_SetTime_Example, "Sends a example date by TuyaMCU to clock/callendar MCU", NULL);
	CMD_RegisterCommand("tuyaMcu_sendCurTime","",TuyaMCU_Send_SetTime_Current, "Sends a current date by TuyaMCU to clock/callendar MCU", NULL);
	CMD_RegisterCommand("uartSendHex","",TuyaMCU_Send_Hex, "Sends raw data by TuyaMCU UART, you must write whole packet with checksum yourself", NULL);
	CMD_RegisterCommand("tuyaMcu_fakeHex","",TuyaMCU_Fake_Hex, "qq", NULL);
	///CMD_RegisterCommand("tuyaMcu_sendSimple","",TuyaMCU_Send_Simple, "Appends a 0x55 0xAA header to a data, append a checksum at end and send");
	CMD_RegisterCommand("linkTuyaMCUOutputToChannel","",TuyaMCU_LinkTuyaMCUOutputToChannel, "Map value send from TuyaMCU (eg. humidity or temperature) to channel", NULL);
	CMD_RegisterCommand("tuyaMcu_setDimmerRange","",TuyaMCU_SetDimmerRange, "Set dimmer range used by TuyaMCU", NULL);
	CMD_RegisterCommand("tuyaMcu_sendHeartbeat","",TuyaMCU_SendHeartbeat, "Send heartbeat to TuyaMCU", NULL);
	CMD_RegisterCommand("tuyaMcu_sendQueryState","",TuyaMCU_SendQueryState, "Send query state command", NULL);
	CMD_RegisterCommand("tuyaMcu_sendProductInformation","",TuyaMCU_SendQueryProductInformation, "Send qqq", NULL);
	CMD_RegisterCommand("tuyaMcu_sendState","",TuyaMCU_SendStateCmd, "Send set state command", NULL);
	CMD_RegisterCommand("tuyaMcu_sendMCUConf","",TuyaMCU_SendMCUConf, "Send MCU conf command", NULL);
	CMD_RegisterCommand("fakeTuyaPacket","",TuyaMCU_FakePacket, "qq", NULL);
	CMD_RegisterCommand("tuyaMcu_setBaudRate","",TuyaMCU_SetBaudRate, "Set the serial baud rate used to communicate with the TuyaMCU", NULL);
}
// Door sensor with TuyaMCU version 0 (not 3), so all replies have x00 and not 0x03 byte
// fakeTuyaPacket 55AA0008000C00010101010101030400010223
// fakeTuyaPacket 55AA0008000C00020202020202010100010123
// https://developer.tuya.com/en/docs/iot/tuyacloudlowpoweruniversalserialaccessprotocol?id=K95afs9h4tjjh
/// 55AA 00 08 000C 00 02 0202 020202010100010123
/// head vr id size dp dt dtln ????
// https://github.com/esphome/feature-requests/issues/497
/// 55AA 00 08 000C 00 02 02 02 02 02 02 01 01 0001 01 23
/// head vr id size FL YY MM DD HH MM SS ID TP SIZE VL CK
/// 55AA 00 08 000C 00 01 01 01 01 01 01 03 04 0001 02 23
// TP = 0x01	bool	1	Value range: 0x00/0x01.
// TP = 0x04	enum	1	Enumeration type, ranging from 0 to 255.


