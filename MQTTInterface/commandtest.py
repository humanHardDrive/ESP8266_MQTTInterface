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
	"getConnectionState" : 0x0f
}

parser = argparse.ArgumentParser(description='MQTT Interface Command Test')
parser.add_argument("--port", required=True, type=str, help="Serial port name")
parser.add_argument("--baud", required=True, type=int, help="Serial port baud rate")
parser.add_argument("--cmd", required=True, type=str, help="Command options", choices=list(cmdMap))

args = parser.parse_args()

comPort = serial.Serial()
comPort.port = args.port
comPort.baudrate = args.baud
comPort.timeout = 0.1

try:
	comPort.open()
except:
	print("Failed to open %s" %(args.port))
	exit(1)
	
msg = [0x55, 0x00, 0x00, 0xaa]
msg[1]= cmdMap[args.cmd]

bMsg = bytes(msg)
comPort.write(msg)
print(comPort.read(256))
	
comPort.close()