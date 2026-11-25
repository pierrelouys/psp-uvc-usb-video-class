	.set noreorder

#include "pspimport.s"

IMPORT_START	"LoadCoreForKernel",0x00090000
IMPORT_FUNC	"LoadCoreForKernel",0xCF8A41B1,sceKernelFindModuleByName
