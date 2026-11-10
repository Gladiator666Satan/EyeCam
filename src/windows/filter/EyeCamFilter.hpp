#pragma once

#if defined(_WIN32)
#include <windows.h>
#include <dshow.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <string>

#include "CommonTypes.hpp"
#include "SharedMemoryProtocol.hpp"
#include "EyeCamGuids.hpp"

namespace EyeCam {

class EyeCamCaptureFilter;

/**
 * @brief Вихідний пін захоплення відео (Video Capture Output Pin).
 * Реалізує IPin, IKsPropertySet (для ідентифікації як PIN_CATEGORY_CAPTURE),
 * та IAMStreamConfig для узгодження роздільної здатності й частоти кадрів (30/60 FPS).
 */
class EyeCamOutputPin : public IPin, public IKsPropertySet, public IAMStreamConfig {
public:
    EyeCamOutputPin(EyeCamCaptureFilter* pFilter);
    virtual ~EyeCamOutputPin();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPin
    STDMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** pPin) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir) override;
    STDMETHODIMP QueryId(LPWSTR* Id) override;
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum) override;
    STDMETHODIMP QueryInternalConnections(IPin** apPin, ULONG* nPin) override;
    STDMETHODIMP EndOfStream() override;
    STDMETHODIMP BeginFlush() override;
    STDMETHODIMP EndFlush() override;
    STDMETHODIMP NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) override;

    // IKsPropertySet (Критично важливо для Zoom/Teams/Discord для виявлення як PIN_CATEGORY_CAPTURE)
    STDMETHODIMP Set(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData) override;
    STDMETHODIMP Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) override;
    STDMETHODIMP QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) override;

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override;
    STDMETHODIMP GetNumberOfCapabilities(int* piCount, int* piSize) override;
    STDMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) override;

    // Управління потоком
    HRESULT Active();
    HRESULT Inactive();
    void ThreadProc();

private:
    HRESULT DecideAllocator(IMemInputPin* pPin, IMemAllocator** ppAlloc);
    HRESULT CreateDefaultMediaType(AM_MEDIA_TYPE* pmt, uint32_t width, uint32_t height, PixelFormat fmt, uint32_t fps);
    void FillBuffer(BYTE* pBuffer, size_t bufferSize);

    std::atomic<ULONG> m_refCount{1};
    EyeCamCaptureFilter* m_pFilter = nullptr;
    IPin* m_pConnectedPin = nullptr;
    IMemInputPin* m_pMemInputPin = nullptr;
    IMemAllocator* m_pAllocator = nullptr;

    AM_MEDIA_TYPE m_mediaType{};
    bool m_hasMediaType = false;

    // Робочий потік генерації/передачі кадрів
    std::thread m_workerThread;
    std::atomic<bool> m_stopThread{false};

    // Shared Memory Client (зчитувач із спільної пам'яті продюсера)
    HANDLE m_hMapFile = nullptr;
    void* m_pSharedBuf = nullptr;
    HANDLE m_hFrameEvent = nullptr;
    HANDLE m_hMutex = nullptr;

    REFERENCE_TIME m_rtFrameTime = 0;
};

/**
 * @brief Головний DirectShow фільтр-джерело захоплення (Capture Source Filter).
 * Реалізує IBaseFilter, IMediaFilter, IPersist.
 */
class EyeCamCaptureFilter : public IBaseFilter {
public:
    EyeCamCaptureFilter();
    virtual ~EyeCamCaptureFilter();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPersist
    STDMETHODIMP GetClassID(CLSID* pClassID) override;

    // IMediaFilter
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Run(REFERENCE_TIME tStart) override;
    STDMETHODIMP GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) override;
    STDMETHODIMP SetSyncSource(IReferenceClock* pClock) override;
    STDMETHODIMP GetSyncSource(IReferenceClock** pClock) override;

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** ppEnum) override;
    STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin) override;
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override;
    STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) override;
    STDMETHODIMP QueryVendorInfo(LPWSTR* pVendorInfo) override;

    FILTER_STATE GetFilterState() const { return m_state; }

private:
    std::atomic<ULONG> m_refCount{1};
    FILTER_STATE m_state = State_Stopped;
    IFilterGraph* m_pGraph = nullptr;
    IReferenceClock* m_pClock = nullptr;
    std::wstring m_filterName;
    EyeCamOutputPin* m_pPin = nullptr;
    std::mutex m_filterLock;
};

} // namespace EyeCam
#endif
