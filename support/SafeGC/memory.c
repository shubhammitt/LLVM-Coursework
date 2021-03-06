#define _GNU_SOURCE

#include "memory.h"

long long NumGCTriggered = 0;
long long NumBytesFreed = 0;
long long NumBytesAllocated = 0;
extern char  etext, edata, end;
//static void myfree(void *Ptr);
static void checkAndRunGC();

static void setAllocPtr(Segment *Seg, char *Ptr) { Seg->Other.AllocPtr = Ptr; }
static void setCommitPtr(Segment *Seg, char *Ptr) { Seg->Other.CommitPtr = Ptr; }
static void setReservePtr(Segment *Seg, char *Ptr) { Seg->Other.ReservePtr = Ptr; }
static void setDataPtr(Segment *Seg, char *Ptr) { Seg->Other.DataPtr = Ptr; }
static char* getAllocPtr(Segment *Seg) { return Seg->Other.AllocPtr; }
static char* getCommitPtr(Segment *Seg) { return Seg->Other.CommitPtr; }
static char* getReservePtr(Segment *Seg) { return Seg->Other.ReservePtr; }
static char* getDataPtr(Segment *Seg) { return Seg->Other.DataPtr; }
static void setBigAlloc(Segment *Seg, int BigAlloc) { Seg->Other.BigAlloc = BigAlloc; }
static int getBigAlloc(Segment *Seg) { return Seg->Other.BigAlloc; }
static void addToSegmentList(Segment *Seg)
{
	SegmentList *L = malloc(sizeof(SegmentList));
	if (L == NULL)
	{
		printf("Unable to allocate list node\n");
		exit(0);
	}
	L->Segment = Seg;
	L->Next = Segments;
	Segments = L;
}

static void allowAccess(void *Ptr, size_t Size)
{
	assert((Size % PAGE_SIZE) == 0);
	assert(((ulong64)Ptr & (PAGE_SIZE-1)) == 0);

	int Ret = mprotect(Ptr, Size, PROT_READ|PROT_WRITE);
	if (Ret == -1)
	{
		printf("unable to mprotect %s():%d\n", __func__, __LINE__);
		exit(0);
	}
}

static Segment* allocateSegment(int BigAlloc)
{
	void* Base = mmap(NULL, SEGMENT_SIZE * 2, PROT_NONE, MAP_ANON|MAP_PRIVATE, -1, 0);
	if (Base == MAP_FAILED)
	{
		printf("unable to allocate a segment\n");
		exit(0);
	}

	/* segments are aligned to segment size */
	Segment *Segment = (struct Segment*)Align((ulong64)Base, SEGMENT_SIZE);
	allowAccess(Segment, METADATA_SIZE);

	char *AllocPtr = (char*)Segment + METADATA_SIZE;
	char *ReservePtr = (char*)Segment + SEGMENT_SIZE;
	setAllocPtr(Segment, AllocPtr);
	setReservePtr(Segment, ReservePtr);
	setCommitPtr(Segment, AllocPtr);
	setDataPtr(Segment, AllocPtr);
	setBigAlloc(Segment, BigAlloc);
	addToSegmentList(Segment);
	return Segment;
}

static void extendCommitSpace(Segment *Seg)
{
	char *AllocPtr = getAllocPtr(Seg);
	char *CommitPtr = getCommitPtr(Seg);
	char *ReservePtr = getReservePtr(Seg);
	char *NewCommitPtr = CommitPtr + COMMIT_SIZE;

	assert(AllocPtr == CommitPtr);
	if (NewCommitPtr <= ReservePtr)
	{
		allowAccess(CommitPtr, COMMIT_SIZE);
		setCommitPtr(Seg, NewCommitPtr);
	}
	else
	{
		assert(CommitPtr == ReservePtr);
	}
}

static unsigned short* getSizeMetadata(char *Ptr)
{
	char *Page = ADDR_TO_PAGE(Ptr);
	Segment *Seg = ADDR_TO_SEGMENT(Ptr);
	ulong64 PageNo = (Page - (char*)Seg)/ PAGE_SIZE;
	return &Seg->Size[PageNo];
}

static void createHole(Segment *Seg)
{
	char *AllocPtr = getAllocPtr(Seg);
	char *CommitPtr = getCommitPtr(Seg);
	size_t HoleSz = CommitPtr - AllocPtr;
	if (HoleSz > 0)
	{
		assert(HoleSz >= 8);
		ObjHeader *Header = (ObjHeader*)AllocPtr;
		Header->Size = HoleSz;
		Header->Status = 0;
		Header->Alignment = 0;
		setAllocPtr(Seg, CommitPtr);
		myfree(AllocPtr + OBJ_HEADER_SIZE);
		NumBytesFreed -= HoleSz;
	}
}

static void reclaimMemory(void *Ptr, size_t Size)
{
	assert((Size % PAGE_SIZE) == 0);
	assert(((ulong64)Ptr & (PAGE_SIZE-1)) == 0);
	
	int Ret = mprotect(Ptr, Size, PROT_NONE);
	if (Ret == -1)
	{
		printf("unable to mprotect %s():%d\n", __func__, __LINE__);
		exit(0);
	}
	Ret = madvise(Ptr, Size, MADV_DONTNEED);
	if (Ret == -1)
	{
		printf("unable to reclaim physical page %s():%d\n", __func__, __LINE__);
		exit(0);
	}
}

/* used by the GC to free objects. */
void myfree(void *Ptr)
{
	ObjHeader *Header = (ObjHeader*)((char*)Ptr - OBJ_HEADER_SIZE);
	assert((Header->Status & FREE) == 0);
	NumBytesFreed += Header->Size;

	if (Header->Size > COMMIT_SIZE)
	{
		assert((Header->Size % PAGE_SIZE) == 0);
		assert(((ulong64)Header & (PAGE_SIZE-1)) == 0);
		size_t Size = Header->Size;
		char *Start = (char*)Header;
		size_t Iter;
		for (Iter = 0; Iter < Size; Iter += PAGE_SIZE)
		{
			unsigned short *SzMeta = getSizeMetadata((char*)Start + Iter);
			SzMeta[0] = PAGE_SIZE;
		}
		Header->Status = FREE;
		reclaimMemory(Header, Header->Size);
		return;
	}

	unsigned short *SzMeta = getSizeMetadata((char*)Header);
	SzMeta[0] += Header->Size;
	assert(SzMeta[0] <= PAGE_SIZE);
	Header->Status = FREE;
	if (SzMeta[0] == PAGE_SIZE)
	{
		char *Page = ADDR_TO_PAGE(Ptr);
		reclaimMemory(Page, PAGE_SIZE);
	}
}

static void* BigAlloc(size_t Size)
{
	size_t AlignedSize = Align(Size + OBJ_HEADER_SIZE, PAGE_SIZE);
	NumBytesAllocated += AlignedSize;
	checkAndRunGC(AlignedSize);
	assert(AlignedSize <= SEGMENT_SIZE - METADATA_SIZE);
	static Segment *CurSeg = NULL;
	if (CurSeg == NULL)
	{
		CurSeg = allocateSegment(1);
	}
	char *AllocPtr = getAllocPtr(CurSeg);
	char *CommitPtr = getCommitPtr(CurSeg);
	char *NewAllocPtr = AllocPtr + AlignedSize;
	char *ReservePtr = getReservePtr(CurSeg);
	if (NewAllocPtr > ReservePtr)
	{
		CurSeg = allocateSegment(1);
		return BigAlloc(Size);
	}
	assert(AllocPtr == CommitPtr);
	allowAccess(CommitPtr, AlignedSize);
	setAllocPtr(CurSeg, NewAllocPtr);
	setCommitPtr(CurSeg, NewAllocPtr);

	unsigned short *SzMeta = getSizeMetadata(AllocPtr);
	SzMeta[0] = 1;

	ObjHeader *Header = (ObjHeader*)AllocPtr;
	Header->Size = AlignedSize;
	Header->Status = 0;
	Header->Alignment = 0;
	Header->Type = 0;
	return AllocPtr + OBJ_HEADER_SIZE;
}


void *_mymalloc(size_t Size)
{
	size_t AlignedSize = Align(Size, 8) + OBJ_HEADER_SIZE;

	if (AlignedSize > COMMIT_SIZE)
	{
		return BigAlloc(Size);
	}
	checkAndRunGC(AlignedSize);
	assert(Size != 0);
	assert(sizeof(struct OtherMetadata) <= OTHER_METADATA_SIZE);
	assert(sizeof(struct Segment) == METADATA_SIZE);

	static Segment *CurSeg = NULL;

	if (CurSeg == NULL)
	{
		CurSeg = allocateSegment(0);
	}
	char *AllocPtr = getAllocPtr(CurSeg);
	char *CommitPtr = getCommitPtr(CurSeg);
	char *NewAllocPtr = AllocPtr + AlignedSize;
	if (NewAllocPtr > CommitPtr)
	{
		if (AllocPtr != CommitPtr)
		{
			/* Free remaining space on this page */
			createHole(CurSeg);
		}
		extendCommitSpace(CurSeg);
		AllocPtr = getAllocPtr(CurSeg);
		NewAllocPtr = AllocPtr + AlignedSize;
		CommitPtr = getCommitPtr(CurSeg);
		if (NewAllocPtr > CommitPtr)
		{
			CurSeg = allocateSegment(0);
			return _mymalloc(Size);
		}
	}

	NumBytesAllocated += AlignedSize;
	setAllocPtr(CurSeg, NewAllocPtr);
	ObjHeader *Header = (ObjHeader*)AllocPtr;
	Header->Size = AlignedSize;
	Header->Status = 0;
	Header->Alignment = 0;
	Header->Type = 0;
	return AllocPtr + OBJ_HEADER_SIZE;
}


/************************************************************************************************
 * for storing reference to the header of reachable objects in the form of singly linked list 	*
 ************************************************************************************************/
typedef struct UnscannedList {
	unsigned char *objHeader;
	struct UnscannedList *next;
} UnscannedList;

UnscannedList *UnscannedListHead = NULL;
UnscannedList *UnscannedListTail = NULL;


/************************************************************************************************
 * 							adds new node at the end of list i.e. tail 							*
 ************************************************************************************************/
void addToUnscannedList(unsigned char *objHeader) {
	
	UnscannedList *node = (UnscannedList*)malloc(sizeof(UnscannedList));
	if (node == NULL) {
		printf("Unable to allocate list node\n");
		exit(0);
	}
	node -> next = NULL;
	node -> objHeader = objHeader;

	if (UnscannedListHead == NULL)
		UnscannedListHead = node;
	else
		UnscannedListTail -> next = node;
	UnscannedListTail = node;
}


/************************************************************************************************
 * Idea:																						*
 *		We can iterate over segments stored in SegmentList. 									*
 * 		In each segment, iterate from dataptr to allocptr. Since myfree stores the amount of	*
 *		memory free for each page, we can use it to know whether current page is free or not.	*
 * 		If free, then move to next page. Otherwise, start of the page will contain the 			*
 * 		objectHeader and using that we will free the object depending on its Status bit and 	*
 * 		move to next Header using the size of object stored in current objectHeader.			*
 ************************************************************************************************/
void sweep() {
	
	SegmentList *segHead = Segments;
	while (segHead) {
		
		char *startptr = getDataPtr(segHead -> Segment);
		char *endptr = getAllocPtr(segHead -> Segment);

		while(startptr < endptr) {
			
			if (getSizeMetadata(ADDR_TO_PAGE(startptr))[0] < PAGE_SIZE) {
				
				ObjHeader *objHeader = (ObjHeader*)(startptr);
				startptr += objHeader -> Size;						// cannot be done later since page might be freed
				
				if(objHeader -> Status == 0){				
					myfree((void*)objHeader + OBJ_HEADER_SIZE);		// object is not reachable, so free it
				}
				else if(objHeader -> Status == MARK) 		
					objHeader -> Status = 0;						// object is reachable so cannot be freed, unmark it
			}
			else										
				startptr = ADDR_TO_PAGE(startptr) + PAGE_SIZE;		// page is already free, go to next page
		}
		segHead = segHead -> Next;									// go to next Segment
	}
}


/************************************************************************************************
 * returns whether address lies in between Data Ptr and Alloc Ptr of some segment 				*
 ************************************************************************************************/
int isPresentInSegmentList(char *addr) {
	
	SegmentList *head = Segments;
	
	while (head) {
		
		if (getDataPtr(head -> Segment) <= addr && addr <= getAllocPtr(head -> Segment))
			return 1;
		
		head = head -> Next;
	}
	return 0;
}

/************************************************************************************************
 * returns: objectHeader corresponding to the object's reference stored in pointer: char *addr 	*
 *			returns NULL for invalid addresses 													*
 * Idea: 				 				 				 										*
 * 		Object's size >  PAGE_SIZE:  				 											*
 * 			"The metadata corresponding to the first page of a big allocation is set to one to 	*
 * 			identify the first byte of these objects." We exploit this feature to find the 		*
 * 			first page of big allocation which in turn stores the objectHeader corresponding 	*
 * 			to the given object at the top of that page. We do this by finding the page 		*
 * 			containing address stored in 'addr' and if the byte is not to set to one then we 	*
 * 			move on to previous page and repeat the same until we reach the first page and then *
 *  		return the objectHeader stored at top of this page. 								*
 *		Object's size <= PAGE_SIZE:																*
 * 			Since an object+its header lies on single page, we can find the page of given 		*
 * 			object. Also, we know that start of the page contains object header, may be of some *
 * 			other object, we can iterate on all object headers in current page using the size 	*
 * 			of object stored in object header which will help in finding next object Header in 	*
 * 			current	page and ultimately finding the header corresponding to given object. 		*
 ************************************************************************************************/
ObjHeader* getObjectHeader(char *addr) {

	if(isPresentInSegmentList(addr) == 0)
		return NULL;

	if (getBigAlloc(ADDR_TO_SEGMENT(addr))) {	/* Find objectHeader for bigAlloc */

		char *myPage = ADDR_TO_PAGE(addr);
		// iterate until first page of bigAlloc is reached
		while (getSizeMetadata(myPage)[0] != 1) myPage -= PAGE_SIZE;
		// return objectHeader if application contained reference 
		// to object and not to its header(i.e >= mypage + 16)
		if (myPage + 16 <= addr)
			return (ObjHeader*)myPage;
	}
	else {										/* Find object header for smallAlloc*/

		ObjHeader *objHeader = (ObjHeader*)ADDR_TO_PAGE(addr);				// first header of current page
		
		while (1) {
			
			if (addr < (char*)objHeader + OBJ_HEADER_SIZE)					// application contains reference to header and not object
				return NULL;
			
			if (addr <= (char*)objHeader + objHeader -> Size)
				return objHeader;
			
			objHeader = (ObjHeader*)((char*)objHeader + objHeader -> Size);	// jump to next Header in current page
		}
	}
	return NULL;
}


/************************************************************************************************ 
 * walk all addresses in the range [Top, Bottom-8].												*
 * add unmarked valid objects to the scanner list after marking them for scanning.				*
 ************************************************************************************************/
static void scanRoots(unsigned char *Top, unsigned char *Bottom) {	// top < bottom	
	
	Bottom -= 8;
	for (unsigned char *addr = Top ; addr <= Bottom ;  addr++) {
		
		char *valueAtAddr = (char*)(*((ulong64*)addr));
		ObjHeader *objHeader = getObjectHeader(valueAtAddr);
		
		if (objHeader && objHeader -> Status == 0) { 					// if object is not marked/free
			objHeader -> Status = MARK;
			addToUnscannedList((unsigned char*)objHeader);
		}
	}
}


/************************************************************************************************
 * scan objects in the scanner list.															*
 * add newly encountered unmarked objects 														*
 * to the not scanned list after marking them.														*
 ************************************************************************************************/
static void scanner() {
	UnscannedList *nextHead = NULL;
	while (UnscannedListHead) {
		
		scanRoots(UnscannedListHead -> objHeader + OBJ_HEADER_SIZE, 
				  UnscannedListHead -> objHeader + ((ObjHeader*)UnscannedListHead -> objHeader) -> Size);
		
		nextHead = UnscannedListHead -> next;
		free(UnscannedListHead);
		UnscannedListHead = nextHead;
	}
}


static size_t
getDataSecSz()
{
	char Exec[PATH_SZ];
	static size_t DsecSz = 0;

	if (DsecSz != 0)
	{
		return DsecSz;
	}
	DsecSz = -1;

	ssize_t Count = readlink( "/proc/self/exe", Exec, PATH_SZ);

	if (Count == -1) {
		return -1;
	}
	Exec[Count] = '\0';

	int fd = open(Exec, O_RDONLY);
	if (fd == -1) {
		return -1;
	}

	struct stat Statbuf;
	fstat(fd, &Statbuf);

	char *Base = mmap(NULL, Statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (Base == NULL) {
		close(fd);
		return -1;
	}

	Elf64_Ehdr *Header = (Elf64_Ehdr*)Base;

	if (Header->e_ident[0] != 0x7f
		|| Header->e_ident[1] != 'E'
		|| Header->e_ident[2] != 'L'
		|| Header->e_ident[3] != 'F')
	{
		goto out;
	}

	int i;
	Elf64_Shdr *Shdr = (Elf64_Shdr*)(Base + Header->e_shoff);
	char *Strtab = Base + Shdr[Header->e_shstrndx].sh_offset;

	for (i = 0; i < Header->e_shnum; i++)
	{
		char *Name = Strtab + Shdr[i].sh_name;
		if (!strncmp(Name, ".data", 6))
		{
			DsecSz = Shdr[i].sh_size;
		}
	}

out:
	munmap(Base, Statbuf.st_size);
	close(fd);
	return DsecSz;
}



void _runGC()
{
	NumGCTriggered++;

	size_t DataSecSz = getDataSecSz();
	unsigned char *DataStart;

	if (DataSecSz == -1)
	{
		DataStart = (unsigned char*)&etext;
	}
	else
	{
		DataStart = (unsigned char*)((char*)&edata - DataSecSz);
	}
	unsigned char *DataEnd = (unsigned char*)(&edata);

	/* scan global variables */
	scanRoots(DataStart, DataEnd);

	unsigned char *UnDataStart = (unsigned char*)(&edata);
	unsigned char *UnDataEnd = (unsigned char*)(&end);

	/* scan uninitialized global variables */
	scanRoots(UnDataStart, UnDataEnd);

	
	int Lvar;
	void *Base;
	size_t Size;
	pthread_attr_t Attr;
	
	int Ret = pthread_getattr_np(pthread_self(), &Attr);
	if (Ret != 0)
	{
		printf("Error getting stackinfo\n");
		return;
	}
	Ret = pthread_attr_getstack(&Attr , &Base, &Size);
	if (Ret != 0)
	{
		printf("Error getting stackinfo\n");
		return;
	}
	unsigned char *Bottom = (unsigned char*)(Base + Size);
	unsigned char *Top = (unsigned char*)&Lvar;
	/* skip GC stack frame */
	while (*((unsigned*)Top) != MAGIC_ADDR)
	{
		assert(Top < Bottom);
		Top++;
	}
	/* scan application stack */
	scanRoots(Top, Bottom);

	scanner();
	sweep();
}

static void checkAndRunGC(size_t Sz)
{
	static size_t TotalAlloc = 0;

	TotalAlloc += Sz;
	if (TotalAlloc < GC_THRESHOLD)
	{
		return;
	}
	TotalAlloc = 0;
	_runGC();
}

void printMemoryStats()
{
	printf("Num Bytes Allocated: %lld\n", NumBytesAllocated);
	printf("Num Bytes Freed: %lld\n", NumBytesFreed);
	printf("Num GC Triggered: %lld\n", NumGCTriggered);
}

static ObjHeader* ObjToHeader(void *Obj) { return (ObjHeader*)((char*)Obj - OBJ_HEADER_SIZE); }

unsigned GetSize(void *Obj)
{
	ObjHeader *Header = ObjToHeader(Obj);
	return (Header->Size) - OBJ_HEADER_SIZE;
}

unsigned long long GetType(void *Obj)
{
	ObjHeader *Header = ObjToHeader(Obj);
	return Header->Type;
}

void SetType(void *Obj, unsigned long long Type)
{
	ObjHeader *Header = ObjToHeader(Obj);
	Header->Type = Type;
}

void* GetAlignedAddr(void *Addr, size_t Alignment)
{
	ObjHeader *Header = ObjToHeader(Addr);
	Header->Alignment = Alignment;
	return (void*)Align((size_t)(Addr), Alignment);
}

int readArgv(const char* argv[], int idx)
{
	return atoi(argv[idx]);
}
