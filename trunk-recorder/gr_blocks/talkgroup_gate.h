#ifndef TALKGROUP_GATE_H
#define TALKGROUP_GATE_H

#include <vector>

// Decides which audio samples belong to the expected talkgroup, using the
// grp_id and terminate stream tags the voice decoder places on its output.
// Audio is held from each boundary (window start, grp_id, terminate) until
// the next one decides it: a mismatched grp_id drops the held audio and
// ignores everything up to the next terminate or matching grp_id. Audio still
// undecided at the end of a window is forwarded unless ignoring.
class Talkgroup_Gate {
public:
  enum Tag_Kind { GRP_ID,
                  TERMINATE };

  struct Tag {
    int offset; // relative to the window start
    Tag_Kind kind;
    long talkgroup; // GRP_ID only
  };

  struct Run {
    int start;
    int end;
  };

  struct Result {
    std::vector<Run> runs; // to forward, in order, non-empty, non-adjacent
    int dropped;
    long received_talkgroup; // the mismatched grp_id behind the drop
  };

  Talkgroup_Gate();

  // Gates on the encoded talkgroup and clears the ignoring state.
  void enable(long talkgroup);
  // Passes everything.
  void disable();

  bool enabled() const { return d_enabled; }
  bool ignoring() const { return d_ignoring; }
  long talkgroup() const { return d_talkgroup; }

  // tags must be in offset order, with offsets within [0, nitems].
  Result process(int nitems, const std::vector<Tag> &tags);

private:
  bool d_enabled;
  bool d_ignoring;
  long d_talkgroup;
  long d_received_talkgroup;
};

#endif
