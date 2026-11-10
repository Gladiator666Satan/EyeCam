#if defined(_WIN32)
#include <windows.h>
#include <dshow.h>
#include <initguid.h>
#include <uuids.h>
#include <shlwapi.h>
#include <atomic>
#include <string>

#include "EyeCamGuids.hpp"
#include "EyeCamFilter.hpp"

#pragma comment(lib, "shlwapi.lib")

static HINSTANCE g_hInst = nullptr;
static std::atomic<ULONG> g_serverLocks{0};

// Проста фабрика класів COM для EyeCamCaptureFilter
class CClassFactory : public IClassFactory {
public:
    CClassFactory() = default;
    virtual ~CClassFactory() = default;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refCount; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = --m_refCount;
        if (count == 0) delete this;
        return count;
    }

    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (pUnkOuter != nullptr) return CLASS_E_NOAGGREGATION;

        auto* pFilter = new EyeCam::EyeCamCaptureFilter();
        HRESULT hr = pFilter->QueryInterface(riid, ppv);
        pFilter->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) ++g_serverLocks;
        else --g_serverLocks;
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refCount{1};
};

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hInst = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (rclsid == CLSID_EyeCamFilter) {
        auto* pFactory = new CClassFactory();
        HRESULT hr = pFactory->QueryInterface(riid, ppv);
        pFactory->Release();
        return hr;
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() {
    return (g_serverLocks == 0) ? S_OK : S_FALSE;
}

// Допоміжна функція реєстрації записів у реєстрі Windows (HKCR)
static HRESULT SetRegistryKey(HKEY hRoot, const std::wstring& subKey, const std::wstring& valueName, const std::wstring& data) {
    HKEY hKey = nullptr;
    LONG lRes = RegCreateKeyExW(hRoot, subKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
    if (lRes != ERROR_SUCCESS) return HRESULT_FROM_WIN32(lRes);

    lRes = RegSetValueExW(hKey, valueName.empty() ? nullptr : valueName.c_str(), 0, REG_SZ, 
                          reinterpret_cast<const BYTE*>(data.c_str()), 
                          static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    return HRESULT_FROM_WIN32(lRes);
}

/**
 * @brief Реєстрація COM-сервера та фільтра DirectShow у системі (через regsvr32).
 * 
 * Фільтр реєструється в категорії CLSID_VideoInputDeviceCategory з атрибутом PIN_CATEGORY_CAPTURE.
 * Завдяки цьому Zoom, MS Teams, Discord, OBS, Google Chrome та Skype автоматично розпізнають
 * фільтр як реальну підключену вебкамеру.
 */
STDAPI DllRegisterServer() {
    wchar_t modulePath[MAX_PATH];
    if (GetModuleFileNameW(g_hInst, modulePath, MAX_PATH) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // 1. Реєстрація COM-сервера в HKEY_CLASSES_ROOT\CLSID\{CLSID_EyeCamFilter}
    std::wstring clsidPath = L"CLSID\\" + std::wstring(CLSID_EYECAM_STRING);
    SetRegistryKey(HKEY_CLASSES_ROOT, clsidPath, L"", VIRTUAL_CAMERA_FRIENDLY_NAME);
    SetRegistryKey(HKEY_CLASSES_ROOT, clsidPath + L"\\InprocServer32", L"", modulePath);
    SetRegistryKey(HKEY_CLASSES_ROOT, clsidPath + L"\\InprocServer32", L"ThreadingModel", L"Both");

    // 2. Реєстрація фільтра в DirectShow Filter Mapper 2 у категорії CLSID_VideoInputDeviceCategory
    HRESULT hr = CoInitialize(nullptr);
    bool coInitialized = SUCCEEDED(hr);

    IFilterMapper2* pFM2 = nullptr;
    hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2, reinterpret_cast<void**>(&pFM2));
    if (SUCCEEDED(hr)) {
        REGPINTYPES pinTypes[2];
        pinTypes[0].clsMajorType = &MEDIATYPE_Video;
        pinTypes[0].clsMinorType = &MEDIASUBTYPE_YUY2;

        pinTypes[1].clsMajorType = &MEDIATYPE_Video;
        pinTypes[1].clsMinorType = &MEDIASUBTYPE_RGB24;

        REGFILTERPINS2 regPin{};
        regPin.dwFlags = REG_PINFLAG_B_OUTPUT;
        regPin.cInstances = 1;
        regPin.nMediaTypes = 2;
        regPin.lpMediaType = pinTypes;
        regPin.nMediums = 0;
        regPin.lpMedium = nullptr;
        regPin.clsPinCategory = &PIN_CATEGORY_CAPTURE; // Ключовий прапорець вебкамери

        REGFILTER2 regFilter{};
        regFilter.dwVersion = 2;
        regFilter.dwMerit = MERIT_DO_NOT_USE; // Стандартний merit для віртуальних камер
        regFilter.cPins2 = 1;
        regFilter.rgPins2 = &regPin;

        hr = pFM2->RegisterFilter(
            CLSID_EyeCamFilter,
            VIRTUAL_CAMERA_FRIENDLY_NAME,
            nullptr,
            &CLSID_VideoInputDeviceCategory, // Категорія фізичних та віртуальних камер
            nullptr,
            &regFilter
        );

        pFM2->Release();
    }

    if (coInitialized) {
        CoUninitialize();
    }

    return hr;
}

/**
 * @brief Деінсталяція/дереєстрація фільтра з системи (regsvr32 /u).
 */
STDAPI DllUnregisterServer() {
    HRESULT hr = CoInitialize(nullptr);
    bool coInitialized = SUCCEEDED(hr);

    IFilterMapper2* pFM2 = nullptr;
    hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2, reinterpret_cast<void**>(&pFM2));
    if (SUCCEEDED(hr)) {
        pFM2->UnregisterFilter(&CLSID_VideoInputDeviceCategory, nullptr, CLSID_EyeCamFilter);
        pFM2->Release();
    }

    if (coInitialized) {
        CoUninitialize();
    }

    // Видалення ключів із реєстру
    std::wstring clsidPath = L"CLSID\\" + std::wstring(CLSID_EYECAM_STRING);
    SHDeleteKeyW(HKEY_CLASSES_ROOT, clsidPath.c_str());

    return S_OK;
}
#endif
