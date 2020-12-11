@0xeb238c6db811bf3c;

struct TopologyRequest {
  nodeControlEndpoint @0 :Text;
  nodeDataEndpoint @1 :Text;
  nodeHostname @2 :Text;
}

struct TopologyResponse {
  parentControlEndpoint @0 :Text;
  parentDataEndpoint @1 :Text;

  numChildren @2 :Int32;
}

struct NetworkCommand {
  union {
    exit @0 :Void;

    startLeaves @1 :Void;

    communicator :group {
      communicatorId @2 :Int32;
      ranks @3 :Data;
    }

    stream :group {
      streamId @4 :Int32;
      filterLibraryName @5 :Text;
      filterFunctionName @6 :Text;
    }

    user :group {
      streamId @7 :Int32;
      content @8 :Data;
    }
  }
}

struct NetworkResponse {
  union {
    exit @0 :Void;

    ready :group {
      ranks @1 :Data;
    }

    communicator :group {
      communicatorId @2 :Int32;
      ranks @3 :Data;
    }

    stream :group {
      streamId @4 :Int32;
    }

    user :group {
      streamId @5 :Int32;
      ranks @6 :Data;
      content @7 :Data;
    }
  }
}
