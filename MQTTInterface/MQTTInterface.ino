#include <SoftwareSerial.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <Streaming.h>

#include "SerialInterface.h"
#include "NetworkHelper.h"

#define LOG logger << '\n' << millis() << '\t'

#define _REIFB    //Recovery Error Is First Boot
/*Send log statements out of the programming port*/
#define PROG_DBG

#define DEVICE_NAME_BASE          "mqttDev-"

#define MAX_NETWORK_NAME_LENGTH   32
#define MAX_DEVICE_NAME_LENGTH    16

#define DBG_TX_PIN  14
#define DBG_RX_PIN  15

struct SAVE_INFO
{
  char sNetworkName[MAX_NETWORK_NAME_LENGTH];
  char sNetworkPass[MAX_NETWORK_NAME_LENGTH];

  char sServerName[MAX_NETWORK_NAME_LENGTH];
  char sServerPass[MAX_NETWORK_NAME_LENGTH];

  char sDeviceName[MAX_DEVICE_NAME_LENGTH];

  uint32_t checksum;
};

/*GLOBALS*/
SAVE_INFO SavedInfo, SavedInfoMirror;
SerialInterface serInterface;
SoftwareSerial dbg(DBG_TX_PIN, DBG_RX_PIN, false, 256);
#ifdef PROG_DBG
Stream& logger(Serial);
#else
Stream& logger(dbg);
#endif
NetworkHelper helper;
uint8_t connectedState = DISCONNECTED, oldConnectedState = UNKNOWN_STATE;

/*ACCESS POINT CONFIGURATION*/
IPAddress local_IP(192, 168, 1, 1);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

/*INFO FUNCTIONS*/
uint32_t calcSavedInfoChecksum(SAVE_INFO* info)
{
  uint32_t checksum = 0;

  for (uint16_t i = 0; i < (sizeof(SAVE_INFO) - sizeof(checksum)); i++)
    checksum += ((uint8_t*)info)[i];

  return checksum;
}

bool isSavedInfoValid(SAVE_INFO* info)
{
  return (info->checksum == calcSavedInfoChecksum(info));
}

void firstBootSetup(SAVE_INFO* info)
{
  String devName;

  memset(info->sNetworkName, 0, MAX_NETWORK_NAME_LENGTH);
  memset(info->sNetworkPass, 0, MAX_NETWORK_NAME_LENGTH);
  memset(info->sServerName, 0, MAX_NETWORK_NAME_LENGTH);
  memset(info->sServerPass, 0, MAX_NETWORK_NAME_LENGTH);

  devName = DEVICE_NAME_BASE;
  size_t remainingChar = (MAX_DEVICE_NAME_LENGTH - devName.length()) - 1;
  for (size_t i = 0; i < remainingChar; i++)
  {
    char c;
    do
    {
      c = random('0', 'Z' + 1);
    } while (!isalnum(c));

    devName += c;
  }

  strcpy(info->sDeviceName, devName.c_str());
}

void SaveInfo()
{
  if (memcmp(&SavedInfo, &SavedInfoMirror, sizeof(SAVE_INFO)))
    //EEPROM and local copy are different, better save
  {
    SavedInfo.checksum = calcSavedInfoChecksum(&SavedInfo);
    LOG << "Saving";
    EEPROM.put(0, SavedInfo);
    EEPROM.commit();
    //Write back the changes to the local copy
    memcpy(&SavedInfoMirror, &SavedInfo, sizeof(SAVE_INFO));
  }
}

bool RecoverInfo()
{
  EEPROM.get(0, SavedInfoMirror);

  if (isSavedInfoValid(&SavedInfoMirror))
  {
    LOG << "Saved info is valid";
    LOG << "Device name " << SavedInfoMirror.sDeviceName;
    LOG << "Network info " << SavedInfoMirror.sNetworkName << " " << SavedInfoMirror.sNetworkPass;
    LOG << "MQTT info " << SavedInfoMirror.sServerName << " " << SavedInfoMirror.sServerPass;

    memcpy(&SavedInfo, &SavedInfoMirror, sizeof(SAVE_INFO));

    return true;
  }
  else
  {
    LOG << "Saved info is not valid";

#ifdef _REIFB
    firstBootSetup(&SavedInfo);
    SaveInfo();

    LOG << "Treating as first boot";
    LOG << "Device name " << SavedInfoMirror.sDeviceName;
    LOG << "Network info " << SavedInfoMirror.sNetworkName << " " << SavedInfoMirror.sNetworkPass;
    LOG << "MQTT info " << SavedInfoMirror.sServerName << " " << SavedInfoMirror.sServerPass;

    return true;
#endif
  }

  return false;
}

/*NETWORK SETUP*/
void DisconnectFromAP()
{
  if (connectedState == CONNECTED_TO_AP)
  {
    LOG << "Disconnecting from AP";
    WiFi.disconnect();
    connectedState = DISCONNECTED;
  }
}

void StopAP()
{
  if (connectedState == ACTING_AS_AP)
  {
    LOG << "Stopping AP";
    WiFi.softAPdisconnect(true);
    connectedState = DISCONNECTED;
  }
}

void GenericDisconnect()
{
  switch (connectedState)
  {
    case CONNECTED_TO_AP:
      DisconnectFromAP();
      break;

    case ACTING_AS_AP:
      StopAP();
      break;
  }
}

void ConnectToAP()
{
  if(connectedState != CONNECTED_TO_AP)
  {
    if (strlen(SavedInfo.sNetworkName))
    {
      GenericDisconnect();
      WiFi.mode(WIFI_STA);
  
      if (strlen(SavedInfo.sNetworkPass))
        WiFi.begin(SavedInfo.sNetworkName, SavedInfo.sNetworkPass);
      else
        WiFi.begin(SavedInfo.sNetworkName);
    }
    else
    {
      LOG << "No network name known";
    }
  }
  else
  {
    LOG << "Already connected to AP. Disconnect first";
  }
}

void StartAP()
{
  if(connectedState != ACTING_AS_AP)
  {
    GenericDisconnect();
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(local_IP, gateway, subnet);
    WiFi.softAP(SavedInfo.sDeviceName);
    
    connectedState = ACTING_AS_AP;
  }
}

/*MESSAGE HANDLERS*/
void HandleSetNetworkName(uint8_t* buf)
{
  LOG << "HandleSetNetworkName";
  strcpy(SavedInfo.sNetworkName, (char*)buf);
}

void HandleSetNetworkPass(uint8_t* buf)
{
  LOG << "HandleSetNetworkPass";
  strcpy(SavedInfo.sNetworkPass, (char*)buf);
}

void HandleGetNetworkName(uint8_t* buf)
{
  LOG << "HandleGetNetworkName";
  serInterface.sendCommand(GET_NETWORK_NAME, SavedInfoMirror.sNetworkName, strlen(SavedInfoMirror.sNetworkName));
}

void HandleGetNetworkPass(uint8_t* buf)
{
  LOG << "HandleGetNetworkPass";
  serInterface.sendCommand(GET_NETWORK_PASS, SavedInfoMirror.sNetworkPass, strlen(SavedInfoMirror.sNetworkPass));
}

void HandleGetDeviceName(uint8_t* buf)
{
  LOG << "HandleGetDeviceName";
  serInterface.sendCommand(GET_DEVICE_NAME, SavedInfoMirror.sDeviceName, strlen(SavedInfoMirror.sDeviceName));
}

void HandleConnectToAP(uint8_t* buf)
{
  LOG << "HandleConnectToAP";
  ConnectToAP();
}

void HandleDisconnectFromAP(uint8_t* buf)
{
  LOG << "HandleDisconnectFromAP";
  DisconnectFromAP();
}

void HandleStartAP(uint8_t* buf)
{
  LOG << "HandleStartAP";
  StartAP();
}

void HandleStopAP(uint8_t* buf)
{
  LOG << "HandleStopAP";
  StopAP();
}

void HandleStartNetworkHelper(uint8_t* buf)
{
  LOG << "HandleStartNetworkHelper";
  helper.start();
}

void HandleStopNetworkHelper(uint8_t* buf)
{
  LOG << "HandleStopNetworkHelper";
  helper.stop();
}

void HandleSave(uint8_t* buf)
{
  LOG << "HandleSave";
  SaveInfo();
}

void SetupMessageHandlers()
{
  serInterface.setCommandHandler(SET_NETWORK_NAME, HandleSetNetworkName);
  serInterface.setCommandHandler(SET_NETWORK_PASS, HandleSetNetworkPass);

  serInterface.setCommandHandler(GET_NETWORK_NAME, HandleGetNetworkName);
  serInterface.setCommandHandler(GET_NETWORK_PASS, HandleGetNetworkPass);
  serInterface.setCommandHandler(GET_DEVICE_NAME, HandleGetDeviceName);

  serInterface.setCommandHandler(CONNECT_TO_AP, HandleConnectToAP);
  serInterface.setCommandHandler(DISCONNECT_FROM_AP, HandleDisconnectFromAP);

  serInterface.setCommandHandler(START_AP, HandleStartAP);
  serInterface.setCommandHandler(STOP_AP, HandleStopAP);

  serInterface.setCommandHandler(START_NETWORK_HELPER, HandleStartNetworkHelper);
  serInterface.setCommandHandler(STOP_NETWORK_HELPER, HandleStopNetworkHelper);

  serInterface.setCommandHandler(SAVE, HandleSave);
}

void MonitorConnectionStatus()
{
  if (connectedState != oldConnectedState)
  {
    /*If trying to connect to an AP, monitor the WiFi status*/
    if (connectedState == CONNECTING_TO_AP)
    {
      /*Connected is the easy case*/
      if (WiFi.status() == WL_CONNECTED)
        connectedState = CONNECTED_TO_AP;
      /*Any other status besides idle (no ssid avail, connect failed, disconnected) is considered disconnected*/
      else if (WiFi.status() != WL_IDLE_STATUS)
        connectedState = DISCONNECTED;
    }

    LOG << "Connection status changed to " << connectedState << " from " << oldConnectedState;
    serInterface.sendCommand(NETWORK_STATE_CHANGE, &connectedState, sizeof(connectedState));
    oldConnectedState = connectedState;
  }
}

void setup()
{
  delay(1000);

  EEPROM.begin(sizeof(SAVE_INFO));
  WiFi.persistent(false);

  randomSeed(micros());

  Serial.begin(115200);
  dbg.begin(115200);

  RecoverInfo();

  SetupMessageHandlers();

  helper.onNetworkChange(
    [](String ssid, String password)
  {
    dbg.println("Network change");
  });
}

void loop()
{
  if (Serial.available())
    serInterface.update(Serial.read());

  MonitorConnectionStatus();
  helper.background();
}
