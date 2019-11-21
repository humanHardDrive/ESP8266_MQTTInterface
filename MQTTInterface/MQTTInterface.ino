#include <SoftwareSerial.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <Streaming.h>
#include <ESP8266WiFi.h>

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
uint32_t nConnectionAttemptStart = 0;
uint8_t networkState = DISCONNECTED, oldNetworkState = UNKNOWN_STATE;
char sDeviceName[MAX_DEVICE_NAME_LENGTH];
char sHexMap[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

/*ACCESS POINT CONFIGURATION*/
IPAddress local_IP(192, 168, 1, 1);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

/*INFO FUNCTIONS*/
void buildDeviceName(char* sName)
{
  uint32_t nID = ESP.getChipId();
  char sID[MAX_DEVICE_NAME_LENGTH];
  strcpy(sName, DEVICE_NAME_BASE);

  memset(sID, 0, MAX_DEVICE_NAME_LENGTH);
  for (uint8_t i = 0; i < 6; i++)
  {
    sID[5 - i] = sHexMap[nID & 0x0F];
    nID >>= 4;
  }

  strcat(sName, sID);
}

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
  memset(info->sNetworkName, 0, MAX_NETWORK_NAME_LENGTH);
  memset(info->sNetworkPass, 0, MAX_NETWORK_NAME_LENGTH);
  memset(info->sServerName, 0, MAX_NETWORK_NAME_LENGTH);
  memset(info->sServerPass, 0, MAX_NETWORK_NAME_LENGTH);
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
    LOG << "Device name " << sDeviceName;
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
    LOG << "Device name " << sDeviceName;
    LOG << "Network info " << SavedInfoMirror.sNetworkName << " " << SavedInfoMirror.sNetworkPass;
    LOG << "MQTT info " << SavedInfoMirror.sServerName << " " << SavedInfoMirror.sServerPass;

    return true;
#endif
  }

  return false;
}

void reboot()
{
  GenericDisconnect();
  helper.stop();

  EEPROM.end();

  delay(500);

  while(1);
}

/*NETWORK SETUP*/
bool WaitForModeChange(WiFiMode mode, uint32_t timeout)
{
  WiFi.mode(mode);
  uint32_t startTime = millis();

  while ((millis() - startTime) < timeout && WiFi.getMode() != mode);

  if (WiFi.getMode() == mode)
    return true;

  return false;
}

void DisconnectFromAP()
{
  if (networkState == CONNECTED_TO_AP)
  {
    LOG << "Disconnecting from AP";
    WiFi.disconnect();
    networkState = DISCONNECTED;
  }
}

void StopAP()
{
  if (networkState == ACTING_AS_AP)
  {
    LOG << "Stopping AP";
    WiFi.softAPdisconnect(true);
    networkState = DISCONNECTED;
  }
}

void GenericDisconnect()
{
  switch (networkState)
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
  if (networkState != CONNECTED_TO_AP)
  {
    if (strlen(SavedInfo.sNetworkName))
    {
      GenericDisconnect();
      if (WaitForModeChange(WIFI_STA, 100))
      {
        if (strlen(SavedInfo.sNetworkPass))
          WiFi.begin(SavedInfo.sNetworkName, SavedInfo.sNetworkPass);
        else
          WiFi.begin(SavedInfo.sNetworkName);

        networkState = CONNECTING_TO_AP;
        nConnectionAttemptStart = millis();
      }
      else
      {
        LOG << "Unable to switch to STA mode";
      }
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
  if (networkState != ACTING_AS_AP)
  {
    GenericDisconnect();

    if (WaitForModeChange(WIFI_AP, 100))
    {
      WiFi.softAPConfig(local_IP, gateway, subnet);
      WiFi.softAP(sDeviceName);

      networkState = ACTING_AS_AP;
    }
  }
  else
  {
    LOG << "Already acting as AP";
  }
}

/*MQTT SERVER FUNCTIONS*/
void DisconnectFromServer()
{
  
}

void ConnectToServer()
{
  
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

/*These should return the current network name, saved in flash or not*/
void HandleGetNetworkName(uint8_t* buf)
{
  LOG << "HandleGetNetworkName";
  serInterface.sendCommand(GET_NETWORK_NAME, SavedInfo.sNetworkName, strlen(SavedInfo.sNetworkName));
}

void HandleGetNetworkPass(uint8_t* buf)
{
  LOG << "HandleGetNetworkPass";
  serInterface.sendCommand(GET_NETWORK_PASS, SavedInfo.sNetworkPass, strlen(SavedInfo.sNetworkPass));
}

void HandleGetDeviceName(uint8_t* buf)
{
  LOG << "HandleGetDeviceName";
  serInterface.sendCommand(GET_DEVICE_NAME, sDeviceName, strlen(sDeviceName));
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

void HandleGetConnectionState(uint8_t* buf)
{
  LOG << "HandleGetConnectionState";
  serInterface.sendCommand(GET_CONNECTION_STATE, &networkState, sizeof(networkState));
}

void HandleReboot(uint8_t* buf)
{
  LOG << "HandleReboot";
  reboot();
}

void HandleSetServerName(uint8_t* buf)
{
  LOG << "HandleSetServerName";
}

void HandleSetServerPass(uint8_t* buf)
{
  LOG << "HandleSetServerPass";
}

void HandleGetServerName(uint8_t* buf)
{
  LOG << "HandleGetServerName";
  serInterface.sendCommand(GET_SERVER_NAME, SavedInfo.sServerName, strlen(SavedInfo.sServerName));
}

void HandleGetServerPass(uint8_t* buf)
{
  LOG << "HandleGetServerPass";
  serInterface.sendCommand(GET_SERVER_PASS, SavedInfo.sServerPass, strlen(SavedInfo.sServerPass));
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

  serInterface.setCommandHandler(GET_CONNECTION_STATE, HandleGetConnectionState);

  serInterface.setCommandHandler(REBOOT, HandleReboot);
}

void MonitorConnectionStatus()
{
  /*If trying to connect to an AP, monitor the WiFi status*/
  if (networkState == CONNECTING_TO_AP)
  {
    /*Connected is the easy case*/
    if (WiFi.status() == WL_CONNECTED)
      networkState = CONNECTED_TO_AP;
    /*Any other status besides idle (no ssid avail, connect failed) is considered disconnected*/
    else if (WiFi.status() == WL_NO_SSID_AVAIL)
    {
      LOG << "SSID not available";
      networkState = DISCONNECTED;
    }
    else if(WiFi.status() == WL_CONNECT_FAILED)
    {
      LOG << "Incorrect password";
      networkState = DISCONNECTED;
    }
    else if ((millis() - nConnectionAttemptStart) > 5000)
    {
      LOG << "Connection timeout";
      networkState = DISCONNECTED;
    }
  }

  /*Only on change*/
  if (networkState != oldNetworkState)
  {
    if (networkState == DISCONNECTED)
      WiFi.mode(WIFI_OFF);

    LOG << "Connection status changed to " << networkState << " from " << oldNetworkState;
    serInterface.sendCommand(NETWORK_STATE_CHANGE, &networkState, sizeof(networkState));
    oldNetworkState = networkState;
  }
}

void UpdateNetworkInfo(String sNetworkName, String sNetworkPass)
{
  strcpy(SavedInfo.sNetworkName, sNetworkName.c_str());
  strcpy(SavedInfo.sNetworkPass, sNetworkPass.c_str());

  LOG << "Network Change " << sNetworkName << " " << sNetworkPass;
  serInterface.sendCommand(NETWORK_CHANGE, SavedInfo.sNetworkName, sNetworkName.length());
}

void setup()
{
  delay(1000);

  EEPROM.begin(sizeof(SAVE_INFO));
  WiFi.persistent(false);

  randomSeed(micros());

  Serial.begin(115200);
  dbg.begin(115200);

  buildDeviceName(sDeviceName);
  RecoverInfo();

  SetupMessageHandlers();

  helper.onNetworkChange(
    [](String ssid, String password)
  {
    UpdateNetworkInfo(ssid, password);
  });
}

void loop()
{
  if (Serial.available())
    serInterface.update(Serial.read());

  MonitorConnectionStatus();
  helper.background();
}
