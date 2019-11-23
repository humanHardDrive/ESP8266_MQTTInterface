#include "SerialInterface.h"

SerialInterface::SerialInterface()
{
}

SerialInterface::~SerialInterface() =
  default;

void SerialInterface::update(uint8_t c)
{
  //Reset the parse state if a character hasn't been received
  //in 10ms
  //TODO: remove the magic number 10
  if ((millis() - m_LastRXTime) > 10 &&
      m_ParseState != WAITING_FOR_STX)
    m_ParseState = WAITING_FOR_STX;

  m_LastRXTime = millis();

  //Call the appropriate state handler
  //TODO: bounds check the current parse state
  m_StateFn[m_ParseState](c);
}

void SerialInterface::setCommandHandler(uint8_t cmd, std::function<void(uint8_t*)> fn)
{
  m_CmdHandler[cmd] = fn;
}

void SerialInterface::sendCommand(uint8_t cmd, void* buf, uint8_t len)
{
  MSG_BODY msg;

  if (len > MAX_PAYLOAD_SIZE)
    len = MAX_PAYLOAD_SIZE;

  if (buf && len)
    memcpy(msg.payload, buf, len);
  else
    len = 0;

  msg.cmd = cmd;
  msg.len = len;

  Serial.write(SERIAL_STX);
  Serial.write(msg.cmd);
  Serial.write(msg.len);
  Serial.write(msg.payload, msg.len);
  Serial.write(SERIAL_ETX);
}

void SerialInterface::WaitingForSTXState(uint8_t c)
{
  if (c == SERIAL_STX)
  {
    m_ParseState = WAITING_FOR_CMD;

    //Reset the current command code and the number of
    //bytes received
    m_CurrentCommand =
      m_CommandDataCount = 0;
    memset(m_CurrentCommandBuf, 0, sizeof(m_CurrentCommandBuf));
  }
}

void SerialInterface::WaitingForCMDState(uint8_t c)
{
  m_CurrentCommand = c;
  m_ParseState = WAITING_FOR_LEN;
}

void SerialInterface::WaitingForLenState(uint8_t c)
{
  //TODO: limit this number to the buffer size
  m_CurrentCommandLen = c;

  if (m_CurrentCommandLen)
    m_ParseState = WAITING_FOR_DATA;
  else
    //If there's no data to be sent, just jump to the end state
    m_ParseState = WAITING_FOR_ETX;
}

void SerialInterface::WaitingForDataState(uint8_t c)
{
  //Add data to the buffer
  m_CurrentCommandBuf[m_CommandDataCount] = c;
  m_CommandDataCount++;

  if (m_CommandDataCount >= m_CurrentCommandLen)
    m_ParseState = WAITING_FOR_ETX;
}

void SerialInterface::WaitingForETXState(uint8_t c)
{
  if (c == SERIAL_ETX)
  {
    m_ParseState = WAITING_FOR_STX;

    //If a handler exists for this command code, call it
    if (m_CmdHandler.find(m_CurrentCommand) != m_CmdHandler.end())
      m_CmdHandler[m_CurrentCommand](m_CurrentCommandBuf);
    else if (m_CmdHandler.find(INVALID_CMD) != m_CmdHandler.end())
      m_CmdHandler[INVALID_CMD](m_CurrentCommandBuf);
  }
  else if (c == SERIAL_STX)
  {
    WaitingForSTXState(c);

    //If a handler exists for this command code, call it
    if (m_CmdHandler.find(m_CurrentCommand) != m_CmdHandler.end())
      m_CmdHandler[m_CurrentCommand](m_CurrentCommandBuf);
    else if (m_CmdHandler.find(INVALID_CMD) != m_CmdHandler.end())
      m_CmdHandler[INVALID_CMD](m_CurrentCommandBuf);
  }
}
