# ZMQNET

ZMQNet is a scalable, high performance networking library. It implements a tree-type command distribution and data processing model. Commands are sent from the root of the tree (the frontend) and distributed to leaves of the tree (backends). Intermediate processes, commnodes, are automatically managed by ZMQNet. Results are created by backends, collected and processed by commnodes, and communicated to the frontend.

ZMQNet is designed to run alongside an existing HPC job, building a tree communication and aggregation structure to support job analysis, debugging, and correctness tools.

In this guide, the "frontend application" will refer to the calling code making use of the ZMQNet library that issues commands and collects results. The "network frontend", by contrast, will refer to the ZMQNet network frontend instance itself. Likewise, the "backend application" will be hosting the "network backend" to receive commands and produce results.

ZMQNet is based on the ZMQ message-queue library. It uses several features of ZMQ beyond scalable message distribution, most notably client authentication and traffic encryption.

ZMQNet also uses the CTI Common Tools Interface library to automatically manage file shipping and application lifecycle. CTI is an interface library that provides a single interface for launching and attaching to running HPC jobs across several different workload managers, including Slurm, PBS / PALS, and Flux. To its end of supporting job analysis tools, ZMQNet requires a launch alongside a running HPC job. Calling code will use CTI to launch a new HPC job, or attach to an existing job.

