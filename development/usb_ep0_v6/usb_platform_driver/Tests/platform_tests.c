#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Uefi.h>
#include "../Library/MotoG20UsbPlatformLib.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x); exit(1); } } while(0)
typedef struct { UINTN Addr; UINT32 Val; } CELL;
static CELL cells[200];
static UINTN cellCount, mmioCount, hcMmioCount, bqOn, bqOff, feed, efuseProgram, gateSet;
static UINT16 pmic[0x8000];
static UINT8 bq[256], tx[2], txLen, regIndex;
static UINT64 timeNs;
static UINT32 adiValue, i2cStatus;
static BOOLEAN mapFail, allMapped, boostSense, nackOff, nackOn, efuseTimeout, adiBadAddress, mapReserved;
static EFI_GCD_MEMORY_SPACE_DESCRIPTOR regions[9];
static EFI_TPL currentTpl;
static UINTN failWriteAddress;
static BOOLEAN nackFeed;
static BOOLEAN failTimerCancel, timerActive;
static UINT16 adiFailReg;
static UINTN adiReadCounts[0x800], pmicWriteCounts[0x800];

VOID *ZeroMem(VOID *Buffer, UINTN Length) { return memset(Buffer,0,Length); }
VOID *CopyMem(VOID *Destination, CONST VOID *Source, UINTN Length) { return memcpy(Destination,Source,Length); }

static CELL *cell(UINTN a) { UINTN i; for(i=0;i<cellCount;i++) if(cells[i].Addr==a) return &cells[i]; CHECK(cellCount<200); cells[cellCount].Addr=a; return &cells[cellCount++]; }
UINT32 MmioRead32(UINTN a) {
 ++mmioCount; CHECK(allMapped);
 if(a>=MUSB && a<MUSB+0x2000) ++hcMmioCount;
 if(a==ADI+0x2c) return adiValue;
 if(a==ADI+0x30) return BIT10;
 if(a==HWLOCK+0x830) return 0;
 if(a==I2C+0x14) return i2cStatus;
 return cell(a)->Val;
}
UINT32 MmioWrite32(UINTN a,UINT32 v) {
 ++mmioCount; CHECK(allMapped);
 if(a>=MUSB && a<MUSB+0x2000) ++hcMmioCount;
 if (a==failWriteAddress) return v; // Persistent failure: write does not stick.
 if(a==ADI+0x28) {
   UINT16 r=(UINT16)(v-PMIC);
   ++adiReadCounts[r/4];
   adiValue=((r>>2)<<16)|pmic[r/4];
   if(adiBadAddress || r==adiFailReg) adiValue^=0x10000;
 } else if(a>=PMIC && a<PMIC+0x2000) {
   UINT16 r=(UINT16)(a-PMIC);
   ++pmicWriteCounts[r/4];
   pmic[r/4]=(UINT16)v;
   if(r==0x310) { if(v&BIT0) ++efuseProgram; if(v&BIT1) pmic[0x314/4]=efuseTimeout?0:BIT4; }
 } else if(a==AON+0x1004 || a==AON+0x1008 || a==AON+0x1138 || a==AP_APB+0x1000) {
   cell(a-0x1000)->Val|=v; ++gateSet;
 } else if(a==I2C+0x2c) { txLen=0; i2cStatus=0; }
 else if(a==I2C+0x14) { i2cStatus=v; }
 else if(a==I2C && (v&BIT0)) {
   i2cStatus=BIT2;
   if(!(v&BIT3)) {
     CHECK(txLen>=1); regIndex=tx[0];
     if(txLen==2) {
       if(regIndex==1 && tx[1]==0x3a) { ++bqOn; if(nackOn) i2cStatus=BIT1; }
       if(regIndex==1 && tx[1]==0x5a) { ++bqOff; if(nackOff) i2cStatus=BIT1; }
       if(regIndex==1 && tx[1]==0x7a) { ++feed; if(nackFeed) i2cStatus=BIT1; }
       if(!(i2cStatus&BIT1)) {
         bq[regIndex]=tx[1];
         if(regIndex==1) pmic[EIC_DATA/4]=(tx[1]&0x20)&&boostSense;
       }
     }
   }
 } else cell(a)->Val=v;
 return v;
}
UINT8 MmioRead8(UINTN a) { CHECK(a==I2C+0xc); return bq[regIndex]; }
UINT8 MmioWrite8(UINTN a,UINT8 v) { CHECK(a==I2C+0x10); CHECK(txLen<2); tx[txLen++]=v; return v; }
VOID MicroSecondDelay(UINTN us) { timeNs+=(UINT64)us*1000; }
UINT64 GetPerformanceCounter(VOID) { return timeNs; }
UINT64 GetTimeInNanoSecond(UINT64 ticks) { return ticks; }
static EFI_STATUS desc(UINT64 a,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *d) {
 UINTN i; for(i=0;i<9;i++) if(a==mRegions[i].Base) { *d=regions[i]; return EFI_SUCCESS; }
 return EFI_INVALID_PARAMETER;
}
static EFI_STATUS add(TEST_GCD t,UINT64 a,UINT64 l,UINT64 c) {
 UINTN i; (void)c; if(mapFail) return EFI_OUT_OF_RESOURCES;
 for(i=0;i<9;i++) if(a==mRegions[i].Base) { regions[i].GcdMemoryType=t; regions[i].Length=l; return EFI_SUCCESS; }
 return EFI_INVALID_PARAMETER;
}
static EFI_STATUS attrs(UINT64 a,UINT64 l,UINT64 c) {
 UINTN i; (void)l; for(i=0;i<9;i++) if(a==mRegions[i].Base) regions[i].Attributes=c;
 allMapped=TRUE; for(i=0;i<9;i++) if(regions[i].Attributes!=EFI_MEMORY_UC) allMapped=FALSE;
 return EFI_SUCCESS;
}
static EFI_STATUS event(UINTN type,EFI_TPL tpl,EFI_EVENT_NOTIFY cb,VOID *ctx,EFI_EVENT *e) { (void)type;(void)tpl;(void)cb;(void)ctx;*e=(void*)1;return EFI_SUCCESS; }
static EFI_STATUS timer(EFI_EVENT e,UINTN type,UINT64 n) {
 (void)e;(void)n;
 if(type==TimerCancel && failTimerCancel) return EFI_DEVICE_ERROR;
 timerActive=type==TimerPeriodic;return EFI_SUCCESS;
}
static EFI_TPL raise(EFI_TPL t) { EFI_TPL old=currentTpl;CHECK(t>=old);currentTpl=t;return old; }
static VOID restore(EFI_TPL t) { currentTpl=t; }
static EFI_STATUS closeEvent(EFI_EVENT e) { (void)e;return EFI_SUCCESS; }
static EFI_DXE_SERVICES ds={desc,add,attrs}; EFI_DXE_SERVICES *gDS=&ds;
static EFI_BOOT_SERVICES bs={event,timer,raise,restore,closeEvent}; EFI_BOOT_SERVICES *gBS=&bs;

static void reset(void) {
 UINTN i;
 memset(cells,0,sizeof(cells));cellCount=mmioCount=hcMmioCount=bqOn=bqOff=feed=efuseProgram=gateSet=0;
 memset(pmic,0,sizeof(pmic));memset(bq,0,sizeof(bq));txLen=regIndex=0;
 timeNs=adiValue=i2cStatus=0;mapFail=allMapped=nackOff=nackOn=efuseTimeout=adiBadAddress=mapReserved=FALSE;boostSense=TRUE;
 for(i=0;i<9;i++) { regions[i].BaseAddress=mRegions[i].Base;regions[i].Length=mRegions[i].Size;regions[i].GcdMemoryType=EfiGcdMemoryTypeNonExistent;regions[i].Attributes=0; }
 currentTpl=TPL_APPLICATION;
 failWriteAddress=0;nackFeed=failTimerCancel=timerActive=FALSE;
 adiFailReg=0xffff;memset(adiReadCounts,0,sizeof(adiReadCounts));memset(pmicWriteCounts,0,sizeof(pmicWriteCounts));
 memset(&mRoleDiagnostic,0,sizeof(mRoleDiagnostic));mRoleDiagnosticInitialized=mRoleSnapshotTaken=FALSE;
 mSnapshotRoleState=0;mSnapshotRoleReadStatus=0;
 mSavedCount=0;mPrepared=mInfra=mRoleReady=mI2cReady=mActive=mBoostAttempted=mBcTouched=mVddSaved=mFault=mRoleSaved=FALSE;
 mEicOriginal=mTrimOriginal=mModeOriginal=mIntenOriginal=0;
 mVddOriginal=mVddVolOriginal=mTypecState=0;mVbusPresent=FALSE;mTimer=NULL;mLastFeed=0;mFeedDue=FALSE;mLastStatus=EFI_NOT_READY;mStage=MotoUsbStageNone;
 pmic[TYPEC_STATUS/4]=5;pmic[0x314/4]=BIT2;pmic[0x304/4]=0x0155;pmic[VDD_PD/4]=1;pmic[VDD_VOL/4]=42;bq[0xb]=0x10;
}
int main(void) {
 UINTN base, previousMmio, reads, i;BOOLEAN active, vbus;EFI_STATUS s;
 UINTN writesBefore, writesAfter, j;
 UINT16 typec;
 MOTO_G20_USB_ROLE_DIAGNOSTIC d;
 reset();CHECK(MotoG20UsbPlatformInit(&base)==EFI_NOT_READY);CHECK(mmioCount==0);
 mapFail=TRUE;CHECK(EFI_ERROR(MotoG20UsbPlatformPrepare()));CHECK(mmioCount==0);puts("PASS mapping failure prevents all MMIO");
 reset();regions[0].GcdMemoryType=EfiGcdMemoryTypeReserved;CHECK(MotoG20UsbPlatformPrepare()==EFI_ACCESS_DENIED);CHECK(mmioCount==0);puts("PASS reserved GCD range rejected");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);pmic[TYPEC_STATUS/4]=2;pmic[EIC_DATA/4]=1;
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_NOT_READY);CHECK(bqOn==0 && base==0);CHECK(efuseProgram==0);puts("PASS PC sink cannot enable boost");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);pmic[EIC_DATA/4]=1;
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_NOT_READY);CHECK(bqOn==0);puts("PASS preexisting VBUS blocks source even with attached-source state");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);CHECK(MotoG20UsbPlatformInit(&base)==0);CHECK(base==MUSB && bqOn==1 && feed==1 && gateSet==5);CHECK(efuseProgram==0);
 CHECK(MotoG20UsbPlatformPollRole(&active)==0 && active);timeNs+=15000000000ULL;
 CHECK(MotoG20UsbPlatformPollRole(&active)==0 && active && feed==2);
 CHECK(MotoG20UsbPlatformStop()==0);CHECK(bqOff==1 && !(pmic[BC12/4]&1));CHECK(pmic[VDD_PD/4]==1 && pmic[VDD_VOL/4]==42);CHECK(MotoG20UsbPlatformStop()==0 && bqOff==1);puts("PASS verified host sequencing, watchdog and idempotent stop");
 reset();boostSense=FALSE;CHECK(MotoG20UsbPlatformPrepare()==0);CHECK(MotoG20UsbPlatformInit(&base)==EFI_NO_RESPONSE);CHECK(bqOn==1 && bqOff==1 && base==0);CHECK((cell(AON+0x204)->Val&BIT24)==0);puts("PASS absent physical VBUS rolls back without valid override");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);CHECK(MotoG20UsbPlatformInit(&base)==0);nackOff=TRUE;pmic[TYPEC_STATUS/4]=2;
 CHECK(EFI_ERROR(MotoG20UsbPlatformPollRole(&active)) && !active);CHECK(!(pmic[BC12/4]&1) && mFault && mBoostAttempted && mI2cReady);
 s=MotoG20UsbPlatformInit(&base);CHECK(EFI_ERROR(s) && bqOn==1 && base==0);nackOff=FALSE;CHECK(MotoG20UsbPlatformStop()==0 && !mBoostAttempted);puts("PASS failed OFF still restores BC1.2, preserves retry transport, latches enable fault");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);efuseTimeout=TRUE;CHECK(MotoG20UsbPlatformInit(&base)==EFI_TIMEOUT);CHECK(bqOn==0 && efuseProgram==0);CHECK(!(pmic[0x1808/4]&BIT6));puts("PASS eFuse timeout never programs or enables boost, restores enable bit");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);adiBadAddress=TRUE;CHECK(MotoG20UsbPlatformInit(&base)==EFI_DEVICE_ERROR && bqOn==0);puts("PASS ADI address mismatch fails closed");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);nackOn=TRUE;CHECK(MotoG20UsbPlatformInit(&base)==EFI_NO_RESPONSE);CHECK(bqOn==1 && bqOff==1 && !(pmic[BC12/4]&1));puts("PASS uncertain boost write always attempts OFF");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);CHECK(MotoG20UsbPlatformInit(&base)==0);CHECK(MotoG20UsbPlatformShutdown()==0);CHECK(mTimer==NULL && !mPrepared && mSavedCount==0 && !mRoleSaved);CHECK(MotoG20UsbPlatformPollRole(&active)==EFI_NOT_READY && !active);puts("PASS shutdown restores role and closes monitor");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);CHECK(MotoG20UsbPlatformInit(&base)==0);
 failWriteAddress=AON+0x208;
 CHECK(MotoG20UsbPlatformStop()==EFI_DEVICE_ERROR && mFault && mSavedCount>2);
 CHECK((cell(AON+0x208)->Val&BIT30)!=0);
 CHECK((cell(AON+4)->Val&(BIT8|BIT12))==(BIT8|BIT12) && pmic[VDD_PD/4]==0 && mVddSaved);
 CHECK(MotoG20UsbPlatformShutdown()==EFI_DEVICE_ERROR && mPrepared && mTimer!=NULL);
 failWriteAddress=0;CHECK(MotoG20UsbPlatformShutdown()==0);
 CHECK(cell(AON+0x208)->Val==0 && mSavedCount==0 && !mPrepared && mTimer==NULL);
 puts("PASS failed PHY rollback is retained, repeated shutdown fails until restoration succeeds");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);CHECK(MotoG20UsbPlatformInit(&base)==0);
 failWriteAddress=AON+8;
 CHECK(MotoG20UsbPlatformStop()==EFI_DEVICE_ERROR && mFault && mSavedCount==1);
 CHECK(MotoG20UsbPlatformShutdown()==EFI_DEVICE_ERROR && mTimer!=NULL);
 failWriteAddress=0;CHECK(MotoG20UsbPlatformShutdown()==0 && mSavedCount==0 && cell(AON+8)->Val==0);
 puts("PASS failed infrastructure rollback remains owned through shutdown retries");
 reset();boostSense=FALSE;CHECK(MotoG20UsbPlatformPrepare()==0);
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_NO_RESPONSE && mFault && bqOn==1 && bqOff==1);
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_NO_RESPONSE && bqOn==1 && base==0);
 boostSense=TRUE;CHECK(MotoG20UsbPlatformInit(&base)==EFI_NO_RESPONSE && bqOn==1);
 puts("PASS absent source voltage latches fault and cannot repeatedly pulse boost");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);CHECK(MotoG20UsbPlatformInit(&base)==0);
 pmic[EIC_DATA/4]=0;CHECK(MotoG20UsbPlatformPollRole(&active)==EFI_NO_RESPONSE && !active && mFault);
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_NO_RESPONSE && bqOn==1);
 puts("PASS source voltage loss while still attached latches fault");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);CHECK(MotoG20UsbPlatformInit(&base)==0);
 timeNs+=15000000000ULL;nackFeed=TRUE;
 CHECK(MotoG20UsbPlatformPollRole(&active)==EFI_NO_RESPONSE && !active && mFault && bqOff==1);
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_NO_RESPONSE && bqOn==1);
 puts("PASS failed watchdog feed shuts down and cannot restart boost");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);pmic[TYPEC_STATUS/4]=2;pmic[EIC_DATA/4]=1;
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_NOT_READY && !mFault && bqOn==0);
 pmic[TYPEC_STATUS/4]=5;pmic[EIC_DATA/4]=0;
 CHECK(MotoG20UsbPlatformInit(&base)==0 && bqOn==1);
 pmic[TYPEC_STATUS/4]=3;CHECK(MotoG20UsbPlatformPollRole(&active)==EFI_NOT_READY && !active && !mFault && bqOff==1);
 pmic[TYPEC_STATUS/4]=5;CHECK(MotoG20UsbPlatformInit(&base)==0 && bqOn==2);
 CHECK(MotoG20UsbPlatformShutdown()==0);
 puts("PASS PC to OTG and clean detach/reattach remain retryable");
 reset();CHECK(MotoG20UsbPlatformGetRoleDiagnostic(NULL)==EFI_INVALID_PARAMETER);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && mmioCount==0);
 CHECK(d.Revision==MOTO_G20_USB_ROLE_DIAGNOSTIC_REVISION && d.Size==sizeof(d));
 CHECK(d.ValidMask==0 && d.Samples==0 && d.SnapshotStatus==EFI_NOT_READY && d.RoleReadStatus==EFI_NOT_READY && d.CalibrationStatus==EFI_NOT_READY);
 CHECK(!d.EfuseValid && !d.TrimApplied);
 puts("PASS role getter before prepare is cache-only and explicitly uninitialized");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);pmic[TYPEC_STATUS/4]=2;pmic[EIC_DATA/4]=1;
 pmic[0x380/4]=0x10;pmic[0x394/4]=3;pmic[0x398/4]=1;
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_NOT_READY);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0);
 CHECK(d.Samples==1 && d.SeenStates==(1U<<2) && d.SourceSeen==0 && d.BoostAttempts==0);
 CHECK(d.SnapshotAttempts==1 && d.SnapshotFailures==0 && d.ValidMask==0x7f && d.ErrorMask==0);
 CHECK(d.En==0x10 && d.Mode==2 && d.IntEn==3 && d.IntRaw==3 && d.IntMask==1 && d.TypecStatus==2);
 CHECK(d.EfuseValid && d.EfuseData==0x155 && d.RequestedTrim==0x2aa && d.TrimApplied && d.Trim==0x2aa && d.CalibrationStatus==0);
 previousMmio=mmioCount;CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && mmioCount==previousMmio && currentTpl==TPL_APPLICATION);
 reads=adiReadCounts[0x394/4];for(i=0;i<100;i++) CHECK(MotoG20UsbPlatformInit(&base)==EFI_NOT_READY);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && d.Samples==101 && d.SnapshotAttempts==1 && adiReadCounts[0x394/4]==reads);
 CHECK(pmicWriteCounts[0x390/4]==0 && pmicWriteCounts[0x394/4]==0 && pmicWriteCounts[0x398/4]==0);
 puts("PASS calibrated register snapshot, truthful counters, cached getter and bounded steady-state cost");
 pmic[TYPEC_STATUS/4]=9;pmic[EIC_DATA/4]=0;CHECK(MotoG20UsbPlatformInit(&base)==EFI_NOT_READY);
 pmic[TYPEC_STATUS/4]=3;CHECK(MotoG20UsbPlatformInit(&base)==EFI_NOT_READY);
 pmic[TYPEC_STATUS/4]=0;CHECK(MotoG20UsbPlatformInit(&base)==EFI_NOT_READY);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && d.SeenStates==((1U<<2)|(1U<<9)|(1U<<3)|1U) && d.SnapshotAttempts==4 && d.SourceSeen==0 && d.BoostAttempts==0);
 timeNs+=600000000000ULL;pmic[TYPEC_STATUS/4]=5;
 CHECK(MotoG20UsbPlatformInit(&base)==0 && !mFault);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && d.SourceSeen>0 && d.BoostAttempts==1 && (d.SeenStates&BIT5)!=0);
 CHECK(pmicWriteCounts[0x390/4]==0);
 puts("PASS detached DRP state history and delayed host attachment remain distinguishable");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);pmic[TYPEC_STATUS/4]=2;pmic[EIC_DATA/4]=1;
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_NOT_READY);adiFailReg=0x394;pmic[TYPEC_STATUS/4]=9;pmic[EIC_DATA/4]=0;
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_NOT_READY && !mFault);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0);
 CHECK(d.RoleReadStatus==0 && d.SnapshotStatus==EFI_DEVICE_ERROR && d.SnapshotFailures==1 && d.ErrorMask==BIT3 && d.ValidMask==(0x7fU&~BIT3));
 CHECK(d.IntRaw==0 && d.TypecStatus==9 && d.SnapshotAttempts==2);
 reads=adiReadCounts[0x394/4];for(i=0;i<50;i++) CHECK(MotoG20UsbPlatformInit(&base)==EFI_NOT_READY);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && adiReadCounts[0x394/4]==reads && d.SnapshotFailures==1);
 adiFailReg=0xffff;pmic[TYPEC_STATUS/4]=3;CHECK(MotoG20UsbPlatformInit(&base)==EFI_NOT_READY);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && d.ValidMask==0x7f && d.ErrorMask==0 && d.SnapshotStatus==0 && d.SnapshotFailures==1);
 puts("PASS failed snapshot fields invalidated explicitly and failures not retried on every poll");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);pmic[TYPEC_STATUS/4]=2;pmic[EIC_DATA/4]=1;
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_NOT_READY);adiFailReg=TYPEC_STATUS;
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_DEVICE_ERROR && mFault);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && d.RoleReadStatus==EFI_DEVICE_ERROR && d.ErrorMask==BIT5 && d.SeenStates==(1U<<2) && d.Samples==2);
 previousMmio=mmioCount;CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && mmioCount==previousMmio);
 puts("PASS failed role reads counted without inventing state zero or MMIO from getters");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);efuseTimeout=TRUE;
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_TIMEOUT);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && d.CalibrationStatus==EFI_TIMEOUT && !d.EfuseValid && !d.TrimApplied && d.Samples==0 && d.BoostAttempts==0 && d.SnapshotAttempts==1);
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);nackOn=TRUE;
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_NO_RESPONSE);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && d.BoostAttempts==1 && d.SourceSeen>0);
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_NO_RESPONSE);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && d.BoostAttempts==1);
 puts("PASS calibration failure and uncertain boost attempts reported without false success or retries");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);pmic[0x304/4]=0x31ee;
 pmic[TYPEC_STATUS/4]=2;pmic[EIC_DATA/4]=1;pmic[TYPEC_TRIM/4]=0x8000;
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_NOT_READY);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && d.EfuseValid && d.EfuseData==0x31ee);
 CHECK(d.RequestedTrim==0x01cf && d.TrimApplied && d.CalibrationStatus==0 && d.Trim==0x81cf);
 CHECK(d.BoostAttempts==0);
 puts("PASS observed Android calibration 0x31EE decodes to 0x01CF without changing unrelated trim bits");
 reset();typec=0xaaaa;vbus=TRUE;
 CHECK(MotoG20UsbPlatformDetectRole(NULL,&vbus)==EFI_INVALID_PARAMETER);
 CHECK(MotoG20UsbPlatformDetectRole(&typec,NULL)==EFI_INVALID_PARAMETER);
 CHECK(MotoG20UsbPlatformDetectRole(&typec,&vbus)==EFI_NOT_READY && typec==0xaaaa && vbus);
 CHECK(MotoG20UsbPlatformQuiesceForRetry()==EFI_NOT_READY && mmioCount==0);
 CHECK(MotoG20UsbPlatformPrepare()==0);
 CHECK(MotoG20UsbPlatformDetectRole(&typec,&vbus)==EFI_NOT_READY && mmioCount==0 && !mRoleReady);
 CHECK(MotoG20UsbPlatformInit(&base)==0);previousMmio=mmioCount;
 CHECK(MotoG20UsbPlatformDetectRole(&typec,&vbus)==EFI_NOT_READY && mmioCount==previousMmio);
 CHECK(typec==0xaaaa && vbus && currentTpl==TPL_APPLICATION);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && d.Revision==2 && d.DriverActive && d.BoostRequested && d.StopAttempts==0 && d.LastStopStatus==EFI_NOT_READY);
 puts("PASS role detection requires inactive prepared detector; rejected calls preserve outputs and never touch MMIO");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);
 pmic[TYPEC_MODE/4]=0x2085;pmic[TYPEC_INTEN/4]=0x1000;pmic[TYPEC_TRIM/4]=0x8000;
 CHECK(MotoG20UsbPlatformInit(&base)==0 && timerActive);
 CHECK(MotoG20UsbPlatformQuiesceForRetry()==0);
 CHECK(!mActive && !mBoostAttempted && !mI2cReady && !timerActive && !mFault);
 CHECK(mRoleReady && mRoleSaved && mInfra && mSavedCount==2 && bqOn==1 && bqOff==1);
 CHECK(pmic[TYPEC_MODE/4]==0x2086 && pmic[TYPEC_INTEN/4]==0x1003 && pmic[TYPEC_TRIM/4]==0x82aa && pmic[EIC_MASK/4]==1);
 CHECK(mStage==MotoUsbStageStopped && mLastStatus==EFI_NOT_READY);
 previousMmio=mmioCount;CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && mmioCount==previousMmio);
 CHECK(!d.DriverActive && !d.BoostRequested && d.StopAttempts==1 && d.LastStopStatus==0 && d.TrimApplied);
 CHECK(mVbusPresent); // Historical sample: do not fabricate a new EIC=0 after OFF.
 writesBefore=0;for(j=0;j<ARRAY_SIZE(pmicWriteCounts);j++) writesBefore+=pmicWriteCounts[j];
 for(i=0;i<100;i++) {
   static const UINT16 states[]={5,9,3,0,2};
   pmic[TYPEC_STATUS/4]=states[i%5];pmic[EIC_DATA/4]=(UINT16)(i&1);
   CHECK(MotoG20UsbPlatformDetectRole(&typec,&vbus)==0 && typec==states[i%5] && vbus==(BOOLEAN)(i&1));
 }
 timeNs+=30000000000ULL;Monitor(NULL,NULL);
 writesAfter=0;for(j=0;j<ARRAY_SIZE(pmicWriteCounts);j++) writesAfter+=pmicWriteCounts[j];
 CHECK(writesAfter==writesBefore);
 CHECK(bqOn==1 && bqOff==1 && feed==1 && hcMmioCount==0 && pmicWriteCounts[0x390/4]==0);
 CHECK(MotoG20UsbPlatformStop()==0 && mSavedCount==0 && !mRoleSaved && !mRoleReady && !mInfra);
 CHECK(pmic[TYPEC_MODE/4]==0x2085 && pmic[TYPEC_INTEN/4]==0x1000 && pmic[TYPEC_TRIM/4]==0x8000 && pmic[EIC_MASK/4]==0);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && d.TrimApplied && d.StopAttempts==2 && d.LastStopStatus==0 && !d.DriverActive && !d.BoostRequested);
 CHECK(mStage==MotoUsbStageStopped && mLastStatus==EFI_NOT_READY);
 puts("PASS quiesce preserves detector, fresh samples never rearm, final stop restores originals and trim history stays historical");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);CHECK(MotoG20UsbPlatformInit(&base)==0);
 CHECK(MotoG20UsbPlatformQuiesceForRetry()==0 && MotoG20UsbPlatformQuiesceForRetry()==0 && bqOff==1);
 pmic[TYPEC_STATUS/4]=3;CHECK(MotoG20UsbPlatformDetectRole(&typec,&vbus)==0 && typec==3 && !vbus && bqOn==1);
 pmic[TYPEC_STATUS/4]=5;CHECK(MotoG20UsbPlatformDetectRole(&typec,&vbus)==0 && typec==5 && !vbus && bqOn==1);
 CHECK(MotoG20UsbPlatformInit(&base)==0 && bqOn==2 && timerActive && mSavedCount>2);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && d.DriverActive && d.BoostRequested && d.StopAttempts==2);
 CHECK(MotoG20UsbPlatformShutdown()==0 && mSavedCount==0);
 puts("PASS quiesce is idempotent; only explicit Init rearms after successful detector observations");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);CHECK(MotoG20UsbPlatformInit(&base)==0);nackOff=TRUE;
 CHECK(MotoG20UsbPlatformQuiesceForRetry()==EFI_NO_RESPONSE && mFault && !mActive && mBoostAttempted);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && !d.DriverActive && d.BoostRequested && d.LastStopStatus==EFI_NO_RESPONSE && d.StopAttempts==1);
 typec=0xaaaa;vbus=TRUE;previousMmio=mmioCount;
 CHECK(MotoG20UsbPlatformDetectRole(&typec,&vbus)==EFI_NO_RESPONSE && typec==0xaaaa && vbus && mmioCount==previousMmio);
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_NO_RESPONSE && bqOn==1);
 nackOff=FALSE;CHECK(MotoG20UsbPlatformQuiesceForRetry()==EFI_NO_RESPONSE && mFault && !mBoostAttempted);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && d.LastStopStatus==0 && !d.BoostRequested && d.StopAttempts==2);
 CHECK(mStage==MotoUsbStageRollback && mLastStatus==EFI_NO_RESPONSE);
 CHECK(MotoG20UsbPlatformStop()==0 && mFault && mStage==MotoUsbStageRollback && mLastStatus==EFI_NO_RESPONSE);
 puts("PASS uncertain OFF remains requested; cleanup retry never clears the original fault or permits rearm");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);CHECK(MotoG20UsbPlatformInit(&base)==0);failWriteAddress=AON+0x208;
 CHECK(MotoG20UsbPlatformQuiesceForRetry()==EFI_DEVICE_ERROR && mFault && mSavedCount>2 && bqOff==1);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && d.LastStopStatus==EFI_DEVICE_ERROR && !d.DriverActive && !d.BoostRequested);
 previousMmio=mmioCount;CHECK(MotoG20UsbPlatformDetectRole(&typec,&vbus)==EFI_DEVICE_ERROR && mmioCount==previousMmio);
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_DEVICE_ERROR && bqOn==1);
 failWriteAddress=0;CHECK(MotoG20UsbPlatformShutdown()==0 && mSavedCount==0 && mFault);
 puts("PASS failed PHY rollback preserves ownership and forbids role polling/rearm until final cleanup");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);CHECK(MotoG20UsbPlatformInit(&base)==0);failTimerCancel=TRUE;
 CHECK(MotoG20UsbPlatformQuiesceForRetry()==EFI_DEVICE_ERROR && mFault && bqOff==1 && !mActive);
 previousMmio=mmioCount;Monitor(NULL,NULL);CHECK(mmioCount==previousMmio && bqOn==1);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && d.LastStopStatus==EFI_DEVICE_ERROR && !d.BoostRequested);
 failTimerCancel=FALSE;CHECK(MotoG20UsbPlatformShutdown()==0 && !timerActive);
 puts("PASS timer cancellation failure propagates while best-effort OFF proceeds; late monitor cannot rearm");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);CHECK(MotoG20UsbPlatformInit(&base)==0);CHECK(MotoG20UsbPlatformQuiesceForRetry()==0);
 typec=0xaaaa;vbus=TRUE;pmic[TYPEC_STATUS/4]=3;adiFailReg=EIC_DATA;
 CHECK(MotoG20UsbPlatformDetectRole(&typec,&vbus)==EFI_DEVICE_ERROR && typec==0xaaaa && vbus && mFault);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && d.RoleReadStatus==EFI_DEVICE_ERROR && !d.DriverActive);
 previousMmio=mmioCount;CHECK(MotoG20UsbPlatformDetectRole(&typec,&vbus)==EFI_DEVICE_ERROR && mmioCount==previousMmio);
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_DEVICE_ERROR && bqOn==1);
 puts("PASS partial fresh role-read failure preserves both outputs and latches against new source power");
 reset();boostSense=FALSE;CHECK(MotoG20UsbPlatformPrepare()==0);
 CHECK(MotoG20UsbPlatformInit(&base)==EFI_NO_RESPONSE && mStage==MotoUsbStageVbusSense);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && d.LastStopStatus==0 && d.StopAttempts==1 && !d.DriverActive && !d.BoostRequested);
 CHECK(MotoG20UsbPlatformStop()==0 && mStage==MotoUsbStageVbusSense && mLastStatus==EFI_NO_RESPONSE && mFault);
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);CHECK(MotoG20UsbPlatformInit(&base)==0);
 adiFailReg=TYPEC_MODE;CHECK(MotoG20UsbPlatformStop()==EFI_DEVICE_ERROR);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && d.LastStopStatus==EFI_DEVICE_ERROR && d.StopAttempts==1);
 CHECK(mStage==MotoUsbStageRollback && mFault);
 puts("PASS automatic cleanup preserves primary VBUS failure; full Stop reports later detector-restoration failure");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);CHECK(MotoG20UsbPlatformInit(&base)==0);
 CHECK(MotoG20UsbPlatformQuiesceForRetry()==0);CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0);
 reads=d.Samples;i=d.SeenStates;typec=0xaaaa;vbus=TRUE;adiFailReg=TYPEC_STATUS;
 CHECK(MotoG20UsbPlatformDetectRole(&typec,&vbus)==EFI_DEVICE_ERROR && typec==0xaaaa && vbus);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && d.Samples==reads+1 && d.SeenStates==i);
 CHECK(d.RoleReadStatus==EFI_DEVICE_ERROR && (d.ValidMask&BIT5)==0 && (d.ErrorMask&BIT5)!=0);
 CHECK(mFault && bqOn==1 && bqOff==1 && !d.DriverActive && !d.BoostRequested && hcMmioCount==0);
 puts("PASS failed first fresh STATUS read preserves outputs and state history without HC or power writes");
 for(i=0;i<3;i++) {
   static const EFI_TPL levels[]={TPL_APPLICATION,TPL_CALLBACK,TPL_NOTIFY};
   reset();CHECK(MotoG20UsbPlatformPrepare()==0);CHECK(MotoG20UsbPlatformInit(&base)==0);
   currentTpl=levels[i];CHECK(MotoG20UsbPlatformQuiesceForRetry()==0 && currentTpl==levels[i]);
   pmic[TYPEC_STATUS/4]=3;
   CHECK(MotoG20UsbPlatformDetectRole(&typec,&vbus)==0 && currentTpl==levels[i] && typec==3 && !vbus);
   CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && currentTpl==levels[i]);
   adiFailReg=EIC_DATA;typec=0xaaaa;vbus=TRUE;
   CHECK(MotoG20UsbPlatformDetectRole(&typec,&vbus)==EFI_DEVICE_ERROR && currentTpl==levels[i] && typec==0xaaaa && vbus);
   CHECK(MotoG20UsbPlatformQuiesceForRetry()==EFI_DEVICE_ERROR && currentTpl==levels[i]);
   CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && currentTpl==levels[i]);
 }
 puts("PASS quiesce, fresh detection and cached diagnostics restore caller TPL on success and fault");
 reset();CHECK(MotoG20UsbPlatformPrepare()==0);CHECK(MotoG20UsbPlatformInit(&base)==0);
 CHECK(MotoG20UsbPlatformQuiesceForRetry()==0);
 // Simulate an unresolved ownership flag independently of the fault guard.
 // This is a guard test, not a supported state from a successful shutdown.
 mBoostAttempted=TRUE;typec=0xaaaa;vbus=TRUE;previousMmio=mmioCount;
 CHECK(MotoG20UsbPlatformDetectRole(&typec,&vbus)==EFI_NOT_READY);
 CHECK(typec==0xaaaa && vbus && mmioCount==previousMmio && bqOn==1 && bqOff==1);
 CHECK(MotoG20UsbPlatformGetRoleDiagnostic(&d)==0 && !d.DriverActive && d.BoostRequested);
 puts("PASS unresolved source ownership independently blocks inactive detection with no MMIO");
 return 0;
}
