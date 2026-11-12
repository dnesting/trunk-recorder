#include "talkgroup_gate.h"

Talkgroup_Gate::Talkgroup_Gate()
    : d_enabled(false), d_ignoring(false), d_talkgroup(0), d_received_talkgroup(0) {}

void Talkgroup_Gate::enable(long talkgroup) {
  d_enabled = true;
  d_ignoring = false;
  d_talkgroup = talkgroup;
  d_received_talkgroup = 0;
}

void Talkgroup_Gate::disable() {
  d_enabled = false;
  d_ignoring = false;
}

Talkgroup_Gate::Result Talkgroup_Gate::process(int nitems, const std::vector<Tag> &tags) {
  Result result{{}, 0, 0};

  auto settle = [&](int start, int end) {
    if (end <= start) {
      return;
    }
    if (d_ignoring) {
      result.dropped += end - start;
    } else if (!result.runs.empty() && result.runs.back().end == start) {
      result.runs.back().end = end;
    } else {
      result.runs.push_back({start, end});
    }
  };

  if (!d_enabled) {
    settle(0, nitems);
    return result;
  }

  int seg_start = 0;
  for (const Tag &tag : tags) {
    if (tag.kind == GRP_ID && tag.talkgroup != d_talkgroup) {
      d_ignoring = true;
      d_received_talkgroup = tag.talkgroup;
      settle(seg_start, tag.offset);
    } else {
      settle(seg_start, tag.offset);
      d_ignoring = false;
    }
    seg_start = tag.offset;
  }
  settle(seg_start, nitems);

  result.received_talkgroup = d_received_talkgroup;
  return result;
}
