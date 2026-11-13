# Fork patch list

This fork of Trunk Recorder feeds [trunk'd](https://github.com/dnesting/trunkd),
which ingests P25 audio from the `simplestream` plugin's JSON-over-UDP stream.
It tracks upstream `master` (remote `upstream`,
`https://github.com/robotastic/trunk-recorder`); the fork commits sit on top of
`git merge-base HEAD upstream/master`. Everything below differs from upstream.

Fork branches are stacked, each building on the previous one:
`feature/simplestream-transmission-v2` → `fix/capture-end-ordering` →
`simplestream-ota-labels` → `simplestream-voice-src`, which contains all of
them. Fork branches live on the `origin` (`git.lab.lan/david/trunk-recorder`)
and `github` (`dnesting/trunk-recorder`) remotes.
Images are tagged `registry.lab.lan/trunk-recorder:simplestream-v<N>-<commit>`;
the deployed tag is set in the tpl `kube` repo at `david.tpl/trunk/recorder.yaml`.

## simplestream plugin (wire format)

The consumer-facing contract is documented in the simplestream section of
[docs/CONFIGURE.md](docs/CONFIGURE.md). Changes from upstream's JSON mode:

| Behaviour | Why | Commits |
|---|---|---|
| `receiverId` config key (default `shortName`); every JSON event carries `receiver_id`, `capture_id`, `timestamp_ms`, `payload_length`, `phase2_tdma`, and `tdma_slot` for Phase 2. | Consumers join packets from several receivers into one capture per recorder assignment. | `9bdd7832` |
| `capture_id` is `receiver/short_name/start_ms/call_num/phase/slot`, using the call's immutable creation time, so it is stable across source changes and the start of playable audio. | A capture must keep one identity for its whole life. | `9bdd7832`, `0aa1a202`, `afa1539a` |
| `audio` carries `seq`, `start_sample` (per `src` within a capture, continuing when a unit talks again) and `voice_fingerprints` (hashes of corrected codec frames). | Loss detection, ordering, and cross-receiver deduplication. | `9bdd7832`, `1e1fd98d` |
| `enc_info` event with `algid`, `keyid` and 72-bit hex `mi` on a positive traffic-channel encryption observation; confirmed fields repeat on later events; PCM for a `src` with confirmed encryption is not streamed. | Encryption is decided from the voice channel, not the control-channel claim on `call_start`. | `9bdd7832` |
| `sendCallEnd` emits `capture_end` (with `final_seq`, `sample_count`, `synthesized: false`) instead of `call_end`, after the recorder stops and PCM callbacks have drained. | `call_end` runs later in the artifact worker and is skipped for filtered calls. | `b6000f08` |
| `capture_end` reports the `src` of the last PCM-bearing packet, that source's last `seq`, and the samples of its final uninterrupted run. | Lets a consumer check the transmission it still has in flight for tail loss. | `1e1fd98d` |
| `audio` and `enc_info` `src` is the unit the traffic-channel decoder read for the current key-up when it is a valid unit id (1..0xFFFFFB), falling back to the control-channel source otherwise. `call_start` keeps the control-channel source. | The control-channel source lags the talker and can revert on a repeated grant. | `1e1fd98d` |
| Codec fingerprints and encryption state are keyed by the same `src` as audio packets. | Otherwise they landed in state the audio path never read. | `288d57fc`, `1e1fd98d` |
| An encryption observation without a unit (the Phase 1 HDU) is held per lane and attributed to the unit of the next decoded voice frame; the resulting `enc_info` is sent from the next audio, encryption, or `capture_end` hook. | It could otherwise be filed under the previous talker and suppress that talker's clear audio. | `475b090f` |
| P25 `audio` adds `ota_talkgroup`, `ota_src`, `ota_age_ms`, `ota_frames`, `ota_errs`: the decoder's own identity and FEC counts since the previous packet. | Diagnostics for audio that belongs to a different unit or group than the labels. | `35c54b3a` |

## Core recorder and plugin API

| Behaviour | Why | Commits |
|---|---|---|
| `Plugin_Api::encryption_info(Call*, const Encryption_Info&)`; `Call` stores the confirmed `Encryption_Info` and `Call_Data_t` carries it (`encryption_info_valid`, `enc_algid`, `enc_keyid`, `enc_mi`). The P25 decoder parses `encryption_info` messages and drops ones from an earlier assignment (generation mismatch) or that disagree with the call's phase, slot, talkgroup or source. | Exposes voice-channel encryption to plugins, bound to one recorder assignment. | `9bdd7832` |
| `p25_recorder_decode` bumps an assignment generation on each `start()` and clears decoder state, output queue and message queue on `start()`/`stop()`/`reset()`. | Metadata and PCM from the previous assignment must not leak into the next call. | `9bdd7832` |
| `Call::get_capture_start_time_ms()` and `Call_Data_t::capture_start_time_ms`. | Immutable `capture_id` input. | `9bdd7832`, `afa1539a` |
| `Plugin_Api::capture_end(Call*, Call_Data_t)`, called once from `Call_Concluder::conclude_call` before artifact processing; not retried. A mutex in `p25_recorder_decode` serializes call assignment with PCM callbacks, so no `audio_stream` callback for the old call runs after `stop()`. | Real-time end-of-capture boundary ordered after the last audio. | `b6000f08` |
| `Plugin_Api::voice_codec_frame(Call*, const Voice_Codec_Frame&)` carries the decoder identity (`ota_talkgroup`, `ota_source`, `ota_id_time_ms`) with each codec frame; its default forwards to `voice_codec_data`. | Per-frame decoder identity for plugins without breaking existing ones. | `35c54b3a` |
| `plugin_wrapper_impl` gates streamed audio with `Talkgroup_Gate` (`trunk-recorder/gr_blocks/talkgroup_gate.*`) using the decoder's `grp_id`/`terminate` tags: audio before a mismatched `grp_id` is dropped, along with everything up to the next terminate or matching `grp_id`. The P25 recorder enables it for trunked calls (SmartNet talkgroups shifted `>> 4`); conventional calls and other recorders pass everything. | At call start the decoder can still emit the previous talkgroup's audio; the WAV sink already drops it, the plugin sink did not. | `4265a013` |

## Vendored OP25 (`lib/op25_repeater`)

Upstream treats `lib/` as vendored and unmodified. These are deliberate local
patches to carry on rebase or to offer upstream.

| Behaviour | Why | Commits |
|---|---|---|
| Phase 1 and Phase 2 decoders emit an `encryption_info` JSON message (`algid`, `keyid`, hex `mi`, phase, slot, `srcaddr`, `grpaddr`, `generation`) on positive ESS, replacing the old `{"encrypted": ...}` message. `reset_ess()` also clears `keyid` (and Phase 2's pending `next_*`). | Feeds the core `encryption_info` path with assignment-scoped data. | `9bdd7832` |
| `p25_frame_assembler::set_call_generation()`; `clear()` resets both decoders' identity/ESS state, the vocoder and the output queue under a mutex shared with `general_work`. | Decoders are reused across calls. | `9bdd7832` |
| The voice codec callback (`voice_codec_cb_t`, P25 and `rx_sync`) adds `ota_grp_id`, `ota_src_id`, `ota_id_time_ms` from the per-assignment identity cache. | Source of `Voice_Codec_Frame` identity. | `35c54b3a` |
| Phase 1 clears the cached source, group and timestamp after TDU and TDULC (after the TDULC's own link control), and clears the source at HDU. | Otherwise a new key-up carries the previous talker until its first LDU1 link control. Phase 2 already clears at MAC_END_PTT/MAC_HANGTIME. | `87d4e7bc` |
| The terminate flag is latched until the frame assembler consumes it, in both phases, and reset when an assignment is cleared. | A terminator followed by another frame in the same `general_work` batch was lost, merging two key-ups. | `ec14a042` |

## Build and Docker

| Behaviour | Why | Commits |
|---|---|---|
| The Dockerfile pins `tr-plugin-mqtt` to a commit (`--build-arg TR_PLUGIN_MQTT_REF=<sha>` overrides). | Its `main` fails to configure against Ubuntu 24.04's Paho packages. | `831cfd4f` |
| CMake options `SIMPLESTREAM_TESTS` and `TALKGROUP_GATE_TESTS` build offline test executables (off by default). | See below. | `b95da091`, `5788e315`, `4265a013` |

## Testing

Two offline test programs, not built by default. Each exits non-zero on a
failed expectation.

```bash
mkdir -p build && cd build
cmake -DSIMPLESTREAM_TESTS=ON -DTALKGROUP_GATE_TESTS=ON ../
make -j$(nproc) simplestream_test talkgroup_gate_test
./plugins/simplestream/simplestream_test
./talkgroup_gate_test
```

- `simplestream_test` (`plugins/simplestream/simplestream_test.cc`) drives the
  plugin hooks with a real `Call_impl`/`System_impl` and reads the JSON packets
  from a loopback UDP socket: voice-channel `src` with control-channel
  fallback, per-`src` `seq`/`start_sample`, fingerprint and encryption
  attribution, and `capture_end` counters. Decoder behaviour is simulated by
  the codec frames it feeds; OP25 itself is not exercised.
- `talkgroup_gate_test` (`trunk-recorder/gr_blocks/talkgroup_gate_test.cc`)
  checks `Talkgroup_Gate` decisions for tag sequences.

## Wire-contract consumers

trunk'd's ingest component consumes this stream; its spec,
`docs/specs/ingest.md` §Inputs / Outputs in
[dnesting/trunkd](https://github.com/dnesting/trunkd), defines how each field is
interpreted. Wire changes here are coordinated with that spec.
