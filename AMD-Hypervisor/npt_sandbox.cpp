#include "npt_sandbox.h"
#include "logging.h"
#include "paging_utils.h"
#include "disassembly.h"
#include "portable_executable.h"
#include "vmexit.h"
#include "utils.h"

namespace Sandbox
{
	void* sandbox_handler = NULL;
	int sandbox_page_count;
	SandboxPage* sandbox_page_array;

	void Init()
	{
		/*	reserve memory for hooks because we can't allocate memory in VM root	*/

		int max_hooks = 6000;

		sandbox_page_array = (SandboxPage*)ExAllocatePoolZero(NonPagedPool, sizeof(SandboxPage) * max_hooks, 'hook');

		for (int i = 0; i < max_hooks; ++i)
		{
			sandbox_page_array[i].Init();
		}

		sandbox_page_count = 0;
	}

	void LogSandboxPageAccess(CoreData* vcpu_data)
	{
		auto guest_rip = vcpu_data->guest_vmcb.save_state_area.Rip;

		BOOL is_kernel = (__readcr3() == vcpu_data->guest_vmcb.save_state_area.Cr3) ? TRUE : FALSE;

		if (is_kernel)
		{
			if (((uintptr_t)guest_rip > 0x7FFFFFFFFFFF))
			{
				vcpu_data->guest_vmcb.save_state_area.Rip = (uintptr_t)sandbox_handler;
				vcpu_data->guest_vmcb.save_state_area.Rax = (uintptr_t)guest_rip;
			}
		}
		else if (((uintptr_t)guest_rip < 0x7FFFFFFFFFFF))
		{
			vcpu_data->guest_vmcb.save_state_area.Rip = (uintptr_t)sandbox_handler;
			vcpu_data->guest_vmcb.save_state_area.Rax = (uintptr_t)guest_rip;
		}
	}

	SandboxPage* ForEachHook(bool(HookCallback)(SandboxPage* hook_entry, void* data), void* callback_data)
	{
		for (int i = 0; i < sandbox_page_count; ++i)
		{
			if (HookCallback(&sandbox_page_array[i], callback_data))
			{
				return &sandbox_page_array[i];
			}
		}
		return 0;
	}

	void ReleasePage(SandboxPage* hook_entry)
	{
		hook_entry->hookless_npte->ExecuteDisable = 0;
		hook_entry->process_cr3 = 0;
		hook_entry->active = false;
		hook_entry->guest_pte->ExecuteDisable = hook_entry->original_nx;

		PageUtils::UnlockPages(hook_entry->mdl);

		sandbox_page_count -= 1;
	}

	/*	
	
		IMPORTANT: if you want to set a hook in a globally mapped DLL such as ntdll.dll, you must trigger copy on write first!	
		Sandbox::IsolatePage() is basically just a modified version of NPTHooks::SetNptHook
	*/

	SandboxPage* IsolatePage(CoreData* vmcb_data, void* address, int32_t tag)
	{
		auto vmroot_cr3 = __readcr3();

		__writecr3(vmcb_data->guest_vmcb.save_state_area.Cr3);

		auto hook_entry = &sandbox_page_array[sandbox_page_count];

		if ((uintptr_t)address < 0x7FFFFFFFFFF)
		{
			hook_entry->mdl = PageUtils::LockPages(address, IoReadAccess, UserMode);
		}
		else
		{
			hook_entry->mdl = PageUtils::LockPages(address, IoReadAccess, KernelMode);
		}

		auto physical_page = PAGE_ALIGN(MmGetPhysicalAddress(address).QuadPart);

		sandbox_page_count += 1;

		hook_entry->active = true;
		hook_entry->tag = tag;
		hook_entry->process_cr3 = vmcb_data->guest_vmcb.save_state_area.Cr3;
		hook_entry->noexecute_ncr3 = Hypervisor::Get()->ncr3_dirs[NCR3_DIRECTORIES::sandbox];

		/*	get the guest pte and physical address of the hooked page	*/

		hook_entry->guest_physical_page = (uint8_t*)physical_page;

		hook_entry->guest_pte = PageUtils::GetPte((void*)address, hook_entry->process_cr3);

		hook_entry->original_nx = hook_entry->guest_pte->ExecuteDisable;

		hook_entry->guest_pte->ExecuteDisable = 0;
		hook_entry->guest_pte->Write = 1;


		/*	disable execute on the nested pte of the guest physical address, in NCR3 1	*/

		hook_entry->hookless_npte = PageUtils::GetPte((void*)physical_page, Hypervisor::Get()->ncr3_dirs[primary]);

		hook_entry->hookless_npte->ExecuteDisable = 1;


		/*	get the nested pte of the guest physical address in the sandbox NCR3, and map it to our page	*/

		PHYSICAL_ADDRESS sandbox_ncr3;

		sandbox_ncr3.QuadPart = Hypervisor::Get()->ncr3_dirs[NCR3_DIRECTORIES::sandbox];

		auto pml4_base = (PML4E_64*)MmGetVirtualForPhysical(sandbox_ncr3);

		auto hooked_npte = AssignNPTEntry(pml4_base, (uintptr_t)physical_page, true);

		hooked_npte->ExecuteDisable = 0;

		auto hooked_copy = PageUtils::VirtualAddrFromPfn(hooked_npte->PageFrameNumber);

		memcpy((uint8_t*)hooked_copy, PAGE_ALIGN(address), PAGE_SIZE);

		/*	SetNptHook epilogue	*/

		vmcb_data->guest_vmcb.control_area.TlbControl = 3;

		__writecr3(vmroot_cr3);

		return hook_entry;
	}

};