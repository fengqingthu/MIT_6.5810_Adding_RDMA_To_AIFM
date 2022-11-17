"""Two 10-core servers connected by two ConnectX-4 NICs for running AIFM with RDMA backend."""

# Import the Portal object.
import geni.portal as portal
# Import the ProtoGENI library.
import geni.rspec.pg as pg
# Import the Emulab specific extensions.
import geni.rspec.emulab as emulab

# Describe the parameter(s) this profile script can accept.
portal.context.defineParameter( "n", "Number of Machines", portal.ParameterType.INTEGER, 2 )

# Retrieve the values the user specifies during instantiation.
params = portal.context.bindParameters()

# Create a portal object,
pc = portal.Context()

# Create a Request object to start building the RSpec.
request = pc.makeRequestRSpec()

nodes = []
ifaces = [None] * params.n * 2

# nodes.append(node_0)
# ifaces.append(iface0)

for i in range(0, params.n):
    n = request.RawPC('node-%d' % i)
    n.routable_control_ip = True
    n.hardware_type = 'xl170'
    n.disk_image = 'urn:publicid:IDN+emulab.net+image+emulab-ops//UBUNTU18-64-STD'

    for j in range(2):
        ifaces[i * 2 + j] = n.addInterface('interface-%d' % (j), pg.IPv4Address('192.168.6.%d' % (i * 2 + j + 1),'255.255.255.0'))
    nodes.append(n)


# Link link-0
link_0 = request.LAN('link-0')
link_0.best_effort = False
link_0.bandwidth = 25000000
link_0.setNoInterSwitchLinks()
link_0.Site('undefined')
link_0.addInterface(ifaces[0])
link_0.addInterface(ifaces[2])

# Link link-1
link_1 = request.LAN('link-1')
link_1.best_effort = False
link_1.bandwidth = 25000000
link_1.setNoInterSwitchLinks()
link_1.Site('undefined')
link_1.addInterface(ifaces[1])
link_1.addInterface(ifaces[3])

# Print the generated rspec
pc.printRequestRSpec(request)
