#   ____  _                       _            _   
#  / ___|| |_ _ __ ___  ___ ___  | |_ ___  ___| |_ 
#  \___ \| __| '__/ _ \/ __/ __| | __/ _ \/ __| __|
#   ___) | |_| | |  __/\__ \__ \ | ||  __/\__ \ |_ 
#  |____/ \__|_|  \___||___/___/  \__\___||___/\__|
#                                                
# Usage: python3 stress.py path/to/bridge [-t topic]
# where the topic is optional (default to "bridge")
# NOTE: this version popens the bridge command: thus DO NOT USE THE PIPE!
import json
import time
import socket
import subprocess
import sys
import signal

running = True

# Signal handler to catch Ctrl-C
def signal_handler(sig, frame):
  global running
  running = False

# Install signal handler
signal.signal(signal.SIGINT, signal_handler)

# Get array of command line arguments: eg. ["path/to/bridge", "-t", "topic"]
command = sys.argv[1:]

# open the bridge as a subprocess
if len(sys.argv) > 1:
  process = subprocess.Popen(command, stdin=subprocess.PIPE)
else:
  print("Usage: python3 stress.py path/to/bridge [-t topic]")
  sys.exit(1)

i = 0
# Write to the subprocess' standard input
while running:
  json_data = {
    "hostname": socket.gethostname(),
    "command": __file__,
    "id": i,
    "date": time.strftime("%Y-%m-%d %H:%M:%S")
  }
  process.stdin.write(json.dumps(json_data).encode())
  process.stdin.write(b'\n')  # newline character
  process.stdin.flush()       # flush the buffer
  time.sleep(0.2)
  i += 1

# Close the subprocess' by sending the magic word "exit"
process.stdin.write(b'exit\n')
process.stdin.close()
process.wait()