#include <windows.h>
#include <olectl.h>
#include "EyeCamFilter.hpp"

// Реєстраційні ключі DirectShow Video Input Device Category
// CLSID_VideoInputDeviceCategory: {860BB310-5D01-11d0-BD3B-00A0C911CE86}
DEFINE_GUID(CLSID_VideoInputDeviceCategory, 
0x860bb310, 0x5d01, 0x11d0, 0xbd, 0x3b, 0x0, 0xa0, 0xc9, 0x11, 0xce, 0x86);

static HINSTANCE g_hInstance = nullptr;
static LONG g_serverLocks = 0;

// Проста реалізація IClassFactory для створення екземпляра фільтра
class EyeCamClassFactory : public IClassFactory {
public:
    EyeCamClassFactory() : m_refCount(1) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_refCount); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0) delete this;
        return count;
    }
    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (pUnkOuter != nullptr) return CLASS_E_NOAGGREGATION;

        EyeCamFilter* filter = new EyeCamFilter();
        HRESULT hr = filter->QueryInterface(riid, ppv);
        filter->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) InterlockedIncrement(&g_serverLocks);
        else InterlockedDecrement(&g_serverLocks);
        return S_OK;
    }
private:
    LONG m_refCount;
};

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hInstance = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (rclsid == CLSID_EyeCamFilter) {
        EyeCamClassFactory* factory = new EyeCamClassFactory();
        HRESULT hr = factory->QueryInterface(riid, ppv);
        factory->Release();
        return hr;
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow(void) {
    return (g_serverLocks == 0) ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer(void) {
    WCHAR path[MAX_PATH];
    if (!GetModuleFileNameW(g_hInstance, path, MAX_PATH)) return E_FAIL;

    // Реєстрація у CLSID
    HKEY hKey = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_CLASSES_ROOT, L"CLSID\\{E4E5C1A0-3882-4C79-880B-DE8A3626C101}", 
                                     0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
    if (status != ERROR_SUCCESS) return E_FAIL;
    RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)L"EyeCam Virtual Camera", sizeof(L"EyeCam Virtual Camera"));

    HKEY hKeyInproc = nullptr;
    RegCreateKeyExW(hKey, L"InprocServer32", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKeyInproc, nullptr);
    RegSetValueExW(hKeyInproc, nullptr, 0, REG_SZ, (const BYTE*)path, (DWORD)((wcslen(path) + 1) * sizeof(WCHAR)));
    RegSetValueExW(hKeyInproc, L"ThreadingModel", 0, REG_SZ, (const BYTE*)L"Both", sizeof(L"Both"));
    RegCloseKey(hKeyInproc);
    RegCloseKey(hKey);

    // Реєстрація у Video Input Device Category (Камери Windows)
    HKEY hKeyCat = nullptr;
    status = RegCreateKeyExW(HKEY_CLASSES_ROOT, 
        L"CLSID\\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\\Instance\\{E4E5C1A0-3882-4C79-880B-DE8A3626C101}",
        0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKeyCat, nullptr);
    if (status == ERROR_SUCCESS) {
        RegSetValueExW(hKeyCat, L"FriendlyName", 0, REG_SZ, (const BYTE*)L"EyeCam Virtual Camera", sizeof(L"EyeCam Virtual Camera"));
        RegSetValueExW(hKeyCat, L"CLSID", 0, REG_SZ, (const BYTE*)L"{E4E5C1A0-3882-4C79-880B-DE8A3626C101}", sizeof(L"{E4E5C1A0-3882-4C79-880B-DE8A3626C101}"));
        RegCloseKey(hKeyCat);
    }

    return S_OK;
}

STDAPI DllUnregisterServer(void) {
    RegDeleteKeyW(HKEY_CLASSES_ROOT, L"CLSID\\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\\Instance\\{E4E5C1A0-3882-4C79-880B-DE8A3626C101}");
    RegDeleteKeyW(HKEY_CLASSES_ROOT, L"CLSID\\{E4E5C1A0-3882-4C79-880B-DE8A3626C101}\\InprocServer32");
    RegDeleteKeyW(HKEY_CLASSES_ROOT, L"CLSID\\{E4E5C1A0-3882-4C79-880B-DE8A3626C101}");
    return S_OK;
}