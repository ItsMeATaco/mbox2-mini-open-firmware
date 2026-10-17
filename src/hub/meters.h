// Peak meters for the Mbox's own Windows endpoints, "Speakers (Mbox 2 Mini)" and "Microphone (Mbox 2 Mini)",
// through IAudioMeterInformation, and the audio the analysis views work on. UI thread only.
//
// Windows meters a capture endpoint only while something records from it, so the input meters need a
// shared-mode capture stream. The hub holds one only while the meters are on screen: an open capture
// stream counts as using the microphone (Windows' privacy indicator shows it, and Settings lists the hub
// under recent microphone use), and it keeps other programs from opening the inputs in exclusive mode.
//
// The analysis views get the output from a loopback capture of the speakers endpoint: what Windows' shared
// mode plays to the Mbox. A program that opens the Mbox in exclusive mode (or through the ASIO driver,
// src/asio) bypasses that mix and doesn't show there.
#pragma once

#include <windows.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The Mbox's active speakers (eRender) or microphone (eCapture) endpoint, or null.
Microsoft::WRL::ComPtr<IMMDevice> FindMboxEndpoint(IMMDeviceEnumerator* enumerator, EDataFlow flow);

// Process IDs of the Windows audio services. When it changes, the services restarted (for example when an
// audio driver is installed or removed), and audio objects from before keep answering with silence instead
// of failing, so they must be created again.
uint64_t AudioServiceInstance();

// A shared-mode capture stream (a microphone, or the loopback of a speakers endpoint), delivered as
// interleaved stereo float (mono is doubled, channels beyond two are dropped).
class CaptureStream {
 public:
  using Tap = std::function<void(const float* stereo, size_t frames, uint32_t rate)>;

  HRESULT Open(IMMDevice* device, bool loopback);
  void Close();
  bool IsOpen() const { return capture_ != nullptr; }
  uint32_t Rate() const { return rate_; }
  // Takes everything that arrived and hands it to `tap` (may be empty). Fails when the stream broke.
  HRESULT Drain(const Tap& tap, size_t* frames);

 private:
  Microsoft::WRL::ComPtr<IAudioClient> audio_;
  Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_;
  uint32_t rate_ = 0, channels_ = 0, block_align_ = 0, bytes_ = 0;
  bool float_ = false;
  std::vector<float> stereo_;
};

class EndpointMeters {
 public:
  using Tap = CaptureStream::Tap;

  ~EndpointMeters() { Unbind(); }

  // Finds the endpoints if they're missing; call about once a second.
  void Refresh();
  // Whether to hold the capture stream open for the input meters.
  void SetInputsWanted(bool wanted);
  // Whether to capture the output for the analysis views.
  void SetOutputWanted(bool wanted);
  // Current peak per channel, 0..1: In 1, In 2, Out 1, Out 2 (0 where unavailable). Drains the streams and
  // passes their audio to `output` and `inputs` (either may be empty). While nothing plays, the output
  // loopback delivers nothing; `output` then gets silence, so the analysis falls back instead of freezing.
  void Read(float peaks[4], const Tap& output = {}, const Tap& inputs = {});

  bool OutputsFound() const { return render_meter_ != nullptr; }
  bool InputsLive() const { return input_.IsOpen(); }
  bool OutputLive() const { return output_.IsOpen(); }
  // Why the input meters (or the output capture) are off while they're wanted, or empty.
  const std::wstring& InputsNote() const { return inputs_note_; }
  const std::wstring& OutputNote() const { return output_note_; }

 private:
  void Unbind();

  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
  Microsoft::WRL::ComPtr<IMMDevice> render_, capture_;
  Microsoft::WRL::ComPtr<IAudioMeterInformation> render_meter_, capture_meter_;
  CaptureStream input_, output_;
  uint64_t service_ = 0;  // AudioServiceInstance() when the objects above were made
  bool inputs_wanted_ = false, output_wanted_ = false;
  HRESULT input_error_ = S_OK, output_error_ = S_OK;  // the last failure to open each stream
  std::wstring inputs_note_, output_note_;
  LARGE_INTEGER last_read_{}, last_output_{};  // for the silence while the loopback is idle
  std::vector<float> silence_;
};
