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

    stream :group {
      streamId @2 :Int32;
      filterLibraryName @3 :Text;
      filterFunctionName @4 :Text;
    }

    subset :group {
      streamId @5 :Int32;
      subsetId @6 :Int32;
      ranks @7 :Data;
    }

    user :group {
      streamId @8 :Int32;
      content @9 :Data;
    }
  }
}

struct NetworkResponse {
  union {
    exit @0 :Void;

    ready :group {
      ranks @1 :Data;
    }

    stream :group {
      streamId @2 :Int32;
    }

    subset :group {
      streamId @3 :Int32;
      subsetId @4 :Int32;
      ranks @5 :Data;
    }

    user :group {
      streamId @6 :Int32;
      ranks @7 :Data;
      content @8 :Data;
    }
  }
}
