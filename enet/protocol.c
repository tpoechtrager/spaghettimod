/** 
 @file  protocol.c
 @brief ENet protocol functions
*/
#include <stdio.h>
#include <string.h>
#define ENET_BUILDING_LIB 1
#include "enet/utility.h"
#include "enet/time.h"
#include "enet/enet.h"

static size_t commandSizes [ENET_PROTOCOL_COMMAND_COUNT] =
{
    0,
    sizeof (ENetProtocolAcknowledge),
    sizeof (ENetProtocolConnect),
    sizeof (ENetProtocolVerifyConnect),
    sizeof (ENetProtocolDisconnect),
    sizeof (ENetProtocolPing),
    sizeof (ENetProtocolSendReliable),
    sizeof (ENetProtocolSendUnreliable),
    sizeof (ENetProtocolSendFragment),
    sizeof (ENetProtocolSendUnsequenced),
    sizeof (ENetProtocolBandwidthLimit),
    sizeof (ENetProtocolThrottleConfigure),
    sizeof (ENetProtocolSendFragment)
};

size_t
enet_protocol_command_size (enet_uint8 commandNumber)
{
    return commandSizes [commandNumber & ENET_PROTOCOL_COMMAND_MASK];
}

static void
enet_protocol_change_state (ENetHost * host, ENetPeer * peer, ENetPeerState state)
{
    if (state == ENET_PEER_STATE_CONNECTED || state == ENET_PEER_STATE_DISCONNECT_LATER)
      enet_peer_on_connect (peer);
    else
      enet_peer_on_disconnect (peer);

    peer -> state = state;
}

static void
enet_protocol_dispatch_state (ENetHost * host, ENetPeer * peer, ENetPeerState state)
{
    enet_protocol_change_state (host, peer, state);

    if (! (peer -> flags & ENET_PEER_FLAG_NEEDS_DISPATCH))
    {
       enet_list_insert (enet_list_end (& host -> dispatchQueue), & peer -> dispatchList);

       peer -> flags |= ENET_PEER_FLAG_NEEDS_DISPATCH;
    }
}

static int
enet_protocol_dispatch_incoming_commands (ENetHost * host, ENetEvent * event)
{
    while (! enet_list_empty (& host -> dispatchQueue))
    {
       ENetPeer * peer = (ENetPeer *) enet_list_remove (enet_list_begin (& host -> dispatchQueue));

       peer -> flags &= ~ ENET_PEER_FLAG_NEEDS_DISPATCH;

       switch (peer -> state)
       {
       case ENET_PEER_STATE_CONNECTION_PENDING:
       case ENET_PEER_STATE_CONNECTION_SUCCEEDED:
           enet_protocol_change_state (host, peer, ENET_PEER_STATE_CONNECTED);

           event -> type = ENET_EVENT_TYPE_CONNECT;
           event -> peer = peer;
           event -> data = peer -> eventData;

           return 1;
           
       case ENET_PEER_STATE_ZOMBIE:
           host -> recalculateBandwidthLimits = 1;

           event -> type = ENET_EVENT_TYPE_DISCONNECT;
           event -> peer = peer;
           event -> data = peer -> eventData;

           enet_peer_reset (peer);

           return 1;

       case ENET_PEER_STATE_CONNECTED:
           if (enet_list_empty (& peer -> dispatchedCommands))
             continue;

           event -> packet = enet_peer_receive (peer, & event -> channelID);
           if (event -> packet == NULL)
             continue;
             
           event -> type = ENET_EVENT_TYPE_RECEIVE;
           event -> peer = peer;

           if (! enet_list_empty (& peer -> dispatchedCommands))
           {
              peer -> flags |= ENET_PEER_FLAG_NEEDS_DISPATCH;
         
              enet_list_insert (enet_list_end (& host -> dispatchQueue), & peer -> dispatchList);
           }

           return 1;

       default:
           break;
       }
    }

    return 0;
}

static void
enet_protocol_notify_connect (ENetHost * host, ENetPeer * peer, ENetEvent * event)
{
    host -> recalculateBandwidthLimits = 1;

    if (event != NULL)
    {
        enet_protocol_change_state (host, peer, ENET_PEER_STATE_CONNECTED);

        event -> type = ENET_EVENT_TYPE_CONNECT;
        event -> peer = peer;
        event -> data = peer -> eventData;
    }
    else 
        enet_protocol_dispatch_state (host, peer, peer -> state == ENET_PEER_STATE_CONNECTING ? ENET_PEER_STATE_CONNECTION_SUCCEEDED : ENET_PEER_STATE_CONNECTION_PENDING);
}

static void
enet_protocol_notify_disconnect (ENetHost * host, ENetPeer * peer, ENetEvent * event)
{
    if (peer -> state >= ENET_PEER_STATE_CONNECTION_PENDING)
       host -> recalculateBandwidthLimits = 1;

    if (peer -> state != ENET_PEER_STATE_CONNECTING && peer -> state < ENET_PEER_STATE_CONNECTION_SUCCEEDED)
        enet_peer_reset (peer);
    else
    if (event != NULL)
    {
        event -> type = ENET_EVENT_TYPE_DISCONNECT;
        event -> peer = peer;
        event -> data = 0;

        enet_peer_reset (peer);
    }
    else 
    {
        peer -> eventData = 0;

        enet_protocol_dispatch_state (host, peer, ENET_PEER_STATE_ZOMBIE);
    }
}

static void
enet_protocol_remove_sent_unreliable_commands (ENetPeer * peer)
{
    ENetOutgoingCommand * outgoingCommand;

    if (enet_list_empty (& peer -> sentUnreliableCommands))
      return;

    do
    {
        outgoingCommand = (ENetOutgoingCommand *) enet_list_front (& peer -> sentUnreliableCommands);
        
        enet_list_remove (& outgoingCommand -> outgoingCommandList);

        if (outgoingCommand -> packet != NULL)
        {
           -- outgoingCommand -> packet -> referenceCount;

           if (outgoingCommand -> packet -> referenceCount == 0)
           {
              outgoingCommand -> packet -> flags |= ENET_PACKET_FLAG_SENT;
 
              enet_packet_destroy (outgoingCommand -> packet);
           }
        }

        enet_free (outgoingCommand);
    } while (! enet_list_empty (& peer -> sentUnreliableCommands));

    if (peer -> state == ENET_PEER_STATE_DISCONNECT_LATER &&
        enet_list_empty (& peer -> outgoingCommands) &&
        enet_list_empty (& peer -> sentReliableCommands))
      enet_peer_disconnect (peer, peer -> eventData);
}

static ENetProtocolCommand
enet_protocol_remove_sent_reliable_command (ENetPeer * peer, enet_uint16 reliableSequenceNumber, enet_uint8 channelID)
{
    ENetOutgoingCommand * outgoingCommand = NULL;
    ENetListIterator currentCommand;
    ENetProtocolCommand commandNumber;
    int wasSent = 1;

    for (currentCommand = enet_list_begin (& peer -> sentReliableCommands);
         currentCommand != enet_list_end (& peer -> sentReliableCommands);
         currentCommand = enet_list_next (currentCommand))
    {
       outgoingCommand = (ENetOutgoingCommand *) currentCommand;
        
       if (outgoingCommand -> reliableSequenceNumber == reliableSequenceNumber &&
           outgoingCommand -> command.header.channelID == channelID)
         break;
    }

    if (currentCommand == enet_list_end (& peer -> sentReliableCommands))
    {
       for (currentCommand = enet_list_begin (& peer -> outgoingCommands);
            currentCommand != enet_list_end (& peer -> outgoingCommands);
            currentCommand = enet_list_next (currentCommand))
       {
          outgoingCommand = (ENetOutgoingCommand *) currentCommand;

          if (! (outgoingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE))
            continue;

          if (outgoingCommand -> sendAttempts < 1) return ENET_PROTOCOL_COMMAND_NONE;

          if (outgoingCommand -> reliableSequenceNumber == reliableSequenceNumber &&
              outgoingCommand -> command.header.channelID == channelID)
            break;
       }

       if (currentCommand == enet_list_end (& peer -> outgoingCommands))
         return ENET_PROTOCOL_COMMAND_NONE;

       wasSent = 0;
    }

    if (outgoingCommand == NULL)
      return ENET_PROTOCOL_COMMAND_NONE;

    if (channelID < peer -> channelCount)
    {
       ENetChannel * channel = & peer -> channels [channelID];
       enet_uint16 reliableWindow = reliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;
       if (channel -> reliableWindows [reliableWindow] > 0)
       {
          -- channel -> reliableWindows [reliableWindow];
          if (! channel -> reliableWindows [reliableWindow])
            channel -> usedReliableWindows &= ~ (1 << reliableWindow);
       }
    }

    commandNumber = (ENetProtocolCommand) (outgoingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_MASK);
    
    enet_list_remove (& outgoingCommand -> outgoingCommandList);

    if (outgoingCommand -> packet != NULL)
    {
       if (wasSent)
         peer -> reliableDataInTransit -= outgoingCommand -> fragmentLength;

       -- outgoingCommand -> packet -> referenceCount;

       if (outgoingCommand -> packet -> referenceCount == 0)
       {
          outgoingCommand -> packet -> flags |= ENET_PACKET_FLAG_SENT;

          enet_packet_destroy (outgoingCommand -> packet);
       }
    }

    enet_free (outgoingCommand);

    if (enet_list_empty (& peer -> sentReliableCommands))
      return commandNumber;
    
    outgoingCommand = (ENetOutgoingCommand *) enet_list_front (& peer -> sentReliableCommands);
    
    peer -> nextTimeout = outgoingCommand -> sentTime + outgoingCommand -> roundTripTimeout;

    return commandNumber;
} 

static void
enet_peer_try_create_own_socket (ENetPeer * peer)
{
#ifdef SO_REUSEPORT
    if (peer -> localAddress.host)
    {
        ENetAddress localBind;
        localBind.host = peer -> localAddress.host;
        localBind.port = peer -> host -> address.port;
        peer -> ownSocket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        int y = 1;
        if (peer -> ownSocket == ENET_SOCKET_NULL ||
                setsockopt (peer -> ownSocket, SOL_SOCKET, SO_REUSEPORT, & y, sizeof (y)) ||
                enet_socket_bind (peer -> ownSocket, & localBind) ||
                enet_socket_connect (peer -> ownSocket, & peer -> address))
        {
            enet_socket_destroy (peer -> ownSocket);
            peer -> ownSocket = ENET_SOCKET_NULL;
            return;
        }
        enet_socket_set_option (peer -> ownSocket, ENET_SOCKOPT_NONBLOCK, 1);
        int tos = 4;
        setsockopt (peer -> ownSocket, IPPROTO_IP, IP_TOS, & tos, sizeof (tos));
    }
#endif
}

static ENetPeer *
enet_protocol_handle_connect (ENetHost * host, ENetProtocolHeader * header, ENetProtocol * command)
{
    enet_uint8 incomingSessionID, outgoingSessionID = 0;
    enet_uint32 mtu, windowSize;
    ENetChannel * channel;
    size_t channelCount, duplicatePeers = 0;
    ENetPeer * currentPeer, * peer = NULL, * realPeer = NULL, dummyPeer;
    ENetProtocol verifyCommand;
    enet_uint16 randomTimeSent = 0;
    ENetConnectingPeer * cookie = NULL;
    ENetProtocolHeader responseHeader;
    enet_uint32 checksum;
    ENetBuffer buffers[3];

    channelCount = ENET_NET_TO_HOST_32 (command -> connect.channelCount);

    if (channelCount < ENET_PROTOCOL_MINIMUM_CHANNEL_COUNT ||
        channelCount > ENET_PROTOCOL_MAXIMUM_CHANNEL_COUNT)
      return NULL;

    if (! host -> connectingPeerTimeout)
    {
        if (! host -> idlePeers)
            return NULL;
        size_t iPeer;
        for (iPeer = 0; iPeer < host -> peerCount - host -> idlePeers; ++ iPeer)
        {
            currentPeer = & host -> peers [host -> busyPeersList [iPeer]];
            if (currentPeer -> state != ENET_PEER_STATE_CONNECTING &&
                currentPeer -> address.host == host -> receivedAddress.host)
            {
                if (currentPeer -> address.port == host -> receivedAddress.port &&
                    currentPeer -> connectID == command -> connect.connectID)
                  return NULL;

                ++ duplicatePeers;
            }
        }
        peer = & host -> peers [host -> idlePeersList [host -> idlePeers - 1]];
    }
    else
    {
      if (! host -> idlePeers) return NULL;
      enet_uint32 random = host -> randomFunction.generate (host -> randomFunction.context), randomTry = random;
      retry:
      outgoingSessionID = randomTry % ENET_PROTOCOL_TOTAL_SESSIONS;
      randomTry /= ENET_PROTOCOL_TOTAL_SESSIONS;
      randomTimeSent = randomTry;
      randomTry >>= 16;
      realPeer = & host -> peers [host -> idlePeersList [randomTry % host -> idlePeers]];
      if (realPeer -> connectingPeers)
      {
        cookie = & realPeer -> connectingPeers [randomTimeSent & realPeer -> connectingPeersTimeMask] [outgoingSessionID];
        if (cookie -> address.host)
        {
          if (host -> serviceTime - cookie -> realTimeSent > host -> connectingPeerTimeout)
            cookie -> address.host = 0;
          else
          {
            if ((host -> connectsInWindow * 100) / host -> connectsWindow < host -> connectsWindowRatio)
            {
              randomTry = ++ random;
              goto retry;
            }
            if (realPeer -> connectingPeersTimeMask == (enet_uint16)-1)
              return NULL;
            else
            {
              enet_uint16 extraBit;
              extraBit = realPeer -> connectingPeersTimeMask + 1;
              randomTimeSent = (randomTimeSent & ~extraBit) | (~(cookie -> randomTimeSent) & extraBit);
              cookie = NULL;
            }
          }
        }
      }
      peer = & dummyPeer;
      peer -> outgoingSessionID = realPeer -> outgoingSessionID;
      peer -> incomingPeerID = realPeer -> incomingPeerID;
    }

    if (peer == NULL || duplicatePeers >= host -> duplicatePeers)
      return NULL;

    if (channelCount > host -> channelLimit)
      channelCount = host -> channelLimit;
    if (! host -> connectingPeerTimeout)
    {
      peer -> channels = (ENetChannel *) enet_malloc (channelCount * sizeof (ENetChannel));
      if (peer -> channels == NULL)
        return NULL;
    }
    peer -> channelCount = channelCount;
    peer -> state = ENET_PEER_STATE_ACKNOWLEDGING_CONNECT;
    peer -> connectID = command -> connect.connectID;
    peer -> address = host -> receivedAddress;
    peer -> outgoingPeerID = ENET_NET_TO_HOST_16 (command -> connect.outgoingPeerID);
    peer -> incomingBandwidth = ENET_NET_TO_HOST_32 (command -> connect.incomingBandwidth);
    peer -> outgoingBandwidth = ENET_NET_TO_HOST_32 (command -> connect.outgoingBandwidth);
    peer -> packetThrottleInterval = ENET_NET_TO_HOST_32 (command -> connect.packetThrottleInterval);
    peer -> packetThrottleAcceleration = ENET_NET_TO_HOST_32 (command -> connect.packetThrottleAcceleration);
    peer -> packetThrottleDeceleration = ENET_NET_TO_HOST_32 (command -> connect.packetThrottleDeceleration);
    peer -> eventData = ENET_NET_TO_HOST_32 (command -> connect.data);

    incomingSessionID = command -> connect.incomingSessionID == 0xFF ? peer -> outgoingSessionID : command -> connect.incomingSessionID;
    incomingSessionID = (incomingSessionID + 1) & (ENET_PROTOCOL_HEADER_SESSION_MASK >> ENET_PROTOCOL_HEADER_SESSION_SHIFT);
    if (incomingSessionID == peer -> outgoingSessionID)
      incomingSessionID = (incomingSessionID + 1) & (ENET_PROTOCOL_HEADER_SESSION_MASK >> ENET_PROTOCOL_HEADER_SESSION_SHIFT);
    peer -> outgoingSessionID = incomingSessionID;

    if (! host -> connectingPeerTimeout)
    {
      outgoingSessionID = command -> connect.outgoingSessionID == 0xFF ? peer -> incomingSessionID : command -> connect.outgoingSessionID;
      outgoingSessionID = (outgoingSessionID + 1) & (ENET_PROTOCOL_HEADER_SESSION_MASK >> ENET_PROTOCOL_HEADER_SESSION_SHIFT);
      if (outgoingSessionID == peer -> incomingSessionID)
        outgoingSessionID = (outgoingSessionID + 1) & (ENET_PROTOCOL_HEADER_SESSION_MASK >> ENET_PROTOCOL_HEADER_SESSION_SHIFT);
    }
    peer -> incomingSessionID = outgoingSessionID;

    if (! host -> connectingPeerTimeout)
    {
      for (channel = peer -> channels;
           channel < & peer -> channels [channelCount];
         ++ channel)
      {
        channel -> outgoingReliableSequenceNumber = 0;
        channel -> outgoingUnreliableSequenceNumber = 0;
        channel -> incomingReliableSequenceNumber = 0;
        channel -> incomingUnreliableSequenceNumber = 0;

        enet_list_clear (& channel -> incomingReliableCommands);
        enet_list_clear (& channel -> incomingUnreliableCommands);

        channel -> usedReliableWindows = 0;
        memset (channel -> reliableWindows, 0, sizeof (channel -> reliableWindows));
      }
    }

    mtu = ENET_NET_TO_HOST_32 (command -> connect.mtu);

    if (mtu < ENET_PROTOCOL_MINIMUM_MTU)
      mtu = ENET_PROTOCOL_MINIMUM_MTU;
    else
    if (mtu > ENET_PROTOCOL_MAXIMUM_MTU)
      mtu = ENET_PROTOCOL_MAXIMUM_MTU;

    peer -> mtu = mtu;

    if (host -> outgoingBandwidth == 0 &&
        peer -> incomingBandwidth == 0)
      peer -> windowSize = ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE;
    else
    if (host -> outgoingBandwidth == 0 ||
        peer -> incomingBandwidth == 0)
      peer -> windowSize = (ENET_MAX (host -> outgoingBandwidth, peer -> incomingBandwidth) /
                                    ENET_PEER_WINDOW_SIZE_SCALE) *
                                      ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;
    else
      peer -> windowSize = (ENET_MIN (host -> outgoingBandwidth, peer -> incomingBandwidth) /
                                    ENET_PEER_WINDOW_SIZE_SCALE) * 
                                      ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;

    if (peer -> windowSize < ENET_PROTOCOL_MINIMUM_WINDOW_SIZE)
      peer -> windowSize = ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;
    else
    if (peer -> windowSize > ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE)
      peer -> windowSize = ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE;

    if (host -> incomingBandwidth == 0)
      windowSize = ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE;
    else
      windowSize = (host -> incomingBandwidth / ENET_PEER_WINDOW_SIZE_SCALE) *
                     ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;

    if (windowSize > ENET_NET_TO_HOST_32 (command -> connect.windowSize))
      windowSize = ENET_NET_TO_HOST_32 (command -> connect.windowSize);

    if (windowSize < ENET_PROTOCOL_MINIMUM_WINDOW_SIZE)
      windowSize = ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;
    else
    if (windowSize > ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE)
      windowSize = ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE;

    peer -> timedOut = 0;

    verifyCommand.header.command = ENET_PROTOCOL_COMMAND_VERIFY_CONNECT | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
    verifyCommand.header.channelID = 0xFF;
    verifyCommand.verifyConnect.outgoingPeerID = ENET_HOST_TO_NET_16 (peer -> incomingPeerID);
    verifyCommand.verifyConnect.incomingSessionID = incomingSessionID;
    verifyCommand.verifyConnect.outgoingSessionID = outgoingSessionID;
    verifyCommand.verifyConnect.mtu = ENET_HOST_TO_NET_32 (peer -> mtu);
    verifyCommand.verifyConnect.windowSize = ENET_HOST_TO_NET_32 (windowSize);
    verifyCommand.verifyConnect.channelCount = ENET_HOST_TO_NET_32 (channelCount);
    verifyCommand.verifyConnect.incomingBandwidth = ENET_HOST_TO_NET_32 (host -> incomingBandwidth);
    verifyCommand.verifyConnect.outgoingBandwidth = ENET_HOST_TO_NET_32 (host -> outgoingBandwidth);
    verifyCommand.verifyConnect.packetThrottleInterval = ENET_HOST_TO_NET_32 (peer -> packetThrottleInterval);
    verifyCommand.verifyConnect.packetThrottleAcceleration = ENET_HOST_TO_NET_32 (peer -> packetThrottleAcceleration);
    verifyCommand.verifyConnect.packetThrottleDeceleration = ENET_HOST_TO_NET_32 (peer -> packetThrottleDeceleration);
    verifyCommand.verifyConnect.connectID = peer -> connectID;

    if (! host -> connectingPeerTimeout)
    {
      host -> busyPeersList [host -> peerCount - host -> idlePeers --] = peer -> incomingPeerID;
      enet_peer_try_create_own_socket (peer);
      enet_peer_queue_outgoing_command (peer, & verifyCommand, NULL, 0, 0);

      return peer;
    }

    responseHeader.peerID = ENET_HOST_TO_NET_16(peer -> outgoingPeerID | (peer -> outgoingSessionID << ENET_PROTOCOL_HEADER_SESSION_SHIFT) | ENET_PROTOCOL_HEADER_FLAG_SENT_TIME);
    responseHeader.sentTime = randomTimeSent;
    verifyCommand.verifyConnect.header.reliableSequenceNumber = ENET_HOST_TO_NET_16(1);
    buffers[0].data = & responseHeader;
    buffers[0].dataLength = sizeof (responseHeader);
    if (host -> checksum != NULL)
    {
      buffers[1].data = & checksum;
      buffers[1].dataLength = sizeof (enet_uint32);
      buffers[2].data = & verifyCommand;
      buffers[2].dataLength = commandSizes [ENET_PROTOCOL_COMMAND_VERIFY_CONNECT];
      checksum = peer -> connectID;
      checksum = host -> checksum (buffers, 3);
    }
    else
    {
      buffers[1].data = & verifyCommand;
      buffers[1].dataLength = commandSizes [ENET_PROTOCOL_COMMAND_VERIFY_CONNECT];
    }
    if (enet_socket_send (host -> socket, & peer -> address, buffers, 2 + (host -> checksum != NULL)) < 0)
      return NULL;

    host -> totalSentData += sizeof (responseHeader) + sizeof (enet_uint32) * (host -> checksum != NULL) + commandSizes [ENET_PROTOCOL_COMMAND_VERIFY_CONNECT];
    ++ host -> totalSentPackets;
    if (! cookie)
    {
      if (! realPeer -> connectingPeers)
      {
        realPeer -> connectingPeers = enet_malloc (sizeof (ENetConnectingPeer) * ENET_PROTOCOL_TOTAL_SESSIONS * (ENET_PEER_MINIMUM_TIME_SENT_MASK + 1));
        memset (realPeer -> connectingPeers, 0, sizeof (ENetConnectingPeer) * ENET_PROTOCOL_TOTAL_SESSIONS * (ENET_PEER_MINIMUM_TIME_SENT_MASK + 1));
        if (! realPeer -> connectingPeers)
          return NULL;
        realPeer -> connectingPeersTimeMask = ENET_PEER_MINIMUM_TIME_SENT_MASK;
        host -> connectsWindow += ENET_PROTOCOL_TOTAL_SESSIONS * (ENET_PEER_MINIMUM_TIME_SENT_MASK + 1);
      }
      else
      {
        size_t iCookie, iSession, oldLength = realPeer -> connectingPeersTimeMask + 1;
        ENetConnectingPeer (* newConnectingPeers) [ENET_PROTOCOL_TOTAL_SESSIONS] = enet_malloc (sizeof (ENetConnectingPeer) * ENET_PROTOCOL_TOTAL_SESSIONS * (oldLength * 2));
        if (! newConnectingPeers)
          return NULL;
        memset (newConnectingPeers, 0, sizeof (ENetConnectingPeer) * ENET_PROTOCOL_TOTAL_SESSIONS * (oldLength * 2));
        realPeer -> connectingPeersTimeMask |= oldLength;
        for (iCookie = 0; iCookie < oldLength; ++ iCookie)
          for (iSession = 0; iSession < ENET_PROTOCOL_TOTAL_SESSIONS; ++ iSession)
          {
            ENetConnectingPeer * oldCookie = & realPeer -> connectingPeers [iCookie] [iSession];
            if (oldCookie -> address.host)
              newConnectingPeers [oldCookie -> randomTimeSent & realPeer -> connectingPeersTimeMask] [iSession] = * oldCookie;
          }
        enet_free (realPeer -> connectingPeers);
        realPeer -> connectingPeers = newConnectingPeers;
        host -> connectsWindow += ENET_PROTOCOL_TOTAL_SESSIONS * oldLength;
      }
      cookie = & realPeer -> connectingPeers [randomTimeSent & realPeer -> connectingPeersTimeMask] [outgoingSessionID];
    }

    cookie -> address = peer -> address;
    cookie -> randomTimeSent = randomTimeSent;
    cookie -> realTimeSent = host -> serviceTime;
    cookie -> channelCount = peer -> channelCount;
    cookie -> connectID = peer -> connectID;
    cookie -> outgoingPeerID = peer -> outgoingPeerID;
    cookie -> incomingBandwidth = peer -> incomingBandwidth;
    cookie -> outgoingBandwidth = peer -> outgoingBandwidth;
    cookie -> packetThrottleInterval = peer -> packetThrottleInterval;
    cookie -> packetThrottleAcceleration = peer -> packetThrottleAcceleration;
    cookie -> packetThrottleDeceleration = peer -> packetThrottleDeceleration;
    cookie -> eventData = peer -> eventData;
    cookie -> outgoingSessionID = peer -> outgoingSessionID;
    cookie -> mtu = peer -> mtu;
    cookie -> windowSize = peer -> windowSize - 1;
    ++ host -> connectsInWindow;
    ++ host -> connectsData [host -> connectsDataIndex];
    return NULL;

}

static int
enet_protocol_handle_send_reliable (ENetHost * host, ENetPeer * peer, const ENetProtocol * command, enet_uint8 ** currentData)
{
    size_t dataLength;

    if (command -> header.channelID >= peer -> channelCount ||
        (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER))
      return -1;

    dataLength = ENET_NET_TO_HOST_16 (command -> sendReliable.dataLength);
    * currentData += dataLength;
    if (dataLength > host -> maximumPacketSize ||
        * currentData < host -> receivedData ||
        * currentData > & host -> receivedData [host -> receivedDataLength])
      return -1;

    if (enet_peer_queue_incoming_command (peer, command, (const enet_uint8 *) command + sizeof (ENetProtocolSendReliable), dataLength, ENET_PACKET_FLAG_RELIABLE, 0) == NULL)
      return -1;

    return 0;
}

static int
enet_protocol_handle_send_unsequenced (ENetHost * host, ENetPeer * peer, const ENetProtocol * command, enet_uint8 ** currentData)
{
    enet_uint32 unsequencedGroup, index;
    size_t dataLength;

    if (command -> header.channelID >= peer -> channelCount ||
        (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER))
      return -1;

    dataLength = ENET_NET_TO_HOST_16 (command -> sendUnsequenced.dataLength);
    * currentData += dataLength;
    if (dataLength > host -> maximumPacketSize ||
        * currentData < host -> receivedData ||
        * currentData > & host -> receivedData [host -> receivedDataLength])
      return -1; 

    unsequencedGroup = ENET_NET_TO_HOST_16 (command -> sendUnsequenced.unsequencedGroup);
    index = unsequencedGroup % ENET_PEER_UNSEQUENCED_WINDOW_SIZE;
   
    if (unsequencedGroup < peer -> incomingUnsequencedGroup)
      unsequencedGroup += 0x10000;

    if (unsequencedGroup >= (enet_uint32) peer -> incomingUnsequencedGroup + ENET_PEER_FREE_UNSEQUENCED_WINDOWS * ENET_PEER_UNSEQUENCED_WINDOW_SIZE)
      return 0;

    unsequencedGroup &= 0xFFFF;

    if (unsequencedGroup - index != peer -> incomingUnsequencedGroup)
    {
        peer -> incomingUnsequencedGroup = unsequencedGroup - index;

        memset (peer -> unsequencedWindow, 0, sizeof (peer -> unsequencedWindow));
    }
    else
    if (peer -> unsequencedWindow [index / 32] & (1 << (index % 32)))
      return 0;
      
    if (enet_peer_queue_incoming_command (peer, command, (const enet_uint8 *) command + sizeof (ENetProtocolSendUnsequenced), dataLength, ENET_PACKET_FLAG_UNSEQUENCED, 0) == NULL)
      return -1;
   
    peer -> unsequencedWindow [index / 32] |= 1 << (index % 32);
 
    return 0;
}

static int
enet_protocol_handle_send_unreliable (ENetHost * host, ENetPeer * peer, const ENetProtocol * command, enet_uint8 ** currentData)
{
    size_t dataLength;

    if (command -> header.channelID >= peer -> channelCount ||
        (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER))
      return -1;

    dataLength = ENET_NET_TO_HOST_16 (command -> sendUnreliable.dataLength);
    * currentData += dataLength;
    if (dataLength > host -> maximumPacketSize ||
        * currentData < host -> receivedData ||
        * currentData > & host -> receivedData [host -> receivedDataLength])
      return -1;

    if (enet_peer_queue_incoming_command (peer, command, (const enet_uint8 *) command + sizeof (ENetProtocolSendUnreliable), dataLength, 0, 0) == NULL)
      return -1;

    return 0;
}

static int
enet_protocol_handle_send_fragment (ENetHost * host, ENetPeer * peer, const ENetProtocol * command, enet_uint8 ** currentData)
{
    enet_uint32 fragmentNumber,
           fragmentCount,
           fragmentOffset,
           fragmentLength,
           startSequenceNumber,
           totalLength;
    ENetChannel * channel;
    enet_uint16 startWindow, currentWindow;
    ENetListIterator currentCommand;
    ENetIncomingCommand * startCommand = NULL;

    if (command -> header.channelID >= peer -> channelCount ||
        (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER))
      return -1;

    fragmentLength = ENET_NET_TO_HOST_16 (command -> sendFragment.dataLength);
    * currentData += fragmentLength;
    if (fragmentLength > host -> maximumPacketSize ||
        * currentData < host -> receivedData ||
        * currentData > & host -> receivedData [host -> receivedDataLength])
      return -1;

    channel = & peer -> channels [command -> header.channelID];
    startSequenceNumber = ENET_NET_TO_HOST_16 (command -> sendFragment.startSequenceNumber);
    startWindow = startSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;
    currentWindow = channel -> incomingReliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;

    if (startSequenceNumber < channel -> incomingReliableSequenceNumber)
      startWindow += ENET_PEER_RELIABLE_WINDOWS;

    if (startWindow < currentWindow || startWindow >= currentWindow + ENET_PEER_FREE_RELIABLE_WINDOWS - 1)
      return 0;

    fragmentNumber = ENET_NET_TO_HOST_32 (command -> sendFragment.fragmentNumber);
    fragmentCount = ENET_NET_TO_HOST_32 (command -> sendFragment.fragmentCount);
    fragmentOffset = ENET_NET_TO_HOST_32 (command -> sendFragment.fragmentOffset);
    totalLength = ENET_NET_TO_HOST_32 (command -> sendFragment.totalLength);
    
    if (fragmentCount > ENET_PROTOCOL_MAXIMUM_FRAGMENT_COUNT ||
        fragmentNumber >= fragmentCount ||
        totalLength > host -> maximumPacketSize ||
        fragmentOffset >= totalLength ||
        fragmentLength > totalLength - fragmentOffset)
      return -1;
 
    for (currentCommand = enet_list_previous (enet_list_end (& channel -> incomingReliableCommands));
         currentCommand != enet_list_end (& channel -> incomingReliableCommands);
         currentCommand = enet_list_previous (currentCommand))
    {
       ENetIncomingCommand * incomingCommand = (ENetIncomingCommand *) currentCommand;

       if (startSequenceNumber >= channel -> incomingReliableSequenceNumber)
       {
          if (incomingCommand -> reliableSequenceNumber < channel -> incomingReliableSequenceNumber)
            continue;
       }
       else
       if (incomingCommand -> reliableSequenceNumber >= channel -> incomingReliableSequenceNumber)
         break;

       if (incomingCommand -> reliableSequenceNumber <= startSequenceNumber)
       {
          if (incomingCommand -> reliableSequenceNumber < startSequenceNumber)
            break;
        
          if ((incomingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_MASK) != ENET_PROTOCOL_COMMAND_SEND_FRAGMENT ||
              totalLength != incomingCommand -> packet -> dataLength ||
              fragmentCount != incomingCommand -> fragmentCount)
            return -1;

          startCommand = incomingCommand;
          break;
       }
    }
 
    if (startCommand == NULL)
    {
       ENetProtocol hostCommand = * command;

       hostCommand.header.reliableSequenceNumber = startSequenceNumber;

       startCommand = enet_peer_queue_incoming_command (peer, & hostCommand, NULL, totalLength, ENET_PACKET_FLAG_RELIABLE, fragmentCount);
       if (startCommand == NULL)
         return -1;
    }
    
    if ((startCommand -> fragments [fragmentNumber / 32] & (1 << (fragmentNumber % 32))) == 0)
    {
       -- startCommand -> fragmentsRemaining;

       startCommand -> fragments [fragmentNumber / 32] |= (1 << (fragmentNumber % 32));

       if (fragmentOffset + fragmentLength > startCommand -> packet -> dataLength)
         fragmentLength = startCommand -> packet -> dataLength - fragmentOffset;

       memcpy (startCommand -> packet -> data + fragmentOffset,
               (enet_uint8 *) command + sizeof (ENetProtocolSendFragment),
               fragmentLength);

        if (startCommand -> fragmentsRemaining <= 0)
          enet_peer_dispatch_incoming_reliable_commands (peer, channel);
    }

    return 0;
}

static int
enet_protocol_handle_send_unreliable_fragment (ENetHost * host, ENetPeer * peer, const ENetProtocol * command, enet_uint8 ** currentData)
{
    enet_uint32 fragmentNumber,
           fragmentCount,
           fragmentOffset,
           fragmentLength,
           reliableSequenceNumber,
           startSequenceNumber,
           totalLength;
    enet_uint16 reliableWindow, currentWindow;
    ENetChannel * channel;
    ENetListIterator currentCommand;
    ENetIncomingCommand * startCommand = NULL;

    if (command -> header.channelID >= peer -> channelCount ||
        (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER))
      return -1;

    fragmentLength = ENET_NET_TO_HOST_16 (command -> sendFragment.dataLength);
    * currentData += fragmentLength;
    if (fragmentLength > host -> maximumPacketSize ||
        * currentData < host -> receivedData ||
        * currentData > & host -> receivedData [host -> receivedDataLength])
      return -1;

    channel = & peer -> channels [command -> header.channelID];
    reliableSequenceNumber = command -> header.reliableSequenceNumber;
    startSequenceNumber = ENET_NET_TO_HOST_16 (command -> sendFragment.startSequenceNumber);

    reliableWindow = reliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;
    currentWindow = channel -> incomingReliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;

    if (reliableSequenceNumber < channel -> incomingReliableSequenceNumber)
      reliableWindow += ENET_PEER_RELIABLE_WINDOWS;

    if (reliableWindow < currentWindow || reliableWindow >= currentWindow + ENET_PEER_FREE_RELIABLE_WINDOWS - 1)
      return 0;

    if (reliableSequenceNumber == channel -> incomingReliableSequenceNumber &&
        startSequenceNumber <= channel -> incomingUnreliableSequenceNumber)
      return 0;

    fragmentNumber = ENET_NET_TO_HOST_32 (command -> sendFragment.fragmentNumber);
    fragmentCount = ENET_NET_TO_HOST_32 (command -> sendFragment.fragmentCount);
    fragmentOffset = ENET_NET_TO_HOST_32 (command -> sendFragment.fragmentOffset);
    totalLength = ENET_NET_TO_HOST_32 (command -> sendFragment.totalLength);

    if (fragmentCount > ENET_PROTOCOL_MAXIMUM_FRAGMENT_COUNT ||
        fragmentNumber >= fragmentCount ||
        totalLength > host -> maximumPacketSize ||
        fragmentOffset >= totalLength ||
        fragmentLength > totalLength - fragmentOffset)
      return -1;

    for (currentCommand = enet_list_previous (enet_list_end (& channel -> incomingUnreliableCommands));
         currentCommand != enet_list_end (& channel -> incomingUnreliableCommands);
         currentCommand = enet_list_previous (currentCommand))
    {
       ENetIncomingCommand * incomingCommand = (ENetIncomingCommand *) currentCommand;

       if (reliableSequenceNumber >= channel -> incomingReliableSequenceNumber)
       {
          if (incomingCommand -> reliableSequenceNumber < channel -> incomingReliableSequenceNumber)
            continue;
       }
       else
       if (incomingCommand -> reliableSequenceNumber >= channel -> incomingReliableSequenceNumber)
         break;

       if (incomingCommand -> reliableSequenceNumber < reliableSequenceNumber)
         break;

       if (incomingCommand -> reliableSequenceNumber > reliableSequenceNumber)
         continue;

       if (incomingCommand -> unreliableSequenceNumber <= startSequenceNumber)
       {
          if (incomingCommand -> unreliableSequenceNumber < startSequenceNumber)
            break;

          if ((incomingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_MASK) != ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT ||
              totalLength != incomingCommand -> packet -> dataLength ||
              fragmentCount != incomingCommand -> fragmentCount)
            return -1;

          startCommand = incomingCommand;
          break;
       }
    }

    if (startCommand == NULL)
    {
       startCommand = enet_peer_queue_incoming_command (peer, command, NULL, totalLength, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT, fragmentCount);
       if (startCommand == NULL)
         return -1;
    }

    if ((startCommand -> fragments [fragmentNumber / 32] & (1 << (fragmentNumber % 32))) == 0)
    {
       -- startCommand -> fragmentsRemaining;

       startCommand -> fragments [fragmentNumber / 32] |= (1 << (fragmentNumber % 32));

       if (fragmentOffset + fragmentLength > startCommand -> packet -> dataLength)
         fragmentLength = startCommand -> packet -> dataLength - fragmentOffset;

       memcpy (startCommand -> packet -> data + fragmentOffset,
               (enet_uint8 *) command + sizeof (ENetProtocolSendFragment),
               fragmentLength);

        if (startCommand -> fragmentsRemaining <= 0)
          enet_peer_dispatch_incoming_unreliable_commands (peer, channel);
    }

    return 0;
}

static int
enet_protocol_handle_ping (ENetHost * host, ENetPeer * peer, const ENetProtocol * command)
{
    if (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER)
      return -1;

    return 0;
}

static int
enet_protocol_handle_bandwidth_limit (ENetHost * host, ENetPeer * peer, const ENetProtocol * command)
{
    if (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER)
      return -1;

    if (peer -> incomingBandwidth != 0)
      -- host -> bandwidthLimitedPeers;

    peer -> incomingBandwidth = ENET_NET_TO_HOST_32 (command -> bandwidthLimit.incomingBandwidth);
    peer -> outgoingBandwidth = ENET_NET_TO_HOST_32 (command -> bandwidthLimit.outgoingBandwidth);

    if (peer -> incomingBandwidth != 0)
      ++ host -> bandwidthLimitedPeers;

    if (peer -> incomingBandwidth == 0 && host -> outgoingBandwidth == 0)
      peer -> windowSize = ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE;
    else
    if (peer -> incomingBandwidth == 0 || host -> outgoingBandwidth == 0)
      peer -> windowSize = (ENET_MAX (peer -> incomingBandwidth, host -> outgoingBandwidth) /
                             ENET_PEER_WINDOW_SIZE_SCALE) * ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;
    else
      peer -> windowSize = (ENET_MIN (peer -> incomingBandwidth, host -> outgoingBandwidth) /
                             ENET_PEER_WINDOW_SIZE_SCALE) * ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;

    if (peer -> windowSize < ENET_PROTOCOL_MINIMUM_WINDOW_SIZE)
      peer -> windowSize = ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;
    else
    if (peer -> windowSize > ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE)
      peer -> windowSize = ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE;

    return 0;
}

static int
enet_protocol_handle_throttle_configure (ENetHost * host, ENetPeer * peer, const ENetProtocol * command)
{
    if (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER)
      return -1;

    peer -> packetThrottleInterval = ENET_NET_TO_HOST_32 (command -> throttleConfigure.packetThrottleInterval);
    peer -> packetThrottleAcceleration = ENET_NET_TO_HOST_32 (command -> throttleConfigure.packetThrottleAcceleration);
    peer -> packetThrottleDeceleration = ENET_NET_TO_HOST_32 (command -> throttleConfigure.packetThrottleDeceleration);

    return 0;
}

static int
enet_protocol_handle_disconnect (ENetHost * host, ENetPeer * peer, const ENetProtocol * command)
{
    if (peer -> state == ENET_PEER_STATE_DISCONNECTED || peer -> state == ENET_PEER_STATE_ZOMBIE || peer -> state == ENET_PEER_STATE_ACKNOWLEDGING_DISCONNECT)
      return 0;

    enet_peer_reset_queues (peer);

    if (peer -> state == ENET_PEER_STATE_CONNECTION_SUCCEEDED || peer -> state == ENET_PEER_STATE_DISCONNECTING || peer -> state == ENET_PEER_STATE_CONNECTING)
        enet_protocol_dispatch_state (host, peer, ENET_PEER_STATE_ZOMBIE);
    else
    if (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER)
    {
        if (peer -> state == ENET_PEER_STATE_CONNECTION_PENDING) host -> recalculateBandwidthLimits = 1;

        enet_peer_reset (peer);
    }
    else
    if (command -> header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE)
      enet_protocol_change_state (host, peer, ENET_PEER_STATE_ACKNOWLEDGING_DISCONNECT);
    else
      enet_protocol_dispatch_state (host, peer, ENET_PEER_STATE_ZOMBIE);

    if (peer -> state != ENET_PEER_STATE_DISCONNECTED)
      peer -> eventData = ENET_NET_TO_HOST_32 (command -> disconnect.data);

    return 0;
}

static int
enet_protocol_handle_acknowledge (ENetHost * host, ENetEvent * event, ENetPeer * peer, const ENetProtocol * command, enet_uint8 sessionID)
{
    enet_uint32 roundTripTime,
           receivedSentTime,
           receivedReliableSequenceNumber;
    ENetProtocolCommand commandNumber;
    int cookieRestored = 0;

    if (peer -> state == ENET_PEER_STATE_ZOMBIE)
      return 0;

    if (peer -> state == ENET_PEER_STATE_DISCONNECTED)
    {
      ENetConnectingPeer * cookie;
      size_t iPeer, duplicatePeers = 0;
      ENetChannel * channel;
      if (! host -> connectingPeerTimeout ||
          ! peer -> connectingPeers ||
          command -> header.channelID != 0xFF ||
          command -> acknowledge.receivedReliableSequenceNumber != ENET_HOST_TO_NET_16(1))
        return -1;
      receivedSentTime = command -> acknowledge.receivedSentTime;
      cookie = & peer -> connectingPeers [receivedSentTime & peer -> connectingPeersTimeMask] [sessionID];
      if (cookie -> randomTimeSent != receivedSentTime ||
          cookie -> address.host != host -> receivedAddress.host ||
          cookie -> address.port != host -> receivedAddress.port)
        return -1;
      cookie -> address.host = 0;
      if (host -> serviceTime - cookie -> realTimeSent > host -> connectingPeerTimeout)
        return -1;
      for (iPeer = 0; iPeer < host -> peerCount && duplicatePeers < host -> duplicatePeers; ++ iPeer)
        if (host -> peers [iPeer].address.host == host -> receivedAddress.host)
          ++ duplicatePeers;
      if (duplicatePeers > host -> duplicatePeers)
        return -1;
      peer -> channels = (ENetChannel *) enet_malloc (cookie -> channelCount * sizeof (ENetChannel));
      if (peer -> channels == NULL)
        return -1;
      for (iPeer = 0; iPeer < host -> idlePeers; ++ iPeer)
        if (host -> idlePeersList [iPeer] == peer -> incomingPeerID)
        {
          host -> busyPeersList [host -> peerCount - host -> idlePeers] = peer -> incomingPeerID;
          host -> idlePeersList [iPeer] = host -> idlePeersList [-- host -> idlePeers];
          break;
        }
      for (channel = peer -> channels;
           channel < & peer -> channels [cookie -> channelCount];
         ++ channel)
      {
        channel -> outgoingReliableSequenceNumber = 0;
        channel -> outgoingUnreliableSequenceNumber = 0;
        channel -> incomingReliableSequenceNumber = 0;
        channel -> incomingUnreliableSequenceNumber = 0;

        enet_list_clear (& channel -> incomingReliableCommands);
        enet_list_clear (& channel -> incomingUnreliableCommands);

        channel -> usedReliableWindows = 0;
        memset (channel -> reliableWindows, 0, sizeof (channel -> reliableWindows));
      }
      peer -> state = ENET_PEER_STATE_ACKNOWLEDGING_CONNECT;
      peer -> address = host -> receivedAddress;
      peer -> incomingSessionID = sessionID;
      peer -> channelCount = cookie -> channelCount;
      peer -> connectID = cookie -> connectID;
      peer -> outgoingPeerID = cookie -> outgoingPeerID;
      peer -> incomingBandwidth = cookie -> incomingBandwidth;
      peer -> outgoingBandwidth = cookie -> outgoingBandwidth;
      peer -> packetThrottleInterval = cookie -> packetThrottleInterval;
      peer -> packetThrottleAcceleration = cookie -> packetThrottleAcceleration;
      peer -> packetThrottleDeceleration = cookie -> packetThrottleDeceleration;
      peer -> eventData = cookie -> eventData;
      peer -> outgoingSessionID = cookie -> outgoingSessionID;
      peer -> mtu = cookie -> mtu;
      peer -> windowSize = cookie -> windowSize + 1;
      peer -> outgoingReliableSequenceNumber = 1;
      peer -> packetsSent = 1;
      peer -> outgoingDataTotal = sizeof (ENetProtocolHeader) + sizeof (enet_uint32) * (host -> checksum != 0) + sizeof (ENetProtocolConnect);
      peer -> incomingDataTotal = host -> receivedDataLength;
      peer -> lastSendTime = cookie -> realTimeSent;
      peer -> timedOut = 0;
      receivedSentTime = cookie -> realTimeSent;
      cookieRestored = 1;
      commandNumber = ENET_PROTOCOL_COMMAND_VERIFY_CONNECT;
      enet_free (peer -> connectingPeers);
      peer -> connectingPeers = NULL;
      enet_peer_try_create_own_socket (peer);
      host -> connectsWindow -= ENET_PROTOCOL_TOTAL_SESSIONS * (peer -> connectingPeersTimeMask + 1);
    }
    else
    {
      receivedSentTime = ENET_NET_TO_HOST_16 (command -> acknowledge.receivedSentTime);
      receivedSentTime |= host -> serviceTime & 0xFFFF0000;
      if ((receivedSentTime & 0x8000) > (host -> serviceTime & 0x8000))
          receivedSentTime -= 0x10000;

      if (ENET_TIME_LESS (host -> serviceTime, receivedSentTime))
        return 0;
    }

    roundTripTime = ENET_TIME_DIFFERENCE (host -> serviceTime, receivedSentTime);
    roundTripTime = ENET_MAX (roundTripTime, 1);

    if (peer -> lastReceiveTime > 0)
    {
       enet_uint32 accumRoundTripTime = (peer -> roundTripTime << 8) + peer -> roundTripTimeRemainder;
       enet_uint32 accumRoundTripTimeVariance = (peer -> roundTripTimeVariance << 8) + peer -> roundTripTimeVarianceRemainder;

       enet_peer_throttle (peer, roundTripTime);

       roundTripTime <<= 8;
       accumRoundTripTimeVariance = (accumRoundTripTimeVariance * 3 + ENET_DIFFERENCE (roundTripTime, accumRoundTripTime)) / 4;
       accumRoundTripTime = (accumRoundTripTime * 7 + roundTripTime) / 8;

       peer -> roundTripTime = accumRoundTripTime >> 8;
       peer -> roundTripTimeRemainder = accumRoundTripTime & 0xFF;
       peer -> roundTripTimeVariance = accumRoundTripTimeVariance >> 8;
       peer -> roundTripTimeVarianceRemainder = accumRoundTripTimeVariance & 0xFF;
    }
    else
    {
       peer -> roundTripTime = roundTripTime;
       peer -> roundTripTimeVariance = (roundTripTime + 1) / 2;
    }

    if (peer -> roundTripTime < peer -> lowestRoundTripTime)
      peer -> lowestRoundTripTime = peer -> roundTripTime;

    if (peer -> roundTripTimeVariance > peer -> highestRoundTripTimeVariance) 
      peer -> highestRoundTripTimeVariance = peer -> roundTripTimeVariance;

    if (peer -> packetThrottleEpoch == 0 ||
        ENET_TIME_DIFFERENCE (host -> serviceTime, peer -> packetThrottleEpoch) >= peer -> packetThrottleInterval)
    {
        peer -> lastRoundTripTime = peer -> lowestRoundTripTime;
        peer -> lastRoundTripTimeVariance = ENET_MAX (peer -> highestRoundTripTimeVariance, 2);
        peer -> lowestRoundTripTime = peer -> roundTripTime;
        peer -> highestRoundTripTimeVariance = peer -> roundTripTimeVariance;
        peer -> packetThrottleEpoch = host -> serviceTime;
    }

    peer -> lastReceiveTime = ENET_MAX (host -> serviceTime, 1);
    peer -> earliestTimeout = 0;

    if (!cookieRestored)
    {
      receivedReliableSequenceNumber = ENET_NET_TO_HOST_16 (command -> acknowledge.receivedReliableSequenceNumber);

      commandNumber = enet_protocol_remove_sent_reliable_command (peer, receivedReliableSequenceNumber, command -> header.channelID);
    }

    switch (peer -> state)
    {
    case ENET_PEER_STATE_ACKNOWLEDGING_CONNECT:
       if (commandNumber != ENET_PROTOCOL_COMMAND_VERIFY_CONNECT)
         return -1;

       enet_protocol_notify_connect (host, peer, event);
       break;

    case ENET_PEER_STATE_DISCONNECTING:
       if (commandNumber != ENET_PROTOCOL_COMMAND_DISCONNECT)
         return -1;

       enet_protocol_notify_disconnect (host, peer, event);
       break;

    case ENET_PEER_STATE_DISCONNECT_LATER:
       if (enet_list_empty (& peer -> outgoingCommands) &&
           enet_list_empty (& peer -> sentReliableCommands))
         enet_peer_disconnect (peer, peer -> eventData);
       break;

    default:
       break;
    }
   
    return 0;
}

static int
enet_protocol_handle_verify_connect (ENetHost * host, ENetEvent * event, ENetPeer * peer, const ENetProtocol * command)
{
    enet_uint32 mtu, windowSize;
    size_t channelCount;

    if (peer -> state != ENET_PEER_STATE_CONNECTING)
      return 0;

    channelCount = ENET_NET_TO_HOST_32 (command -> verifyConnect.channelCount);

    if (channelCount < ENET_PROTOCOL_MINIMUM_CHANNEL_COUNT || channelCount > ENET_PROTOCOL_MAXIMUM_CHANNEL_COUNT ||
        ENET_NET_TO_HOST_32 (command -> verifyConnect.packetThrottleInterval) != peer -> packetThrottleInterval ||
        ENET_NET_TO_HOST_32 (command -> verifyConnect.packetThrottleAcceleration) != peer -> packetThrottleAcceleration ||
        ENET_NET_TO_HOST_32 (command -> verifyConnect.packetThrottleDeceleration) != peer -> packetThrottleDeceleration ||
        command -> verifyConnect.connectID != peer -> connectID)
    {
        peer -> eventData = 0;

        enet_protocol_dispatch_state (host, peer, ENET_PEER_STATE_ZOMBIE);

        return -1;
    }

    enet_protocol_remove_sent_reliable_command (peer, 1, 0xFF);
    
    if (channelCount < peer -> channelCount)
      peer -> channelCount = channelCount;

    peer -> outgoingPeerID = ENET_NET_TO_HOST_16 (command -> verifyConnect.outgoingPeerID);
    peer -> incomingSessionID = command -> verifyConnect.incomingSessionID;
    peer -> outgoingSessionID = command -> verifyConnect.outgoingSessionID;

    mtu = ENET_NET_TO_HOST_32 (command -> verifyConnect.mtu);

    if (mtu < ENET_PROTOCOL_MINIMUM_MTU)
      mtu = ENET_PROTOCOL_MINIMUM_MTU;
    else 
    if (mtu > ENET_PROTOCOL_MAXIMUM_MTU)
      mtu = ENET_PROTOCOL_MAXIMUM_MTU;

    if (mtu < peer -> mtu)
      peer -> mtu = mtu;

    windowSize = ENET_NET_TO_HOST_32 (command -> verifyConnect.windowSize);

    if (windowSize < ENET_PROTOCOL_MINIMUM_WINDOW_SIZE)
      windowSize = ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;

    if (windowSize > ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE)
      windowSize = ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE;

    if (windowSize < peer -> windowSize)
      peer -> windowSize = windowSize;

    peer -> incomingBandwidth = ENET_NET_TO_HOST_32 (command -> verifyConnect.incomingBandwidth);
    peer -> outgoingBandwidth = ENET_NET_TO_HOST_32 (command -> verifyConnect.outgoingBandwidth);

    enet_protocol_notify_connect (host, peer, event);
    return 0;
}

static int
enet_protocol_handle_incoming_commands (ENetHost * host, ENetEvent * event)
{
    ENetProtocolHeader * header;
    ENetProtocol * command;
    ENetPeer * peer;
    enet_uint8 * currentData;
    size_t headerSize;
    enet_uint16 peerID, flags;
    enet_uint8 sessionID;

    if (host -> receivedDataLength < (size_t) & ((ENetProtocolHeader *) 0) -> sentTime)
      return 0;

    header = (ENetProtocolHeader *) host -> receivedData;

    peerID = ENET_NET_TO_HOST_16 (header -> peerID);
    sessionID = (peerID & ENET_PROTOCOL_HEADER_SESSION_MASK) >> ENET_PROTOCOL_HEADER_SESSION_SHIFT;
    flags = peerID & ENET_PROTOCOL_HEADER_FLAG_MASK;
    peerID &= ~ (ENET_PROTOCOL_HEADER_FLAG_MASK | ENET_PROTOCOL_HEADER_SESSION_MASK);

    headerSize = (flags & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME ? sizeof (ENetProtocolHeader) : (size_t) & ((ENetProtocolHeader *) 0) -> sentTime);
    if (host -> checksum != NULL)
      headerSize += sizeof (enet_uint32);

    if (peerID == ENET_PROTOCOL_MAXIMUM_PEER_ID)
      peer = NULL;
    else
    if (peerID >= host -> peerCount)
      return 0;
    else
    {
       peer = & host -> peers [peerID];

       if (peer -> state == ENET_PEER_STATE_DISCONNECTED)
       {
         if (! host -> connectingPeerTimeout || !peer -> connectingPeers)
           return 0;
       }
       else if (peer -> state == ENET_PEER_STATE_ZOMBIE ||
           (peer -> ownSocket != ENET_SOCKET_NULL ? peer -> incomingPeerID != peerID :
           (host -> receivedAddress.host != peer -> address.host ||
             host -> receivedAddress.port != peer -> address.port) &&
             peer -> address.host != ENET_HOST_BROADCAST) ||
           (peer -> outgoingPeerID < ENET_PROTOCOL_MAXIMUM_PEER_ID &&
            sessionID != peer -> incomingSessionID))
         return 0;
    }
 
    if (flags & ENET_PROTOCOL_HEADER_FLAG_COMPRESSED)
    {
        size_t originalSize;
        if (host -> compressor.context == NULL || host -> compressor.decompress == NULL)
          return 0;

        originalSize = host -> compressor.decompress (host -> compressor.context,
                                    host -> receivedData + headerSize, 
                                    host -> receivedDataLength - headerSize, 
                                    host -> packetData [1] + headerSize, 
                                    sizeof (host -> packetData [1]) - headerSize);
        if (originalSize <= 0 || originalSize > sizeof (host -> packetData [1]) - headerSize)
          return 0;

        memcpy (host -> packetData [1], header, headerSize);
        host -> receivedData = host -> packetData [1];
        host -> receivedDataLength = headerSize + originalSize;
    }

    if (host -> checksum != NULL && !(host -> connectingPeerTimeout && peer -> state == ENET_PEER_STATE_DISCONNECTED))
    {
        enet_uint32 * checksum = (enet_uint32 *) & host -> receivedData [headerSize - sizeof (enet_uint32)],
                    desiredChecksum = * checksum;
        ENetBuffer buffer;

        * checksum = peer != NULL ? peer -> connectID : 0;

        buffer.data = host -> receivedData;
        buffer.dataLength = host -> receivedDataLength;

        if (host -> checksum (& buffer, 1) != desiredChecksum)
          return 0;
    }
       
    if (peer != NULL && peer -> state != ENET_PEER_STATE_DISCONNECTED)
    {
       peer -> address.host = host -> receivedAddress.host;
       peer -> address.port = host -> receivedAddress.port;
       peer -> incomingDataTotal += host -> receivedDataLength;
    }
    
    currentData = host -> receivedData + headerSize;
  
    while (currentData < & host -> receivedData [host -> receivedDataLength])
    {
       enet_uint8 commandNumber;
       size_t commandSize;

       command = (ENetProtocol *) currentData;

       if (currentData + sizeof (ENetProtocolCommandHeader) > & host -> receivedData [host -> receivedDataLength])
         break;

       commandNumber = command -> header.command & ENET_PROTOCOL_COMMAND_MASK;
       if (commandNumber >= ENET_PROTOCOL_COMMAND_COUNT) 
         break;
       
       commandSize = commandSizes [commandNumber];
       if (commandSize == 0 || currentData + commandSize > & host -> receivedData [host -> receivedDataLength])
         break;

       currentData += commandSize;

       if (peer == NULL && (commandNumber != ENET_PROTOCOL_COMMAND_CONNECT || currentData < & host -> receivedData [host -> receivedDataLength]))
         break;
         
       command -> header.reliableSequenceNumber = ENET_NET_TO_HOST_16 (command -> header.reliableSequenceNumber);

       switch (commandNumber)
       {
       case ENET_PROTOCOL_COMMAND_ACKNOWLEDGE:
          if (enet_protocol_handle_acknowledge (host, event, peer, command, sessionID))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_CONNECT:
          if (peer != NULL)
            goto commandError;
          enet_protocol_handle_connect (host, header, command);
          return 0;

       case ENET_PROTOCOL_COMMAND_VERIFY_CONNECT:
          if (enet_protocol_handle_verify_connect (host, event, peer, command))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_DISCONNECT:
          if (enet_protocol_handle_disconnect (host, peer, command))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_PING:
          if (enet_protocol_handle_ping (host, peer, command))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_SEND_RELIABLE:
          if (enet_protocol_handle_send_reliable (host, peer, command, & currentData))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE:
          if (enet_protocol_handle_send_unreliable (host, peer, command, & currentData))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED:
          if (enet_protocol_handle_send_unsequenced (host, peer, command, & currentData))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_SEND_FRAGMENT:
          if (enet_protocol_handle_send_fragment (host, peer, command, & currentData))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_BANDWIDTH_LIMIT:
          if (enet_protocol_handle_bandwidth_limit (host, peer, command))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_THROTTLE_CONFIGURE:
          if (enet_protocol_handle_throttle_configure (host, peer, command))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT:
          if (enet_protocol_handle_send_unreliable_fragment (host, peer, command, & currentData))
            goto commandError;
          break;

       default:
          goto commandError;
       }

       if (peer != NULL &&
           (command -> header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE) != 0)
       {
           enet_uint16 sentTime;

           if (! (flags & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME))
             break;

           sentTime = ENET_NET_TO_HOST_16 (header -> sentTime);

           switch (peer -> state)
           {
           case ENET_PEER_STATE_DISCONNECTING:
           case ENET_PEER_STATE_ACKNOWLEDGING_CONNECT:
           case ENET_PEER_STATE_DISCONNECTED:
           case ENET_PEER_STATE_ZOMBIE:
              break;

           case ENET_PEER_STATE_ACKNOWLEDGING_DISCONNECT:
              if ((command -> header.command & ENET_PROTOCOL_COMMAND_MASK) == ENET_PROTOCOL_COMMAND_DISCONNECT)
                enet_peer_queue_acknowledgement (peer, command, sentTime);
              break;

           default:   
              enet_peer_queue_acknowledgement (peer, command, sentTime);        
              break;
           }
       }
    }

commandError:
    if (event != NULL && event -> type != ENET_EVENT_TYPE_NONE)
      return 1;

    return 0;
}
 
static int
enet_protocol_receive_incoming_commands (ENetHost * host, ENetEvent * event, enet_uint32 timeout)
{
    size_t iPeer;
    for (iPeer = 0; iPeer <= host -> peerCount - host -> idlePeers; ++ iPeer)
    {
        ENetPeer * peer = NULL;
        int packets = 0;
        if (iPeer < host -> peerCount - host -> idlePeers)
        {
            peer = & host -> peers [host -> busyPeersList [iPeer]];
            if (peer -> ownSocket == ENET_SOCKET_NULL)
                continue;
        }

        for (;;)
        {
            int receivedLength;
            ENetBuffer buffer;

            buffer.data = host -> packetData [0];
            buffer.dataLength = host -> mtu;
            if (! peer)
            {
                if (! (++ packets % 1000) && enet_time_get () >= timeout)
                    return 0;
                receivedLength = enet_socket_receive (host -> socket,
                                                            & host -> receivedAddress,
                                                            & buffer,
                                                            1);
            }
            else
            {
                receivedLength = enet_socket_receive (peer -> ownSocket, NULL, & buffer, 1);
                host -> receivedAddress = peer -> address;
            }

            if (receivedLength == -2)
                continue;

            if (receivedLength <= 0)
                break;

            host -> receivedData = host -> packetData [0];
            host -> receivedDataLength = receivedLength;

            host -> totalReceivedData += receivedLength;
            host -> totalReceivedPackets ++;

            if (host -> intercept != NULL)
            {
                switch (host -> intercept (host, event))
                {
                case 1:
                    if (event != NULL && event -> type != ENET_EVENT_TYPE_NONE)
                        return 1;

                    continue;

                case -1:
                    return -1;

                default:
                    break;
                }
            }

            switch (enet_protocol_handle_incoming_commands (host, event))
            {
            case 1:
                return 1;

            case -1:
                return -1;

            default:
                break;
            }
        }
    }

    return 0;
}

static void
enet_protocol_send_acknowledgements (ENetHost * host, ENetPeer * peer)
{
    ENetProtocol * command = & host -> commands [host -> commandCount];
    ENetBuffer * buffer = & host -> buffers [host -> bufferCount];
    ENetAcknowledgement * acknowledgement;
    ENetListIterator currentAcknowledgement;
    enet_uint16 reliableSequenceNumber;
 
    currentAcknowledgement = enet_list_begin (& peer -> acknowledgements);
         
    while (currentAcknowledgement != enet_list_end (& peer -> acknowledgements))
    {
       if (command >= & host -> commands [sizeof (host -> commands) / sizeof (ENetProtocol)] ||
           buffer >= & host -> buffers [sizeof (host -> buffers) / sizeof (ENetBuffer)] ||
           peer -> mtu - host -> packetSize < sizeof (ENetProtocolAcknowledge))
       {
          host -> continueSending = 1;

          break;
       }

       acknowledgement = (ENetAcknowledgement *) currentAcknowledgement;
 
       currentAcknowledgement = enet_list_next (currentAcknowledgement);

       buffer -> data = command;
       buffer -> dataLength = sizeof (ENetProtocolAcknowledge);

       host -> packetSize += buffer -> dataLength;

       reliableSequenceNumber = ENET_HOST_TO_NET_16 (acknowledgement -> command.header.reliableSequenceNumber);
  
       command -> header.command = ENET_PROTOCOL_COMMAND_ACKNOWLEDGE;
       command -> header.channelID = acknowledgement -> command.header.channelID;
       command -> header.reliableSequenceNumber = reliableSequenceNumber;
       command -> acknowledge.receivedReliableSequenceNumber = reliableSequenceNumber;
       command -> acknowledge.receivedSentTime = ENET_HOST_TO_NET_16 (acknowledgement -> sentTime);
  
       if ((acknowledgement -> command.header.command & ENET_PROTOCOL_COMMAND_MASK) == ENET_PROTOCOL_COMMAND_DISCONNECT)
         enet_protocol_dispatch_state (host, peer, ENET_PEER_STATE_ZOMBIE);

       enet_list_remove (& acknowledgement -> acknowledgementList);
       enet_free (acknowledgement);

       ++ command;
       ++ buffer;
    }

    host -> commandCount = command - host -> commands;
    host -> bufferCount = buffer - host -> buffers;
}

static int
enet_protocol_check_timeouts (ENetHost * host, ENetPeer * peer, ENetEvent * event)
{
    ENetOutgoingCommand * outgoingCommand;
    ENetListIterator currentCommand, insertPosition;

    currentCommand = enet_list_begin (& peer -> sentReliableCommands);
    insertPosition = enet_list_begin (& peer -> outgoingCommands);

    while (currentCommand != enet_list_end (& peer -> sentReliableCommands))
    {
       outgoingCommand = (ENetOutgoingCommand *) currentCommand;

       currentCommand = enet_list_next (currentCommand);

       if (ENET_TIME_DIFFERENCE (host -> serviceTime, outgoingCommand -> sentTime) < outgoingCommand -> roundTripTimeout)
         continue;

       if (peer -> earliestTimeout == 0 ||
           ENET_TIME_LESS (outgoingCommand -> sentTime, peer -> earliestTimeout))
         peer -> earliestTimeout = outgoingCommand -> sentTime;

       if (peer -> earliestTimeout != 0 && ! host -> noTimeouts &&
             (ENET_TIME_DIFFERENCE (host -> serviceTime, peer -> earliestTimeout) >= peer -> timeoutMaximum ||
               (outgoingCommand -> roundTripTimeout >= outgoingCommand -> roundTripTimeoutLimit &&
                 ENET_TIME_DIFFERENCE (host -> serviceTime, peer -> earliestTimeout) >= peer -> timeoutMinimum)))
       {
		  peer -> timedOut = 1;
          enet_protocol_notify_disconnect (host, peer, event);

          return 1;
       }

       if (outgoingCommand -> packet != NULL)
         peer -> reliableDataInTransit -= outgoingCommand -> fragmentLength;
          
       ++ peer -> packetsLost;

       outgoingCommand -> roundTripTimeout *= host -> noTimeouts ? 1 : 2;

       enet_list_insert (insertPosition, enet_list_remove (& outgoingCommand -> outgoingCommandList));

       if (currentCommand == enet_list_begin (& peer -> sentReliableCommands) &&
           ! enet_list_empty (& peer -> sentReliableCommands))
       {
          outgoingCommand = (ENetOutgoingCommand *) currentCommand;

          peer -> nextTimeout = outgoingCommand -> sentTime + outgoingCommand -> roundTripTimeout;
       }
    }
    
    return 0;
}

static int
enet_protocol_check_outgoing_commands (ENetHost * host, ENetPeer * peer)
{
    ENetProtocol * command = & host -> commands [host -> commandCount];
    ENetBuffer * buffer = & host -> buffers [host -> bufferCount];
    ENetOutgoingCommand * outgoingCommand;
    ENetListIterator currentCommand;
    ENetChannel *channel;
    enet_uint16 reliableWindow;
    size_t commandSize;
    int windowExceeded = 0, windowWrap = 0, canPing = 1;

    currentCommand = enet_list_begin (& peer -> outgoingCommands);
    
    while (currentCommand != enet_list_end (& peer -> outgoingCommands))
    {
       outgoingCommand = (ENetOutgoingCommand *) currentCommand;

       if (outgoingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE)
       {
          channel = outgoingCommand -> command.header.channelID < peer -> channelCount ? & peer -> channels [outgoingCommand -> command.header.channelID] : NULL;
          reliableWindow = outgoingCommand -> reliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;
          if (channel != NULL)
          {
             if (! windowWrap &&      
                  outgoingCommand -> sendAttempts < 1 && 
                  ! (outgoingCommand -> reliableSequenceNumber % ENET_PEER_RELIABLE_WINDOW_SIZE) &&
                  (channel -> reliableWindows [(reliableWindow + ENET_PEER_RELIABLE_WINDOWS - 1) % ENET_PEER_RELIABLE_WINDOWS] >= ENET_PEER_RELIABLE_WINDOW_SIZE ||
                    channel -> usedReliableWindows & ((((1 << ENET_PEER_FREE_RELIABLE_WINDOWS) - 1) << reliableWindow) | 
                      (((1 << ENET_PEER_FREE_RELIABLE_WINDOWS) - 1) >> (ENET_PEER_RELIABLE_WINDOWS - reliableWindow)))))
                windowWrap = 1;
             if (windowWrap)
             {
                currentCommand = enet_list_next (currentCommand);
 
                continue;
             }
          }
 
          if (outgoingCommand -> packet != NULL)
          {
             if (! windowExceeded)
             {
                enet_uint32 windowSize = (peer -> packetThrottle * peer -> windowSize) / ENET_PEER_PACKET_THROTTLE_SCALE;
             
                if (peer -> reliableDataInTransit + outgoingCommand -> fragmentLength > ENET_MAX (windowSize, peer -> mtu))
                  windowExceeded = 1;
             }
             if (windowExceeded)
             {
                currentCommand = enet_list_next (currentCommand);

                continue;
             }
          }

          canPing = 0;
       }

       commandSize = commandSizes [outgoingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_MASK];
       if (command >= & host -> commands [sizeof (host -> commands) / sizeof (ENetProtocol)] ||
           buffer + 1 >= & host -> buffers [sizeof (host -> buffers) / sizeof (ENetBuffer)] ||
           peer -> mtu - host -> packetSize < commandSize ||
           (outgoingCommand -> packet != NULL && 
             (enet_uint16) (peer -> mtu - host -> packetSize) < (enet_uint16) (commandSize + outgoingCommand -> fragmentLength)))
       {
          host -> continueSending = 1;
          
          break;
       }

       currentCommand = enet_list_next (currentCommand);

       if (outgoingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE)
       {
          if (channel != NULL && outgoingCommand -> sendAttempts < 1)
          {
             channel -> usedReliableWindows |= 1 << reliableWindow;
             ++ channel -> reliableWindows [reliableWindow];
          }

          ++ outgoingCommand -> sendAttempts;
 
          if (outgoingCommand -> roundTripTimeout == 0)
          {
             outgoingCommand -> roundTripTimeout = peer -> roundTripTime + 4 * peer -> roundTripTimeVariance;
             outgoingCommand -> roundTripTimeoutLimit = peer -> timeoutLimit * outgoingCommand -> roundTripTimeout;
          }

          if (enet_list_empty (& peer -> sentReliableCommands))
            peer -> nextTimeout = host -> serviceTime + outgoingCommand -> roundTripTimeout;

          enet_list_insert (enet_list_end (& peer -> sentReliableCommands),
                            enet_list_remove (& outgoingCommand -> outgoingCommandList));

          outgoingCommand -> sentTime = host -> serviceTime;

          host -> headerFlags |= ENET_PROTOCOL_HEADER_FLAG_SENT_TIME;

          peer -> reliableDataInTransit += outgoingCommand -> fragmentLength;
       }
       else
       {
          if (outgoingCommand -> packet != NULL && outgoingCommand -> fragmentOffset == 0)
          {
             peer -> packetThrottleCounter += ENET_PEER_PACKET_THROTTLE_COUNTER;
             peer -> packetThrottleCounter %= ENET_PEER_PACKET_THROTTLE_SCALE;

             if (peer -> packetThrottleCounter > peer -> packetThrottle)
             {
                enet_uint16 reliableSequenceNumber = outgoingCommand -> reliableSequenceNumber,
                            unreliableSequenceNumber = outgoingCommand -> unreliableSequenceNumber;
                for (;;)
                {
                   -- outgoingCommand -> packet -> referenceCount;

                   if (outgoingCommand -> packet -> referenceCount == 0)
                     enet_packet_destroy (outgoingCommand -> packet);

                   enet_list_remove (& outgoingCommand -> outgoingCommandList);
                   enet_free (outgoingCommand);

                   if (currentCommand == enet_list_end (& peer -> outgoingCommands))
                     break;

                   outgoingCommand = (ENetOutgoingCommand *) currentCommand;
                   if (outgoingCommand -> reliableSequenceNumber != reliableSequenceNumber ||
                       outgoingCommand -> unreliableSequenceNumber != unreliableSequenceNumber)
                     break;

                   currentCommand = enet_list_next (currentCommand);
                }

                continue;
             }
          }

          enet_list_remove (& outgoingCommand -> outgoingCommandList);

          if (outgoingCommand -> packet != NULL)
            enet_list_insert (enet_list_end (& peer -> sentUnreliableCommands), outgoingCommand);
       }

       buffer -> data = command;
       buffer -> dataLength = commandSize;

       host -> packetSize += buffer -> dataLength;

       * command = outgoingCommand -> command;

       if (outgoingCommand -> packet != NULL)
       {
          ++ buffer;
          
          buffer -> data = outgoingCommand -> packet -> data + outgoingCommand -> fragmentOffset;
          buffer -> dataLength = outgoingCommand -> fragmentLength;

          host -> packetSize += outgoingCommand -> fragmentLength;
       }
       else
       if (! (outgoingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE))
         enet_free (outgoingCommand);

       ++ peer -> packetsSent;
        
       ++ command;
       ++ buffer;
    }

    host -> commandCount = command - host -> commands;
    host -> bufferCount = buffer - host -> buffers;

    if (peer -> state == ENET_PEER_STATE_DISCONNECT_LATER &&
        enet_list_empty (& peer -> outgoingCommands) &&
        enet_list_empty (& peer -> sentReliableCommands) &&
        enet_list_empty (& peer -> sentUnreliableCommands))
      enet_peer_disconnect (peer, peer -> eventData);

    return canPing;
}

static int
enet_protocol_send_outgoing_commands (ENetHost * host, ENetEvent * event, int checkForTimeouts)
{
    enet_uint8 headerData [sizeof (ENetProtocolHeader) + sizeof (enet_uint32)];
    ENetProtocolHeader * header = (ENetProtocolHeader *) headerData;
    ENetPeer * currentPeer;
    int sentLength;
    size_t shouldCompress = 0;
    size_t iPeer;
 
    host -> continueSending = 1;

    while (host -> continueSending)
    for (host -> continueSending = 0, iPeer = 0; iPeer < host -> peerCount - host -> idlePeers; ++ iPeer)
    {
        currentPeer = & host -> peers [host -> busyPeersList [iPeer]];
        if (currentPeer -> state == ENET_PEER_STATE_ZOMBIE)
          continue;

        host -> headerFlags = 0;
        host -> commandCount = 0;
        host -> bufferCount = 1;
        host -> packetSize = sizeof (ENetProtocolHeader);

        if (! enet_list_empty (& currentPeer -> acknowledgements))
          enet_protocol_send_acknowledgements (host, currentPeer);

        if (checkForTimeouts != 0 &&
            ! enet_list_empty (& currentPeer -> sentReliableCommands) &&
            ENET_TIME_GREATER_EQUAL (host -> serviceTime, currentPeer -> nextTimeout) &&
            enet_protocol_check_timeouts (host, currentPeer, event) == 1)
        {
            if (event != NULL && event -> type != ENET_EVENT_TYPE_NONE)
              return 1;
            else
              continue;
        }

        if ((enet_list_empty (& currentPeer -> outgoingCommands) ||
              enet_protocol_check_outgoing_commands (host, currentPeer)) &&
            enet_list_empty (& currentPeer -> sentReliableCommands) &&
            ENET_TIME_DIFFERENCE (host -> serviceTime, currentPeer -> lastReceiveTime) >= currentPeer -> pingInterval &&
            currentPeer -> mtu - host -> packetSize >= sizeof (ENetProtocolPing))
        { 
            enet_peer_ping (currentPeer);
            enet_protocol_check_outgoing_commands (host, currentPeer);
        }

        if (host -> commandCount == 0)
          continue;

        if (currentPeer -> packetLossEpoch == 0)
          currentPeer -> packetLossEpoch = host -> serviceTime;
        else
        if (ENET_TIME_DIFFERENCE (host -> serviceTime, currentPeer -> packetLossEpoch) >= ENET_PEER_PACKET_LOSS_INTERVAL &&
            currentPeer -> packetsSent > 0)
        {
           enet_uint32 packetLoss = currentPeer -> packetsLost * ENET_PEER_PACKET_LOSS_SCALE / currentPeer -> packetsSent;

#ifdef ENET_DEBUG
           printf ("peer %u: %f%%+-%f%% packet loss, %u+-%u ms round trip time, %f%% throttle, %u outgoing, %u/%u incoming\n", currentPeer -> incomingPeerID, currentPeer -> packetLoss / (float) ENET_PEER_PACKET_LOSS_SCALE, currentPeer -> packetLossVariance / (float) ENET_PEER_PACKET_LOSS_SCALE, currentPeer -> roundTripTime, currentPeer -> roundTripTimeVariance, currentPeer -> packetThrottle / (float) ENET_PEER_PACKET_THROTTLE_SCALE, enet_list_size (& currentPeer -> outgoingCommands), currentPeer -> channels != NULL ? enet_list_size (& currentPeer -> channels -> incomingReliableCommands) : 0, currentPeer -> channels != NULL ? enet_list_size (& currentPeer -> channels -> incomingUnreliableCommands) : 0);
#endif

           currentPeer -> packetLossVariance = (currentPeer -> packetLossVariance * 3 + ENET_DIFFERENCE (packetLoss, currentPeer -> packetLoss)) / 4;
           currentPeer -> packetLoss = (currentPeer -> packetLoss * 7 + packetLoss) / 8;

           currentPeer -> packetLossEpoch = host -> serviceTime;
           currentPeer -> packetsSent = 0;
           currentPeer -> packetsLost = 0;
        }

        host -> buffers -> data = headerData;
        if (host -> headerFlags & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME)
        {
            header -> sentTime = ENET_HOST_TO_NET_16 (host -> serviceTime & 0xFFFF);

            host -> buffers -> dataLength = sizeof (ENetProtocolHeader);
        }
        else
          host -> buffers -> dataLength = (size_t) & ((ENetProtocolHeader *) 0) -> sentTime;

        shouldCompress = 0;
        if (host -> compressor.context != NULL && host -> compressor.compress != NULL)
        {
            size_t originalSize = host -> packetSize - sizeof(ENetProtocolHeader),
                   compressedSize = host -> compressor.compress (host -> compressor.context,
                                        & host -> buffers [1], host -> bufferCount - 1,
                                        originalSize,
                                        host -> packetData [1],
                                        originalSize);
            if (compressedSize > 0 && compressedSize < originalSize)
            {
                host -> headerFlags |= ENET_PROTOCOL_HEADER_FLAG_COMPRESSED;
                shouldCompress = compressedSize;
#ifdef ENET_DEBUG_COMPRESS
                printf ("peer %u: compressed %u -> %u (%u%%)\n", currentPeer -> incomingPeerID, originalSize, compressedSize, (compressedSize * 100) / originalSize);
#endif
            }
        }

        if (currentPeer -> outgoingPeerID < ENET_PROTOCOL_MAXIMUM_PEER_ID)
          host -> headerFlags |= currentPeer -> outgoingSessionID << ENET_PROTOCOL_HEADER_SESSION_SHIFT;
        header -> peerID = ENET_HOST_TO_NET_16 (currentPeer -> outgoingPeerID | host -> headerFlags);
        if (host -> checksum != NULL)
        {
            enet_uint32 * checksum = (enet_uint32 *) & headerData [host -> buffers -> dataLength];
            * checksum = currentPeer -> outgoingPeerID < ENET_PROTOCOL_MAXIMUM_PEER_ID ? currentPeer -> connectID : 0;
            host -> buffers -> dataLength += sizeof (enet_uint32);
            * checksum = host -> checksum (host -> buffers, host -> bufferCount);
        }

        if (shouldCompress > 0)
        {
            host -> buffers [1].data = host -> packetData [1];
            host -> buffers [1].dataLength = shouldCompress;
            host -> bufferCount = 2;
        }

        currentPeer -> lastSendTime = host -> serviceTime;

#ifdef SO_REUSEPORT
        if (currentPeer -> ownSocket != ENET_SOCKET_NULL)
            sentLength = enet_socket_send (currentPeer -> ownSocket, NULL, host -> buffers, host -> bufferCount);
        else
#endif
        sentLength = enet_socket_send (host -> socket, & currentPeer -> address, host -> buffers, host -> bufferCount);

        enet_protocol_remove_sent_unreliable_commands (currentPeer);

        if (sentLength < 0)
          return -1;

        host -> totalSentData += sentLength;
        host -> totalSentPackets ++;
    }
   
    return 0;
}

/** Sends any queued packets on the host specified to its designated peers.

    @param host   host to flush
    @remarks this function need only be used in circumstances where one wishes to send queued packets earlier than in a call to enet_host_service().
    @ingroup host
*/
void
enet_host_flush (ENetHost * host)
{
    host -> serviceTime = enet_time_get ();

    enet_protocol_send_outgoing_commands (host, NULL, 0);
}

/** Checks for any queued events on the host and dispatches one if available.

    @param host    host to check for events
    @param event   an event structure where event details will be placed if available
    @retval > 0 if an event was dispatched
    @retval 0 if no events are available
    @retval < 0 on failure
    @ingroup host
*/
int
enet_host_check_events (ENetHost * host, ENetEvent * event)
{
    if (event == NULL) return -1;

    event -> type = ENET_EVENT_TYPE_NONE;
    event -> peer = NULL;
    event -> packet = NULL;

    return enet_protocol_dispatch_incoming_commands (host, event);
}

/** Waits for events on the host specified and shuttles packets between
    the host and its peers.

    @param host    host to service
    @param event   an event structure where event details will be placed if one occurs
                   if event == NULL then no events will be delivered
    @param timeout number of milliseconds that ENet should wait for events
    @retval > 0 if an event occurred within the specified time limit
    @retval 0 if no event occurred
    @retval < 0 on failure
    @remarks enet_host_service should be called fairly regularly for adequate performance
    @ingroup host
*/
int
enet_host_service (ENetHost * host, ENetEvent * event, enet_uint32 timeout)
{
    enet_uint32 waitCondition;

    if (event != NULL)
    {
        event -> type = ENET_EVENT_TYPE_NONE;
        event -> peer = NULL;
        event -> packet = NULL;

        switch (enet_protocol_dispatch_incoming_commands (host, event))
        {
        case 1:
            return 1;

        case -1:
#ifdef ENET_DEBUG
            perror ("Error dispatching incoming packets");
#endif

            return -1;

        default:
            break;
        }
    }

    host -> serviceTime = enet_time_get ();
    if (host -> connectingPeerTimeout)
    {
        while (host -> serviceTime - host -> connectsDataIndexTimestamp > ENET_HOST_CONNECTS_TIME_DELTA)
        {
            host -> connectsDataIndex = (host -> connectsDataIndex + 1) % (host -> connectingPeerTimeout / ENET_HOST_CONNECTS_TIME_DELTA + 1);
            host -> connectsInWindow -= host -> connectsData [host -> connectsDataIndex];
            host -> connectsData [host -> connectsDataIndex] = 0;
            host -> connectsDataIndexTimestamp += ENET_HOST_CONNECTS_TIME_DELTA;
        }
        if (! host -> connectsInWindow && host -> connectsWindow * sizeof (ENetConnectingPeer) > ENET_HOST_CONNECTS_CLEANUP_SIZE)
        {
            size_t iPeer;
            for (iPeer = 0; iPeer < host -> idlePeers; ++ iPeer)
            {
                ENetPeer * peer = & host -> peers [host -> idlePeersList [iPeer]];
                if (peer -> connectingPeers)
                {
                    enet_free (peer -> connectingPeers);
                    peer -> connectingPeers = NULL;
                }
            }
            host -> connectsWindow = 0;
        }
    }
    
    timeout += host -> serviceTime;

    do
    {
       if (ENET_TIME_DIFFERENCE (host -> serviceTime, host -> bandwidthThrottleEpoch) >= ENET_HOST_BANDWIDTH_THROTTLE_INTERVAL)
         enet_host_bandwidth_throttle (host);

       switch (enet_protocol_send_outgoing_commands (host, event, 1))
       {
       case 1:
          return 1;

       case -1:
#ifdef ENET_DEBUG
          perror ("Error sending outgoing packets");
#endif

          return -1;

       default:
          break;
       }

       switch (enet_protocol_receive_incoming_commands (host, event, timeout))
       {
       case 1:
          return 1;

       case -1:
#ifdef ENET_DEBUG
          perror ("Error receiving incoming packets");
#endif

          return -1;

       default:
          break;
       }

       switch (enet_protocol_send_outgoing_commands (host, event, 1))
       {
       case 1:
          return 1;

       case -1:
#ifdef ENET_DEBUG
          perror ("Error sending outgoing packets");
#endif

          return -1;

       default:
          break;
       }

       if (event != NULL)
       {
          switch (enet_protocol_dispatch_incoming_commands (host, event))
          {
          case 1:
             return 1;

          case -1:
#ifdef ENET_DEBUG
             perror ("Error dispatching incoming packets");
#endif

             return -1;

          default:
             break;
          }
       }

       if (ENET_TIME_GREATER_EQUAL (host -> serviceTime, timeout))
         return 0;

       do
       {
          host -> serviceTime = enet_time_get ();

          if (ENET_TIME_GREATER_EQUAL (host -> serviceTime, timeout))
            return 0;

          waitCondition = ENET_SOCKET_WAIT_RECEIVE | ENET_SOCKET_WAIT_INTERRUPT;

          if (enet_socket_wait (host -> socket, & waitCondition, ENET_TIME_DIFFERENCE (timeout, host -> serviceTime)) != 0)
            return -1;
       }
       while (waitCondition & ENET_SOCKET_WAIT_INTERRUPT);

       host -> serviceTime = enet_time_get ();
    } while (waitCondition & ENET_SOCKET_WAIT_RECEIVE);

    return 0; 
}

