/***********************************************************************
* This is an example that shows how to open a p96 PIP Window 
* to get input events and how to paint in that window.
*
***********************************************************************/

#include	<proto/exec.h>
#include	<proto/dos.h>
#include <proto/picasso96.h>

#include	<intuition/intuition.h>

char	Template[] = "Width=W/N,Height=H/N,Pubscreen=PS/K";
LONG	Array[] = { 0, 0, (LONG)"Workbench"};

struct Library	*P96Base;

void main(void)
{
	if(P96Base=OpenLibrary("Picasso96API.library",2)){
		struct RDArgs	*rda;
		struct Window	*wd;

		LONG	Width = 256, Height = 256;
		char *PubScreenName = "Workbench";
		
		if(rda = ReadArgs(Template, Array, NULL)){
			if(Array[0])	Width =*((LONG *)Array[0]);
			if(Array[1])	Height=*((LONG *)Array[1]);
			PubScreenName = (char *)Array[2];
		}

		if(wd = p96PIP_OpenTags(
										P96PIP_SourceFormat, RGBFB_R5G5B5, // RGBFB_Y4U2V2,
										P96PIP_SourceWidth, 256,
										P96PIP_SourceHeight, 256,
										/* these tags are optional, but help */
										WA_Title, "Picasso96 API PIP Test",
										WA_Activate, TRUE,
										WA_RMBTrap, TRUE,
										WA_Width, Width,
										WA_Height, Height,
										WA_DragBar, TRUE,
										WA_DepthGadget, TRUE,
										WA_SimpleRefresh, TRUE,
										WA_SizeGadget, TRUE,
										WA_CloseGadget, TRUE,
										WA_IDCMP, IDCMP_CLOSEWINDOW,
										WA_PubScreenName, PubScreenName,
										TAG_DONE)){

			struct IntuiMessage	*imsg;
			BOOL	goahead = TRUE;

			struct RastPort *rp = NULL;
			
			p96PIP_GetTags(wd, P96PIP_SourceRPort, (ULONG)&rp, TAG_END);
			
			if(rp){
				UWORD	x, y;

			   for(y=0; y<Height; y++){
			      for(x=0; x<Width; x++){
			        p96WritePixel(rp, x, y, (x*256+y)*256);
					}
				}
			}

			do{
				WaitPort(wd->UserPort);
				while(imsg = (struct IntuiMessage *)GetMsg(wd->UserPort)){

					switch(imsg->Class){
					case	IDCMP_CLOSEWINDOW:
						goahead = FALSE;
						break;
					}
					ReplyMsg((struct Message *)imsg);
				}
			}while(goahead);

			p96PIP_Close(wd);
		}

		if(rda)	FreeArgs(rda);

		CloseLibrary(P96Base);
	}
}
