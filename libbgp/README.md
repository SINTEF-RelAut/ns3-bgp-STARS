# Bundled libbgp

This is a copy of [libbgp](https://github.com/Nat-Lab/libbgp) 0.6.3 (upstream commit
`c86b6eb`), MIT licensed (see `LICENSE`), compiled as part of the ns-3 `bgp` module
instead of linking a system-wide installation. Only the `.cc`/`.h` files of upstream
`src/` are included; nothing else of the upstream tree is needed.

It is bundled because the simulations depend on fixes that upstream does not have, and
shipping them here means a checkout of this module builds the fixed version without a
separately patched libbgp install. All local changes are marked `ns3-bgp-STARS:` in the
source. They make libbgp send the UPDATEs RFC 4271 requires when the best route changes,
whose absence left stale routes behind and caused persistent forwarding loops:

1. `BgpRib4::withdraw`/`BgpRib6::withdraw`: a withdrawal of a prefix the peer never sent
   returned the "now unreachable" result, so the session withdrew the prefix from every
   peer. It now returns the "not in RIB" result, which the session ignores.
2. `BgpFsm::fsmEvalEstablished`: an UPDATE whose AS_PATH contains our own ASN is still
   rejected, but now also withdraws that peer's previous route for those prefixes
   (implicit withdraw, RFC 4271 9.1.2/9.1.3).
3. `BgpFsm::fsmEvalEstablished`: the event bus never delivers an event to the session
   that published it. That session now itself withdraws prefixes from its peer whose best
   route has become that peer's own, and announces to its peer the routes of other peers
   that its peer's update or withdrawal made the best.
4. `BgpFsm::handleRoute4AddEvent`: a session receiving a replaced entry learned from its
   own peer used to skip it silently; it now withdraws the prefix from that peer, which
   would otherwise keep the route we announced to it before.

5. `BgpFsm` keeps an IPv4 Adj-RIB-Out (`adj_rib_out4`, RFC 4271 3.2): the prefixes announced
   to the peer and not withdrawn since, with announcements the peer rejects for containing
   its ASN left out. Every withdrawal, upstream's own included, is limited to these, so the
   session no longer withdraws prefixes the peer never had from it.

IPv6 gets fix 1 only; the simulations use IPv4.

A related change outside this directory: `Bgp::AddRoute` in `model/bgp.cc` originates local
routes with weight 32768 instead of 0, so a router never prefers a peer's route to its own
prefix. With the fixes above, link subnets originated by both ends of a link otherwise made
the routers prefer each other's routes in a cycle that never converged.
