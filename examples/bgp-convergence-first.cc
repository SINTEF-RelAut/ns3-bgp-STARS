#include <arpa/inet.h>

#include <errno.h>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <utility>
#include <vector>

#include "ns3/application-container.h"
#include "ns3/core-module.h"
#include "ns3/internet-apps-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4.h"
#include "ns3/ipv4-address.h"
#include "ns3/ipv4-interface-address.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/socket.h"
#include "ns3/virtual-ixp-fabric.h"

#include "ns3/bgp.h"

using namespace ns3;

namespace {

struct LinkSpec
{
  uint16_t as_a;
  uint16_t as_b;
  double latency_s;
  uint32_t capacity_mbps;
};

struct LinkRuntime
{
  LinkSpec spec;
  Ptr<Node> node_a;
  Ptr<Node> node_b;
  Ptr<NetDevice> dev_a;
  Ptr<NetDevice> dev_b;
  uint32_t iface_a;
  uint32_t iface_b;
  Ipv4Address ip_a;
  Ipv4Address ip_b;
};

struct FailureWindow
{
  double down_s;
  double up_s;
  uint16_t as_a;
  uint16_t as_b;
};

struct VirtualIxpPair
{
  IxpLinkId side_a;
  IxpLinkId side_b;
  double quality_a;
  double quality_b;
  uint16_t as_a;
  uint16_t as_b;
};

class UdpEchoResponder : public Application
{
public:
  explicit UdpEchoResponder (uint16_t port) : m_port (port)
  {
  }

private:
  void
  StartApplication () override
  {
    m_socket = Socket::CreateSocket (GetNode (), UdpSocketFactory::GetTypeId ());
    m_socket->Bind (InetSocketAddress (Ipv4Address::GetAny (), m_port));
    m_socket->SetRecvCallback (MakeCallback (&UdpEchoResponder::HandleRead, this));
  }

  void
  StopApplication () override
  {
    if (m_socket)
      {
        m_socket->SetRecvCallback (MakeNullCallback<void, Ptr<Socket>> ());
        m_socket->Close ();
        m_socket = 0;
      }
  }

  void
  HandleRead (Ptr<Socket> socket)
  {
    Address from;
    Ptr<Packet> packet = socket->RecvFrom (from);
    if (!packet)
      {
        return;
      }
    socket->SendTo (packet, 0, from);
  }

  Ptr<Socket> m_socket;
  uint16_t m_port;
};

class UdpProbeApp : public Application
{
public:
  UdpProbeApp ()
      : m_src_as (0),
        m_dst_as (0),
        m_port (9000),
        m_seq (0),
        m_count (0),
        m_timeout (Seconds (2.0)),
        m_interval (Seconds (1.0))
  {
  }

  void
  Configure (uint16_t src_as, uint16_t dst_as, Ipv4Address dst_ip, uint16_t port, uint32_t count,
             Time interval, Time timeout, const std::string &output_path)
  {
    m_src_as = src_as;
    m_dst_as = dst_as;
    m_dst_ip = dst_ip;
    m_port = port;
    m_count = count;
    m_interval = interval;
    m_timeout = timeout;
    m_output_path = output_path;
  }

private:
  struct ProbePayload
  {
    uint32_t seq;
    uint64_t send_time_ns;
  };

  static uint64_t
  ToNs (Time t)
  {
    return static_cast<uint64_t> (t.GetNanoSeconds ());
  }

  void
  StartApplication () override
  {
    m_socket = Socket::CreateSocket (GetNode (), UdpSocketFactory::GetTypeId ());
    m_socket->Bind ();
    m_socket->Connect (InetSocketAddress (m_dst_ip, m_port));
    m_socket->SetRecvCallback (MakeCallback (&UdpProbeApp::HandleRead, this));

    m_out.open (m_output_path.c_str (), std::ios::out | std::ios::trunc);
    m_out << "time_s,src_as,dst_as,seq,event,rtt_ms" << std::endl;

    SendOne ();
  }

  void
  StopApplication () override
  {
    if (m_send_event.IsRunning ())
      {
        m_send_event.Cancel ();
      }

    for (std::map<uint32_t, EventId>::iterator it = m_timeouts.begin (); it != m_timeouts.end ();
         ++it)
      {
        it->second.Cancel ();
      }

    if (m_socket)
      {
        m_socket->SetRecvCallback (MakeNullCallback<void, Ptr<Socket>> ());
        m_socket->Close ();
        m_socket = 0;
      }

    if (m_out.is_open ())
      {
        m_out.close ();
      }
  }

  void
  SendOne ()
  {
    if (m_seq >= m_count)
      {
        return;
      }

    ProbePayload payload;
    payload.seq = m_seq;
    payload.send_time_ns = ToNs (Simulator::Now ());

    Ptr<Packet> p = Create<Packet> (reinterpret_cast<const uint8_t *> (&payload), sizeof (payload));
    m_socket->Send (p);

    m_send_times.insert (std::make_pair (m_seq, Simulator::Now ()));
    m_out << std::fixed << std::setprecision (6) << Simulator::Now ().GetSeconds () << ","
          << m_src_as << "," << m_dst_as << "," << m_seq << ",sent," << "" << std::endl;

    EventId timeout_event = Simulator::Schedule (m_timeout, &UdpProbeApp::OnTimeout, this, m_seq);
    m_timeouts.insert (std::make_pair (m_seq, timeout_event));

    m_seq++;
    m_send_event = Simulator::Schedule (m_interval, &UdpProbeApp::SendOne, this);
  }

  void
  HandleRead (Ptr<Socket> socket)
  {
    Ptr<Packet> p = socket->Recv ();
    if (!p || p->GetSize () < sizeof (ProbePayload))
      {
        return;
      }

    ProbePayload payload;
    p->CopyData (reinterpret_cast<uint8_t *> (&payload), sizeof (payload));

    std::map<uint32_t, Time>::iterator send_it = m_send_times.find (payload.seq);
    if (send_it == m_send_times.end ())
      {
        return;
      }

    std::map<uint32_t, EventId>::iterator timeout_it = m_timeouts.find (payload.seq);
    if (timeout_it != m_timeouts.end ())
      {
        timeout_it->second.Cancel ();
        m_timeouts.erase (timeout_it);
      }

    Time rtt = Simulator::Now () - send_it->second;
    m_send_times.erase (send_it);

    m_out << std::fixed << std::setprecision (6) << Simulator::Now ().GetSeconds () << ","
          << m_src_as << "," << m_dst_as << "," << payload.seq << ",reply," << std::setprecision (3)
          << (rtt.GetSeconds () * 1000.0) << std::endl;
  }

  void
  OnTimeout (uint32_t seq)
  {
    std::map<uint32_t, Time>::iterator it = m_send_times.find (seq);
    if (it == m_send_times.end ())
      {
        return;
      }

    m_send_times.erase (it);
    m_timeouts.erase (seq);

    m_out << std::fixed << std::setprecision (6) << Simulator::Now ().GetSeconds () << ","
          << m_src_as << "," << m_dst_as << "," << seq << ",timeout," << "" << std::endl;
  }

  Ptr<Socket> m_socket;
  EventId m_send_event;

  uint16_t m_src_as;
  uint16_t m_dst_as;
  Ipv4Address m_dst_ip;
  uint16_t m_port;

  uint32_t m_seq;
  uint32_t m_count;
  Time m_timeout;
  Time m_interval;

  std::string m_output_path;
  std::ofstream m_out;
  std::map<uint32_t, Time> m_send_times;
  std::map<uint32_t, EventId> m_timeouts;
};

std::string
PairKey (uint16_t a, uint16_t b)
{
  std::ostringstream oss;
  if (a < b)
    {
      oss << a << "-" << b;
    }
  else
    {
      oss << b << "-" << a;
    }
  return oss.str ();
}

void
SetLinkState (LinkRuntime *link, bool up, std::ofstream *events_out)
{
  Ptr<Ipv4> ipv4_a = link->node_a->GetObject<Ipv4> ();
  Ptr<Ipv4> ipv4_b = link->node_b->GetObject<Ipv4> ();

  if (up)
    {
      ipv4_a->SetUp (link->iface_a);
      ipv4_b->SetUp (link->iface_b);
    }
  else
    {
      ipv4_a->SetDown (link->iface_a);
      ipv4_b->SetDown (link->iface_b);
    }

  if (events_out && events_out->is_open ())
    {
      (*events_out) << std::fixed << std::setprecision (6) << Simulator::Now ().GetSeconds () << ","
                    << (up ? "link_up" : "link_down") << "," << link->spec.as_a << ","
                    << link->spec.as_b << std::endl;
    }
}

void
SetVirtualIxpPairState (Ptr<VirtualIxpFabric> fabric, VirtualIxpPair pair, bool up,
                        std::ofstream *events_out)
{
  if (fabric == 0)
    {
      return;
    }

  if (up)
    {
      fabric->SetFeasibleUp (pair.side_a, pair.quality_a);
      fabric->SetFeasibleUp (pair.side_b, pair.quality_b);
    }
  else
    {
      fabric->SetFeasibleDown (pair.side_a);
      fabric->SetFeasibleDown (pair.side_b);
    }

  if (events_out && events_out->is_open ())
    {
      (*events_out) << std::fixed << std::setprecision (6) << Simulator::Now ().GetSeconds () << ","
                    << (up ? "link_up" : "link_down") << "," << pair.as_a << "," << pair.as_b
                    << std::endl;
    }
}

bool
EnsureDirectory (const std::string &path)
{
  if (path.empty ())
    {
      return false;
    }

  int rc = mkdir (path.c_str (), 0775);
  return rc == 0 || errno == EEXIST;
}

void
SnapshotRoutingTables (std::map<uint16_t, Ptr<Node>> as_nodes,
                       std::map<uint16_t, Ptr<Bgp>> bgp_apps, const std::string &dir)
{
  for (std::map<uint16_t, Ptr<Bgp>>::const_iterator it = bgp_apps.begin (); it != bgp_apps.end ();
       ++it)
    {
      uint16_t asn = it->first;
      Ptr<Node> node = as_nodes.at (asn);
      Ptr<Ipv4> ipv4 = node->GetObject<Ipv4> ();
      Ptr<Ipv4RoutingProtocol> rp = ipv4->GetRoutingProtocol ();

      std::ostringstream filename;
      filename << dir << "/bgp_rib_as" << asn << "_t"
               << static_cast<uint32_t> (Simulator::Now ().GetSeconds ()) << ".txt";
      Ptr<OutputStreamWrapper> stream =
          Create<OutputStreamWrapper> (filename.str (), std::ios::out);
      rp->PrintRoutingTable (stream, Time::S);
    }
}

} // namespace

int
main (int argc, char *argv[])
{
  Time::SetResolution (Time::NS);

  std::string out_dir = "build/bgp_convergence_first";
  std::string scenario = "A";
  double clock_interval_s = 1.0;
  double sim_time_s = 800.0;
  uint32_t virtual_ixp = 0;
  uint32_t virtual_ixp_port_count = 2;
  double virtual_ixp_rebalance_s = 0.25;
  double virtual_ixp_hold_down_s = 0.0;
  CommandLine cmd;
  cmd.AddValue ("outDir", "Output directory for probe and event CSV files", out_dir);
  cmd.AddValue ("scenario", "Scenario ID: A, C, or D", scenario);
  cmd.AddValue ("clockInterval", "BGP FSM clock interval in seconds", clock_interval_s);
  cmd.AddValue ("simTime", "Simulation duration in seconds", sim_time_s);
  cmd.AddValue ("virtualIxp", "Enable virtual IXP mediation for failed links (0/1)", virtual_ixp);
  cmd.AddValue ("virtualIxpPortCount", "Virtual IXP port count", virtual_ixp_port_count);
  cmd.AddValue ("virtualIxpRebalance", "Virtual IXP rebalance period (seconds)",
                virtual_ixp_rebalance_s);
  cmd.AddValue ("virtualIxpHoldDown", "Virtual IXP hold-down (seconds)", virtual_ixp_hold_down_s);
  cmd.Parse (argc, argv);

  std::vector<uint16_t> as_ids;
  as_ids.push_back (101);
  as_ids.push_back (102);
  as_ids.push_back (103);
  as_ids.push_back (104);
  as_ids.push_back (105);
  as_ids.push_back (106);
  as_ids.push_back (107);
  as_ids.push_back (108);

  std::vector<LinkSpec> links;
  links.push_back ((LinkSpec) {101, 102, 0.0020, 300});
  links.push_back ((LinkSpec) {102, 104, 0.0020, 300});
  links.push_back ((LinkSpec) {104, 106, 0.0020, 300});
  links.push_back ((LinkSpec) {101, 103, 0.0022, 300});
  links.push_back ((LinkSpec) {103, 105, 0.0022, 300});
  links.push_back ((LinkSpec) {105, 106, 0.0022, 300});
  links.push_back ((LinkSpec) {102, 103, 0.0012, 200});
  links.push_back ((LinkSpec) {104, 105, 0.0012, 200});
  links.push_back ((LinkSpec) {101, 107, 0.0015, 150});
  links.push_back ((LinkSpec) {106, 108, 0.0015, 150});

  std::map<uint16_t, uint32_t> as_to_node_idx;
  std::map<uint16_t, Ptr<Node>> as_nodes;
  NodeContainer nodes;
  for (uint32_t i = 0; i < as_ids.size (); ++i)
    {
      nodes.Create (1);
      as_to_node_idx.insert (std::make_pair (as_ids.at (i), i));
      as_nodes.insert (std::make_pair (as_ids.at (i), nodes.Get (i)));
    }

  EnsureDirectory (out_dir);

  InternetStackHelper internet;
  internet.Install (nodes);

  std::vector<LinkRuntime> runtimes;
  std::map<std::string, uint32_t> link_key_to_runtime_idx;

  uint32_t subnet_id = 0;
  for (uint32_t i = 0; i < links.size (); ++i)
    {
      const LinkSpec &spec = links.at (i);

      Ptr<Node> node_a = nodes.Get (as_to_node_idx.at (spec.as_a));
      Ptr<Node> node_b = nodes.Get (as_to_node_idx.at (spec.as_b));

      std::ostringstream rate;
      rate << spec.capacity_mbps << "Mbps";

      PointToPointHelper p2p;
      p2p.SetDeviceAttribute ("DataRate", StringValue (rate.str ()));
      p2p.SetChannelAttribute ("Delay", TimeValue (Seconds (spec.latency_s)));

      NetDeviceContainer devs = p2p.Install (node_a, node_b);

      std::ostringstream base;
      base << "10." << (subnet_id / 256) << "." << (subnet_id % 256) << ".0";
      Ipv4AddressHelper ipv4;
      ipv4.SetBase (base.str ().c_str (), "255.255.255.252");
      Ipv4InterfaceContainer ifaces = ipv4.Assign (devs);

      Ptr<Ipv4> ipv4_a = node_a->GetObject<Ipv4> ();
      Ptr<Ipv4> ipv4_b = node_b->GetObject<Ipv4> ();

      uint32_t iface_a = ipv4_a->GetInterfaceForDevice (devs.Get (0));
      uint32_t iface_b = ipv4_b->GetInterfaceForDevice (devs.Get (1));

      LinkRuntime runtime;
      runtime.spec = spec;
      runtime.node_a = node_a;
      runtime.node_b = node_b;
      runtime.dev_a = devs.Get (0);
      runtime.dev_b = devs.Get (1);
      runtime.iface_a = iface_a;
      runtime.iface_b = iface_b;
      runtime.ip_a = ifaces.GetAddress (0);
      runtime.ip_b = ifaces.GetAddress (1);
      runtimes.push_back (runtime);

      link_key_to_runtime_idx.insert (std::make_pair (PairKey (spec.as_a, spec.as_b), i));
      subnet_id++;
    }

  std::map<uint16_t, Ptr<Bgp>> bgp_apps;
  for (std::map<uint16_t, uint32_t>::const_iterator it = as_to_node_idx.begin ();
       it != as_to_node_idx.end (); ++it)
    {
      uint16_t asn = it->first;
      Ptr<Node> node = nodes.Get (it->second);
      Ptr<Ipv4> ipv4 = node->GetObject<Ipv4> ();

      Ipv4Address router_id = Ipv4Address::GetAny ();
      for (uint32_t iface = 1; iface < ipv4->GetNInterfaces (); ++iface)
        {
          if (ipv4->GetNAddresses (iface) > 0)
            {
              router_id = ipv4->GetAddress (iface, 0).GetLocal ();
              break;
            }
        }

      Ptr<Bgp> bgp = CreateObject<Bgp> ();
      bgp->SetAttribute ("RouterID", Ipv4AddressValue (router_id));
      bgp->SetAttribute ("HoldTimer", TimeValue (Seconds (3.0 * clock_interval_s)));
      bgp->SetAttribute ("ClockInterval", TimeValue (Seconds (clock_interval_s)));
      bgp->SetAttribute ("LibbgpLogLevel", EnumValue (libbgp::FATAL));
      node->AddApplication (bgp);
      bgp_apps.insert (std::make_pair (asn, bgp));

      std::ostringstream cp_out;
      cp_out << out_dir << "/bgp_cp_as" << asn << ".csv";
      bgp->SetControlPlaneOutput (cp_out.str ());
    }

  for (uint32_t i = 0; i < runtimes.size (); ++i)
    {
      LinkRuntime &rt = runtimes.at (i);

      Peer a_to_b;
      a_to_b.local_asn = rt.spec.as_a;
      a_to_b.peer_asn = rt.spec.as_b;
      a_to_b.peer_address = rt.ip_b;
      a_to_b.passive = false;
      bgp_apps.at (rt.spec.as_a)->AddPeer (a_to_b);

      Peer b_to_a;
      b_to_a.local_asn = rt.spec.as_b;
      b_to_a.peer_asn = rt.spec.as_a;
      b_to_a.peer_address = rt.ip_a;
      b_to_a.passive = false;
      bgp_apps.at (rt.spec.as_b)->AddPeer (b_to_a);
    }

  std::map<uint16_t, Ipv4Address> probe_ip_per_as;
  for (std::map<uint16_t, uint32_t>::const_iterator it = as_to_node_idx.begin ();
       it != as_to_node_idx.end (); ++it)
    {
      uint16_t asn = it->first;
      Ptr<Node> node = nodes.Get (it->second);
      Ptr<Ipv4> ipv4 = node->GetObject<Ipv4> ();

      Ipv4Address first_addr = Ipv4Address::GetAny ();

      for (uint32_t iface = 1; iface < ipv4->GetNInterfaces (); ++iface)
        {
          if (ipv4->GetNAddresses (iface) == 0)
            {
              continue;
            }

          Ipv4InterfaceAddress if_addr = ipv4->GetAddress (iface, 0);
          if (first_addr == Ipv4Address::GetAny ())
            {
              first_addr = if_addr.GetLocal ();
            }

          Ipv4Address prefix = if_addr.GetLocal ().CombineMask (if_addr.GetMask ());
          bgp_apps.at (asn)->AddRoute (prefix, if_addr.GetMask (), if_addr.GetLocal ());
        }

      probe_ip_per_as.insert (std::make_pair (asn, first_addr));
    }

  // Scenario-specific run profile aligned to SCION scenarios.
  std::vector<FailureWindow> failure_windows;
  std::vector<std::pair<uint16_t, uint16_t>> probe_pairs;
  double simulation_duration_s = sim_time_s;
  double probe_start_s = 10.0;
  double probe_interval_s = 1.0;
  double probe_timeout_s = 2.0;
  uint32_t probe_count = 0;

  if (scenario == "A")
    {
      failure_windows.push_back ((FailureWindow) {50.0, 250.0, 101, 102});

      probe_pairs.push_back (std::make_pair (101, 102));
      probe_pairs.push_back (std::make_pair (101, 103));
      probe_pairs.push_back (std::make_pair (101, 104));
      probe_pairs.push_back (std::make_pair (101, 105));
      probe_pairs.push_back (std::make_pair (101, 106));
      probe_pairs.push_back (std::make_pair (102, 103));
      probe_pairs.push_back (std::make_pair (102, 104));
      probe_pairs.push_back (std::make_pair (102, 105));
      probe_pairs.push_back (std::make_pair (102, 106));
      probe_pairs.push_back (std::make_pair (103, 104));
      probe_pairs.push_back (std::make_pair (103, 105));
      probe_pairs.push_back (std::make_pair (103, 106));
      probe_pairs.push_back (std::make_pair (104, 105));
      probe_pairs.push_back (std::make_pair (104, 106));
      probe_pairs.push_back (std::make_pair (105, 106));
      probe_pairs.push_back (std::make_pair (107, 108));
    }
  else if (scenario == "B")
    {
      failure_windows.push_back ((FailureWindow) {50.0, 250.0, 101, 102});
      // Keep events near-simultaneous while avoiding same-timestamp toggles.
      failure_windows.push_back ((FailureWindow) {50.001, 250.001, 101, 103});

      probe_pairs.push_back (std::make_pair (101, 102));
      probe_pairs.push_back (std::make_pair (101, 103));
      probe_pairs.push_back (std::make_pair (101, 104));
      probe_pairs.push_back (std::make_pair (101, 105));
      probe_pairs.push_back (std::make_pair (101, 106));
      probe_pairs.push_back (std::make_pair (102, 103));
      probe_pairs.push_back (std::make_pair (102, 104));
      probe_pairs.push_back (std::make_pair (102, 105));
      probe_pairs.push_back (std::make_pair (102, 106));
      probe_pairs.push_back (std::make_pair (103, 104));
      probe_pairs.push_back (std::make_pair (103, 105));
      probe_pairs.push_back (std::make_pair (103, 106));
      probe_pairs.push_back (std::make_pair (104, 105));
      probe_pairs.push_back (std::make_pair (104, 106));
      probe_pairs.push_back (std::make_pair (105, 106));
      probe_pairs.push_back (std::make_pair (107, 108));
    }
  else if (scenario == "C")
    {
      failure_windows.push_back ((FailureWindow) {30.0, 230.0, 101, 102});
      failure_windows.push_back ((FailureWindow) {320.0, 520.0, 101, 102});
      failure_windows.push_back ((FailureWindow) {610.0, 810.0, 101, 102});

      probe_pairs.push_back (std::make_pair (101, 102));
      probe_pairs.push_back (std::make_pair (101, 103));
      probe_pairs.push_back (std::make_pair (101, 104));
      probe_pairs.push_back (std::make_pair (101, 105));
      probe_pairs.push_back (std::make_pair (101, 106));
      probe_pairs.push_back (std::make_pair (102, 103));
      probe_pairs.push_back (std::make_pair (102, 104));
      probe_pairs.push_back (std::make_pair (102, 105));
      probe_pairs.push_back (std::make_pair (102, 106));
      probe_pairs.push_back (std::make_pair (107, 108));
    }
  else
    {
      NS_FATAL_ERROR ("Unsupported scenario: " << scenario << " (expected A/B/C)");
    }

  const double activity_end_guard_s = 10.0;
  const double routing_stop_s = simulation_duration_s - activity_end_guard_s;
  // Probe from t=10s with 1s period and keep sends strictly before (simulation_duration - 10s).
  const double last_probe_send_s = routing_stop_s;
  probe_count = static_cast<uint32_t> ((last_probe_send_s - probe_start_s) / probe_interval_s);

  for (std::map<uint16_t, Ptr<Bgp>>::iterator it = bgp_apps.begin (); it != bgp_apps.end (); ++it)
    {
      it->second->SetStartTime (Seconds (1.0));
      it->second->SetStopTime (Seconds (routing_stop_s));
    }

  uint16_t probe_port = 9000;
  ApplicationContainer responders;
  for (std::map<uint16_t, uint32_t>::const_iterator it = as_to_node_idx.begin ();
       it != as_to_node_idx.end (); ++it)
    {
      Ptr<UdpEchoResponder> responder = CreateObject<UdpEchoResponder> (probe_port);
      nodes.Get (it->second)->AddApplication (responder);
      responder->SetStartTime (Seconds (5.0));
      responder->SetStopTime (Seconds (routing_stop_s));
      responders.Add (responder);
    }

  ApplicationContainer probe_apps;
  for (uint32_t i = 0; i < probe_pairs.size (); ++i)
    {
      uint16_t src_as = probe_pairs.at (i).first;
      uint16_t dst_as = probe_pairs.at (i).second;

      Ptr<UdpProbeApp> probe = CreateObject<UdpProbeApp> ();
      std::ostringstream out;
      out << out_dir << "/probe_" << src_as << "_" << dst_as << ".csv";

      probe->Configure (src_as, dst_as, probe_ip_per_as.at (dst_as), probe_port, probe_count,
                        Seconds (probe_interval_s), Seconds (probe_timeout_s), out.str ());
      nodes.Get (as_to_node_idx.at (src_as))->AddApplication (probe);
      // Slightly stagger starts to avoid synchronized bursts from many probe apps.
      probe->SetStartTime (Seconds (probe_start_s + static_cast<double> (i) * 0.001));
      probe->SetStopTime (Seconds (routing_stop_s));
      probe_apps.Add (probe);
    }

  std::ofstream event_out;
  event_out.open ((out_dir + "/link_events.csv").c_str (), std::ios::out | std::ios::trunc);
  event_out << "time_s,event,as_a,as_b" << std::endl;

  for (uint32_t i = 0; i < failure_windows.size (); ++i)
    {
      const FailureWindow &fw = failure_windows.at (i);
      std::map<std::string, uint32_t>::const_iterator it =
          link_key_to_runtime_idx.find (PairKey (fw.as_a, fw.as_b));
      if (it == link_key_to_runtime_idx.end ())
        {
          NS_FATAL_ERROR ("Scenario references unknown link AS" << fw.as_a << "-AS" << fw.as_b);
        }
      LinkRuntime *link = &runtimes.at (it->second);

      if (virtual_ixp == 0)
        {
          Simulator::Schedule (Seconds (fw.down_s), &SetLinkState, link, false, &event_out);
          Simulator::Schedule (Seconds (fw.up_s), &SetLinkState, link, true, &event_out);
          continue;
        }

      static uint32_t next_link_index = 1;
      static Ptr<VirtualIxpFabric> fabric;
      static std::map<std::string, VirtualIxpPair> pairs;

      if (fabric == 0)
        {
          fabric = CreateObject<VirtualIxpFabric> ();
          IxpConfig cfg;
          cfg.portCount = virtual_ixp_port_count;
          cfg.rebalancePeriod = Seconds (virtual_ixp_rebalance_s);
          cfg.holdDown = Seconds (virtual_ixp_hold_down_s);
          fabric->Configure (cfg);
          fabric->Start ();
        }

      const std::string pair_key = PairKey (fw.as_a, fw.as_b);
      if (pairs.find (pair_key) == pairs.end ())
        {
          VirtualIxpPair pair;
          pair.as_a = fw.as_a;
          pair.as_b = fw.as_b;
          pair.quality_a = 1.0;
          pair.quality_b = 1.0;

          pair.side_a.asn = fw.as_a;
          pair.side_a.linkIndex = next_link_index++;
          pair.side_b.asn = fw.as_b;
          pair.side_b.linkIndex = next_link_index++;

          Ptr<BgpIxpEndpointAdapter> ep_a = CreateObject<BgpIxpEndpointAdapter> ();
          ep_a->Bind (fw.as_a, link->node_a, link->iface_a);
          fabric->RegisterEndpoint (pair.side_a, ep_a);

          Ptr<BgpIxpEndpointAdapter> ep_b = CreateObject<BgpIxpEndpointAdapter> ();
          ep_b->Bind (fw.as_b, link->node_b, link->iface_b);
          fabric->RegisterEndpoint (pair.side_b, ep_b);

          fabric->SetFeasibleUp (pair.side_a, pair.quality_a);
          fabric->SetFeasibleUp (pair.side_b, pair.quality_b);

          pairs[pair_key] = pair;
        }

      const VirtualIxpPair pair = pairs.at (pair_key);
      Simulator::Schedule (Seconds (fw.down_s), &SetVirtualIxpPairState, fabric, pair, false,
                           &event_out);
      Simulator::Schedule (Seconds (fw.up_s), &SetVirtualIxpPairState, fabric, pair, true,
                           &event_out);
    }

  for (Time t = Seconds (20.0); t <= Seconds (simulation_duration_s); t += Seconds (20.0))
    {
      Simulator::Schedule (t, &SnapshotRoutingTables, as_nodes, bgp_apps, out_dir);
    }

  Simulator::Stop (Seconds (simulation_duration_s));
  Simulator::Run ();
  Simulator::Destroy ();

  event_out.close ();

  return 0;
}
