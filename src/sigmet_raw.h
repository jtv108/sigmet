/*
   -	sigmet_raw.h --
   -		Declarations for the sigmet_raw daemon and client.
   -
   .	Copyright (c) 2009 Gordon D. Carrie.
   .	All rights reserved.
   .
   .	Please send feedback to user0@tkgeomap.org
   .
   .	$Revision: 1.21 $ $Date: 2010/11/08 18:06:29 $
 */

#ifndef SIGMET_RAW_H_
#define SIGMET_RAW_H_

#include <stdio.h>
#include <proj_api.h>
#include "sigmet.h"

/* Default name for daemon socket */
#define SIGMET_RAWD_IN "sigmet.in"

/* Maximum number of arguments */
#define SIGMET_RAWD_ARGCX	512

void SigmetRaw_MkDDir(void);
char *SigmetRaw_GetDDir(void);
char *SigmetRaw_GetSock(void);
int SigmetRaw_Cmd(const char *);
void SigmetRaw_Start(int, char **);
int SigmetRaw_ProjInit(void);
enum Sigmet_Status SigmetRaw_SetProj(int, char **);
projPJ SigmetRaw_GetProj(void);
void SigmetRaw_SetImgSz(unsigned, unsigned);
void SigmetRaw_GetImgSz(unsigned *, unsigned *);
void SigmetRaw_SetImgAlpha(double);
double SigmetRaw_GetImgAlpha(void);
enum Sigmet_Status SigmetRaw_SetImgApp(char *);
char * SigmetRaw_GetImgApp(void);
void SigmetRaw_VolInit(void);
void SigmetRaw_VolFree(void);
enum Sigmet_Status SigmetRaw_GoodVol(char *, int, FILE *);
enum Sigmet_Status SigmetRaw_ReadHdr(char *, FILE *, int, struct Sigmet_Vol **);
enum Sigmet_Status SigmetRaw_ReadVol(char *, FILE *, int, struct Sigmet_Vol **);
void SigmetRaw_Keep(char *);
void SigmetRaw_Release(char *);
enum Sigmet_Status SigmetRaw_Delete(char *);
void SigmetRaw_VolList(FILE *);
enum Sigmet_Status SigmetRaw_Flush(void);
size_t SigmetRaw_MaxSize(size_t);

#endif
