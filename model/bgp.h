/**
 * @file bgp.h
 * @author Nato Morichika <nat@nat.moe>
 * @brief BGP module for ns3.
 * @version 0.1
 * @date 2019-07-15
 * 
 * @copyright Copyright (c) 2019
 * 
 */
#ifndef BGP_H
#define BGP_H

#include <fstream>
#include <map>
#include <vector>
#include "bgp-ns3-fsm.h"
#include "bgp-ns3-clock.h"
#include "bgp-ns3-socket-out.h"
#include "bgp-log.h"
#include "bgp-routing.h"
#include "bgp-ns3-socket-in.h"
#include "ns3/application.h"
#include "ns3/ipv4-address.h"
#include "ns3/socket.h"

namespace ns3 {

class Bgp;
class BgpNs3SocketIn;

/**
 * @brief Peer configuration class.
 * 
 */
class Peer : public SimpleRefCount<Peer>
{
public:
  Peer ();

  uint32_t local_asn; //!< local ASN.
  uint32_t peer_asn; //!< peer ASN.
  Ipv4Address peer_address; //!< peer's address.
  libbgp::BgpFilterRules ingress_rules; //!< ingress router filter rules. See libbgp documents.
  libbgp::BgpFilterRules egress_rules; //!< egress router filter rules. See libbgp documents.

  bool passive; //!< passive peering (don't send OPEN)
  int8_t allow_local_as; //!< Allow N local ASN in AS_PATH
  int32_t weight; //!< weight of this peer
  bool no_nexthop_check; //!< disable nexthop attribute validation
  bool forced_default_nexthop; //!< always use peering IP as nexthop.
  bool ibgp_alter_nexthop; //!< alter IBGP nexthop attribute the same way as EBGP.
  uint32_t igp_metric; //!< IGP metric to this peer, used by the RFC 4271 decision process.
  Time mrai; //!< MRAI for this session; negative (default) means the Bgp "Mrai" attribute.

  uint8_t ebgp_multihop;
};

/**
 * @brief Session information class.
 * 
 */
class Session : public SimpleRefCount<Session>
{
public:
  Ptr<Peer> peer;
  Ptr<BgpNs3Fsm> fsm;
  Ptr<Socket> socket;
  Ptr<BgpLog> logger;
  Ptr<BgpNs3SocketOut> out_handler;
  Ptr<BgpNs3SocketIn> in_handler;

  bool local_init;

  void Drop ();
};

/**
 * @brief The Bgp Application.
 * 
 */
class Bgp : public Application
{
public:
  Bgp ();
  static TypeId GetTypeId (void);

  void StartApplication (void);
  void StopApplication (void);

  void AddPeer (const Peer &peer);
  void AddRoute (libbgp::Prefix4 route, uint32_t nexthop);
  void AddRoute (uint32_t prefix, uint8_t mask, uint32_t nexthop);
  void AddRoute (const Ipv4Address &prefix, const Ipv4Mask &mask, const Ipv4Address &nexthop);
  bool LookupRoute (const Ipv4Address &dst, Ipv4Address *nexthop, uint8_t *prefixLen,
                    Ipv4Address *srcRouterId) const;
  // Drop any session peered at this address, as if the cable carrying it had been cut, and
  // schedule the usual reconnect. Needed because a session only notices a dead link when its
  // hold timer expires, and libbgp scopes its RIB by peer router ID rather than per session:
  // with one session per cable of an adjacency, the stale session's late teardown calls
  // dropAllRoutes() and discards the routes a newly established session has already
  // installed for the same peer node. Dropping the stale session at the moment the link goes
  // down puts that discard before the new session's UPDATE instead of after it.
  void DropSessionsToPeer (const Ipv4Address &peer_address);
  // Move a configured peer from one address to another, tearing down its session first, and
  // start a session at the new address. Used where an adjacency's two cables carry separate
  // subnets: one peer entry follows whichever cable is live, instead of one entry per cable.
  // Two entries would both key on this node's single router ID, so tearing one down would
  // discard the routes the other had installed. Returns false if no peer has that address.
  bool RepointPeer (const Ipv4Address &from, const Ipv4Address &to);
  bool ConnectPeerAddress (const Ipv4Address &peer_address);
  void SetLibbgpLogLevel (libbgp::LogLevel log_level);
  void SetBgpId (Ipv4Address bgp_id);
  void SetHoldTimer (Time hold_timer);
  void SetClockInterval (Time interval);
  // Open a CSV file to record outgoing BGP messages and session state transitions.
  void SetControlPlaneOutput (const std::string &path);

private:
  void Tick ();

  bool ConnectPeer (Ptr<Peer> peer);
  bool SessionInit (bool local_init, Ptr<Socket> socket);

  void HandleConnectIn (Ptr<Socket> socket, const Address &src);
  bool HandleConnectInRequest (Ptr<Socket> socket, const Address &src);
  void HandleConnectOut (Ptr<Socket> socket);
  void HandleConnectOutFailed (Ptr<Socket> socket);
  // Schedule another ConnectPeer for a peer whose outbound connect failed.
  void RetryPeerLater (Ptr<Peer> peer);
  void HandleClose (Ptr<Socket> socket);
  void HandleStateChange (Ptr<Socket> socket, int old_state, int new_state);
  void CloseSocket (Ptr<Socket> socket);
  // Called per outgoing message (type byte + message length + UPDATE refinement booleans).
  void HandleMessageOut (Ptr<Socket> socket, uint8_t type, size_t length, bool has_nlri, bool has_withdrawals);

  Time _hold_timer;
  Time _mrai;
  Time _clock_interval;
  Time _error_hold;
  bool _rfc_decision_process;

  BgpLog _logger;
  BgpNs3Clock _clock;
  Ipv4Address _bgp_id;

  Ptr<BgpRouting> _routing;
  Ptr<Socket> _listen_socket;

  std::vector<Ptr<Peer>> _peers;
  std::vector<Ptr<Session>> _sessions;
  // Outbound sockets whose TCP connect has not completed yet. A Session only exists once
  // the connect succeeds, so without this a failed connect leaves nothing to retry from.
  std::map<Ptr<Socket>, Ptr<Peer>> _pending_connects;

  libbgp::BgpConfig _template;
  libbgp::BgpRib4 _rib;
  libbgp::RouteEventBus _bus;
  libbgp::LogLevel _log_level;

  std::ofstream _cp_out;
  bool _msg_cb_null; // true until SetControlPlaneOutput is called

  bool _running;
};

} // namespace ns3

#endif /* BGP_H */
