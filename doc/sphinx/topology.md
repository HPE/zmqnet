# NETWORK TOPOLOGY

There are two parameters for the network topology type, launch type and fanout. The launch type will determine how backend leaf processes and commnode intermediate processes will connect to the frontend. The fanout determines how many children each non-leaf commnode will have in the communication tree. For example, with a fanout of 32, a communication tree with 32 backend leaf processes will have one commnode intermediate process and a fanout of 64 will have two commnode processes.

There are two main launch strategies, first-come first-served and layered. FCFS will request a single tool daemon launch from the workload manager. This is generally the fastest launch strategy. The other launch type, layered, will invoke a new tool daemon launch for each layer of the communication tree. This is generally the slowest launch strategy. While it reduces the overall network traffic during the initial launch, it is dwarfed by the high overhead of multiple workload manager tool daemon launches.

To create a topology with the first-come first-served launch strategy and a fanout of 32,

	auto topology = zmqnet::Topology
		{ .launch = zmqnet::Topology::Fcfs
		, .fanout = 32
	};


