#ifndef __JFAT_H__
#define __JFAT_H__


#include "JFAT_CFG.H"

#ifndef _WIN32
//_lopen() OpenMode
#define OF_READ             0
#define OF_WRITE            1
#define OF_READWRITE        2

#ifndef HFILE_ERROR
#define HFILE_ERROR         (-1)
#endif

//_llseek() Origin
#define FILE_BEGIN          0
#define FILE_CURRENT        1
#define FILE_END            2
#endif

DWORD WINAPI JFAT_GetFileAttributes(LPCSTR FullPath);
BOOL  WINAPI IsExistFile(LPCSTR Path);
HFILE WINAPI JFAT_Open(LPCSTR FullPath, int OpenMode);
HFILE WINAPI JFAT_Create(LPCSTR FullPath, int Attr);
LONG  WINAPI JFAT_Seek(HFILE hFile, LONG Pos, int Origin);
LONG  WINAPI JFAT_Read(HFILE hFile, LPVOID Buff, UINT ReadByteSize);
LONG  WINAPI JFAT_Write(HFILE hFile, LPCVOID Buff, UINT WriteByteSize);
VOID  WINAPI JFAT_Close(HFILE hFile);
DWORD WINAPI CreateNewFile(LPCSTR FileName, int Attr, DWORD FileSize);
BOOL  WINAPI JFAT_DeleteFile(LPCSTR FilePath);
LONG  WINAPI JFAT_GetFileSize(HFILE hFile);
BOOL  WINAPI JFAT_CreateDirectory(LPCSTR FullPath);
BOOL  WINAPI JFAT_SetFileTime(HFILE hFile, JTIME CreationTime, JTIME LastAccessTime, JTIME LastWriteTime);
DWORD WINAPI JFAT_GetNoUsePos(LPCSTR DriveRootPath);
BOOL  WINAPI JFAT_GetInfo(UINT Lun, int *lpFatType, DWORD *lpTotalScts, DWORD *lpFreeScts);


#ifndef _WIN32
#define _lopen              JFAT_Open
#define _lcreat             JFAT_Create
#define _llseek             JFAT_Seek
#define _lread              JFAT_Read
#define _lwrite             JFAT_Write
#define _lclose             JFAT_Close
#define GetFileAttributes   JFAT_GetFileAttributes
#define DeleteFile          JFAT_DeleteFile
#define GetFileSize         JFAT_GetFileSize
#define FindFirstFile       JFAT_FindFirstFile
#define FindNextFile        JFAT_FindNextFile
#define CreateDirectory     JFAT_CreateDirectory
#define SetFileTime         JFAT_SetFileTime
#else
#undef  WIN32_FIND_DATA
#define WIN32_FIND_DATA     JFAT_FIND_DATA
#endif

struct _DISKCONTROLBLOCK;
typedef struct _JFAT_FIND_DATA
    {
    DWORD DirClust;         //지금 처리중인 클러스터 번호
    UINT  SctOfsInClust;    //클러스터 중에 섹터 Offset
    UINT  OfsInSct;         //섹터 내 DirEntry Offset (Byte단위)
    DWORD ClustStartSctNo;  //클러스터 시작섹터번호
    UINT  EntryBlockSctQty; //파일 엔트리 블럭 섹터수
    struct _DISKCONTROLBLOCK  *Dcb;
    BYTE  Eof;              //1이면 끝임
    CHAR  Wildcard[LFN_MAXLEN]; //L_FindNextFile()에서 사용
    //이 위는 내부에서 처리하기 위함
    DWORD dwFileAttributes;
    DWORD nFileSizeLow;
    //DWORD nFileSizeHigh;
    JTIME ftLastWriteTime;
    CHAR  cAlternateFileName[16];
    CHAR  cFileName[LFN_MAXLEN];
    BYTE  SctBuff[SUPPORTSECTORBYTES] ALIGN_END;    //SctBuff가 4로 나누어지는 어드레스에 배치되게 하기 위함
    } WIN32_FIND_DATA;

//WIN32_FIND_DATA.dwFileAttributes와 GetFileAttributes()의 리턴 값
#define FILE_ATTRIBUTE_READONLY     0x00000001
#define FILE_ATTRIBUTE_HIDDEN       0x00000002
#define FILE_ATTRIBUTE_SYSTEM       0x00000004
#define FILE_ATTRIBUTE_VOLUME       0x00000008
#define FILE_ATTRIBUTE_DIRECTORY    0x00000010
#define FILE_ATTRIBUTE_ARCHIVE      0x00000020
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400


WIN32_FIND_DATA* WINAPI JFAT_FindFirstFile(LPCSTR FullPath);
BOOL WINAPI JFAT_FindNextFile(WIN32_FIND_DATA *WFD);
#define FindClose   FreeMem

BOOL WINAPI JFAT_Formatting(LPCSTR DriveRootPath);
BOOL WINAPI JFAT_Init(UINT Lun, BOOL Verbose);

//GetLastError()의 리턴 값
#define JFAT_NOERROR            0
#define JFAT_FILENOTFOUND       1
#define JFAT_PATHNOTFOUND       2
#define JFAT_DISKACCESSERROR    3
#define JFAT_FATBROKENERROR     4
#define JFAT_DISKFULL           5
#define JFAT_DIRENTRYFULL       6
#define JFAT_INTERNALERROR      7
#define JFAT_INSUFFICIENTMEMORY 8
#define JFAT_LFNTOOLONG         9
#define JFAT_ALREADYEXISTS      10


#endif //__JFAT_H__

