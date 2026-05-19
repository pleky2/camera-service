#pragma once

#include <string>
#include <tchar.h>
#include <windows.h>

#include "PTPDef.h"

struct CameraStatus {
  bool is_valid = false;
  std::string last_updated;

  unsigned long long iso_value = 0;
  std::string iso_label;

  unsigned long long fnumber_value = 0;
  std::string fnumber_label;

  unsigned long long shutter_value = 0;
  std::string shutter_label;

  unsigned long long exposure_mode_value = 0;
  std::string exposure_mode_label;

  unsigned long long white_balance_value = 0;
  std::string white_balance_label;

  unsigned long long drive_mode_value = 0;
  std::string drive_mode_label;

  unsigned long long battery_level_value = 0;
  std::string battery_level_label;

  unsigned long long battery_charge_value = 0;

  unsigned long long af_status_value = 0;
  std::string af_status_label;

  unsigned long long liveview_status_value = 0;

  unsigned long long focus_mode_value = 0;
  std::string focus_mode_label;
};

class DataManager {
 public:
  static DataManager &getInstance();
  DataManager();

  ~DataManager();

  void setIsLiveviewValidFlag(BOOL isLiveviewValid);
  BOOL getIsLiveviewValidFlag();

  void parseCameraDataHeadless(CameraStatus& out);

  void setDeviceInfoData(PTP_VENDOR_DATA_OUT *pDataOut);
  PTP_VENDOR_DATA_OUT *getDeviceInfoData();

 protected:
  void setCameraData(PTP_VENDOR_DATA_OUT *pDataOut);

 private:
  const UINT ShutterSpeedList = 0;
  const UINT FnumberList = 1;
  const UINT ISOItemList = 2;
  const UINT ExpList = 3;
  const UINT exposureModeList = 4;
  const UINT flashmodeList = 5;
  const UINT liveviewModeList = 6;
  const UINT focusareaList = 7;
  const UINT drohdrModeList = 8;
  const UINT ImageSizeList = 9;
  const UINT CompressionSettingList = 10;
  const UINT AspectRatioList = 11;
  const UINT SaveMediaList = 12;
  const UINT whitebalanceList = 13;
  const UINT DriveModeList = 14;
  const UINT meteringmodeList = 15;
  const UINT pictureeffectsList = 16;
  const UINT batterylevelList = 17;
  const UINT aelList = 18;
  const UINT aflList = 19;
  const UINT focusmodeList = 20;
  const UINT whitebalanceabList = 21;
  const UINT whitebalancegmList = 22;
  const UINT movierecordingstatusList = 23;
  const UINT nearfarList = 24;
  const UINT flashcompList = 25;
  const UINT viewList = 26;
  const UINT afstatusList = 27;
  const UINT FocusMagnifierPhaseList = 28;
  const UINT mfstatusList = 29;
};
