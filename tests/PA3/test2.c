#include <stdio.h>
#include <stdlib.h>
#include "memory.h"

typedef unsigned long long u64;

struct A
{
	u64 *a;
};

struct B
{
	u64 a;
};

void foo()
{
	struct B *v1 = (struct B*)mymalloc(sizeof(struct B));
	struct A *v2 = (struct A*)v1;
}

int main()
{
	foo();
	return 0;
}
