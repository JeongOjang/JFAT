#define LFN_MAXLEN              64      //실제는 256인데 스텍소모를 줄이기 위해 제한함
#define OPENFILEQTY             8       //동시에 열 수 있는 파일수
#define SUPPORTDISKMAX          1       //디스크 갯수
#define SUPPORTSECTORBYTES      0x200   //Flash가 바뀌면 이값을 바꾸어 주어야함


#define SUPPORT_UTF8            1
#define JFAT_FASTBOOT           1       //0: 부팅시 빈공간 찾아 놓음 (시간이 걸림, 찾아 놓지 않으면 최초 파일 기록할 때 시간이 걸림)
#define JFATDEBUG               0
#define JFAT_READOLNY           0


//포팅해줘야 하는 함수
BOOL WINAPI STORAGE_Init(UINT LogUnitNo);
BOOL WINAPI STORAGE_GetCapacity(UINT LogUnitNo, DWORD *lpBlockQty, UINT *lpBlockSize);
BOOL WINAPI STORAGE_IsReady(UINT LogUnitNo);
BOOL WINAPI STORAGE_IsWriteProtected(UINT LogUnitNo);
BOOL WINAPI STORAGE_Read(UINT LogUnitNo, LPBYTE Buff, DWORD BlockAddr, UINT BlockLen);
BOOL WINAPI STORAGE_Write(UINT LogUnitNo, LPCBYTE Buff, DWORD BlockAddr, UINT BlockLen);
int  WINAPI STORAGE_GetMaxLun(VOID);
VOID WINAPI STORAGE_AutoFlush(UINT LogUnitNo, BOOL NowFlush);




