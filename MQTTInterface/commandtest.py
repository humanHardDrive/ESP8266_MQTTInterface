import argparse
import serial

cmdMap = {
	"getNetworkName" : 0x03,
	"getNetworkPass": 0x04,
	"getDeviceName" : 0x05,
	"connectToAP" : 0x06,
	"disconnectFromAP" : 0x07,
	"startAP" : 0x08,
	"stopAP" : 0x09,
	"startNetworkHelper" : 0x0a,
	"stopNetworkHelper" : 0x0b,
	"save" : 0x0e,
	"getConnectionState" : 0x0f,
	"reboot" : 0x10,
	"getServerAddr" : 0x15,
	"getServerPort" : 0x16,
	"getUserName" : 0x17,
	"getUserPass" : 0x18,
	"connectToServer" : 0x19,
	"disconnectFromServer" : 0x1a
}

parser = argparse.ArgumentParser(description='MQTT Interface Command Test')
parser.add_argument("--port", required=True, type=str, help="Serial port name")
parser.add_argument("--baud", required=True, type=int, help="Serial port baud rate")
parser.add_argument("--cmd", required=True, type=str, help="Command", choices=list(cmdMap))
parser.add_argument("--data", required=False, type=str, help="Msg Payload", nargs='*')

args = parser.parse_args()

comPort = serial.Serial()
comPort.port = args.port
comPort.baudrate = args.baud
comPort.timeout = 0.1

payload = args.data

try:
	comPort.open()
except:
	print("Failed to open %s" %(args.port))
	exit(1)
	
msg = []
msg.append(0x55) #STX
msg.append(cmdMap[args.cmd]) #Command
if(payload == None):
	msg.append(0)
else:
	msg.append(len(payload[0]))
	for c in payload[0]:
		msg.append(ord(c))
		
msg.append(0xaa) #ETX

print(msg)

bMsg = bytes(msg)
comPort.write(msg)
print(comPort.read(256))
	
comPort.close()