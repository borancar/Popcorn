/***********************************************************************
* This is example shows how to use p96BestModeIDTagList()
*
* tabt (Mon Aug 28 14:07:40 1995)
***********************************************************************/

#include <proto/exec.h>
#include <proto/dos.h>
#include	<proto/graphics.h>
#include <proto/picasso96.h>

#include <stdio.h>

char template[]="Width=W/N,Height=H/N,Depth=D/N";

char	*fmtstrings[RGBFB_MaxFormats] = {
	"RGBFB_NONE",
	"RGBFB_CLUT",
	"RGBFB_R8G8B8",
	"RGBFB_B8G8R8",
	"RGBFB_R5G6B5PC",
	"RGBFB_R5G5B5PC",
	"RGBFB_A8R8G8B8",
	"RGBFB_A8B8G8R8",
	"RGBFB_R8G8B8A8",
	"RGBFB_B8G8R8A8",
	"RGBFB_R5G6B5",
	"RGBFB_R5G5B5",
	"RGBFB_B5G6R5PC",
	"RGBFB_B5G5R5PC"
};

LONG array[]={	0, 0, 0, FALSE	};

struct Library *P96Base;

void main(int argc,char **argv)
{
	if(P96Base=OpenLibrary("Picasso96API.library",2)){
		struct RDArgs *rda;

		ULONG	DisplayID;
		LONG width=640, height=480, depth=24;
		
		if(rda=ReadArgs(template,array,NULL)){
			if(array[0])	width =*((LONG *)array[0]);
			if(array[1])	height=*((LONG *)array[1]);
			if(array[2])	depth =*((LONG *)array[2]);
			FreeArgs(rda);
		}
	
		if(DisplayID=p96BestModeIDTags(
										P96BIDTAG_NominalWidth, width,
										P96BIDTAG_NominalHeight, height,
										P96BIDTAG_Depth, depth,
										P96BIDTAG_FormatsForbidden, (RGBFF_R5G5B5|RGBFF_R5G5B5PC|RGBFF_B5G5R5PC),
										TAG_DONE)){
			printf("DisplayID: %lx\n", DisplayID);
			if(DisplayID != INVALID_ID){
				printf("Width: %ld\n", p96GetModeIDAttr(DisplayID, P96IDA_WIDTH));
				printf("Height: %ld\n", p96GetModeIDAttr(DisplayID, P96IDA_HEIGHT));
				printf("Depth: %ld\n", p96GetModeIDAttr(DisplayID, P96IDA_DEPTH));
				printf("BytesPerPixel: %ld\n", p96GetModeIDAttr(DisplayID, P96IDA_BYTESPERPIXEL));
				printf("BitsPerPixel: %ld\n", p96GetModeIDAttr(DisplayID, P96IDA_BITSPERPIXEL));
				printf("RGBFormat: %s\n", fmtstrings[p96GetModeIDAttr(DisplayID,P96IDA_RGBFORMAT)]);
				printf("Is P96: %s\n", p96GetModeIDAttr(DisplayID, P96IDA_ISP96) ? "yes" : "no");
			}
		}
		CloseLibrary(P96Base);
	}
}
