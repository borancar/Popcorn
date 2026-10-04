/***********************************************************************
* This is example shows how to use p96RequestModeIDTagList()
*
* tabt (Sat Dec 28 03:44:35 1996)
***********************************************************************/

#include <proto/exec.h>
#include <proto/dos.h>
#include	<proto/graphics.h>
#include <proto/picasso96.h>

#include <stdio.h>

char template[]="Width=W/N,Height=H/N,Depth=D/N";

LONG array[]={	0, 0, 0, FALSE	};

struct Library *P96Base;

void main(int argc,char **argv)
{
	if(P96Base=OpenLibrary("Picasso96API.library",2)){
		struct RDArgs *rda;

		ULONG	DisplayID;
		LONG width=640, height=480, depth=15;
		
		if(rda=ReadArgs(template,array,NULL)){
			if(array[0])	width =*((LONG *)array[0]);
			if(array[1])	height=*((LONG *)array[1]);
			if(array[2])	depth =*((LONG *)array[2]);
			FreeArgs(rda);
		}
	
		if(DisplayID=p96RequestModeIDTags(
										P96MA_MinWidth, width,
										P96MA_MinHeight, height,
										P96MA_MinDepth, depth,
										P96MA_WindowTitle, "RequestModeID Test",
										P96MA_FormatsAllowed, (RGBFF_CLUT|RGBFF_R5G6B5|RGBFF_R8G8B8|RGBFF_A8R8G8B8),
										TAG_DONE)){
			printf("DisplayID: %lx\n", DisplayID);
			if(DisplayID != INVALID_ID){
				struct DimensionInfo dim;
				if(GetDisplayInfoData(NULL,(UBYTE *)&dim,sizeof(dim),DTAG_DIMS,DisplayID)){
					printf("Dimensions: %ldx%ldx%ld\n",(ULONG)(dim.Nominal.MaxX-dim.Nominal.MinX+1),(ULONG)(dim.Nominal.MaxY-dim.Nominal.MinY+1),(ULONG)dim.MaxDepth);
				}
			}
		}
		CloseLibrary(P96Base);
	}
}
