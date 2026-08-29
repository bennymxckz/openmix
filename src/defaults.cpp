// Making openmix the default audio device.
//
// Windows keeps three default endpoints per direction: Console, Multimedia and
// Communications. Applications pick between them -- Discord and Teams follow
// Communications, most everything else follows Console and Multimedia. That
// split is what lets a chat application land on the Chat channel while
// everything else goes to Game, without touching either application's
// settings.
//
// Setting them means IPolicyConfig, which is undocumented but has been the
// same shape since Vista and is what every "switch my audio device" utility on
// Windows uses. Unlike the per-application routing interface, its layout is
// well established.

#include "audio.h"

#include <windows.h>
#include <mmdeviceapi.h>

#include <string>

namespace {

// {870af99c-171d-4f9e-af0d-e63df40c2bc9}
const CLSID CLSID_PolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

// {f8679f50-850a-41cf-9c72-430f290290c8}
const IID IID_PolicyConfig = {
    0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

// Only SetDefaultEndpoint is used, but every preceding entry has to be
// declared so it lands at the right vtable offset.
struct IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR deviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

}  // namespace

bool defaultDeviceControlAvailable() {
    IPolicyConfig* cfg = nullptr;
    const HRESULT hr = ::CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL,
                                          IID_PolicyConfig, reinterpret_cast<void**>(&cfg));
    if (FAILED(hr) || !cfg) return false;
    cfg->Release();
    return true;
}

bool setDefaultEndpoint(const std::wstring& deviceId, int role) {
    if (deviceId.empty()) return false;

    IPolicyConfig* cfg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL,
                                  IID_PolicyConfig, reinterpret_cast<void**>(&cfg))) || !cfg) {
        return false;
    }
    const HRESULT hr = cfg->SetDefaultEndpoint(deviceId.c_str(), static_cast<ERole>(role));
    cfg->Release();
    return SUCCEEDED(hr);
}

std::wstring defaultEndpointId(bool capture, int role) {
    IMMDeviceEnumerator* devEnum = nullptr;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&devEnum)))) {
        return {};
    }
    IMMDevice* dev = nullptr;
    std::wstring out;
    if (SUCCEEDED(devEnum->GetDefaultAudioEndpoint(capture ? eCapture : eRender,
                                                   static_cast<ERole>(role), &dev)) && dev) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id)) && id) {
            out = id;
            ::CoTaskMemFree(id);
        }
        dev->Release();
    }
    devEnum->Release();
    return out;
}
