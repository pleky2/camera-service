#include "DataManager.h"

#include <tchar.h>
#include <windows.h>

#include "DevicePropItemList.h"
#include "PTPDef.h"

static PTP_VENDOR_DATA_OUT *pDataHolder = NULL;
static PTP_VENDOR_DATA_OUT *pDeviceInfoHolder = NULL;

// ---- Headless label lookup helpers ----

static std::string tcharToString(LPCTSTR tstr) {
  if (!tstr) return "";
#ifdef UNICODE
  int len = WideCharToMultiByte(CP_UTF8, 0, tstr, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 0) return "";
  std::string result(static_cast<size_t>(len - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, tstr, -1, &result[0], len, nullptr, nullptr);
  return result;
#else
  return std::string(tstr);
#endif
}

static std::string lookupPropLabel(int listIdx, unsigned long long value) {
  ListEntryItems &entry = devicePropItemList[listIdx];
  for (unsigned int i = 0; i < entry.size; i++) {
    if (static_cast<unsigned long long>(entry.itemList[i].value) == value) {
      return tcharToString(entry.itemList[i].str);
    }
  }
  return "Unknown (" + std::to_string(value) + ")";
}

static BOOL isLiveviewValid = false;

DataManager &DataManager::getInstance() {
  static DataManager instance;
  return instance;
}

DataManager::DataManager() {}

DataManager::~DataManager() {}

void DataManager::setIsLiveviewValidFlag(BOOL validFlag) {
  isLiveviewValid = validFlag;
}

BOOL DataManager::getIsLiveviewValidFlag() { return isLiveviewValid; }

void DataManager::setCameraData(PTP_VENDOR_DATA_OUT *pDataOut) {
  pDataHolder = pDataOut;
}

void DataManager::setDeviceInfoData(PTP_VENDOR_DATA_OUT *pDataOut) {
  pDeviceInfoHolder = pDataOut;
}

PTP_VENDOR_DATA_OUT *DataManager::getDeviceInfoData() {
  return pDeviceInfoHolder;
}

// ========================================================
// Headless camera data parser (no UI/MFC dependencies)
// Parses the raw SDIOGetAllExtDevicePropInfo response and
// fills a CameraStatus struct. Frees pDataHolder when done.
// ========================================================
void DataManager::parseCameraDataHeadless(CameraStatus &out) {
  if (pDataHolder == NULL) {
    return;
  }

  unsigned long long length =
      *reinterpret_cast<unsigned long long *>(&pDataHolder->VendorReadData[0]);
  unsigned int offset = sizeof(length);

  for (unsigned long long i = 0; i < length; i++) {
    unsigned short propertyCode = *reinterpret_cast<unsigned short *>(
        &pDataHolder->VendorReadData[offset]);
    offset += sizeof(unsigned short);
    unsigned short dataType = *reinterpret_cast<unsigned short *>(
        &pDataHolder->VendorReadData[offset]);
    offset += sizeof(unsigned short);
    // flagGetSet
    offset += sizeof(unsigned char);
    // isEnabled
    offset += sizeof(unsigned char);

    unsigned long sizeofType = 0;
    unsigned long long currentValue = 0;

    switch (dataType) {
      case PTP_DT_INT8:
      case PTP_DT_UINT8:
        sizeofType = sizeof(char);
        offset += sizeof(unsigned char);  // defaultValue
        currentValue =
            *static_cast<unsigned char *>(&pDataHolder->VendorReadData[offset]);
        offset += sizeof(unsigned char);
        break;
      case PTP_DT_INT16:
      case PTP_DT_UINT16:
        sizeofType = sizeof(short);
        offset += sizeof(unsigned short);  // defaultValue
        currentValue = *reinterpret_cast<unsigned short *>(
            &pDataHolder->VendorReadData[offset]);
        offset += sizeof(unsigned short);
        break;
      case PTP_DT_INT32:
      case PTP_DT_UINT32:
        sizeofType = sizeof(long);
        offset += sizeof(unsigned long);  // defaultValue
        currentValue = *reinterpret_cast<unsigned long *>(
            &pDataHolder->VendorReadData[offset]);
        offset += sizeof(unsigned long);
        break;
      case PTP_DT_INT64:
      case PTP_DT_UINT64:
        sizeofType = sizeof(long long);
        offset += sizeof(unsigned long long);  // defaultValue
        currentValue = *reinterpret_cast<unsigned long long *>(
            &pDataHolder->VendorReadData[offset]);
        offset += sizeof(unsigned long long);
        break;
      case PTP_DT_STR:
        sizeofType = 0;
        offset += sizeof(wchar_t) *
                  (wcslen(reinterpret_cast<wchar_t *>(
                       &pDataHolder->VendorReadData[offset])) +
                   1);  // defaultValue
        offset += sizeof(wchar_t) *
                  (wcslen(reinterpret_cast<wchar_t *>(
                       &pDataHolder->VendorReadData[offset])) +
                   1);  // currentValue
        break;
      case PTP_DT_AINT8:
      case PTP_DT_AUINT8: {
        sizeofType = 0;
        unsigned long cnt =
            *reinterpret_cast<unsigned long *>(&pDataHolder->VendorReadData[offset]);
        offset += sizeof(unsigned long) +
                  static_cast<unsigned int>(sizeof(unsigned char) * cnt);
        cnt = *reinterpret_cast<unsigned long *>(&pDataHolder->VendorReadData[offset]);
        offset += sizeof(unsigned long) +
                  static_cast<unsigned int>(sizeof(unsigned char) * cnt);
        break;
      }
      case PTP_DT_AINT16:
      case PTP_DT_AUINT16: {
        sizeofType = 0;
        unsigned long cnt =
            *reinterpret_cast<unsigned long *>(&pDataHolder->VendorReadData[offset]);
        offset += sizeof(unsigned long) +
                  static_cast<unsigned int>(sizeof(unsigned short) * cnt);
        cnt = *reinterpret_cast<unsigned long *>(&pDataHolder->VendorReadData[offset]);
        offset += sizeof(unsigned long) +
                  static_cast<unsigned int>(sizeof(unsigned short) * cnt);
        break;
      }
      case PTP_DT_AINT32:
      case PTP_DT_AUINT32: {
        sizeofType = 0;
        unsigned long cnt =
            *reinterpret_cast<unsigned long *>(&pDataHolder->VendorReadData[offset]);
        offset += sizeof(unsigned long) +
                  static_cast<unsigned int>(sizeof(unsigned long) * cnt);
        cnt = *reinterpret_cast<unsigned long *>(&pDataHolder->VendorReadData[offset]);
        offset += sizeof(unsigned long) +
                  static_cast<unsigned int>(sizeof(unsigned long) * cnt);
        break;
      }
      case PTP_DT_AINT64:
      case PTP_DT_AUINT64: {
        sizeofType = 0;
        unsigned long cnt =
            *reinterpret_cast<unsigned long *>(&pDataHolder->VendorReadData[offset]);
        offset += sizeof(unsigned long) +
                  static_cast<unsigned int>(sizeof(unsigned long long) * cnt);
        cnt = *reinterpret_cast<unsigned long *>(&pDataHolder->VendorReadData[offset]);
        offset += sizeof(unsigned long) +
                  static_cast<unsigned int>(sizeof(unsigned long long) * cnt);
        break;
      }
      default:
        sizeofType = 0;
        break;
    }

    unsigned char formFlag =
        *static_cast<unsigned char *>(&pDataHolder->VendorReadData[offset]);
    offset += sizeof(unsigned char);

    // Store values for properties we expose in the status API
    switch (propertyCode) {
      case DPC_ISO:
        out.iso_value = currentValue;
        out.iso_label = lookupPropLabel(ISOItemList, currentValue);
        break;
      case DPC_FNUMBER:
        out.fnumber_value = currentValue;
        out.fnumber_label = lookupPropLabel(FnumberList, currentValue);
        break;
      case DPC_SHUTTER_SPEED:
        out.shutter_value = currentValue;
        out.shutter_label = lookupPropLabel(ShutterSpeedList, currentValue);
        break;
      case DPC_EXPOSURE_MODE:
        out.exposure_mode_value = currentValue;
        out.exposure_mode_label =
            lookupPropLabel(exposureModeList, currentValue);
        break;
      case DPC_WHITE_BALANCE:
        out.white_balance_value = currentValue;
        out.white_balance_label =
            lookupPropLabel(whitebalanceList, currentValue);
        break;
      case DPC_DRIVE_MODE:
        out.drive_mode_value = currentValue;
        out.drive_mode_label = lookupPropLabel(DriveModeList, currentValue);
        break;
      case DPC_BATTERY_LEVEL:
        out.battery_level_value = currentValue;
        out.battery_level_label =
            lookupPropLabel(batterylevelList, currentValue);
        break;
      case DPC_BATTERY_CHARGE:
        out.battery_charge_value = currentValue & 0xFFFF;
        break;
      case DPC_AF_STATUS:
        out.af_status_value = currentValue;
        out.af_status_label = lookupPropLabel(afstatusList, currentValue);
        break;
      case DPC_LIVEVIEW_STATUS:
        out.liveview_status_value = currentValue;
        setIsLiveviewValidFlag((currentValue == 1));
        break;
      case DPC_FOCUS_MODE:
        out.focus_mode_value = currentValue;
        out.focus_mode_label = lookupPropLabel(focusmodeList, currentValue);
        break;
    }

    // Skip form data (range: min/max/step; enum: supported values list)
    if (formFlag == 0x01) {
      offset += (sizeofType * 3);
    } else if (formFlag != 0x00) {
      unsigned short num = *reinterpret_cast<unsigned short *>(
          &pDataHolder->VendorReadData[offset]);
      offset += sizeof(unsigned short);
      offset += (num * sizeofType);
    }
  }

  CoTaskMemFree(pDataHolder);
  pDataHolder = NULL;
  out.is_valid = true;
}