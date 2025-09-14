///////////////////////////////////////////////////////////////////////////////
//                      FAT12/FAT16/FAT32 FileSystem
//
//                  ROM:12K, SRAM:3K (측정일 2022-05-14)
//
// 2022-03-17 한글 파일명 처리
// 2022-04-23 SUPPORT_UTF8==0 으로 하면 토탈커멘더로 ImageDisk에 넣은 LFN인식
// 2022-05-13 LFN 파일 생성
//
// 제약: 파일을 삭제할 때 파일명 문자수가 194(13*15)보다 크면 일부 LFN 엔트리가 삭제되지 않음
//       FAT12에서는 읽기만 지원함
///////////////////////////////////////////////////////////////////////////////

#include "JLIB.H"
#ifndef _WIN32
#include "DRIVER.H"
#include "JOS.H"
#endif
#include "MAIN.H"
#include "JFAT.H"
#include "MONITOR.H"


#define MEMOWNER_FindFirstFile  (MEMOWNER_JFAT+0)
#define MEMOWNER_JFAT_MakeLfn   (MEMOWNER_JFAT+1)


typedef struct _DIRENTRY
    {
    BYTE FileName[11];
    BYTE FileAttr;      //0B
    BYTE LfnType;       //0C 83엔트리에서는 NT Resource (보통0)
    BYTE LfnChkSum;     //0D 83엔트리에서는 Create Time Tenth (보통0)
    WORD CreateTime;    //0E Win95에서 추가
    WORD CreateDate;    //10 Win95에서 추가
    WORD AccessDate;    //12 Win95에서 추가
    WORD ClusterNoHi;   //14 Win95 OSR2에서 추가 (FAT32)
    WORD LastModiTime;  //16
    WORD LastModiDate;  //18
    WORD StartCluster;  //1A
    DWORD FileSize;     //1C
    } DIRENTRY;         //20

//DIRENTRY.파일명 첫자의 의미
#define DIRENTRY_END    0
#define DIRENTRY_ERASE  0xE5


#define FILE_ATTRIBUTE_LFN  (FILE_ATTRIBUTE_READONLY|FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM|FILE_ATTRIBUTE_VOLUME)


//AccessStorageBytes()의 Access 인자
#define DEVICE_READ     0
#define DEVICE_WRITE    1


#define FAT12_MAXQTY    0xFF5           //4085
#define FAT12_BAD       0xFF7
#define FAT12_EOF       0xFF8           //~0FFFh
#define FAT16_MAXQTY    0xFFF5          //65525
#define FAT16_BAD       0xFFF7
#define FAT16_EOF       0xFFF8          //~0FFFFh
#define FAT32_BAD       0xFFFFFF7
#define FAT32_EOF       0xFFFFFF8       //~0FFFFFFFh

#define FILEOPENSIGN    0xA5

typedef struct _DISKCONTROLBLOCK
    {
    BYTE  FatType;              //12 or 16 or 32
    BYTE  Lun;
    UINT  RootDirSctQty;
    DWORD VolumeStartSctNo;
    DWORD RootClustNo;
    DWORD FirstFatSctNo;
    DWORD SecondFatSctNo;
    DWORD ClusterStartSctNo;
    DWORD SctsPerCluster;
    DWORD RootDirSctNo;
    DWORD TotalClusters;
    DWORD DiskSectorQty;        //디스크의 총 섹터수
    DWORD LastFreeClustNo;
    DWORD BPB_LastFreeClustNo;  //ReadVolID()에서 BPB에 저장된 기록할 위치
    BYTE  FatCacheDirtyFg;      //실제 Disk 내용과 Cache 내용이 다른 경우
    DWORD FatCachedSctNo;       //-1이면 캐쉬되지 않은 것임
    #ifdef USE_JOS
    JOS_EVENT* DCB_Sem;
    #endif
    BYTE  FatCacheBuff[SUPPORTSECTORBYTES] ALIGN_END;   //DMA 전송시 버퍼의 시작번지는 4로 나누어져야 함
    BYTE  SctBuffer[SUPPORTSECTORBYTES];
    } DISKCONTROLBLOCK;

static DISKCONTROLBLOCK DiskControlBlock[SUPPORTDISKMAX];


typedef struct _FILECONTROLBLOCK
    {
    BYTE  FileOpened;           //1이면 Open되어 있는 것임
    //BYTE  FileAttr;
    BYTE  OpenMode;
    DWORD StartCluster;         //0인 경우는 파일크기가 0일 때임
    DWORD AccCluster;           //현재 파일 포인터가 위치한 클러스터
    DWORD PrevAccCluster;       //기록할 때 AccCluster==Eof 인경우 클러스트를 추가할당하여 연결할 때 필요
    DWORD FilePointer;
    DWORD FileSize;
    DWORD DESctNo, DESctOfs;    //파일을 생성한 경우 크기를 변경해야 하므로 DE의 위치를 보관함
    DISKCONTROLBLOCK *Dcb;
    } FILECONTROLBLOCK;

static FILECONTROLBLOCK FileCtrlBlock[OPENFILEQTY];



#ifdef _WIN32
#pragma pack(push,1)
#endif

//-----------------------------------------------------------------------------
//              FAT16 Boot Record (BiosPrmBlock)
//-----------------------------------------------------------------------------
typedef struct _BPB_F16
    {
    BYTE JumpStartBoot[3];      //00
    BYTE BootOsSign[8];         //03 'MSWIN4.0'
    WORD BytesPerSector;        //0B 512
    BYTE SectorsPerCluster;     //0D 64
    WORD SystemUseSctNo;        //0E 1  - 시스템이 사용하는 섹터수, FAT의 시작을 가리킴
    BYTE FATCopys;              //10 2  - FAT수
    WORD RootDirEntrys;         //11 512 - 최대 메인 디렉터리 파일수
    WORD OldTotalSectors;       //13 0  - 디스크의 총 섹터 (0=사용안함)
    BYTE MediaSign;             //15 0F8h  - 메디아종류
    WORD SectorsPerFAT;         //16 192
    WORD SectorsPerHead;        //18 63
    WORD HeadQty;               //1A 128
    DWORD StartRtvSctNo;        //1C 63 - 이 드라이브의 시작 섹터번호(하드디스크 전체를 기준, 처음은 0), 이 부트레코드를 가리킴
    DWORD BigTotalSectors;      //20 3144897 - 이 드라이브의 총섹터수 (이 부트섹터부터 ...)
    BYTE PhyDiskNo;             //24 80h  - 물리적인 드라이브 번호
    BYTE Unknown1;              //25 0
    BYTE ExtBootSign;           //26 29h - 확장 부트레코더 사인
    DWORD SerialNo;             //27 0C210FF5h
    CHAR VolumeLabel[11];       //2B 'No Name    '
    CHAR FileSystemSign[8];     //36 'FAT16   '
    BYTE BootProcCode[448];     //3E
    WORD BootSctValidSign;      //1FE 0AA55h
    }PACK_STRUCT BPB_F16;



//------------------------------------------------------------------------------
//              FAT32 Boot Record (BiosPrmBlock)
//------------------------------------------------------------------------------
typedef struct _BPB_F32
    {
    BYTE JumpStartBoot[3];      //00
    BYTE BootOsSign[8];         //03 'MSWIN4.0'
    WORD BytesPerSector;        //0B 512
    BYTE SectorsPerCluster;     //0D 64
    WORD SystemUseSctNo;        //0E 1  - 시스템이 사용하는 섹터수, FAT의 시작을 가리킴
    BYTE FATCopys;              //10 2  - FAT수
    WORD RootDirEntrys;         //11 0 - 최대 메인 디렉터리 파일수 (FAT32에서는 사용안함)
    WORD OldTotalSectors;       //13 0  - 디스크의 총 섹터 (0=사용안함)
    BYTE MediaSign;             //15 0F8h  - 메디아종류
    WORD SectorsPerFAT;         //16 0 (FAT32에서는 사용안함)
    WORD SectorsPerHead;        //18 63
    WORD HeadQty;               //1A 128
    DWORD StartRtvSctNo;        //1C 63 - 이 드라이브의 시작 섹터번호(하드디스크 전체를 기준, 처음은 0), 이 부트레코드를 가리킴
    DWORD BigTotalSectors;      //20 3144897 - 이 드라이브의 총섹터수 (이 부트섹터부터 ...)
    //여기부터는 FAT16과 완전히 다름
    DWORD BigSctPerFAT;         //24 6475
    BYTE ActiveFATNo;           //28 0 (0-15) - BPB구조에서는 아래와 WORD로 BPB_ExtFlags로 명시
    BYTE ExtFlagsHigh;          //29 0
    WORD FS_Version;            //2A 0 - 이 값이 0이 아니면 오류처리
    DWORD RootDirClustNo;       //2C 2
    WORD FSInfoSctQty;          //30 1
    WORD BkUpBootSctOfs;        //32 6
    DWORD Reserved4;            //34 0
    DWORD Reserved5;            //38 0
    DWORD Reserved6;            //3C 0
    //FAT16에도 있었지만 위치가 바뀜
    BYTE FS32PhyDiskNo;         //40 80h - 물리적인 드라이브 번호
    BYTE FS32Unknown1;          //41 0
    BYTE FS32ExtBootSign;       //42 29h - 확장 부트레코더 사인
    DWORD FS32SerialNo;         //43 37231BF5h
    CHAR FS32VolumeLabel[11];   //47 'No Name    '
    CHAR FS32Sign[8];           //52 'FAT32   '
    BYTE FS32BootCode[420];     //5A
    WORD BootSctValidSign;      //1FE 0AA55h
    }PACK_STRUCT BPB_F32;


#ifdef _WIN32
#pragma pack()
#endif





//-----------------------------------------------------------------------------
//      DosFileTime 형식으로 리턴
//-----------------------------------------------------------------------------
LOCAL(DWORD) SystemTimeToDosFileTime(CONST SYSTEMTIME *ST)
    {
    return ST->wYear<1980 ? 0x00210000:
           ((DWORD)(ST->wYear-1980)<<25)|((DWORD)ST->wMonth<<21)|((DWORD)ST->wDay<<16)|
           ((DWORD)ST->wHour<<11)|((DWORD)ST->wMinute<<5)|((DWORD)ST->wSecond>>1);
    }




//-----------------------------------------------------------------------------
//      JTIME시간을 DosFileTime으로 변환
//-----------------------------------------------------------------------------
LOCAL(DWORD) JTimeToDosDateTime(JTIME JTime)
    {
    SYSTEMTIME ST;

    UnpackTotalSecond(&ST, JTime);
    return SystemTimeToDosFileTime(&ST);
    }




//-----------------------------------------------------------------------------
//      DosFileTime을 JTIME시간으로 변환
//-----------------------------------------------------------------------------
LOCAL(JTIME) DosFileTimeToJTime(DWORD DosTime)
    {
    int Y,M,D, Hour,Min,Sec;

    Y=((DosTime>>25)&0x7F)+1980;
    M=(DosTime>>21)&0x0F;
    D=(DosTime>>16)&0x1F;
    Hour=(DosTime>>11)&0x1F;
    Min=(DosTime>>5)&0x3F;
    Sec=(DosTime<<1)&0x3E;
    return (GetTotalDays(Y, M, D)-730120)*86400 + Hour*3600 + Min*60 + Sec;     //730120=GetTotalDays(2000,1,1) / 86400=24*60*60
    }




//-----------------------------------------------------------------------------
//      멀티 쓰레드 환경에서 재진입을 막기 위한 함수
//-----------------------------------------------------------------------------
LOCAL(VOID) JFAT_Lock(CONST DISKCONTROLBLOCK *Dcb)
    {
    #ifdef USE_JOS
    JOSSemPend(Dcb->DCB_Sem, INFINITE);
    #endif
    }



//-----------------------------------------------------------------------------
//          LFN[롱파일네임 시스템에서 쓰는] 체크썸을 구합니다
//-----------------------------------------------------------------------------
LOCAL(VOID) JFAT_Unlock(CONST DISKCONTROLBLOCK *Dcb)
    {
    #ifdef USE_JOS
    if (Dcb) JOSSemPost(Dcb->DCB_Sem);
    #endif
    }



//-----------------------------------------------------------------------------
//          LFN[롱파일네임 시스템에서 쓰는] 체크썸을 구합니다
//-----------------------------------------------------------------------------
UINT WINAPI GetLfnChkSum(LPCBYTE lpMem, UINT Size)
    {
    int Sum=0;

    while (Size--)
        {
        if (Sum & 1) {Sum>>=1; Sum|=0x80;}
        else         Sum>>=1;
        Sum+=*lpMem++;
        Sum&=0xFF;
        }
    return Sum;
    }



//-----------------------------------------------------------------------------
//      문자열을 뒤집음
//-----------------------------------------------------------------------------
VOID WINAPI ReverseString(LPSTR Buff, int Len)
    {
    int   T;
    LPSTR End;

    if (Len<0) Len=lstrlen(Buff);
    End=Buff+Len;
    for (Len>>=1; Len>0; Len--)
        {
        T=*--End;
        *End=*Buff;
        *Buff++=T;
        }
    }




//-----------------------------------------------------------------------------
//          DirEntry에서 LFN의 문자를 끄집어 냅니다
//-----------------------------------------------------------------------------
VOID WINAPI CollectLfn(DIRENTRY *DE, LPSTR Lfn, int BuffSize)
    {
    int Cha;
    LPCBYTE LfnOfs;
    static CONST BYTE LfnCharPos[]={0x1E,0x1C,0x18,0x16,0x14,0x12,0x10,0x0E,9,7,5,3,1,0};   //맨뒤 0은 끝이라는 의미

    LfnOfs=LfnCharPos;
    for (;;)
        {
        if ((Cha=*LfnOfs++)==0) break;
        Cha=PeekW(DE->FileName+Cha);
        if (Cha==0) continue;       //Lfn에는 파일명 맨뒤에 \0까지 저장함
        if (Cha==0xFFFF) continue;  //Unicode 0xFFFF는 더이상 없음을 의미
        if (Cha<=0x7F)
            {
            if (BuffSize>1) {*Lfn++=Cha; BuffSize--;}
            }
        else if (Cha<=0x7FF)
            {
            if (BuffSize>2)
                {
                *Lfn++=(BYTE)((Cha&0x3F)|0x80);
                *Lfn++=(BYTE)((Cha>>6)|0xC0);
                BuffSize-=2;
                }
            }
        else{
            if (BuffSize>3)
                {
                *Lfn++=(BYTE)((Cha&0x3F)|0x80);
                *Lfn++=(BYTE)(((Cha>>6)&0x3F)|0x80);
                *Lfn++=(BYTE)((Cha>>12)|0xE0);
                BuffSize-=3;
                }
            }
        }
    if (BuffSize>0) *Lfn=0;
    }




//-----------------------------------------------------------------------------
//      토탈커멘더로 이미지 안에 파일을 넣은 경우 비호환으로 LFN이 만들어짐
//-----------------------------------------------------------------------------
VOID WINAPI CollectLfnII(DIRENTRY *DE, LPSTR Lfn, int BuffSize)
    {
    int Cha;
    LPCBYTE LfnOfs;
    static CONST BYTE LfnCharPos[]={1,3,5,7,9,0x0E,0x10,0x12,0x14,0x16,0x18,0x1C,0x1E,0};   //맨뒤 0은 끝이라는 의미

    LfnOfs=LfnCharPos;
    for (;;)
        {
        if ((Cha=*LfnOfs++)==0) break;
        Cha=PeekW(DE->FileName+Cha);
        if (Cha==0) break;              //Lfn에는 파일명 맨뒤에 \0까지 저장함
        if (Cha==0xFFFF) break;         //Unicode 0xFFFF는 더이상 없음을 의미
        if (BuffSize>1) {*Lfn++=Cha; BuffSize--;}
        }
    if (BuffSize>0) *Lfn=0;
    }




//-----------------------------------------------------------------------------
//          공백으로 채워진 83파일명을 보통 파일명으로 변환
//-----------------------------------------------------------------------------
VOID WINAPI Conv83toFName(LPSTR FileName, LPCBYTE _83FName)
    {
    int I, Cha;

    for (I=0; I<11; I++)
        {
        if ((Cha=_83FName[I])==' ')
            {
            if (I>=8) break;
            I=7;
            continue;
            }
        if (I==8) *FileName++='.';
        *FileName++=Cha;
        }
    *FileName=0;
    }



//-----------------------------------------------------------------------------
//          파일명을 공백으로 채워진 83파일명으로 변환
//-----------------------------------------------------------------------------
VOID WINAPI ConvFileNameTo83Name(LPBYTE _83FName, LPCSTR FileName)
    {
    int I, Cha;
    LPCSTR lpExt;

    lpExt=GetFileExtNameLoc((LPSTR)FileName);
    for (I=0; I<11; I++)
        {
        if (I<8)
            {
            Cha=*(LPCBYTE)FileName;
            if (Cha=='.' || Cha==0) Cha=' '; else FileName++;
            }
        else{
            Cha=*(LPCBYTE)lpExt;
            if (Cha==0) Cha=' '; else lpExt++;
            }
        _83FName[I]=UpCaseCha(Cha);
        }
    }



//-----------------------------------------------------------------------------
//          FullPath에서 '/'이 나올 때까지 폴더명을 복사해줌
//-----------------------------------------------------------------------------
LPCSTR WINAPI CatchFileName(LPCSTR lp, LPSTR Buff, int BuffSize)
    {
    int Cha;

    for (;;)
        {
        if ((Cha=*lp++)==0) {lp--; break;}
        if (Cha=='/') break;
        if (BuffSize>1) {*Buff++=Cha; BuffSize--;}
        }
    if (BuffSize>0) *Buff=0;
    return lp;
    }




//-----------------------------------------------------------------------------
//          Disk를 바이트 단위로 읽기/쓰기
//-----------------------------------------------------------------------------
LOCAL(BOOL) AccessStorageBytes(CONST DISKCONTROLBLOCK *Dcb, int Access, DWORD SctNo, UINT OfsInSct, LPBYTE Buff, int AccBytes)
    {
    BOOL Rslt;
    UINT Lun;
    static BYTE TmpBuff[SUPPORTSECTORBYTES];

    Lun=Dcb->Lun;
    if (Access==DEVICE_READ)
        {
        Rslt=STORAGE_Read(Lun, TmpBuff, SctNo, 1);
        CopyMemory(Buff, TmpBuff+OfsInSct, AccBytes);
        }
    else{   //DEVICE_WRITE
        Rslt=TRUE;
        if (OfsInSct!=0 || AccBytes!=SUPPORTSECTORBYTES)
            Rslt=STORAGE_Read(Lun, TmpBuff, SctNo, 1);

        if (Rslt!=FALSE)
            {
            CopyMemory(TmpBuff+OfsInSct, Buff, AccBytes);
            Rslt=STORAGE_Write(Lun, TmpBuff, SctNo, 1);
            }
        }
    if (Rslt==FALSE) Printf("%sStorageBytes(SDAddr=%X, OfsInSct=%X, AccBytes=%u) Error" CRLF, Access==DEVICE_READ ? "Read":"Write", SctNo, OfsInSct, AccBytes);
    (VOID)Lun;      //STORAGE_Read()를 #define으로 연결할 때 Lun이 쓰이기 않으면 경고가 발생함
    return Rslt;
    }



//-----------------------------------------------------------------------------
//      클러스터 번호를 섹터번호로 변환
//-----------------------------------------------------------------------------
LOCAL(DWORD) ClusterNoToSectorNo(CONST DISKCONTROLBLOCK *Dcb, DWORD ClusterNo)
    {
    return (ClusterNo-2)*Dcb->SctsPerCluster + Dcb->ClusterStartSctNo;
    }



//-----------------------------------------------------------------------------
//      주어진 클러스터 안에서 Data를 읽거나 씀
//-----------------------------------------------------------------------------
LOCAL(BOOL) AccessCluster(CONST DISKCONTROLBLOCK *Dcb, int Access, DWORD ClusterNo, UINT OfsInCluster, LPBYTE Buff, UINT ToAccBytes)
    {
    UINT  AccBytes, OfsInSct;
    DWORD SctNo;
    BOOL Rslt=FALSE;

    SctNo=ClusterNoToSectorNo(Dcb, ClusterNo) + OfsInCluster/SUPPORTSECTORBYTES;
    OfsInSct=OfsInCluster%SUPPORTSECTORBYTES;

    while (ToAccBytes>0)
        {
        AccBytes=GetMin(ToAccBytes, SUPPORTSECTORBYTES-OfsInSct);
        if ((Rslt=AccessStorageBytes(Dcb, Access, SctNo, OfsInSct, Buff, AccBytes))==FALSE) break;

        Buff+=AccBytes;
        ToAccBytes-=AccBytes;
        SctNo++;
        OfsInSct=0;
        }
    return Rslt;
    }




//-----------------------------------------------------------------------------
//      변경된 FAT이면 기록함
//-----------------------------------------------------------------------------
LOCAL(VOID) FlushChcheBuff(DISKCONTROLBLOCK *Dcb)
    {
    if (Dcb->FatCacheDirtyFg)
        {
        STORAGE_Write(Dcb->Lun, Dcb->FatCacheBuff, Dcb->FatCachedSctNo, 1);
        if (Dcb->SecondFatSctNo!=0)
            STORAGE_Write(Dcb->Lun, Dcb->FatCacheBuff, Dcb->FatCachedSctNo-Dcb->FirstFatSctNo+Dcb->SecondFatSctNo, 1);

        Dcb->FatCacheDirtyFg=0;
        //Printf("Flushed FAT" CRLF);
        }
    }




//-----------------------------------------------------------------------------
//      주어진 FAT의 다음 연결된 FAT을 구함 (엔트리의 끝이면 TRUE 리턴)
//
//  FAT12 FAT Entry
//
//  F0 FF FF  | 03 40 00  | 05 60 00  |  07 80 00  |  09 A0 00  |  0B
//  FFF0 FFFF   4003 0040   6005 0060   8007 0080    A009 00A0
//  &FFF >>=4   &FFF >>=4   &FFF >>=4   &FFF >>=4    &FFF >>=4
//  =FF0 =FFF   =003 =004   =005 =006   =007 =008    =009 =00A
//-----------------------------------------------------------------------------
LOCAL(BOOL) GetNextCluster(DISKCONTROLBLOCK *Dcb, INOUT DWORD *lpFatEntry)
    {
    UINT  SctNo, Ofs, FirstFatOfs;
    DWORD Eof, CurrEntry, NextEntry;
    LPCBYTE lp;

    FirstFatOfs=Dcb->FirstFatSctNo*SUPPORTSECTORBYTES;
    CurrEntry=*lpFatEntry;
    if (Dcb->FatType==16)
        {
        CurrEntry<<=1;
        Eof=FAT16_EOF;
        goto ReadEntry;
        }
    else if (Dcb->FatType==32)
        {
        if (CurrEntry==0) CurrEntry=Dcb->RootClustNo;
        CurrEntry<<=2;
        Eof=FAT32_EOF;

        ReadEntry:
        if ((SctNo=UDivMod(FirstFatOfs+CurrEntry, SUPPORTSECTORBYTES, &Ofs))!=Dcb->FatCachedSctNo)
            {
            FlushChcheBuff(Dcb);
            STORAGE_Read(Dcb->Lun, Dcb->FatCacheBuff, SctNo, 1);
            Dcb->FatCachedSctNo=SctNo;
            }
        lp=Dcb->FatCacheBuff+Ofs;
        if (Dcb->FatType==16) NextEntry=*(WORD*)lp;
        else                  NextEntry=*(DWORD*)lp;
        }
    else{
        Eof=FAT12_EOF;

        if ((SctNo=UDivMod((CurrEntry*3>>1)+FirstFatOfs, SUPPORTSECTORBYTES, &Ofs))!=Dcb->FatCachedSctNo)
            {
            FlushChcheBuff(Dcb);
            STORAGE_Read(Dcb->Lun, Dcb->FatCacheBuff, SctNo, 1);
            Dcb->FatCachedSctNo=SctNo;
            }

        if (SUPPORTSECTORBYTES-Ofs>=2) NextEntry=PeekW(Dcb->FatCacheBuff+Ofs);
        else{
            NextEntry=Dcb->FatCacheBuff[Ofs];
            Dcb->FatCachedSctNo++;
            FlushChcheBuff(Dcb);
            STORAGE_Read(Dcb->Lun, Dcb->FatCacheBuff, Dcb->FatCachedSctNo, 1);
            NextEntry|=Dcb->FatCacheBuff[0]<<8;
            }
        if (CurrEntry&1) NextEntry>>=4;
        NextEntry&=0xFFF;
        }
    *lpFatEntry=NextEntry;
    return NextEntry>=Eof;
    }




//-----------------------------------------------------------------------------
//      주어진 FAT의 다음 연결된 FAT을 기록함
//              엔트리의 끝이면 TRUE리턴
//-----------------------------------------------------------------------------
LOCAL(BOOL) SetFatEntry(DISKCONTROLBLOCK *Dcb, DWORD CurrEntry, DWORD NewEntry, DWORD *lpNextEntry)
    {
    BOOL Rslt=TRUE;
    UINT SctNo, Ofs;
    DWORD Eof, NextEntry=0;
    LPBYTE lp;

    if (Dcb->FatType==16)
        {
        CurrEntry<<=1;
        Eof=FAT16_EOF;
        goto ReadEntry;
        }
    else if (Dcb->FatType==32)
        {
        CurrEntry<<=2;
        Eof=FAT32_EOF;

        ReadEntry:
        if ((SctNo=UDivMod(Dcb->FirstFatSctNo*SUPPORTSECTORBYTES+CurrEntry, SUPPORTSECTORBYTES, &Ofs))!=Dcb->FatCachedSctNo)
            {
            FlushChcheBuff(Dcb);
            STORAGE_Read(Dcb->Lun, Dcb->FatCacheBuff, SctNo, 1);
            Dcb->FatCachedSctNo=SctNo;
            }

        lp=Dcb->FatCacheBuff+Ofs;
        if (Dcb->FatType==16)
            {
            NextEntry=*(WORD*)lp;
            *(WORD*)lp=(WORD)NewEntry;
            }
        else{
            NextEntry=*(DWORD*)lp;
            *(DWORD*)lp=NewEntry;
            }
        Dcb->FatCacheDirtyFg=1;
        Rslt=NextEntry>=Eof;
        }

    if (lpNextEntry) *lpNextEntry=NextEntry;
    return Rslt;
    }




typedef struct _FILENAMEFINDRESULT
    {
    UINT LfnFirstLocSctNo;      //첫번째 LFN을 발견한 섹터번호, FindSectorNo와 다를 수도 있음, LFN을 안가진 파일인 경우 0임
    UINT LfnFirstLocSctOfs;     //첫번째 LFN을 발견한 섹터내의 Offset
    UINT FindSectorNo;          //찾은 파일명이 있는 섹터번호
    UINT FindSectorOfs;         //찾은 파일명 섹터 내 Offset
    UINT LfnID;                 //Lfn파일명을 생성할 때 ShortName을 만들 재료
    } FILENAMEFINDRESULT;




//-----------------------------------------------------------------------------
//      Directory Entry를 에서 파일명을 찾음
//
//      'Abcdefghijklmnopqrstuvwxyz.123'의 Entry 예
//
// 43 2E 00 31 00 32 00 33 - 00 00 00 0F 00 2A FF FF C. 1 2 3     *
// FF FF FF FF FF FF FF FF - FF FF 00 00 FF FF FF FF
// 02 6E 00 6F 00 70 00 71 - 00 72 00 0F 00 2A 73 00  n o p q r   *s
// 74 00 75 00 76 00 77 00 - 78 00 00 00 79 00 7A 00 t u v w x   y z
// 01 41 00 62 00 63 00 64 - 00 65 00 0F 00 2A 66 00  A b c d e   *f
// 67 00 68 00 69 00 6A 00 - 6B 00 00 00 6C 00 6D 00 g h i j k   l m
// 41 42 43 44 45 46 7E 31 - 31 32 33 20 00 2D D3 5A ABCDEF~1123
// 52 37 52 37 00 00 D4 5A - 52 37 00 00 00 00 00 00
//-----------------------------------------------------------------------------
LOCAL(DIRENTRY*) SearchFileName(DISKCONTROLBLOCK *Dcb, LPCSTR ToFindFN, INOUT DWORD *DirCluster, FILENAMEFINDRESULT *FI, LPBYTE SctBuff)
    {
    int   I, LfnSeqNo=0, FirstCha, LfnSum=0;
    UINT  SctOfs, SctOfsInClust, ClustStart, BlockSctQty, SctNo;
    LPSTR lpExt;
    DIRENTRY *DE;
    CHAR  Lfn[LFN_MAXLEN], ShotFName[16];

    #if SUPPORT_UTF8==0
    (VOID)LfnSeqNo; (VOID)LfnSum;
    #endif
    ZeroMem(FI, sizeof(FILENAMEFINDRESULT));
    lpExt=GetFileExtNameLoc((LPSTR)ToFindFN);

    Lfn[0]=0;
    for (;;)
        {
        if (*DirCluster==0)     //RootDir
            {
            ClustStart=Dcb->RootDirSctNo;
            BlockSctQty=Dcb->RootDirSctQty;
            }
        else{
            ClustStart=ClusterNoToSectorNo(Dcb, *DirCluster);
            BlockSctQty=Dcb->SctsPerCluster;
            }

        //Printf("C=%u CL=%u S=%u '%s'" CRLF, *DirCluster, BlockSctQty, ClustStart, ToFindFN);
        for (SctOfsInClust=0; SctOfsInClust<BlockSctQty; SctOfsInClust++)
            {
            if (STORAGE_Read(Dcb->Lun, SctBuff, SctNo=ClustStart+SctOfsInClust, 1)==FALSE) {ErExit: DE=NULL; goto ProcExit;}

            for (SctOfs=0; SctOfs<SUPPORTSECTORBYTES; SctOfs+=sizeof(DIRENTRY))
                {
                DE=(DIRENTRY*)(SctBuff+SctOfs);

                FirstCha=DE->FileName[0];
                if (FirstCha==DIRENTRY_END) {DE=NULL; goto ProcExit;}
                if (FirstCha==DIRENTRY_ERASE) goto ClearLfnCont;
                if (DE->FileAttr==FILE_ATTRIBUTE_LFN)
                    {
                    #if SUPPORT_UTF8!=0                 //토탈커멘더로 DiskImage안에 파일을 넣은 경우
                    if (FirstCha & 0x40)                //첫 Lfn엔트리
                        {
                        FI->LfnFirstLocSctNo=SctNo;
                        FI->LfnFirstLocSctOfs=SctOfs;

                        LfnSeqNo=FirstCha & 0x1F;       //다음 Lfn엔트리의 Seq번호
                        LfnSum=DE->LfnChkSum;
                        Lfn[0]=0;
                        }
                    else{
                        if (FirstCha!=LfnSeqNo || DE->LfnChkSum!=LfnSum) goto ClearLfnCont;
                        }
                    I=lstrlen(Lfn);
                    CollectLfn(DE, Lfn+I, LFN_MAXLEN-I);
                    LfnSeqNo--;
                    #else
                    I=lstrlen(Lfn);
                    CollectLfnII(DE, Lfn+I, LFN_MAXLEN-I);
                    #endif
                    }
                else{
                    #if SUPPORT_UTF8!=0
                    if (Lfn[0]!=0)
                        {
                        if (GetLfnChkSum(DE->FileName, 11)==LfnSum) ReverseString(Lfn, -1);
                        else Lfn[0]=0;
                        }
                    #endif
                    Conv83toFName(ShotFName, DE->FileName);

                    if (ToFindFN[0]=='~' && ToFindFN[1]=='*')
                        {
                        if (ShotFName[0]=='~' && lstrcmpi(GetFileExtNameLoc(ShotFName), lpExt)==0)
                            FI->LfnID=GetMax(FI->LfnID, AtoN(ShotFName+1, NULL));   //AtoI는 앞에 0이 포함된 수는 8진수로 봄
                        }
                    else if (lstrcmpi(Lfn, ToFindFN)==0 || lstrcmpi(ShotFName, ToFindFN)==0)
                        {
                        FI->FindSectorNo=SctNo;
                        FI->FindSectorOfs=SctOfs;
                        goto ProcExit;
                        }

                    ClearLfnCont:
                    FI->LfnFirstLocSctNo=0;
                    Lfn[0]=0;
                    LfnSum=0;
                    LfnSeqNo=0;
                    }
                }
            }
        if (*DirCluster==0 && Dcb->FatType!=32) goto ErExit;
        if (GetNextCluster(Dcb, DirCluster)!=0) goto ErExit;
        }

    ProcExit:
    return DE;
    }




//-----------------------------------------------------------------------------
//      파일이나 폴더를 오픈합니다 (리턴값은 SctBuff 내부의 위치임)
//-----------------------------------------------------------------------------
#define OPENDIR_DIR     0
#define OPENDIR_FILE    1
LOCAL(DIRENTRY*) OpenDir(DISKCONTROLBLOCK *Dcb, LPCSTR FullPath, LPBYTE SctBuff, FILENAMEFINDRESULT *FI, int Mode)
    {
    DWORD DirCluster;
    DIRENTRY *DE;
    CHAR  FName[LFN_MAXLEN];

    DirCluster=0;   //루트 클러스터
    for (;;)
        {
        FullPath=CatchFileName(FullPath, FName, LFN_MAXLEN);
        if ((DE=SearchFileName(Dcb, FName, &DirCluster, FI, SctBuff))==NULL)
            {
            if (Mode!=OPENDIR_DIR && FullPath[0]==0) FI->LfnID++; else FI->LfnID=0;
            break;
            }
        if (Mode==OPENDIR_DIR)
            {
            if (SearchCha(FullPath, '/')<0) break;
            }
        else{
            if (FullPath[0]==0) break;
            }

        if ((DE->FileAttr & FILE_ATTRIBUTE_DIRECTORY)==0) goto Err;
        if ((DirCluster=(DE->ClusterNoHi<<16)+DE->StartCluster)==0) {Err: DE=NULL; break;}
        }
    return DE;
    }




//-----------------------------------------------------------------------------
//      DIRENTRY 다음 위치로 이동함
//-----------------------------------------------------------------------------
LOCAL(VOID) MoveNextDirEntry(DISKCONTROLBLOCK *Dcb, WIN32_FIND_DATA *WFD)
    {
    if ((WFD->OfsInSct+=sizeof(DIRENTRY))>=SUPPORTSECTORBYTES)
        {
        WFD->OfsInSct=0;
        if (++WFD->SctOfsInClust>=WFD->EntryBlockSctQty)
            {
            WFD->SctOfsInClust=0;

            if (WFD->DirClust==0 && Dcb->FatType!=32) WFD->Eof=1;
            else{
                if (GetNextCluster(Dcb, &WFD->DirClust)!=0) WFD->Eof=1;
                else{
                    WFD->ClustStartSctNo=ClusterNoToSectorNo(Dcb, WFD->DirClust);
                    WFD->EntryBlockSctQty=Dcb->SctsPerCluster;
                    }
                }
            }
        }
    }



//-----------------------------------------------------------------------------
//      Directory Entry를 에서 파일명을 찾음
//-----------------------------------------------------------------------------
LOCAL(BOOL) L_FindNextFile(DISKCONTROLBLOCK *Dcb, WIN32_FIND_DATA *WFD)
    {
    int  I, Rslt=FALSE, LfnSeqNo=0, FirstCha, LfnSum=0;
    DIRENTRY *DE;

    #if SUPPORT_UTF8==0
    (VOID)LfnSeqNo; (VOID)LfnSum;
    #endif

    WFD->cFileName[0]=0;
    while (WFD->Eof==0)
        {
        if (WFD->OfsInSct==0) STORAGE_Read(Dcb->Lun, WFD->SctBuff, WFD->ClustStartSctNo+WFD->SctOfsInClust, 1);

        DE=(DIRENTRY*)(WFD->SctBuff+WFD->OfsInSct);
        FirstCha=DE->FileName[0];
        if (FirstCha==DIRENTRY_END) {WFD->Eof=1; goto ProcExit;}
        if (DE->FileAttr==FILE_ATTRIBUTE_LFN)
            {
            #if SUPPORT_UTF8!=0
            if (FirstCha & 0x40)                //첫 Lfn엔트리
                {
                LfnSeqNo=FirstCha & 0x1F;       //다음 Lfn엔트리의 Seq번호
                LfnSum=DE->LfnChkSum;
                WFD->cFileName[0]=0;
                }
            else{
                if (FirstCha!=LfnSeqNo || DE->LfnChkSum!=LfnSum) goto ClearLfnCont;
                }

            I=lstrlen(WFD->cFileName);
            CollectLfn(DE, WFD->cFileName+I, LFN_MAXLEN-I);
            LfnSeqNo--;
            #else                               //토탈커멘더로 DiskImage안에 파일을 넣은 경우
            I=lstrlen(WFD->cFileName);
            CollectLfnII(DE, WFD->cFileName+I, LFN_MAXLEN-I);
            #endif
            }
        else{
            if (FirstCha!=DIRENTRY_ERASE)
                {
                #if SUPPORT_UTF8!=0
                if (WFD->cFileName[0]!=0)
                    {
                    if (GetLfnChkSum(DE->FileName, 11)==LfnSum) ReverseString(WFD->cFileName, -1);
                    else WFD->cFileName[0]=0;
                    }
                #endif
                Conv83toFName(WFD->cAlternateFileName, DE->FileName);
                if (WFD->Wildcard[0]!=0)
                    {
                    if (ChkWildcardFileName(WFD->cAlternateFileName, WFD->Wildcard)==FALSE &&
                        ChkWildcardFileName(WFD->cFileName, WFD->Wildcard)==FALSE) {WFD->cAlternateFileName[0]=0; goto ClearLfnCont;}
                    }
                if (WFD->cFileName[0]==0) lstrcpy(WFD->cFileName, WFD->cAlternateFileName); //8.3파일명만 존재하는 경우
                WFD->ftLastWriteTime=DosFileTimeToJTime((DE->LastModiDate<<16)|DE->LastModiTime);
                WFD->dwFileAttributes=DE->FileAttr;
                WFD->nFileSizeLow=DE->FileSize;
                MoveNextDirEntry(Dcb, WFD);
                Rslt++;
                goto ProcExit;
                }
            ClearLfnCont:
            WFD->cFileName[0]=0;
            LfnSum=0;
            LfnSeqNo=0;
            }
        MoveNextDirEntry(Dcb, WFD);
        }

    ProcExit:
    return Rslt;
    }


BOOL WINAPI JFAT_FindNextFile(WIN32_FIND_DATA *WFD)
    {
    BOOL Rslt;
    DISKCONTROLBLOCK *Dcb;

    Dcb=WFD->Dcb;
    JFAT_Lock(Dcb);
    Rslt=L_FindNextFile(Dcb, WFD);
    JFAT_Unlock(Dcb);
    return Rslt;
    }




//-----------------------------------------------------------------------------
//      주어진 Path에 해당하는 Dcb를 리턴
//-----------------------------------------------------------------------------
LOCAL(DISKCONTROLBLOCK*) CheckLunSpace(UINT Lun)
    {
    DISKCONTROLBLOCK *Dcb=NULL;

    if (Lun>=SUPPORTDISKMAX)
        {
        Printf("No support Disk, Check SUPPORTDISKMAX" CRLF);
        goto ProcExit;
        }
    if (STORAGE_IsReady(Lun)==FALSE)
        {
        Printf("%c: No Disk" CRLF, Lun+'A');
        goto ProcExit;
        }

    Dcb=DiskControlBlock+Lun;

    ProcExit:
    return Dcb;
    }



//-----------------------------------------------------------------------------
//      주어진 Path에 해당하는 Dcb를 리턴
//-----------------------------------------------------------------------------
LOCAL(DISKCONTROLBLOCK*) GetDCB(LPCSTR *lpFullPath, BOOL ChkNoNameFg, BOOL CheckFormat)
    {
    UINT DrvLett, Lun=0;
    LPCSTR FullPath;
    DISKCONTROLBLOCK *Dcb=NULL;

    FullPath=*lpFullPath;
    DrvLett=UpCaseCha(FullPath[0]);
    if (DrvLett!=0 && FullPath[1]==':')
        {
        Lun=DrvLett-'A';
        FullPath+=2;
        }

    if (FullPath[0]=='/') FullPath++;   //Root 기호 Skip
    if (ChkNoNameFg && FullPath[0]==0) {Printf("Empty File Name" CRLF); goto ProcExit;}
    if ((Dcb=CheckLunSpace(Lun))==NULL) goto ProcExit;
    if (Dcb->DiskSectorQty==0 || (CheckFormat && Dcb->ClusterStartSctNo==0))
        {
        Printf("Not initialized Disk, Call JFAT_Init() First" CRLF);
        Dcb=NULL;
        }

    ProcExit:
    *lpFullPath=FullPath;
    return Dcb;
    }




//-----------------------------------------------------------------------------
//      주어진 폴터안에 첫번째 파일을 찾아줌
//-----------------------------------------------------------------------------
WIN32_FIND_DATA* WINAPI JFAT_FindFirstFile(LPCSTR FullPath)
    {
    BOOL Rslt=FALSE;
    LPCSTR PathQ;
    DIRENTRY *DE;
    WIN32_FIND_DATA  *WFD=NULL;
    DISKCONTROLBLOCK *Dcb;
    FILENAMEFINDRESULT FI;  //Dummy

    if ((Dcb=GetDCB(&FullPath, FALSE, TRUE))==NULL) goto ProcExit;
    JFAT_Lock(Dcb);
    if ((WFD=AllocMemS(WIN32_FIND_DATA, MEMOWNER_FindFirstFile))==NULL) goto ProcExit;
    ZeroMem(WFD, GetMemberOffset(WIN32_FIND_DATA, SctBuff));
    PathQ=GetFileNameLocU8((LPSTR)FullPath);
    lstrcpyn(WFD->Wildcard, PathQ, sizeof(WFD->Wildcard));

    WFD->Dcb=Dcb;
    WFD->ClustStartSctNo=Dcb->RootDirSctNo;
    WFD->EntryBlockSctQty=Dcb->RootDirSctQty;
    if (PathQ-FullPath>0)
        {
        if ((DE=OpenDir(Dcb, FullPath, WFD->SctBuff, &FI, OPENDIR_DIR))==NULL) goto ProcExit;
        if ((DE->FileAttr & FILE_ATTRIBUTE_DIRECTORY)==0) goto ProcExit;
        WFD->DirClust=(DE->ClusterNoHi<<16)+DE->StartCluster;
        WFD->ClustStartSctNo=ClusterNoToSectorNo(Dcb, WFD->DirClust);
        WFD->EntryBlockSctQty=Dcb->SctsPerCluster;
        }

    Rslt=L_FindNextFile(Dcb, WFD);

    ProcExit:
    if (Rslt==FALSE) {FreeMem(WFD); WFD=NULL;}
    JFAT_Unlock(Dcb);
    return WFD;
    }




//-----------------------------------------------------------------------------
//      사용안하는 FCB 번호를 리턴합
//-----------------------------------------------------------------------------
LOCAL(HFILE) GetNoUseFCB(VOID)
    {
    int I;

    for (I=0; I<OPENFILEQTY; I++) if (FileCtrlBlock[I].FileOpened==0) break;
    if (I<OPENFILEQTY) ZeroMem(FileCtrlBlock+I, sizeof(FILECONTROLBLOCK));
    else{
        Printf("Too many files are open" CRLF);
        I=-1;   //파일핸들부족
        }
    return I;
    }




//-----------------------------------------------------------------------------
//      FullPath의 위치를 찾음
//-----------------------------------------------------------------------------
LOCAL(int) L_lopen(DISKCONTROLBLOCK *Dcb, LPCSTR FullPath, int OpenMode)
    {
    int   I, hFile=HFILE_ERROR;
    DIRENTRY *DE;
    FILECONTROLBLOCK *FCB;
    FILENAMEFINDRESULT FI;

    if ((I=GetNoUseFCB())<0) goto ProcExit;      //파일핸들부족
    FCB=FileCtrlBlock+I;
    if ((DE=OpenDir(Dcb, FullPath, Dcb->SctBuffer, &FI, OPENDIR_FILE))==NULL) goto ProcExit;    //DE는 SctBuff내부 위치임
    if (DE->FileAttr & FILE_ATTRIBUTE_DIRECTORY) goto ProcExit;
    FCB->Dcb=Dcb;
    FCB->OpenMode=OpenMode;
    FCB->FileOpened=FILEOPENSIGN;
    FCB->StartCluster=FCB->AccCluster=(DE->ClusterNoHi<<16)+DE->StartCluster;
    //FCB->FileAttr=DE->FileAttr;
    FCB->FileSize=DE->FileSize;
    FCB->FilePointer=0;
    FCB->DESctNo=FI.FindSectorNo;
    FCB->DESctOfs=FI.FindSectorOfs;
    hFile=I;

    ProcExit:
    return hFile;
    }




//-----------------------------------------------------------------------------
//      주어진 파일의 Attribute를 리턴
//-----------------------------------------------------------------------------
DWORD WINAPI JFAT_GetFileAttributes(LPCSTR FullPath)
    {
    DWORD FileAttr=~0;
    DIRENTRY *DE;
    DISKCONTROLBLOCK *Dcb;
    FILENAMEFINDRESULT FI;

    if ((Dcb=GetDCB(&FullPath, TRUE, TRUE))==NULL) goto ProcExit;
    JFAT_Lock(Dcb);
    if ((DE=OpenDir(Dcb, FullPath, Dcb->SctBuffer, &FI, OPENDIR_FILE))!=NULL)   //DE는 SctBuff내부 위치임
        FileAttr=DE->FileAttr;

    ProcExit:
    JFAT_Unlock(Dcb);
    return FileAttr;
    }




//-----------------------------------------------------------------------------
//      파일이 존재하는지 알려줌
//-----------------------------------------------------------------------------
BOOL WINAPI IsExistFile(LPCSTR Path)
    {
    BOOL  Rslt=FALSE;
    DWORD Attr;

    if ((Attr=JFAT_GetFileAttributes(Path))!=~0)
        {
        if ((Attr & (FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))==0) Rslt++;
        }
    return Rslt;
    }




//-----------------------------------------------------------------------------
//      파일 오픈
//-----------------------------------------------------------------------------
int WINAPI JFAT_Open(LPCSTR FullPath, int OpenMode)
    {
    int hFile=HFILE_ERROR;
    DISKCONTROLBLOCK *Dcb;

    if ((Dcb=GetDCB(&FullPath, TRUE, TRUE))==NULL) goto ProcExit;
    JFAT_Lock(Dcb);
    hFile=L_lopen(Dcb, FullPath, OpenMode);

    ProcExit:
    JFAT_Unlock(Dcb);
    if (hFile==HFILE_ERROR) Printf("'%s' not Found" CRLF, FullPath);
    return hFile;
    }




//-----------------------------------------------------------------------------
//      파일 위치 이동
//-----------------------------------------------------------------------------
LONG WINAPI JFAT_Seek(HFILE hFile, LONG Pos, int Origin)
    {
    UINT  ClustBytes, OrgClustIdx, NewClustIdx;
    DWORD I, NewPos=(DWORD)HFILE_ERROR;
    FILECONTROLBLOCK *FCB;
    DISKCONTROLBLOCK *Dcb=NULL;

    if ((UINT)hFile>=OPENFILEQTY) goto ProcExit;

    FCB=FileCtrlBlock+hFile;
    if (FCB->FileOpened!=FILEOPENSIGN) goto ProcExit;
    Dcb=FCB->Dcb;
    JFAT_Lock(Dcb);
    ClustBytes=Dcb->SctsPerCluster*SUPPORTSECTORBYTES;

    OrgClustIdx=(NewPos=FCB->FilePointer)/ClustBytes;
    switch (Origin)
        {
        case FILE_BEGIN:   NewPos=Pos; break;
        case FILE_CURRENT: NewPos+=Pos; break;
        case FILE_END:     NewPos=FCB->FileSize+Pos; //break;
        }
    if (NewPos>FCB->FileSize) {Printf("JFAT_Seek() No Support" CRLF); goto ProcExit;}
    if (FCB->FilePointer==NewPos) goto ProcExit;

    NewClustIdx=(FCB->FilePointer=NewPos)/ClustBytes;

    if (OrgClustIdx!=NewClustIdx)
        {   //새 위치를 포함한 클러스터 번호를 구해놓음
        FCB->AccCluster=FCB->StartCluster;
        for (I=0; I<NewPos; I+=ClustBytes)
            {
            if (NewPos<I+ClustBytes) break;
            FCB->PrevAccCluster=FCB->AccCluster;
            if (GetNextCluster(Dcb, &FCB->AccCluster)!=0) break;                //파일 끝이면
            }
        }

    ProcExit:
    JFAT_Unlock(Dcb);
    return NewPos;
    }



//-----------------------------------------------------------------------------
//      파일 읽기
//-----------------------------------------------------------------------------
LONG WINAPI JFAT_Read(HFILE hFile, LPVOID Buff, UINT ReadByteSize)
    {
    UINT   ToReadBytes, OfsInCluster, ClustBytes;
    DWORD  Eof;
    LONG   TotalReadBytes=HFILE_ERROR;
    FILECONTROLBLOCK *FCB;
    DISKCONTROLBLOCK *Dcb=NULL;

    if ((UINT)hFile>=OPENFILEQTY) goto ProcExit;
    FCB=FileCtrlBlock+hFile;
    if (FCB->FileOpened!=FILEOPENSIGN) goto ProcExit;
    if (FCB->OpenMode!=OF_READ && FCB->OpenMode!=OF_READWRITE) goto ProcExit;

    Dcb=FCB->Dcb;
    JFAT_Lock(Dcb);
    TotalReadBytes=0;
    ClustBytes=Dcb->SctsPerCluster*SUPPORTSECTORBYTES;
    if ((ReadByteSize=GetMin(FCB->FileSize-FCB->FilePointer, ReadByteSize))==0) goto ProcExit;

    Eof=(Dcb->FatType==16) ? FAT16_EOF:FAT32_EOF;
    OfsInCluster=FCB->FilePointer % ClustBytes;
    for (;;)
        {
        if (FCB->AccCluster<2 || FCB->AccCluster==Eof)
            {
            Printf("%c: Read Error" CRLF, Dcb->Lun+'A');
            goto ProcExit;
            }
        ToReadBytes=GetMin(ReadByteSize, ClustBytes-OfsInCluster);
        AccessCluster(Dcb, DEVICE_READ, FCB->AccCluster, OfsInCluster, (LPBYTE)Buff, ToReadBytes);
        Buff=(LPBYTE)Buff+ToReadBytes;
        TotalReadBytes+=ToReadBytes;
        FCB->FilePointer+=ToReadBytes;
        ReadByteSize-=ToReadBytes;
        if (ReadByteSize==0)
            {
            if (FCB->FilePointer % ClustBytes==0)
                {
                FCB->PrevAccCluster=FCB->AccCluster;
                GetNextCluster(Dcb, &FCB->AccCluster);                          //다음클러스터 읽을 위치로
                }
            break;
            }
        FCB->PrevAccCluster=FCB->AccCluster;
        if (GetNextCluster(Dcb, &FCB->AccCluster)!=0) break;                    //FAT가 깨진경우임
        OfsInCluster=0;
        }

    ProcExit:
    JFAT_Unlock(Dcb);
    return TotalReadBytes;
    }




//-----------------------------------------------------------------------------
//      뒷부분 안쓰는 영역의 시작을 찾아냄 (리턴값은 사용안한 클러스터 수임)
//-----------------------------------------------------------------------------
LOCAL(DWORD) FindLastFreeClustNo(DISKCONTROLBLOCK *Dcb)
    {
    DWORD Clust, FatEntry, TotalClusters, PrevFatEntry, FreeClustQty=0;

    TotalClusters=Dcb->TotalClusters+2;
    for (Clust=PrevFatEntry=2; Clust<TotalClusters; Clust++)
        {
        FatEntry=Clust;
        GetNextCluster(Dcb, &FatEntry);
        if (FatEntry==0)
            {
            if (PrevFatEntry!=0) Dcb->LastFreeClustNo=Clust;
            FreeClustQty++;
            }
        PrevFatEntry=FatEntry;
        }
    return FreeClustQty;
    }




//-----------------------------------------------------------------------------
//      빈 클러스터 하나를 찾아줌 (빈 클러스터가 없으면 0리턴)
//-----------------------------------------------------------------------------
LOCAL(DWORD) AllocFatOne(DISKCONTROLBLOCK *Dcb)
    {
    DWORD Clust, FatEntry, TotalClusters;

    if (Dcb->LastFreeClustNo==0)
        {
        if (Dcb->BPB_LastFreeClustNo!=0) Dcb->LastFreeClustNo=Dcb->BPB_LastFreeClustNo;
        else FindLastFreeClustNo(Dcb);
        }

    TotalClusters=Dcb->TotalClusters+2;
    for (Clust=Dcb->LastFreeClustNo; Clust<TotalClusters; Clust++)
        {
        FatEntry=Clust;
        GetNextCluster(Dcb, &FatEntry);
        if (FatEntry==0) break;
        }

    if (Clust>=TotalClusters)
        {
        for (Clust=2; Clust<TotalClusters; Clust++)
            {
            FatEntry=Clust;
            GetNextCluster(Dcb, &FatEntry);
            if (FatEntry==0) break;
            }
        if (Clust>=TotalClusters)
            {
            Printf("%d: Disk Full" CRLF, Dcb->Lun+'A');
            Clust=0;
            goto ProcExit;
            }
        }
    Dcb->LastFreeClustNo=Clust+1;

    ProcExit:
    return Clust;
    }



//-----------------------------------------------------------------------------
//      파일 쓰기
//-----------------------------------------------------------------------------
LONG WINAPI JFAT_Write(HFILE hFile, LPCVOID Buff, UINT WriteByteSize)
    {
    UINT   ToWriteBytes, OfsInCluster, ClustBytes;
    DWORD  Eof, NewEntry;
    LONG   TotalWriteBytes=HFILE_ERROR;
    FILECONTROLBLOCK *FCB;
    DISKCONTROLBLOCK *Dcb=NULL;

    if ((UINT)hFile>=OPENFILEQTY) goto ProcExit;
    FCB=FileCtrlBlock+hFile;
    if (FCB->FileOpened!=FILEOPENSIGN) goto ProcExit;
    if (FCB->OpenMode!=OF_WRITE && FCB->OpenMode!=OF_READWRITE) goto ProcExit;

    Dcb=FCB->Dcb;
    JFAT_Lock(Dcb);
    TotalWriteBytes=0;
    ClustBytes=Dcb->SctsPerCluster*SUPPORTSECTORBYTES;
    if (WriteByteSize==0) goto ProcExit;

    Eof=(Dcb->FatType==16) ? FAT16_EOF:FAT32_EOF;
    OfsInCluster=FCB->FilePointer % ClustBytes;
    for (;;)
        {
        if (FCB->AccCluster<2 || FCB->AccCluster==Eof)      //0바이트 파일이거나 Eof인 경우
            {
            if ((NewEntry=AllocFatOne(Dcb))==0) goto ProcExit;
            if (FCB->StartCluster==0) FCB->StartCluster=NewEntry;
            if (FCB->PrevAccCluster!=0) SetFatEntry(Dcb, FCB->PrevAccCluster, NewEntry, NULL);
            SetFatEntry(Dcb, NewEntry, Eof, NULL);
            FCB->AccCluster=NewEntry;
            }

        ToWriteBytes=GetMin(WriteByteSize, ClustBytes-OfsInCluster);
        AccessCluster(Dcb, DEVICE_WRITE, FCB->AccCluster, OfsInCluster, (LPBYTE)Buff, ToWriteBytes);
        Buff=(LPCBYTE)Buff+ToWriteBytes;
        TotalWriteBytes+=ToWriteBytes;
        FCB->FilePointer+=ToWriteBytes;
        FCB->FileSize=GetMax(FCB->FileSize, FCB->FilePointer);
        WriteByteSize-=ToWriteBytes;
        if (WriteByteSize==0)
            {
            if (FCB->FilePointer % ClustBytes==0)
                {
                FCB->PrevAccCluster=FCB->AccCluster;
                GetNextCluster(Dcb, &FCB->AccCluster);  //다음클러스터 읽을 위치로
                }
            break;
            }
        FCB->PrevAccCluster=FCB->AccCluster;
        GetNextCluster(Dcb, &FCB->AccCluster);
        OfsInCluster=0;
        }

    ProcExit:
    JFAT_Unlock(Dcb);
    return TotalWriteBytes;
    }




LONG WINAPI JFAT_GetFileSize(HFILE hFile)
    {
    LONG Size=-1;

    if ((UINT)hFile<OPENFILEQTY) Size=FileCtrlBlock[hFile].FileSize;
    return Size;
    }



VOID WINAPI JFAT_Close(HFILE hFile)
    {
    LPBYTE SctBuff;
    DIRENTRY *DE;
    FILECONTROLBLOCK *FCB;
    DISKCONTROLBLOCK *Dcb=NULL;

    if ((UINT)hFile>=OPENFILEQTY) goto ProcExit;
    FCB=FileCtrlBlock+hFile;
    if (FCB->FileOpened!=FILEOPENSIGN) goto ProcExit;
    Dcb=FCB->Dcb;
    JFAT_Lock(Dcb);
    SctBuff=Dcb->SctBuffer;

    if (FCB->OpenMode==OF_WRITE || FCB->OpenMode==OF_READWRITE)
        {
        if (FCB->DESctNo!=0)
            {
            if (STORAGE_Read(Dcb->Lun, SctBuff, FCB->DESctNo, 1))
                {
                DE=(DIRENTRY*)(SctBuff+FCB->DESctOfs);
                if (DE->FileSize!=FCB->FileSize)
                    {
                    if (DE->StartCluster==0 && DE->ClusterNoHi==0)
                        {
                        DE->ClusterNoHi=FCB->StartCluster>>16;
                        DE->StartCluster=(WORD)FCB->StartCluster;
                        }
                    DE->FileSize=FCB->FileSize;
                    STORAGE_Write(Dcb->Lun, SctBuff, FCB->DESctNo, 1);
                    }
                }

            if (Dcb->FatType==32)
                {
                if (STORAGE_Read(Dcb->Lun, SctBuff, Dcb->VolumeStartSctNo+1, 1) &&
                    *(DWORD*)(SctBuff+0x1E4)==0x61417272 &&     //'rrAa'
                    *(DWORD*)(SctBuff+0x1EC)!=Dcb->LastFreeClustNo)
                    {
                    *(DWORD*)(SctBuff+0x1EC)=Dcb->LastFreeClustNo;
                    STORAGE_Write(Dcb->Lun, SctBuff, Dcb->VolumeStartSctNo+1, 1);
                    }
                }
            }
        else Printf("JFAT_Close() error, FCB->DESctAddr is Zero" CRLF);
        FlushChcheBuff(Dcb);
        }
    FCB->FileOpened=0;

    ProcExit:
    JFAT_Unlock(Dcb);
    }




//-----------------------------------------------------------------------------
//      파일의 날짜 시간 설정
//-----------------------------------------------------------------------------
BOOL WINAPI JFAT_SetFileTime(HFILE hFile, JTIME CreationTime, JTIME LastAccessTime, JTIME LastWriteTime)
    {
    BOOL   Rslt=FALSE;
    DWORD  DosTime;
    LPBYTE SctBuff;
    DIRENTRY *DE;
    FILECONTROLBLOCK *FCB;
    DISKCONTROLBLOCK *Dcb=NULL;

    if ((UINT)hFile>=OPENFILEQTY) goto ProcExit;
    FCB=FileCtrlBlock+hFile;
    if (FCB->FileOpened!=FILEOPENSIGN) goto ProcExit;
    Dcb=FCB->Dcb;
    JFAT_Lock(Dcb);
    SctBuff=Dcb->SctBuffer;

    if (FCB->OpenMode==OF_WRITE || FCB->OpenMode==OF_READWRITE)
        {
        if (FCB->DESctNo!=0)
            {
            if (STORAGE_Read(Dcb->Lun, SctBuff, FCB->DESctNo, 1))
                {
                DE=(DIRENTRY*)(SctBuff+FCB->DESctOfs);

                if (CreationTime)
                    {
                    DosTime=JTimeToDosDateTime(CreationTime);
                    DE->CreateDate=DosTime>>16;
                    DE->CreateTime=(WORD)DosTime;
                    }
                if (LastAccessTime)
                    {
                    DosTime=JTimeToDosDateTime(LastAccessTime);
                    DE->AccessDate=DosTime>>16;
                    }
                if (LastWriteTime)
                    {
                    DosTime=JTimeToDosDateTime(LastWriteTime);
                    DE->LastModiDate=DosTime>>16;
                    DE->LastModiTime=(WORD)DosTime;
                    }
                Rslt=STORAGE_Write(Dcb->Lun, SctBuff, FCB->DESctNo, 1);
                }
            }
        }

    ProcExit:
    JFAT_Unlock(Dcb);
    return Rslt;
    }




//-----------------------------------------------------------------------------
//      FAT FileSystem인지 확인함
//-----------------------------------------------------------------------------
LOCAL(BOOL) IsFatFileSystem(BPB_F32 *BPB)
    {
    return CompMemStr(BPB->FS32Sign, "FAT32")==0 ||
           CompMemStr(((BPB_F16*)BPB)->FileSystemSign, "FAT16")==0 ||
           CompMemStr(((BPB_F16*)BPB)->FileSystemSign, "FAT12")==0;
    }




//-----------------------------------------------------------------------------
//      불륨의 정보를 읽어 놓음
//-----------------------------------------------------------------------------
LOCAL(BOOL) ReadVolID(DISKCONTROLBLOCK *Dcb)
    {
    int  Rslt=FALSE;
    UINT FatScts, RootDirScts, TotalSectors;                                    //RootDirScts: FAT16에서 루트디렉토리의 섹터수
    BPB_F32 *BPB;

    Dcb->VolumeStartSctNo=0;
    BPB=(BPB_F32*)Dcb->SctBuffer;
    STORAGE_Read(Dcb->Lun, (LPBYTE)BPB, 0, 1);                                  //MBR일 수 있음
    if (BPB->BootSctValidSign!=0xAA55)
        {
        FormatFirst:
        Printf("Format %c:, first"CRLF, Dcb->Lun+'A');
        goto ProcExit;
        }
    if (IsFatFileSystem(BPB)==FALSE)
        {
        Dcb->VolumeStartSctNo=Peek((LPBYTE)BPB+0x1C6);
        STORAGE_Read(Dcb->Lun, (LPBYTE)BPB, Dcb->VolumeStartSctNo, 1);
        //DumpMem((LPBYTE)BPB, sizeof(BPB_F32));
        if (BPB->BootSctValidSign!=0xAA55 || IsFatFileSystem(BPB)==FALSE) goto FormatFirst;
        }

    if (PeekW((LPCVOID)&BPB->BytesPerSector)!=SUPPORTSECTORBYTES)
        {
        Printf("Mismatch Sector size in FAT" CRLF);
        goto ProcExit;
        }

    Dcb->FirstFatSctNo=BPB->SystemUseSctNo + Dcb->VolumeStartSctNo;
    Dcb->SecondFatSctNo=0;
    RootDirScts=((PeekW((LPCVOID)&BPB->RootDirEntrys)<<5)+SUPPORTSECTORBYTES-1)/SUPPORTSECTORBYTES;  //FAT32에서는 0
    Dcb->SctsPerCluster=BPB->SectorsPerCluster;

    if ((TotalSectors=PeekW((LPCVOID)&BPB->OldTotalSectors))==0) TotalSectors=BPB->BigTotalSectors;
    if ((FatScts=BPB->SectorsPerFAT)==0) FatScts=BPB->BigSctPerFAT;
    Dcb->RootDirSctNo=FatScts*BPB->FATCopys + Dcb->FirstFatSctNo;
    if (FatScts>0) Dcb->SecondFatSctNo=Dcb->FirstFatSctNo+FatScts;

    Dcb->TotalClusters=(TotalSectors - BPB->SystemUseSctNo - FatScts*BPB->FATCopys - RootDirScts) / BPB->SectorsPerCluster;
    if (Dcb->TotalClusters<=0xFF4) {Dcb->FatType=12; goto Fat16Proc;}
    else if (Dcb->TotalClusters<=0xFFF4)
        {
        Dcb->FatType=16;

        Fat16Proc:
        Dcb->RootClustNo=0;
        Dcb->RootDirSctQty=RootDirScts;
        Dcb->ClusterStartSctNo=Dcb->RootDirSctNo + Dcb->RootDirSctQty;
        }
    else{
        Dcb->FatType=32;
        Dcb->RootClustNo=BPB->RootDirClustNo;
        Dcb->RootDirSctQty=Dcb->SctsPerCluster;
        Dcb->ClusterStartSctNo=Dcb->RootDirSctNo;
        Dcb->RootDirSctNo=ClusterNoToSectorNo(Dcb, Dcb->RootClustNo);
        }

    Dcb->LastFreeClustNo=0;
    if (Dcb->FatType==32)
        {
        STORAGE_Read(Dcb->Lun, (LPBYTE)BPB, Dcb->VolumeStartSctNo+1, 1);
        if (*(DWORD*)((LPBYTE)BPB+0x1E4)==0x61417272)     //'rrAa'
            Dcb->BPB_LastFreeClustNo=*(DWORD*)((LPBYTE)BPB+0x1EC);
        }

    #if JFATDEBUG
    Printf("StartSctNo=%u" CRLF,Dcb->VolumeStartSctNo);
    Printf("FATScts  =%u" CRLF, FatScts);
    Printf("FAT1SctNo=%u" CRLF, Dcb->FirstFatSctNo);
    Printf("FAT2SctNo=%u" CRLF, Dcb->SecondFatSctNo);
    Printf("RootSctNo=%u" CRLF, Dcb->RootDirSctNo);
    Printf("DataSctNo=%u" CRLF, Dcb->ClusterStartSctNo);
    Printf("SctsPerCluster=%u" CRLF, Dcb->SctsPerCluster);
    Printf("LastFreeClustNo=%u" CRLF, Dcb->LastFreeClustNo);
    Printf("TotalClusters=%u" CRLF, Dcb->TotalClusters);
    #endif
    Rslt++;

    ProcExit:
    return Rslt;
    }



#if JFAT_READOLNY==0
//-----------------------------------------------------------------------------
//      FAT을 연속으로 주어진 크기만큼 할당함
//-----------------------------------------------------------------------------
LOCAL(DWORD) AllocFat(DISKCONTROLBLOCK *Dcb, DWORD FileSize)
    {
    DWORD Eof, FSize, AllocStartClustNo, NewEntry, PrevEntry;

    Eof=(Dcb->FatType==16) ? FAT16_EOF:FAT32_EOF;
    FSize=AllocStartClustNo=PrevEntry=0;
    for (;;)
        {
        if ((NewEntry=AllocFatOne(Dcb))==0)
            {
            if (PrevEntry!=0) SetFatEntry(Dcb, PrevEntry, Eof, NULL);
            break;
            }

        if (PrevEntry!=0) SetFatEntry(Dcb, PrevEntry, NewEntry, NULL);
        if (AllocStartClustNo==0) AllocStartClustNo=NewEntry;

        if ((FSize+=Dcb->SctsPerCluster*SUPPORTSECTORBYTES)>=FileSize)
            {
            SetFatEntry(Dcb, NewEntry, Eof, NULL);
            break;
            }
        PrevEntry=NewEntry;
        }
    return AllocStartClustNo;
    }




//-----------------------------------------------------------------------------
//      LFN Entry를 만들어 줌
//
//      'Abcdefghijklmnopqrstuvwxyz.123'의 Entry 예
//
// 43 2E 00 31 00 32 00 33 - 00 00 00 0F 00 2A FF FF C. 1 2 3     *
// FF FF FF FF FF FF FF FF - FF FF 00 00 FF FF FF FF
// 02 6E 00 6F 00 70 00 71 - 00 72 00 0F 00 2A 73 00  n o p q r   *s
// 74 00 75 00 76 00 77 00 - 78 00 00 00 79 00 7A 00 t u v w x   y z
// 01 41 00 62 00 63 00 64 - 00 65 00 0F 00 2A 66 00  A b c d e   *f
// 67 00 68 00 69 00 6A 00 - 6B 00 00 00 6C 00 6D 00 g h i j k   l m
// 41 42 43 44 45 46 7E 31 - 31 32 33 20 00 2D D3 5A ABCDEF~1123
// 52 37 52 37 00 00 D4 5A - 52 37 00 00 00 00 00 00
//-----------------------------------------------------------------------------
#define ONEENTRY_CHARS  13      //하나의 엔트리에 들어가는 문자 수
LOCAL(DIRENTRY*) JFAT_MakeLfn(DISKCONTROLBLOCK *Dcb, LPCSTR FullPath)
    {
    int  Cha, I, Len, EnterQty, LfnCnt, LfnChkSum, Err=JFAT_NOERROR;
    LPSTR ShortFilePath, SFN;
    LPCSTR NewFileName;
    CHAR ExtName[4];
    DIRENTRY *Lfn, *DE;
    FILENAMEFINDRESULT FI;
    static CONST BYTE LfnCharPos[]={1,3,5,7,9,0x0E,0x10,0x12,0x14,0x16,0x18,0x1C,0x1E,0};   //멘뒤0은 끝이라는 의미

    NewFileName=GetFileNameLocU8((LPSTR)FullPath);
    if ((Len=GetChQtyU8(NewFileName)+1)>195) {Err=JFAT_LFNTOOLONG; goto ProcExit;}          //+1: 맨뒤 Null문자까지
    EnterQty=(Len+ONEENTRY_CHARS-1)/ONEENTRY_CHARS +1;                                      //+1은 8.3이름 저장위치
    Len=EnterQty*sizeof(DIRENTRY);
    if ((Lfn=(DIRENTRY*)AllocMem(Len+lstrlen(FullPath)+16, MEMOWNER_JFAT_MakeLfn))==NULL) {Err=JFAT_INSUFFICIENTMEMORY; goto ProcExit;} //+16은 8.3파일명 버퍼
    ShortFilePath=(LPSTR)Lfn+Len;
    ZeroMem(Lfn, Len);

    lstrcpy(ShortFilePath, FullPath); SFN=GetFileNameLocU8(ShortFilePath);
    lstrcpyn(ExtName, GetFileExtNameLoc((LPSTR)NewFileName), sizeof(ExtName));
    wsprintf(SFN, "~*.%s", ExtName);
    OpenDir(Dcb, ShortFilePath, Dcb->SctBuffer, &FI, OPENDIR_FILE);
    if (FI.LfnID==0) {Err=JFAT_PATHNOTFOUND; goto ProcExit;}
    wsprintf(SFN, "~%07d.%s", FI.LfnID, ExtName);

    DE=Lfn+EnterQty-1;
    ConvFileNameTo83Name(DE->FileName, SFN);
    LfnChkSum=GetLfnChkSum(DE->FileName, 11);
    DE--; I=0; LfnCnt=1;
    for (;;)
        {
        Cha=GetCharU8(NewFileName, &Len); NewFileName+=Len;
        PokeW((LPBYTE)DE+LfnCharPos[I], Cha);
        if (Cha==0)
            {
            DE->FileName[0]=LfnCnt|0x40;
            DE->FileAttr=0x0F;
            DE->LfnChkSum=LfnChkSum;
            while (++I<ONEENTRY_CHARS) PokeW((LPBYTE)DE+LfnCharPos[I], 0xFFFF);
            break;
            }
        if (++I>=ONEENTRY_CHARS)
            {
            DE->FileName[0]=LfnCnt++;
            DE->FileAttr=0x0F;
            DE->LfnChkSum=LfnChkSum;
            DE--; I=0;
            }
        }

    ProcExit:
    SetLastError(Err);
    return Lfn;
    }




//-----------------------------------------------------------------------------
//      주어진 클러스터의 파일엔트리에서 주어진 파일명을 삭제함
//-----------------------------------------------------------------------------
LOCAL(BOOL) EraseFileName(CONST DISKCONTROLBLOCK *Dcb, FILENAMEFINDRESULT *FI, LPBYTE SctBuff)
    {
    BOOL Rslt=FALSE;
    UINT Lun, SctNo, SctOfs;
    DIRENTRY *DE;

    Lun=Dcb->Lun;
    if (FI->FindSectorNo==0) goto ProcExit;

    if ((SctNo=FI->LfnFirstLocSctNo)!=0)
        {
        SctOfs=FI->LfnFirstLocSctOfs;
        for (;;)
            {
            if (STORAGE_Read(Lun, SctBuff, SctNo, 1)==FALSE) goto ProcExit;
            for (;;)
                {
                DE=(DIRENTRY*)(SctBuff+SctOfs);
                DE->FileName[0]=DIRENTRY_ERASE;
                if (FI->FindSectorNo==SctNo && FI->FindSectorOfs==SctOfs) break;
                if ((SctOfs+=sizeof(DIRENTRY))>=SUPPORTSECTORBYTES) break;
                }
            if (STORAGE_Write(Lun, SctBuff, SctNo, 1)==FALSE) goto ProcExit;

            if (FI->FindSectorNo==SctNo) break; //1섹터를 초과하지 않는 최대 파일명 문자수 (13*15=195)
            SctNo=FI->FindSectorNo;
            SctOfs=0;
            }
        }
    else{
        if (STORAGE_Read(Lun, SctBuff, FI->FindSectorNo, 1)==FALSE) goto ProcExit;
        DE=(DIRENTRY*)(SctBuff+FI->FindSectorOfs);
        DE->FileName[0]=DIRENTRY_ERASE;
        if (STORAGE_Write(Lun, SctBuff, FI->FindSectorNo, 1)==FALSE) goto ProcExit;
        }
    Rslt++;

    ProcExit:
    (VOID)Lun;      //STORAGE_Read()를 #define으로 연결할 때 Lun이 쓰이기 않으면 경고가 발생함
    return Rslt;
    }




//-----------------------------------------------------------------------------
//      파일삭제 (아직 194문자를 초과하는 LFN의 일부는 삭제를 못함)
//-----------------------------------------------------------------------------
LOCAL(BOOL) L_DeleteFile(DISKCONTROLBLOCK *Dcb, LPCSTR FilePath)
    {
    int    Eof, Err=JFAT_NOERROR;
    LPBYTE SctBuff;
    DWORD  Clust, AccSize, FileSize, NextEntry;
    DIRENTRY *DE;
    FILENAMEFINDRESULT FI;

    SctBuff=Dcb->SctBuffer;
    if ((DE=OpenDir(Dcb, FilePath, SctBuff, &FI, OPENDIR_FILE))==NULL) {NoFile: Err=JFAT_FILENOTFOUND; goto ProcExit;}
    if (DE->FileAttr & FILE_ATTRIBUTE_DIRECTORY) goto NoFile;
    Clust=(DE->ClusterNoHi<<16)+DE->StartCluster;
    FileSize=DE->FileSize;

    //파일 NameEntry 삭제 (LFN도 삭제)
    if (EraseFileName(Dcb, &FI, SctBuff)==FALSE) {Err=JFAT_DISKACCESSERROR; goto ProcExit;}
    if (Clust==0) goto ProcExit;

    AccSize=0;
    for (;;)
        {
        Eof=SetFatEntry(Dcb, Clust, 0, &NextEntry); //엔트리의 끝이면 TRUE리턴
        if (NextEntry==0) goto BrokenFat;
        if ((AccSize+=Dcb->SctsPerCluster*SUPPORTSECTORBYTES)>=FileSize)
            {
            if (Eof) break;

            BrokenFat:
            Printf("DeleteFile() Broken FAT" CRLF);
            Err=JFAT_FATBROKENERROR;
            break;
            }
        Clust=NextEntry;
        }
    FlushChcheBuff(Dcb);

    ProcExit:
    SetLastError(Err);
    return Err==JFAT_NOERROR;
    }




//-----------------------------------------------------------------------------
//      파일삭제 (아직 LFN 부분은 삭제를 못함)
//-----------------------------------------------------------------------------
BOOL WINAPI JFAT_DeleteFile(LPCSTR FilePath)
    {
    BOOL Rslt=FALSE;
    DISKCONTROLBLOCK *Dcb;

    if ((Dcb=GetDCB(&FilePath, TRUE, TRUE))!=NULL)
        {
        JFAT_Lock(Dcb);
        Rslt=L_DeleteFile(Dcb, FilePath);
        JFAT_Unlock(Dcb);
        }
    return Rslt;
    }




//-----------------------------------------------------------------------------
//      주어진 클러스터를 0으로 Clear함
//-----------------------------------------------------------------------------
LOCAL(BOOL) ZeroCluster(DISKCONTROLBLOCK *Dcb, DWORD ClusterNo)
    {
    int  I, Rslt=FALSE;
    DWORD SctNo;

    ZeroMem(Dcb->SctBuffer, SUPPORTSECTORBYTES);
    SctNo=ClusterNoToSectorNo(Dcb, ClusterNo);
    for (I=0; I<(int)Dcb->SctsPerCluster; I++)
        {
        if (STORAGE_Write(Dcb->Lun, Dcb->SctBuffer, SctNo+I, 1)==FALSE) goto ProcExit;
        }
    Rslt++;

    ProcExit:
    return Rslt;
    }




//-----------------------------------------------------------------------------
//      주어진 새 엔트리를 기록
//-----------------------------------------------------------------------------
LOCAL(BOOL) WriteDirEntry(DISKCONTROLBLOCK *Dcb, LPCSTR FullPath, DIRENTRY *ToWrtDE, int ToWrtDEQty)
    {
    int   I, FirstCha, EmptyDECnt=0, Err=JFAT_PATHNOTFOUND;
    UINT  SctOfs, SctOfsInClust, ClustStart, BlockSctQty, SctNo, T, EmptySctNo[2], EmptyOfs[2];
    DWORD DirCluster;
    LPBYTE SctBuff;
    DIRENTRY *DE;
    CHAR  FName[LFN_MAXLEN];
    FILENAMEFINDRESULT FI;

    SctBuff=Dcb->SctBuffer;
    DirCluster=0;   //루트 클러스터
    for (;;)
        {
        if (SearchCha(FullPath, '/')<0) break;
        FullPath=CatchFileName(FullPath, FName, LFN_MAXLEN);
        if ((DE=SearchFileName(Dcb, FName, &DirCluster, &FI, SctBuff))==NULL) goto ProcExit;
        if ((DirCluster=(DE->ClusterNoHi<<16)+DE->StartCluster)==0) goto ProcExit;
        }

    EmptySctNo[0]=EmptySctNo[1]=0;
    for (;;)
        {
        if (DirCluster==0)      //RootDir
            {
            ClustStart=Dcb->RootDirSctNo;
            BlockSctQty=Dcb->RootDirSctQty;
            }
        else{
            ClustStart=ClusterNoToSectorNo(Dcb, DirCluster);
            BlockSctQty=Dcb->SctsPerCluster;
            }

        for (SctOfsInClust=0; SctOfsInClust<BlockSctQty; SctOfsInClust++)
            {
            if (STORAGE_Read(Dcb->Lun, SctBuff, SctNo=ClustStart+SctOfsInClust, 1)==FALSE) {DiskErr: Err=JFAT_DISKACCESSERROR; goto ProcExit;}

            for (SctOfs=0; SctOfs<SUPPORTSECTORBYTES; SctOfs+=sizeof(DIRENTRY))
                {
                DE=(DIRENTRY*)(SctBuff+SctOfs);

                FirstCha=DE->FileName[0];
                if (FirstCha==DIRENTRY_END || FirstCha==DIRENTRY_ERASE)
                    {
                    if (EmptySctNo[0]==0)
                        {
                        EmptySctNo[0]=SctNo;
                        EmptyOfs[0]=SctOfs;
                        EmptyDECnt=1;
                        if (ToWrtDEQty==1) goto WriteDE;
                        }
                    else{
                        if (EmptySctNo[0]!=SctNo) {EmptySctNo[1]=SctNo; EmptyOfs[1]=0;}
                        if (++EmptyDECnt==ToWrtDEQty) goto WriteDE;
                        }
                    }
                else EmptySctNo[0]=EmptySctNo[1]=0;
                }
            }
        if (DirCluster==0 && Dcb->FatType!=32) {Err=JFAT_DIRENTRYFULL; goto ProcExit;}
        T=DirCluster;
        if (GetNextCluster(Dcb, &DirCluster))   //EOF이면
            {
            if ((DirCluster=AllocFatOne(Dcb))==0) {Err=JFAT_DISKFULL; goto ProcExit;}  //디스크가 꽉참
            SetFatEntry(Dcb, T, DirCluster, NULL);
            SetFatEntry(Dcb, DirCluster, Dcb->FatType==16 ? FAT16_EOF:FAT32_EOF, NULL);
            if (ZeroCluster(Dcb, DirCluster)==FALSE) goto DiskErr;
            }
        }

    WriteDE:
    for (I=0; I<2; I++)
        {
        if ((SctNo=EmptySctNo[I])==0) {Err=JFAT_INTERNALERROR; goto ProcExit;}
        if (STORAGE_Read(Dcb->Lun, SctBuff, SctNo, 1)==FALSE) goto DiskErr;
        T=GetMin(ToWrtDEQty, (SUPPORTSECTORBYTES-EmptyOfs[I])/sizeof(DIRENTRY));
        CopyMem(SctBuff+EmptyOfs[I], ToWrtDE, T*sizeof(DIRENTRY));
        if (STORAGE_Write(Dcb->Lun, SctBuff, SctNo, 1)==FALSE) goto DiskErr;
        if ((ToWrtDEQty-=T)==0) break;
        ToWrtDE+=T;
        }
    Err=JFAT_NOERROR;

    ProcExit:
    SetLastError(Err);
    return Err==JFAT_NOERROR;
    }




//-----------------------------------------------------------------------------
//      이 파일명이 8.3파일인가를 알려줍니다
//-----------------------------------------------------------------------------
LOCAL(BOOL) Is83FileName(LPCSTR InFileName)
    {
    int I,J, Rslt=FALSE;
    CHAR Cha;
    static CONST CHAR Valid83Char[]="`~!@#$%^&()-_'0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.";

    I=0; J=8;
    for (;;)
        {
        Cha=*InFileName++;
        if (Cha==0)   {Rslt++; break;}
        if (SearchCha(Valid83Char, Cha)<0) break;

        if (J==8)       //파일명모드
            {
            if (Cha=='.')
                {
                if (I==0) break;
                J=3; I=0;
                }
            else{
                if (++I>J) break;
                }
            }
        else{           //확장자모드
            if (Cha=='.') break;
            if (++I>J) break;
            }
        }
    return Rslt;
    }




//-----------------------------------------------------------------------------
//      파일 생성
//-----------------------------------------------------------------------------
HFILE WINAPI JFAT_Create(LPCSTR FullPath, int Attr)
    {
    int    hFile=HFILE_ERROR, LfnDEQty=0;
    LPCSTR FileName;
    DWORD  DosTime;
    DIRENTRY *DE, *CreatedDE=NULL, _83DE;
    SYSTEMTIME ST;
    DISKCONTROLBLOCK *Dcb;

    if ((Dcb=GetDCB(&FullPath, TRUE, TRUE))==NULL) goto Ret;
    JFAT_Lock(Dcb);
    if (Dcb->FatType!=16 && Dcb->FatType!=32) goto ProcExit;
    if (GetNoUseFCB()<0) goto ProcExit;         //파일핸들부족

    if (L_DeleteFile(Dcb, FullPath)==FALSE && GetLastError()!=JFAT_FILENOTFOUND) goto ProcExit;      //이미 있는 파일은 삭제

    FileName=GetFileNameLocU8((LPSTR)FullPath);
    if (Is83FileName(FileName))
        {
        ZeroMem(DE=&_83DE, sizeof(_83DE));
        ConvFileNameTo83Name(DE->FileName, FileName);
        }
    else{
        if ((CreatedDE=JFAT_MakeLfn(Dcb, FullPath))==NULL) goto ProcExit;
        LfnDEQty=CreatedDE->FileName[0]&0x3F;
        DE=CreatedDE+LfnDEQty;
        }

    GetLocalTime(&ST);
    DosTime=SystemTimeToDosFileTime(&ST);

    DE->FileAttr=Attr;
    DE->CreateTime=DE->LastModiTime=(WORD)DosTime;
    DE->CreateDate=DE->AccessDate=DE->LastModiDate=DosTime>>16;
    //DE->ClusterNoHi=0;
    //DE->StartCluster=0;
    //DE->FileSize=0;

    if (WriteDirEntry(Dcb, FullPath, LfnDEQty==0 ? DE:CreatedDE, LfnDEQty+1)==FALSE) goto ProcExit;
    hFile=L_lopen(Dcb, FullPath, OF_READWRITE);

    ProcExit:
    JFAT_Unlock(Dcb);
    FreeMem(CreatedDE);
    Ret:
    return hFile;
    }





//-----------------------------------------------------------------------------
//      폴더 생성
//-----------------------------------------------------------------------------
BOOL WINAPI JFAT_CreateDirectory(LPCSTR FullPath)
    {
    int    Err=JFAT_INTERNALERROR, LfnDEQty=0;
    LPCSTR FileName;
    DWORD  DosTime, DirCluster, UpCluster;
    DIRENTRY *DE, *CreatedDE=NULL, _83DE;
    LPBYTE SctBuff;
    SYSTEMTIME ST;
    FILENAMEFINDRESULT FI;
    DISKCONTROLBLOCK *Dcb;

    if ((Dcb=GetDCB(&FullPath, TRUE, TRUE))==NULL) goto ProcExit;
    JFAT_Lock(Dcb);
    if (Dcb->FatType!=16 && Dcb->FatType!=32) goto ProcExit;

    SctBuff=Dcb->SctBuffer;
    if (OpenDir(Dcb, FullPath, SctBuff, &FI, OPENDIR_FILE)!=NULL) {Err=JFAT_ALREADYEXISTS; goto ProcExit;}

    FileName=GetFileNameLocU8((LPSTR)FullPath);
    if (Is83FileName(FileName))
        {
        ZeroMem(DE=&_83DE, sizeof(_83DE));
        ConvFileNameTo83Name(DE->FileName, FileName);
        }
    else{
        if ((CreatedDE=JFAT_MakeLfn(Dcb, FullPath))==NULL) {Err=JFAT_INSUFFICIENTMEMORY; goto ProcExit;}
        LfnDEQty=CreatedDE->FileName[0]&0x3F;
        DE=CreatedDE+LfnDEQty;
        }

    GetLocalTime(&ST);
    DosTime=SystemTimeToDosFileTime(&ST);

    DE->FileAttr=FILE_ATTRIBUTE_DIRECTORY;
    DE->CreateTime=DE->LastModiTime=(WORD)DosTime;
    DE->CreateDate=DE->AccessDate=DE->LastModiDate=DosTime>>16;
    //DE->FileSize=0;
    if ((DirCluster=AllocFatOne(Dcb))==0) {Err=JFAT_DISKFULL; goto ProcExit;}  //디스크가 꽉참
    if (ZeroCluster(Dcb, DirCluster)==FALSE) {DiskErr: Err=JFAT_DISKACCESSERROR; goto ProcExit;}
    DE->ClusterNoHi=DirCluster>>16;
    DE->StartCluster=(WORD)DirCluster;

    if (WriteDirEntry(Dcb, FullPath, LfnDEQty==0 ? DE:CreatedDE, LfnDEQty+1)==FALSE) {Err=GetLastError(); goto ProcExit;}
    SetFatEntry(Dcb, DirCluster, Dcb->FatType==16 ? FAT16_EOF:FAT32_EOF, NULL);

    if (&_83DE!=DE) _83DE=*DE;
    UpCluster=0;
    if (GetFileNameLocU8((LPSTR)FullPath)>FullPath)
        {
        if ((DE=OpenDir(Dcb, FullPath, SctBuff, &FI, OPENDIR_DIR))==NULL) {Err=JFAT_INTERNALERROR; goto ProcExit;}
        UpCluster=(DE->ClusterNoHi<<16)+DE->StartCluster;
        }

    ZeroMem(SctBuff, SUPPORTSECTORBYTES);
    FillMem(_83DE.FileName, 8+3, ' '); _83DE.FileName[0]='.';
    CopyMem(SctBuff, &_83DE, sizeof(DIRENTRY));
    FillMem(_83DE.FileName, 8+3, ' '); _83DE.FileName[0]=_83DE.FileName[1]='.';
    _83DE.ClusterNoHi=UpCluster>>16;
    _83DE.StartCluster=(WORD)UpCluster;
    CopyMem(SctBuff+sizeof(DIRENTRY), &_83DE, sizeof(DIRENTRY));
    if (STORAGE_Write(Dcb->Lun, SctBuff, ClusterNoToSectorNo(Dcb, DirCluster), 1)==FALSE) goto DiskErr;

    FlushChcheBuff(Dcb);
    Err=JFAT_NOERROR;

    ProcExit:
    JFAT_Unlock(Dcb);
    FreeMem(CreatedDE);
    SetLastError(Err);
    return Err==JFAT_NOERROR;
    }





//-----------------------------------------------------------------------------
//      주어진 크기의 파일생성 (8.3파일명만 지원, LFN은 만들지 못함)
//      리눅스의 touch 와 같은 함수
//
//      연속적인 바디를 가진 파일을 만든 후 파일 시스템을 이용하지 않고,
//      파일 바디에 물리적으로 억세스를 하기 위함 (센서 데이터를 빠르게 저장하는데 사용)
//-----------------------------------------------------------------------------
DWORD WINAPI CreateNewFile(LPCSTR FullPath, int Attr, DWORD FileSize)
    {
    DWORD StartClust, DosTime, StartSctNo=0;
    LPBYTE SctBuff;
    DIRENTRY *DE;
    SYSTEMTIME ST;
    DISKCONTROLBLOCK *Dcb;
    FILENAMEFINDRESULT FI;
    DIRENTRY NewDE;

    if ((Dcb=GetDCB(&FullPath, TRUE, TRUE))==NULL) goto ProcExit;
    JFAT_Lock(Dcb);
    SctBuff=Dcb->SctBuffer;
    if (Dcb->FatType!=16 && Dcb->FatType!=32) goto ProcExit;

    if ((DE=OpenDir(Dcb, FullPath, SctBuff, &FI, OPENDIR_FILE))!=NULL)
        {
        StartClust=(DE->ClusterNoHi<<16)+DE->StartCluster;
        }
    else{
        Printf("Create '%s'" CRLF, FullPath);
        if ((StartClust=AllocFat(Dcb, FileSize))==0)
            {
            Printf("No Disk spece for '%s'" CRLF, FullPath);
            goto ProcExit;
            }

        ZeroMem(&NewDE, sizeof(DIRENTRY));
        GetLocalTime(&ST);
        DosTime=SystemTimeToDosFileTime(&ST);

        ConvFileNameTo83Name(NewDE.FileName, GetFileNameLocU8((LPSTR)FullPath));
        NewDE.FileAttr=Attr;
        NewDE.CreateTime=NewDE.LastModiTime=(WORD)DosTime;
        NewDE.CreateDate=NewDE.AccessDate=NewDE.LastModiDate=DosTime>>16;
        NewDE.ClusterNoHi=StartClust>>16;
        NewDE.StartCluster=(WORD)StartClust;
        NewDE.FileSize=FileSize;
        if (WriteDirEntry(Dcb, FullPath, &NewDE, 1)==FALSE) goto ProcExit;
        FlushChcheBuff(Dcb);
        }

    StartSctNo=ClusterNoToSectorNo(Dcb, StartClust);

    ProcExit:
    JFAT_Unlock(Dcb);
    return StartSctNo;
    }




static CONST BYTE F16BootSector[]=
    {
    0xEB,0x3C,0x90,0x4D,0x53,0x57,0x49,0x4E,0x34,0x2E,0x31,0x00,0x02,0x99,0x01,0x00,    //'MSWIN4.1'
    0x02,0x00,0x02,0x00,0x00,0xF8,0x99,0x99,0x3F,0x00,0xFF,0x00,0x11,0x11,0x11,0x11,
    0x99,0x99,0x99,0x99,0x80,0x00,0x29,0xCC,0xCC,0xCC,0xCC,0x4E,0x4F,0x20,0x4E,0x41,    //'NO NAME    '
    0x4D,0x45,0x20,0x20,0x20,0x20,0x46,0x41,0x54,0x31,0x36,0x20,0x20,0x20,0x33,0xC9,    //'FAT16   '
    0x8E,0xD1,0xBC,0xFC,0x7B,0x16,0x07,0xBD,0x78,0x00,0xC5,0x76,0x00,0x1E,0x56,0x16,
    0x55,0xBF,0x22,0x05,0x89,0x7E,0x00,0x89,0x4E,0x02,0xB1,0x0B,0xFC,0xF3,0xA4,0x06,
    0x1F,0xBD,0x00,0x7C,0xC6,0x45,0xFE,0x0F,0x38,0x4E,0x24,0x7D,0x20,0x8B,0xC1,0x99,
    0xE8,0x7E,0x01,0x83,0xEB,0x3A,0x66,0xA1,0x1C,0x7C,0x66,0x3B,0x07,0x8A,0x57,0xFC,
    0x75,0x06,0x80,0xCA,0x02,0x88,0x56,0x02,0x80,0xC3,0x10,0x73,0xED,0x33,0xC9,0xFE,
    0x06,0xD8,0x7D,0x8A,0x46,0x10,0x98,0xF7,0x66,0x16,0x03,0x46,0x1C,0x13,0x56,0x1E,
    0x03,0x46,0x0E,0x13,0xD1,0x8B,0x76,0x11,0x60,0x89,0x46,0xFC,0x89,0x56,0xFE,0xB8,
    0x20,0x00,0xF7,0xE6,0x8B,0x5E,0x0B,0x03,0xC3,0x48,0xF7,0xF3,0x01,0x46,0xFC,0x11,
    0x4E,0xFE,0x61,0xBF,0x00,0x07,0xE8,0x28,0x01,0x72,0x3E,0x38,0x2D,0x74,0x17,0x60,
    0xB1,0x0B,0xBE,0xD8,0x7D,0xF3,0xA6,0x61,0x74,0x3D,0x4E,0x74,0x09,0x83,0xC7,0x20,
    0x3B,0xFB,0x72,0xE7,0xEB,0xDD,0xFE,0x0E,0xD8,0x7D,0x7B,0xA7,0xBE,0x7F,0x7D,0xAC,
    0x98,0x03,0xF0,0xAC,0x98,0x40,0x74,0x0C,0x48,0x74,0x13,0xB4,0x0E,0xBB,0x07,0x00,
    0xCD,0x10,0xEB,0xEF,0xBE,0x82,0x7D,0xEB,0xE6,0xBE,0x80,0x7D,0xEB,0xE1,0xCD,0x16,
    0x5E,0x1F,0x66,0x8F,0x04,0xCD,0x19,0xBE,0x81,0x7D,0x8B,0x7D,0x1A,0x8D,0x45,0xFE,
    0x8A,0x4E,0x0D,0xF7,0xE1,0x03,0x46,0xFC,0x13,0x56,0xFE,0xB1,0x04,0xE8,0xC2,0x00,
    0x72,0xD7,0xEA,0x00,0x02,0x70,0x00,0x52,0x50,0x06,0x53,0x6A,0x01,0x6A,0x10,0x91,
    0x8B,0x46,0x18,0xA2,0x26,0x05,0x96,0x92,0x33,0xD2,0xF7,0xF6,0x91,0xF7,0xF6,0x42,
    0x87,0xCA,0xF7,0x76,0x1A,0x8A,0xF2,0x8A,0xE8,0xC0,0xCC,0x02,0x0A,0xCC,0xB8,0x01,
    0x02,0x80,0x7E,0x02,0x0E,0x75,0x04,0xB4,0x42,0x8B,0xF4,0x8A,0x56,0x24,0xCD,0x13,
    0x61,0x61,0x72,0x0A,0x40,0x75,0x01,0x42,0x03,0x5E,0x0B,0x49,0x75,0x77,0xC3,0x03,
    0x18,0x01,0x27,0x0D,0x0A,0x49,0x6E,0x76,0x61,0x6C,0x69,0x64,0x20,0x73,0x79,0x73,    //'Invalid system disk'
    0x74,0x65,0x6D,0x20,0x64,0x69,0x73,0x6B,0xFF,0x0D,0x0A,0x44,0x69,0x73,0x6B,0x20,    //'Disk I/O error'
    0x49,0x2F,0x4F,0x20,0x65,0x72,0x72,0x6F,0x72,0xFF,0x0D,0x0A,0x52,0x65,0x70,0x6C,    //'Replace the disk, and then press any key'
    0x61,0x63,0x65,0x20,0x74,0x68,0x65,0x20,0x64,0x69,0x73,0x6B,0x2C,0x20,0x61,0x6E,
    0x64,0x20,0x74,0x68,0x65,0x6E,0x20,0x70,0x72,0x65,0x73,0x73,0x20,0x61,0x6E,0x79,
    0x20,0x6B,0x65,0x79,0x0D,0x0A,0x00,0x00,0x49,0x4F,0x20,0x20,0x20,0x20,0x20,0x20,    //'IO      SYS'
    0x53,0x59,0x53,0x4D,0x53,0x44,0x4F,0x53,0x20,0x20,0x20,0x53,0x59,0x53,0x7F,0x01,    //'MSDOS   SYS'
    0x00,0x41,0xBB,0x00,0x07,0x60,0x66,0x6A,0x00,0xE9,0x3B,0xFF,0x00,0x00,0x55,0xAA,
    };



static CONST BYTE F32BootSector[]=
    {
    0xEB,0x58,0x90,0x4D,0x53,0x57,0x49,0x4E,0x34,0x2E,0x31,0x00,0x02,0x08,0x20,0x00,    //'MSWIN4.1'
    0x02,0x00,0x00,0x00,0x00,0xF8,0x00,0x00,0x3F,0x00,0xFF,0x00,0x11,0x11,0x11,0x11,
    0x99,0x99,0x99,0x99,0x22,0x22,0x22,0x22,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0x01,0x00,0x06,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x80,0x00,0x29,0xCC,0xCC,0xCC,0xCC,0x4E,0x4F,0x20,0x4E,0x41,0x4D,0x45,0x20,0x20,    //'NO NAME    '
    0x20,0x20,0x46,0x41,0x54,0x33,0x32,0x20,0x20,0x20,0xFA,0x33,0xC9,0x8E,0xD1,0xBC,    //'FAT32  '
    0xF8,0x7B,0x8E,0xC1,0xBD,0x78,0x00,0xC5,0x76,0x00,0x1E,0x56,0x16,0x55,0xBF,0x22,
    0x05,0x89,0x7E,0x00,0x89,0x4E,0x02,0xB1,0x0B,0xFC,0xF3,0xA4,0x8E,0xD9,0xBD,0x00,
    0x7C,0xC6,0x45,0xFE,0x0F,0x8B,0x46,0x18,0x88,0x45,0xF9,0x38,0x4E,0x40,0x7D,0x25,
    0x8B,0xC1,0x99,0xBB,0x00,0x07,0xE8,0x97,0x00,0x72,0x1A,0x83,0xEB,0x3A,0x66,0xA1,
    0x1C,0x7C,0x66,0x3B,0x07,0x8A,0x57,0xFC,0x75,0x06,0x80,0xCA,0x02,0x88,0x56,0x02,
    0x80,0xC3,0x10,0x73,0xED,0xBF,0x02,0x00,0x83,0x7E,0x16,0x00,0x75,0x45,0x8B,0x46,
    0x1C,0x8B,0x56,0x1E,0xB9,0x03,0x00,0x49,0x40,0x75,0x01,0x42,0xBB,0x00,0x7E,0xE8,
    0x5F,0x00,0x73,0x26,0xB0,0xF8,0x4F,0x74,0x1D,0x8B,0x46,0x32,0x33,0xD2,0xB9,0x03,
    0x00,0x3B,0xC8,0x77,0x1E,0x8B,0x76,0x0E,0x3B,0xCE,0x73,0x17,0x2B,0xF1,0x03,0x46,
    0x1C,0x13,0x56,0x1E,0xEB,0xD1,0x73,0x0B,0xEB,0x27,0x83,0x7E,0x2A,0x00,0x77,0x03,
    0xE9,0xFD,0x02,0xBE,0x7E,0x7D,0xAC,0x98,0x03,0xF0,0xAC,0x84,0xC0,0x74,0x17,0x3C,
    0xFF,0x74,0x09,0xB4,0x0E,0xBB,0x07,0x00,0xCD,0x10,0xEB,0xEE,0xBE,0x81,0x7D,0xEB,
    0xE5,0xBE,0x7F,0x7D,0xEB,0xE0,0x98,0xCD,0x16,0x5E,0x1F,0x66,0x8F,0x04,0xCD,0x19,
    0x41,0x56,0x66,0x6A,0x00,0x52,0x50,0x06,0x53,0x6A,0x01,0x6A,0x10,0x8B,0xF4,0x60,
    0x80,0x7E,0x02,0x0E,0x75,0x04,0xB4,0x42,0xEB,0x1D,0x91,0x92,0x33,0xD2,0xF7,0x76,
    0x18,0x91,0xF7,0x76,0x18,0x42,0x87,0xCA,0xF7,0x76,0x1A,0x8A,0xF2,0x8A,0xE8,0xC0,
    0xCC,0x02,0x0A,0xCC,0xB8,0x01,0x02,0x8A,0x56,0x40,0xCD,0x13,0x61,0x8D,0x64,0x10,
    0x5E,0x72,0x0A,0x40,0x75,0x01,0x42,0x03,0x5E,0x0B,0x49,0x75,0xB4,0xC3,0x03,0x18,
    0x01,0x27,0x0D,0x0A,0x49,0x6E,0x76,0x61,0x6C,0x69,0x64,0x20,0x73,0x79,0x73,0x74,    //'Invalid system disk'
    0x65,0x6D,0x20,0x64,0x69,0x73,0x6B,0xFF,0x0D,0x0A,0x44,0x69,0x73,0x6B,0x20,0x49,    //'Disk I/O error'
    0x2F,0x4F,0x20,0x65,0x72,0x72,0x6F,0x72,0xFF,0x0D,0x0A,0x52,0x65,0x70,0x6C,0x61,    //'Replace the disk, and then press any key'
    0x63,0x65,0x20,0x74,0x68,0x65,0x20,0x64,0x69,0x73,0x6B,0x2C,0x20,0x61,0x6E,0x64,
    0x20,0x74,0x68,0x65,0x6E,0x20,0x70,0x72,0x65,0x73,0x73,0x20,0x61,0x6E,0x79,0x20,
    0x6B,0x65,0x79,0x0D,0x0A,0x00,0x00,0x00,0x49,0x4F,0x20,0x20,0x20,0x20,0x20,0x20,    //'IO      SYS'
    0x53,0x59,0x53,0x4D,0x53,0x44,0x4F,0x53,0x20,0x20,0x20,0x53,0x59,0x53,0x7E,0x01,    //'MSDOS   SYS'
    0x00,0x57,0x49,0x4E,0x42,0x4F,0x4F,0x54,0x20,0x53,0x59,0x53,0x00,0x00,0x55,0xAA,    //'WINBOOT SYS'

    0x52,0x52,0x61,0x41,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,    //'RRaA'
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x72,0x72,0x41,0x61,0xFF,0xFF,0xFF,0xFF,0x02,0x00,0x00,0x00,    //'rrAa'
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x55,0xAA,

    0xFA,0x66,0x0F,0xB6,0x46,0x10,0x66,0x8B,0x4E,0x24,0x66,0xF7,0xE1,0x66,0x03,0x46,
    0x1C,0x66,0x0F,0xB7,0x56,0x0E,0x66,0x03,0xC2,0x33,0xC9,0x66,0x89,0x46,0xFC,0x66,
    0xC7,0x46,0xF8,0xFF,0xFF,0xFF,0xFF,0xFA,0x66,0x8B,0x46,0x2C,0x66,0x83,0xF8,0x02,
    0x0F,0x82,0xCF,0xFC,0x66,0x3D,0xF8,0xFF,0xFF,0x0F,0x0F,0x83,0xC5,0xFC,0x66,0x0F,
    0xA4,0xC2,0x10,0xFB,0x52,0x50,0xFA,0x66,0xC1,0xE0,0x10,0x66,0x0F,0xAC,0xD0,0x10,
    0x66,0x83,0xE8,0x02,0x66,0x0F,0xB6,0x5E,0x0D,0x8B,0xF3,0x66,0xF7,0xE3,0x66,0x03,
    0x46,0xFC,0x66,0x0F,0xA4,0xC2,0x10,0xFB,0xBB,0x00,0x07,0x8B,0xFB,0xB9,0x01,0x00,
    0xE8,0xBE,0xFC,0x0F,0x82,0xAA,0xFC,0x38,0x2D,0x74,0x1E,0xB1,0x0B,0x56,0xBE,0xD8,
    0x7D,0xF3,0xA6,0x5E,0x74,0x19,0x03,0xF9,0x83,0xC7,0x15,0x3B,0xFB,0x72,0xE8,0x4E,
    0x75,0xD6,0x58,0x5A,0xE8,0x66,0x00,0x72,0xAB,0x83,0xC4,0x04,0xE9,0x64,0xFC,0x83,
    0xC4,0x04,0x8B,0x75,0x09,0x8B,0x7D,0x0F,0x8B,0xC6,0xFA,0x66,0xC1,0xE0,0x10,0x8B,
    0xC7,0x66,0x83,0xF8,0x02,0x72,0x3B,0x66,0x3D,0xF8,0xFF,0xFF,0x0F,0x73,0x33,0x66,
    0x48,0x66,0x48,0x66,0x0F,0xB6,0x4E,0x0D,0x66,0xF7,0xE1,0x66,0x03,0x46,0xFC,0x66,
    0x0F,0xA4,0xC2,0x10,0xFB,0xBB,0x00,0x07,0x53,0xB9,0x04,0x00,0xE8,0x52,0xFC,0x5B,
    0x0F,0x82,0x3D,0xFC,0x81,0x3F,0x4D,0x5A,0x75,0x08,0x81,0xBF,0x00,0x02,0x42,0x4A,
    0x74,0x06,0xBE,0x80,0x7D,0xE9,0x0E,0xFC,0xEA,0x00,0x02,0x70,0x00,0x03,0xC0,0x13,
    0xD2,0x03,0xC0,0x13,0xD2,0xE8,0x18,0x00,0xFA,0x26,0x66,0x8B,0x01,0x66,0x25,0xFF,
    0xFF,0xFF,0x0F,0x66,0x0F,0xA4,0xC2,0x10,0x66,0x3D,0xF8,0xFF,0xFF,0x0F,0xFB,0xC3,
    0xBF,0x00,0x7E,0xFA,0x66,0xC1,0xE0,0x10,0x66,0x0F,0xAC,0xD0,0x10,0x66,0x0F,0xB7,
    0x4E,0x0B,0x66,0x33,0xD2,0x66,0xF7,0xF1,0x66,0x3B,0x46,0xF8,0x74,0x44,0x66,0x89,
    0x46,0xF8,0x66,0x03,0x46,0x1C,0x66,0x0F,0xB7,0x4E,0x0E,0x66,0x03,0xC1,0x66,0x0F,
    0xB7,0x5E,0x28,0x83,0xE3,0x0F,0x74,0x16,0x3A,0x5E,0x10,0x0F,0x83,0xA4,0xFB,0x52,
    0x66,0x8B,0xC8,0x66,0x8B,0x46,0x24,0x66,0xF7,0xE3,0x66,0x03,0xC1,0x5A,0x52,0x66,
    0x0F,0xA4,0xC2,0x10,0xFB,0x8B,0xDF,0xB9,0x01,0x00,0xE8,0xB4,0xFB,0x5A,0x0F,0x82,
    0x9F,0xFB,0xFB,0x8B,0xDA,0xC3,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x55,0xAA,
    };




//-----------------------------------------------------------------------------
//      섹터를 0으로 Clear함
//-----------------------------------------------------------------------------
#define COPYSECTORQTY   127
LOCAL(VOID) FillPhysicalSector(int Lun, DWORD SctNo, int SctQty, LPBYTE SctBuff)
    {
    ZeroMem(SctBuff, SUPPORTSECTORBYTES);
    while (SctQty--) STORAGE_Write(Lun, SctBuff, SctNo++, 1);
    }



//-----------------------------------------------------------------------------
//      디스크 시리얼 번호를 만듦
//-----------------------------------------------------------------------------
LOCAL(DWORD) SetDiskSerialNo(VOID)
    {
    SYSTEMTIME ST;

    GetLocalTime(&ST);
    return CalculateCRC((BYTE*)&ST, sizeof(SYSTEMTIME), 0);
    }



//-----------------------------------------------------------------------------
//      1인 비트가 1개인 Num 이상인 가장 작은 수를 리턴
//-----------------------------------------------------------------------------
LOCAL(int) Cnv1BitNum(int Num)
    {
    int _1BitNo=1;
    while (_1BitNo<Num) _1BitNo<<=1;
    return _1BitNo;
    }




//-----------------------------------------------------------------------------
//      HDD의 주어진 파티션을 빠른Format을 합니다
//      FALSE를 리턴하면 FAT32으로는 포맷할 수 없음을 나타냄
//-----------------------------------------------------------------------------
BOOL WINAPI FormatFAT32(int DrvNo, DWORD StartSctNo, DWORD TotalSctors, DIRENTRY *VolDE, UINT ClusterSize, LPBYTE SctBuff)
    {
    int  Rslt=FALSE, FatSctQty, RootDirSctQty, FatEntryQty;
    BPB_F32 *BR;

    CopyMem(SctBuff, F32BootSector, sizeof(BPB_F32));
    BR=(BPB_F32*)SctBuff;

    BR->StartRtvSctNo=StartSctNo;
    BR->SectorsPerHead=63;      //(WORD)DG.SectorsPerTrack;
    BR->HeadQty=128;            //(WORD)DG.TracksPerCylinder;
    BR->FS32SerialNo=SetDiskSerialNo();
    BR->FS32PhyDiskNo=StartSctNo!=0 ? 0xF0:0x00;
    BR->MediaSign=StartSctNo!=0 ? 0xF8:0xF0;            //파티션이 있으면 HDD, 아니면 FDD
    if (VolDE!=NULL) CopyMem(BR->FS32VolumeLabel, VolDE->FileName, 11);

    if (ClusterSize!=0) BR->SectorsPerCluster=ClusterSize>>9;
    else                BR->SectorsPerCluster=GetMin(64, Cnv1BitNum(TotalSctors>>21));
    //if (BR->SectorsPerCluster<8) BR->SectorsPerCluster=8;
    //FAT32 클러스티 크기 결정
    //512MB ~ 8GB,    8 sectors per cluster
    //      ~ 16GB,   16 sectors per cluster
    //      ~ 32GB,   32 sectors per cluster
    //      ~ 2TB,    64 sectors per cluster
    BR->BigSctPerFAT=(((TotalSctors/BR->SectorsPerCluster)<<2)+511)>>9;
    BR->BigTotalSectors=TotalSctors;

    RootDirSctQty=BR->SectorsPerCluster;
    FatEntryQty=(TotalSctors-BR->SystemUseSctNo-BR->BigSctPerFAT-BR->BigSctPerFAT-RootDirSctQty)/BR->SectorsPerCluster+2;
    if (FatEntryQty<=FAT16_MAXQTY)
        {
        Printf("This capacity is too small to be formatting with FAT32." CRLF);
        goto ProcExit;
        }

    Printf("Formatting..." CRLF);

    STORAGE_Write(DrvNo, (LPCBYTE)BR, StartSctNo, 1); StartSctNo++;             //6=BR->BkUpBootSctOfs
    STORAGE_Write(DrvNo, F32BootSector+512, StartSctNo, 2); StartSctNo+=5;

    STORAGE_Write(DrvNo, (LPCBYTE)BR, StartSctNo, 1); StartSctNo++;             //32=BR->SystemUseSctNo
    STORAGE_Write(DrvNo, F32BootSector+512, StartSctNo, 2); StartSctNo+=32-7;

    FatSctQty=BR->BigSctPerFAT-1;

    Printf("Writing First FAT..." CRLF);
    ZeroMem(BR, sizeof(BPB_F32));
    *(DWORD*)((LPBYTE)BR+0)=0x0FFFFFF0;         //Chkdsk 로 이상없음을 검증함
    *(DWORD*)((LPBYTE)BR+4)=0x0FFFFFFF;
    *(DWORD*)((LPBYTE)BR+8)=0x0FFFFFFF;         //2번클러스터: 루트디렉토리
    STORAGE_Write(DrvNo, (LPCBYTE)BR, StartSctNo, 1); StartSctNo++;
    FillPhysicalSector(DrvNo, StartSctNo, FatSctQty, (LPBYTE)BR); StartSctNo+=FatSctQty;    //SctBuffer사용

    Printf("Writing Second FAT..." CRLF);
    ZeroMem(BR, sizeof(BPB_F32));
    *(DWORD*)((LPBYTE)BR+0)=0x0FFFFFF0;
    *(DWORD*)((LPBYTE)BR+4)=0x0FFFFFFF;
    *(DWORD*)((LPBYTE)BR+8)=0x0FFFFFFF;
    STORAGE_Write(DrvNo, (LPCBYTE)BR, StartSctNo, 1); StartSctNo++;
    FillPhysicalSector(DrvNo, StartSctNo, FatSctQty, (LPBYTE)BR); StartSctNo+=FatSctQty;    //SctBuffer사용

    if (VolDE!=NULL)
        {
        ZeroMem(BR, sizeof(BPB_F32));
        CopyMem(BR, VolDE, sizeof(DIRENTRY));
        STORAGE_Write(DrvNo, (LPCBYTE)BR, StartSctNo, 1); StartSctNo++;
        RootDirSctQty--;
        }
    FillPhysicalSector(DrvNo, StartSctNo, RootDirSctQty, (LPBYTE)BR);           //SctBuffer사용, 두번째 FAT다음이 2번 클러스터이고 거기가 RootDir임

    Printf("Completed Formatting." CRLF);
    Rslt++;

    ProcExit:
    return Rslt;
    }




//-----------------------------------------------------------------------------
//      HDD의 주어진 파티션을 FAT16으로 빠른Format을 합니다
//-----------------------------------------------------------------------------
BOOL WINAPI FormatFAT16(int DrvNo, DWORD StartSctNo, DWORD TotalSctors, DIRENTRY *VolDE, UINT ClusterSize, LPBYTE SctBuff)
    {
    int  Rslt=FALSE, FatSctQty, RootDirSctQty, FatEntryQty;
    BPB_F16 *BR;

    Printf("TotalSctors=%u" CRLF,TotalSctors);
    if (TotalSctors<16384)  //8MByte 이하는 지원안함
        {
        Printf("Too small to be formatting with FAT16." CRLF);
        goto ProcExit;
        }
    CopyMem(SctBuff, F16BootSector, 512);
    BR=(BPB_F16*)SctBuff;

    BR->StartRtvSctNo=StartSctNo;
    BR->BigTotalSectors=TotalSctors;
    //BR->SectorsPerHead=63;
    //BR->HeadQty=255;
    //BR->PhyDiskNo=StartSctNo!=0 ? 0xF0:0x00;
    //BR->MediaSign=StartSctNo!=0 ? 0xF8:0xF0;      //파티션이 있으면 HDD, 아니면 FDD
    if (VolDE!=NULL) CopyMem(BR->VolumeLabel, VolDE->FileName, 11);

    if (ClusterSize!=0) BR->SectorsPerCluster=ClusterSize>>9;
    else if (TotalSctors<65536)     BR->SectorsPerCluster=1;    //8~32M
    else if (TotalSctors<131072)    BR->SectorsPerCluster=2;    //32~64M
    else if (TotalSctors<262144)    BR->SectorsPerCluster=4;    //64~128M
    else if (TotalSctors<524288)    BR->SectorsPerCluster=8;    //128~256M
    else if (TotalSctors<1048576)   BR->SectorsPerCluster=16;   //256~512M
    else if (TotalSctors<2097152)   BR->SectorsPerCluster=32;   //512~1G
    else                            BR->SectorsPerCluster=64;   //1G~2G

    BR->RootDirEntrys=512;
    RootDirSctQty=(BR->RootDirEntrys*sizeof(DIRENTRY)+511)>>9;
    BR->SectorsPerFAT=(WORD)((((TotalSctors/BR->SectorsPerCluster)<<1)+511)>>9);

    FatEntryQty=(TotalSctors-BR->SystemUseSctNo-BR->SectorsPerFAT-BR->SectorsPerFAT-RootDirSctQty)/BR->SectorsPerCluster+2;
    Printf("SectorsPerCluster=%d" CRLF, BR->SectorsPerCluster);
    Printf("FatEntryQty=%u" CRLF, FatEntryQty);
    if (FatEntryQty<=FAT12_MAXQTY)
        {
        Printf("This capacity is too small to be formatting with FAT16." CRLF);
        goto ProcExit;
        }

    Printf("Formatting..." CRLF);

    STORAGE_Write(DrvNo, (LPCBYTE)BR, StartSctNo, 1); StartSctNo++;

    FatSctQty=BR->SectorsPerFAT-1;


    Printf("Writing First FAT..." CRLF);
    ZeroMem(BR, sizeof(BPB_F16)); *(DWORD*)BR=0xFFFFFFF0;
    STORAGE_Write(DrvNo, (LPCBYTE)BR, StartSctNo, 1); StartSctNo++;
    FillPhysicalSector(DrvNo, StartSctNo, FatSctQty, (LPBYTE)BR); StartSctNo+=FatSctQty;    //SctBuffer사용

    Printf("Writing Second FAT..." CRLF);
    ZeroMem(BR, sizeof(BPB_F16)); *(DWORD*)BR=0xFFFFFFF0;
    STORAGE_Write(DrvNo, (LPCBYTE)BR, StartSctNo, 1); StartSctNo++;
    FillPhysicalSector(DrvNo, StartSctNo, FatSctQty, (LPBYTE)BR); StartSctNo+=FatSctQty;    //SctBuffer사용

    if (VolDE!=NULL)
        {
        ZeroMem(BR, sizeof(BPB_F16));
        CopyMem(BR, VolDE, sizeof(DIRENTRY));
        STORAGE_Write(DrvNo, (LPCBYTE)BR, StartSctNo, 1); StartSctNo++;
        RootDirSctQty--;
        }
    FillPhysicalSector(DrvNo, StartSctNo, RootDirSctQty, (LPBYTE)BR);           //SctBuffer사용, 두번째 FAT다음이 2번 클러스터이고 거기가 RootDir임

    Printf("Completed Formatting." CRLF);
    Rslt++;

    ProcExit:
    return Rslt;
    }




BOOL WINAPI JFAT_Formatting(LPCSTR DriveRootPath)
    {
    BOOL Rslt=FALSE;
    DISKCONTROLBLOCK *Dcb;

    if ((Dcb=GetDCB(&DriveRootPath, FALSE, FALSE))==NULL) goto ProcExit;
    JFAT_Lock(Dcb);

    if ((Rslt=FormatFAT32(Dcb->Lun, 0, Dcb->DiskSectorQty, NULL, 8192, Dcb->SctBuffer))==FALSE)
         Rslt=FormatFAT16(Dcb->Lun, 0, Dcb->DiskSectorQty, NULL, 0,    Dcb->SctBuffer);

    if (Rslt) ReadVolID(Dcb);

    ProcExit:
    JFAT_Unlock(Dcb);
    return Rslt;
    }
#endif //JFAT_READOLNY==0




//-----------------------------------------------------------------------------
//      멘뒤에서 부터 안쓴 공간의 시작위치(섹터번호)를 리턴함
//-----------------------------------------------------------------------------
DWORD WINAPI JFAT_GetNoUsePos(LPCSTR DriveRootPath)
    {
    DWORD LastFreeSct;
    DISKCONTROLBLOCK *Dcb;

    if ((Dcb=GetDCB(&DriveRootPath, FALSE, TRUE))!=NULL)
        {
        JFAT_Lock(Dcb);
        FindLastFreeClustNo(Dcb);
        LastFreeSct=ClusterNoToSectorNo(Dcb, Dcb->LastFreeClustNo);
        JFAT_Unlock(Dcb);
        }
    return LastFreeSct;
    }




//-----------------------------------------------------------------------------
//      볼륨 정보를 읽어옴 (FAT종류와 총섹터수)
//      MakeImageDisk.exe 에서 사용
//-----------------------------------------------------------------------------
BOOL WINAPI JFAT_GetInfo(UINT Lun, int *lpFatType, DWORD *lpTotalScts, DWORD *lpFreeScts)
    {
    BOOL Rslt=FALSE;
    DISKCONTROLBLOCK *Dcb;

    if ((Dcb=CheckLunSpace(Lun))!=NULL)
        {
        *lpFatType=Dcb->FatType;
        *lpTotalScts=ClusterNoToSectorNo(Dcb, Dcb->TotalClusters+2);
        if (lpFreeScts) *lpFreeScts=FindLastFreeClustNo(Dcb) * Dcb->SctsPerCluster;
        Rslt++;
        }
    return Rslt;
    }




//-----------------------------------------------------------------------------
//      JFAT 초기화
//-----------------------------------------------------------------------------
BOOL WINAPI JFAT_Init(UINT Lun, BOOL Verbose)
    {
    BOOL Rslt=FALSE;
    UINT SectorSize;
    DISKCONTROLBLOCK *Dcb;
    CHAR Buff[40];

    if ((Dcb=CheckLunSpace(Lun))==NULL) goto ProcExit;
    ZeroMem(Dcb, sizeof(DISKCONTROLBLOCK));
    Dcb->Lun=Lun;
    Dcb->FatCachedSctNo=~0;     //-1이면 캐쉬되지 않은 것임
    #ifdef USE_JOS
    Dcb->DCB_Sem=JOSSemCreate(1);
    #endif

    STORAGE_Init(Lun);
    STORAGE_GetCapacity(Lun, &Dcb->DiskSectorQty, &SectorSize);
    if (SectorSize!=SUPPORTSECTORBYTES)
        {
        Printf("Only %d sector sizes are supported." CRLF, SUPPORTSECTORBYTES);
        goto ProcExit;
        }
    MakeSizeStrEx(Buff, (UINT64)Dcb->DiskSectorQty*SUPPORTSECTORBYTES);
    if (Verbose) Printf("%c: Disk Size=%s" CRLF, Lun+'A', Buff);
    if ((Rslt=ReadVolID(Dcb))!=FALSE)
        {
        if (Verbose) Printf("%c: FAT%d" CRLF, Lun+'A', Dcb->FatType);
        }

    ProcExit:
    return Rslt;
    }




//-----------------------------------------------------------------------------
//      디렉토리 표시
//-----------------------------------------------------------------------------
int WINAPI Mon_FileSystem(int PortNo, LPCSTR MonCmd, LPCSTR Arg, LPCSTR CmdLine)
    {
    int I, Rslt=MONRSLT_SYNTAXERR, hFile=HFILE_ERROR, CrcMode=0;
    DWORD Crc=UMINUS1;
    SYSTEMTIME ST;
    WIN32_FIND_DATA *WFD;
    CHAR FileSize[16], Buff[100];

    if (Arg[0]=='?')
        {
        PrintfII(PortNo, "FS DIR/CAT/DEL/FORMAT ... File System" CRLF);
        Rslt=MONRSLT_EXIT;
        goto ProcExit;
        }

    Rslt=MONRSLT_EXIT;

    if (CompMemStrI(Arg, "DIR")==0)
        {
        Arg=(LPSTR)SkipSpace(Arg+3);

        if ((WFD=JFAT_FindFirstFile(Arg))!=NULL)
            {
            do  {
                UnpackTotalSecond(&ST, WFD->ftLastWriteTime);

                if (WFD->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                     Jsprintf(FileSize, "%-13s", "<DIR>");
                else if (WFD->dwFileAttributes & FILE_ATTRIBUTE_VOLUME)
                     Jsprintf(FileSize, "%-13s", "<VOL>");
                else Jsprintf(FileSize, "%,13u", WFD->nFileSizeLow);

                PrintfII(PortNo, "%d-%02d-%02d %02d:%02d:%02d %s %s", ST.wYear, ST.wMonth, ST.wDay, ST.wHour, ST.wMinute, ST.wSecond, FileSize, WFD->cAlternateFileName);
                if (lstrcmp(WFD->cAlternateFileName, WFD->cFileName)!=0) PrintfII(PortNo, " [%s]" CRLF, WFD->cFileName);
                else PrintfII(PortNo, CRLF);
                } while (JFAT_FindNextFile(WFD)!=FALSE);

            PrintfII(PortNo, "%,u bytes free" CRLF, (WFD->Dcb->SctsPerCluster*SUPPORTSECTORBYTES) * FindLastFreeClustNo(WFD->Dcb));
            FindClose(WFD);
            }
        Rslt=MONRSLT_OK;
        }
    else if (CompMemStrI(Arg, "CAT")==0 || CompMemStrI(Arg, "CRC")==0)
        {
        if (CompMemStrI(Arg, "CRC")==0) CrcMode=1;

        Arg=(LPSTR)SkipSpace(Arg+3);
        if (Arg[0]==0)
            {
            #if JFAT_READOLNY==0
            NoFName:
            #endif
            PrintfII(PortNo, "No FileName" CRLF);
            goto CatExit;
            }

        if ((hFile=JFAT_Open(Arg, OF_READ))==HFILE_ERROR) goto CatExit;
        for (;;)
            {
            if ((I=JFAT_Read(hFile, Buff, sizeof(Buff)-1))<=0) break;
            if (CrcMode==0) {Buff[I]=0; UART_TxStrIT(COM_DEBUG, Buff);}
            else            Crc=CalculateCRC((LPCBYTE)Buff, I, Crc);
            }
        if (CrcMode) PrintfII(PortNo, "CRC32: %X"CRLF, ~Crc);
        Rslt=MONRSLT_OK;

        CatExit:
        if (hFile!=HFILE_ERROR) JFAT_Close(hFile);
        }
    #if JFAT_READOLNY==0
    else if (CompMemStrI(Arg, "DEL")==0)
        {
        Arg=(LPSTR)SkipSpace(Arg+3);
        if (Arg[0]==0) goto NoFName;
        if (JFAT_DeleteFile(Arg)!=FALSE) PrintfII(PortNo, "Delete OK" CRLF);
        else PrintfII(PortNo, "Delete Fail" CRLF);
        }
    else if (CompMemStrI(Arg, "MD")==0)
        {
        Arg=(LPSTR)SkipSpace(Arg+2);
        if (Arg[0]==0) goto NoFName;
        if (JFAT_CreateDirectory(Arg)!=FALSE) PrintfII(PortNo, "CreateDir OK" CRLF);
        else PrintfII(PortNo, "CreateDir Fail" CRLF);
        }
    else if (CompMemStrI(Arg, "FORMAT")==0)
        {
        Arg=(LPSTR)SkipSpace(Arg+6);
        if (JFAT_Formatting(Arg)!=FALSE) Rslt=MONRSLT_OK;
        else PrintfII(PortNo, "Format Fail" CRLF);
        }
    #endif //JFAT_READOLNY==0
    else Rslt=MONRSLT_SYNTAXERR;

    ProcExit:
    return Rslt;
    }



