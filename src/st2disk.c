/* Ecrit un fichier .ST sur la disquette */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ext.h>
#include <aes.h>
#include <vdi.h>
#include <tos.h>
#include "st2disk.h"

#define RSC_FILENAME	"ST2DISK.RSC"

int apid;	/* AES id */
int vdih;	/* VDI handle */
int rsc_loaded;

OBJECT* dlg_main;
GRECT dlg_main_size;


/* D‚crit la structure d'une disquette */
typedef struct {
	int nTracks;
	int nSectors;
	int nSides;
	int sectorSize;
	int sectorsPerTrack;
} DISK_STRUCTURE;

/* Operation progress, for use by callback */
typedef struct {
	float format;
	float write;
	float read;
	int track;
	int side;
} PROGRESS;

/* Prototypes business */
int   st2disk(const char *filename, void (*updateProgress)(PROGRESS *p));
short intel2motorola(short n); /* Convert little endian to big endian */
short lsbmsb2short(char lsb, char msb);
int   analyzeDisk(DISK_STRUCTURE *dsk, const char *bootsector, long size);

/* Prototypes UI */
void showDialog(void);
void hideDialog(void);
int loadFile(char *file);
void updateProgress(PROGRESS *progress);

int main(int argc, char **argv)
{
	int argumentNumber;
	char fileToProcess[256];
	char trackSide[100];
	char formAlertMessage[1000];
	int clicked;
	int workin[11],workout[27];

	strcpy(fileToProcess,"");

	/* Initalise GEM so we can use file selector and so on */	
	apid = appl_init();
	{
		int i;
		for (i=0; i<10; workin[i++]=1);
		workin[10] = 2;
		v_opnvwk(workin, &vdih, workout);
	}

	/* Load resource file and get address of main form */
	rsc_loaded = rsrc_load(RSC_FILENAME);
	if (!rsc_loaded) {
		sprintf(formAlertMessage,"[1][Impossible de charger|%s][D‚sol‚]",RSC_FILENAME);
		form_alert(1,formAlertMessage);
		goto end;
	}	
	if (!rsrc_gaddr(0, DLG_MAIN, &dlg_main)) {
		sprintf(formAlertMessage,"[1][Mauvais fichier RSC][D‚sol‚]",RSC_FILENAME);
		form_alert(1,formAlertMessage);
		goto end;
	}	
	dlg_main[LBL_FILENAME].ob_spec.free_string = fileToProcess;
	dlg_main[LBL_TRACKSIDE].ob_spec.free_string = trackSide;
	
	showDialog();
	graf_mouse(ARROW,(MFORM*)0L);
				
	do {
		clicked = form_do (dlg_main, 0);
		dlg_main[clicked].ob_state &= !SELECTED; /* D‚select */

		switch (clicked) {
			case BTN_LOAD:
				if( loadFile(fileToProcess) ) {
					OBJECT lbl_filename = dlg_main[LBL_FILENAME];
					strcpy(lbl_filename.ob_spec.free_string, fileToProcess);
				}
				showDialog();
				break;
			
			case BTN_WRITE:
				if (strlen(fileToProcess) > 0)
					/* Nothing on command line, so prompt user for ST file */
					st2disk(fileToProcess, updateProgress);				
				break;
		}

	} while (clicked != BTN_QUIT);
	
	hideDialog();
	goto end;

	{
		/* Selection of files provided on the command line */
	
		argumentNumber=argc;
	
		while (argumentNumber <= argc )
		{
			strcpy(fileToProcess,argv[argumentNumber]);
	
			printf("Processing %s...",argv[argumentNumber]);
			
			argumentNumber++;
		}
	}
		
end:
	if (rsc_loaded)
		rsrc_free();
	if (vdih > 0)
		v_clsvwk(vdih);
	appl_exit();
	return 0;
}

/***************************** Functions business ************************/


/* Converts ST file to MSA, similar name.
 * The function expects a file wit ST extension
 * Return values:
 * -1: Invalid file name
 * -2: Cannot open file
 * -3: Empty file
 * -4: Not enough memory to load ST file
 * -5: Error while reading ST file
 * -6: Action cancelled by user
 * -7: Impossible to read boot sector
 * -8: Error while writting
 * -9: Error while verifying
 * -10: Error while formatting
 */
int   st2disk(const char *filename, void (*updateProgress)(PROGRESS *p))
{
	int 	filenameLength;	/* Length of filename of ST file */
	long 	stSize;			/* Size of ST file */
	char 	*stbuffer;		/* Content of ST file */
	int		ret;			/* Return value */
	int		st;				/* Handle of the ST file */
	DISK_STRUCTURE	dsk;
	PROGRESS	progress;

	progress.format = progress.read = progress.write = 0.;
		
	/* Do basic checks */
	filenameLength = (int)strlen(filename);
	if( filenameLength < 4 )
		return -1;
	if( stricmp(&filename[filenameLength-3],".st") )
		return -1;

	/* Open file in memory */
	st = (int)Fopen(filename, FO_READ);
	if( st < 0 ) 
		return -2;
		
	/* Find size, allocate memory, and load file in memory */
	stSize = Fseek( 0L, st, 2 /* from end */ );
	if( stSize==0L )
		return -3;
	stbuffer=(char*)malloc(stSize);
	if( stbuffer==NULL )
		return -4;
	{
		long readBytes;	/* Number of bytes read from the file */
		/* Restore reader to beginning of file */
		Fseek( 0L, st, 0 /* from start */ );
		readBytes = Fread(st, stSize, stbuffer);
		progress.read = ((float)readBytes) / (float)stSize;
		if(updateProgress)
			updateProgress(&progress);
		if( readBytes != stSize )
		{
			ret = -5;
			goto end;
		}
	}
	
	/* Detect number of tracks/sectors */
	if (!analyzeDisk(&dsk,stbuffer,stSize))
	{
		form_alert(1,"[1][Impossible to read boot sector!][Cancel]");
		return -7;
	}

	if (2 == form_alert(2,"[1][Insert target floppy|in a: then click ok.][OK|Cancel]") )
		return -6;
		
#if 1		
	{
		int side;
		int track;
		int ret;
		long trackSize =  dsk.sectorSize * dsk.sectorsPerTrack;
		
		void *buf = (void*)malloc(12*600);
		
		for (track=0 ; track < dsk.nTracks ; track++) {
			for (side=0; side < dsk.nSides ; side++) {

				/* Format floppy */
				ret = Flopfmt( buf, (void*)0L, 0	/* a: */, 
					dsk.sectorsPerTrack, track, side,
					1, 0x87654321L, 0xe5e5);

				if (ret<0) {
					ret = -10;
					continue;
				}

				progress.format = ((float)(dsk.nSides*track+side)) / (float)(dsk.nSides*dsk.nTracks);
				if(updateProgress)
					updateProgress(&progress);

				/* Write data */
				ret = Flopwr( (void*)&(stbuffer[trackSize*(dsk.nSides*track+side)]), (void*)0L, 0	/* a: */, 
					1, track, side, dsk.sectorsPerTrack);					

				if (ret<0) {
					ret = -8;					
					goto done_writting;
				}

				progress.write = (dsk.nSides*track+side) / (dsk.nSides*dsk.nTracks);
				if(updateProgress)
					updateProgress(&progress);
			}
		}
done_writting:
		;
	}			
#endif

end:
	if( stbuffer )
		free(stbuffer);
	Fclose(st);
	return ret;
}

short lsbmsb2short(char lsb, char msb)
{
	short ret;
	ret = 0;
	((char*)&ret)[0] = msb;
	((char*)&ret)[1] = lsb;	
	return ret;
}

/* Analyze the structure of a disk from the information contained in the
 * boot sector. Return -1 if ok, 0 otherwize.
 * Note: I found that the number of sectors is often wrong ! */
int analyzeDisk(DISK_STRUCTURE *dsk, const char *bootsector, long size)
{
	dsk->nSectors = lsbmsb2short(bootsector[0x13],bootsector[0x14]);		
	dsk->sectorSize = lsbmsb2short(bootsector[0xb],bootsector[0xc]);
	dsk->nSides = bootsector[0x1a];
	/* Sanity check */
	if (dsk->nSides == 1 || dsk->nSectors > (12*82))
		dsk->nSides = 2;
	dsk->sectorsPerTrack = lsbmsb2short(bootsector[0x18],bootsector[0x19]);
	dsk->nTracks = (int)(size / dsk->sectorSize / dsk->nSides / dsk->sectorsPerTrack);

	return !(size == dsk->sectorSize * dsk->nSides * dsk->sectorsPerTrack * dsk->nTracks);
}

/*************************** Functions UI ********************************/

/* Affiche le formulaire principal */
void showDialog() {
	int x,y,w,h;

	/* Affiche le formulaire */

	form_center(dlg_main, &x, &y, &w, &h);
	form_dial(FMD_START, x, y, w, h, x, y, w, h);
	form_dial(FMD_GROW, x, y, 1, 1, x, y, w, h);	
	objc_draw(dlg_main, 0, 4, x, y, w, h);
}

/* Ferme le formulaire principal */
void hideDialog() {
	int x,y,w,h;

	form_center(dlg_main, &x, &y, &w, &h);
	form_dial(FMD_SHRINK, x, y, 1, 1, x, y, w, h);
	form_dial(FMD_FINISH, x, y, w, h, x, y, w, h);
}

/* Demande … l'utilisateur de choisir un fichier
 * file: pointeur vers un buffer (256 octets) destination du nom du fichier
 * retourne -1 si OK, 0 sinon
 */
int loadFile(char *file) {
	char selectedPath[256];
	char selectedFile[256];
	int  fsel_button;
	int  i;	

	strcpy(selectedPath,"c:\\");
	strcpy(selectedFile,"*.ST");
	fsel_input(selectedPath,selectedFile,&fsel_button);

	if( fsel_button == 0 )
		return 0;
		
	/* Strip filter from path */
	for ( i=(int)strlen(selectedPath)-1; selectedPath[i]!='\\' && selectedPath[i]!='/'; i--);
	selectedPath[++i] = 0;
	strcat(selectedPath,selectedFile);
	strcpy(file,selectedPath);
	return -1;
}

/* Update the progress information (bar, text) */
void updateProgress(PROGRESS *progress) {
	OBJECT progressBar = dlg_main[PBR_PROGRESS];
	OBJECT progressInfo = dlg_main[LBL_TRACKSIDE];
	int pxy[4];
	int x,y;
	
	/* Mets … jour la barre de progression */
	vsf_interior(vdih, FIS_PATTERN);
	vsf_color(vdih, 2);
	vsf_style(2, 2);
	objc_offset(dlg_main, PBR_PROGRESS, &pxy[0], &pxy[1]);
	pxy[2] = pxy[0] + (progressBar.ob_width) * progress->format;
	pxy[3] = pxy[1] + progressBar.ob_height-1;
	v_rfbox(vdih, pxy);
	
	/* Met … jour le texte */
	sprintf(
		progressInfo.ob_spec.free_string,
		"Piste %d, Face %d", progress->track, progress->side+1);
	objc_offset(dlg_main,LBL_TRACKSIDE,&x,&y);
	objc_draw(dlg_main,LBL_TRACKSIDE,1,
		x, y, progressInfo.ob_width, progressInfo.ob_height);
}