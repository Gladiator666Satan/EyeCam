#include "EyeCamFilter.hpp"

#if defined(_WIN32)
#include <uuids.h>
#include <iostream>

#ifndef AMPROPERTY_PIN_CATEGORY
#define AMPROPERTY_PIN_CATEGORY 0
#endif
#ifndef KSPROPERTY_SUPPORT_GET
#define KSPROPERTY_SUPPORT_GET 1
#endif

namespace EyeCam {

// Допоміжний клас перелічувача пінів (IEnumPins)
class CEnumPins : public IEnumPins {
public:
    CEnumPins(IPin* pPin) : m_pPin(pPin) {
        if (m_pPin) m_pPin->AddRef();
    }
    virtual ~CEnumPins() {
        if (m_pPin) m_pPin->Release();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumPins) {
            *ppv = static_cast<IEnumPins*>(this);
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

    STDMETHODIMP Next(ULONG cPins, IPin** ppPins, ULONG* pcFetched) override {
        if (!ppPins) return E_POINTER;
        if (cPins > 0 && m_index == 0 && m_pPin) {
            ppPins[0] = m_pPin;
            m_pPin->AddRef();
            m_index++;
            if (pcFetched) *pcFetched = 1;
            return (cPins == 1) ? S_OK : S_FALSE;
        }
        if (pcFetched) *pcFetched = 0;
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
        *ppEnum = new CEnumPins(m_pPin);
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refCount{1};
    IPin* m_pPin = nullptr;
    ULONG m_index = 0;
};

// Допоміжні функції для роботи з форматами відео
static void FreeMediaType(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat != 0) {
        CoTaskMemFree(mt.pbFormat);
        mt.cbFormat = 0;
        mt.pbFormat = nullptr;
    }
    if (mt.pUnk != nullptr) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

static HRESULT CopyMediaType(AM_MEDIA_TYPE* pDest, const AM_MEDIA_TYPE* pSrc) {
    if (!pDest || !pSrc) return E_POINTER;
    *pDest = *pSrc;
    if (pSrc->cbFormat != 0 && pSrc->pbFormat != nullptr) {
        pDest->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(pSrc->cbFormat));
        if (!pDest->pbFormat) return E_OUTOFMEMORY;
        std::memcpy(pDest->pbFormat, pSrc->pbFormat, pSrc->cbFormat);
    }
    if (pDest->pUnk != nullptr) {
        pDest->pUnk->AddRef();
    }
    return S_OK;
}

// Пресети роздільних здатностей та FPS для виявлення вебкамерою
struct FormatPreset {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    GUID subtype;
};

static const FormatPreset g_Presets[] = {
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
constexpr int g_PresetCount = sizeof(g_Presets) / sizeof(g_Presets[0]);

// Клас перелічувача медіа-форматів (IEnumMediaTypes)
class CEnumMediaTypes : public IEnumMediaTypes {
public:
    CEnumMediaTypes(EyeCamOutputPin* pPin) : m_pPin(pPin) {}
    virtual ~CEnumMediaTypes() = default;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
            *ppv = static_cast<IEnumMediaTypes*>(this);
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

    STDMETHODIMP Next(ULONG cMediaTypes, AM_MEDIA_TYPE** ppMediaTypes, ULONG* pcFetched) override;
    STDMETHODIMP Skip(ULONG cMediaTypes) override {
        m_index += cMediaTypes;
        return (m_index <= g_PresetCount) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override {
        m_index = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumMediaTypes** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new CEnumMediaTypes(m_pPin);
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refCount{1};
    EyeCamOutputPin* pPin = nullptr;
    EyeCamOutputPin* m_pPin = nullptr;
    ULONG m_index = 0;
};

// ----------------------------------------------------------------------------
// EyeCamOutputPin Реалізація
// ----------------------------------------------------------------------------

EyeCamOutputPin::EyeCamOutputPin(EyeCamCaptureFilter* pFilter)
    : m_pFilter(pFilter) {
    CreateDefaultMediaType(&m_mediaType, 1920, 1080, PixelFormat::YUY2, 60);
    m_hasMediaType = true;
}

EyeCamOutputPin::~EyeCamOutputPin() {
    Inactive();
    FreeMediaType(m_mediaType);
}

STDMETHODIMP EyeCamOutputPin::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;

    if (riid == IID_IUnknown || riid == IID_IPin) {
        *ppv = static_cast<IPin*>(this);
    } else if (riid == IID_IKsPropertySet) {
        // Обов'язково для Zoom та Teams для запиту властивості PIN_CATEGORY_CAPTURE
        *ppv = static_cast<IKsPropertySet*>(this);
    } else if (riid == IID_IAMStreamConfig) {
        // Необхідно для узгодження роздільної здатності та частоти кадрів клієнтським додатком
        *ppv = static_cast<IAMStreamConfig*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) EyeCamOutputPin::AddRef() {
    return ++m_refCount;
}

STDMETHODIMP_(ULONG) EyeCamOutputPin::Release() {
    ULONG count = --m_refCount;
    if (count == 0) delete this;
    return count;
}

STDMETHODIMP EyeCamOutputPin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
    if (!pReceivePin) return E_POINTER;
    if (m_pConnectedPin) return VFW_E_ALREADY_CONNECTED;

    AM_MEDIA_TYPE mt{};
    if (pmt) {
        CopyMediaType(&mt, pmt);
    } else {
        CopyMediaType(&mt, &m_mediaType);
    }

    HRESULT hr = pReceivePin->ReceiveConnection(this, &mt);
    if (SUCCEEDED(hr)) {
        m_pConnectedPin = pReceivePin;
        m_pConnectedPin->AddRef();

        pReceivePin->QueryInterface(IID_IMemInputPin, reinterpret_cast<void**>(&m_pMemInputPin));
        if (m_pMemInputPin) {
            DecideAllocator(m_pMemInputPin, &m_pAllocator);
        }

        FreeMediaType(m_mediaType);
        CopyMediaType(&m_mediaType, &mt);
        m_hasMediaType = true;
    }

    FreeMediaType(mt);
    return hr;
}

STDMETHODIMP EyeCamOutputPin::ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) {
    (void)pConnector; (void)pmt;
    return E_UNEXPECTED; // Вихідний пін не приймає з'єднань
}

STDMETHODIMP EyeCamOutputPin::Disconnect() {
    if (!m_pConnectedPin) return S_FALSE;

    if (m_pAllocator) {
        m_pAllocator->Decommit();
        m_pAllocator->Release();
        m_pAllocator = nullptr;
    }
    if (m_pMemInputPin) {
        m_pMemInputPin->Release();
        m_pMemInputPin = nullptr;
    }

    m_pConnectedPin->Release();
    m_pConnectedPin = nullptr;
    return S_OK;
}

STDMETHODIMP EyeCamOutputPin::ConnectedTo(IPin** pPin) {
    if (!pPin) return E_POINTER;
    if (!m_pConnectedPin) return VFW_E_NOT_CONNECTED;
    *pPin = m_pConnectedPin;
    (*pPin)->AddRef();
    return S_OK;
}

STDMETHODIMP EyeCamOutputPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (!m_pConnectedPin) return VFW_E_NOT_CONNECTED;
    return CopyMediaType(pmt, &m_mediaType);
}

STDMETHODIMP EyeCamOutputPin::QueryPinInfo(PIN_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    pInfo->pFilter = m_pFilter;
    if (m_pFilter) m_pFilter->AddRef();
    pInfo->dir = PINDIR_OUTPUT;
    wcsncpy_s(pInfo->achName, L"Capture", _TRUNCATE);
    return S_OK;
}

STDMETHODIMP EyeCamOutputPin::QueryDirection(PIN_DIRECTION* pPinDir) {
    if (!pPinDir) return E_POINTER;
    *pPinDir = PINDIR_OUTPUT;
    return S_OK;
}

STDMETHODIMP EyeCamOutputPin::QueryId(LPWSTR* Id) {
    if (!Id) return E_POINTER;
    const wchar_t* pinId = L"Output";
    size_t len = (wcslen(pinId) + 1) * sizeof(wchar_t);
    *Id = static_cast<LPWSTR>(CoTaskMemAlloc(len));
    if (!*Id) return E_OUTOFMEMORY;
    std::memcpy(*Id, pinId, len);
    return S_OK;
}

STDMETHODIMP EyeCamOutputPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (pmt->majortype != MEDIATYPE_Video) return S_FALSE;
    if (pmt->subtype != MEDIASUBTYPE_YUY2 && 
        pmt->subtype != MEDIASUBTYPE_RGB24 && 
        pmt->subtype != MEDIASUBTYPE_RGB32) {
        return S_FALSE;
    }
    return S_OK;
}

STDMETHODIMP EyeCamOutputPin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new CEnumMediaTypes(this);
    return S_OK;
}

STDMETHODIMP EyeCamOutputPin::QueryInternalConnections(IPin** apPin, ULONG* nPin) {
    (void)apPin; (void)nPin;
    return E_NOTIMPL;
}

STDMETHODIMP EyeCamOutputPin::EndOfStream() { return S_OK; }
STDMETHODIMP EyeCamOutputPin::BeginFlush() { return S_OK; }
STDMETHODIMP EyeCamOutputPin::EndFlush() { return S_OK; }
STDMETHODIMP EyeCamOutputPin::NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) {
    (void)tStart; (void)tStop; (void)dRate;
    return S_OK;
}

// IKsPropertySet - Критично важливий блок для детекції в Zoom, Teams, Discord
STDMETHODIMP EyeCamOutputPin::Set(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, 
                                  DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData) {
    (void)guidPropSet; (void)dwPropID; (void)pInstanceData; (void)cbInstanceData; 
    (void)pPropData; (void)cbPropData;
    return E_NOTIMPL;
}

STDMETHODIMP EyeCamOutputPin::Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, 
                                  DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) {
    (void)pInstanceData; (void)cbInstanceData;
    if (guidPropSet == AMPROPSETID_Pin && dwPropID == AMPROPERTY_PIN_CATEGORY) {
        if (!pPropData) return E_POINTER;
        if (cbPropData < sizeof(GUID)) return E_FAIL;

        // Повертаємо PIN_CATEGORY_CAPTURE, щоб DirectShow класифікував фільтр як вебкамеру
        *reinterpret_cast<GUID*>(pPropData) = PIN_CATEGORY_CAPTURE;
        if (pcbReturned) *pcbReturned = sizeof(GUID);
        return S_OK;
    }
    return E_PROP_SET_UNSUPPORTED;
}

STDMETHODIMP EyeCamOutputPin::QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) {
    if (!pTypeSupport) return E_POINTER;
    if (guidPropSet == AMPROPSETID_Pin && dwPropID == AMPROPERTY_PIN_CATEGORY) {
        *pTypeSupport = KSPROPERTY_SUPPORT_GET;
        return S_OK;
    }
    return E_PROP_SET_UNSUPPORTED;
}

// IAMStreamConfig
STDMETHODIMP EyeCamOutputPin::SetFormat(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (QueryAccept(pmt) != S_OK) return VFW_E_INVALIDMEDIATYPE;

    FreeMediaType(m_mediaType);
    CopyMediaType(&m_mediaType, pmt);
    m_hasMediaType = true;
    return S_OK;
}

STDMETHODIMP EyeCamOutputPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    if (!ppmt) return E_POINTER;
    *ppmt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!*ppmt) return E_OUTOFMEMORY;
    return CopyMediaType(*ppmt, &m_mediaType);
}

STDMETHODIMP EyeCamOutputPin::GetNumberOfCapabilities(int* piCount, int* piSize) {
    if (!piCount || !piSize) return E_POINTER;
    *piCount = g_PresetCount;
    *piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP EyeCamOutputPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
    if (!ppmt || !pSCC) return E_POINTER;
    if (iIndex < 0 || iIndex >= g_PresetCount) return E_INVALIDARG;

    const auto& preset = g_Presets[iIndex];
    *ppmt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!*ppmt) return E_OUTOFMEMORY;

    PixelFormat fmt = (preset.subtype == MEDIASUBTYPE_YUY2) ? PixelFormat::YUY2 : PixelFormat::RGB24;
    CreateDefaultMediaType(*ppmt, preset.width, preset.height, fmt, preset.fps);

    // Заповнення VIDEO_STREAM_CONFIG_CAPS
    auto* caps = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(pSCC);
    std::memset(caps, 0, sizeof(VIDEO_STREAM_CONFIG_CAPS));
    caps->guid = FORMAT_VideoInfo;
    caps->VideoStandard = 0;
    caps->InputSize.cx = preset.width;
    caps->InputSize.cy = preset.height;
    caps->MinCroppingSize.cx = preset.width;
    caps->MinCroppingSize.cy = preset.height;
    caps->MaxCroppingSize.cx = preset.width;
    caps->MaxCroppingSize.cy = preset.height;
    caps->MinOutputSize = caps->MinCroppingSize;
    caps->MaxOutputSize = caps->MaxCroppingSize;
    caps->MinFrameInterval = 10000000LL / preset.fps;
    caps->MaxFrameInterval = 10000000LL / 10; // До 10 fps мінімум

    return S_OK;
}

HRESULT EyeCamOutputPin::CreateDefaultMediaType(AM_MEDIA_TYPE* pmt, uint32_t width, uint32_t height, 
                                                PixelFormat fmt, uint32_t fps) {
    std::memset(pmt, 0, sizeof(AM_MEDIA_TYPE));
    pmt->majortype = MEDIATYPE_Video;
    pmt->subtype = (fmt == PixelFormat::YUY2) ? MEDIASUBTYPE_YUY2 : MEDIASUBTYPE_RGB24;
    pmt->bFixedSizeSamples = TRUE;
    pmt->bTemporalCompression = FALSE;
    pmt->lSampleSize = static_cast<ULONG>(CalculateFrameSize(width, height, fmt));
    pmt->formattype = FORMAT_VideoInfo;

    VIDEOINFOHEADER* vih = static_cast<VIDEOINFOHEADER*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
    if (!vih) return E_OUTOFMEMORY;

    std::memset(vih, 0, sizeof(VIDEOINFOHEADER));
    vih->rcSource = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    vih->rcTarget = vih->rcSource;
    vih->AvgTimePerFrame = (fps > 0) ? (10000000LL / fps) : 333333LL;

    BITMAPINFOHEADER& bih = vih->bmiHeader;
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = width;
    bih.biHeight = height; // Додатнє значення для верхнього рядка першим у YUY2
    bih.biPlanes = 1;
    bih.biBitCount = (fmt == PixelFormat::YUY2) ? 16 : 24;
    bih.biCompression = (fmt == PixelFormat::YUY2) ? MAKEFOURCC('Y','U','Y','2') : BI_RGB;
    bih.biSizeImage = static_cast<DWORD>(pmt->lSampleSize);

    pmt->cbFormat = sizeof(VIDEOINFOHEADER);
    pmt->pbFormat = reinterpret_cast<BYTE*>(vih);
    return S_OK;
}

HRESULT EyeCamOutputPin::DecideAllocator(IMemInputPin* pPin, IMemAllocator** ppAlloc) {
    if (!pPin || !ppAlloc) return E_POINTER;

    HRESULT hr = pPin->GetAllocator(ppAlloc);
    if (FAILED(hr)) {
        hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, 
                              IID_IMemAllocator, reinterpret_cast<void**>(ppAlloc));
        if (FAILED(hr)) return hr;
    }

    ALLOCATOR_PROPERTIES propReq{}, propAct{};
    propReq.cBuffers = 3;
    propReq.cbBuffer = m_mediaType.lSampleSize;
    propReq.cbAlign = 1;
    propReq.cbPrefix = 0;

    hr = (*ppAlloc)->SetProperties(&propReq, &propAct);
    if (SUCCEEDED(hr)) {
        hr = pPin->NotifyAllocator(*ppAlloc, FALSE);
    }
    return hr;
}

HRESULT EyeCamOutputPin::Active() {
    if (m_workerThread.joinable()) {
        return S_OK;
    }

    // Підключення до спільної пам'яті (спершу Global\, потім Local\)
    m_hMapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, SHM_NAME_GLOBAL);
    if (!m_hMapFile) {
        m_hMapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, SHM_NAME_LOCAL);
    }
    if (m_hMapFile) {
        m_pSharedBuf = MapViewOfFile(m_hMapFile, FILE_MAP_READ, 0, 0, TOTAL_SHARED_MEMORY_SIZE);
    }

    m_hFrameEvent = OpenEventW(SYNCHRONIZE, FALSE, EVENT_FRAME_GLOBAL);
    if (!m_hFrameEvent) {
        m_hFrameEvent = OpenEventW(SYNCHRONIZE, FALSE, EVENT_FRAME_LOCAL);
    }

    m_hMutex = OpenMutexW(SYNCHRONIZE, FALSE, MUTEX_NAME_GLOBAL);
    if (!m_hMutex) {
        m_hMutex = OpenMutexW(SYNCHRONIZE, FALSE, MUTEX_NAME_LOCAL);
    }

    if (m_pAllocator) {
        m_pAllocator->Commit();
    }

    m_stopThread = false;
    m_workerThread = std::thread(&EyeCamOutputPin::ThreadProc, this);
    return S_OK;
}

HRESULT EyeCamOutputPin::Inactive() {
    m_stopThread = true;
    if (m_hFrameEvent) {
        SetEvent(m_hFrameEvent); // Пробуджуємо робочий потік для зупинки
    }

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    if (m_pAllocator) {
        m_pAllocator->Decommit();
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

    return S_OK;
}

void EyeCamOutputPin::ThreadProc() {
    REFERENCE_TIME avgFrameDuration = 333333LL; // За замовчуванням 30 FPS (~33.3 мс)
    if (m_mediaType.formattype == FORMAT_VideoInfo && m_mediaType.pbFormat) {
        auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(m_mediaType.pbFormat);
        if (vih->AvgTimePerFrame > 0) {
            avgFrameDuration = vih->AvgTimePerFrame;
        }
    }

    DWORD waitTimeoutMs = static_cast<DWORD>(avgFrameDuration / 10000); // 100-нс в мс

    while (!m_stopThread) {
        // Очікування нового кадру від продюсера через спільну подію або таймаут таймінгу
        if (m_hFrameEvent) {
            WaitForSingleObject(m_hFrameEvent, waitTimeoutMs);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(waitTimeoutMs));
        }

        if (m_stopThread) break;

        if (!m_pAllocator || !m_pMemInputPin) {
            continue;
        }

        IMediaSample* pSample = nullptr;
        HRESULT hr = m_pAllocator->GetBuffer(&pSample, nullptr, nullptr, 0);
        if (FAILED(hr) || !pSample) {
            continue;
        }

        BYTE* pBuffer = nullptr;
        hr = pSample->GetPointer(&pBuffer);
        if (SUCCEEDED(hr) && pBuffer) {
            long bufferSize = pSample->GetSize();
            FillBuffer(pBuffer, static_cast<size_t>(bufferSize));
        }

        REFERENCE_TIME rtStart = m_rtFrameTime;
        REFERENCE_TIME rtEnd = rtStart + avgFrameDuration;
        m_rtFrameTime = rtEnd;

        pSample->SetTime(&rtStart, &rtEnd);
        pSample->SetSyncPoint(TRUE);

        m_pMemInputPin->Receive(pSample);
        pSample->Release();
    }
}

void EyeCamOutputPin::FillBuffer(BYTE* pBuffer, size_t bufferSize) {
    bool frameRendered = false;

    // Якщо клієнт підключений до спільної пам'яті
    if (m_pSharedBuf) {
        const uint32_t magic = *reinterpret_cast<const uint32_t*>(m_pSharedBuf);

        // Формат 1: Кільцевий трипл-буфер VirtualCameraSink (0x4543414D 'ECAM')
        if (magic == MAGIC_ECAM_TRIPLE) {
            auto* th = reinterpret_cast<const TripleBufferShmHeader*>(m_pSharedBuf);
            uint32_t activeIdx = th->activeBufferIdx % 3;
            size_t frameSize = CalculateFrameSize(th->width, th->height, PixelFormat::NV12);
            const uint8_t* base = reinterpret_cast<const uint8_t*>(th + 1);
            const uint8_t* src = base + (activeIdx * frameSize);

            // Якщо джерело NV12, а клієнтський додаток (Zoom/Teams) очікує YUY2:
            if (m_mediaType.subtype == MEDIASUBTYPE_YUY2 && th->fourcc == FOURCC_NV12) {
                const uint32_t w = th->width;
                const uint32_t h = th->height;
                const uint8_t* yPlane = src;
                const uint8_t* uvPlane = src + (w * h);

                for (uint32_t y = 0; y < h; ++y) {
                    const uint8_t* yRow = yPlane + (y * w);
                    const uint8_t* uvRow = uvPlane + ((y / 2) * w);
                    BYTE* outRow = pBuffer + (y * w * 2);

                    for (uint32_t x = 0; x < w; x += 2) {
                        outRow[x * 2]     = yRow[x];       // Y0
                        outRow[x * 2 + 1] = uvRow[x];      // U
                        outRow[x * 2 + 2] = yRow[x + 1];   // Y1
                        outRow[x * 2 + 3] = uvRow[x + 1];  // V
                    }
                }
                frameRendered = true;
            } else {
                size_t copyBytes = (frameSize < bufferSize) ? frameSize : bufferSize;
                std::memcpy(pBuffer, src, copyBytes);
                frameRendered = true;
            }
        }
        // Формат 2: Прямий кадр WindowsDirectShowSink (0x45594543 'EYEC')
        else if (magic == EYECAM_MAGIC && m_hMutex) {
            DWORD waitRes = WaitForSingleObject(m_hMutex, 20);
            if (waitRes == WAIT_OBJECT_0) {
                auto* header = reinterpret_cast<const SharedMemoryHeader*>(m_pSharedBuf);
                if (header->isConnected == 1 && header->dataSize > 0) {
                    const uint8_t* srcData = reinterpret_cast<const uint8_t*>(m_pSharedBuf) + sizeof(SharedMemoryHeader);
                    size_t copyBytes = (header->dataSize < bufferSize) ? header->dataSize : bufferSize;
                    std::memcpy(pBuffer, srcData, copyBytes);
                    frameRendered = true;
                }
                ReleaseMutex(m_hMutex);
            }
        }
    }

    // Якщо стрім від продюсера ще не активний - відображаємо тестовий патерн (сірий фон для вебкамери)
    if (!frameRendered) {
        if (m_mediaType.subtype == MEDIASUBTYPE_YUY2) {
            // YUY2 шаблон сірого: Y=128 (0x80), U=128 (0x80), V=128 (0x80)
            for (size_t i = 0; i + 3 < bufferSize; i += 4) {
                pBuffer[i]     = 128; // Y0
                pBuffer[i + 1] = 128; // U
                pBuffer[i + 2] = 128; // Y1
                pBuffer[i + 3] = 128; // V
            }
        } else {
            // RGB24: темно-сірий фон
            std::memset(pBuffer, 40, bufferSize);
        }
    }
}

// ----------------------------------------------------------------------------
// CEnumMediaTypes Реалізація
// ----------------------------------------------------------------------------

STDMETHODIMP CEnumMediaTypes::Next(ULONG cMediaTypes, AM_MEDIA_TYPE** ppMediaTypes, ULONG* pcFetched) {
    if (!ppMediaTypes) return E_POINTER;
    ULONG count = 0;

    while (count < cMediaTypes && m_index < static_cast<ULONG>(g_PresetCount)) {
        ppMediaTypes[count] = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
        if (!ppMediaTypes[count]) break;

        const auto& preset = g_Presets[m_index];
        PixelFormat fmt = (preset.subtype == MEDIASUBTYPE_YUY2) ? PixelFormat::YUY2 : PixelFormat::RGB24;
        
        // Використовуємо метод створення типу піна
        AM_MEDIA_TYPE tmp{};
        // Виклик CreateDefaultMediaType через збережений пін або локально
        std::memset(&tmp, 0, sizeof(AM_MEDIA_TYPE));
        tmp.majortype = MEDIATYPE_Video;
        tmp.subtype = preset.subtype;
        tmp.bFixedSizeSamples = TRUE;
        tmp.bTemporalCompression = FALSE;
        tmp.lSampleSize = static_cast<ULONG>(CalculateFrameSize(preset.width, preset.height, fmt));
        tmp.formattype = FORMAT_VideoInfo;

        auto* vih = static_cast<VIDEOINFOHEADER*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
        if (vih) {
            std::memset(vih, 0, sizeof(VIDEOINFOHEADER));
            vih->rcSource = {0, 0, static_cast<LONG>(preset.width), static_cast<LONG>(preset.height)};
            vih->rcTarget = vih->rcSource;
            vih->AvgTimePerFrame = 10000000LL / preset.fps;

            BITMAPINFOHEADER& bih = vih->bmiHeader;
            bih.biSize = sizeof(BITMAPINFOHEADER);
            bih.biWidth = preset.width;
            bih.biHeight = preset.height;
            bih.biPlanes = 1;
            bih.biBitCount = (fmt == PixelFormat::YUY2) ? 16 : 24;
            bih.biCompression = (fmt == PixelFormat::YUY2) ? MAKEFOURCC('Y','U','Y','2') : BI_RGB;
            bih.biSizeImage = tmp.lSampleSize;

            tmp.cbFormat = sizeof(VIDEOINFOHEADER);
            tmp.pbFormat = reinterpret_cast<BYTE*>(vih);
        }

        *ppMediaTypes[count] = tmp;
        count++;
        m_index++;
    }

    if (pcFetched) *pcFetched = count;
    return (count == cMediaTypes) ? S_OK : S_FALSE;
}

// ----------------------------------------------------------------------------
// EyeCamCaptureFilter Реалізація
// ----------------------------------------------------------------------------

EyeCamCaptureFilter::EyeCamCaptureFilter() {
    m_pPin = new EyeCamOutputPin(this);
}

EyeCamCaptureFilter::~EyeCamCaptureFilter() {
    if (m_pPin) {
        m_pPin->Release();
        m_pPin = nullptr;
    }
    if (m_pClock) {
        m_pClock->Release();
        m_pClock = nullptr;
    }
}

STDMETHODIMP EyeCamCaptureFilter::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;

    if (riid == IID_IUnknown || riid == IID_IBaseFilter || riid == IID_IMediaFilter) {
        *ppv = static_cast<IBaseFilter*>(this);
    } else if (riid == IID_IPersist) {
        *ppv = static_cast<IPersist*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) EyeCamCaptureFilter::AddRef() {
    return ++m_refCount;
}

STDMETHODIMP_(ULONG) EyeCamCaptureFilter::Release() {
    ULONG count = --m_refCount;
    if (count == 0) delete this;
    return count;
}

STDMETHODIMP EyeCamCaptureFilter::GetClassID(CLSID* pClassID) {
    if (!pClassID) return E_POINTER;
    *pClassID = CLSID_EyeCamFilter;
    return S_OK;
}

STDMETHODIMP EyeCamCaptureFilter::Stop() {
    std::lock_guard<std::mutex> lock(m_filterLock);
    m_state = State_Stopped;
    if (m_pPin) {
        m_pPin->Inactive();
    }
    return S_OK;
}

STDMETHODIMP EyeCamCaptureFilter::Pause() {
    std::lock_guard<std::mutex> lock(m_filterLock);
    m_state = State_Paused;
    if (m_pPin) {
        m_pPin->Active();
    }
    return S_OK;
}

STDMETHODIMP EyeCamCaptureFilter::Run(REFERENCE_TIME tStart) {
    (void)tStart;
    std::lock_guard<std::mutex> lock(m_filterLock);
    m_state = State_Running;
    if (m_pPin) {
        m_pPin->Active();
    }
    return S_OK;
}

STDMETHODIMP EyeCamCaptureFilter::GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) {
    (void)dwMilliSecsTimeout;
    if (!State) return E_POINTER;
    *State = m_state;
    return S_OK;
}

STDMETHODIMP EyeCamCaptureFilter::SetSyncSource(IReferenceClock* pClock) {
    std::lock_guard<std::mutex> lock(m_filterLock);
    if (m_pClock) m_pClock->Release();
    m_pClock = pClock;
    if (m_pClock) m_pClock->AddRef();
    return S_OK;
}

STDMETHODIMP EyeCamCaptureFilter::GetSyncSource(IReferenceClock** pClock) {
    if (!pClock) return E_POINTER;
    *pClock = m_pClock;
    if (*pClock) (*pClock)->AddRef();
    return S_OK;
}

STDMETHODIMP EyeCamCaptureFilter::EnumPins(IEnumPins** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new CEnumPins(m_pPin);
    return S_OK;
}

STDMETHODIMP EyeCamCaptureFilter::FindPin(LPCWSTR Id, IPin** ppPin) {
    if (!ppPin) return E_POINTER;
    if (Id && wcscmp(Id, L"Output") == 0 && m_pPin) {
        *ppPin = m_pPin;
        (*ppPin)->AddRef();
        return S_OK;
    }
    *ppPin = nullptr;
    return VFW_E_NOT_FOUND;
}

STDMETHODIMP EyeCamCaptureFilter::QueryFilterInfo(FILTER_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    pInfo->pGraph = m_pGraph;
    if (m_pGraph) m_pGraph->AddRef();
    wcsncpy_s(pInfo->achName, m_filterName.c_str(), _TRUNCATE);
    return S_OK;
}

STDMETHODIMP EyeCamCaptureFilter::JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) {
    std::lock_guard<std::mutex> lock(m_filterLock);
    m_pGraph = pGraph; // Без AddRef для запобігання циклічним посиланням у DirectShow
    m_filterName = (pName != nullptr) ? pName : L"";
    return S_OK;
}

STDMETHODIMP EyeCamCaptureFilter::QueryVendorInfo(LPWSTR* pVendorInfo) {
    if (!pVendorInfo) return E_POINTER;
    const wchar_t* vendor = L"EyeCam Virtual Devices";
    size_t len = (wcslen(vendor) + 1) * sizeof(wchar_t);
    *pVendorInfo = static_cast<LPWSTR>(CoTaskMemAlloc(len));
    if (!*pVendorInfo) return E_OUTOFMEMORY;
    std::memcpy(*pVendorInfo, vendor, len);
    return S_OK;
}

} // namespace EyeCam
#endif
