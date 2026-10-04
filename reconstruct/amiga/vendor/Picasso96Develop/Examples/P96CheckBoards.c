/***********************************************************************
* This is example shows how to use p96GetRTGDataTagList and p96GetBoardDataTagList
*
* tabt (Sat Sep 12 23:06:28 1998)
***********************************************************************/

#include <proto/exec.h>
#include <proto/dos.h>
#include	<proto/graphics.h>
#include <proto/picasso96.h>

#include <stdio.h>

struct Library *P96Base;

void main(int argc,char **argv)
{
	if(P96Base = OpenLibrary(P96NAME, 2)){
		ULONG NumBoards;
		
		if(p96GetRTGDataTags(P96GRD_NumberOfBoards, &NumBoards, TAG_END) == 1){
			int i;

			printf("Looking through all boards installed for Picasso96\n");
			for(i = 0; i < NumBoards; i++){
				char *BoardName;
				ULONG RGBFormats, MemorySize, FreeMemory, LargestFreeMemory, MemoryClock, MoniSwitch;
				int clock;
				
				p96GetBoardDataTags(i,
						P96GBD_BoardName, (ULONG)&BoardName,
						P96GBD_RGBFormats, (ULONG)&RGBFormats,
						P96GBD_TotalMemory, (ULONG)&MemorySize,
						P96GBD_FreeMemory, (ULONG)&FreeMemory,
						P96GBD_LargestFreeMemory, (ULONG)&LargestFreeMemory,
						P96GBD_MemoryClock, (ULONG)&MemoryClock,
						P96GBD_MonitorSwitch, (ULONG)&MoniSwitch,
						TAG_END);
				printf("--------------------------------------------------\n");
				printf("Board %ld:      %s\n", i, BoardName);
				printf("Total size of memory:     %8ld\n", MemorySize);
				printf("Size of free memory:      %8ld\n", FreeMemory);
				printf("Largest free chunk:       %8ld\n", LargestFreeMemory);
				printf("Monitor switch:   %s\n", MoniSwitch ? "set" : "not set");

				printf("\nThis board supports:\n");
				printf("\tfollowing rgb formats:\n");
				if(RGBFormats & RGBFF_NONE)		printf("\t\tPLANAR\n");
				if(RGBFormats & RGBFF_CLUT)		printf("\t\tCHUNKY\n");
				if(RGBFormats & RGBFF_R5G5B5)		printf("\t\tR5G5B5\n");
				if(RGBFormats & RGBFF_R5G5B5PC)	printf("\t\tR5G5B5PC\n");
				if(RGBFormats & RGBFF_B5G5R5PC)	printf("\t\tB5G5R5PC\n");
				if(RGBFormats & RGBFF_R5G6B5)		printf("\t\tR5G6B5\n");
				if(RGBFormats & RGBFF_R5G6B5PC)	printf("\t\tR5G6B5PC\n");
				if(RGBFormats & RGBFF_B5G6R5PC)	printf("\t\tB5G6R5PC\n");
				if(RGBFormats & RGBFF_R8G8B8)		printf("\t\tR8G8B8\n");
				if(RGBFormats & RGBFF_B8G8R8)		printf("\t\tB8G8R8\n");
				if(RGBFormats & RGBFF_A8R8G8B8)	printf("\t\tA8R8G8B8\n");
				if(RGBFormats & RGBFF_A8B8G8R8)	printf("\t\tA8B8G8R8\n");
				if(RGBFormats & RGBFF_R8G8B8A8)	printf("\t\tR8G8B8A8\n");
				if(RGBFormats & RGBFF_B8G8R8A8)	printf("\t\tB8G8R8A8\n");
				if(RGBFormats & RGBFF_Y4U2V2)		printf("\t\tY4U2V2\n");
				if(RGBFormats & RGBFF_Y4U1V1)		printf("\t\tY4U1V1\n");
				clock = (MemoryClock+50000)/100000;
				printf("\tmemory clock set to %ld.%1ld MHz,\n",clock/10,clock%10);
			}
		}
	
		CloseLibrary(P96Base);
	}
}
