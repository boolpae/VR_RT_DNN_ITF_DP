/** @file	liblaserdlnet.h
 * 	@brief	ESTkDLNet LASER API.
 *
 * 	Written by HWA JEON SONG (songhj@etri.re.kr)
 */

#ifndef	_LIB_LASER_DLNet_H
#define	_LIB_LASER_DLNet_H

#ifdef __cplusplus
extern "C" {
#endif

void *createDLNet( int useGPU, int idGPU, char *netFn, char *normFn, char *priorFn, float prWgt, int miniBatch );
void freeDLNet( void *p );
void calcDLNetSetLog( int t, void *p, int a_dim, float *a_Ot, int a_N, float *a_bjotP, float *a_bjotP2 );

void *createDLNetChild( void *, int useGPU, int idGPU, char *netFn, char *normFn, char *priorFn, float prWgt, int miniBatch );
void freeDLNetChild( void *p );

int  getDLNetKind( void *p );
int  getDLNetChunkSize( void );
int *getDLNetFrameOrder(void *p, int nframes);
float *getDLNetPostBuffer(void *p);
float *getDLNetPostBufferExt(void *p, int type);
int getDLNetNumOutNode(void *p);

#ifdef __cplusplus
}
#endif

#endif
