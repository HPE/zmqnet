@0xeb238c6db811bf3c;

struct TopologyRequest {
  nodeControlEndpoint @0 :Text;
  nodeDataEndpoint @1 :Text;
}

struct TopologyResponse {
  parentControlEndpoint @0 :Text;
  parentDataEndpoint @1 :Text;

  numChildren @2 :Int32;
}

struct NetworkCommand {
  union {
    exit @0 :Void;

    newStream :group {
      newStreamId @1 :Int32;
      filterName @2 :Text;
    }

    userCommand :group {
      streamId @3 :Int32;
      content @4 :Text;
    }
  }
}

struct NetworkResponse {
  union {
    readyAck @0 :Void;
    exitAck @1 :Void;

    streamAck :group {
      streamId @2 :Int32;
    }

    userResult :group {
      streamId @3 :Int32;
      content @4 :Text;
    }
  }
}
