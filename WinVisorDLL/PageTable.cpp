#include "WinVisorDLL.h"

void *gpPageTableBase = NULL;
DWORD gdwPageTableAllocSize = 0;

MappedVirtualAddressStruct gPagedVirtualAddressList[MAX_MAPPED_PAGE_COUNT];

QWORD gqwCurrPagedVirtualAddressListCreationIndex = 0;

DWORD GetVirtualAddressTableIndexes(UINT64 qwVirtualAddress, VirtualAddressTableIndexesStruct *pVirtualAddressTableIndexes)
{
	// virtual address must be canonical (bits 48-63 must match bit 47)
	if((qwVirtualAddress >> 48) != (WORD)(0 - ((qwVirtualAddress >> 47) & 1)))
	{
		return 1;
	}

	// extract page table indexes from virtual address
	pVirtualAddressTableIndexes->wOffset = (WORD)(qwVirtualAddress & 0xFFF);
	pVirtualAddressTableIndexes->wPT = (WORD)((qwVirtualAddress >> 12) & 0x1FF);
	pVirtualAddressTableIndexes->wPD = (WORD)((qwVirtualAddress >> 21) & 0x1FF);
	pVirtualAddressTableIndexes->wPDPT = (WORD)((qwVirtualAddress >> 30) & 0x1FF);
	pVirtualAddressTableIndexes->wPML4 = (WORD)((qwVirtualAddress >> 39) & 0x1FF);

	return 0;
}

DWORD ResetPageTable(PageTableStruct *pPageTable)
{
	// clear all entries within the specified page table
	for(DWORD i = 0; i < 512; i++)
	{
		pPageTable->qwEntries[i] = 0;
	}

	return 0;
}

PageTableStruct *GetNextTableLevel(PagingStateStruct *pPagingState, PageTableStruct *pPageTable, WORD wIndex)
{
	UINT64 qwTempPhysicalAddress = 0;
	PageTableStruct *pNextPageTable = NULL;

	// check if a child entry already exists for this index
	if(pPageTable->qwEntries[wIndex] != 0)
	{
		qwTempPhysicalAddress = ((pPageTable->qwEntries[wIndex] >> 12) & 0xFFFFFF) * 0x1000;
		pNextPageTable = (PageTableStruct*)((BYTE*)gpPageTableBase + qwTempPhysicalAddress - PAGE_TABLE_BASE_PHYSICAL_ADDRESS);
	}
	else
	{
		// create a new entry
		if(pPagingState->dwNextEntryIndex >= pPagingState->dwTotalEntryCount)
		{
			return NULL;
		}
		pNextPageTable = (PageTableStruct*)((BYTE*)gpPageTableBase + (pPagingState->dwNextEntryIndex * sizeof(PageTableStruct)));
		pPageTable->qwEntries[wIndex] = (PAGE_TABLE_BASE_PHYSICAL_ADDRESS + (pPagingState->dwNextEntryIndex * sizeof(PageTableStruct))) | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
		pPagingState->dwNextEntryIndex++;
		ResetPageTable(pNextPageTable);
	}

	return pNextPageTable;
}

DWORD CreatePageTables()
{
	// allocate page tables (PML4 + all possible tables for next 3 levels)
	gdwPageTableAllocSize = sizeof(PageTableStruct) + (MAX_MAPPED_PAGE_COUNT * (3 * sizeof(PageTableStruct)));
	gpPageTableBase = VirtualAlloc(NULL, gdwPageTableAllocSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if(gpPageTableBase == NULL)
	{
		return 1;
	}

	// map page tables into the guest at a fixed physical address
	if(HypervisorUtils_MapGuestMemory(gpPageTableBase, (void*)PAGE_TABLE_BASE_PHYSICAL_ADDRESS, gdwPageTableAllocSize) != 0)
	{
		DeletePageTables();
		return 1;
	}

	// initialise mapped address list
	memset(gPagedVirtualAddressList, 0, sizeof(gPagedVirtualAddressList));
	for(DWORD i = 0; i < MAX_MAPPED_PAGE_COUNT; i++)
	{
		gPagedVirtualAddressList[i].dwInUse = 0;
		gPagedVirtualAddressList[i].qwCreationIndex = 0;
		gPagedVirtualAddressList[i].qwVirtualAddress = 0;
		gPagedVirtualAddressList[i].qwPhysicalAddress = PAGE_TABLE_BASE_PHYSICAL_ADDRESS + gdwPageTableAllocSize + (i * PAGE_SIZE);
	}

	return 0;
}

DWORD DeletePageTables()
{
	if(gpPageTableBase != NULL)
	{
		// free memory
		VirtualFree(gpPageTableBase, 0, MEM_RELEASE);
	}

	return 0;
}

DWORD RebuildPageTables()
{
	PageTableStruct *pPML4 = NULL;
	PageTableStruct *pPDPT = NULL;
	PageTableStruct *pPD = NULL;
	PageTableStruct *pPT = NULL;
	VirtualAddressTableIndexesStruct VirtualAddressTableIndexes;
	PagingStateStruct PagingState;

	// reset PML4
	pPML4 = (PageTableStruct*)gpPageTableBase;
	ResetPageTable(pPML4);

	// initialise state
	memset(&PagingState, 0, sizeof(PagingState));
	PagingState.dwTotalEntryCount = (gdwPageTableAllocSize / 0x1000);
	PagingState.dwNextEntryIndex = 1;

	// rebuild page tables
	for(DWORD i = 0; i < MAX_MAPPED_PAGE_COUNT; i++)
	{
		if(gPagedVirtualAddressList[i].dwInUse == 0)
		{
			continue;
		}

		// extract table indexes from current virtual address
		memset(&VirtualAddressTableIndexes, 0, sizeof(VirtualAddressTableIndexes));
		if(GetVirtualAddressTableIndexes(gPagedVirtualAddressList[i].qwVirtualAddress, &VirtualAddressTableIndexes) != 0)
		{
			return 1;
		}

		// navigate to the final level of the paging table
		pPDPT = GetNextTableLevel(&PagingState, pPML4, VirtualAddressTableIndexes.wPML4);
		pPD = GetNextTableLevel(&PagingState, pPDPT, VirtualAddressTableIndexes.wPDPT);
		pPT = GetNextTableLevel(&PagingState, pPD, VirtualAddressTableIndexes.wPD);

		// set mirrored page physical address
		pPT->qwEntries[VirtualAddressTableIndexes.wPT] = gPagedVirtualAddressList[i].qwPhysicalAddress | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
	}

	// flush TLB
	HypervisorUtils_FlushTLB();

	return 0;
}

DWORD AddPagedVirtualAddress(UINT64 qwVirtualAddress)
{
	QWORD qwVirtualAddressPage = 0;
	MappedVirtualAddressStruct *pFreeEntry = NULL;
	MappedVirtualAddressStruct *pOldestEntry = NULL;

	// round virtual address down to page base
	qwVirtualAddressPage = qwVirtualAddress & ~0xFFF;

	// check if this page is already mapped
	for(DWORD i = 0; i < MAX_MAPPED_PAGE_COUNT; i++)
	{
		if(gPagedVirtualAddressList[i].dwInUse == 0)
		{
			continue;
		}

		if(gPagedVirtualAddressList[i].qwVirtualAddress == qwVirtualAddressPage)
		{
			// this entry is already paged in - unknown error
			return 1;
		}
	}

	// check for a free entry in the list
	for(DWORD i = 0; i < MAX_MAPPED_PAGE_COUNT; i++)
	{
		if(gPagedVirtualAddressList[i].dwInUse == 0)
		{
			pFreeEntry = &gPagedVirtualAddressList[i];
			break;
		}
	}

	if(pFreeEntry == NULL)
	{
		// list is full - find the oldest entry and remove it
		for(DWORD i = 0; i < MAX_MAPPED_PAGE_COUNT; i++)
		{
			if(gPagedVirtualAddressList[i].dwInUse == 0)
			{
				continue;
			}

			if(pOldestEntry == NULL)
			{
				pOldestEntry = &gPagedVirtualAddressList[i];
			}
			else
			{
				if(gPagedVirtualAddressList[i].qwCreationIndex < pOldestEntry->qwCreationIndex)
				{
					pOldestEntry = &gPagedVirtualAddressList[i];
				}
			}
		}

		if(pOldestEntry == NULL)
		{
			return 1;
		}

		// remove oldest entry
		HypervisorUtils_UnmapGuestMemory((void*)pOldestEntry->qwPhysicalAddress, PAGE_SIZE);
		pOldestEntry->dwInUse = 0;
		pFreeEntry = pOldestEntry;
	}

	// map page into guest
	if(HypervisorUtils_MapGuestMemory((void*)qwVirtualAddressPage, (void*)pFreeEntry->qwPhysicalAddress, PAGE_SIZE) != 0)
	{
		return 1;
	}

	// store current entry in list
	pFreeEntry->dwInUse = 1;
	pFreeEntry->qwVirtualAddress = qwVirtualAddressPage;
	pFreeEntry->qwCreationIndex = gqwCurrPagedVirtualAddressListCreationIndex;
	gqwCurrPagedVirtualAddressListCreationIndex++;

	// rebuild page tables
	if(RebuildPageTables() != 0)
	{
		return 1;
	}

	return 0;
}
