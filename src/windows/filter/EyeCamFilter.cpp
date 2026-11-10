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
    explicit CEnumPins(EyeCamOutputPin* pPin) : m_pPin(pPin) {
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

    STDMETHODIMP_(ULONG) AddRef() override {
        return ++m_refCount;
    }

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
            m_index = 1;
            if (pcFetched) *pcFetched = 1;
            return (cPins == 1) ? S_OK : S_FALSE;
        }
        if (pcFetched) *pcFetched = 0;
        return S_FALSE;
    }

    STDMETHODIMP Skip(ULONG cPins) override {
        m_index += cPins;
        return S_OK;
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
    EyeCamOutputPin* m_pPin = nullptr;
    ULONG m_index = 0;
};

// ==============================================================================
// Реалізація EyeCamOutputPin
// ==============================================================================

EyeCamOutputPin::EyeCamOutputPin(EyeCamCaptureFilter* pFilter, CCritSec* pLock, HRESULT* phr)
    : CBaseOutputPin(NAME("EyeCamOutputPin"), pFilter, pLock, phr, L"Output"),
      m_pFilter(pFilter) {
    InitPresets();
    m_hStopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    OpenSharedMemory();
}

EyeCamOutputPin::~EyeCamOutputPin() {
    CloseSharedMemory();
    if (m_hStopEvent) {
        CloseHandle(m_hStopEvent);
        m_hStopEvent = nullptr;
    }
}

STDMETHODIMP EyeCamOutputPin::NonDelegatingQueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;

    if (riid == _uuidof(IKsPropertySet)) {
        return GetInterface(static_cast<IKsPropertySet*>(this), ppv);
    }
    if (riid == _uuidof(IAMStreamConfig)) {
        return GetInterface(static_cast<IAMStreamConfig*>(this), ppv);
    }

    return CBaseOutputPin::NonDelegatingQueryInterface(riid, ppv);
}

void EyeCamOutputPin::InitPresets() {
    m_presets.clear();

    // 4K UHD (3840x2160) @ 60 FPS
    m_presets.push_back({3840, 2160, 60, PixelFormat::NV12});
    m_presets.push_back({3840, 2160, 60, PixelFormat::YUY2});

    // 1080p FHD (1920x1080) @ 60 FPS (Основний режим за замовчуванням)
    m_presets.push_back({1920, 1080, 60, PixelFormat::NV12});
    m_presets.push_back({1920, 1080, 60, PixelFormat::YUY2});

    // 720p HD (1280x720) @ 60 FPS
    m_presets.push_back({1280, 720, 60, PixelFormat::NV12});
    m_presets.push_back({1280, 720, 60, PixelFormat::YUY2});
}

HRESULT EyeCamOutputPin::GetMediaType(int iPosition, CMediaType* pMediaType) {
    CAutoLock cAutoLock(m_pLock);
    if (!pMediaType) return E_POINTER;
    if (iPosition < 0) return E_INVALIDARG;

    if (static_cast<size_t>(iPosition) >= m_presets.size()) {
        return VFW_S_NO_MORE_ITEMS;
    }

    const auto& preset = m_presets[iPosition];
    pMediaType->SetType(&MEDIATYPE_Video);

    if (preset.format == PixelFormat::NV12) {
        pMediaType->SetSubtype(&MEDIASUBTYPE_NV12);
    } else if (preset.format == PixelFormat::YUY2) {
        pMediaType->SetSubtype(&MEDIASUBTYPE_YUY2);
    } else {
        pMediaType->SetSubtype(&MEDIASUBTYPE_RGB24);
    }

    pMediaType->SetFormatType(&FORMAT_VideoInfo);
    pMediaType->SetTemporalCompression(FALSE);

    VIDEOINFOHEADER* pvi = reinterpret_cast<VIDEOINFOHEADER*>(
        pMediaType->AllocFormatBuffer(sizeof(VIDEOINFOHEADER)));
    if (!pvi) return E_OUTOFMEMORY;

    ZeroMemory(pvi, sizeof(VIDEOINFOHEADER));
    pvi->rcSource = {0, 0, static_cast<LONG>(preset.width), static_cast<LONG>(preset.height)};
    pvi->rcTarget = pvi->rcSource;
    pvi->AvgTimePerFrame = 10000000LL / preset.fps; // 100-ns юніти

    BITMAPINFOHEADER& bih = pvi->bmiHeader;
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = preset.width;
    bih.biHeight = preset.height;
    bih.biPlanes = 1;

    if (preset.format == PixelFormat::NV12) {
        bih.biBitCount = 12;
        bih.biCompression = MAKEFOURCC('N', 'V', '1', '2');
        bih.biSizeImage = preset.width * preset.height * 3 / 2;
    } else if (preset.format == PixelFormat::YUY2) {
        bih.biBitCount = 16;
        bih.biCompression = MAKEFOURCC('Y', 'U', 'Y', '2');
        bih.biSizeImage = preset.width * preset.height * 2;
    } else {
        bih.biBitCount = 24;
        bih.biCompression = BI_RGB;
        bih.biSizeImage = preset.width * preset.height * 3;
    }

    pMediaType->SetSampleSize(bih.biSizeImage);
    return S_OK;
}

HRESULT EyeCamOutputPin::CheckMediaType(const CMediaType* pMediaType) {
    CAutoLock cAutoLock(m_pLock);
    if (!pMediaType) return E_POINTER;

    if (*pMediaType->Type() != MEDIATYPE_Video) {
        return VFW_E_INVALIDMEDIATYPE;
    }

    const GUID* subtype = pMediaType->Subtype();
    if (*subtype != MEDIASUBTYPE_NV12 && 
        *subtype != MEDIASUBTYPE_YUY2 && 
        *subtype != MEDIASUBTYPE_RGB24) {
        return VFW_E_INVALIDMEDIATYPE;
    }

    if (*pMediaType->FormatType() != FORMAT_VideoInfo) {
        return VFW_E_INVALIDMEDIATYPE;
    }

    VIDEOINFOHEADER* pvi = reinterpret_cast<VIDEOINFOHEADER*>(pMediaType->Format());
    if (!pvi) return E_POINTER;

    return S_OK;
}

HRESULT EyeCamOutputPin::DecideBufferSize(IMemAllocator* pAlloc, ALLOCATOR_PROPERTIES* ppropInputRequest) {
    CAutoLock cAutoLock(m_pLock);
    if (!pAlloc || !ppropInputRequest) return E_POINTER;

    HRESULT hr = S_OK;
    VIDEOINFOHEADER* pvi = reinterpret_cast<VIDEOINFOHEADER*>(m_mt.Format());
    if (!pvi) return E_UNEXPECTED;

    ppropInputRequest->cBuffers = 3; // Triple Buffering у DirectShow графі
    ppropInputRequest->cbBuffer = pvi->bmiHeader.biSizeImage;
    if (ppropInputRequest->cbBuffer == 0) {
        ppropInputRequest->cbBuffer = pvi->bmiHeader.biWidth * pvi->bmiHeader.biHeight * 2;
    }

    ALLOCATOR_PROPERTIES actual;
    hr = pAlloc->SetProperties(ppropInputRequest, &actual);
    if (FAILED(hr)) return hr;

    if (actual.cbBuffer < ppropInputRequest->cbBuffer) {
        return E_FAIL;
    }

    return S_OK;
}

// ------------------------------------------------------------------------------
// IKsPropertySet (Позначення піна як камери для Skype, Teams, Chrome, OBS)
// ------------------------------------------------------------------------------

STDMETHODIMP EyeCamOutputPin::Set(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, 
                                  DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData) {
    (void)guidPropSet; (void)dwPropID; (void)pInstanceData; 
    (void)cbInstanceData; (void)pPropData; (void)cbPropData;
    return E_NOTIMPL;
}

STDMETHODIMP EyeCamOutputPin::Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, 
                                  DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) {
    (void)pInstanceData; (void)cbInstanceData;
    if (guidPropSet == AMPROPSETID_Pin && dwPropID == AMPROPERTY_PIN_CATEGORY) {
        if (!pPropData) return E_POINTER;
        if (cbPropData < sizeof(GUID)) return E_FAIL;

        *static_cast<GUID*>(pPropData) = PIN_CATEGORY_CAPTURE;
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

// ------------------------------------------------------------------------------
// IAMStreamConfig
// ------------------------------------------------------------------------------

STDMETHODIMP EyeCamOutputPin::SetFormat(AM_MEDIA_TYPE* pmt) {
    CAutoLock cAutoLock(m_pLock);
    if (!pmt) return E_POINTER;

    if (m_pFilter->IsActive()) {
        return VFW_E_WRONG_STATE;
    }

    HRESULT hr = CheckMediaType(reinterpret_cast<CMediaType*>(pmt));
    if (FAILED(hr)) return hr;

    m_mt = *pmt;
    if (m_Connected) {
        m_pFilter->ReconnectPin(this, pmt);
    }
    return S_OK;
}

STDMETHODIMP EyeCamOutputPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    CAutoLock cAutoLock(m_pLock);
    if (!ppmt) return E_POINTER;

    *ppmt = CreateMediaType(&m_mt);
    return (*ppmt) ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP EyeCamOutputPin::GetNumberOfCapabilities(int* piCount, int* piSize) {
    if (!piCount || !piSize) return E_POINTER;
    *piCount = static_cast<int>(m_presets.size());
    *piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP EyeCamOutputPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
    CAutoLock cAutoLock(m_pLock);
    if (!ppmt || !pSCC) return E_POINTER;
    if (iIndex < 0 || static_cast<size_t>(iIndex) >= m_presets.size()) return E_INVALIDARG;

    CMediaType mt;
    HRESULT hr = GetMediaType(iIndex, &mt);
    if (FAILED(hr)) return hr;

    *ppmt = CreateMediaType(&mt);
    if (!*ppmt) return E_OUTOFMEMORY;

    const auto& preset = m_presets[iIndex];
    VIDEO_STREAM_CONFIG_CAPS* pCaps = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(pSCC);
    ZeroMemory(pCaps, sizeof(VIDEO_STREAM_CONFIG_CAPS));

    pCaps->guid = FORMAT_VideoInfo;
    pCaps->VideoStandard = 0;
    pCaps->InputSize = {static_cast<LONG>(preset.width), static_cast<LONG>(preset.height)};
    pCaps->MinCroppingSize = pCaps->InputSize;
    pCaps->MaxCroppingSize = pCaps->InputSize;
    pCaps->MinOutputSize = pCaps->InputSize;
    pCaps->MaxOutputSize = pCaps->InputSize;
    pCaps->MinFrameInterval = 10000000LL / preset.fps;
    pCaps->MaxFrameInterval = 10000000LL / 15;

    return S_OK;
}

// ------------------------------------------------------------------------------
// Робота зі спільною пам'яттю EyeCam Shared Memory
// ------------------------------------------------------------------------------

bool EyeCamOutputPin::OpenSharedMemory() {
    if (m_pSharedBuf) return true;

    m_hMapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, SHM_NAME_GLOBAL);
    if (!m_hMapFile) {
        m_hMapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, SHM_NAME_LOCAL);
    }

    if (!m_hMapFile) return false;

    m_pSharedBuf = MapViewOfFile(m_hMapFile, FILE_MAP_READ, 0, 0, 0);
    if (!m_pSharedBuf) {
        CloseHandle(m_hMapFile);
        m_hMapFile = nullptr;
        return false;
    }

    m_hFrameEvent = OpenEventW(SYNCHRONIZE, FALSE, EVENT_FRAME_GLOBAL);
    if (!m_hFrameEvent) {
        m_hFrameEvent = OpenEventW(SYNCHRONIZE, FALSE, EVENT_FRAME_LOCAL);
    }

    m_hMutex = OpenMutexW(SYNCHRONIZE, FALSE, MUTEX_NAME_GLOBAL);
    if (!m_hMutex) {
        m_hMutex = OpenMutexW(SYNCHRONIZE, FALSE, MUTEX_NAME_LOCAL);
    }

    return true;
}

void EyeCamOutputPin::CloseSharedMemory() {
    if (m_hMutex) {
        CloseHandle(m_hMutex);
        m_hMutex = nullptr;
    }
    if (m_hFrameEvent) {
        CloseHandle(m_hFrameEvent);
        m_hFrameEvent = nullptr;
    }
    if (m_pSharedBuf) {
        UnmapViewOfFile(m_pSharedBuf);
        m_pSharedBuf = nullptr;
    }
    if (m_hMapFile) {
        CloseHandle(m_hMapFile);
        m_hMapFile = nullptr;
    }
}

HRESULT EyeCamOutputPin::Active() {
    CAutoLock cAutoLock(m_pLock);
    HRESULT hr = CBaseOutputPin::Active();
    if (FAILED(hr)) return hr;

    ResetEvent(m_hStopEvent);
    OpenSharedMemory();

    // Запуск фонового потоку генерації та виштовхування кадрів DirectShow
    m_workerThread = std::thread(&EyeCamOutputPin::ThreadProc, this);
    return S_OK;
}

HRESULT EyeCamOutputPin::Inactive() {
    CAutoLock cAutoLock(m_pLock);
    SetEvent(m_hStopEvent);

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    CloseSharedMemory();
    return CBaseOutputPin::Inactive();
}

void EyeCamOutputPin::ThreadProc() {
    REFERENCE_TIME rtStart = 0;
    const REFERENCE_TIME rtAvg = 10000000LL / 60; // 60 FPS (16.6ms)

    HANDLE waitHandles[2] = { m_hStopEvent, m_hFrameEvent };

    while (true) {
        DWORD waitRes = WaitForMultipleObjects(
            (m_hFrameEvent ? 2 : 1), 
            waitHandles, 
            FALSE, 
            16 // 16ms таймаут для стабільної частоти 60 FPS
        );

        if (waitRes == WAIT_OBJECT_0) {
            // Зупинка потоку через StopEvent
            break;
        }

        IMediaSample* pSample = nullptr;
        HRESULT hr = GetDeliveryBuffer(&pSample, nullptr, nullptr, 0);
        if (FAILED(hr) || !pSample) {
            continue;
        }

        BYTE* pBuffer = nullptr;
        hr = pSample->GetPointer(&pBuffer);
        long actualSize = pSample->GetSize();

        if (SUCCEEDED(hr) && pBuffer && actualSize > 0) {
            FillSampleBuffer(pBuffer, static_cast<size_t>(actualSize));

            REFERENCE_TIME rtEnd = rtStart + rtAvg;
            pSample->SetTime(&rtStart, &rtEnd);
            pSample->SetSyncPoint(TRUE);
            pSample->SetPreroll(FALSE);

            Deliver(pSample);
            rtStart = rtEnd;
        }

        if (pSample) {
            pSample->Release();
        }
    }
}

void EyeCamOutputPin::FillSampleBuffer(BYTE* pBuffer, size_t bufferSize) {
    if (!m_pSharedBuf) {
        OpenSharedMemory();
    }

    bool frameRendered = false;

    if (m_pSharedBuf) {
        const uint32_t magic = *reinterpret_cast<const uint32_t*>(m_pSharedBuf);

        // Формат 1: Triple Buffer (Zero-Lock)
        if (magic == MAGIC_ECAM_TRIPLE) {
            const auto* header = reinterpret_cast<const TripleBufferShmHeader*>(m_pSharedBuf);
            uint32_t slot = header->activeBufferIdx % 3;

            const uint8_t* slotBase = reinterpret_cast<const uint8_t*>(m_pSharedBuf) + sizeof(TripleBufferShmHeader);
            const uint8_t* src = slotBase + (slot * MAX_FRAME_BUFFER_SIZE);

            uint32_t w = header->width;
            uint32_t h = header->height;
            size_t frameSize = (header->fourcc == FOURCC_NV12) ? (w * h * 3 / 2) : (w * h * 2);

            // Якщо формати збігаються, виконуємо копіювання
            if (m_mediaType.subtype == MEDIASUBTYPE_YUY2 && header->fourcc == FOURCC_NV12) {
                // Швидка асемблерна або прямолінійна конвертація NV12 -> YUY2
                const uint8_t* yPlane = src;
                const uint8_t* uvPlane = src + (w * h);

                for (uint32_t y = 0; y < h; ++y) {
                    const uint8_t* yRow = yPlane + (y * w);
                    const uint8_t* uvRow = uvPlane + ((y / 2) * w);
                    uint8_t* outRow = pBuffer + (y * w * 2);

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
        else if (magic == MAGIC_EYECAM_DIRECT && m_hMutex) {
            DWORD waitRes = WaitForSingleObject(m_hMutex, 20);
            if (waitRes == WAIT_OBJECT_0) {
                auto* header = reinterpret_cast<const TripleBufferShmHeader*>(m_pSharedBuf);
                if (header->isConnected == 1) {
                    const uint8_t* srcData = reinterpret_cast<const uint8_t*>(m_pSharedBuf) + sizeof(TripleBufferShmHeader);
                    size_t copyBytes = bufferSize;
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
            // NV12 шаблон сірого
            size_t ySize = bufferSize * 2 / 3;
            std::memset(pBuffer, 128, ySize);
            std::memset(pBuffer + ySize, 128, bufferSize - ySize);
        }
    }
}

// ==============================================================================
// Реалізація EyeCamCaptureFilter
// ==============================================================================

EyeCamCaptureFilter::EyeCamCaptureFilter(LPUNKNOWN pUnk, HRESULT* phr)
    : CBaseFilter(NAME("EyeCam Virtual Camera"), pUnk, &m_lock, CLSID_EyeCamVirtualCamera) {
    m_pPin = new EyeCamOutputPin(this, &m_lock, phr);
    if (!m_pPin && phr) {
        *phr = E_OUTOFMEMORY;
    }
}

EyeCamCaptureFilter::~EyeCamCaptureFilter() {
    delete m_pPin;
    m_pPin = nullptr;
}

CUnknown* WINAPI EyeCamCaptureFilter::CreateInstance(LPUNKNOWN pUnk, HRESULT* phr) {
    EyeCamCaptureFilter* pFilter = new EyeCamCaptureFilter(pUnk, phr);
    if (!pFilter && phr) {
        *phr = E_OUTOFMEMORY;
    }
    return pFilter;
}

int EyeCamCaptureFilter::GetPinCount() {
    return 1;
}

CBasePin* EyeCamCaptureFilter::GetPin(int n) {
    if (n == 0) return m_pPin;
    return nullptr;
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

} // namespace EyeCam

#endif // _WIN32
