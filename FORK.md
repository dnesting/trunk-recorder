# Fork patch list

This fork of Trunk Recorder feeds [trunk'd](https://github.com/dnesting/trunkd),
which ingests P25 audio from the `simplestream` plugin's JSON-over-UDP stream.
Everything below differs from upstream
([robotastic/trunk-recorder](https://github.com/robotastic/trunk-recorder),
remote `upstream`, branch `master`).

## Branch, tags and images

- **`trunkd`** is the fork: `upstream/master` plus the patch series listed
  below, kept as a linear series on top of upstream and rebased (never
  merged) when upstream moves. It lives on `origin`
  (`git.lab.lan/david/trunk-recorder`) and `github`
  (`dnesting/trunk-recorder`); a rebase is pushed with
  `--force-with-lease`.
- Every released image has an annotated git tag of the same name on the
  commit it was built from, so a rebase never orphans deployed source.
- Image: `registry.lab.lan/trunk-recorder:trunkd-<YYYYMMDD>-<sha8>`
  (HEAD's commit date and abbreviated hash), linux/amd64, built by
  `fork/release.sh`. The deployed tag is set in the tpl `kube` repo at
  `david.tpl/trunk/recorder.yaml`.
- Retired: the stacked branches `feature/simplestream-transmission-v2`,
  `fix/capture-end-ordering`, `simplestream-ota-labels` and
  `simplestream-voice-src` (all contained in `trunkd`), and images tagged
  `simplestream-v<N>-<sha>`. `fork`, `master`, `unit-filename`, `encryption`,
  `pcap` and `data` on `origin` are older experiments not carried here.

## Updating from upstream

```bash
git fetch upstream origin
git switch trunkd
git rebase upstream/master          # resolve conflicts, keeping each patch's behaviour below
fork/test.sh                        # Docker build + offline tests (lab buildkit, linux/amd64)
git push --force-with-lease origin trunkd && git push --force-with-lease github trunkd
fork/release.sh                     # test, build and push the image, tag the commit
```

Conflict hotspots, in the order they usually bite:

- `lib/op25_repeater/lib/p25_frame_assembler_impl.cc` `clear()` and
  `general_work()`: upstream edits the per-call reset. Keep our mutex,
  `call_end()` on both decoders and output-queue reset, plus whatever upstream
  adds (for example the vocoder `clear()` calls).
- `lib/op25_repeater/lib/p25p1_fdma.cc`, `p25p2_tdma.cc`, `rx_sync.cc`: the
  ESS/`encryption_info` message, identity cache and voice codec callback.
- `trunk-recorder/recorders/p25_recorder_decode.cc`: assignment generation and
  the PCM-callback mutex.
- `trunk-recorder/plugin_manager/plugin_api.h` and `plugin_manager.*`: our
  added hooks.
- `plugins/simplestream/simplestream.cc`: almost entirely ours; take ours and
  port any upstream fix by hand. Check that upstream changes there do not
  alter the JSON wire (see below).
- `trunk-recorder/monitor_systems.cc`: the SIGTERM drain.

If upstream now does what one of our patches does, drop or shrink the patch
and record it under "Dropped" below. The fork's test bar is a clean Docker
build plus `fork/test.sh`; upstream has no test suite.

## simplestream plugin (wire format)

The consumer-facing contract is documented in the simplestream section of
[docs/CONFIGURE.md](docs/CONFIGURE.md). Files: `plugins/simplestream/`,
`docs/CONFIGURE.md`. Not upstreamable as is: the wire is trunk'd's contract,
though `capture_id` and `capture_end` would make sense upstream.

| Behaviour | Why | Commit |
|---|---|---|
| `receiverId` config key (default `shortName`); every JSON event carries `receiver_id`, `capture_id`, `timestamp_ms`, `payload_length`, `phase2_tdma`, and `tdma_slot` for Phase 2. | Consumers join packets from several receivers into one capture per recorder assignment. | simplestream: scope P25 metadata to recorder assignments |
| `capture_id` is `receiver/short_name/start_ms/call_num/phase/slot`, using the call's immutable creation time, so it is stable across source changes and the start of playable audio. | A capture must keep one identity for its whole life. | simplestream: scope P25 metadata to recorder assignments |
| `audio` carries `seq`, `start_sample` (per `src` within a capture, continuing when a unit talks again) and `voice_fingerprints` (hashes of corrected codec frames). | Loss detection, ordering, and cross-receiver deduplication. | simplestream: scope P25 metadata…; simplestream: take audio src from the voice channel |
| `enc_info` event with `algid`, `keyid` and 72-bit hex `mi` on a positive traffic-channel encryption observation; confirmed fields repeat on later events; PCM for a `src` with confirmed encryption is not streamed. | Encryption is decided from the voice channel, not the control-channel claim on `call_start`. | simplestream: scope P25 metadata… |
| `sendCallEnd` emits `capture_end` (with `final_seq`, `sample_count`, `synthesized: false`) instead of `call_end`, after the recorder stops and PCM callbacks have drained. | `call_end` runs later in the artifact worker and is skipped for filtered calls. | Emit capture end after streaming audio drains |
| `capture_end` reports the `src` of the last PCM-bearing packet, that source's last `seq`, and the samples of its final uninterrupted run. | Lets a consumer check the transmission it still has in flight for tail loss. | simplestream: take audio src from the voice channel |
| `audio` and `enc_info` `src` is the unit the traffic-channel decoder read for the current key-up when it is a valid unit id (1..0xFFFFFB), falling back to the control-channel source otherwise. `call_start` keeps the control-channel source. | The control-channel source lags the talker and can revert on a repeated grant. | simplestream: take audio src from the voice channel |
| Codec fingerprints and encryption state are keyed by the same `src` as audio packets. | Otherwise they landed in state the audio path never read. | simplestream: key codec and encryption state like audio packets |
| An encryption observation without a unit (the Phase 1 HDU) is held per lane and attributed to the unit of the next decoded voice frame; the resulting `enc_info` is sent from the next audio, encryption, or `capture_end` hook. | It could otherwise be filed under the previous talker and suppress that talker's clear audio. | simplestream: attribute source-less encryption by the next voice frame |
| P25 `audio` adds `ota_talkgroup`, `ota_src`, `ota_age_ms`, `ota_frames`, `ota_errs`: the decoder's own identity and FEC counts since the previous packet. | Diagnostics for audio that belongs to a different unit or group than the labels. | simplestream: report decoder (over-the-air) talkgroup and source on audio |

## Core recorder and plugin API

| Behaviour | Why | Upstreamable | Commit |
|---|---|---|---|
| `Plugin_Api::encryption_info(Call*, const Encryption_Info&)`; `Call` stores the confirmed `Encryption_Info` and `Call_Data_t` carries it (`encryption_info_valid`, `enc_algid`, `enc_keyid`, `enc_mi`). The P25 decoder parses `encryption_info` messages and drops ones from an earlier assignment (generation mismatch) or that disagree with the call's phase, slot, talkgroup or source. Files: `plugin_manager/*`, `call*.{h,cc}`, `global_structs.h`, `recorders/p25_recorder_decode.*`. | Exposes voice-channel encryption to plugins, bound to one recorder assignment. | Yes | simplestream: scope P25 metadata to recorder assignments |
| `p25_recorder_decode` bumps an assignment generation on each `start()` and clears decoder state, output queue and message queue on `start()`/`stop()`/`reset()`. | Metadata and PCM from the previous assignment must not leak into the next call. | Yes | simplestream: scope P25 metadata to recorder assignments |
| `Call::get_capture_start_time_ms()` and `Call_Data_t::capture_start_time_ms`. | Immutable `capture_id` input. | Yes | simplestream: scope P25 metadata to recorder assignments |
| `Plugin_Api::capture_end(Call*, Call_Data_t)`, called once from `Call_Concluder::conclude_call` before artifact processing; not retried. A mutex in `p25_recorder_decode` serializes call assignment with PCM callbacks, so no `audio_stream` callback for the old call runs after `stop()`. | Real-time end-of-capture boundary ordered after the last audio. | Yes | Emit capture end after streaming audio drains |
| `Plugin_Api::voice_codec_frame(Call*, const Voice_Codec_Frame&)` carries the decoder identity (`ota_talkgroup`, `ota_source`, `ota_id_time_ms`) with each codec frame; its default forwards to `voice_codec_data`. Files also `recorders/dmr_recorder_impl.*`. | Per-frame decoder identity for plugins without breaking existing ones. | Yes | simplestream: report decoder (over-the-air) talkgroup and source on audio |
| `plugin_wrapper_impl` gates streamed audio with `Talkgroup_Gate` (`gr_blocks/talkgroup_gate.*`) using the decoder's `grp_id`/`terminate` tags: audio before a mismatched `grp_id` is dropped, along with everything up to the next terminate or matching `grp_id`. The P25 recorder enables it for trunked calls (SmartNet talkgroups shifted `>> 4`); conventional calls and other recorders pass everything. | At call start the decoder can still emit the previous talkgroup's audio; the WAV sink already drops it, the plugin sink did not. | Yes | plugin_wrapper: gate streamed P25 audio on the call's talkgroup |
| SIGTERM drains instead of killing the process: new call grants are ignored, calls in progress keep recording (including grant updates and source changes) and conclude through the normal `callTimeout` path, so plugins get `capture_end`; with none left the process exits as on SIGINT. `drainTimeout` (seconds, default 60, `0` = no drain) bounds it, after which the remaining calls are concluded and the process exits. A second SIGTERM, or SIGINT, concludes all calls and exits at once. Conventional channels keep recording; one counts as in progress while it has audio. Files: `monitor_systems.cc`, `config.cc`, `global_structs.h`, `docs/CONFIGURE.md`. | Rolling restarts with two overlapping recorders: before this a SIGTERM truncated calls in progress and no `capture_end` was sent. | Yes | Drain calls in progress on SIGTERM before exiting |

## Vendored OP25 (`lib/op25_repeater`)

Upstream treats `lib/` as vendored, but edits it in place itself. These are
deliberate local patches to carry on rebase or to offer upstream.

| Behaviour | Why | Upstreamable | Commit |
|---|---|---|---|
| Phase 1 and Phase 2 decoders emit an `encryption_info` JSON message (`algid`, `keyid`, hex `mi`, phase, slot, `srcaddr`, `grpaddr`, `generation`) on positive ESS, replacing the old `{"encrypted": ...}` message. `reset_ess()` also clears `keyid` (and Phase 2's pending `next_*`). | Feeds the core `encryption_info` path with assignment-scoped data. | Yes, with the core hook | simplestream: scope P25 metadata to recorder assignments |
| `p25_frame_assembler::set_call_generation()`; `clear()` resets both decoders' identity/ESS state (`call_end()`), the vocoders (`clear()`, also done by upstream) and the output queue under a mutex shared with `general_work`. | Decoders are reused across calls. | Yes | simplestream: scope P25 metadata to recorder assignments |
| The voice codec callback (`voice_codec_cb_t`, P25 and `rx_sync`) adds `ota_grp_id`, `ota_src_id`, `ota_id_time_ms` from the per-assignment identity cache. | Source of `Voice_Codec_Frame` identity. | Yes, with the core hook | simplestream: report decoder (over-the-air) talkgroup and source on audio |
| Phase 1 clears the cached source, group and timestamp after TDU and TDULC (after the TDULC's own link control), and clears the source at HDU. | Otherwise a new key-up carries the previous talker until its first LDU1 link control. Phase 2 already clears at MAC_END_PTT/MAC_HANGTIME. | Yes | op25: clear the Phase 1 talker at the end of each key-up |
| The terminate flag is latched until the frame assembler consumes it, in both phases, and reset when an assignment is cleared. | A terminator followed by another frame in the same `general_work` batch was lost, merging two key-ups. | Yes | op25: keep P25 terminators that share a batch with later frames |

## Build and Docker

| Behaviour | Why | Commit |
|---|---|---|
| The Dockerfile pins `tr-plugin-mqtt` to a commit (`--build-arg TR_PLUGIN_MQTT_REF=<sha>` overrides). | Its `main` fails to configure against Ubuntu 24.04's Paho packages. | build: pin tr-plugin-mqtt in the Docker image |
| CMake options `SIMPLESTREAM_TESTS` and `TALKGROUP_GATE_TESTS` build offline test executables (off by default). | See Testing. | simplestream: offline wire test for source attribution; plugin_wrapper: gate streamed P25 audio… |
| Dockerfile `test` stage (built only with `--target test`) runs both offline tests; `fork/test.sh` and `fork/release.sh` drive the build. | One command checks a rebase; images map to tagged commits. | build: Docker test stage and fork release scripts |

## Testing

`fork/test.sh` builds the Docker `builder` stage and runs both offline tests
(it fails if either fails). To run them in a local build instead:

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

The SIGTERM drain has no automated test. It can be exercised without radio
hardware with a conventional analog system on an `osmosdr` source reading a
synthetic IQ file (`"device": "file=x.iq,freq=...,rate=2400000,repeat=true,throttle=true"`,
a carrier on the channel for a few seconds, then noise): send SIGTERM while
the squelch is open and watch the drain log lines and the simplestream
`capture_end`. The `iqfile` driver cannot be used for this (it has no signal
detector, so conventional setup crashes), and keep `$HOME` between runs so
GNU Radio's FFTW wisdom spares the flowgraph a multi-second stall at start.

## Dropped

Nothing yet. When upstream absorbs a patch, list it here with the upstream
commit that replaced it.

## Wire-contract consumers

trunk'd's ingest component consumes this stream; its spec,
`docs/specs/ingest.md` §Inputs / Outputs in
[dnesting/trunkd](https://github.com/dnesting/trunkd), defines how each field is
interpreted. Wire changes here are coordinated with that spec.
