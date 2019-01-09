#ifndef _IVEC_H
#define _IVEC_H

#ifdef  _cplusplus
extern "C" {
#endif

typedef struct _IVEC IVEC;

struct _IVEC
{
	char *m_chnMode;
	char *m_scoreMode;
	char *m_dbDir;
	char *m_modelDir;

	int m_nGauss;
	int m_featDim;
	int m_rank;
	int m_rank2;
	int m_rankLDA;
	float m_sparseThr;
	float m_statScale;

	double *m_TV;
	double *m_mean;
	double *m_iCov;		// inverse cov
	double *m_weight;
	double *m_TViCovTV;
	double *m_LDA;
	double *m_WCCN;
	double *m_LDA_WCCN;
};

void inverse(double *Matrix, int N);
int computeTViCovTV(IVEC *ivecP);
int computeIvec(IVEC *ivecP, float *feat, int nFrame, double *ivec);
int computeIvecExt(IVEC *ivecP, float *feat, int nFrame, double *ivec);
double logsumexp(double nums[], size_t ct);

IVEC* createIVEC();
int prepareIVEC(IVEC *ivecP);
void freeIVEC(IVEC *ivecP);

int readConfigIVEC(IVEC *ivecP, char *cfgFn);
int setOptionIVEC(IVEC *ivecP, char *key, char *val);
void showOptionIVEC(FILE *fp, IVEC *ivecP);
int getOptionIVECint(IVEC *ivecP, char *key);
float getOptionIVECfloat(IVEC *ivecP, char *key);
char* getOptionIVECstring(IVEC *ivecP, char *key);
int getIVECdim(IVEC *ivecP);

double cosScore(double *vec1, double *vec2, int dim);
double eucScore(double *vec1, double *vec2, int dim);

int makeProfile(IVEC *ivecP, float *feat, int nFrame, double *ivec, char *id);
int loadProfile(IVEC *ivecP, double *ivec, char *id);
double svDo(IVEC *ivecP, float *feat, int nFrame, double *ivecEnroll);

#ifdef _cplusplus
}
#endif

#endif

