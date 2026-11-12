// Offline check of the simplestream JSON wire for source attribution.
// Drives the plugin hooks directly with a real Call_impl/System_impl and reads
// the emitted packets from a loopback UDP socket. Decoder behaviour (identity
// per codec frame, its reset at key-up boundaries) is simulated by the codec
// frames the test feeds in; the OP25 decoder itself is not exercised. Build
// with -DSIMPLESTREAM_TESTS=ON and run
// build/plugins/simplestream/simplestream_test; it exits non-zero if any
// expectation fails or an expected packet does not arrive within 2s.

#include "simplestream.cc"

#include "../../trunk-recorder/call_impl.h"
#include "../../trunk-recorder/systems/system.h"

#include <sys/socket.h>
#include <sys/time.h>

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
  Wire() : socket_(io_, ip::udp::endpoint(ip::address_v4::loopback(), 0)) {
    struct timeval timeout = {2, 0};
    setsockopt(socket_.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  }

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

int run() {
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

  // Key-up 2 by B while the control channel still says A. Frames before B's
  // link control carry no identity (simulating the decoder's reset at the
  // terminator), so they fall back.
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

  // Trailing hang-time silence after the final source belongs to its counters.
  p = audio(plugin, wire, &call, &rec, 160, true);
  expect(p["src"] == A && p["payload_length"] == 0 && p["seq"] == 6, "trailing silence keeps the final source");

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
  expect(p["final_seq"] == 6, "final_seq includes trailing empty packets");
  expect(p["sample_count"] == 320, "sample_count covers the final run only");

  // Encryption observed at an HDU (no unit) while the lane still names the
  // previous talker: it is held for the next key-up's codec frame instead of
  // marking A, and A's later clear audio keeps its PCM.
  Test_Call call2(730, 852000000, sys, cfg);
  call2.add_source(A);
  plugin.call_start(&call2);
  wire.next();
  codec(plugin, &call2, A);
  p = audio(plugin, wire, &call2, &rec, 160);
  expect(p["src"] == A && p["payload_length"] == 320, "clear audio from A");

  Encryption_Info hdu;
  hdu.encrypted = true;
  hdu.algid = 0x84;
  hdu.keyid = 0x1234;
  hdu.source = -1;
  plugin.encryption_info(&call2, hdu);
  p = audio(plugin, wire, &call2, &rec, 160); // A's tail, decoded before B's first frame
  expect(p["event"] == "audio" && p["src"] == A && p["payload_length"] == 320 && !p.contains("encrypted"),
         "source-less encryption is not attributed to the previous talker");

  codec(plugin, &call2, B);
  p = audio(plugin, wire, &call2, &rec, 160, true);
  expect(p["event"] == "enc_info" && p["src"] == B && p["algid"] == 0x84, "held encryption goes to the next key-up's talker");
  p = wire.next();
  expect(p["event"] == "audio" && p["src"] == B && p["encrypted"] == true && p["payload_length"] == 0,
         "B's audio carries the encryption");

  codec(plugin, &call2, -1);
  codec(plugin, &call2, A);
  p = audio(plugin, wire, &call2, &rec, 160);
  expect(p["src"] == A && p["payload_length"] == 320 && !p.contains("encrypted"), "A's next clear key-up keeps its PCM");

  Encryption_Info ldu2 = hdu;
  ldu2.source = B;
  plugin.encryption_info(&call2, ldu2);
  p = audio(plugin, wire, &call2, &rec, 160);
  expect(p["event"] == "audio", "a repeated observation for the same unit is not re-sent");

  Call_Data_t info2{};
  info2.call_num = call2.get_call_num();
  info2.talkgroup = call2.get_talkgroup();
  info2.freq = call2.get_freq();
  info2.short_name = "t";
  info2.capture_start_time_ms = call2.get_capture_start_time_ms();
  info2.tdma_slot = -1;
  plugin.capture_end(&call2, info2);
  p = wire.next();
  expect(p["event"] == "capture_end", "second capture ends");

  plugin.stop();
  if (failures > 0) {
    std::cout << failures << " failure(s)" << std::endl;
    return EXIT_FAILURE;
  }
  std::cout << "all passed" << std::endl;
  return EXIT_SUCCESS;
}

int main() {
  try {
    return run();
  } catch (const std::exception &e) {
    std::cout << "FAIL " << e.what() << std::endl;
    return EXIT_FAILURE;
  }
}
