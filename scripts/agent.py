#      _    ____ _____ _   _ _____                                  _      
#     / \  / ___| ____| \ | |_   _|   _____  ____ _ _ __ ___  _ __ | | ___ 
#    / _ \| |  _|  _| |  \| | | |    / _ \ \/ / _` | '_ ` _ \| '_ \| |/ _ \
#   / ___ \ |_| | |___| |\  | | |   |  __/>  < (_| | | | | | | |_) | |  __/
#  /_/   \_\____|_____|_| \_| |_|    \___/_/\_\__,_|_| |_| |_| .__/|_|\___|
#                                                            |_|           
# See the file products/bin/Miroscic.py for the definition of the Agent class
import sys
import time
sys.path.insert(1, "products/bin")
from Miroscic import *


payload = {
  "name": LIB_NAME,
  "version": LIB_VERSION, 
  "agent": "feedback, via Python module"
}

# Agent creation and initialization can be done in one step
agent = Agent("wrapper", "tcp://127.0.0.1:9092").init()
print(agent.info())
print(agent.get_settings()) # settings are available as a python dictionary
agent.connect(250)
agent.register_event(event_type_startup)
time.sleep(1)
agent.publish(payload) # payload can be either a JSON string or a dictionary
time.sleep(1)
agent.register_event(event_type_shutdown)
agent.disconnect()
