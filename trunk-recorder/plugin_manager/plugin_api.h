#ifndef PLUGIN_API_H
#define PLUGIN_API_H

#include "../call.h"
#include "../global_structs.h"
#include "../gr_blocks/decoder_wrapper.h"
#include "../systems/system.h"
#include "../systems/parser.h"
#include "../formatter.h"

#include <json.hpp>

typedef enum {
  PLUGIN_UNKNOWN,
  PLUGIN_INITIALIZED,
  PLUGIN_RUNNING,
  PLUGIN_FAILED,
  PLUGIN_STOPPED,
  PLUGIN_DISABLED
} plugin_state_t;

enum VoiceCodecType {
  CODEC_P25_IMBE = 0,     // P25 Phase 1: 8 IMBE codewords, uint32_t u[8]
  CODEC_P25_AMBE = 1,     // P25 Phase 2: 4 AMBE+2 codewords, uint32_t u[4]
  CODEC_DMR_AMBE = 2,     // DMR: 4 AMBE codewords, int U[4] cast to uint32_t
  CODEC_DSTAR_AMBE = 3,   // D-STAR: 9 AMBE2400 params, int b[9] cast to uint32_t
  CODEC_YSF_FULLRATE = 4, // YSF full rate: 8 IMBE codewords, uint32_t u[8]
  CODEC_YSF_HALFRATE = 5, // YSF half rate: 9 AMBE2250 params, int b[9] cast to uint32_t
};

using json = nlohmann::json;

// One decoded voice codec frame, delivered from the decoder thread.
struct Voice_Codec_Frame {
  int codec_type = 0;
  // Legacy decoder-provided values, passed unchanged to voice_codec_data.
  // For P25 Phase 1, tgid is the last HDU ESS group and is not reset between
  // assignments; for Phase 2 it is a volatile value that is usually -1.
  long tgid = 0;
  uint32_t src_id = 0;
  const uint32_t *params = NULL;
  int param_count = 0;
  int errs = 0; // FEC bit errors corrected in this codec frame
  // P25 decoder identity cache for the current recorder assignment, from
  // Phase 1 link control (Group Voice Channel User; HDU also sets the group)
  // or Phase 2 MAC (MAC_PTT, Group Voice Channel User). -1 when unknown.
  // Same numbering as Call::get_talkgroup() and unit ids for P25 systems.
  long ota_talkgroup = -1;
  long ota_source = -1;
  // Wall-clock ms since the epoch when link control/MAC last set the cache,
  // or 0 when unknown (a Phase 1 HDU sets the group without a timestamp).
  uint64_t ota_id_time_ms = 0;
};

class Plugin_Api {
public:
  virtual int init(Config *config, std::vector<Source *> sources, std::vector<System *> systems) { frequency_format = config->frequency_format; return 0; };
  virtual int parse_config(json config_data) { return 0; }; // const { BOOST_LOG_TRIVIAL(info) << "plugin_api created!";return 0; };
  virtual int start() { return 0; };
  virtual int stop() { return 0; };
  virtual int poll_one() { return 0; };
  virtual int signal(long unitId, const char *signaling_type, gr::blocks::SignalType sig_type, Call *call, System *system, Recorder *recorder) { return 0; };
  virtual int audio_stream(Call *call, Recorder *recorder, int16_t *samples, int sampleCount) { return 0; };
  virtual int trunk_message(std::vector<TrunkMessage> messages, System *system) { return 0; };
  virtual int call_start(Call *call) { return 0; };
  // Called exactly once after the recorder has stopped and every in-progress
  // audio_stream callback has returned, but before asynchronous call artifact
  // processing. This is the real-time streaming lifecycle boundary; call_end
  // remains the post-processing/upload plugin hook.
  virtual int capture_end(Call *call, Call_Data_t call_info) { return 0; };
  virtual int call_end(Call_Data_t call_info) { return 0; }; //= 0; //{ BOOST_LOG_TRIVIAL(info) << "plugin_api call_end"; return 0; };
  virtual int calls_active(std::vector<Call *> calls) { return 0; };
  virtual int setup_recorder(Recorder *recorder) { return 0; };
  virtual int setup_system(System *system) { return 0; };
  virtual int setup_systems(std::vector<System *> systems) { return 0; };
  virtual int setup_sources(std::vector<Source *> sources) { return 0; };
  virtual int setup_config(std::vector<Source *> sources, std::vector<System *> systems) { return 0; };
  virtual int system_rates(std::vector<System *> systems, float timeDiff) { return 0; };
  virtual int unit_registration(System *sys, long source_id) { return 0; };
  virtual int unit_deregistration(System *sys, long source_id) { return 0; };
  virtual int unit_acknowledge_response(System *sys, long source_id) { return 0; };
  virtual int unit_group_affiliation(System *sys, long source_id, long talkgroup_num) { return 0; };
  virtual int unit_data_grant(System *sys, long source_id) { return 0; };
  virtual int unit_answer_request(System *sys, long source_id, long talkgroup) { return 0; };
  virtual int unit_call_alert(System *sys, long source_id, long talkgroup) { return 0; };
  virtual int unit_location(System *sys, long source_id, long talkgroup_num) { return 0; };
  // Called for every voice codec frame before vocoder synthesis.
  // tgid/src_id are populated from voice channel headers for P25; for DMR, src_id comes from
  // rx_sync slot tracking; for D-STAR/YSF they are 0. Use call->get_talkgroup() and
  // call->get_current_source_id() for authoritative values across all codec types.
  virtual int voice_codec_data(Call *call, int codec_type, long tgid, uint32_t src_id, const uint32_t *params, int param_count, int errs) { return 0; };
  // Full per-frame view including the decoder identity cache. The default
  // forwards to voice_codec_data, so plugins override one or the other.
  virtual int voice_codec_frame(Call *call, const Voice_Codec_Frame &frame) {
    return voice_codec_data(call, frame.codec_type, frame.tgid, frame.src_id, frame.params, frame.param_count, frame.errs);
  };
  virtual int encryption_info(Call *call, const Encryption_Info &info) { return 0; };
  //void set_frequency_format(int f) { frequencyFormat = f; }
  virtual ~Plugin_Api(){};
};

#endif
