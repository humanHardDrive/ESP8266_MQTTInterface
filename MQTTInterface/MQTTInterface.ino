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

#define MAX_DEVICE_NAME_LENGTH    16
#define MAX_PATH_LENGTH           32
#define MAX_SUBS                  8
#define MAX_PUBS                  8

#define MEM_DEVICE_SIZE           1024

/*Send log statements out of the programming port*/
//#define PROG_DBG

/*Root of the device name. The chip ID will be appended to this*/
#define DEVICE_NAME_BASE          "mqttDev-"

#define STATUS_PIN  4
#define DBG_TX_PIN  12
#define DBG_RX_PIN  14

/*GLOBALS*/
SerialInterface serInterface;
SoftwareSerial dbg(DBG_RX_PIN, DBG_TX_PIN, false);
#ifdef PROG_DBG
Stream& logger(Serial);
#else
Stream& logger(dbg);
#endif

/*Memory Buffer*/
uint8_t memBuffer[MEM_DEVICE_SIZE];

/*Access Point Info*/
char sNetworkName[MAX_NETWORK_NAME_LENGTH];
char sNetworkPass[MAX_NETWORK_NAME_LENGTH];

/*MQTT Server Info*/
char sServerAddr[MAX_NETWORK_NAME_LENGTH];
uint16_t nServerPort;
char sUserName[MAX_NETWORK_NAME_LENGTH];
char sUserPass[MAX_NETWORK_NAME_LENGTH];

char sSubList[MAX_SUBS][MAX_PATH_LENGTH];
/*Create a pointer list for the sub list*/
char* pSubListWrapper[MAX_SUBS];
/*Create memory block for the sub alias*/
char sSubAlias[MAX_SUBS][MAX_PATH_LENGTH];
/*Create a pointer list for the sub alias list*/
char* pSubAliasWrapper[MAX_SUBS];

/*Do the same thing for publications*/
char sPubList[MAX_SUBS][MAX_PATH_LENGTH];
char* pPubListWrapper[MAX_PUBS];
char sPubAlias[MAX_PUBS][MAX_PATH_LENGTH];
char* pPubAliasWrapper[MAX_PUBS];

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

void reboot()
{
  GenericDisconnect();
  helper.stop();

  EEPROM.end();

  delay(500);

  while (1);
}

void strcpy_s(char* dst, char* src, size_t count)
{
  while (*src && count)
  {
    *dst = *src;

    src++;
    dst++;
    count--;
  }

  *dst = '\0';
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
    if (sNetworkName[0])
    {
      GenericDisconnect();
      if (WaitForModeChange(WIFI_STA, 100))
      {
        if (sNetworkPass[0])
          WiFi.begin(sNetworkName, sNetworkPass);
        else
          WiFi.begin(sNetworkName);

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
    if (sServerAddr[0] && nServerPort)
    {
      mqttClient.setServer(sServerAddr, nServerPort);

      if (sUserName[0])
        mqttClient.connect(sDeviceName, sUserName, sUserPass);
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
  if (networkState == DISCONNECTED ||
      networkState == CONNECTED_TO_AP ||
      networkState == ACTING_AS_AP)
    helper.start();
}

void HandleStopNetworkHelper(uint8_t* buf)
{
  LOG << "HandleStopNetworkHelper";
  helper.stop();
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

void HandleSetSubAlias(uint8_t* buf)
{
  LOG << "HandleSetSubAlias";

  uint8_t nIndex = buf[0];
  if (nIndex < MAX_SUBS)
  {
    memset(sSubList[nIndex], 0, MAX_PATH_LENGTH);
    strcpy_s(sSubList[nIndex], (char*)&buf[1], MAX_PATH_LENGTH - 1);

    LOG << nIndex << " " << sSubList[nIndex];
  }
  else
    LOG << "Invalid index " << nIndex;
}

void HandleSetPubAlias(uint8_t* buf)
{
  LOG << "HandleSetPubAlias";

  uint8_t nIndex = buf[0];
  if (nIndex < MAX_PUBS)
  {
    memset(sPubList[nIndex], 0, MAX_PATH_LENGTH);
    strcpy_s(sPubList[nIndex], (char*)&buf[1], MAX_PATH_LENGTH - 1);

    LOG << nIndex << " " << sPubList[nIndex];
  }
  else
    LOG << "Invalid index " << nIndex;
}

void HandleClearPubList(uint8_t* buf)
{
  LOG << "HandleClearPubList";

  memset(sPubList, 0, sizeof(sPubList));
  memset(sPubAlias, 0, sizeof(sPubAlias));
}

void HandleClearSubList(uint8_t* buf)
{
  LOG << "HandleClearSubList";

  memset(sSubList, 0, sizeof(sSubList));
  memset(sSubAlias, 0, sizeof(sSubAlias));
}

void HandlePubInfo(uint8_t* buf)
{
  if (serverState == CONNECTED_TO_AP)
  {

  }
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

  serInterface.setCommandHandler(GET_CONNECTION_INFO, HandleGetConnectionState);

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

  serInterface.setCommandHandler(GET_IP, HandleGetIP);

  serInterface.setCommandHandler(CLEAR_SUB_LIST, HandleClearSubList);
  serInterface.setCommandHandler(SET_SUB_ALIAS, HandleSetSubAlias);
  serInterface.setCommandHandler(CLEAR_PUB_LIST, HandleClearPubList);
  serInterface.setCommandHandler(SET_PUB_ALIAS, HandleSetPubAlias);

  serInterface.setCommandHandler(PUB_INFO, HandlePubInfo);
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
    else if ((millis() - nConnectionAttemptStart) > 10000)
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
      nLastTimeRequest = millis();
    }
  }

  /*Only on change*/
  if (networkState != oldNetworkState)
  {
    LOG << "Connection status changed from " << oldNetworkState << " to " << networkState;
    serInterface.sendCommand(NETWORK_STATE_CHANGE, &networkState, sizeof(networkState));
    oldNetworkState = networkState;

    if (networkState == DISCONNECTED)
    {
      /*Diconnect the server without a WiFi connection*/
      serverState = DISCONNECTED;
      bConnectedToInternet = false;
      WiFi.mode(WIFI_OFF);
    }

    if (networkState == CONNECTED_TO_AP)
    {
      LOG << "IP Address " << WiFi.localIP();
    }
  }
}

void UpdateNetworkInfo(String sNetworkName, String sNetworkPass)
{
  /*Send notification that the network info has been updated*/
}

void UpdateServerInfo(String sServerAddr, uint16_t nServerPort, String sUserName, String sUserPass)
{
  /*Send notification that the server info has been updated*/
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
    else
      mqttClient.loop();
  }

  if (serverState != oldServerState)
  {
    if (serverState == DISCONNECTED)
      mqttClient.disconnect();

    if (serverState == CONNECTED_TO_AP)
    {
      for (unsigned int i = 0; i < MAX_SUBS; i++)
      {
        if (sSubList[i][0])
        {
          if (mqttClient.subscribe(sSubList[i]))
            LOG << "Subscribed to " << sSubList[i];
          else
            LOG << "Failed to subscribe to " << sSubList[i];
        }
      }
    }

    LOG << "Server connection status changed from " << oldServerState << " to " << serverState;
    serInterface.sendCommand(MQTT_STATE_CHANGE, &serverState, sizeof(serverState));
    oldServerState = serverState;
  }
}

void UpdateSubscription(uint8_t nIndex, String sPub)
{
  if (nIndex < MAX_SUBS)
  {
    LOG << "Subscription " << nIndex << " changed from " <<
        sSubList[nIndex] << " to " << sPub;

    if (mqttClient.unsubscribe(sSubList[nIndex]))
      LOG << "Unsubscribed";
    else
      LOG << "Failed to unsubscribe";

    strcpy_s(sSubList[nIndex], (char*)sPub.c_str(), MAX_PATH_LENGTH);

    if (mqttClient.subscribe(sSubList[nIndex]))
      LOG << "Subscribed";
    else
      LOG << "Failed to subscribe";
  }
  else
    LOG << "Invalid subscription change " << nIndex;
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

  SetupMessageHandlers();

  timeClient.begin();

  helper.setSubList(pSubListWrapper);
  helper.setSubAliasList(pSubAliasWrapper);
  helper.setSubCount(MAX_SUBS);

  helper.setPubList(pPubListWrapper);
  helper.setPubAliasList(pPubAliasWrapper);
  helper.setPubCount(MAX_PUBS);

  helper.onNetworkChange(
    [](String ssid, String password)
  {
    UpdateNetworkInfo(ssid, password);
  });

  helper.onServerChange(
    [](String addr, uint16_t port, String user, String pass)
  {
    UpdateServerInfo(addr, port, user, pass);
  });

  helper.onSubChange(
    [](uint8_t index, String sub)
  {
    UpdateSubscription(index, sub);
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
