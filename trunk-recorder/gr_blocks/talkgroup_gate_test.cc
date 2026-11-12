// Offline check of Talkgroup_Gate, the stream-tag gate that keeps another
// talkgroup's audio out of streaming plugins. Build with
// -DTALKGROUP_GATE_TESTS=ON and run build/talkgroup_gate_test; it exits
// non-zero if any expectation fails.

#include "talkgroup_gate.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int passed = 0;
int failures = 0;

typedef Talkgroup_Gate G;

std::string runs(const G::Result &r) {
  std::ostringstream s;
  for (const G::Run &run : r.runs) {
    s << "[" << run.start << "," << run.end << ")";
  }
  return s.str();
}

void expect(const G::Result &r, const std::string &want_runs, int want_dropped, const std::string &what) {
  bool ok = runs(r) == want_runs && r.dropped == want_dropped;
  std::cout << (ok ? "ok   " : "FAIL ") << what;
  if (!ok) {
    std::cout << " (runs " << runs(r) << " dropped " << r.dropped << ", want " << want_runs << " dropped " << want_dropped << ")";
    ++failures;
  } else {
    ++passed;
  }
  std::cout << std::endl;
}

void expect(bool ok, const std::string &what) {
  std::cout << (ok ? "ok   " : "FAIL ") << what << std::endl;
  ok ? ++passed : ++failures;
}

G::Tag grp(int offset, long tg) { return {offset, G::GRP_ID, tg}; }
G::Tag term(int offset) { return {offset, G::TERMINATE, 0}; }

const long TG = 100;
const long OTHER = 200;

} // namespace

int main() {
  {
    G gate;
    expect(gate.process(160, {grp(0, OTHER), term(80)}), "[0,160)", 0, "a new gate passes everything");
    gate.enable(TG);
    gate.disable();
    expect(gate.process(160, {grp(40, OTHER)}), "[0,160)", 0, "a disabled gate passes everything");
  }
  {
    G gate;
    gate.enable(TG);
    expect(gate.process(160, {}), "[0,160)", 0, "no tags passes all");
    expect(gate.process(0, {}), "", 0, "an empty window forwards nothing");
  }
  {
    G gate;
    gate.enable(TG);
    G::Result r = gate.process(1440, {grp(320, OTHER)});
    expect(r, "", 1440, "call-start bleed: a mismatched grp_id drops the prefix and the rest");
    expect(r.received_talkgroup == OTHER, "the drop reports the received talkgroup");
    expect(gate.ignoring(), "the gate is ignoring after a mismatch");
  }
  {
    G gate;
    gate.enable(TG);
    expect(gate.process(800, {grp(100, OTHER), term(400)}), "[400,800)", 400, "untagged audio after a terminate resumes");
    expect(!gate.ignoring(), "a terminate leaves ignoring");
  }
  {
    G gate;
    gate.enable(TG);
    expect(gate.process(800, {grp(100, OTHER), grp(500, TG)}), "[500,800)", 500, "a matching grp_id resumes at the tag");
  }
  {
    G gate;
    gate.enable(TG);
    expect(gate.process(800, {grp(300, TG), grp(600, TG)}), "[0,800)", 0, "matching grp_ids forward the prefix as one run");
  }
  {
    G gate;
    gate.enable(TG);
    expect(gate.process(800, {grp(200, TG), grp(500, OTHER)}), "[0,200)", 600, "a mismatch drops only audio after the last boundary");
  }
  {
    G gate;
    gate.enable(TG);
    expect(gate.process(800, {term(300), grp(600, OTHER)}), "[0,300)", 500, "a terminate settles the audio before it");
  }
  {
    G gate;
    gate.enable(TG);
    gate.process(160, {grp(0, OTHER)});
    expect(gate.process(160, {}), "", 160, "ignoring persists across windows with no tags");
    expect(gate.process(160, {term(160)}), "", 160, "a terminate at the window end drops what precedes it");
    expect(gate.process(160, {}), "[0,160)", 0, "and the next window passes");
  }
  {
    G gate;
    gate.enable(TG);
    gate.process(160, {grp(0, OTHER)});
    gate.enable(OTHER);
    expect(!gate.ignoring(), "reconfiguring resets ignoring");
    expect(gate.process(160, {}), "[0,160)", 0, "the reconfigured gate passes untagged audio");
    expect(gate.process(160, {grp(80, TG)}), "", 160, "and gates on the new talkgroup");
  }

  std::cout << passed << " passed, " << failures << " failed" << std::endl;
  return failures > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
