// Offline check of the simplestream JSON wire for source attribution.
// Drives the plugin hooks directly with a real Call_impl/System_impl and reads
// the emitted packets from a loopback UDP socket. Build with
// -DSIMPLESTREAM_TESTS=ON and run build/plugins/simplestream/simplestream_test;
// it exits non-zero on the first failed expectation.

#include "simplestream.cc"

#include "../../trunk-recorder/call_impl.h"
#include "../../trunk-recorder/systems/system.h"

#include <cstdlib>
#include <iostream>

namespace {

int failures = 0;

// Exposes the control-channel source setter that grant handling uses.
struct Test_Call : Call_impl {
  using Call_impl::Call_impl;
  using Call_impl::add_source;
};

void expect(bool ok, const std::string &what) {
  std::cout << (ok ? "ok   " : "FAIL ") << what << std::endl;
  if (!ok) {
    ++failures;
  }
}

class Wire {
public:
  Wire() : socket_(io_, ip::udp::endpoint(ip::address_v4::loopback(), 0)) {}

  unsigned short port() const { return socket_.local_endpoint().port(); }

  json next() {
    std::vector<char> buf(65536);
    ip::udp::endpoint from;
    size_t n = socket_.receive_from(boost::asio::buffer(buf), from);
    uint32_t json_length = 0;
    std::memcpy(&json_length, buf.data(), sizeof(json_length));
    if (n < sizeof(json_length) + json_length) {
      throw std::runtime_error("short packet");
    }
    return json::parse(std::string(buf.data() + sizeof(json_length), json_length));
  }

private:
  boost::asio::io_context io_;
  ip::udp::socket socket_;
};

void codec(Simple_Stream &plugin, Call *call, long ota_source, int frames = 3) {
  uint32_t params[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  for (int i = 0; i < frames; ++i) {
    params[0] = static_cast<uint32_t>(ota_source + i);
    Voice_Codec_Frame frame;
    frame.codec_type = CODEC_P25_IMBE;
    frame.tgid = call->get_talkgroup();
    frame.params = params;
    frame.param_count = 8;
    frame.ota_talkgroup = call->get_talkgroup();
    frame.ota_source = ota_source;
    frame.ota_id_time_ms = ota_source > 0 ? 1 : 0;
    plugin.voice_codec_frame(call, frame);
  }
}

json audio(Simple_Stream &plugin, Wire &wire, Call *call, Recorder *rec, int samples, bool silent = false) {
  std::vector<int16_t> pcm(samples, silent ? 0 : 100);
  plugin.audio_stream(call, rec, pcm.data(), samples);
  return wire.next();
}

} // namespace

int main() {
  const long A = 1116734; // control-channel source for the whole capture
  const long B = 1113737; // a second unit that keys up in the same capture

  Wire wire;
  Simple_Stream plugin;
  json config = {{"streams", json::array({{{"TGID", 0},
                                           {"address", "127.0.0.1"},
                                           {"port", wire.port()},
                                           {"sendJSON", true},
                                           {"sendCallStart", true},
                                           {"sendCallEnd", true},
                                           {"shortName", "t"}}})}};
  plugin.parse_config(config);
  plugin.start();

  System *sys = System::make(0);
  sys->set_short_name("t");
  Config cfg;
  Test_Call call(729, 851000000, sys, cfg);
  call.add_source(A);
  Recorder rec(P25);

  plugin.call_start(&call);
  json p = wire.next();
  expect(p["event"] == "call_start" && p["src"] == A, "call_start carries the control-channel source");

  // Key-up 1, before its link control: falls back to the control channel.
  codec(plugin, &call, -1);
  p = audio(plugin, wire, &call, &rec, 160);
  expect(p["src"] == A && p["seq"] == 0 && p["start_sample"] == 0, "pre-LC audio uses control-channel src");
  expect(!p.contains("ota_src"), "no ota_src before link control");

  codec(plugin, &call, A);
  p = audio(plugin, wire, &call, &rec, 160);
  expect(p["src"] == A && p["seq"] == 1 && p["start_sample"] == 160, "same unit keeps one seq/sample run");

  // Hang time after a terminator: no codec frames, so the silence packets
  // carry the lane's last identity (and no PCM).
  p = audio(plugin, wire, &call, &rec, 160, true);
  expect(p["src"] == A && p["payload_length"] == 0 && p["seq"] == 2 && p["ota_frames"] == 0,
         "hang-time silence carries the last identity without PCM");

  // Key-up 2 by B while the control channel still says A. The decoder cleared
  // its identity at the terminator, so frames before B's link control fall back.
  codec(plugin, &call, -1);
  p = audio(plugin, wire, &call, &rec, 160);
  expect(p["src"] == A && !p.contains("ota_src") && p["seq"] == 3 && p["start_sample"] == 320,
         "new key-up before its link control falls back, never inherits a unit");

  codec(plugin, &call, B);
  p = audio(plugin, wire, &call, &rec, 320);
  expect(p["src"] == B && p["ota_src"] == B, "voice-channel talker labels the audio");
  expect(p["seq"] == 0 && p["start_sample"] == 0, "a new source starts its own counters");
  expect(p["voice_fingerprints"].size() == 2, "B's codec frames are fingerprinted under B");
  p = audio(plugin, wire, &call, &rec, 160);
  expect(p["src"] == B && p["seq"] == 1 && p["start_sample"] == 320, "B continues its run");

  // Reserved/system unit id from the decoder: fall back, but report ota_src.
  codec(plugin, &call, 0xFFFFFD, 1);
  p = audio(plugin, wire, &call, &rec, 160);
  expect(p["src"] == A && p["ota_src"] == 0xFFFFFD, "reserved unit id falls back to control-channel src");
  expect(p["seq"] == 4 && p["start_sample"] == 480, "A resumes its own counters");

  // Key-up 3 by A again.
  codec(plugin, &call, A);
  p = audio(plugin, wire, &call, &rec, 160);
  expect(p["src"] == A && p["seq"] == 5 && p["start_sample"] == 640, "A continues after B");

  Call_Data_t info{};
  info.call_num = call.get_call_num();
  info.talkgroup = call.get_talkgroup();
  info.freq = call.get_freq();
  info.short_name = "t";
  info.capture_start_time_ms = call.get_capture_start_time_ms();
  info.stop_time_ms = 0;
  info.phase2_tdma = false;
  info.tdma_slot = -1;
  plugin.capture_end(&call, info);
  p = wire.next();
  expect(p["event"] == "capture_end" && p["src"] == A, "capture_end names the final run's source");
  expect(p["final_seq"] == 5, "final_seq is the final source's last seq");
  expect(p["sample_count"] == 320, "sample_count covers the final run only");

  plugin.stop();
  if (failures > 0) {
    std::cout << failures << " failure(s)" << std::endl;
    return EXIT_FAILURE;
  }
  std::cout << "all passed" << std::endl;
  return EXIT_SUCCESS;
}
