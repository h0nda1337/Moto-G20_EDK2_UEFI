/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Guid/FdtHob.h>
#include <IndustryStandard/SmBios.h>
#include <Protocol/AcpiTable.h>
#include <Protocol/Smbios.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "../memory/memory_plan.h"
#include "MotoG20BootPlatformDiagnostic.h"
#include "MotoG20AcpiTables.generated.h"

STATIC MOTO_G20_BOOT_PLATFORM_DIAGNOSTIC mDiag;
STATIC EFI_GUID mDiagGuid=MOTO_G20_BOOT_PLATFORM_DIAGNOSTIC_GUID;
STATIC EFI_HANDLE mHandle;
STATIC UINTN mKeys[4];
STATIC CONST VOID *mFdt;
EFI_STATUS EFIAPI MotoG20PrepareWindowsCpu(VOID);

STATIC BOOLEAN Cells (CONST CHAR8 *Path, CONST CHAR8 *Name, CONST UINT32 *Expected, UINTN Count) {
  INT32 Node, Len;
  CONST UINT32 *Value;
  UINTN I;
  Node=FdtPathOffset(mFdt,Path); if(Node<0) return FALSE;
  Value=FdtGetProp(mFdt,Node,Name,&Len);
  if(Value==NULL || Len!=(INT32)(Count*sizeof(UINT32))) return FALSE;
  for(I=0;I<Count;++I) if(Fdt32ToCpu(Value[I])!=Expected[I]) return FALSE;
  return TRUE;
}
STATIC BOOLEAN String (CONST CHAR8 *Path, CONST CHAR8 *Name, CONST CHAR8 *Expected) {
  INT32 Node,Len; CONST CHAR8 *Value; UINTN Size;
  Node=FdtPathOffset(mFdt,Path); if(Node<0) return FALSE;
  Value=FdtGetProp(mFdt,Node,Name,&Len);Size=AsciiStrSize(Expected);
  return Value!=NULL && Len==(INT32)Size && CompareMem(Value,Expected,Size)==0;
}
STATIC EFI_STATUS ValidateBoard(VOID) {
  EFI_HOB_GUID_TYPE *Hob; UINT64 Address; UINT32 Total,I,Count=0;
  INT32 Node,Cpus,Len; CONST CHAR8 *Type;
  CONST UINT8 *Fdt; struct mp_plan Plan;
  CONST UINT32 Gic[]={0,0x12000000,0,0x20000,0,0x12040000,0,0x100000};
  CONST UINT32 Stride[]={0,0x20000};
  CONST UINT32 Timer[]={1,13,4,1,14,4,1,11,4,1,10,4};
  CONST UINT32 Maintenance[]={1,9,4};
  CONST UINT32 Pmu[]={0,0x70,4,0,0x71,4,0,0x72,4,0,0x73,4,0,0x74,4,0,0x75,4,0,0x76,4,0,0x77,4};
  UINT32 Reg[2]={0,0};
  CHAR8 Path[]="/cpus/cpu@000";
  Hob=GetFirstGuidHob(&gFdtHobGuid);
  if(Hob==NULL || GET_GUID_HOB_DATA_SIZE(Hob)!=8) return EFI_NOT_FOUND;
  CopyMem(&Address,GET_GUID_HOB_DATA(Hob),8);
  if(Address!=0x82000000ULL) return EFI_SECURITY_VIOLATION;
  Fdt=(CONST UINT8*)(UINTN)Address;
  Total=((UINT32)Fdt[4]<<24)|((UINT32)Fdt[5]<<16)|((UINT32)Fdt[6]<<8)|Fdt[7];
  if(mp_make_plan(Fdt,Total,&Plan)!=MP_OK) return EFI_COMPROMISED_DATA;
  mFdt=Fdt;
  if(!String("/interrupt-controller@12000000","compatible","arm,gic-v3") ||
     !Cells("/interrupt-controller@12000000","reg",Gic,ARRAY_SIZE(Gic)) ||
     !Cells("/interrupt-controller@12000000","redistributor-stride",Stride,2) ||
     !Cells("/interrupt-controller@12000000","interrupts",Maintenance,3) ||
     !Cells("/timer","interrupts",Timer,ARRAY_SIZE(Timer)) ||
     !String("/timer","compatible","arm,armv8-timer") ||
     !Cells("/pmu","interrupts",Pmu,ARRAY_SIZE(Pmu)) ||
     !String("/psci","compatible","arm,psci-0.2") || !String("/psci","method","smc")) return EFI_UNSUPPORTED;
  for(I=0;I<8;++I) {
    if(I==0) { Path[10]='0';Path[11]=0; }
    else { Path[10]=(CHAR8)('0'+I);Path[11]='0';Path[12]='0';Path[13]=0; }
    Reg[1]=I*0x100;
    if(!Cells(Path,"reg",Reg,2) || !String(Path,"enable-method","psci")) return EFI_UNSUPPORTED;
  }
  Cpus=FdtPathOffset(mFdt,"/cpus");
  for(Node=FdtFirstSubnode(mFdt,Cpus);Node>=0;Node=FdtNextSubnode(mFdt,Node)) {
    Type=FdtGetProp(mFdt,Node,"device_type",&Len);
    if(Type!=NULL && Len==4 && CompareMem(Type,"cpu",4)==0) ++Count;
  }
  if(Count!=8 || Node!=-FDT_ERR_NOTFOUND) return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI RefreshBootState(MOTO_G20_BOOT_PLATFORM_DIAGNOSTIC *This) {
  UINT32 Issues=0,I; ARM_SMC_ARGS Args;
  if(This!=&mDiag) return EFI_INVALID_PARAMETER;
  __asm__ volatile("mrs %0, CurrentEL":"=r"(mDiag.CurrentEl));
  if(mDiag.CurrentEl!=8) { mDiag.BootStateIssues=MOTO_BOOT_EL_INVALID;return EFI_UNSUPPORTED; }
  __asm__ volatile("mrs %0, SCTLR_EL2":"=r"(mDiag.Sctlr));
  __asm__ volatile("mrs %0, TCR_EL2":"=r"(mDiag.Tcr));
  __asm__ volatile("mrs %0, CPTR_EL2":"=r"(mDiag.Cptr));
  __asm__ volatile("mrs %0, CPACR_EL1":"=r"(mDiag.Cpacr));
  __asm__ volatile("mrs %0, CNTHCTL_EL2":"=r"(mDiag.Cnthctl));
  __asm__ volatile("mrs %0, CNTFRQ_EL0":"=r"(mDiag.Cntfrq));
  __asm__ volatile("mrs %0, HCR_EL2":"=r"(mDiag.Hcr));
  __asm__ volatile("mrs %0, MPIDR_EL1":"=r"(mDiag.Mpidr));
  __asm__ volatile("mrs %0, DAIF":"=r"(mDiag.Daif));
  if((mDiag.Sctlr&(BIT0|BIT1|BIT2|BIT3|BIT12|BIT25))!=(BIT0|BIT2|BIT3|BIT12)) Issues|=MOTO_BOOT_SCTLR_INVALID;
  if((mDiag.Tcr&(BIT14|BIT15|BIT20))!=0 || (mDiag.Tcr&63)>31) Issues|=MOTO_BOOT_TCR_INVALID;
  if((mDiag.Cptr&BIT10)!=0 || (mDiag.Cpacr&(BIT20|BIT21))!=(BIT20|BIT21)) Issues|=MOTO_BOOT_FP_TRAPS;
  if((mDiag.Cnthctl&3)!=3) Issues|=MOTO_BOOT_TIMER_ACCESS;
  if(mDiag.Cntfrq!=26000000) Issues|=MOTO_BOOT_FREQ_INVALID;
  if((mDiag.Hcr&BIT0)!=0) Issues|=MOTO_BOOT_STAGE2_ACTIVE;
  if((mDiag.Daif&BIT7)!=0) Issues|=MOTO_BOOT_IRQ_MASKED;
  if((mDiag.Cptr&BIT10)==0) {
    __asm__ volatile("mrs %0, FPCR":"=r"(mDiag.Fpcr));
    if(mDiag.Fpcr!=0) Issues|=MOTO_BOOT_FPCR_INVALID;
  }
  for(I=1;I<8;++I) {
    ZeroMem(&Args,sizeof(Args));Args.Arg0=0xc4000004;Args.Arg1=I*0x100;
    ArmCallSmc(&Args);mDiag.SecondaryState[I-1]=(INT64)Args.Arg0;
    if(Args.Arg0!=1) Issues|=MOTO_BOOT_SECONDARY_ACTIVE;
  }
  mDiag.BootStateIssues=Issues;
  return Issues?EFI_NOT_READY:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI PrepareWindowsCpu(MOTO_G20_BOOT_PLATFORM_DIAGNOSTIC *This) {
  EFI_STATUS Status;
  if(This!=&mDiag) return EFI_INVALID_PARAMETER;
  if(EFI_ERROR(mDiag.Status) || mDiag.InstalledTables!=4 || mDiag.SmbiosRecords!=1) return EFI_NOT_READY;
  Status=MotoG20PrepareWindowsCpu();
  if(EFI_ERROR(Status)) return Status;
  return RefreshBootState(This);
}

STATIC EFI_STATUS InstallSystemIdentity(EFI_SMBIOS_PROTOCOL *Smbios) {
  struct { SMBIOS_TABLE_TYPE1 Info; CHAR8 Strings[64]; } Record;
  CONST CHAR8 Strings[]="motorola\0Moto G20\0XT2128-1\0UMS512\0";
  EFI_SMBIOS_HANDLE Handle=SMBIOS_HANDLE_PI_RESERVED;
  EFI_SMBIOS_TYPE Type=SMBIOS_TYPE_SYSTEM_INFORMATION;
  EFI_SMBIOS_TABLE_HEADER *Existing;
  EFI_STATUS Status;
  /* This is the actual SKU captured by the board DT, not a substitute PC ID.
   * UUID and serial are unspecified instead of inventing identities.
   */
  if(!String("/firmware/android","hardware.sku","XT2128-1")) return EFI_UNSUPPORTED;
  Status=Smbios->GetNext(Smbios,&Handle,&Type,&Existing,NULL);
  if(!EFI_ERROR(Status)) return EFI_ALREADY_STARTED;
  if(Status!=EFI_NOT_FOUND) return Status;
  Handle=SMBIOS_HANDLE_PI_RESERVED;
  ZeroMem(&Record,sizeof(Record));Record.Info.Hdr.Type=SMBIOS_TYPE_SYSTEM_INFORMATION;
  Record.Info.Hdr.Length=sizeof(SMBIOS_TABLE_TYPE1);
  Record.Info.Manufacturer=1;Record.Info.ProductName=2;Record.Info.SKUNumber=3;
  Record.Info.Family=4;Record.Info.WakeUpType=SystemWakeupTypeUnknown;
  CopyMem(Record.Strings,Strings,sizeof(Strings));
  return Smbios->Add(Smbios,NULL,&Handle,(EFI_SMBIOS_TABLE_HEADER*)&Record);
}

STATIC EFI_STATUS InstallTables(VOID) {
  EFI_ACPI_TABLE_PROTOCOL *Acpi;EFI_SMBIOS_PROTOCOL *Smbios; EFI_STATUS Status,Undo;
  ARM_SMC_ARGS Args;UINT32 I;UINT64 Mpidr;
  CONST VOID *Tables[]={mDsdtTemplate,mFadtTemplate,mMadtTemplate,mGtdtTemplate};
  CONST UINTN Sizes[]={sizeof(mDsdtTemplate),sizeof(mFadtTemplate),sizeof(mMadtTemplate),sizeof(mGtdtTemplate)};
  mDiag.Stage=1;Status=ValidateBoard();if(EFI_ERROR(Status))return Status;
  __asm__ volatile("mrs %0, MPIDR_EL1":"=r"(Mpidr));
  /* MADT's first GICC describes boot CPU0. Refuse a different boot CPU rather
   * than silently publishing a wrong first entry.
   */
  if((Mpidr&0xff00ffffffULL)!=0) return EFI_UNSUPPORTED;
  ZeroMem(&Args,sizeof(Args));Args.Arg0=0x84000000;ArmCallSmc(&Args);
  mDiag.PsciVersion=(UINT32)Args.Arg0;
  if((INT32)Args.Arg0<0 || (UINT32)Args.Arg0<2) return EFI_UNSUPPORTED;
  (VOID)RefreshBootState(&mDiag);
  Status=gBS->LocateProtocol(&gEfiAcpiTableProtocolGuid,NULL,(VOID**)&Acpi);if(EFI_ERROR(Status))return Status;
  Status=gBS->LocateProtocol(&gEfiSmbiosProtocolGuid,NULL,(VOID**)&Smbios);if(EFI_ERROR(Status))return Status;
  mDiag.Stage=2;
  for(I=0;I<4;++I) {
    Status=Acpi->InstallAcpiTable(Acpi,(VOID*)Tables[I],Sizes[I],&mKeys[I]);
    if(EFI_ERROR(Status)) {
      while(I>0) {
        --I; Undo=Acpi->UninstallAcpiTable(Acpi,mKeys[I]);
        if(EFI_ERROR(Undo)) mDiag.RollbackStatus=Undo;
        else mDiag.InstalledTables--;
      }
      return Status;
    }
    mDiag.InstalledTables++;
  }
  mDiag.Stage=3;Status=InstallSystemIdentity(Smbios);if(EFI_ERROR(Status))return Status;
  mDiag.SmbiosRecords=1;mDiag.Stage=4;return EFI_SUCCESS;
}
EFI_STATUS EFIAPI MotoG20BootPlatformEntry(EFI_HANDLE ImageHandle,EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS Status;(VOID)ImageHandle;(VOID)SystemTable;
  ZeroMem(&mDiag,sizeof(mDiag));mDiag.Revision=1;mDiag.Status=EFI_NOT_READY;mDiag.RefreshBootState=RefreshBootState;
  mDiag.PrepareWindowsCpu=PrepareWindowsCpu;
  Status=gBS->InstallProtocolInterface(&mHandle,&mDiagGuid,EFI_NATIVE_INTERFACE,&mDiag);if(EFI_ERROR(Status))return Status;
  mDiag.Status=InstallTables();return EFI_SUCCESS;
}
