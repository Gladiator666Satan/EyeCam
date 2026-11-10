#include "EyeCamFilter.hpp"
#include <initguid.h>
#include <dvdmedia.h>
#include <process.h>
#include <chrono>
#include <vector>

// Визначення форматів DirectShow та GUID
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace {

void CopyMediaType(AM_MEDIA_TYPE* pDest, const AM_MEDIA_TYPE* pSrc) {
    *pDest = *pSrc;
    if (pSrc->cbFormat > 0 && pSrc->pbFormat != nullptr) {
        pDest->pbFormat = (BYTE*)CoTaskMemAlloc(pSrc->cbFormat);
        if (pDest->pbFormat) {
            memcpy(pDest->pbFormat, pSrc->pbFormat, pSrc->cbFormat);
        }
    }
    if (pSrc->pUnk != nullptr) {
        pDest->pUnk = pSrc->pUnk;
        pDest->pUnk->AddRef();
    }
}

void FreeMediaType(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat > 0) {
        CoTaskMemFree(mt.pbFormat);
        mt.pbFormat = nullptr;
        mt.cbFormat = 0;
    }
    if (mt.pUnk != nullptr) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

void CreateDefaultMediaType(AM_MEDIA_TYPE* pmt, uint32_t width, uint32_t height, EyeCam::PixelFormat format, uint32_t fps) {
    ZeroMemory(pmt, sizeof(AM_MEDIA_TYPE));
    pmt->majortype = MEDIATYPE_Video;

    VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
    ZeroMemory(vih, sizeof(VIDEOINFOHEADER));

    vih->rcSource = RECT{ 0, 0, (LONG)width, (LONG)height };
    vih->rcTarget = RECT{ 0, 0, (LONG)width, (LONG)height };
    vih->AvgTimePerFrame = (fps > 0) ? (10000000LL / fps) : 333333LL; // 100ns units

    BITMAPINFOHEADER& bih = vih->bmiHeader;
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = width;
    bih.biHeight = height;
    bih.biPlanes = 1;

    if (format == EyeCam::PixelFormat::YUY2) {
        pmt->subtype = MEDIASUBTYPE_YUY2;
        bih.biBitCount = 16;
        bih.biCompression = MAKEFOURCC('Y', 'U', 'Y', '2');
        bih.biSizeImage = width * height * 2;
    } else {
        pmt->subtype = MEDIASUBTYPE_RGB24;
        bih.biBitCount = 24;
        bih.biCompression = BI_RGB;
        bih.biSizeImage = width * height * 3;
    }

    pmt->formattype = FORMAT_VideoInfo;
    pmt->cbFormat = sizeof(VIDEOINFOHEADER);
    pmt->pbFormat = (BYTE*)vih;
    pmt->bFixedSizeSamples = TRUE;
    pmt->bTemporalCompression = FALSE;
    pmt->lSampleSize = bih.biSizeImage;
}

} // anonymous namespace

// -----------------------------------------------------------------------------
// EyeCamFilter Implementation
// -----------------------------------------------------------------------------

EyeCamFilter::EyeCamFilter()
    : m_refCount(1),
      m_state(State_Stopped),
      m_pClock(nullptr),
      m_pGraph(nullptr),
      m_pName(nullptr),
      m_pPin(nullptr) {
    InitializeCriticalSection(&m_csFilter);
    m_pPin = new EyeCamPin(this);
}

EyeCamFilter::~EyeCamFilter() {
    if (m_pPin) {
        delete m_pPin;
        m_pPin = nullptr;
    }
    if (m_pClock) {
        m_pClock->Release();
        m_pClock = nullptr;
    }
    if (m_pName) {
        CoTaskMemFree(m_pName);
        m_pName = nullptr;
    }
    DeleteCriticalSection(&m_csFilter);
}

STDMETHODIMP EyeCamFilter::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (riid == IID_IUnknown || riid == IID_IPersist || riid == IID_IMediaFilter || riid == IID_IBaseFilter) {
        *ppv = static_cast<IBaseFilter*>(this);
    } else if (riid == IID_IAMFilterMiscFlags) {
        *ppv = static_cast<IAMFilterMiscFlags*>(this);
    }

    if (*ppv) {
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) EyeCamFilter::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) EyeCamFilter::Release() {
    ULONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) {
        delete this;
    }
    return count;
}

STDMETHODIMP EyeCamFilter::GetClassID(CLSID* pClsid) {
    if (!pClsid) return E_POINTER;
    *pClsid = CLSID_EyeCamFilter;
    return S_OK;
}

STDMETHODIMP EyeCamFilter::Stop() {
    EnterCriticalSection(&m_csFilter);
    if (m_state != State_Stopped) {
        if (m_pPin) m_pPin->Inactive();
        m_state = State_Stopped;
    }
    LeaveCriticalSection(&m_csFilter);
    return S_OK;
}

STDMETHODIMP EyeCamFilter::Pause() {
    EnterCriticalSection(&m_csFilter);
    if (m_state == State_Stopped) {
        if (m_pPin) m_pPin->Active();
    }
    m_state = State_Paused;
    LeaveCriticalSection(&m_csFilter);
    return S_OK;
}

STDMETHODIMP EyeCamFilter::Run(REFERENCE_TIME tStart) {
    EnterCriticalSection(&m_csFilter);
    if (m_state == State_Stopped) {
        if (m_pPin) m_pPin->Active();
    }
    m_state = State_Running;
    LeaveCriticalSection(&m_csFilter);
    return S_OK;
}

STDMETHODIMP EyeCamFilter::GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) {
    if (!State) return E_POINTER;
    *State = m_state;
    return S_OK;
}

STDMETHODIMP EyeCamFilter::SetSyncSource(IReferenceClock* pClock) {
    EnterCriticalSection(&m_csFilter);
    if (m_pClock) m_pClock->Release();
    m_pClock = pClock;
    if (m_pClock) m_pClock->AddRef();
    LeaveCriticalSection(&m_csFilter);
    return S_OK;
}

STDMETHODIMP EyeCamFilter::GetSyncSource(IReferenceClock** ppClock) {
    if (!ppClock) return E_POINTER;
    EnterCriticalSection(&m_csFilter);
    *ppClock = m_pClock;
    if (*ppClock) (*ppClock)->AddRef();
    LeaveCriticalSection(&m_csFilter);
    return S_OK;
}

// Проста імплементація IEnumPins
class EnumPinsImpl : public IEnumPins {
public:
    EnumPinsImpl(IPin* pPin) : m_refCount(1), m_pPin(pPin), m_index(0) {
        if (m_pPin) m_pPin->AddRef();
    }
    virtual ~EnumPinsImpl() {
        if (m_pPin) m_pPin->Release();
    }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumPins) {
            *ppv = static_cast<IEnumPins*>(this);
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
    STDMETHODIMP Next(ULONG cPins, IPin** ppPins, ULONG* pcFetched) override {
        if (!ppPins) return E_POINTER;
        if (pcFetched) *pcFetched = 0;
        if (cPins == 0) return S_OK;
        if (m_index == 0 && m_pPin) {
            ppPins[0] = m_pPin;
            m_pPin->AddRef();
            m_index = 1;
            if (pcFetched) *pcFetched = 1;
            return (cPins == 1) ? S_OK : S_FALSE;
        }
        return S_FALSE;
    }
    STDMETHODIMP Skip(ULONG cPins) override {
        m_index += cPins;
        return (m_index <= 1) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override {
        m_index = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumPins** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new EnumPinsImpl(m_pPin);
        return S_OK;
    }
private:
    LONG m_refCount;
    IPin* m_pPin;
    ULONG m_index;
};

STDMETHODIMP EyeCamFilter::EnumPins(IEnumPins** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new EnumPinsImpl(m_pPin);
    return S_OK;
}

STDMETHODIMP EyeCamFilter::FindPin(LPCWSTR Id, IPin** ppPin) {
    if (!ppPin) return E_POINTER;
    *ppPin = nullptr;
    if (Id && wcscmp(Id, L"Capture") == 0) {
        *ppPin = m_pPin;
        if (*ppPin) (*ppPin)->AddRef();
        return S_OK;
    }
    return VFW_E_NOT_FOUND;
}

STDMETHODIMP EyeCamFilter::QueryFilterInfo(FILTER_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    EnterCriticalSection(&m_csFilter);
    if (m_pName) {
        wcsncpy_s(pInfo->achName, m_pName, _TRUNCATE);
    } else {
        wcscpy_s(pInfo->achName, L"EyeCam Virtual Camera");
    }
    pInfo->pGraph = m_pGraph;
    if (pInfo->pGraph) pInfo->pGraph->AddRef();
    LeaveCriticalSection(&m_csFilter);
    return S_OK;
}

STDMETHODIMP EyeCamFilter::JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) {
    EnterCriticalSection(&m_csFilter);
    m_pGraph = pGraph;
    if (m_pName) {
        CoTaskMemFree(m_pName);
        m_pName = nullptr;
    }
    if (pName) {
        size_t len = wcslen(pName) + 1;
        m_pName = (LPWSTR)CoTaskMemAlloc(len * sizeof(WCHAR));
        if (m_pName) wcscpy_s(m_pName, len, pName);
    }
    LeaveCriticalSection(&m_csFilter);
    return S_OK;
}

STDMETHODIMP EyeCamFilter::QueryVendorInfo(LPWSTR* pVendorInfo) {
    if (!pVendorInfo) return E_POINTER;
    const WCHAR vendor[] = L"EyeCam Open-Source Project";
    *pVendorInfo = (LPWSTR)CoTaskMemAlloc(sizeof(vendor));
    if (!*pVendorInfo) return E_OUTOFMEMORY;
    wcscpy_s(*pVendorInfo, sizeof(vendor)/sizeof(WCHAR), vendor);
    return S_OK;
}

STDMETHODIMP_(ULONG) EyeCamFilter::GetMiscFlags() {
    return AM_FILTER_MISC_FLAGS_IS_SOURCE;
}

// -----------------------------------------------------------------------------
// EyeCamPin Implementation
// -----------------------------------------------------------------------------

EyeCamPin::EyeCamPin(EyeCamFilter* pFilter)
    : m_pFilter(pFilter),
      m_pConnectedPin(nullptr),
      m_pInputPin(nullptr),
      m_pAllocator(nullptr),
      m_hThread(nullptr),
      m_hStopEvent(nullptr),
      m_hMapFile(nullptr),
      m_pSharedBuf(nullptr),
      m_hFrameEvent(nullptr),
      m_hMutex(nullptr) {
    InitializeCriticalSection(&m_csPin);
    CreateDefaultMediaType(&m_mediaType, 1920, 1080, EyeCam::PixelFormat::YUY2, 60);
}

EyeCamPin::~EyeCamPin() {
    Inactive();
    FreeMediaType(m_mediaType);
    DeleteCriticalSection(&m_csPin);
}

STDMETHODIMP EyeCamPin::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (riid == IID_IUnknown || riid == IID_IPin) {
        *ppv = static_cast<IPin*>(this);
    } else if (riid == IID_IAMStreamConfig) {
        *ppv = static_cast<IAMStreamConfig*>(this);
    } else if (riid == IID_IKsPropertySet) {
        *ppv = static_cast<IKsPropertySet*>(this);
    }

    if (*ppv) {
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) EyeCamPin::AddRef() {
    return m_pFilter->AddRef();
}

STDMETHODIMP_(ULONG) EyeCamPin::Release() {
    return m_pFilter->Release();
}

STDMETHODIMP EyeCamPin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
    if (!pReceivePin) return E_POINTER;
    EnterCriticalSection(&m_csPin);

    if (m_pConnectedPin != nullptr) {
        LeaveCriticalSection(&m_csPin);
        return VFW_E_ALREADY_CONNECTED;
    }

    // Узгодження медіа типу
    AM_MEDIA_TYPE mt;
    if (pmt) {
        if (QueryAccept(pmt) != S_OK) {
            LeaveCriticalSection(&m_csPin);
            return VFW_E_TYPE_NOT_ACCEPTED;
        }
        CopyMediaType(&mt, pmt);
    } else {
        CopyMediaType(&mt, &m_mediaType);
    }

    HRESULT hr = pReceivePin->ReceiveConnection(this, &mt);
    if (SUCCEEDED(hr)) {
        m_pConnectedPin = pReceivePin;
        m_pConnectedPin->AddRef();
        m_pConnectedPin->QueryInterface(IID_IMemInputPin, (void**)&m_pInputPin);
        FreeMediaType(m_mediaType);
        m_mediaType = mt;
    } else {
        FreeMediaType(mt);
    }

    LeaveCriticalSection(&m_csPin);
    return hr;
}

STDMETHODIMP EyeCamPin::ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) {
    return E_UNEXPECTED; // Вихідний пін не приймає вхідні з'єднання
}

STDMETHODIMP EyeCamPin::Disconnect() {
    EnterCriticalSection(&m_csPin);
    if (m_pConnectedPin == nullptr) {
        LeaveCriticalSection(&m_csPin);
        return S_FALSE;
    }

    if (m_pInputPin) {
        m_pInputPin->Release();
        m_pInputPin = nullptr;
    }

    m_pConnectedPin->Release();
    m_pConnectedPin = nullptr;

    LeaveCriticalSection(&m_csPin);
    return S_OK;
}

STDMETHODIMP EyeCamPin::ConnectedTo(IPin** ppPin) {
    if (!ppPin) return E_POINTER;
    EnterCriticalSection(&m_csPin);
    *ppPin = m_pConnectedPin;
    if (*ppPin) (*ppPin)->AddRef();
    LeaveCriticalSection(&m_csPin);
    return (*ppPin) ? S_OK : VFW_E_NOT_CONNECTED;
}

STDMETHODIMP EyeCamPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    EnterCriticalSection(&m_csPin);
    if (!m_pConnectedPin) {
        LeaveCriticalSection(&m_csPin);
        return VFW_E_NOT_CONNECTED;
    }
    CopyMediaType(pmt, &m_mediaType);
    LeaveCriticalSection(&m_csPin);
    return S_OK;
}

STDMETHODIMP EyeCamPin::QueryPinInfo(PIN_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    pInfo->pFilter = m_pFilter;
    if (pInfo->pFilter) pInfo->pFilter->AddRef();
    pInfo->dir = PINDIR_OUTPUT;
    wcscpy_s(pInfo->achName, L"Capture");
    return S_OK;
}

STDMETHODIMP EyeCamPin::QueryDirection(PIN_DIRECTION* pPinDir) {
    if (!pPinDir) return E_POINTER;
    *pPinDir = PINDIR_OUTPUT;
    return S_OK;
}

STDMETHODIMP EyeCamPin::QueryId(LPWSTR* Id) {
    if (!Id) return E_POINTER;
    const WCHAR pinId[] = L"Capture";
    *Id = (LPWSTR)CoTaskMemAlloc(sizeof(pinId));
    if (!*Id) return E_OUTOFMEMORY;
    wcscpy_s(*Id, sizeof(pinId)/sizeof(WCHAR), pinId);
    return S_OK;
}

STDMETHODIMP EyeCamPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (pmt->majortype != MEDIATYPE_Video) return S_FALSE;
    if (pmt->subtype != MEDIASUBTYPE_YUY2 && 
        pmt->subtype != MEDIASUBTYPE_RGB24 && 
        pmt->subtype != MEDIASUBTYPE_RGB32) {
        return S_FALSE;
    }
    return S_OK;
}

// Імплементація IEnumMediaTypes для піна
class EnumMediaTypesImpl : public IEnumMediaTypes {
public:
    EnumMediaTypesImpl(EyeCamPin* pPin) : m_refCount(1), m_pPin(pPin), m_index(0) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
            *ppv = static_cast<IEnumMediaTypes*>(this);
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
    STDMETHODIMP Next(ULONG cMediaTypes, AM_MEDIA_TYPE** ppMediaTypes, ULONG* pcFetched) override {
        if (!ppMediaTypes) return E_POINTER;
        if (pcFetched) *pcFetched = 0;
        ULONG fetched = 0;

        int totalFormats = 0;
        int dummySize = 0;
        m_pPin->GetNumberOfCapabilities(&totalFormats, &dummySize);

        while (m_index < (ULONG)totalFormats && fetched < cMediaTypes) {
            ppMediaTypes[fetched] = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
            m_pPin->GetStreamCaps(m_index, &ppMediaTypes[fetched], nullptr);
            m_index++;
            fetched++;
        }

        if (pcFetched) *pcFetched = fetched;
        return (fetched == cMediaTypes) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG cMediaTypes) override {
        m_index += cMediaTypes;
        return S_OK;
    }
    STDMETHODIMP Reset() override {
        m_index = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumMediaTypes** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new EnumMediaTypesImpl(m_pPin);
        return S_OK;
    }
private:
    LONG m_refCount;
    EyeCamPin* m_pPin;
    ULONG m_index;
};

STDMETHODIMP EyeCamPin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new EnumMediaTypesImpl(this);
    return S_OK;
}

STDMETHODIMP EyeCamPin::QueryInternalConnections(IPin** apPin, ULONG* nPin) {
    return E_NOTIMPL;
}

STDMETHODIMP EyeCamPin::EndOfStream() { return S_OK; }
STDMETHODIMP EyeCamPin::BeginFlush() { return S_OK; }
STDMETHODIMP EyeCamPin::EndFlush() { return S_OK; }
STDMETHODIMP EyeCamPin::NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) { return S_OK; }

// -----------------------------------------------------------------------------
// IAMStreamConfig
// -----------------------------------------------------------------------------

struct FormatPreset {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    GUID subtype;
};

static const FormatPreset g_presets[] = {
    { 1920, 1080, 60, MEDIASUBTYPE_YUY2 },
    { 1920, 1080, 30, MEDIASUBTYPE_YUY2 },
    { 1920, 1080, 60, MEDIASUBTYPE_RGB24 },
    { 1920, 1080, 30, MEDIASUBTYPE_RGB24 },
    { 1280, 720,  60, MEDIASUBTYPE_YUY2 },
    { 1280, 720,  30, MEDIASUBTYPE_YUY2 },
    { 1280, 720,  60, MEDIASUBTYPE_RGB24 },
    { 1280, 720,  30, MEDIASUBTYPE_RGB24 },
    { 640,  480,  30, MEDIASUBTYPE_YUY2 },
    { 640,  480,  30, MEDIASUBTYPE_RGB24 }
};

STDMETHODIMP EyeCamPin::SetFormat(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    EnterCriticalSection(&m_csPin);

    if (m_pFilter->m_state != State_Stopped) {
        LeaveCriticalSection(&m_csPin);
        return VFW_E_NOT_STOPPED;
    }

    if (QueryAccept(pmt) != S_OK) {
        LeaveCriticalSection(&m_csPin);
        return VFW_E_INVALIDMEDIATYPE;
    }

    FreeMediaType(m_mediaType);
    CopyMediaType(&m_mediaType, pmt);

    LeaveCriticalSection(&m_csPin);
    return S_OK;
}

STDMETHODIMP EyeCamPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    if (!ppmt) return E_POINTER;
    *ppmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    if (!*ppmt) return E_OUTOFMEMORY;
    EnterCriticalSection(&m_csPin);
    CopyMediaType(*ppmt, &m_mediaType);
    LeaveCriticalSection(&m_csPin);
    return S_OK;
}

STDMETHODIMP EyeCamPin::GetNumberOfCapabilities(int* piCount, int* piSize) {
    if (!piCount || !piSize) return E_POINTER;
    *piCount = sizeof(g_presets) / sizeof(g_presets[0]);
    *piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP EyeCamPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
    if (!ppmt) return E_POINTER;
    int count = sizeof(g_presets) / sizeof(g_presets[0]);
    if (iIndex < 0 || iIndex >= count) return E_INVALIDARG;

    const auto& preset = g_presets[iIndex];
    EyeCam::PixelFormat fmt = (preset.subtype == MEDIASUBTYPE_YUY2) ? EyeCam::PixelFormat::YUY2 : EyeCam::PixelFormat::RGB24;

    *ppmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    CreateDefaultMediaType(*ppmt, preset.width, preset.height, fmt, preset.fps);

    if (pSCC) {
        VIDEO_STREAM_CONFIG_CAPS* caps = (VIDEO_STREAM_CONFIG_CAPS*)pSCC;
        ZeroMemory(caps, sizeof(VIDEO_STREAM_CONFIG_CAPS));
        caps->guid = FORMAT_VideoInfo;
        caps->VideoStandard = 0;
        caps->InputSize = SIZE{ (LONG)preset.width, (LONG)preset.height };
        caps->MinCroppingSize = SIZE{ (LONG)preset.width, (LONG)preset.height };
        caps->MaxCroppingSize = SIZE{ (LONG)preset.width, (LONG)preset.height };
        caps->MinOutputSize = SIZE{ (LONG)preset.width, (LONG)preset.height };
        caps->MaxOutputSize = SIZE{ (LONG)preset.width, (LONG)preset.height };
        caps->MinFrameInterval = 10000000LL / preset.fps;
        caps->MaxFrameInterval = 10000000LL / 1;
        caps->MinBitsPerSecond = preset.width * preset.height * 16 * 15;
        caps->MaxBitsPerSecond = preset.width * preset.height * 24 * preset.fps;
    }

    return S_OK;
}

// -----------------------------------------------------------------------------
// IKsPropertySet (Необхідно для визначення Windows як WebCam Pin Category)
// -----------------------------------------------------------------------------

STDMETHODIMP EyeCamPin::Set(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData) {
    return E_NOTIMPL;
}

STDMETHODIMP EyeCamPin::Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) {
    if (guidPropSet == AMPROPSETID_Pin) {
        if (dwPropID == AMPROPERTY_PIN_CATEGORY) {
            if (pcbReturned) *pcbReturned = sizeof(GUID);
            if (pPropData == nullptr) return S_OK;
            if (cbPropData < sizeof(GUID)) return E_UNEXPECTED;
            *(GUID*)pPropData = PIN_CATEGORY_CAPTURE;
            return S_OK;
        }
    }
    return E_PROP_SET_UNSUPPORTED;
}

STDMETHODIMP EyeCamPin::QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) {
    if (guidPropSet == AMPROPSETID_Pin) {
        if (dwPropID == AMPROPERTY_PIN_CATEGORY) {
            if (pTypeSupport) *pTypeSupport = KSPROPERTY_SUPPORT_GET;
            return S_OK;
        }
    }
    return E_PROP_SET_UNSUPPORTED;
}

// -----------------------------------------------------------------------------
// Потокова передача кадрів (Capture Thread)
// -----------------------------------------------------------------------------

HRESULT EyeCamPin::Active() {
    EnterCriticalSection(&m_csPin);

    // Ініціалізація спільної пам'яті продюсера (Global / Local)
    m_hMapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, EyeCam::SHM_NAME_GLOBAL);
    if (!m_hMapFile) m_hMapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, EyeCam::SHM_NAME_LOCAL);

    if (m_hMapFile) {
        m_pSharedBuf = MapViewOfFile(m_hMapFile, FILE_MAP_READ, 0, 0, EyeCam::TOTAL_SHARED_MEMORY_SIZE);
    }

    m_hFrameEvent = OpenEventW(SYNCHRONIZE, FALSE, EyeCam::SHM_EVENT_NAME_GLOBAL);
    if (!m_hFrameEvent) m_hFrameEvent = OpenEventW(SYNCHRONIZE, FALSE, EyeCam::SHM_EVENT_NAME_LOCAL);

    m_hMutex = OpenMutexW(SYNCHRONIZE, FALSE, EyeCam::SHM_MUTEX_NAME_GLOBAL);
    if (!m_hMutex) m_hMutex = OpenMutexW(SYNCHRONIZE, FALSE, EyeCam::SHM_MUTEX_NAME_LOCAL);

    m_hStopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    m_hThread = CreateThread(nullptr, 0, ThreadProcThunk, this, 0, nullptr);

    LeaveCriticalSection(&m_csPin);
    return S_OK;
}

HRESULT EyeCamPin::Inactive() {
    EnterCriticalSection(&m_csPin);

    if (m_hStopEvent) {
        SetEvent(m_hStopEvent);
        if (m_hThread) {
            WaitForSingleObject(m_hThread, 2000);
            CloseHandle(m_hThread);
            m_hThread = nullptr;
        }
        CloseHandle(m_hStopEvent);
        m_hStopEvent = nullptr;
    }

    if (m_pSharedBuf) {
        UnmapViewOfFile(m_pSharedBuf);
        m_pSharedBuf = nullptr;
    }
    if (m_hMapFile) {
        CloseHandle(m_hMapFile);
        m_hMapFile = nullptr;
    }
    if (m_hFrameEvent) {
        CloseHandle(m_hFrameEvent);
        m_hFrameEvent = nullptr;
    }
    if (m_hMutex) {
        CloseHandle(m_hMutex);
        m_hMutex = nullptr;
    }

    LeaveCriticalSection(&m_csPin);
    return S_OK;
}

DWORD WINAPI EyeCamPin::ThreadProcThunk(LPVOID lpParameter) {
    return static_cast<EyeCamPin*>(lpParameter)->ThreadProc();
}

DWORD EyeCamPin::ThreadProc() {
    REFERENCE_TIME avgTimePerFrame = 333333LL; // 30 FPS за замовчуванням
    if (m_mediaType.formattype == FORMAT_VideoInfo && m_mediaType.pbFormat) {
        VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)m_mediaType.pbFormat;
        if (vih->AvgTimePerFrame > 0) avgTimePerFrame = vih->AvgTimePerFrame;
    }

    REFERENCE_TIME streamTime = 0;
    std::vector<uint8_t> frameBuffer;

    while (true) {
        if (WaitForSingleObject(m_hStopEvent, 0) == WAIT_OBJECT_0) {
            break;
        }

        // Синхронізація з кадрами продюсера через Win32 Event
        HANDLE waitHandles[2] = { m_hStopEvent, m_hFrameEvent };
        DWORD waitCount = (m_hFrameEvent != nullptr) ? 2 : 1;
        DWORD dwWait = WaitForMultipleObjects(waitCount, waitHandles, FALSE, (DWORD)(avgTimePerFrame / 10000));

        if (dwWait == WAIT_OBJECT_0) {
            break; // Stop event
        }

        if (m_pInputPin == nullptr) {
            Sleep((DWORD)(avgTimePerFrame / 10000));
            continue;
        }

        // Отримання зразка пам'яті від Downstream Allocator (Zoom/OBS/Teams)
        IMediaSample* pSample = nullptr;
        HRESULT hr = S_OK;

        if (m_pAllocator) {
            hr = m_pAllocator->GetBuffer(&pSample, nullptr, nullptr, 0);
        } else {
            // Спроба отримати алокатор від підключеного вхідного піна
            hr = m_pInputPin->GetAllocator(&m_pAllocator);
            if (SUCCEEDED(hr) && m_pAllocator) {
                hr = m_pAllocator->GetBuffer(&pSample, nullptr, nullptr, 0);
            }
        }

        if (FAILED(hr) || !pSample) {
            Sleep(5);
            continue;
        }

        BYTE* pBuffer = nullptr;
        hr = pSample->GetPointer(&pBuffer);
        long bufSize = pSample->GetSize();

        if (SUCCEEDED(hr) && pBuffer && bufSize > 0) {
            LONGLONG frameTimestamp = 0;
            bool gotFrame = ReadSharedMemoryFrame(pBuffer, bufSize, &frameTimestamp);
            if (!gotFrame) {
                FillFallbackPattern(pBuffer, bufSize);
            }

            REFERENCE_TIME rtStart = streamTime;
            REFERENCE_TIME rtEnd = streamTime + avgTimePerFrame;
            streamTime = rtEnd;

            pSample->SetTime(&rtStart, &rtEnd);
            pSample->SetSyncPoint(TRUE);
            pSample->SetActualDataLength(bufSize);

            m_pInputPin->Receive(pSample);
        }

        pSample->Release();
    }

    return 0;
}

bool EyeCamPin::ReadSharedMemoryFrame(uint8_t* dest, size_t destSize, LONGLONG* pTimestampNs) {
    if (!m_pSharedBuf || !m_hMutex) return false;

    DWORD waitResult = WaitForSingleObject(m_hMutex, 5);
    if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) {
        return false;
    }

    const EyeCam::SharedMemoryHeader* header = reinterpret_cast<const EyeCam::SharedMemoryHeader*>(m_pSharedBuf);
    if (header->magic != EyeCam::SHM_MAGIC || header->state != EyeCam::StreamState::RUNNING) {
        ReleaseMutex(m_hMutex);
        return false;
    }

    uint32_t activeSlot = header->activeSlot;
    size_t slotOffset = sizeof(EyeCam::SharedMemoryHeader) + (static_cast<size_t>(activeSlot) * EyeCam::SINGLE_SLOT_SIZE);

    const EyeCam::FrameMetadata* meta = reinterpret_cast<const EyeCam::FrameMetadata*>(
        reinterpret_cast<const uint8_t*>(m_pSharedBuf) + slotOffset
    );

    const uint8_t* pFramePayload = reinterpret_cast<const uint8_t*>(meta) + sizeof(EyeCam::FrameMetadata);

    size_t copySize = min((size_t)meta->dataSize, destSize);
    memcpy(dest, pFramePayload, copySize);

    if (pTimestampNs) *pTimestampNs = meta->timestampNs;

    ReleaseMutex(m_hMutex);
    return true;
}

void EyeCamPin::FillFallbackPattern(uint8_t* dest, size_t destSize) {
    // Темно-сірий колір заглушки, якщо зв'язок з додатком ще не встановлено
    if (m_mediaType.subtype == MEDIASUBTYPE_YUY2) {
        for (size_t i = 0; i < destSize; i += 4) {
            dest[i + 0] = 0x30; // Y0
            dest[i + 1] = 0x80; // U
            dest[i + 2] = 0x30; // Y1
            dest[i + 3] = 0x80; // V
        }
    } else {
        memset(dest, 0x1E, destSize);
    }
}