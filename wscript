# def options(opt):
#     pass

def configure(conf):
    # libbgp is compiled into this module from the bundled copy in libbgp/ (see
    # libbgp/README.md), not linked from a system install: it carries fixes upstream lacks.
    # Linking a system libbgp as well would let the dynamic linker resolve its symbols to the
    # unpatched copy.
    pass

def build(bld):
    module = bld.create_ns3_module('bgp', ['core', 'internet', 'network'])
    # <libbgp/...> resolves to the bundled copy, for this module and for everything using it.
    module.includes = ['.']
    module.export_includes = ['.']
    module.source = [
        'libbgp/bgp-bad-message.cc',
        'libbgp/bgp-capability.cc',
        'libbgp/bgp-errcode.cc',
        'libbgp/bgp-filter.cc',
        'libbgp/bgp-fsm.cc',
        'libbgp/bgp-keepalive-message.cc',
        'libbgp/bgp-log-handler.cc',
        'libbgp/bgp-notification-message.cc',
        'libbgp/bgp-open-message.cc',
        'libbgp/bgp-packet.cc',
        'libbgp/bgp-path-attrib.cc',
        'libbgp/bgp-rib4.cc',
        'libbgp/bgp-rib6.cc',
        'libbgp/bgp-sink.cc',
        'libbgp/bgp-update-message.cc',
        'libbgp/fd-out-handler.cc',
        'libbgp/prefix4.cc',
        'libbgp/prefix6.cc',
        'libbgp/realtime-clock.cc',
        'libbgp/route-event-bus.cc',
        'libbgp/serializable.cc',
        'model/bgp.cc',
        'model/bgp-routing.cc',
        'model/bgp-ns3-clock.cc',
        'model/bgp-ns3-fsm.cc',
        'model/bgp-ns3-socket-in.cc',
        'model/bgp-ns3-socket-out.cc',
        'model/bgp-log.cc'
        ]

    headers = bld(features='ns3header')
    headers.module = 'bgp'
    headers.source = [
        'model/bgp.h',
        'model/bgp-routing.h',
        'model/bgp-ns3-clock.h',
        'model/bgp-ns3-fsm.h',
        'model/bgp-ns3-socket-in.h',
        'model/bgp-ns3-socket-out.h',
        'model/bgp-log.h'
        ]

    if bld.env.ENABLE_EXAMPLES:
        bld.recurse('examples')

    # bld.ns3_python_bindings()

