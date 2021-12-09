@0xeb238c6db811bf3c;

struct TopologyRequest {
  nodeControlEndpoint @0 :Text;
  nodeDataEndpoint @1 :Text;
  nodeHostname @2 :Text;
}

struct TopologyResponse {
  nodeId @0 :Int32;

  parentControlEndpoint @1 :Text;
  parentDataEndpoint @2 :Text;

  numChildren @3 :Int32;
}

struct NetworkCommand {
  union {
    exit @0 :Void;

    startLeaves @1 :Void;

    communicator :group {
      communicatorId @2 :Int32;
      nodes @3 :Data;
    }

    stream :group {
      streamId @4 :Int32;
      filterLibraryName @5 :Text;
      filterFunctionName @6 :Text;
      sync :union {
        immediate @7 :Void;
        waitForAll @8 :Void;
        timeout @9 :Int32;
      }
    }

    user :group {
      streamId @10 :Int32;
      content @11 :Data;
    }
  }
}

struct NetworkResponse {
  union {
    exit :group {
      nodes @0 :Data;
    }

    ready :group {
      nodes @1 :Data;
    }

    communicator :group {
      communicatorId @2 :Int32;
      nodes @3 :Data;
    }

    stream :group {
      streamId @4 :Int32;
      nodes @5 :Data;
    }

    user :group {
      streamId @6 :Int32;
      nodes @7 :Data;
      content @8 :Data;
    }
  }
}
