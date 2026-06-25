/**
 * @file bgp-ns3-socket-out.h
 * @author Nato Morichika <nat@nat.moe>
 * @brief Simple container class for FSM to send data to peer.
 * @version 0.1
 * @date 2019-07-15
 * 
 * @copyright Copyright (c) 2019
 * 
 */
#ifndef BGP_NS3_SOCKET_OUT_H
#define BGP_NS3_SOCKET_OUT_H

#include <libbgp/bgp-out-handler.h>
#include <vector>
#include "ns3/socket.h"
#include "ns3/simple-ref-count.h"
#include "ns3/callback.h"
#include "ns3/event-id.h"
#include "ns3/nstime.h"

namespace ns3 {

/**
 * @brief Simple container class for FSM to send data to peer.
 * 
 */
class BgpNs3SocketOut : public libbgp::BgpOutHandler, public SimpleRefCount<BgpNs3SocketOut>
{
public:
  BgpNs3SocketOut (Ptr<Socket> socket, Callback<void, Ptr<Socket>, int, int> state_change_cb);
  bool handleOut (const uint8_t *buffer, size_t length);
  void notifyStateChange (int old_state, int new_state);
  void setStateChangeCallback (Callback<void, Ptr<Socket>, int, int> state_change_cb);
  // msg_cb(socket, type, length, has_nlri, has_withdrawals) — called once per outgoing BGP message.
  void SetMessageCallback (Callback<void, Ptr<Socket>, uint8_t, size_t, bool, bool> msg_cb);
  void SetMrai (Time mrai);
  // Called by Session::Drop() to cancel any pending flush and null the socket reference.
  void DisconnectSocket ();

private:
  struct PendingMessage
  {
    std::vector<uint8_t> bytes;
    uint8_t type;
    bool has_nlri;
    bool has_withdrawals;
  };

  void ScheduleUpdateFlush ();
  void FlushPendingUpdate ();

  Ptr<Socket> _socket;
  bool _disconnected;
  Callback<void, Ptr<Socket>, int, int> _state_change_cb;
  Callback<void, Ptr<Socket>, uint8_t, size_t, bool, bool> _msg_cb;
  Time _mrai;
  Time _next_update_send_time;
  EventId _update_flush_event;
  bool _has_pending_update;
  PendingMessage _pending_update;
};

} // namespace ns3

#endif //BGP_NS3_SOCKET_OUT_H