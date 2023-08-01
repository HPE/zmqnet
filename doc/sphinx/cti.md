# CTI

Before launching the network, there must be an active job running on the system. The communication network will be launched inside the job allocation. You must use the CTI library to either launch a new job using the workload manager, or attach to an existing job. Once a job is registered with CTI, ZMQNet uses it to launch "tool daemons" alongside the job inside the job's allocation. From the workload manager's perspective, tool daemons can be any process. The leaf backend processes, commnode intermediate processes, and your tool that is calling ZMQNet are all tool daemons that are launched into the job allocation.

For a job using N compute nodes, ZMQNet will launch N leaf backend processes, one on each compute node. Intermediate commnode processes will be automatically launched as needed to complete the network topology.

This guide provides two basic examples on launching and attaching to a workload manager job for use with ZMQNet. For more information, review the CTI documentation.

## Launching a job

For widest compatibility, the target application to launch under CTI should be an MPI application. If you are intending to start the communication network *before* the target application starts (but after its allocation is assigned by the workload manager), e.g. for complete application analysis, this would be using the "startup barrier" functionality of CTI. If you use a startup barrier, the ALPS and PALS workload managers require that the target application be an MPI application.

The first step of the job launch process is to create an argument array for the launch of your application. This array contains not only your application arguments, but also the arguments that are to be passed to the workload manager, such as the number of ranks (but not the name of the launcher itself).

For example, to launch the target application `a.out` across four ranks,

	char const* launcher_args[] = {"-n4", "./a.out", "application_argument", nullptr};

Next, request an application launch with a startup barrier from CTI. The application launch function will return an ID that is used to interact with the launched job.

	int app_id = cti_launchAppBarrier(launcher_args,
		-1, // Suppress application standard out
		-1, // Suppress application standard out
		nullptr, // No standard in from a file path
		nullptr, // Keep current working directory
		nullptr); // No additional environment variables
	if (app_id == 0) {
		// Launch failed, error diagnostic can be obtained from `cti_error_str()`
	}

After a successful barrier launch, you have a successful resource allocation from the workload manager. The target applications have been started on all compute nodes, but stopped at a startup barrier before `main`. You can then proceed to set up the ZMQNet frontend and network. After the network and your leaf backend processes have been started, you can then continue the target application, allowing it to launch.

	if (cti_releaseAppBarrier(app_id)) {
		// Barrier release failed, error diagnostic can be obtained from `cti_error_str()`
	}

## Attaching to a job

If you would instead like to attach to a running job, you will use the CTI job attach procedure. Because each workload manager requires different types of data to identify running jobs, the CTI job attach interface uses WLM-specific functions.

This example will show how to attach to a running Slurm job using the PID of the Slurm launcher that started the job. More information on attaching and using different WLM operations can be found in the CTI developer documentation.

	// Application ID will be provided by WLM-specific attach
	int app_id = 0;

	// Each supported WLM type should be handled for attach code
	switch (cti_current_wlm()) {

	case CTI_WLM_SLURM:
		// Create an empty ops struct pointer
		{ cti_slurm_ops_t *ops = nullptr;

			// Call the ops struct fill function
			if (cti_open_ops((void **)&ops) == nullptr) {
				// Failed to fill ops struct
			}

			// Get the job and step ID from Slurm using the launcher PID
			auto srun_info = ops->getJobInfo(launcherPid);
			if (srun_info == nullptr) {
				// Error diagnostic can be obtained from `cti_error_str()`
			}

			// Attach to job and get application ID for later use
			app_id = ops->registerJobStep(srun_info->jobid, srun_info->stepid);
			if (app_id == 0) {
				// Error diagnostic can be obtained from `cti_error_str()`
			}
		}
		break;

	// Other WLM cases here
	}

Once you have a valid application ID from CTI, you can proceed with launching the ZMQNet network.

## Sessions and Manifests

In addition to job lifecycle management, CTI also provides a system to ship files and dependencies to temporary directories on compute nodes to support running tools. For efficiency, CTI will deduplicate shipped files. To support this, there are two file support concepts, sessions and manifests.

A session represents a single temporary remote directory. Sessions own manifests. A manifest is an in-progress list of files that will be shipped to a remote temporary directory. Files are deduplicated across manifests and inside of a single session, but not between sessions.

When CTI launches a tool daemon, it consumes a manifest. As part of the tool daemon launch, CTI will determine dynamic library dependencies of the tool daemon and automatically add them to the manifest to be shipped. Other dependency files, such as configuration files, can be added to the manifest before using the same manifest to launch the tool daemon. Then, the tool daemon will have access to these extra dependency files.

Sessions are created using the application ID from a job previously registered with CTI.

	auto session_id = cti_createSession(app_id);

Manifests are created using the session ID from a previously created session.

	auto manifest_id = cti_createManifest(session_id);

Extra dependencies can be added to the manifest using a different type of function for different dependency types. Adding a binary or shared library will automatically add the dynamically linked library dependencies to the manifest as well. Tool daemon binaries will automatically have all shipped libraries in its library search path as well.

	cti_addManifestBinary(manifest_id, "./leaf_binary");
	cti_addManifestLibrary(manifest_id, "./libfilter.so");
	cti_addManifestFile(manifest_id, "./info.txt");

When launching a network, ZMQNet consumes both a session and manifest ID. Internally, it uses the manifest tool daemon launch functions to ship the files and create the network. To use ZMQNet, it is sufficient to understand creating sessions and manifests, and adding files to manifests. To learn more about shipping manifests, refer to the CTI developer documentation.


