#include "hub/meters.h"

#include <cfgmgr32.h>

#include <algorithm>
#include <cstring>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace {

constexpr REFERENCE_TIME kCaptureBuffer = 5000000;  // 500 ms: drained every UI frame (33 ms)

// An endpoint's device node is SWD\MMDEVAPI\<endpoint id>; its parent is the audio function it belongs
// to, which for the Mbox is interface 0 of the USB device.
bool IsMboxEndpoint(IMMDevice* device) {
  LPWSTR id = nullptr;
  if (FAILED(device->GetId(&id))) return false;
  std::wstring instance = L"SWD\\MMDEVAPI\\" + std::wstring(id);
  CoTaskMemFree(id);
  DEVINST endpoint = 0, parent = 0;
  if (CM_Locate_DevNodeW(&endpoint, instance.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS ||
      CM_Get_Parent(&parent, endpoint, 0) != CR_SUCCESS) {
    return false;
  }
  wchar_t parent_id[MAX_DEVICE_ID_LEN] = {};
  if (CM_Get_Device_IDW(parent, parent_id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return false;
  std::wstring upper = parent_id;
  for (auto& c : upper) c = static_cast<wchar_t>(std::towupper(c));
  return upper.rfind(L"USB\\VID_1209&PID_0001&MI_00\\", 0) == 0;
}

bool Active(IMMDevice* device) {
  DWORD state = 0;
  return SUCCEEDED(device->GetState(&state)) && state == DEVICE_STATE_ACTIVE;
}

// Peaks of the first two channels (a mono meter feeds both).
bool ReadMeter(IAudioMeterInformation* meter, float* left, float* right) {
  UINT channels = 0;
  float values[2] = {};
  if (FAILED(meter->GetMeteringChannelCount(&channels)) || channels == 0) return false;
  if (FAILED(meter->GetChannelsPeakValues(std::min(channels, 2u), values))) return false;
  *left = values[0];
  *right = channels > 1 ? values[1] : values[0];
  return true;
}

}  // namespace

ComPtr<IMMDevice> FindMboxEndpoint(IMMDeviceEnumerator* enumerator, EDataFlow flow) {
  ComPtr<IMMDeviceCollection> devices;
  if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &devices))) return nullptr;
  UINT count = 0;
  devices->GetCount(&count);
  for (UINT i = 0; i < count; ++i) {
    ComPtr<IMMDevice> device;
    if (SUCCEEDED(devices->Item(i, &device)) && IsMboxEndpoint(device.Get())) return device;
  }
  return nullptr;
}

uint64_t AudioServiceInstance() {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!manager) return 0;
  uint64_t instance = 0;
  for (const wchar_t* name : {L"Audiosrv", L"AudioEndpointBuilder"}) {
    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    if (SC_HANDLE service = OpenServiceW(manager, name, SERVICE_QUERY_STATUS)) {
      if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof status,
                               &needed)) {
        instance = instance << 32 | status.dwProcessId;
      }
      CloseServiceHandle(service);
    }
  }
  CloseServiceHandle(manager);
  return instance;
}

// ---- CaptureStream

HRESULT CaptureStream::Open(IMMDevice* device, bool loopback) {
  Close();
  HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audio_);
  WAVEFORMATEX* format = nullptr;
  if (SUCCEEDED(hr)) hr = audio_->GetMixFormat(&format);
  if (SUCCEEDED(hr)) {
    rate_ = format->nSamplesPerSec;
    channels_ = format->nChannels;
    block_align_ = format->nBlockAlign;
    bytes_ = format->wBitsPerSample / 8;
    WORD tag = format->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
      tag = static_cast<WORD>(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat.Data1);
    }
    float_ = tag == WAVE_FORMAT_IEEE_FLOAT;
    if (!channels_ || !(float_ ? bytes_ == 4 : tag == WAVE_FORMAT_PCM && bytes_ >= 2 && bytes_ <= 4)) {
      hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
  }
  if (SUCCEEDED(hr)) {
    hr = audio_->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0, kCaptureBuffer, 0,
                            format, nullptr);
  }
  if (format) CoTaskMemFree(format);
  if (SUCCEEDED(hr)) hr = audio_->GetService(IID_PPV_ARGS(&capture_));
  if (SUCCEEDED(hr)) hr = audio_->Start();
  if (FAILED(hr)) Close();
  return hr;
}

void CaptureStream::Close() {
  if (audio_) audio_->Stop();
  capture_.Reset();
  audio_.Reset();
}

HRESULT CaptureStream::Drain(const Tap& tap, size_t* total) {
  *total = 0;
  if (!capture_) return S_OK;
  UINT32 packet = 0;
  HRESULT hr;
  while (SUCCEEDED(hr = capture_->GetNextPacketSize(&packet)) && packet) {
    BYTE* data = nullptr;
    UINT32 frames = 0;
    DWORD flags = 0;
    if (FAILED(hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
    if (tap && frames) {
      stereo_.resize(2 * static_cast<size_t>(frames));
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        std::fill(stereo_.begin(), stereo_.end(), 0.0f);
      } else {
        for (UINT32 i = 0; i < frames; ++i) {
          const BYTE* frame = data + static_cast<size_t>(i) * block_align_;
          for (uint32_t c = 0; c < 2; ++c) {
            const BYTE* s = frame + std::min(c, channels_ - 1) * bytes_;
            float v;
            if (float_) {
              memcpy(&v, s, 4);
            } else if (bytes_ == 2) {
              v = static_cast<int16_t>(s[0] | s[1] << 8) / 32768.0f;
            } else if (bytes_ == 3) {
              v = static_cast<int32_t>(static_cast<uint32_t>(s[0] << 8 | s[1] << 16 | s[2] << 24)) / 2147483648.0f;
            } else {
              int32_t x;
              memcpy(&x, s, 4);
              v = x / 2147483648.0f;
            }
            stereo_[2 * i + c] = v;
          }
        }
      }
      tap(stereo_.data(), frames, rate_);
    }
    *total += frames;
    capture_->ReleaseBuffer(frames);
  }
  return hr;
}

// ---- EndpointMeters

void EndpointMeters::Unbind() {
  input_.Close();
  output_.Close();
  render_meter_.Reset();
  capture_meter_.Reset();
  render_.Reset();
  capture_.Reset();
}

void EndpointMeters::SetInputsWanted(bool wanted) {
  if (wanted == inputs_wanted_) return;
  inputs_wanted_ = wanted;
  input_error_ = S_OK;
  Refresh();
}

void EndpointMeters::SetOutputWanted(bool wanted) {
  if (wanted == output_wanted_) return;
  output_wanted_ = wanted;
  output_error_ = S_OK;
  Refresh();
}

void EndpointMeters::Refresh() {
  // Objects from before an audio service restart keep answering with silence: start over.
  uint64_t service = AudioServiceInstance();
  if (service != service_) {
    if (service_) {
      Unbind();
      enumerator_.Reset();
    }
    service_ = service;
  }
  if (!enumerator_ &&
      FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_)))) {
    return;
  }
  if (render_ && !Active(render_.Get())) {
    output_.Close();
    render_meter_.Reset();
    render_.Reset();
  }
  if (capture_ && !Active(capture_.Get())) {
    input_.Close();
    capture_meter_.Reset();
    capture_.Reset();
  }
  if (!render_ && (render_ = FindMboxEndpoint(enumerator_.Get(), eRender))) {
    render_->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, &render_meter_);
    output_error_ = S_OK;
  }
  if (!capture_ && (capture_ = FindMboxEndpoint(enumerator_.Get(), eCapture))) {
    capture_->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, &capture_meter_);
    input_error_ = S_OK;
  }

  if (!inputs_wanted_) {
    input_.Close();
  } else if (capture_ && !input_.IsOpen()) {
    input_error_ = input_.Open(capture_.Get(), false);
  }
  if (!output_wanted_) {
    output_.Close();
  } else if (render_ && !output_.IsOpen()) {
    output_error_ = output_.Open(render_.Get(), true);
    QueryPerformanceCounter(&last_output_);
  }

  auto error_text = [](const wchar_t* what, HRESULT hr) {
    wchar_t text[96];
    swprintf_s(text, L"Couldn't open the %s (0x%08lX).", what, static_cast<unsigned long>(hr));
    return std::wstring(text);
  };
  if (!inputs_wanted_ || input_.IsOpen()) {
    inputs_note_.clear();
  } else if (!capture_) {
    inputs_note_ = L"The Mbox's microphone endpoint isn't active.";
  } else if (input_error_ == AUDCLNT_E_DEVICE_IN_USE) {
    inputs_note_ = L"Another program has the inputs in exclusive mode.";
  } else if (input_error_ == E_ACCESSDENIED) {
    inputs_note_ = L"Windows' microphone privacy setting blocks desktop apps.";
  } else {
    inputs_note_ = error_text(L"inputs for metering", input_error_);
  }
  if (!output_wanted_ || output_.IsOpen()) {
    output_note_.clear();
  } else if (!render_) {
    output_note_ = L"The Mbox's speakers endpoint isn't active.";
  } else {
    output_note_ = error_text(L"output for analysis", output_error_);
  }
}

void EndpointMeters::Read(float peaks[4], const Tap& output, const Tap& inputs) {
  std::fill(peaks, peaks + 4, 0.0f);
  LARGE_INTEGER now, frequency;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&frequency);
  const double since_read = last_read_.QuadPart ? static_cast<double>(now.QuadPart - last_read_.QuadPart) / frequency.QuadPart : 0.0;
  last_read_ = now;

  size_t frames = 0;
  if (input_.IsOpen() && FAILED(input_error_ = input_.Drain(inputs, &frames))) {
    input_.Close();  // Refresh() reopens it or drops the endpoint
  }
  if (output_.IsOpen()) {
    if (FAILED(output_error_ = output_.Drain(output, &frames))) {
      output_.Close();
    } else if (frames) {
      last_output_ = now;
    } else if (output && static_cast<double>(now.QuadPart - last_output_.QuadPart) / frequency.QuadPart > 0.1) {
      // Nothing is playing: the loopback stays quiet rather than delivering silence.
      size_t silent = static_cast<size_t>(std::min(since_read, 0.25) * output_.Rate());
      silence_.assign(2 * silent, 0.0f);
      if (silent) output(silence_.data(), silent, output_.Rate());
    }
  }
  if (input_.IsOpen() && capture_meter_ && !ReadMeter(capture_meter_.Get(), &peaks[0], &peaks[1])) {
    peaks[0] = peaks[1] = 0.0f;
  }
  if (render_meter_ && !ReadMeter(render_meter_.Get(), &peaks[2], &peaks[3])) peaks[2] = peaks[3] = 0.0f;
}
