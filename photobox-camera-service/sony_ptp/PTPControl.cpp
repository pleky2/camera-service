#include "PTPControl.h"

#include <Sti.h>
#include <tchar.h>

#include <string>

//#include "afxdialogex.h"
#include "atlimage.h"

UINT8 WhitebalanceAB;
UINT8 WhitebalanceGM;

IWiaItemExtras *itemExtra = NULL;
typedef int(__stdcall *PrepareDll)(IWiaItemExtras *itemExtraArray[10]);
typedef BOOL(__stdcall *createBitmapDataForLiveViewDll)(BYTE &buffer,
                                                        BITMAP &bitmap,
                                                        HBITMAP &hBitmap);
typedef BOOL(__stdcall *createBitmapDataForThumbnailDll)(BYTE &buffer,
                                                         BITMAP &bitmap,
                                                         HBITMAP &hBitmap,
                                                         DWORD size);
static HANDLE hMutex;
static DataManager &dMgr = DataManager::getInstance();

struct Liveview_ObjectInfoStr {
  DWORD Offset;
  DWORD Size;
};

PTPControl &PTPControl::getInstance() {
  static PTPControl instance;
  return instance;
}

PTPControl::PTPControl() {}

BOOL PTPControl::prepareConnection() {
  HRESULT hr = 0;
  CComPtr<IWiaDevMgr> pWiaDevMgr;
  CComPtr<IWiaItem> pWiaItemRoot;
  IWiaItemExtras *pWiaItemExtras = NULL;

  hr = pWiaDevMgr.CoCreateInstance(CLSID_WiaDevMgr);

  if (hr != S_OK) {
    return FALSE;
  }

  CComPtr<IEnumWIA_DEV_INFO> pEnumDevInfo;
  hr = pWiaDevMgr->EnumDeviceInfo(WIA_DEVINFO_ENUM_LOCAL, &pEnumDevInfo);
  if (hr != S_OK) return FALSE;

  CComPtr<IWiaPropertyStorage> pWiaPropertyStorage;
  BOOL found = FALSE;
  while (pEnumDevInfo->Next(1, &pWiaPropertyStorage, NULL) == S_OK) {
    PROPSPEC PropSpec[1] = {0};
    PROPVARIANT PropVar[1] = {0};
    PropSpec[0].ulKind = PRSPEC_PROPID;
    PropSpec[0].propid = WIA_DIP_DEV_ID;

    hr = pWiaPropertyStorage->ReadMultiple(1, PropSpec, PropVar);
    if (SUCCEEDED(hr)) {
      hr = pWiaDevMgr->CreateDevice(PropVar[0].bstrVal, &pWiaItemRoot);
      FreePropVariantArray(1, PropVar);
      if (SUCCEEDED(hr)) {
        found = TRUE;
        break;
      }
    }
    pWiaPropertyStorage.Release();
  }

  if (!found) {
    itemExtra = NULL;
    return FALSE;
  }

  hr = pWiaItemRoot->QueryInterface(IID_IWiaItemExtras,
                                    reinterpret_cast<void **>(&pWiaItemExtras));
  if (hr != S_OK) {
    return FALSE;
  }

  // Release previous interface before overwriting to avoid COM leak on reconnect.
  if (itemExtra != NULL) {
    itemExtra->Release();
    itemExtra = NULL;
  }
  itemExtra = pWiaItemExtras;
  return TRUE;
}

std::vector<std::string> PTPControl::getConnectedCameras() {
  std::vector<std::string> cameras;
  HRESULT hr = 0;
  CComPtr<IWiaDevMgr> pWiaDevMgr;
  hr = pWiaDevMgr.CoCreateInstance(CLSID_WiaDevMgr);
  if (hr != S_OK) return cameras;

  CComPtr<IEnumWIA_DEV_INFO> pEnumDevInfo;
  hr = pWiaDevMgr->EnumDeviceInfo(WIA_DEVINFO_ENUM_LOCAL, &pEnumDevInfo);
  if (hr != S_OK) return cameras;

  CComPtr<IWiaPropertyStorage> pWiaPropertyStorage;
  while (pEnumDevInfo->Next(1, &pWiaPropertyStorage, NULL) == S_OK) {
    PROPSPEC PropSpec[1] = {0};
    PROPVARIANT PropVar[1] = {0};
    PropSpec[0].ulKind = PRSPEC_PROPID;
    PropSpec[0].propid = WIA_DIP_DEV_NAME;

    hr = pWiaPropertyStorage->ReadMultiple(1, PropSpec, PropVar);
    if (SUCCEEDED(hr)) {
      int size_needed = WideCharToMultiByte(CP_UTF8, 0, PropVar[0].bstrVal, -1, NULL, 0, NULL, NULL);
      std::string name(size_needed, 0);
      WideCharToMultiByte(CP_UTF8, 0, PropVar[0].bstrVal, -1, &name[0], size_needed, NULL, NULL);
      // Remove null terminator
      if (!name.empty() && name.back() == '\0') name.pop_back();
      cameras.push_back(name);
      FreePropVariantArray(1, PropVar);
    }
    pWiaPropertyStorage.Release();
  }
  return cameras;
}

HRESULT PTPControl::GetObjectInfo(int objectHandle, PTP_GetObjectInfo &info) {
  if (itemExtra == NULL) {
    return S_FALSE;
  }

  WaitForSingleObject(hMutex, INFINITE);

  HRESULT hr = 0;
  PTP_VENDOR_DATA_IN *pDataIn = NULL;
  PTP_VENDOR_DATA_OUT *pDataOut = NULL;
  DWORD dwDataInSize = SIZEOF_REQUIRED_VENDOR_DATA_IN;
  DWORD dwDataOutSize = sizeof(info) + 0x1000;
  DWORD dwActualDataOutSize = 0;

  pDataIn = static_cast<PTP_VENDOR_DATA_IN *>(CoTaskMemAlloc(dwDataInSize));
  if (pDataIn == NULL) {
    return S_FALSE;
  }
  pDataOut = static_cast<PTP_VENDOR_DATA_OUT *>(CoTaskMemAlloc(dwDataOutSize));
  if (pDataOut == NULL) {
    CoTaskMemFree(pDataIn);
    return S_FALSE;
  }

  ZeroMemory(pDataIn, dwDataInSize);
  ZeroMemory(pDataOut, dwDataOutSize);

  pDataIn->OpCode = PTP_OC_GetObjectInfo;
  pDataIn->NextPhase = PTP_NEXTPHASE_READ_DATA;
  pDataIn->Params[0] = objectHandle;
  pDataIn->NumParams = 1;

  hr = itemExtra->Escape(ESCAPE_PTP_VENDOR_COMMAND,
                         reinterpret_cast<BYTE *>(pDataIn), dwDataInSize,
                         reinterpret_cast<BYTE *>(pDataOut), dwDataOutSize,
                         &dwActualDataOutSize);

  if (SUCCEEDED(hr)) {
    memcpy_s(&info, dwActualDataOutSize, pDataOut->VendorReadData,
             dwActualDataOutSize - sizeof(PTP_VENDOR_DATA_OUT));
  }

  CoTaskMemFree(pDataIn);
  CoTaskMemFree(pDataOut);

  ReleaseMutex(hMutex);

  return hr;
}

HRESULT PTPControl::ExecuteGetObject(DWORD objectHandle, BYTE *buffer,
                                     DWORD size) {
  if (itemExtra == NULL) {
    return S_FALSE;
  }

  WaitForSingleObject(hMutex, INFINITE);

  HRESULT hr = 0;
  PTP_VENDOR_DATA_IN *pDataIn = NULL;
  PTP_VENDOR_DATA_OUT *pDataOut = NULL;
  DWORD dwDataInSize = SIZEOF_REQUIRED_VENDOR_DATA_IN;
  DWORD dwDataOutSize = SIZEOF_REQUIRED_VENDOR_DATA_OUT + size;
  DWORD dwActualDataOutSize = 0;

  pDataIn = static_cast<PTP_VENDOR_DATA_IN *>(CoTaskMemAlloc(dwDataInSize));
  if (pDataIn == NULL) {
    return S_FALSE;
  }
  pDataOut = static_cast<PTP_VENDOR_DATA_OUT *>(CoTaskMemAlloc(dwDataOutSize));
  if (pDataOut == NULL) {
    CoTaskMemFree(pDataIn);
    return S_FALSE;
  }

  ZeroMemory(pDataIn, dwDataInSize);
  if (pDataOut != NULL) {
    ZeroMemory(pDataOut, dwDataOutSize);
  }

  pDataIn->OpCode = PTP_OC_GetObject;
  pDataIn->NextPhase = PTP_NEXTPHASE_READ_DATA;
  pDataIn->Params[0] = objectHandle;
  pDataIn->NumParams = 1;

  hr = itemExtra->Escape(ESCAPE_PTP_VENDOR_COMMAND,
                         reinterpret_cast<BYTE *>(pDataIn), dwDataInSize,
                         reinterpret_cast<BYTE *>(pDataOut), dwDataOutSize,
                         &dwActualDataOutSize);

  if (PTP_RC_OK != pDataOut->ResponseCode) {
    hr = E_FAIL;
  }

  if (SUCCEEDED(hr)) {
    memcpy_s(buffer, size, pDataOut->VendorReadData, size);
  }

  CoTaskMemFree(pDataIn);
  CoTaskMemFree(pDataOut);

  ReleaseMutex(hMutex);

  return hr;
}

HRESULT PTPControl::SDIOConnect(DWORD param1, DWORD param2, DWORD param3) {
  if (itemExtra == NULL) {
    return S_FALSE;
  }
  if (hMutex == NULL) {
    hMutex = CreateMutex(NULL, FALSE, MUTEX);
    if (hMutex == NULL) {
      return S_FALSE;
    }
  }
  WaitForSingleObject(hMutex, INFINITE);

  HRESULT hr = 0;
  PTP_VENDOR_DATA_IN *pDataIn = NULL;
  PTP_VENDOR_DATA_OUT *pDataOut = NULL;
  DWORD dwDataInSize = SIZEOF_REQUIRED_VENDOR_DATA_IN;
  DWORD dwDataOutSize = SIZEOF_REQUIRED_VENDOR_DATA_OUT + 0x0008;
  DWORD dwActualDataOutSize = 0;

  pDataIn = static_cast<PTP_VENDOR_DATA_IN *>(CoTaskMemAlloc(dwDataInSize));
  if (pDataIn == NULL) {
    return S_FALSE;
  }
  pDataOut = static_cast<PTP_VENDOR_DATA_OUT *>(CoTaskMemAlloc(dwDataOutSize));
  if (pDataOut == NULL) {
    CoTaskMemFree(pDataIn);
    return S_FALSE;
  }

  ZeroMemory(pDataIn, dwDataInSize);
  ZeroMemory(pDataOut, dwDataOutSize);

  pDataIn->OpCode = PTP_OC_SDIOConnect;
  pDataIn->NextPhase = PTP_NEXTPHASE_READ_DATA;
  pDataIn->Params[0] = param1;
  pDataIn->Params[1] = param2;
  pDataIn->Params[2] = param3;
  pDataIn->NumParams = 3;

  hr = itemExtra->Escape(ESCAPE_PTP_VENDOR_COMMAND,
                         reinterpret_cast<BYTE *>(pDataIn), dwDataInSize,
                         reinterpret_cast<BYTE *>(pDataOut), dwDataOutSize,
                         &dwActualDataOutSize);

  CoTaskMemFree(pDataIn);
  CoTaskMemFree(pDataOut);

  ReleaseMutex(hMutex);

  return hr;
}

HRESULT PTPControl::SDIOGetExtDeviceInfo(DWORD param) {
  if (itemExtra == NULL) {
    return S_FALSE;
  }
  WaitForSingleObject(hMutex, INFINITE);

  HRESULT hr = 0;
  PTP_VENDOR_DATA_IN *pDataIn = NULL;
  PTP_VENDOR_DATA_OUT *pDataOut = NULL;
  DWORD dwDataInSize = SIZEOF_REQUIRED_VENDOR_DATA_IN;
  DWORD dwDataOutSize = SIZEOF_REQUIRED_VENDOR_DATA_OUT + 0x1000;
  DWORD dwActualDataOutSize = 0;

  pDataIn = static_cast<PTP_VENDOR_DATA_IN *>(CoTaskMemAlloc(dwDataInSize));
  if (pDataIn == NULL) {
    return S_FALSE;
  }
  pDataOut = static_cast<PTP_VENDOR_DATA_OUT *>(CoTaskMemAlloc(dwDataOutSize));
  if (pDataOut == NULL) {
    CoTaskMemFree(pDataIn);
    return S_FALSE;
  }

  ZeroMemory(pDataIn, dwDataInSize);
  ZeroMemory(pDataOut, dwDataOutSize);

  pDataIn->OpCode = PTP_OC_SDIOGetExtDeviceInfo;
  pDataIn->NextPhase = PTP_NEXTPHASE_READ_DATA;
  pDataIn->Params[0] = param;
  pDataIn->NumParams = 1;

  hr = itemExtra->Escape(ESCAPE_PTP_VENDOR_COMMAND,
                         reinterpret_cast<BYTE *>(pDataIn), dwDataInSize,
                         reinterpret_cast<BYTE *>(pDataOut), dwDataOutSize,
                         &dwActualDataOutSize);

  CoTaskMemFree(pDataIn);
  CoTaskMemFree(pDataOut);
  ReleaseMutex(hMutex);
  return hr;
}

HRESULT PTPControl::SDIOControlDevice(DWORD param1, UINT32 value) {
  if (itemExtra == NULL) {
    return S_FALSE;
  }
  WaitForSingleObject(hMutex, INFINITE);

  HRESULT hr = 0;
  PTP_VENDOR_DATA_IN *pDataIn = NULL;
  PTP_VENDOR_DATA_OUT *pDataOut = NULL;
  DWORD dwDataInSize = SIZEOF_REQUIRED_VENDOR_DATA_IN + sizeof(UINT16);
  DWORD dwDataOutSize = SIZEOF_REQUIRED_VENDOR_DATA_OUT;
  DWORD dwActualDataOutSize = 0;

  pDataIn = static_cast<PTP_VENDOR_DATA_IN *>(CoTaskMemAlloc(dwDataInSize));
  if (pDataIn == NULL) {
    return S_FALSE;
  }
  pDataOut = static_cast<PTP_VENDOR_DATA_OUT *>(CoTaskMemAlloc(dwDataOutSize));
  if (pDataOut == NULL) {
    CoTaskMemFree(pDataIn);
    return S_FALSE;
  }

  ZeroMemory(pDataIn, dwDataInSize);
  ZeroMemory(pDataOut, dwDataOutSize);

  pDataIn->OpCode = PTP_OC_SDIOControlDevice;
  pDataIn->NextPhase = PTP_NEXTPHASE_WRITE_DATA;
  pDataIn->Params[0] = param1;
  pDataIn->NumParams = 1;

  UINT16 *tmp = reinterpret_cast<UINT16 *>(pDataIn->VendorWriteData);
  *tmp = value;

  hr = itemExtra->Escape(ESCAPE_PTP_VENDOR_COMMAND,
                         reinterpret_cast<BYTE *>(pDataIn), dwDataInSize,
                         reinterpret_cast<BYTE *>(pDataOut), dwDataOutSize,
                         &dwActualDataOutSize);

  if (hr == S_OK && PTP_RC_OK != pDataOut->ResponseCode) {
    hr = E_FAIL;
  }

  CoTaskMemFree(pDataIn);
  CoTaskMemFree(pDataOut);
  ReleaseMutex(hMutex);
  return hr;
}

HRESULT PTPControl::SDIOSetExtDevicePropValueImpl(DWORD param1, void *value,
                                                  DWORD size) {
  if (itemExtra == NULL) {
    return S_FALSE;
  }

  WaitForSingleObject(hMutex, INFINITE);

  HRESULT hr = 0;
  PTP_VENDOR_DATA_IN *pDataIn = NULL;
  PTP_VENDOR_DATA_OUT *pDataOut = NULL;
  DWORD dwDataInSize = SIZEOF_REQUIRED_VENDOR_DATA_IN + size;
  DWORD dwDataOutSize = SIZEOF_REQUIRED_VENDOR_DATA_OUT;
  DWORD dwActualDataOutSize = 0;

  pDataIn = static_cast<PTP_VENDOR_DATA_IN *>(CoTaskMemAlloc(dwDataInSize));
  if (pDataIn == NULL) {
    return S_FALSE;
  }
  pDataOut = static_cast<PTP_VENDOR_DATA_OUT *>(CoTaskMemAlloc(dwDataOutSize));
  if (pDataOut == NULL) {
    CoTaskMemFree(pDataIn);
    return S_FALSE;
  }

  ZeroMemory(pDataIn, dwDataInSize);
  ZeroMemory(pDataOut, dwDataOutSize);

  pDataIn->OpCode = PTP_OC_SDIOSetExtDevicePropValue;
  pDataIn->NextPhase = PTP_NEXTPHASE_WRITE_DATA;
  pDataIn->Params[0] = param1;
  pDataIn->NumParams = 1;

  memcpy_s(pDataIn->VendorWriteData, size, value, size);

  hr = itemExtra->Escape(ESCAPE_PTP_VENDOR_COMMAND,
                         reinterpret_cast<BYTE *>(pDataIn), dwDataInSize,
                         reinterpret_cast<BYTE *>(pDataOut), dwDataOutSize,
                         &dwActualDataOutSize);

  if (hr == S_OK && PTP_RC_OK != pDataOut->ResponseCode) {
    hr = E_FAIL;
  }

  CoTaskMemFree(pDataIn);
  CoTaskMemFree(pDataOut);
  ReleaseMutex(hMutex);
  return hr;
}

HRESULT PTPControl::SDIOGetAllExtDevicePropInfo(HWND pHwnd) {
  if (itemExtra == NULL) {
    return S_FALSE;
  }
  WaitForSingleObject(hMutex, INFINITE);

  HRESULT hr = 0;
  PTP_VENDOR_DATA_IN *pDataIn = NULL;
  PTP_VENDOR_DATA_OUT *pDataOut = NULL;
  DWORD dwDataInSize = SIZEOF_REQUIRED_VENDOR_DATA_IN;
  DWORD dwDataOutSize = SIZEOF_REQUIRED_VENDOR_DATA_OUT + 64 * 1024;
  DWORD dwActualDataOutSize = 0;

  pDataIn = static_cast<PTP_VENDOR_DATA_IN *>(CoTaskMemAlloc(dwDataInSize));
  if (pDataIn == NULL) {
    return S_FALSE;
  }
  pDataOut = static_cast<PTP_VENDOR_DATA_OUT *>(CoTaskMemAlloc(dwDataOutSize));
  if (pDataOut == NULL) {
    CoTaskMemFree(pDataIn);
    return S_FALSE;
  }

  ZeroMemory(pDataIn, dwDataInSize);
  ZeroMemory(pDataOut, dwDataOutSize);

  pDataIn->OpCode = PTP_OC_SDIOGetAllExtDeviceInfo;
  pDataIn->NextPhase = PTP_NEXTPHASE_READ_DATA;
  pDataIn->NumParams = 0;

  hr = itemExtra->Escape(ESCAPE_PTP_VENDOR_COMMAND,
                         reinterpret_cast<BYTE *>(pDataIn), dwDataInSize,
                         reinterpret_cast<BYTE *>(pDataOut), dwDataOutSize,
                         &dwActualDataOutSize);
  setCameraData(pDataOut);

  CoTaskMemFree(pDataIn);

  ReleaseMutex(hMutex);

  return hr;
}

HRESULT PTPControl::GetDeviceInfo() {
  if (itemExtra == NULL) {
    return S_FALSE;
  }
  WaitForSingleObject(hMutex, INFINITE);

  HRESULT hr = 0;
  PTP_VENDOR_DATA_IN *pDataIn = NULL;
  PTP_VENDOR_DATA_OUT *pDataOut = NULL;
  DWORD dwDataInSize = SIZEOF_REQUIRED_VENDOR_DATA_IN;
  DWORD dwDataOutSize = SIZEOF_REQUIRED_VENDOR_DATA_OUT + 0x1000;
  DWORD dwActualDataOutSize = 0;

  pDataIn = static_cast<PTP_VENDOR_DATA_IN *>(CoTaskMemAlloc(dwDataInSize));
  if (pDataIn == NULL) {
    return S_FALSE;
  }
  pDataOut = static_cast<PTP_VENDOR_DATA_OUT *>(CoTaskMemAlloc(dwDataOutSize));
  if (pDataOut == NULL) {
    CoTaskMemFree(pDataIn);
    return S_FALSE;
  }

  ZeroMemory(pDataIn, dwDataInSize);
  ZeroMemory(pDataOut, dwDataOutSize);

  pDataIn->OpCode = PTP_OC_GetDeviceInfo;
  pDataIn->NextPhase = PTP_NEXTPHASE_READ_DATA;
  pDataIn->NumParams = 0;

  hr = itemExtra->Escape(ESCAPE_PTP_VENDOR_COMMAND,
                         reinterpret_cast<BYTE *>(pDataIn), dwDataInSize,
                         reinterpret_cast<BYTE *>(pDataOut), dwDataOutSize,
                         &dwActualDataOutSize);

  dMgr.setDeviceInfoData(pDataOut);
  CoTaskMemFree(pDataIn);

  ReleaseMutex(hMutex);

  return hr;
}

void PTPControl::closeMutex() {
  if (hMutex) {
    CloseHandle(hMutex);
    hMutex = NULL;
  }
}