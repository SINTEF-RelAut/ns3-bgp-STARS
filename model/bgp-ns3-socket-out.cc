/**
 * @file bgp-ns3-socket-out.cc
 * @author Nato Morichika <nat@nat.moe>
 * @brief Simple container class for FSM to send data to peer.
 * @version 0.1
 * @date 2019-07-15
 * 
 * @copyright Copyright (c) 2019
 * 
 */
#include "bgp-ns3-socket-out.h"
#include "ns3/simulator.h"
#include "ns3/ptr.h"

namespace ns3 {

/**
 * @brief Construct the container.
 * 
 * @param socket socket to write to.
 * @param state_change_cb Callback to notify FSM state changes.
 */
BgpNs3SocketOut::BgpNs3SocketOut (Ptr<Socket> socket,
                                  Callback<void, Ptr<Socket>, int, int> state_change_cb)
{
  _socket = socket;
  _disconnected = false;
  _state_change_cb = state_change_cb;
  _mrai = Seconds (0);
  _next_update_send_time = Seconds (0);
  _has_pending_update = false;
}

/**
 * @brief Handle output from libbgp. See libbgp document.
 * 
 * @param buffer Packet buffer.
 * @param length Buffer size.
 * @return true Packet written.
 * @return false Failed to write packet.
 */
bool
BgpNs3SocketOut::handleOut (const uint8_t *buffer, size_t length)
{
  // Session teardown may race with FSM ticks; ignore writes after disconnect.
  if (_disconnected || _socket == nullptr)
    {
      _disconnected = true;
      return false;
    }

  Ptr<Socket> socket = _socket;
  if (socket == nullptr)
    {
      _disconnected = true;
      return false;
    }

  uint8_t type = 0;
  bool has_nlri = false;
  bool has_withdrawals = false;
  if (length >= 19)
    {
      type = buffer[18];
      if (type == 2 && length >= 23)
        { // UPDATE
          uint16_t withdrawn_len = (uint16_t) ((buffer[19] << 8) | buffer[20]);
          has_withdrawals = (withdrawn_len > 0);
          size_t path_attr_offset = 19 + withdrawn_len;
          if (length >= path_attr_offset + 2)
            {
              uint16_t path_attr_len =
                  (uint16_t) ((buffer[path_attr_offset] << 8) | buffer[path_attr_offset + 1]);
              size_t nlri_offset = path_attr_offset + 2 + path_attr_len;
              has_nlri = (nlri_offset < length);
            }
        }
    }

  if (_mrai > Seconds (0) && type == 2)
    {
      Time now = Simulator::Now ();
      if (now >= _next_update_send_time && !_has_pending_update)
        {
          bool sent_now = socket->Send (buffer, length, 0) == (int) length;
          if (sent_now)
            {
              if (!_msg_cb.IsNull () && length >= 19)
                {
                  _msg_cb (socket, type, length, has_nlri, has_withdrawals);
                }
              _next_update_send_time = now + _mrai;
            }
          return sent_now;
        }

      // Coalesce UPDATE bursts to a single pending message to keep memory bounded.
      _pending_update.bytes.assign (buffer, buffer + length);
      _pending_update.type = type;
      _pending_update.has_nlri = has_nlri;
      _pending_update.has_withdrawals = has_withdrawals;
      _has_pending_update = true;
      ScheduleUpdateFlush ();
      return true;
    }

  bool sent = socket->Send (buffer, length, 0) == (int) length;
  if (sent && !_msg_cb.IsNull () && length >= 19)
    {
      _msg_cb (socket, type, length, has_nlri, has_withdrawals);
    }
  return sent;
}

/**
 * @brief Handle state change notification from libbgp. See libbgp document.
 * 
 * @param old_state Old state.
 * @param new_state New state.
 */
void
BgpNs3SocketOut::notifyStateChange (int old_state, int new_state)
{
  if (_disconnected)
    {
      return;
    }
  _state_change_cb (_socket, old_state, new_state);
}

/**
 * @brief Set callback to use when FSM change state.
 * 
 * @param state_change_cb Callback.
 */
void
BgpNs3SocketOut::setStateChangeCallback (Callback<void, Ptr<Socket>, int, int> state_change_cb)
{
  _state_change_cb = state_change_cb;
}

void
BgpNs3SocketOut::SetMessageCallback (Callback<void, Ptr<Socket>, uint8_t, size_t, bool, bool> msg_cb)
{
  _msg_cb = msg_cb;
}

void
BgpNs3SocketOut::SetMrai (Time mrai)
{
  _mrai = mrai;
}

void
BgpNs3SocketOut::DisconnectSocket ()
{
  _disconnected = true;
  if (_update_flush_event.IsRunning ())
    {
      _update_flush_event.Cancel ();
    }
  _has_pending_update = false;
  _socket = nullptr;
}

void
BgpNs3SocketOut::ScheduleUpdateFlush ()
{
  if (!_has_pending_update || _update_flush_event.IsRunning ())
    {
      return;
    }

  Time now = Simulator::Now ();
  if (_next_update_send_time <= now)
    {
      _update_flush_event = Simulator::ScheduleNow (&BgpNs3SocketOut::FlushPendingUpdate,
                                                    Ptr<BgpNs3SocketOut> (this));
      return;
    }

  _update_flush_event =
      Simulator::Schedule (_next_update_send_time - now, &BgpNs3SocketOut::FlushPendingUpdate,
                           Ptr<BgpNs3SocketOut> (this));
}

void
BgpNs3SocketOut::FlushPendingUpdate ()
{
  if (!_has_pending_update)
    {
      return;
    }

  PendingMessage msg = _pending_update;
  _has_pending_update = false;

  if (_socket == nullptr)
    {
      _disconnected = true;
      return;
    }

  Ptr<Socket> socket = _socket;
  if (socket == nullptr)
    {
      _disconnected = true;
      return;
    }

  bool sent = socket->Send (msg.bytes.data (), msg.bytes.size (), 0) == (int) msg.bytes.size ();
  if (sent)
    {
      if (!_msg_cb.IsNull ())
        {
          _msg_cb (socket, msg.type, msg.bytes.size (), msg.has_nlri, msg.has_withdrawals);
        }
      _next_update_send_time = Simulator::Now () + _mrai;
    }

  if (_has_pending_update)
    {
      _update_flush_event = Simulator::Schedule (_mrai, &BgpNs3SocketOut::FlushPendingUpdate,
                                                 Ptr<BgpNs3SocketOut> (this));
    }
}

} // namespace ns3