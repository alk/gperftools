// tcmalloc-test.cpp : Defines the entry point for the console application.
//

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <gperftools/malloc_extension.h>
//#include <libc_override.h>

#ifdef WIN32_DO_PATCHING
#error asdasd
#endif

size_t malloc_prop_get(const char *prop)
{
	size_t value = (size_t)(-1);
	MallocExtension::instance()->GetNumericProperty(prop, &value);
	return value;
}

int main(int argc, char* argv[])
{
	char *arr;
	printf("stuff %llu\n", (unsigned long long)(malloc_prop_get("generic.current_allocated_bytes")));
	arr = new char[10];
	printf("stuff2 %llu\n", (unsigned long long)(malloc_prop_get("generic.current_allocated_bytes")));
	
	delete [] arr;
	printf("stuff2 %llu\n", (unsigned long long)(malloc_prop_get("generic.current_allocated_bytes")));
	getchar();
	return 0;
}

