// COM entry points of mbox2asio.dll. DllRegisterServer writes the two keys an ASIO driver needs (an
// administrator's job: tools\asio\install.ps1 runs regsvr32 on the installed copy):
//   HKCR\CLSID\{5FE5CF79-...}\InprocServer32 = <path of this DLL>, ThreadingModel = Apartment
//   HKLM\SOFTWARE\ASIO\Mbox 2 Mini (open firmware): CLSID, Description
#include <windows.h>

#include <objbase.h>

#include <atomic>
#include <new>
#include <string>

#include "asio/driver.h"
#include "asio/log.h"
#include "asio/names.h"

namespace mbox2::asio {
namespace {

std::atomic<long> g_module_refs{0};
HMODULE g_module = nullptr;

class ClassFactory final : public IClassFactory {
 public:
  ClassFactory() { ModuleAddRef(); }

  STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
      *object = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return refs_.fetch_add(1) + 1; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG remaining = refs_.fetch_sub(1) - 1;
    if (remaining == 0) delete this;
    return remaining;
  }
  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;
    return Driver::Create(riid, object);
  }
  STDMETHODIMP LockServer(BOOL lock) override {
    if (lock) {
      ModuleAddRef();
    } else {
      ModuleRelease();
    }
    return S_OK;
  }

 private:
  ~ClassFactory() { ModuleRelease(); }

  std::atomic<ULONG> refs_{1};
};

HRESULT SetString(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
  const LSTATUS status =
      RegSetKeyValueW(root, key.c_str(), name, REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  return HRESULT_FROM_WIN32(status);
}

}  // namespace

void ModuleAddRef() { g_module_refs.fetch_add(1); }
void ModuleRelease() { g_module_refs.fetch_sub(1); }

}  // namespace mbox2::asio

using namespace mbox2::asio;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = instance;
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** object) {
  if (!object) return E_POINTER;
  *object = nullptr;
  if (!IsEqualCLSID(clsid, kClsid)) return CLASS_E_CLASSNOTAVAILABLE;
  auto* factory = new (std::nothrow) ClassFactory;
  if (!factory) return E_OUTOFMEMORY;
  const HRESULT hr = factory->QueryInterface(riid, object);
  factory->Release();
  return hr;
}

STDAPI DllCanUnloadNow() { return g_module_refs.load() == 0 ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() {
  wchar_t path[MAX_PATH];
  if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
  const std::wstring clsid_key = std::wstring(L"CLSID\\") + kClsidString;
  const std::wstring asio_key = std::wstring(L"SOFTWARE\\ASIO\\") + kRegistryName;
  HRESULT hr = SetString(HKEY_CLASSES_ROOT, clsid_key, nullptr, kRegistryName);
  if (SUCCEEDED(hr)) hr = SetString(HKEY_CLASSES_ROOT, clsid_key + L"\\InprocServer32", nullptr, path);
  if (SUCCEEDED(hr)) hr = SetString(HKEY_CLASSES_ROOT, clsid_key + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
  if (SUCCEEDED(hr)) hr = SetString(HKEY_LOCAL_MACHINE, asio_key, L"CLSID", kClsidString);
  if (SUCCEEDED(hr)) hr = SetString(HKEY_LOCAL_MACHINE, asio_key, L"Description", kRegistryName);
  Log("DllRegisterServer %ls: 0x%08lX", path, static_cast<unsigned long>(hr));
  return hr;
}

STDAPI DllUnregisterServer() {
  const std::wstring clsid_key = std::wstring(L"CLSID\\") + kClsidString;
  const std::wstring asio_key = std::wstring(L"SOFTWARE\\ASIO\\") + kRegistryName;
  const LSTATUS a = RegDeleteTreeW(HKEY_CLASSES_ROOT, clsid_key.c_str());
  const LSTATUS b = RegDeleteTreeW(HKEY_LOCAL_MACHINE, asio_key.c_str());
  const bool ok = (a == ERROR_SUCCESS || a == ERROR_FILE_NOT_FOUND) && (b == ERROR_SUCCESS || b == ERROR_FILE_NOT_FOUND);
  Log("DllUnregisterServer: %ld %ld", static_cast<long>(a), static_cast<long>(b));
  return ok ? S_OK : HRESULT_FROM_WIN32(a != ERROR_SUCCESS && a != ERROR_FILE_NOT_FOUND ? a : b);
}
