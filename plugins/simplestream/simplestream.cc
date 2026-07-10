#include "../../trunk-recorder/plugin_manager/plugin_api.h"
#include "../../trunk-recorder/recorders/recorder.h"

#include <boost/asio.hpp>
#include <boost/dll/alias.hpp>
#include <boost/foreach.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <tuple>
#include <vector>

using namespace boost::asio;

struct stream_t {
  long TGID = 0;
  std::string address;
  std::string short_name;
  std::string receiver_id;
  long port = 0;
  ip::udp::endpoint remote_endpoint;
  ip::tcp::socket *tcp_socket = NULL;
  bool sendTGID = false;
  bool sendJSON = false;
  bool sendCallStart = false;
  bool sendCallEnd = false;
  bool tcp = false;
};

using capture_key = std::tuple<long, long, bool, int>; // call, unit, phase2, slot

struct capture_state {
  uint32_t seq = 0;
  uint64_t samples = 0;
  bool have_frame_hash = false;
  uint64_t last_frame_hash = 0;
  std::deque<std::string> fingerprints;
  bool encryption_valid = false;
  Encryption_Info encryption;
};

class Simple_Stream : public Plugin_Api {
  boost::asio::io_context my_io_context;
  boost::asio::io_context my_tcp_io_context;
  ip::udp::socket my_socket{my_io_context};
  std::vector<stream_t> streams;
  std::mutex state_mutex;
  std::mutex send_mutex;
  std::map<capture_key, capture_state> capture_states;

  static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  static long current_source(Call *call) {
    long source = call->get_current_source_id();
    if (source <= 0) {
      std::vector<Transmission> transmissions = call->get_transmissions();
      if (!transmissions.empty()) {
        source = transmissions.back().source;
      }
    }
    return source;
  }

  static capture_key key_for(Call *call, long source) {
    return std::make_tuple(call->get_call_num(), source,
                           call->get_phase2_tdma(),
                           call->get_phase2_tdma() ? call->get_tdma_slot() : -1);
  }

  static std::string capture_id(const std::string &receiver_id,
                                const std::string &short_name,
                                long call_num, int64_t start_ms,
                                bool phase2, int slot) {
    std::ostringstream out;
    out << receiver_id << '/' << short_name << '/' << start_ms << '/'
        << call_num << '/' << (phase2 ? 2 : 1) << '/'
        << (phase2 ? slot : -1);
    return out.str();
  }

  static std::vector<long> patched_talkgroups(Call *call) {
    std::vector<unsigned long> unsigned_groups =
        call->get_system()->get_talkgroup_patch(call->get_talkgroup());
    std::vector<long> groups;
    for (unsigned long group : unsigned_groups) {
      groups.push_back(static_cast<long>(group));
    }
    if (std::find(groups.begin(), groups.end(), call->get_talkgroup()) == groups.end()) {
      groups.insert(groups.begin(), call->get_talkgroup());
    }
    return groups;
  }

  static bool stream_matches(const stream_t &stream, long talkgroup,
                             const std::vector<long> &patched) {
    return stream.TGID == 0 || stream.TGID == talkgroup ||
           std::find(patched.begin(), patched.end(), stream.TGID) != patched.end();
  }

  static void add_lane(json &object, bool phase2, int slot) {
    object["phase2_tdma"] = phase2;
    if (phase2) {
      object["tdma_slot"] = slot;
    }
  }

  static std::string mi_hex(const std::array<uint8_t, 9> &mi) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (uint8_t byte : mi) {
      out << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return out.str();
  }

  static void add_encryption(json &object, const Encryption_Info &info) {
    object["encrypted"] = true;
    object["algid"] = info.algid;
    object["keyid"] = info.keyid;
    object["mi"] = mi_hex(info.mi);
  }

  static uint64_t fnv_mix(uint64_t hash, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      hash ^= static_cast<uint8_t>(value >> shift);
      hash *= UINT64_C(1099511628211);
    }
    return hash;
  }

  static std::string hash_hex(uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
  }

  void add_common(json &object, const stream_t &stream, Call *call,
                  long source, int64_t timestamp_ms, int payload_length) {
    object["receiver_id"] = stream.receiver_id;
    object["capture_id"] = capture_id(
        stream.receiver_id, call->get_short_name(), call->get_call_num(),
        call->get_capture_start_time_ms(), call->get_phase2_tdma(),
        call->get_tdma_slot());
    object["short_name"] = call->get_short_name();
    object["talkgroup"] = call->get_talkgroup();
    object["freq"] = call->get_freq();
    object["src"] = source;
    object["timestamp_ms"] = timestamp_ms;
    object["payload_length"] = payload_length;
    object["emergency"] = call->get_emergency();
    add_lane(object, call->get_phase2_tdma(), call->get_tdma_slot());
  }

  void add_common(json &object, const stream_t &stream,
                  const Call_Data_t &call_info, long source,
                  int64_t timestamp_ms) {
    object["receiver_id"] = stream.receiver_id;
    object["capture_id"] = capture_id(
        stream.receiver_id, call_info.short_name, call_info.call_num,
        call_info.capture_start_time_ms, call_info.phase2_tdma,
        call_info.tdma_slot);
    object["short_name"] = call_info.short_name;
    object["talkgroup"] = call_info.talkgroup;
    object["freq"] = call_info.freq;
    object["src"] = source;
    object["timestamp_ms"] = timestamp_ms;
    object["payload_length"] = 0;
    object["emergency"] = call_info.emergency;
    add_lane(object, call_info.phase2_tdma, call_info.tdma_slot);
  }

  void send_packet(stream_t &stream, const json &metadata,
                   const int16_t *samples = NULL, int sample_count = 0) {
    std::string json_string = metadata.dump();
    uint32_t json_length = static_cast<uint32_t>(json_string.size());
    std::vector<boost::asio::const_buffer> buffers;
    buffers.push_back(buffer(&json_length, sizeof(json_length)));
    buffers.push_back(buffer(json_string));
    if (samples != NULL && sample_count > 0) {
      buffers.push_back(buffer(samples, sample_count * sizeof(int16_t)));
    }

    boost::system::error_code error;
    std::lock_guard<std::mutex> lock(send_mutex);
    if (stream.tcp) {
      try {
        boost::asio::write(*stream.tcp_socket, buffers);
      } catch (const boost::system::system_error &e) {
        BOOST_LOG_TRIVIAL(error) << "SimpleStream TCP send failed: " << e.what();
      }
      return;
    }
    my_socket.send_to(buffers, stream.remote_endpoint, 0, error);
    if (error) {
      BOOST_LOG_TRIVIAL(warning) << "SimpleStream UDP send failed: " << error.message();
    }
  }

  void send_raw(stream_t &stream, long talkgroup, const int16_t *samples,
                int sample_count) {
    if (samples == NULL || sample_count <= 0) {
      return;
    }
    int32_t tgid = static_cast<int32_t>(talkgroup);
    std::vector<boost::asio::const_buffer> buffers;
    if (stream.sendTGID) {
      buffers.push_back(buffer(&tgid, sizeof(tgid)));
    }
    buffers.push_back(buffer(samples, sample_count * sizeof(int16_t)));
    boost::system::error_code error;
    std::lock_guard<std::mutex> lock(send_mutex);
    if (stream.tcp) {
      try {
        boost::asio::write(*stream.tcp_socket, buffers);
      } catch (const boost::system::system_error &e) {
        BOOST_LOG_TRIVIAL(error) << "SimpleStream TCP send failed: " << e.what();
      }
      return;
    }
    my_socket.send_to(buffers, stream.remote_endpoint, 0, error);
    if (error) {
      BOOST_LOG_TRIVIAL(warning) << "SimpleStream UDP send failed: " << error.message();
    }
  }

public:
  int parse_config(json config_data) override {
    for (json element : config_data["streams"]) {
      stream_t stream;
      stream.TGID = element["TGID"];
      if (element.contains("url")) {
        std::string url = element["url"];
        if (url.substr(0, 6) == "udp://") {
          stream.tcp = false;
          url = url.substr(6);
        } else if (url.substr(0, 6) == "tcp://") {
          stream.tcp = true;
          url = url.substr(6);
        } else {
          BOOST_LOG_TRIVIAL(error) << "SimpleStream: invalid URL scheme";
          continue;
        }
        size_t colon = url.rfind(':');
        if (colon == std::string::npos) {
          BOOST_LOG_TRIVIAL(error) << "SimpleStream: missing URL port";
          continue;
        }
        stream.address = url.substr(0, colon);
        stream.port = static_cast<long>(std::stoul(url.substr(colon + 1)));
      } else {
        stream.address = element["address"];
        stream.port = element["port"];
        stream.tcp = element.value("useTCP", false);
      }

      stream.sendTGID = element.value("sendTGID", false);
      stream.sendJSON = element.value("sendJSON", false);
      stream.sendCallStart = element.value("sendCallStart", false);
      stream.sendCallEnd = element.value("sendCallEnd", false);
      stream.short_name = element.value("shortName", "");
      stream.receiver_id = element.value("receiverId", stream.short_name);
      if (stream.receiver_id.empty()) {
        BOOST_LOG_TRIVIAL(error) << "SimpleStream: receiverId or shortName is required";
        continue;
      }

      if (!stream.tcp) {
        ip::udp::resolver resolver(my_io_context);
        boost::system::error_code error;
        ip::udp::resolver::results_type results =
            resolver.resolve(stream.address, std::to_string(stream.port), error);
        if (error || results.empty()) {
          BOOST_LOG_TRIVIAL(error) << "SimpleStream: failed to resolve UDP address "
                                   << stream.address << ": " << error.message();
          continue;
        }
        stream.remote_endpoint = results.begin()->endpoint();
      }
      streams.push_back(stream);
    }
    return 0;
  }

  int audio_stream(Call *call, Recorder *recorder, int16_t *samples,
                   int sampleCount) override {
    const long source = current_source(call);
    const std::vector<long> patched = patched_talkgroups(call);
    bool all_zero = sampleCount > 0;
    for (int i = 0; i < sampleCount && all_zero; ++i) {
      all_zero = samples[i] == 0;
    }

    uint32_t seq;
    uint64_t start_sample;
    std::vector<std::string> fingerprints;
    bool encryption_valid;
    Encryption_Info encryption;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      capture_state &state = capture_states[key_for(call, source)];
      seq = state.seq++;
      start_sample = state.samples;
      fingerprints.assign(state.fingerprints.begin(), state.fingerprints.end());
      encryption_valid = state.encryption_valid;
      encryption = state.encryption;
      if (!all_zero && !encryption_valid) {
        state.samples += sampleCount;
      }
    }

    const int payload_samples = (!all_zero && !encryption_valid) ? sampleCount : 0;
    for (stream_t &stream : streams) {
      if (!(stream.short_name.empty() || stream.short_name == call->get_short_name()) ||
          !stream_matches(stream, call->get_talkgroup(), patched)) {
        continue;
      }
      if (!stream.sendJSON) {
        send_raw(stream, call->get_talkgroup(), samples, payload_samples);
        continue;
      }
      json object;
      add_common(object, stream, call, source, now_ms(),
                 payload_samples * static_cast<int>(sizeof(int16_t)));
      object["event"] = "audio";
      object["patched_talkgroups"] = patched;
      object["src_tag"] = call->get_system()->find_unit_tag(source);
      object["audio_sample_rate"] = recorder->get_wav_hz();
      object["seq"] = seq;
      object["start_sample"] = start_sample;
      object["voice_fingerprints"] = fingerprints;
      if (encryption_valid) {
        add_encryption(object, encryption);
      }
      send_packet(stream, object, payload_samples > 0 ? samples : NULL,
                  payload_samples);
    }
    return 0;
  }

  int call_start(Call *call) override {
    const long source = current_source(call);
    const std::vector<long> patched = patched_talkgroups(call);
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      capture_states.emplace(key_for(call, source), capture_state{});
    }
    std::vector<std::string> patched_tags;
    for (long group : patched) {
      Talkgroup *tg = call->get_system()->find_talkgroup(static_cast<unsigned long>(group));
      patched_tags.push_back(tg == NULL ? "" : tg->alpha_tag);
    }

    for (stream_t &stream : streams) {
      if (!stream.sendJSON || !stream.sendCallStart ||
          !(stream.short_name.empty() || stream.short_name == call->get_short_name()) ||
          !stream_matches(stream, call->get_talkgroup(), patched)) {
        continue;
      }
      json object;
      add_common(object, stream, call, source, call->get_start_time_ms(), 0);
      object["event"] = "call_start";
      object["src_tag"] = call->get_system()->find_unit_tag(source);
      object["talkgroup_tag"] = call->get_talkgroup_tag();
      object["patched_talkgroups"] = patched;
      object["patched_talkgroup_tags"] = patched_tags;
      object["encrypted"] = call->get_encrypted();
      send_packet(stream, object);
    }
    return 0;
  }

  int encryption_info(Call *call, const Encryption_Info &info) override {
    const long source = info.source > 0 ? info.source : current_source(call);
    const std::vector<long> patched = patched_talkgroups(call);
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      capture_state &state = capture_states[key_for(call, source)];
      changed = !state.encryption_valid || state.encryption.algid != info.algid ||
                state.encryption.keyid != info.keyid || state.encryption.mi != info.mi;
      state.encryption_valid = true;
      state.encryption = info;
    }
    if (!changed) {
      return 0;
    }

    for (stream_t &stream : streams) {
      if (!stream.sendJSON ||
          !(stream.short_name.empty() || stream.short_name == call->get_short_name()) ||
          !stream_matches(stream, call->get_talkgroup(), patched)) {
        continue;
      }
      json object;
      add_common(object, stream, call, source, now_ms(), 0);
      object["event"] = "enc_info";
      object["patched_talkgroups"] = patched;
      add_encryption(object, info);
      send_packet(stream, object);
    }
    return 0;
  }

  int voice_codec_data(Call *call, int codec_type, long tgid, uint32_t src_id,
                       const uint32_t *params, int param_count, int errs) override {
    (void)errs;
    if (codec_type != CODEC_P25_IMBE && codec_type != CODEC_P25_AMBE) {
      return 0;
    }
    const long source = src_id > 0 ? static_cast<long>(src_id) : current_source(call);
    uint64_t frame_hash = UINT64_C(1469598103934665603);
    frame_hash = fnv_mix(frame_hash, static_cast<uint64_t>(codec_type));
    frame_hash = fnv_mix(frame_hash, static_cast<uint64_t>(tgid));
    for (int i = 0; i < param_count; ++i) {
      frame_hash = fnv_mix(frame_hash, params[i]);
    }

    std::lock_guard<std::mutex> lock(state_mutex);
    capture_state &state = capture_states[key_for(call, source)];
    if (state.have_frame_hash) {
      uint64_t window_hash = fnv_mix(fnv_mix(UINT64_C(1469598103934665603),
                                             state.last_frame_hash),
                                     frame_hash);
      state.fingerprints.push_back(hash_hex(window_hash));
      while (state.fingerprints.size() > 8) {
        state.fingerprints.pop_front();
      }
    }
    state.last_frame_hash = frame_hash;
    state.have_frame_hash = true;
    return 0;
  }

  int capture_end(Call *call, Call_Data_t call_info) override {
    const long source = current_source(call);
    std::vector<long> patched;
    for (unsigned long group : call_info.patched_talkgroups) {
      patched.push_back(static_cast<long>(group));
    }
    if (std::find(patched.begin(), patched.end(), call_info.talkgroup) == patched.end()) {
      patched.insert(patched.begin(), call_info.talkgroup);
    }
    uint32_t final_seq = 0;
    uint64_t sample_count = 0;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      for (const auto &item : capture_states) {
        if (std::get<0>(item.first) != call_info.call_num ||
            (source > 0 && std::get<1>(item.first) != source)) {
          continue;
        }
        if (item.second.seq > 0) {
          final_seq = std::max(final_seq, item.second.seq - 1);
        }
        sample_count = std::max(sample_count, item.second.samples);
      }
    }
    const int64_t timestamp = call_info.stop_time_ms > 0 ? call_info.stop_time_ms : now_ms();
    for (stream_t &stream : streams) {
      if (!stream.sendJSON || !stream.sendCallEnd ||
          !(stream.short_name.empty() || stream.short_name == call_info.short_name) ||
          !stream_matches(stream, call_info.talkgroup, patched)) {
        continue;
      }
      json object;
      add_common(object, stream, call_info, source, timestamp);
      object["event"] = "capture_end";
      object["patched_talkgroups"] = patched;
      object["encrypted"] = call_info.encrypted;
      object["synthesized"] = false;
      object["final_seq"] = final_seq;
      object["sample_count"] = sample_count;
      if (call_info.encryption_info_valid) {
        Encryption_Info info;
        info.encrypted = true;
        info.algid = call_info.enc_algid;
        info.keyid = call_info.enc_keyid;
        info.mi = call_info.enc_mi;
        add_encryption(object, info);
      }
      send_packet(stream, object);
    }

    std::lock_guard<std::mutex> lock(state_mutex);
    for (std::map<capture_key, capture_state>::iterator it = capture_states.begin();
         it != capture_states.end();) {
      if (std::get<0>(it->first) == call_info.call_num) {
        it = capture_states.erase(it);
      } else {
        ++it;
      }
    }
    return 0;
  }

  // call_end is the artifact-processing completion hook and may arrive many
  // seconds later or not at all on filtered calls. capture_end above owns the
  // streaming lifecycle, so emitting here would create a delayed duplicate.
  int call_end(Call_Data_t call_info) override {
    (void)call_info;
    return 0;
  }

  int start() override {
    for (stream_t &stream : streams) {
      if (!stream.tcp) {
        continue;
      }
      stream.tcp_socket = new ip::tcp::socket{my_tcp_io_context};
      ip::tcp::resolver resolver(my_tcp_io_context);
      boost::system::error_code error;
      ip::tcp::resolver::results_type results =
          resolver.resolve(stream.address, std::to_string(stream.port), error);
      if (!error && !results.empty()) {
        stream.tcp_socket->connect(results.begin()->endpoint(), error);
      }
      if (error) {
        BOOST_LOG_TRIVIAL(error) << "SimpleStream TCP connect failed: " << error.message();
      }
    }
    my_socket.open(ip::udp::v4());
    return 0;
  }

  int stop() override {
    for (stream_t &stream : streams) {
      if (stream.tcp && stream.tcp_socket != NULL) {
        boost::system::error_code error;
        stream.tcp_socket->shutdown(ip::tcp::socket::shutdown_both, error);
        stream.tcp_socket->close(error);
        delete stream.tcp_socket;
        stream.tcp_socket = NULL;
      }
    }
    boost::system::error_code error;
    my_socket.close(error);
    return 0;
  }

  static boost::shared_ptr<Simple_Stream> create() {
    return boost::shared_ptr<Simple_Stream>(new Simple_Stream());
  }
};

BOOST_DLL_ALIAS(Simple_Stream::create, create_plugin)
