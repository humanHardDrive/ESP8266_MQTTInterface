import argparse
import serial

cmdOptions = ["getDeviceName", "getNetworkName", "getNetworkPass", "startNetworkHelper", "stopNetworkHelper", "save"]

parser = argparse.ArgumentParser(description='MQTT Interface Command Test')
parser.add_argument("--port", required=True, type=str, help="Serial port name")
parser.add_argument("--baud", required=True, type=int, help="Serial port baud rate")
parser.add_argument("--cmd", required=True, type=str, help="Command options", choices=cmdOptions)

args = parser.parse_args()

comPort = serial.Serial()
comPort.port = args.port
comPort.baudrate = args.baud
comPort.timeout = 1

try:
	comPort.open()
except:
	print("Failed to open %s" %(args.port))
	exit(1)
	
msg = [0x55, 0x00, 0x00, 0xaa]

#Need a map equivalent
if args.cmd == "getDeviceName":
	msg[1] = 0x05
elif args.cmd == "getNetworkName":
	msg[1] = 0x03
elif args.cmd == "getNetworkPass":
	msg[1] = 0x04
elif args.cmd == "startNetworkHelper":
	msg[1] = 0x0a
elif args.cmd == "stopNetworkHelper":
	msg[1] = 0x0b
elif args.cmd == "save":
	msg[1] = 0x0e

bMsg = bytes(msg)
comPort.write(msg)
print(comPort.read(128))
	
comPort.close()