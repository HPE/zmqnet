@0xeb238c6db811bf3c;

struct TopologyRequest {
  nodeControlEndpoint @0 :Text;
  nodeDataEndpoint @1 :Text;
  nodeHostname @2 :Text;
  nodePublicKey @3 :Text;
}

struct TopologyResponse {
  nodeId @0 :Int32;

  parentControlEndpoint @1 :Text;
  parentDataEndpoint @2 :Text;
  parentPublicKey @3 :Text;

  numChildren @4 :Int32;
}

struct NetworkCommand {
  union {
    exit @0 :Void;

    kill @13 :Void;

    startLeaves @1 :Void;

    ping @12 :Text;

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
        timeoutAfterLast @14 :Int32;
      }
    }

    user :group {
      streamId @10 :Int32;
      content @11 :Data;
    }
  }
}

enum NodeType {
  unset @0;
  leaf @1;
  commnode @2;
}

struct NetworkResponse {
  union {
    exit :group {
      nodes @0 :Data;
      nodeType @10 :NodeType = unset;
    }

    ready :group {
      nodes @1 :Data;
    }

    ping :group {
      nodeId @9 :Int32;
      payload @11 :Text;
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
