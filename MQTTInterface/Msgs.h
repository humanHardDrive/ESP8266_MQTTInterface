#ifndef __MSGS_H__
#define __MSGS_H__

#include <cstdint>

#define MAX_PAYLOAD_SIZE  128

struct MSG_BODY
{
  uint8_t cmd;
  uint8_t len;
  uint8_t payload[MAX_PAYLOAD_SIZE];
};

#define MSG_BODY_SIZE   sizeof(MSG_BODY)

enum CMD_TYPE
{
  NO_CMD = 0,
  INVALID_CMD
};

#endif
