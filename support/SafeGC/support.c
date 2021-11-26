#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "memory.h"
#include "support.h"

void checkTypeInv(void *Src, unsigned long long DstType)
{
}

void checkSizeInv(void *Dst, unsigned DstSize)
{
	unsigned DstOrigSize = GetSize(Dst);

	if (DstOrigSize < DstSize)
	{
		printf("Invalid obj size: min_required:%x current:%x\n", 
			(unsigned)DstSize, DstOrigSize);
		exit(0);
	}
}

void checkSizeAndTypeInv(void *Src, unsigned long long DstType, unsigned DstSize)
{
	checkTypeInv(Src, DstType);
	checkSizeInv(Src, DstSize);
}

void* mycast(void *Ptr, unsigned long long Bitmap, unsigned Size)
{
	//checkSizeInv(Ptr, Size);
	SetType(Ptr, Bitmap);
	return Ptr;
}

void IsSafeToEscape(void *Base, void *Ptr)
{
	ObjHeader *objHeader = getObjectHeader((char*)Base);
	int objSize = objHeader->Size - OBJ_HEADER_SIZE;
	char *objStart = (char*)objHeader + OBJ_HEADER_SIZE;
	if((char*)Ptr < objStart || (((char*)Ptr) >= (objStart + objSize))){
		printf("Aborting due to disallowing out-of-bounds pointers\n");
		exit(0);
	}
}

void BoundsCheckWithSize(void *RealBase, void *Ptr, size_t Size, size_t AccessSize)
{
	if(Ptr < RealBase || ((Ptr + AccessSize - 1) >= (RealBase + Size))){
		printf("Aborting due to BoundsCheck\n");
		exit(0);
	}
}

void BoundsCheck(void *Base, void *Ptr, size_t AccessSize)
{
	ObjHeader *objHeader = getObjectHeader((char*)Base);
	int objSize = objHeader->Size - OBJ_HEADER_SIZE;
	char *objStart = (char*)objHeader + OBJ_HEADER_SIZE;
	BoundsCheckWithSize(objStart, Ptr, objSize, AccessSize);
}

int findPosLastSetBit(unsigned long long Type){
	int count = 0;
	while(Type){
		Type >>= 1;
		count++;
	}
	return count - 1;

}

void WriteBarrierWithSize(void *RealBase, void *Ptr, size_t Size,
	size_t AccessSize, unsigned long long Type)
{
	if(Type == 0)
		return;
	
	int numFields = findPosLastSetBit(Type);
	Type = Type ^ (1ULL << numFields); // unset MSB

	int fieldNum = 0;
	for(void *i = RealBase ; i < RealBase + Size - 8 ; i += 8, fieldNum = (fieldNum + 1) % numFields){
		
		if((Type & (1ULL << fieldNum)) != 0 && !((Ptr + AccessSize <= i) || (i + 8 <= Ptr))){
			if((*(int64_t*)i) && ! getObjectHeader((char*)(*(int64_t*)i))){
				printf("Aborting due to Write-Barrier\n");
				exit(0);
			}
		}
	}
}

void WriteBarrier(void *Base, void *Ptr, size_t AccessSize)
{
	ObjHeader *objHeader = getObjectHeader((char*)Base);
	int objSize = objHeader->Size - OBJ_HEADER_SIZE;
	char *objStart = (char*)objHeader + OBJ_HEADER_SIZE;
	WriteBarrierWithSize((void*)objStart, Ptr, objSize, AccessSize, objHeader->Type);
}
