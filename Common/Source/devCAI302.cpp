// $Id: devCAI302.cpp,v 1.7 2005/07/05 10:34:55 samgi Exp $


//


// CAUTION!
// cai302ParseNMEA is called from com port read thread
// all other functions are called from windows message loop thread


#define  LOGSTREAM 0


#include <windows.h>
#include <tchar.h>


#include "externs.h"
#include "utils.h"
#include "parser.h"
#include "port.h"

#include "devCai302.h"

#ifdef _SIM_
static BOOL fSimMode = TRUE;
#else
static BOOL fSimMode = FALSE;
#endif


#define  CtrlC  0x03
#define  swap(x)      x = ((((x<<8) & 0xff00) | ((x>>8) & 0x00ff)) & 0xffff)

#pragma pack(push, 1)                  // force byte allignement

typedef struct{
  unsigned char result[3];
  unsigned char reserved[15];
  unsigned char ID[3];
  unsigned char Type;
  unsigned char Version[5];
  unsigned char reserved2[6];
  unsigned char cai302ID;
  unsigned char reserved3;
}cai302_Wdata_t;

typedef struct{
  unsigned char result[3];
  unsigned char PilotCount;
  unsigned char PilotRecordSize;
}cai302_OdataNoArgs_t;

typedef struct{
  unsigned char  result[3];
  char           PilotName[24];
  unsigned char  OldUnit;                                       // old unit
  unsigned char  OldTemperaturUnit;
  unsigned char  SinkTone;
  unsigned char  TotalEnergyFinalGlide;
  unsigned char  ShowFinalGlideAltitude;
  unsigned char  MapDatum;  // ignored on IGC version
  unsigned short ApproachRadius;
  unsigned short ArrivalRadius;
  unsigned short EnrouteLoggingInterval;
  unsigned short CloseTpLoggingInterval;
  unsigned short TimeBetweenFlightLogs;                     // [Minutes]
  unsigned short MinimumSpeedToForceFlightLogging;          // (Knots)
  unsigned char  StfDeadBand;                                // (M/S)
  unsigned char  Reserved;
  unsigned short UnitWord;
  unsigned short Reserved2;
  unsigned short MarginHeight;                              // (10ths of Meters)
  unsigned char  Spare[60];                                 // 302 expect more data than the documented filed
                                                            // be shure there is space to hold the data
}cai302_OdataPilot_t;

typedef struct{
  unsigned char result[3];
  unsigned char GliderRecordSize;
}cai302_GdataNoArgs_t;

typedef struct{
  unsigned char  result[3];
  unsigned char  GliderType[12];
  unsigned char  GliderID[12];
  unsigned char  bestLD;
  unsigned char  BestGlideSpeed;
  unsigned char  TwoMeterSinkAtSpeed;
  unsigned char  Reserved1;
  unsigned short WeightInLiters;
  unsigned short BallastCapacity;
  unsigned short Reserved2;
  unsigned short ConfigWord;
  unsigned char  Spare[60];                                 // 302 expect more data than the documented filed
                                                            // be shure there is space to hold the data
}cai302_Gdata_t;


#pragma pack(pop)

static cai302_Wdata_t cai302_Wdata;
static cai302_OdataNoArgs_t cai302_OdataNoArgs;
static cai302_OdataPilot_t cai302_OdataPilot;
static cai302_GdataNoArgs_t cai302_GdataNoArgs;
static cai302_Gdata_t cai302_Gdata;

// Additional sentance for CAI302 support
static BOOL cai_w(TCHAR *String, NMEA_INFO *GPS_INFO);
static BOOL cai_PCAIB(TCHAR *String, NMEA_INFO *GPS_INFO);
static cai_PCAID(TCHAR *String, NMEA_INFO *GPS_INFO);

static int  McReadyUpdateTimeout = 0;
static int  BugsUpdateTimeout = 0;
static int  BallastUpdateTimeout = 0;

#if LOGSTREAM > 0
static HANDLE hLogFile;
#endif

BOOL cai302ParseNMEA(PDeviceDescriptor_t d, TCHAR *String, NMEA_INFO *GPS_INFO){

  #if LOGSTREAM > 0
  if (hLogFile != INVALID_HANDLE_VALUE && String != NULL && _tcslen(String) > 0){
    char  sTmp[500];  // temp multibyte buffer
    TCHAR *pWC = String;
    char  *pC  = sTmp;
    DWORD dwBytesRead;
    static DWORD lastFlush = 0;

    
    sprintf(pC, "%9d <", GetTickCount());
    pC = sTmp + strlen(sTmp);

    while (*pWC){
      if (*pWC != '\r'){
        *pC = (char)*pWC;
        pC++; 
      }
      pWC++;
    }
    *pC++ = '>';
    *pC++ = '\r';
    *pC++ = '\n';
    *pC++ = '\0';

    WriteFile(hLogFile, sTmp, strlen(sTmp), &dwBytesRead, NULL); 

    /*
    if (GetTickCount() - lastFlush > 10000){
      FlushFileBuffers(hLogFile);
      lastFlush = GetTickCount();
    }
    */
      
  }
  #endif

  
  if (!NMEAChecksum(String) || (GPS_INFO == NULL)){
    return FALSE;
  }
  
  if(_tcsstr(String,TEXT("$PCAIB")) == String){
    return cai_PCAIB(&String[7], GPS_INFO);
  }

  if(_tcsstr(String,TEXT("$PCAID")) == String){
    return cai_PCAID(&String[7], GPS_INFO);
  }

  if(_tcsstr(String,TEXT("!w")) == String){
    return cai_w(&String[3], GPS_INFO);
  }

  return FALSE;

}


BOOL cai302PutMcReady(PDeviceDescriptor_t d, double McReady){

  TCHAR  szTmp[32];

  _stprintf(szTmp, TEXT("!g,m%d\r\n"), int(((McReady * 10) / KNOTSTOMETRESSECONDS) + 0.5));

  if (!fSimMode)
    (d->Com.WriteString)(szTmp);

  McReadyUpdateTimeout = 2;

  return(TRUE);

}


BOOL cai302PutBugs(PDeviceDescriptor_t d, double Bugs){

  TCHAR  szTmp[32];

  _stprintf(szTmp, TEXT("!g,u%d\r\n"), int((Bugs * 100) + 0.5));

  if (!fSimMode)
    (d->Com.WriteString)(szTmp);

  BugsUpdateTimeout = 2;

  return(TRUE);

}


BOOL cai302PutBallast(PDeviceDescriptor_t d, double Ballast){

  TCHAR  szTmp[32];

  _stprintf(szTmp, TEXT("!g,b%d\r\n"), int((Ballast * 10) + 0.5));

  if (!fSimMode)
    (d->Com.WriteString)(szTmp);

  BallastUpdateTimeout = 2;

  return(TRUE);

}

BOOL cai302Open(PDeviceDescriptor_t d, int Port){

  
  d->Port = Port;

  if (!fSimMode){
    (d->Com.WriteString)(TEXT("\x03"));
    (d->Com.WriteString)(TEXT("LOG 0\r"));
  }

  #if LOGSTREAM > 0
  hLogFile = CreateFile(TEXT("\\Speicherkarte\\cai302Nmea.log"), GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  SetFilePointer(hLogFile, 0, NULL, FILE_END);
  #endif

  return(TRUE);
}

BOOL cai302Close(PDeviceDescriptor_t d){

  
  #if LOGSTREAM > 0
  if (hLogFile != INVALID_HANDLE_VALUE)
    CloseHandle(hLogFile);
  #endif

  return(TRUE);
}


static int DeclIndex = 128;
static int nDeclErrorCode; 


BOOL cai302DeclBegin(PDeviceDescriptor_t d, TCHAR *PilotsName, TCHAR *Class, TCHAR *ID){

  TCHAR PilotName[25];
  TCHAR GliderType[13];
  TCHAR GliderID[13];
  TCHAR szTmp[255];
  nDeclErrorCode = 0;

  if (!fSimMode){

    (d->Com.StopRxThread)();

    (d->Com.SetRxTimeout)(500);
    (d->Com.WriteString)(TEXT("\x03"));
    ExpectString(d, TEXT("$$$"));            // empty rx buffer (searching for pattern that never occure)

    (d->Com.WriteString)(TEXT("\x03"));
    if (!ExpectString(d, TEXT("cmd>"))){
      nDeclErrorCode = 1;
      return(FALSE);
    };

    (d->Com.WriteString)(TEXT("upl 1\r"));
    if (!ExpectString(d, TEXT("up>"))){
      nDeclErrorCode = 1;
      return(FALSE);
    };

    ExpectString(d, TEXT("$$$"));

    (d->Com.WriteString)(TEXT("O\r"));
    (d->Com.Read)(&cai302_OdataNoArgs, sizeof(cai302_OdataNoArgs));
    if (!ExpectString(d, TEXT("up>"))){
      nDeclErrorCode = 1;
      return(FALSE);
    };

    (d->Com.WriteString)(TEXT("O 128\r"));
    (d->Com.Read)(&cai302_OdataPilot, cai302_OdataNoArgs.PilotRecordSize + 3);
    if (!ExpectString(d, TEXT("up>"))){
      nDeclErrorCode = 1;
      return(FALSE);
    };

    swap(cai302_OdataPilot.ApproachRadius);
    swap(cai302_OdataPilot.ArrivalRadius);
    swap(cai302_OdataPilot.EnrouteLoggingInterval);
    swap(cai302_OdataPilot.CloseTpLoggingInterval);
    swap(cai302_OdataPilot.TimeBetweenFlightLogs);
    swap(cai302_OdataPilot.MinimumSpeedToForceFlightLogging);
    swap(cai302_OdataPilot.UnitWord);
    swap(cai302_OdataPilot.MarginHeight);

    (d->Com.WriteString)(TEXT("G\r"));
    (d->Com.Read)(&cai302_GdataNoArgs, sizeof(cai302_GdataNoArgs));
    if (!ExpectString(d, TEXT("up>"))){
      nDeclErrorCode = 1;
      return(FALSE);
    };

    (d->Com.WriteString)(TEXT("G 0\r"));
    (d->Com.Read)(&cai302_Gdata, cai302_GdataNoArgs.GliderRecordSize + 3);
    if (!ExpectString(d, TEXT("up>"))){
      nDeclErrorCode = 1;
      return(FALSE);
    };

    swap(cai302_Gdata.WeightInLiters);
    swap(cai302_Gdata.BallastCapacity);
    swap(cai302_Gdata.ConfigWord);

    (d->Com.SetRxTimeout)(1500);

    (d->Com.WriteString)(TEXT("\x03"));
    if (!ExpectString(d, TEXT("cmd>"))){
      nDeclErrorCode = 1;
      return(FALSE);
    };

    (d->Com.WriteString)(TEXT("dow 1\r"));
    if (!ExpectString(d, TEXT("dn>"))){
      nDeclErrorCode = 1;
      return(FALSE);
    };

    _tcsncpy(PilotName, PilotsName, 24);
    PilotName[24] = '\0';
    _tcsncpy(GliderType, Class, 12);
    GliderType[12] = '\0';
    _tcsncpy(GliderID, ID, 12);
    GliderID[12] = '\0';

    _stprintf(szTmp, TEXT("O,%-24s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\r"),
      PilotName,
      cai302_OdataPilot.OldUnit,
      cai302_OdataPilot.OldTemperaturUnit,
      cai302_OdataPilot.SinkTone,
      cai302_OdataPilot.TotalEnergyFinalGlide,
      cai302_OdataPilot.ShowFinalGlideAltitude,
      cai302_OdataPilot.MapDatum,
      cai302_OdataPilot.ApproachRadius,
      cai302_OdataPilot.ArrivalRadius,
      cai302_OdataPilot.EnrouteLoggingInterval,
      cai302_OdataPilot.CloseTpLoggingInterval,
      cai302_OdataPilot.TimeBetweenFlightLogs,
      cai302_OdataPilot.MinimumSpeedToForceFlightLogging,
      cai302_OdataPilot.StfDeadBand,
      255,
      cai302_OdataPilot.UnitWord,
      cai302_OdataPilot.MarginHeight
    );


    (d->Com.WriteString)(szTmp);
    if (!ExpectString(d, TEXT("dn>"))){
      nDeclErrorCode = 1;
      return(FALSE);
    };

    _stprintf(szTmp, TEXT("G,%-12s,%-12s,%d,%d,%d,%d,%d,%d,%d\r"),
      GliderType,
      GliderID,
      cai302_Gdata.bestLD,
      cai302_Gdata.BestGlideSpeed,
      cai302_Gdata.TwoMeterSinkAtSpeed,
      cai302_Gdata.WeightInLiters,
      cai302_Gdata.BallastCapacity,
      0,
      cai302_Gdata.ConfigWord
    );

    (d->Com.WriteString)(szTmp);
    if (!ExpectString(d, TEXT("dn>"))){
      nDeclErrorCode = 1;
      return(FALSE);
    };

  }

  DeclIndex = 128;

  return(TRUE);

}


BOOL cai302DeclEnd(PDeviceDescriptor_t d){
  
  TCHAR  szTmp[32];

  if (!fSimMode){

    if (nDeclErrorCode == 0){

      _stprintf(szTmp, TEXT("D,%d\r"), 255 /* end of declaration */);
      (d->Com.WriteString)(szTmp);

      (d->Com.SetRxTimeout)(1500);            // D,255 takes more than 800ms

      if (!ExpectString(d, TEXT("dn>"))){
        nDeclErrorCode = 1;
      };

      // todo error checking

    }

    (d->Com.SetRxTimeout)(500);

    (d->Com.WriteString)(TEXT("\x03"));
    ExpectString(d, TEXT("cmd>"));

    (d->Com.WriteString)(TEXT("LOG 0\r"));
  
    (d->Com.SetRxTimeout)(0);
    (d->Com.StartRxThread)();

  }

  return(nDeclErrorCode == 0);

}


BOOL cai302DeclAddWayPoint(PDeviceDescriptor_t d, WAYPOINT *wp){

  TCHAR Name[13];
  TCHAR  szTmp[128];
  int DegLat, DegLon;
  double MinLat, MinLon;
  char NoS, EoW;

  if (nDeclErrorCode != 0)
    return(FALSE);

  _tcsncpy(Name, wp->Name, 12);
  Name[12] = '\0';
  
  DegLat = (int)wp->Lattitude;
  MinLat = wp->Lattitude - DegLat;
  NoS = 'N';
  if(MinLat<0)
    {
      NoS = 'S';
      DegLat *= -1; 
      MinLat *= -1;
    }
  MinLat *= 60;


  DegLon = (int)wp->Longditude ;
  MinLon = wp->Longditude  - DegLon;
  EoW = 'E';
  if(MinLon<0)
    {
      EoW = 'W';
      DegLon *= -1; 
      MinLon *= -1;
    }
  MinLon *=60;

  _stprintf(szTmp, TEXT("D,%d,%02d%07.4f%c,%03d%07.4f%c,%s,%d\r"), 
    DeclIndex,
    DegLat, MinLat, NoS, 
    DegLon, MinLon, EoW, 
    Name,
    (int)wp->Altitude
  );

  DeclIndex++;

  if (!fSimMode){

    (d->Com.WriteString)(szTmp);

    if (!ExpectString(d, TEXT("dn>"))){
      nDeclErrorCode = 1;
      return(FALSE);
    };

  }

  return(TRUE);

}


BOOL cai302IsLogger(PDeviceDescriptor_t d){
  return(TRUE);
}


BOOL cai302IsGPSSource(PDeviceDescriptor_t d){
  return(TRUE);
}


BOOL cai302Install(PDeviceDescriptor_t d){

  _tcscpy(d->Name, TEXT("CAI 302"));
  d->ParseNMEA = cai302ParseNMEA;
  d->PutMcReady = cai302PutMcReady;
  d->PutBugs = cai302PutBugs;
  d->PutBallast = cai302PutBallast;
  d->Open = cai302Open;
  d->Close = cai302Close;
  d->Init = NULL;
  d->LinkTimeout = NULL;
  d->DeclBegin = cai302DeclBegin;
  d->DeclEnd = cai302DeclEnd;
  d->DeclAddWayPoint = cai302DeclAddWayPoint;
  d->IsLogger = cai302IsLogger;
  d->IsGPSSource = cai302IsGPSSource;

  return(TRUE);

}


BOOL cai302Register(void){
  return(devRegister(
    TEXT("CAI 302"), 
    (1l << dfGPS)
      | (1l << dfLogger)
      | (1l << dfSpeed)
      | (1l << dfVario)
      | (1l << dfBaroAlt)
      | (1l << dfWind),
    cai302Install
  ));
}

// local stuff

/*
$PCAIB,<1>,<2>,<CR><LF>
<1> Destination Navpoint elevation in meters, format XXXXX (leading zeros will be transmitted)
<2> Destination Navpoint attribute word, format XXXXX (leading zeros will be transmitted)
*/

BOOL cai_PCAIB(TCHAR *String, NMEA_INFO *GPS_INFO){
  return TRUE;
}


/*
$PCAID,<1>,<2>,<3>,<4>*hh<CR><LF>
<1> Logged 'L' Last point Logged 'N' Last Point not logged
<2> Barometer Altitude in meters (Leading zeros will be transmitted)
<3> Engine Noise Level
<4> Log Flags
*hh Checksum, XOR of all bytes of the sentence after the �$� and before the �*�
*/

BOOL cai_PCAID(TCHAR *String, NMEA_INFO *GPS_INFO){
  return TRUE;
}


/*
!w,<1>,<2>,<3>,<4>,<5>,<6>,<7>,<8>,<9>,<10>,<11>,<12>,<13>*hh<CR><LF>
<1>  Vector wind direction in degrees
<2>  Vector wind speed in 10ths of meters per second
<3>  Vector wind age in seconds
<4>  Component wind in 10ths of Meters per second + 500 (500 = 0, 495 = 0.5 m/s tailwind)
<5>  True altitude in Meters + 1000
<6>  Instrument QNH setting
<7>  True airspeed in 100ths of Meters per second
<8>  Variometer reading in 10ths of knots + 200
<9>  Averager reading in 10ths of knots + 200
<10> Relative variometer reading in 10ths of knots + 200
<11> Instrument MacCready setting in 10ths of knots
<12> Instrument Ballast setting in percent of capacity
<13> Instrument Bug setting
*hh  Checksum, XOR of all bytes
*/

BOOL cai_w(TCHAR *String, NMEA_INFO *GPS_INFO){

  TCHAR ctemp[80];

  
  ExtractParameter(String,ctemp,1);
  GPS_INFO->ExternalWindAvailalbe = TRUE;
  GPS_INFO->ExternalWindSpeed = (StrToDouble(ctemp,NULL) / 10.0);
  ExtractParameter(String,ctemp,0);
  GPS_INFO->ExternalWindDirection = StrToDouble(ctemp,NULL);  


  ExtractParameter(String,ctemp,4);
  GPS_INFO->BaroAltitudeAvailable = TRUE;
  GPS_INFO->BaroAltitude = StrToDouble(ctemp, NULL) - 1000;

//  ExtractParameter(String,ctemp,5);
//  GPS_INFO->QNH = StrToDouble(ctemp, NULL) - 1000;
  
  ExtractParameter(String,ctemp,6);
  GPS_INFO->AirspeedAvailable = TRUE;
  GPS_INFO->Airspeed = (StrToDouble(ctemp,NULL) / 100.0);
  
  ExtractParameter(String,ctemp,7);
  GPS_INFO->VarioAvailable = TRUE;
  GPS_INFO->Vario = ((StrToDouble(ctemp,NULL) - 200.0) / 10.0) * KNOTSTOMETRESSECONDS;


  ExtractParameter(String,ctemp,10);
  GPS_INFO->MacReady = (StrToDouble(ctemp,NULL) / 10.0) * KNOTSTOMETRESSECONDS;
  if (McReadyUpdateTimeout <= 0)
    MACREADY = GPS_INFO->MacReady;
  else
    McReadyUpdateTimeout--; 


  ExtractParameter(String,ctemp,11);
  GPS_INFO->Ballast = StrToDouble(ctemp,NULL) / 100.0;
  if (BugsUpdateTimeout <= 0)
    BALLAST = GPS_INFO->Ballast;
  else 
    BallastUpdateTimeout--;


  ExtractParameter(String,ctemp,12);
  GPS_INFO->Bugs = StrToDouble(ctemp,NULL) / 100.0;
  if (BugsUpdateTimeout <= 0)
    BUGS = GPS_INFO->Bugs;
  else 
    BugsUpdateTimeout--;

  return TRUE;

}

