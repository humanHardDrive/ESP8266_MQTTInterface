#include <SoftwareSerial.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <Streaming.h>

#include "SerialInterface.h"
#include "NetworkHelper.h"

#define LOG logger << '\n' << millis() << '\t'

#define _REIFB    //Recovery Error Is First Boot

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
Stream& logger(dbg);
NetworkHelper helper;

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
    EEPROM.put(0, SavedInfo);
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
    firstBootSetup(&SavedInfoMirror);

    LOG << "Treating as first boot";
    LOG << "Device name " << SavedInfoMirror.sDeviceName;
    LOG << "Network info " << SavedInfoMirror.sNetworkName << " " << SavedInfoMirror.sNetworkPass;
    LOG << "MQTT info " << SavedInfoMirror.sServerName << " " << SavedInfoMirror.sServerPass;

    memcpy(&SavedInfo, &SavedInfoMirror, sizeof(SAVE_INFO));

    return true;
#endif
  }

  return false;
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

  helper.background();
}
