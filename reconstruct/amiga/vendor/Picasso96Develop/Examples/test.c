#include <proto/picasso96.h>

#include <stdio.h>

int main(int argc, char ** argv)
{
	ULONG DisplayID;
	DisplayID = p96RequestModeIDTags(TAG_END);
	printf("DisplayID %08lx\n",DisplayID);
	return(0);
}
