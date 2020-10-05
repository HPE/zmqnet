@0xeb238c6db811bf3c;

struct TopologyRequest {
  nodeCommandEndpoint @0 :Text;
  nodeDataEndpoint @1 :Text;
}

struct TopologyResponse {
  parentCommandEndpoint @0 :Text;
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
  streamId @0 :Int32;

  union {
    ack @1 :Void;
    userData @2 :Text;
  }
}
