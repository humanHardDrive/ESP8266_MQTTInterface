#include <SoftwareSerial.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <Streaming.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

#define VERSION_MAJOR   0
#define VERSION_MINOR   0

#include "SerialInterface.h"
#include "NetworkHelper.h"

/*Basic logging macro. The ESP8266 library seems to have a problem
   adding a __FUNCTION__ call this for some reason
*/
#define LOG logger << "\n\r" << millis() << '\t'

#define _REIFB    //Recovery Error Is First Boot
/*Send log statements out of the programming port*/
//#define PROG_DBG

/*Root of the device name. The chip ID will be appended to this*/
#define DEVICE_NAME_BASE          "mqttDev-"

#define MAX_NETWORK_NAME_LENGTH   32
#define MAX_DEVICE_NAME_LENGTH    16
#define MAX_SUB_PATH_LENGTH       64
#define MAX_SUBS                  16

#define STATUS_PIN  4
#define DBG_TX_PIN  12
#define DBG_RX_PIN  14

#define TIME_UPDATE_PERIOD  60000

struct SAVE_INFO
{
  /*Access Point Info*/
  char sNetworkName[MAX_NETWORK_NAME_LENGTH];
  char sNetworkPass[MAX_NETWORK_NAME_LENGTH];

  /*MQTT Server Info*/
  char sServerAddr[MAX_NETWORK_NAME_LENGTH];
  uint16_t nServerPort;
  char sUserName[MAX_NETWORK_NAME_LENGTH];
  char sUserPass[MAX_NETWORK_NAME_LENGTH];

  /*MQTT Subscription List*/
  char sSubList[MAX_SUBS][MAX_SUB_PATH_LENGTH];

  uint32_t checksum;
};

/*GLOBALS*/
SAVE_INFO SavedInfo, SavedInfoMirror;
SerialInterface serInterface;
SoftwareSerial dbg(DBG_RX_PIN, DBG_TX_PIN, false);
#ifdef PROG_DBG
Stream& logger(Serial);
#else
Stream& logger(dbg);
#endif

NetworkHelper helper;
uint32_t nConnectionAttemptStart = 0;
/*Connection states are monitored using these variables
   This keeps all of the logging and notification in one centralized place
   instead of trying to capture it in each method
*/
uint8_t networkState = DISCONNECTED, oldNetworkState = UNKNOWN_STATE;
/*The MQTT server status uses the same enum but is only ever disconnected, connecting, or connected*/
uint8_t serverState = DISCONNECTED, oldServerState = UNKNOWN_STATE;
char sDeviceName[MAX_DEVICE_NAME_LENGTH];
const char sHexMap[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
WiFiClient wifiClient;
WiFiUDP ntpUDP;
PubSubClient mqttClient(wifiClient);
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0);
uint32_t nLastTimeRequest = 0;
bool bConnectedToInternet = false;

/*ACCESS POINT CONFIGURATION*/
IPAddress local_IP(192, 168, 1, 1);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

/*INFO FUNCTIONS*/
void buildDeviceName(char* sName)
{
  /*Use the chip ID to build a unique identifier*/
  /*Makes sense to use the one built in instead of creating a new one*/
  uint32_t nID = ESP.getChipId();
  char sID[MAX_DEVICE_NAME_LENGTH];
  strcpy(sName, DEVICE_NAME_BASE);

  memset(sID, 0, MAX_DEVICE_NAME_LENGTH);
  /*The chip ID is 3 bytes, so 6 hex characters*/
  for (uint8_t i = 0; i < 6; i++)
  {
    /*Reverse the bit order*/
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
  /*Default all connection info*/
  memset(info, 0, sizeof(SAVE_INFO));
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
    LOG << "MQTT info " << SavedInfoMirror.sServerAddr << " " << SavedInfoMirror.sUserName << " " << SavedInfoMirror.sUserPass;

    LOG << "Subscription list: ";
    for(unsigned int i = 0; i < MAX_SUBS; i++)
    {
      if(SavedInfoMirror.sSubList[i][0])
        LOG << SavedInfoMirror.sSubList[i];
    }

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

  while (1);
}

/*NETWORK SETUP*/
/*Changing the WiFi mode actually takes some amount of time
   This method changes the WiFi mode and waits for it to complete, with a timeout
   This is most useful for AP mode where if the mode isn't set before trying to start
   the AP, the SSID is garbage
*/
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
    if (SavedInfo.sNetworkName[0])
    {
      GenericDisconnect();
      if (WaitForModeChange(WIFI_STA, 100))
      {
        if (SavedInfo.sNetworkPass[0])
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
      /*Use the device name as the AP SSID*/
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
  if (serverState == CONNECTED_TO_AP)
    serverState = DISCONNECTED;
}

void ConnectToServer()
{
  if ((networkState == CONNECTED_TO_AP) && serverState == DISCONNECTED)
  {
    if (SavedInfo.sServerAddr[0] && SavedInfo.nServerPort)
    {
      mqttClient.setServer(SavedInfo.sServerAddr, SavedInfo.nServerPort);

      if (SavedInfo.sUserName[0])
        mqttClient.connect(sDeviceName, SavedInfo.sUserName, SavedInfo.sUserPass);
      else
        mqttClient.connect(sDeviceName);

      serverState = CONNECTING_TO_AP;
    }
    else
    {
      LOG << "No server or port defined";
    }
  }
  else if (networkState != CONNECTED_TO_AP)
  {
    LOG << "Not connected to AP " << networkState;
  }
  else if (serverState != DISCONNECTED)
  {
    LOG << "Not disconnected from server " << serverState;
  }
}

/*MESSAGE HANDLERS*/
void HandleSetNetworkName(uint8_t* buf)
{
  strcpy(SavedInfo.sNetworkName, (char*)buf);
  LOG << "HandleSetNetworkName" << " " << SavedInfo.sNetworkName;
}

void HandleSetNetworkPass(uint8_t* buf)
{
  strcpy(SavedInfo.sNetworkPass, (char*)buf);
  LOG << "HandleSetNetworkPass" << " " << SavedInfo.sNetworkPass;
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

void HandleSetServerAddr(uint8_t* buf)
{
  strcpy(SavedInfo.sServerAddr, (char*)buf);
  LOG << "HandleSetServerName" << " " << SavedInfo.sServerAddr;
}

void HandleSetServerPort(uint8_t* buf)
{
  memcpy(&SavedInfo.nServerPort, buf, sizeof(SavedInfo.nServerPort));
  LOG << "HandleSetServerPort" << " " << SavedInfo.nServerPort;
}

void HandleSetUserName(uint8_t* buf)
{
  strcpy(SavedInfo.sUserName, (char*)buf);
  LOG << "HandleSetUserName" << " " << SavedInfo.sUserName;
}

void HandleSetUserPass(uint8_t* buf)
{
  strcpy(SavedInfo.sUserPass, (char*)buf);
  LOG << "HandleSetUserPass" << " " << SavedInfo.sUserPass;
}

void HandleGetServerAddr(uint8_t* buf)
{
  LOG << "HandleGetServerName";
  serInterface.sendCommand(GET_SERVER_ADDR, SavedInfo.sServerAddr, strlen(SavedInfo.sServerAddr));
}

void HandleGetServerPort(uint8_t* buf)
{
  LOG << "HandleGetServerPort";
  serInterface.sendCommand(GET_SERVER_PORT, &SavedInfo.nServerPort, sizeof(SavedInfo.nServerPort));
}

void HandleGetUserName(uint8_t* buf)
{
  LOG << "HandleGetUserName";
  serInterface.sendCommand(GET_USER_NAME, SavedInfo.sUserName, strlen(SavedInfo.sUserName));
}

void HandleGetUserPass(uint8_t* buf)
{
  LOG << "HandleGetUserPass";
  serInterface.sendCommand(GET_USER_PASS, SavedInfo.sUserPass, strlen(SavedInfo.sUserPass));
}

void HandleConnectToServer(uint8_t* buf)
{
  LOG << "HandleConnectToServer";
  ConnectToServer();
}

void HandleDisconnectFromServer(uint8_t* buf)
{
  LOG << "HandleDisconnectFromServer";
  DisconnectFromServer();
}

void HandleVersion(uint8_t* buf)
{
  uint16_t verBuf[2];
  LOG << "HandleVersion";

  verBuf[0] = VERSION_MAJOR;
  verBuf[1] = VERSION_MINOR;
  serInterface.sendCommand(VERSION, verBuf, sizeof(verBuf));
}

void HandleTime(uint8_t* buf)
{
  uint32_t currentTime;
  LOG << "HandleTime";

  if (bConnectedToInternet)
    currentTime = timeClient.getEpochTime();
  else
    currentTime = 0;

  serInterface.sendCommand(TIME, &currentTime, sizeof(currentTime));
}

void HandleSetTimeOffset(uint8_t* buf)
{
  int32_t nTimeOffset = *((int32_t*)buf);
  LOG << "HandleSetTimeOffset";

  timeClient.setTimeOffset(nTimeOffset);
}

void HandleGetIP(uint8_t* buf)
{
  uint32_t byteIP = WiFi.localIP();
  LOG << "HandleGetIP";

  serInterface.sendCommand(GET_IP, &byteIP, sizeof(byteIP));
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

  serInterface.setCommandHandler(SET_SERVER_ADDR, HandleSetServerAddr);
  serInterface.setCommandHandler(SET_SERVER_PORT, HandleSetServerPort);
  serInterface.setCommandHandler(SET_USER_NAME, HandleSetUserName);
  serInterface.setCommandHandler(SET_USER_PASS, HandleSetUserPass);

  serInterface.setCommandHandler(GET_SERVER_ADDR, HandleGetServerAddr);
  serInterface.setCommandHandler(GET_SERVER_PORT, HandleGetServerPort);
  serInterface.setCommandHandler(GET_USER_NAME, HandleGetUserName);
  serInterface.setCommandHandler(GET_USER_PASS, HandleGetUserPass);

  serInterface.setCommandHandler(CONNECT_TO_SERVER, HandleConnectToServer);
  serInterface.setCommandHandler(DISCONNECT_FROM_SERVER, HandleDisconnectFromServer);

  serInterface.setCommandHandler(VERSION, HandleVersion);

  serInterface.setCommandHandler(TIME, HandleTime);
  serInterface.setCommandHandler(SET_TIME_OFFSET, HandleSetTimeOffset);
}

void MonitorNetworkStatus()
{
  /*If trying to connect to an AP, monitor the WiFi status*/
  if (networkState == CONNECTING_TO_AP)
  {
    /*Connected is the easy case*/
    if (WiFi.status() == WL_CONNECTED)
    {
      networkState = CONNECTED_TO_AP;

      if (timeClient.forceUpdate())
      {
        bConnectedToInternet = true;
        networkState = CONNECTED_TO_AP;
        nLastTimeRequest = millis();
      }
    }
    /*Any other status besides idle (no ssid avail, connect failed) is considered disconnected*/
    else if (WiFi.status() == WL_NO_SSID_AVAIL)
    {
      LOG << "SSID not available";
      networkState = DISCONNECTED;
    }
    else if (WiFi.status() == WL_CONNECT_FAILED)
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

  /*Do stuff when connected to AP*/
  if (networkState == CONNECTED_TO_AP)
  {
    if ((millis() - nLastTimeRequest) > TIME_UPDATE_PERIOD)
    {
      if (!timeClient.update())
      {
        bConnectedToInternet = false;
        LOG << "Couldn't get time from server";
      }
    }
  }

  /*Only on change*/
  if (networkState != oldNetworkState)
  {
    if (networkState == DISCONNECTED)
    {
      /*Diconnect the server without a WiFi connection*/
      serverState = DISCONNECTED;
      bConnectedToInternet = false;
      WiFi.mode(WIFI_OFF);
    }

    LOG << "Connection status changed from " << oldNetworkState << " to " << networkState;
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

void MonitorServerConnection()
{
  if (serverState == CONNECTING_TO_AP)
  {
    if (mqttClient.connected())
      serverState = CONNECTED_TO_AP;
  }
  else if (serverState == CONNECTED_TO_AP)
  {
    if (!mqttClient.connected())
      serverState = DISCONNECTED;
  }

  if (serverState != oldServerState)
  {
    if (serverState == DISCONNECTED)
      mqttClient.disconnect();

    if(serverState == CONNECTED_TO_AP)
    {
      for(unsigned int i = 0; i < MAX_SUBS; i++)
      {
        if(SavedInfo.sSubList[i][0])
          mqttClient.subscribe(SavedInfo.sSubList[i]);
      }
    }

    LOG << "Server connection status changed from " << oldServerState << " to " << serverState;
    serInterface.sendCommand(MQTT_STATE_CHANGE, &serverState, sizeof(serverState));
    oldServerState = serverState;
  }
}

void setup()
{
  /*Setup pins*/
  pinMode(STATUS_PIN, OUTPUT);
  digitalWrite(STATUS_PIN, HIGH);

  delay(1000);

  /*Startup EEPROM*/
  EEPROM.begin(sizeof(SAVE_INFO));
  /*Disable persistant WiFi info. The code handles that*/
  WiFi.persistent(false);

  /*Setup serial*/
  Serial.begin(57600);
  dbg.begin(57600);

  buildDeviceName(sDeviceName);
  RecoverInfo();

  SetupMessageHandlers();

  timeClient.begin();

  helper.onNetworkChange(
    [](String ssid, String password)
  {
    UpdateNetworkInfo(ssid, password);
  });

  digitalWrite(STATUS_PIN, LOW);
}

void loop()
{
  if (Serial.available())
    serInterface.update(Serial.read());

  MonitorNetworkStatus();
  MonitorServerConnection();

  helper.background();
}
