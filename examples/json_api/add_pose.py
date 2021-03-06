# Example on how to add a new object with an associated pose.
# 
# The examples use the JSON API that sends a messages via
# ZMQ to the SHERPA WM.  

import zmq
import random
import sys
import time
import json

# The port is defined by the system composition (sherpa_world_model.usc) file
# via the ``local_json_in_port`` variable. 
# e.g.
# local local_json_query_port = "22422"  
port = "22422" 

# Set up the ZMQ REQ-REP communication layer.
context = zmq.Context()

def sendMessageToSWM(message):
  socket = context.socket(zmq.REQ)
  socket.connect("tcp://localhost:%s" % port) # Currently he have to reconnect for every message.
  print("Sending update: %s " % (message))
  socket.send_string(message)
  result = socket.recv()
  print("Received result: %s " % (result))


# JSON message to CREATE a new Node. Note, that the parentId must
# exist beforehands, otherwise this operation does not succeed.
# in this case the "parentId": "e379121f-06c6-4e21-ae9d-ae78ec1986a1"
# denotes the root id of the graph. It is set in the respective
# system composition (.usc) file and initialized the SHERPA

newNodeMsg = {
  "@worldmodeltype": "RSGUpdate",
  "operation": "CREATE",
  "node": {
    "@graphtype": "Node",
    "id": "1c81d51b-e064-463c-8061-5ea822f6f74b", 
    "attributes": [
          {"key": "name", "value": "test_rescuer"},
    ],
  },
  "parentId": "e379121f-06c6-4e21-ae9d-ae78ec1986a1",
}
# Anatomy of the message:
#
# Please refere to the add_node.py example. The difference is that
# another "id" and another set of attributes is used.
# The transform node is used as parent Id.


# JSON message to CREATE a new Transform. Note that the parentId must
# exist beforehands, otherwise this operation does not succeed.
# in this case the "parentId": "e379121f-06c6-4e21-ae9d-ae78ec1986a1"
# denotes the root id of the graph. It is set in the respective
# system composition (.usc) file and initialized the SHERPA
newTransformMsg = {
  "@worldmodeltype": "RSGUpdate",
  "operation": "CREATE",
  "node": {    
    "@graphtype": "Connection",
    "@semanticContext":"Transform",
    "id": "3304e4a0-44d4-4fc8-8834-b0b03b418d5b",
    "attributes": [
      {"key": "tf:name", "value": "world_to_rescuer"},
      {"key": "type", "value": "Transform"},
    ],
    "sourceIds": [
      "e379121f-06c6-4e21-ae9d-ae78ec1986a1",
    ],
    "targetIds": [
      "1c81d51b-e064-463c-8061-5ea822f6f74b",
    ],
    "history" : [
      {
        "stamp": {
          "@stamptype": "TimeStampDate",
          "stamp": "2015-07-01T16:34:26Z",
        },
        "transform": {
          "type": "HomogeneousMatrix44",
          "matrix": [
            [1,0,0,0.01],
            [0,1,0,0.015],
            [0,0,1,0],
            [0,0,0,1] 
          ],
          "unit": "m"
        }
      }
    ], 	    
  },
  "parentId": "e379121f-06c6-4e21-ae9d-ae78ec1986a1",
}
# Anatomy of the message:
# The field 
#   "@worldmodeltype": "RSGUpdate",
# as seen in the previous example is mandatory and denotes the message is an update of the
# database for the Robot Scene Graph.
#
#   "operation": "CREATE", 
# Defines that this update creates a new graph primitive, while
# the two following lines specify to create an actual Transform (= pose)
#    "@graphtype": "Connection",
#    "@semanticContext":"Transform",
#
# We exlicitly specify a unique ID, that is specified here as: 
#   "id": "3304e4a0-44d4-4fc8-8834-b0b03b418d5b", 
# such that we directly know how to adreess the Transform in 
# order to perform successive updates.
#
# The explary attributes are:
# There might be  
#     "attributes": [
#        {"key": "tf:name", "value": "world_to_rescuer"},
#        {"key": "type", "value": "Transform"},
#    ],
#
# The attributes can be used for queries to find relevant nodes or
# for debugging purposes.
#
# For definition of the the meaning, a single sourceId describes the 
# frecerence frame as the origin of this node. The second node 
# denotes where the Trasform "points" to.
#
# "sourceIds": [
#      "e379121f-06c6-4e21-ae9d-ae78ec1986a1",
#    ],
#    "targetIds": [
#      "1c81d51b-e064-463c-8061-5ea822f6f74b",
#    ],
# 
# The actual data is stored in a temporal cache labelled as
# "history". Is is essantially a list of tuples of time stamps and 
# data for a rigid transformation as represented by a homogeneous
# transformation matrix:
#
#
#   "history" : [
#      {
#        "stamp": {
#          "@stamptype": "TimeStampDate",
#          "stamp": "2015-07-01T16:34:26Z",
#        },
#        "transform": {
#          "type": "HomogeneousMatrix44",
#          "matrix": [
#            [1,0,0,0.01],
#            [0,1,0,0.015],
#            [0,0,1,0],
#            [0,0,0,1] 
#          ],
#          "unit": "m"
#        }
#      }
#    ], 	    
#
# On creation of the Transform it is enough to specify one
# entry. Though a longer list could be provided. The units
# of mesurements indiacation "unit" is optional but highly 
# recommended. Default is [m].
#
#
# The field 
#
#   "parentId": "e379121f-06c6-4e21-ae9d-ae78ec1986a1",
#
# defined where this node is added in the Robot Scene Graph.
# It can be added to any "Group" node. The root node of
# a World Model Agent is a Group as well. Like it is done in 
# this example.   





# Send message.
print (json.dumps(newNodeMsg))
sendMessageToSWM(json.dumps(newNodeMsg))  
print (json.dumps(newTransformMsg))
sendMessageToSWM(json.dumps(newTransformMsg))  



