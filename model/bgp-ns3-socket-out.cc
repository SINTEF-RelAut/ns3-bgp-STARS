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
  _next_pending_seq = 0;
  _next_update_send_time = Seconds (0);
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
      if (now >= _next_update_send_time && _pending_updates.empty ())
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

      // MRAI queue compression: keep the latest pending UPDATE per prefix. Prefixes this
      // message re-advertises drop their earlier pending message; prefixes it does not
      // mention keep theirs. Compressing per peer instead of per prefix, as this used to,
      // silently discards the routes of every prefix absent from the newest message.
      PendingMessage pending;
      pending.bytes.assign (buffer, buffer + length);
      pending.type = type;
      pending.has_nlri = has_nlri;
      pending.has_withdrawals = has_withdrawals;

      const uint64_t seq = _next_pending_seq++;
      const std::vector<PrefixKey> prefixes = ExtractPrefixes (buffer, length);
      for (std::vector<PrefixKey>::const_iterator it = prefixes.begin (); it != prefixes.end ();
           ++it)
        {
          std::map<PrefixKey, uint64_t>::iterator prev = _pending_seq_by_prefix.find (*it);
          if (prev != _pending_seq_by_prefix.end ())
            {
              // Superseded for this prefix. Retire the old message only once no prefix
              // still refers to it, otherwise a message covering several prefixes would
              // vanish when just one of them is re-advertised.
              const uint64_t old_seq = prev->second;
              prev->second = seq;
              bool still_referenced = false;
              for (std::map<PrefixKey, uint64_t>::const_iterator scan =
                       _pending_seq_by_prefix.begin ();
                   scan != _pending_seq_by_prefix.end (); ++scan)
                {
                  if (scan->second == old_seq)
                    {
                      still_referenced = true;
                      break;
                    }
                }
              if (!still_referenced)
                {
                  _pending_updates.erase (old_seq);
                }
            }
          else
            {
              _pending_seq_by_prefix.insert (std::make_pair (*it, seq));
            }
        }

      // An UPDATE with no prefixes at all (attribute-only) still has to reach the peer.
      _pending_updates.insert (std::make_pair (seq, pending));

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
  _pending_seq_by_prefix.clear ();
  _pending_updates.clear ();
  _socket = nullptr;
}

void
BgpNs3SocketOut::ScheduleUpdateFlush ()
{
  if (_pending_updates.empty () || _update_flush_event.IsRunning ())
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
  if (_pending_updates.empty ())
    {
      return;
    }

  Ptr<Socket> socket = _socket;
  if (socket == nullptr)
    {
      _disconnected = true;
      return;
    }

  // One MRAI tick releases the compressed state: the surviving latest UPDATE for every
  // prefix still pending, in the order libbgp produced them so the peer ends on the
  // correct final state. Everything superseded during the window was already dropped.
  std::map<uint64_t, PendingMessage> due;
  due.swap (_pending_updates);
  _pending_seq_by_prefix.clear ();

  for (std::map<uint64_t, PendingMessage>::const_iterator it = due.begin (); it != due.end ();
       ++it)
    {
      const PendingMessage &msg = it->second;
      bool sent =
          socket->Send (msg.bytes.data (), msg.bytes.size (), 0) == (int) msg.bytes.size ();
      if (sent && !_msg_cb.IsNull ())
        {
          _msg_cb (socket, msg.type, msg.bytes.size (), msg.has_nlri, msg.has_withdrawals);
        }
    }

  _next_update_send_time = Simulator::Now () + _mrai;
}

/**
 * @brief Collect every prefix a BGP UPDATE touches.
 *
 * Walks the withdrawn-routes list and the NLRI list, both of which encode each prefix as a
 * one-byte length in bits followed by ceil(len/8) bytes of network-order address. Used to
 * key MRAI queue compression, so a re-advertisement only supersedes the pending message for
 * the prefixes it actually carries.
 *
 * @param buffer Raw BGP message.
 * @param length Message length in bytes.
 * @return The prefixes referenced, as (prefix length, address) pairs.
 */
std::vector<BgpNs3SocketOut::PrefixKey>
BgpNs3SocketOut::ExtractPrefixes (const uint8_t *buffer, size_t length)
{
  std::vector<PrefixKey> prefixes;
  if (length < 23 || buffer[18] != 2)
    {
      return prefixes;
    }

  const uint16_t withdrawn_len = (uint16_t) ((buffer[19] << 8) | buffer[20]);
  const size_t path_attr_offset = 21 + withdrawn_len;
  if (path_attr_offset + 2 > length)
    {
      return prefixes;
    }

  const uint16_t path_attr_len =
      (uint16_t) ((buffer[path_attr_offset] << 8) | buffer[path_attr_offset + 1]);
  const size_t nlri_offset = path_attr_offset + 2 + path_attr_len;

  const size_t ranges[2][2] = {{21, path_attr_offset}, {nlri_offset, length}};
  for (uint32_t r = 0; r < 2; ++r)
    {
      size_t pos = ranges[r][0];
      const size_t end = ranges[r][1] > length ? length : ranges[r][1];
      while (pos < end)
        {
          const uint8_t bits = buffer[pos];
          if (bits > 32)
            {
              break;
            }
          const size_t octets = (bits + 7) / 8;
          if (pos + 1 + octets > end)
            {
              break;
            }

          uint32_t addr = 0;
          for (size_t b = 0; b < octets; ++b)
            {
              addr |= ((uint32_t) buffer[pos + 1 + b]) << (24 - 8 * b);
            }
          prefixes.push_back (std::make_pair (bits, addr));
          pos += 1 + octets;
        }
    }

  return prefixes;
}

} // namespace ns3
