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

    startLeaves @5 :Void;

    stream :group {
      streamId @1 :Int32;
      filterLibraryName @2 :Text;
      filterFunctionName @6 :Text;
      ranks @7 :Data;
    }

    user :group {
      streamId @3 :Int32;
      content @4 :Data;
    }
  }
}

struct NetworkResponse {
  union {
    exit @0 :Void;

    ready @1 :Void;

    stream :group {
      streamId @2 :Int32;
    }

    user :group {
      streamId @3 :Int32;
      ranks @5 :Data;
      content @4 :Data;
    }
  }
}
