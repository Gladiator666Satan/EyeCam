#pragma once

#include <windows.h>
#include <dshow.h>

#include "SharedMemoryProtocol.hpp"
#include "CommonTypes.hpp"

#ifndef REFTIME
typedef LONGLONG REFERENCE_TIME;
#endif

// CLSID фільтра віртуальної камери EyeCam: {E4E5C1A0-3882-4C79-880B-DE8A3626C101}
// {E4E5C1A0-3882-4C79-880B-DE8A3626C101}
DEFINE_GUID(CLSID_EyeCamFilter, 
0xe4e5c1a0, 0x3882, 0x4c79, 0x88, 0xb, 0xde, 0x8a, 0x36, 0x26, 0xc1, 0x1);

class EyeCamPin;

/**
 * @brief Повноцінний DirectShow Source Filter для реєстрації віртуальної камери у системі.
 * Реалізує інтерфейси IBaseFilter, IMediaFilter, IPersist, IAMFilterMiscFlags.
 */
class EyeCamFilter : public IBaseFilter, public IAMFilterMiscFlags {
public:
    EyeCamFilter();
    virtual ~EyeCamFilter();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPersist
    STDMETHODIMP GetClassID(CLSID* pClsid) override;

    // IMediaFilter
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Run(REFERENCE_TIME tStart) override;
    STDMETHODIMP GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) override;
    STDMETHODIMP SetSyncSource(IReferenceClock* pClock) override;
    STDMETHODIMP GetSyncSource(IReferenceClock** ppClock) override;

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** ppEnum) override;
    STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin) override;
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override;
    STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) override;
    STDMETHODIMP QueryVendorInfo(LPWSTR* pVendorInfo) override;

    // IAMFilterMiscFlags
    STDMETHODIMP_(ULONG) GetMiscFlags() override;

private:
    LONG m_refCount;
    FILTER_STATE m_state;
    IReferenceClock* m_pClock;
    IFilterGraph* m_pGraph;
    LPWSTR m_pName;
    CRITICAL_SECTION m_csFilter;

    EyeCamPin* m_pPin;

    friend class EyeCamPin;
};

/**
 * @brief Вихідний пін фільтра віртуальної камери.
 * Реалізує IPin, IQualityControl, IAMStreamConfig, IKsPropertySet.
 */
class EyeCamPin : public IPin, public IAMStreamConfig, public IKsPropertySet {
public:
    EyeCamPin(EyeCamFilter* pFilter);
    virtual ~EyeCamPin();

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

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override;
    STDMETHODIMP GetNumberOfCapabilities(int* piCount, int* piSize) override;
    STDMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) override;

    // IKsPropertySet
    STDMETHODIMP Set(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData) override;
    STDMETHODIMP Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) override;
    STDMETHODIMP QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) override;

    // Керування стрімінгом
    HRESULT Active();
    HRESULT Inactive();

private:
    static DWORD WINAPI ThreadProcThunk(LPVOID lpParameter);
    DWORD ThreadProc();

    bool ReadSharedMemoryFrame(uint8_t* dest, size_t destSize, LONGLONG* pTimestampNs);
    void FillFallbackPattern(uint8_t* dest, size_t destSize);

    EyeCamFilter* m_pFilter;
    IPin* m_pConnectedPin;
    IMemInputPin* m_pInputPin;
    IMemAllocator* m_pAllocator;
    AM_MEDIA_TYPE m_mediaType;

    HANDLE m_hThread;
    HANDLE m_hStopEvent;
    
    // Спільна пам'ять
    HANDLE m_hMapFile;
    void*  m_pSharedBuf;
    HANDLE m_hFrameEvent;
    HANDLE m_hMutex;

    CRITICAL_SECTION m_csPin;
};