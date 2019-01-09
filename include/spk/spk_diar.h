#ifndef _SPK_DIAR_H
#define _SPK_DIAR_H

#include "frontend_api.h"
#include "ivec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SPKDIAR SPKDIAR;

struct _SPKDIAR
{
	IVEC *m_ivecP;

	int m_maxIter;
	int m_minDur;
	float m_selfLoop;
	float m_stopThr;
};

typedef struct _SPKDIARCFG SPKDIARCFG;

struct _SPKDIARCFG
{
	SPKDIAR *spkdiarP;
	int S;
	int miniBatch;
	char inFn[256];
	char outFn[256];
	LFrontEnd *pFront;
	int doSaveVAD;
	int doWF;
	int idGPU;
	char *job_name;
};

int featEx(LFrontEnd *pFront, FILE *ifp, int *num1, float *feat, int *num2, int *fstatus, int doGCMS);

SPKDIAR * createSPKDIAR();
int prepareSPKDIAR(int idGPU, SPKDIAR *spkdiarP);
void showOptionSPKDIAR(FILE *fp, SPKDIAR *spkdiarP);
int readConfigSPKDIAR(SPKDIAR *spkdiarP, char *cfgFn);
int setOptionSPKDIAR(SPKDIAR *spkdiarP, char *key, char *val);
int getOptionSPKDIARint(SPKDIAR *spkdiarP, char *key);
float getOptionSPKDIARfloat(SPKDIAR *spkdiarP, char *key);
char * getOptionSPKDIARstring(SPKDIAR *spkdiarP, char *key);
void freeSPKDIAR(int idGPU, SPKDIAR *spkdiarP);

double * spkDiar(FILE *logFp, int idGPU, SPKDIAR *spkdiarP, float *featFloat, int N, int S, int miniBatch, int totalFrame, int *fstatus);

int postProc(char *outFn, double *postprob, int *fstatus, int N, int S, int minFrame);
int saveVAD(char *outFn, int *fstatus, int totalFrame);
#ifdef __cplusplus
}
#endif

#endif
