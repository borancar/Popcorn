/***********************************************************************
* This is an example that shows how to use p96WriteTrueColorData
* Program terminates when space bar or any mouse button is pressed!
*
* alx (Mon Dec 30 12:09:35 1996)
***********************************************************************/

#include	<proto/exec.h>
#include	<proto/dos.h>
#include	<proto/intuition.h>
#include	<proto/picasso96.h>

#include	<intuition/screens.h>
#include	<exec/memory.h>

#include	<math.h>
#include	<stdio.h>
#include	<stdlib.h>

#define WIDTH 160
#define HEIGHT 160

/* for ReadArgs */
char template[]="Width=W/N,Height=H/N,Depth=D/N";
LONG array[]={	0, 0, 0 };

/* p96WriteTrueColorData only works on True- and HiColorModes */

#define HiColorFormats		(RGBFF_R5G6B5|RGBFF_R5G5B5|RGBFF_R5G6B5PC|RGBFF_R5G5B5PC|RGBFF_B5G6R5PC|RGBFF_B5G5R5PC)
#define TrueColorFormats	(RGBFF_R8G8B8|RGBFF_B8G8R8)
#define TrueAlphaFormats	(RGBFF_R8G8B8A8|RGBFF_B8G8R8A8|RGBFF_A8R8G8B8|RGBFF_A8B8G8R8)
#define UsefulFormats		(HiColorFormats|TrueColorFormats|TrueAlphaFormats)

struct Library		*P96Base;
WORD	Pens[] = {~0};

void main(void){
	struct Screen			*sc;
	struct Window			*win;
	struct RDArgs			*rda;
	ULONG						DisplayID;
	WORD						width=640, height=480, depth=24;
	
	int						i;
	
	/* get user parameters */

	if(rda=ReadArgs(template,array,NULL)){
		if(array[0])	width =*((LONG *)array[0]);
		if(array[1])	height=*((LONG *)array[1]);
		if(array[2])	depth =*((LONG *)array[2]);
		FreeArgs(rda);
	}

	if(P96Base=OpenLibrary("Picasso96API.library",2)){

		DisplayID=p96BestModeIDTags(
										P96BIDTAG_NominalWidth, width,
										P96BIDTAG_NominalHeight, height,
										P96BIDTAG_Depth, depth,
										P96BIDTAG_FormatsAllowed, UsefulFormats,
										TAG_DONE);

		if(sc = p96OpenScreenTags(
										P96SA_DisplayID, DisplayID,
										P96SA_Width, width,
										P96SA_Height, height,
										P96SA_Depth, depth,
										P96SA_AutoScroll, TRUE,
										P96SA_Pens, Pens,
										P96SA_Title, "WriteTrueColorData Test",
										TAG_DONE)){

			if(win=OpenWindowTags(NULL,
											WA_CustomScreen, sc,
											WA_Backdrop, TRUE,
											WA_Borderless, TRUE,
											WA_SimpleRefresh, TRUE,
											WA_RMBTrap, TRUE,
											WA_Activate, TRUE,
											WA_IDCMP, IDCMP_RAWKEY|IDCMP_MOUSEBUTTONS,
											TAG_END)){

				BOOL	quit = FALSE;
				UBYTE	*reddata, *greendata, *bluedata;

				reddata   = AllocVec(WIDTH*HEIGHT, MEMF_ANY);
				greendata = AllocVec(WIDTH*HEIGHT, MEMF_ANY);
				bluedata  = AllocVec(WIDTH*HEIGHT, MEMF_ANY);

				if(reddata && greendata && bluedata){

					struct TrueColorInfo	tci;
					BPTR	file;

					tci.PixelDistance = 1;
					tci.BytesPerRow = WIDTH;
					tci.RedData = reddata; 
					tci.GreenData = greendata; 
					tci.BlueData = bluedata; 

					/* load red data */

					if(file=Open("Symbol.red", MODE_OLDFILE)){
						Read(file, reddata, WIDTH*HEIGHT);
						Close(file);
					}
					if(file=Open("Symbol.green", MODE_OLDFILE)){
						Read(file, greendata, WIDTH*HEIGHT);
						Close(file);
					}
					if(file=Open("Symbol.blue", MODE_OLDFILE)){
						Read(file, bluedata, WIDTH*HEIGHT);
						Close(file);
					}

					/* paint something on the screen */

					p96WriteTrueColorData(&tci,0,0,win->RPort,50,50,WIDTH,HEIGHT);
				}

				FreeVec(reddata);
				FreeVec(greendata);
				FreeVec(bluedata);

				/* wait for input */

				do{
					struct IntuiMessage	*imsg;

					WaitPort(win->UserPort);
					while(imsg=(struct IntuiMessage *)GetMsg(win->UserPort)){
						if((imsg->Class==IDCMP_MOUSEBUTTONS) || ((imsg->Class==IDCMP_RAWKEY) && (imsg->Code==0x40))){

							// press MOUSEBUTTONS or SPACE bar to end program
							quit=TRUE;
						}
						ReplyMsg((struct Message *)imsg);
					}
				}while(!quit);
				CloseWindow(win);
			}
			p96CloseScreen(sc);
		}
		CloseLibrary(P96Base);
	}
}
