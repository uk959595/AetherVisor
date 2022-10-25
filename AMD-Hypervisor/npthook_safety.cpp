#include "npthook_safety.h"
#include "portable_executable.h"
#include "vmexit.h"

namespace NptHooks
{
	Hooks::JmpRipCode hk_MmCleanProcessAddressSpace;

	char __fastcall MmCleanProcessAddressSpace_hook(__int64 a1, __int64 a2)
	{
		/*	unset all NPT hooks for this process	*/

		ForEachHook(
			[](NptHook* hook_entry, void* data)-> bool {

				if (hook_entry->process_cr3 == (uintptr_t)data)
				{
					UnsetHook(hook_entry);
				}

				return false;
			},
			(void*)__readcr3()
		);

		return static_cast<decltype(&MmCleanProcessAddressSpace_hook)>(hk_MmCleanProcessAddressSpace.original_bytes)(a1, a2);
	}

	void PageSynchronizationPatch()
	{
		/*	place a callback on NtTerminateProcess to remove npt hooks inside terminating processes, to prevent PFN check bsods	*/

		ULONG nt_size = NULL;
		auto ntoskrnl = (uintptr_t)Utils::GetKernelModule(&nt_size, RTL_CONSTANT_STRING(L"ntoskrnl.exe"));

		auto pe_hdr = PeHeader(ntoskrnl);

		auto section = (IMAGE_SECTION_HEADER*)(pe_hdr + 1);

		for (int i = 0; i < pe_hdr->FileHeader.NumberOfSections; ++i)
		{
			/*	MmCleanProcessAddressSpace hook to clean up NPT hooks after process exit	*/

			if (!strcmp((char*)section[i].Name, "PAGE"))
			{
				uint8_t* start = section[i].VirtualAddress + (uint8_t*)ntoskrnl;

				uintptr_t clean_process_address_space = NULL;

				clean_process_address_space = RELATIVE_ADDR(Utils::FindPattern((uintptr_t)start, section[i].Misc.VirtualSize, "\xE8\x00\x00\x00\x00\x33\xD2\x48\x8D\x4C\x24\x00\xE8\x00\x00\x00\x00\x4C\x39\xBE", 20, 0x00), 1, 5);

				hk_MmCleanProcessAddressSpace = Hooks::JmpRipCode{ clean_process_address_space, (uintptr_t)MmCleanProcessAddressSpace_hook };

				LARGE_INTEGER length_tag;
				length_tag.LowPart = NULL;
				length_tag.HighPart = hk_MmCleanProcessAddressSpace.hook_size;

				svm_vmmcall(VMMCALL_ID::set_npt_hook, clean_process_address_space, hk_MmCleanProcessAddressSpace.hook_code, length_tag.QuadPart);
			}
		}
	}
};