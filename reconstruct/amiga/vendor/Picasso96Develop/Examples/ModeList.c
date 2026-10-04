/***********************************************************************
* This is example shows how to use p96AllocModeListTagList()
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
		struct RDArgs	*rda;
		struct List		*ml;
		LONG width=640, height=480, depth=8;
		
		if(rda=ReadArgs(template,array,NULL)){
			if(array[0])	width =*((LONG *)array[0]);
			if(array[1])	height=*((LONG *)array[1]);
			if(array[2])	depth =*((LONG *)array[2]);
			FreeArgs(rda);
		}
	
		if(ml=p96AllocModeListTags(
										P96MA_MinWidth, width,
										P96MA_MinHeight, height,
										P96MA_MinDepth, depth,
										TAG_DONE)){
			struct P96Mode	*mn;

			for(mn=(struct P96Mode *)(ml->lh_Head);
					mn->Node.ln_Succ;
					mn=(struct P96Mode *)mn->Node.ln_Succ){

				printf("%s\n",mn->Description);
			}

			p96FreeModeList(ml);
		}
		CloseLibrary(P96Base);
	}
}
