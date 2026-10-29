/**
 * @file bgp.cc
 * @author Nato Morichika <nat@nat.moe>
 * @brief BGP module for ns3.
 * @version 0.1
 * @date 2019-07-15
 * 
 * @copyright Copyright (c) 2019
 * 
 */
#include <arpa/inet.h>
#include <iomanip>
#include <sstream>
#include "bgp.h"
#include "ns3/log.h"
#include "ns3/enum.h"
#include "ns3/boolean.h"
#include "ns3/ipv4-list-routing.h"
#include "ns3/ipv4-static-routing.h"
#include "ns3/tcp-socket-factory.h"
#include "ns3/simulator.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("Bgp");
NS_OBJECT_ENSURE_REGISTERED (Bgp);

Peer::Peer ()
{
  allow_local_as = 0;
  passive = false;
  weight = 0;
  no_nexthop_check = false;
  forced_default_nexthop = false;
  ibgp_alter_nexthop = false;
  igp_metric = 0;
  mrai = Seconds (-1);
  ebgp_multihop = 1;
}

/**
 * @brief Drop this session
 * 
 */
void
Session::Drop ()
{
  if (socket != nullptr)
    {
      socket->SetRecvCallback (MakeNullCallback<void, Ptr<Socket>> ());
      socket->SetCloseCallbacks (MakeNullCallback<void, Ptr<Socket>> (),
                                 MakeNullCallback<void, Ptr<Socket>> ());
      socket->Close ();
    }

  if (out_handler != nullptr)
    {
      out_handler->DisconnectSocket ();
    }

  socket = nullptr;
  fsm->resetHard ();
}

TypeId
Bgp::GetTypeId (void)
{
  static TypeId tid =
      TypeId ("ns3::Bgp")
          .SetParent<Application> ()
          .SetGroupName ("Internet")
          .AddConstructor<Bgp> ()
          .AddAttribute ("LibbgpLogLevel", "Log level for libbgp to use", EnumValue (libbgp::INFO),
                         MakeEnumAccessor (&Bgp::_log_level),
                         MakeEnumChecker (libbgp::FATAL, "Fatal", libbgp::ERROR, "Error",
                                          libbgp::WARN, "Warning", libbgp::INFO, "Info",
                                          libbgp::DEBUG, "Debug"))
          .AddAttribute ("RouterID", "Local BGP ID", Ipv4AddressValue (),
                         MakeIpv4AddressAccessor (&Bgp::_bgp_id), MakeIpv4AddressChecker ())
          .AddAttribute ("HoldTimer", "Hold Timer", TimeValue (Seconds (120)),
                         MakeTimeAccessor (&Bgp::_hold_timer),
                         MakeTimeChecker (Seconds (3), Seconds (UINT16_MAX)))
          .AddAttribute ("Mrai", "Minimum Route Advertisement Interval for UPDATE pacing",
                         TimeValue (Seconds (0)), MakeTimeAccessor (&Bgp::_mrai),
                         MakeTimeChecker (Seconds (0), Seconds (UINT16_MAX)))
          .AddAttribute ("ClockInterval", "Time to wait between ticking FSMs",
                         TimeValue (Seconds (1)), MakeTimeAccessor (&Bgp::_clock_interval),
                         MakeTimeChecker ())
          .AddAttribute ("ErrorHold", "Time to wait before retry.", TimeValue (Seconds (45)),
                         MakeTimeAccessor (&Bgp::_error_hold), MakeTimeChecker ())
          .AddAttribute ("RfcDecisionProcess",
                         "Select best routes with the RFC 4271 decision process (AS_PATH length "
                         "before EBGP over IBGP) instead of the libbgp order.",
                         BooleanValue (false), MakeBooleanAccessor (&Bgp::_rfc_decision_process),
                         MakeBooleanChecker ());

  return tid;
}

Bgp::Bgp () : _logger ("(local)"), _rib (&_logger)
{
  _log_level = libbgp::INFO;
  _mrai = Seconds (0);
  _rfc_decision_process = false;

  _logger.setLogLevel (_log_level);
  _routing = CreateObject<BgpRouting> ();
  _routing->SetRib (&_rib);

  _running = false;
  _listen_socket = nullptr;
  _msg_cb_null = true;
}

void
Bgp::StartApplication (void)
{
  if (_running)
    {
      NS_LOG_WARN ("BGP application already started.");
      return;
    }

  NS_LOG_LOGIC ("creating configuration template...");
  _template.clock = &_clock;
  _template.forced_default_nexthop4 = false;
  _template.hold_timer = (uint16_t) _hold_timer.GetSeconds ();
  _template.no_collision_detection = false;
  _template.no_nexthop_check4 = false;
  _template.rev_bus = &_bus;
  _template.rib4 = &_rib;
  _template.router_id = htonl (_bgp_id.Get ());
  // Routes added before the start keep the libbgp order flag, but a comparison uses the RFC
  // order as soon as either entry has it.
  _rib.setRfcDecisionProcess (_rfc_decision_process);
  _template.use_4b_asn = true;

  Ptr<Ipv4> ipv4 = GetNode ()->GetObject<Ipv4> ();

  NS_LOG_LOGIC ("installing BgpRouting...");

  Ptr<Ipv4ListRouting> list_routing = CreateObject<Ipv4ListRouting> ();
  list_routing->AddRoutingProtocol (CreateObject<Ipv4StaticRouting> (), 10);
  list_routing->AddRoutingProtocol (_routing, 5);
  ipv4->SetRoutingProtocol (list_routing);

  NS_LOG_LOGIC ("starting listening socket...");

  _listen_socket = Socket::CreateSocket (GetNode (), TcpSocketFactory::GetTypeId ());

  if (_listen_socket->Bind (InetSocketAddress (Ipv4Address::GetAny (), 179)) == -1)
    {
      NS_FATAL_ERROR ("failed to bind socket.");
    }

  if (_listen_socket->Listen () == -1)
    {
      NS_FATAL_ERROR ("failed to listen socket");
    }

  NS_LOG_LOGIC ("registering callbacks...");

  _listen_socket->SetAcceptCallback (MakeCallback (&Bgp::HandleConnectInRequest, this),
                                     MakeCallback (&Bgp::HandleConnectIn, this));

  NS_LOG_LOGIC ("sending OPEN message to peers...");

  for (Ptr<Peer> &peer : _peers)
    {
      if (peer->passive)
        {
          NS_LOG_LOGIC ("skipping passive peer AS" << peer->peer_asn << " (" << peer->peer_address
                                                   << ").");
          continue;
        }

      // allow other events to run too.
      Simulator::ScheduleNow (&Bgp::ConnectPeer, this, peer);
    }

  NS_LOG_LOGIC ("init complete.");
  _running = true;

  Simulator::Schedule (_clock_interval, &Bgp::Tick, this);
}

void
Bgp::StopApplication (void)
{
  if (!_running)
    return;

  _running = false;

  NS_LOG_LOGIC ("closing listening socket...");

  _listen_socket->Close ();
  _listen_socket->SetAcceptCallback (MakeNullCallback<bool, Ptr<Socket>, const Address &> (),
                                     MakeNullCallback<void, Ptr<Socket>, const Address &> ());

  NS_LOG_LOGIC ("de-peering...");

  for (std::vector<Ptr<Session>>::iterator s = _sessions.begin (); s != _sessions.end ();)
    {
      (*s)->Drop ();
      s = _sessions.erase (s);
    }

  NS_LOG_LOGIC ("stopped.");

  if (_cp_out.is_open ())
    {
      _cp_out.close ();
    }
}

void
Bgp::Tick ()
{
  // Ticking an FSM can synchronously trigger close callbacks that erase
  // entries from _sessions. Iterate over a snapshot to avoid invalidation.
  std::vector<Ptr<Session>> sessions_snapshot = _sessions;

  for (std::vector<Ptr<Session>>::const_iterator it = sessions_snapshot.begin ();
       it != sessions_snapshot.end (); ++it)
    {
      Ptr<Session> session = *it;
      if (session == nullptr)
        {
          continue;
        }

      bool still_active = false;
      for (std::vector<Ptr<Session>>::const_iterator live = _sessions.begin ();
           live != _sessions.end (); ++live)
        {
          if (*live == session)
            {
              still_active = true;
              break;
            }
        }

      if (!still_active)
        {
          continue;
        }

      session->fsm->tick ();
    }

  if (_running)
    Simulator::Schedule (_clock_interval, &Bgp::Tick, this);
  else
    NS_LOG_LOGIC ("ticker stopped.");
}

bool
Bgp::ConnectPeer (Ptr<Peer> peer)
{
  if (!_running)
    return false;

  for (const Ptr<Session> session : _sessions)
    {
      if (session->peer == peer)
        {
          NS_LOG_LOGIC ("session or fsm for peer AS" << peer->peer_asn << " (" << peer->peer_address
                                                     << ") already exist, skipping.");
          return false;
        }
    }

  NS_LOG_LOGIC ("obtaning local address information for peer AS" << peer->peer_asn << " ("
                                                                 << peer->peer_address << ").");

  Ipv4InterfaceAddress local_address = _routing->GetAddressByNexthop (peer->peer_address);

  if (local_address.GetLocal ().Get () == 0)
    {
      NS_LOG_WARN ("peer AS" << peer->peer_asn << " (" << peer->peer_address
                             << ") unreachable on any device, skipping.");
      return false;
    }

  NS_LOG_LOGIC ("create and bind socket for peer AS" << peer->peer_asn << " (" << peer->peer_address
                                                     << ").");

  Ptr<Socket> peer_socket = Socket::CreateSocket (GetNode (), TcpSocketFactory::GetTypeId ());

  NS_LOG_LOGIC ("using local address: " << local_address);
  NS_LOG_LOGIC ("registering callbacks...");

  peer_socket->SetConnectCallback (MakeCallback (&Bgp::HandleConnectOut, this),
                                   MakeCallback (&Bgp::HandleConnectOutFailed, this));

  if (peer->local_asn != peer->peer_asn)
    peer_socket->SetIpTtl (peer->ebgp_multihop);

  _pending_connects[peer_socket] = peer;

  if (peer_socket->Connect (InetSocketAddress (peer->peer_address, 179)) == -1)
    {
      NS_LOG_ERROR ("failed to connect: " << strerror (errno));
      _pending_connects.erase (peer_socket);
      RetryPeerLater (peer);
      return false;
    }

  return true;
}

bool
Bgp::HandleConnectInRequest (Ptr<Socket> socket, const Address &src)
{
  return true;
}

void
Bgp::HandleConnectOutFailed (Ptr<Socket> socket)
{
  NS_LOG_WARN ("socket connect failed: " << socket);

  // A Session is only created once the connect succeeds, so a failed connect leaves no
  // session behind and HandleClose - the only other place that schedules a retry - never
  // runs for this peer. Without retrying here, the first connect attempt that lands while
  // the link is down silently retires the peer for the rest of the run, and the session
  // never returns even after the link comes back up.
  std::map<Ptr<Socket>, Ptr<Peer>>::iterator it = _pending_connects.find (socket);
  if (it == _pending_connects.end ())
    {
      return;
    }

  Ptr<Peer> peer = it->second;
  _pending_connects.erase (it);
  socket->Close ();
  RetryPeerLater (peer);
}

void
Bgp::RetryPeerLater (Ptr<Peer> peer)
{
  if (!_running || peer->passive)
    {
      return;
    }

  NS_LOG_LOGIC ("scheduled connect retry in " << _error_hold.GetSeconds () << " seconds.");
  Simulator::Schedule (_error_hold, &Bgp::ConnectPeer, this, peer);
}

void
Bgp::HandleClose (Ptr<Socket> socket)
{
  NS_LOG_LOGIC ("handling connection shutdown: " << socket);

  for (std::vector<Ptr<Session>>::iterator session = _sessions.begin ();
       session != _sessions.end (); session++)
    {
      if ((*session)->socket == socket)
        {

          if (_running && !(*session)->peer->passive)
            {
              NS_LOG_LOGIC ("scheduled retry in " << _error_hold.GetSeconds () << " seconds.");
              Simulator::Schedule (_error_hold, &Bgp::ConnectPeer, this, (*session)->peer);
            }

          NS_LOG_INFO ("dropping session of AS" << (*session)->peer->peer_asn << "/"
                                                << ((*session)->local_init ? 'L' : 'R') << " ("
                                                << (*session)->peer->peer_address << ").");
          (*session)->Drop ();
          _sessions.erase (session);
          return;
        }
    }
}

void
Bgp::DropSessionsToPeer (const Ipv4Address &peer_address)
{
  for (std::vector<Ptr<Session>>::iterator session = _sessions.begin ();
       session != _sessions.end ();)
    {
      if ((*session)->peer->peer_address != peer_address)
        {
          session++;
          continue;
        }

      // Same reconnect policy as a socket-level close, so a cable that comes back is peered
      // again rather than lost for the rest of the run.
      if (_running && !(*session)->peer->passive)
        {
          NS_LOG_LOGIC ("scheduled retry in " << _error_hold.GetSeconds () << " seconds.");
          Simulator::Schedule (_error_hold, &Bgp::ConnectPeer, this, (*session)->peer);
        }

      NS_LOG_INFO ("dropping session of AS" << (*session)->peer->peer_asn << " ("
                                            << peer_address << ") on link-down.");
      (*session)->Drop ();
      session = _sessions.erase (session);
    }

  // A connect still in flight would otherwise establish over the cable that has just gone down
  // and then have to fail on its own timers.
  for (std::map<Ptr<Socket>, Ptr<Peer>>::iterator pending = _pending_connects.begin ();
       pending != _pending_connects.end ();)
    {
      if (pending->second->peer_address == peer_address)
        {
          pending->first->SetConnectCallback (MakeNullCallback<void, Ptr<Socket>> (),
                                              MakeNullCallback<void, Ptr<Socket>> ());
          pending->first->Close ();
          pending = _pending_connects.erase (pending);
        }
      else
        {
          pending++;
        }
    }
}

bool
Bgp::RepointPeer (const Ipv4Address &from, const Ipv4Address &to)
{
  for (Ptr<Peer> &peer : _peers)
    {
      if (peer->peer_address != from)
        {
          continue;
        }

      // Drop first, then move. The teardown discards what this peer taught us, which is
      // correct: that adjacency is gone. Because there is exactly one peer entry per
      // adjacency, the discard cannot touch a session that is still up. The new session
      // repopulates both directions when it reaches ESTABLISHED.
      DropSessionsToPeer (from);
      peer->peer_address = to;
      NS_LOG_INFO ("repointed peer AS" << peer->peer_asn << " from " << from << " to " << to);
      return true;
    }
  return false;
}

bool
Bgp::ConnectPeerAddress (const Ipv4Address &peer_address)
{
  for (Ptr<Peer> &peer : _peers)
    {
      if (peer->peer_address == peer_address)
        {
          return ConnectPeer (peer);
        }
    }
  return false;
}

void
Bgp::HandleConnectIn (Ptr<Socket> socket, const Address &peer_addr)
{
  NS_LOG_LOGIC ("incoming connection.");
  SessionInit (false, socket);
}

void
Bgp::HandleConnectOut (Ptr<Socket> socket)
{
  NS_LOG_LOGIC ("connected. unregister connect callback...");
  socket->SetConnectCallback (MakeNullCallback<void, Ptr<Socket>> (),
                              MakeNullCallback<void, Ptr<Socket>> ());
  _pending_connects.erase (socket);
  SessionInit (true, socket);
}

bool
Bgp::SessionInit (bool local_init, Ptr<Socket> socket)
{
  Address peer_addr;

  if (socket->GetPeerName (peer_addr) == -1)
    {
      NS_LOG_WARN ("failed to get peer information.");
      socket->Close ();
      return false;
    }

  InetSocketAddress peer_sockaddr = InetSocketAddress::ConvertFrom (peer_addr);
  Ptr<Peer> peer = nullptr;

  NS_LOG_LOGIC ("prepareing session with " << peer_sockaddr.GetIpv4 () << "...");

  for (Ptr<Peer> _peer : _peers)
    {
      if (_peer->peer_address == peer_sockaddr.GetIpv4 ())
        {
          peer = _peer;
          break;
        }
    }

  if (peer == nullptr)
    {
      NS_LOG_WARN ("socket peer address " << peer_sockaddr.GetIpv4 ()
                                          << " does not belong to any peer.");
      socket->Close ();
      return false;
    }

  for (const Ptr<Session> session : _sessions)
    {
      if (session->peer == peer && session->fsm->getState () == libbgp::ESTABLISHED)
        {
          NS_LOG_INFO ("session or fsm for peer AS"
                       << peer->peer_asn << " (" << peer->peer_address
                       << ") already exist and established, closing socket.");
          socket->Close ();
          return false;
        }
    }

  Ipv4InterfaceAddress local_addr = _routing->GetAddressByNexthop (peer_sockaddr.GetIpv4 ());

  if (local_addr.GetLocal ().Get () == 0)
    {
      NS_LOG_WARN ("peer AS" << peer->peer_asn << " (" << peer->peer_address
                             << ") unreachable on any device.");
      socket->Close ();
      return false;
    }

  NS_LOG_LOGIC ("matching peer found. (AS" << peer->peer_asn << ", " << peer->peer_address << ").");

  char peer_name[128];
  snprintf (peer_name, 128, "AS%d/AS%d/%c", peer->local_asn, peer->peer_asn,
            local_init ? 'L' : 'R');
  libbgp::BgpConfig peer_config (_template);

  Ptr<BgpLog> peer_logger = Create<BgpLog> (peer_name);
  Ptr<BgpNs3SocketOut> peer_out_handler =
      Create<BgpNs3SocketOut> (socket, MakeCallback (&Bgp::HandleStateChange, this));
  peer_out_handler->SetMrai (peer->mrai.IsStrictlyNegative () ? _mrai : peer->mrai);

  peer_logger->setLogLevel (_log_level);

  peer_config.asn = peer->local_asn;
  peer_config.in_filters4 = peer->ingress_rules;
  peer_config.out_filters4 = peer->egress_rules;
  peer_config.log_handler = PeekPointer (peer_logger);
  peer_config.default_nexthop4 = htonl (local_addr.GetLocal ().Get ());
  peer_config.out_handler = PeekPointer (peer_out_handler);
  peer_config.peer_asn = peer->peer_asn;
  peer_config.peering_lan4 =
      libbgp::Prefix4 (htonl (local_addr.GetLocal ().CombineMask (local_addr.GetMask ()).Get ()),
                       local_addr.GetMask ().GetPrefixLength ());

  peer_config.allow_local_as = peer->allow_local_as;
  peer_config.no_nexthop_check4 = peer->no_nexthop_check;
  peer_config.weight = peer->weight;
  peer_config.forced_default_nexthop4 = peer->forced_default_nexthop;
  peer_config.ibgp_alter_nexthop = peer->ibgp_alter_nexthop;
  peer_config.igp_metric = peer->igp_metric;

  Ptr<BgpNs3Fsm> peer_fsm = Create<BgpNs3Fsm> (peer_config);
  Ptr<Session> peer_session = Create<Session> ();
  Ptr<BgpNs3SocketIn> in_handler = Create<BgpNs3SocketIn> (peer_fsm);
  peer_session->peer = peer;
  peer_session->socket = socket;
  peer_session->fsm = peer_fsm;
  peer_session->logger = peer_logger;
  peer_session->out_handler = peer_out_handler;
  peer_session->in_handler = in_handler;
  peer_session->local_init = local_init;

  socket->SetRecvCallback (MakeCallback (&BgpNs3SocketIn::HandleRead, in_handler));
  socket->SetCloseCallbacks (MakeCallback (&Bgp::HandleClose, this),
                             MakeCallback (&Bgp::HandleClose, this));

  _sessions.push_back (peer_session);

  if (!_msg_cb_null)
    {
      peer_out_handler->SetMessageCallback (MakeCallback (&Bgp::HandleMessageOut, this));
    }

  if (local_init)
    peer_fsm->start ();

  return true;
}

void
Bgp::HandleStateChange (Ptr<Socket> socket, int old_state, int new_state)
{
  if (_cp_out.is_open ())
    {
      uint32_t local_asn = 0, peer_asn = 0;
      for (std::vector<Ptr<Session>>::iterator s = _sessions.begin (); s != _sessions.end (); ++s)
        {
          if ((*s)->socket == socket)
            {
              local_asn = (*s)->peer->local_asn;
              peer_asn = (*s)->peer->peer_asn;
              break;
            }
        }
      static const char *state_names[] = {"IDLE", "OPEN_SENT", "OPEN_CONFIRM", "ESTABLISHED",
                                          "BROKEN"};
      const char *old_name =
          (old_state >= 0 && old_state <= 4) ? state_names[old_state] : "UNKNOWN";
      const char *new_name =
          (new_state >= 0 && new_state <= 4) ? state_names[new_state] : "UNKNOWN";
      _cp_out << std::fixed << std::setprecision (6) << Simulator::Now ().GetSeconds () << ","
              << local_asn << "," << peer_asn << ",STATE_CHANGE," << old_name << "->" << new_name
              << "\n";
    }
  if (new_state == libbgp::IDLE || new_state == libbgp::BROKEN)
    {
      // Defer the close so it fires after the current fsm->tick() call unwinds.
      // Calling socket->Close() synchronously here triggers HandleClose ->
      // Drop() -> fsm->resetHard() while we are still inside tick(), which
      // corrupts FSM state and causes intermittent SIGSEGV with larger MRAI/clock values.
      Simulator::ScheduleNow (&Bgp::CloseSocket, this, socket);
    }
}

void
Bgp::CloseSocket (Ptr<Socket> socket)
{
  if (socket != nullptr)
    {
      socket->Close ();
    }
}

void
Bgp::HandleMessageOut (Ptr<Socket> socket, uint8_t type, size_t length, bool has_nlri, bool has_withdrawals)
{
  if (!_cp_out.is_open ())
    return;
  uint32_t local_asn = 0, peer_asn = 0;
  for (std::vector<Ptr<Session>>::iterator s = _sessions.begin (); s != _sessions.end (); ++s)
    {
      if ((*s)->socket == socket)
        {
          local_asn = (*s)->peer->local_asn;
          peer_asn = (*s)->peer->peer_asn;
          break;
        }
    }
  double t = Simulator::Now ().GetSeconds ();
  std::ostringstream oss;
  oss << std::fixed << std::setprecision (6) << t << "," << local_asn << "," << peer_asn << ","
      << length << ",";
  if (type == 1)
    {
      _cp_out << oss.str () << "OPEN,\n";
    }
  else if (type == 2)
    {
      if (has_nlri)
        {
          _cp_out << oss.str () << "UPDATE,announce\n";
        }
      if (has_withdrawals)
        {
          _cp_out << oss.str () << "UPDATE,withdraw\n";
        }
      if (!has_nlri && !has_withdrawals)
        {
          _cp_out << oss.str () << "UPDATE,\n";
        }
    }
  else if (type == 3)
    {
      _cp_out << oss.str () << "NOTIFICATION,\n";
    }
  else if (type == 4)
    {
      _cp_out << oss.str () << "KEEPALIVE,\n";
    }
}

void
Bgp::SetControlPlaneOutput (const std::string &path)
{
  _cp_out.open (path.c_str (), std::ios::out | std::ios::trunc);
  _cp_out << "time_s,local_asn,peer_asn,size_bytes,event,detail\n";
  _msg_cb_null = false;
}

/**
 * @brief Add a peer to the BGP application.
 * 
 * @param peer Peer to add.
 */
void
Bgp::AddPeer (const Peer &peer)
{
  NS_ASSERT (!_running);
  _peers.push_back (Create<Peer> (peer));
}

/**
 * @brief Add a route to BGP routing table. 
 * 
 * @param route The route.
 * @param nexthop Nexthop.
 */
void
Bgp::AddRoute (libbgp::Prefix4 route, uint32_t nexthop)
{
  NS_ASSERT (!_running);
  // Locally originated routes outrank any route learned from a peer, as on real routers (Cisco
  // gives them weight 32768). At libbgp's default weight of 0 a peer route with a configured
  // weight won, so for a link subnet originated by both ends of the link each router preferred
  // its neighbours' routes to its own. With libbgp sending the withdrawals RFC 4271 requires,
  // four routers doing that formed a cycle that never converged; upstream libbgp only hid it
  // by leaving stale routes in place.
  _rib.insert (&_logger, route, nexthop, 32768);
}

/**
 * @brief Add a route to BGP routing table. 
 * 
 * @param prefix The prefix of the route in network byte order.
 * @param mask The netmask of the route in CIDR notation.
 * @param nexthop Nexthop.
 */
void
Bgp::AddRoute (uint32_t prefix, uint8_t mask, uint32_t nexthop)
{
  NS_ASSERT (!_running);
  AddRoute (libbgp::Prefix4 (prefix, mask), nexthop);
}

/**
 * @brief Add a route to BGP routing table. 
 * 
 * @param prefix Prefix of the route.
 * @param mask Netmask of the route.
 * @param nexthop Nexthop of the route.
 */
void
Bgp::AddRoute (const Ipv4Address &prefix, const Ipv4Mask &mask, const Ipv4Address &nexthop)
{
  NS_ASSERT (!_running);
  AddRoute (htonl (prefix.Get ()), (uint8_t) mask.GetPrefixLength (), htonl (nexthop.Get ()));
}

bool
Bgp::LookupRoute (const Ipv4Address &dst, Ipv4Address *nexthop, uint8_t *prefixLen,
                  Ipv4Address *srcRouterId) const
{
  const libbgp::BgpRib4Entry *entry = _rib.lookup (htonl (dst.Get ()));
  if (entry == nullptr)
    {
      return false;
    }

  if (nexthop)
    {
      *nexthop = Ipv4Address (ntohl (entry->getNexthop ()));
    }
  if (prefixLen)
    {
      *prefixLen = entry->route.getLength ();
    }
  if (srcRouterId)
    {
      *srcRouterId = Ipv4Address (ntohl (entry->src_router_id));
    }
  return true;
}

/**
 * @brief Set libbgp log level.
 * 
 * @param log_level Log level.
 */
void
Bgp::SetLibbgpLogLevel (libbgp::LogLevel log_level)
{
  NS_ASSERT (!_running);
  _log_level = log_level;
}

/**
 * @brief Set local router ID.
 * 
 * @param bgp_id Router ID.
 */
void
Bgp::SetBgpId (Ipv4Address bgp_id)
{
  NS_ASSERT (!_running);
  _bgp_id = bgp_id;
}

/**
 * @brief Set local hold timer.
 * 
 * @param hold_timer Hold timer.
 */
void
Bgp::SetHoldTimer (Time hold_timer)
{
  NS_ASSERT (!_running);
  _hold_timer = hold_timer;
}

/**
 * @brief Set Tick() interval
 * 
 * @param interval Tick interval.
 */
void
Bgp::SetClockInterval (Time interval)
{
  NS_ASSERT (!_running);
  _clock_interval = interval;
}

/**
 * @example bgp-example.cc
 * A simple BGP setup example with two nodes peering with each other.
 * 
 */

} // namespace ns3
