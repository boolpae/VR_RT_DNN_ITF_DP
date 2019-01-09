/*! \file frontend_api.h
 * \brief LFrontEnd�� ����� �������.

 * ETRI LASER ���� �νı⿡ ���Ǵ� ����Ʈ���� ����� LFrontEnd��
 * ����ϱ� ���� ����� ��������̴�.
 * LFrontEnd�� ����ϱ� ���ؼ��� �Ʒ��� ���� ������� �ʿ��� �Լ��� ȣ���� �� �ִ�.
 * \verbatim
    LFrontEnd* pFront;
    pFront = createLFrontEnd();
    if ( pFront == NULL )
        return -1; // ���� ���� ����
    if ( readOptionLFrontEnd( pFront, "frontend.cfg" ) )
        return -1; // frontend.cfg ���� �ɼ��� ���������� ���� �� ����

    while( end_of_program )
    {
        resetLFrontEnd( pFront );
        while( end_of_utterance )
        {
            n = read_samples_from_audio_device( data ); // n�� �о�� ������ ����
            ret = stepFrameLFrontEnd( pFront, n, data, &m, out );
            send_extracted_feature_to_recognize( out, m );
        }
    } \endverbatim
 *
 * ���� ������ ����� ������ �����ϵ��� �Ѵ�.
 */

#ifndef __FRONTEND_API_H_
#define __FRONTEND_API_H_
#ifdef __cplusplus
extern "C" {
#endif

#define FRONTEND_OPTION_8KHZFRONTEND    0x0001

/// LFrontEnd ��ü ����.
/// ��ó���� �ʿ��� �޸� ���� ��ü���� �����ϰ� �ִ�.
/// �ϳ��� ���μ������� �������� ��ü�� ���� �ٸ� �ɼ����� ���۽�Ű�� �͵� �����ϸ�
/// �� ��ü�� ���������� �����Ѵ�.
typedef void LFrontEnd;

/// stepFrameLFrontEnd �Լ����� ��ȯ�ϴ� ��
typedef     enum { 
	noise = 0x00,  //!< ���� �Էµ� �������� ���� ������
	detecting = 0x01, //!< ���� �Էµ� �������� ���� ������
	detected = 0x02, //!< ���� �Էµ� �������� ������ ���� ������
	reset = 0x04,  //!< LFrontEnd�� reset�Ǿ���. ����� ���������� �ʹ� ª�Ƽ� ������ �ƴ� ������ ������ ���� �ش��.
    onset = 0x08, //!< ������ȣ�� ���۵Ǿ���. ������������ �ƴϳ�, ���ڴ��� �������� �� �� �ִ� que�� ����.
                  //!< �� ���¿����� ���ο� ���۸��� ���� �����ӿ� �ش��ϴ� Ư¡���͵� ���� ��µȴ�.
                  //!< �� ���̴� �ɼǿ� ���� �޶����� �뷫 ���� ������������ �ش��ϴ� Ư¡���Ͱ� ��µȴ�.
    offset = 0x10, //!< ������ȣ�� ����Ǿ���. ���ڴ��� ������ �� �� �ִ� que�� ����
    timeout = 0x20,
    restart = 0x40
} FSTATUS;

#define FRONTEND_COPYFEATUREVECTOR_ASIS           0
#define FRONTEND_COPYFEATUREVECTOR_MEANSUBTRACTED 1
#define FRONTEND_COPYFEATUREVECTOR_STATICONLY     2
#define FRONTEND_COPYFEATUREVECTOR_MFCC39         3
#define FRONTEND_COPYFEATUREVECTOR_MFCC52         4
#define FRONTEND_COPYFEATUREVECTOR_MFCC53         5


#ifdef _USRDLL
#define APITYPE __declspec (dllexport)
#else
#define APITYPE
#endif

/// LFrontEnd ��ü�� �����ϰ�, ���ۿ� �ʿ��� �޸𸮸� �Ҵ��Ѵ�.
///
/// @return ������ ��ü�� �����͸� ��ȯ�Ѵ�. ������ ������ NULL�� ��ȯ
///
APITYPE LFrontEnd	* 	createLFrontEnd( );


/// LFrontEnd ��ü�� �����ϰ�, ���ۿ� �ʿ��� �޸𸮸� �Ҵ��Ѵ�.
/// createLFrontEnd �Լ��� ������ ����� �ϳ� �ɼ��� �� �� �ִ�.
/// ���� ������ �ɼ��� �Ʒ��� ����.
///     if ( a_opt & FRONTEND_OPTION_8KHZFRONTEND ) �̸� 8k ����Ʈ���带 �����Ͽ� ��ȯ�Ѵ�.
///
/// @return ������ ��ü�� �����͸� ��ȯ�Ѵ�. ������ ������ NULL�� ��ȯ
///
APITYPE LFrontEnd	* 	createLFrontEndExt( int a_opt );

/// ��ȭ������ �ʱ�ȭ�� �����Ѵ�.
///
/// ������ �ɼǿ� ���� ���ۿ� �ʿ��� �������� �ʱⰪ ���� �� ���� �ʱ�ȭ �����Ѵ�.
/// �� ��ȭ���� ȣ���ؾ� �Ѵ�.
APITYPE int 	resetLFrontEnd( LFrontEnd* a_pFront );

/// ��ȭ������ �ʱ�ȭ�� �����Ѵ�. lookback �ɼ��� �� �� �ִ�.
///
/// resetLFrontEnd�Լ��� �⺻���� ����� �����ϸ�
/// Ư���� lookback �ɼ��� �� �� �ִ�.
/// lookback �Ķ���Ͱ� 0���� ũ�� �־��� ���, ������ ����� ���� ���� ������ �����Ҷ�,
/// �̹��� ����� ��ȭ�� ���κ� �� �� ���ڸ�ŭ�� frame�� �����ϰԵȴ�.
/// ���� EPD status�� detected�ΰ�쿡�� ����ȴ�. �׷��� ���� ��� resetLFrontEnd�� ����
APITYPE int 	resetLFrontEndExt( LFrontEnd* a_pFront, int a_framelen_lookback );

/// �־��� �Է½�ȣ�� ó���Ͽ� Ư¡���͸� ����Ѵ�.
///
/// @param a_pFront ������ LFrontEnd ��ü�� ������
/// @param a_ilen �Է½�ȣ�� ����. 0���� ���ų� ū ������ ����. 
///		(a_sig == NULL�� ��쿡�� 0�̾���Ѵ�. ) 
/// @param a_sig �Է½�ȣ �迭�� ������. NULL�� ��ȣ�� ���� �ǹ�.
/// @param a_olen ��º����� ����. 39�� �迭�� 10�� �ΰ�쿡�� 390�� ���
/// @param a_out ��º��͸� ���� �迭�� ������. �̸� �Ҵ� �Ǿ��־�� �Ѵ�.
///		���ο��� �޸� overflow�� check���� �����Ƿ� ����� �Ҵ� �Ǿ��־�� �Ѵ�
/// @return FSTATUS�� �� �ϳ� �̻��� or�Ǿ� ��µȴ�.
///     �Է½�ȣ�� 1 frame (160samples)�� ��� noise, detecting,
//      detected, reset, onset, offset �� �ϳ��� ��ȣ�� ������
//      �� ����� TIMEOUT �ɼǿ� ���� timeout�� or�Ǿ� ��µ� �� �ִ�.
//      �Է½�ȣ�� 2 frame�̻��� ��� �� �������� ������� ��� or�Ǿ� ��µǰ�,
//      �� event�� ������ �� �� ����. ���� ���� ������
//      ��µǴ� Ư¡������ ���� ������ �����ؼ� ó���ؾ��Ѵ�.
//
APITYPE int 	stepFrameLFrontEnd( LFrontEnd* a_pFront, 
					int a_ilen, short* a_sig, int* a_olen, float* a_out );

/// ������ �޸𸮸� �����Ѵ�.
///
APITYPE void 	closeLFrontEnd( LFrontEnd* a_pFront );

/// LFrontEnd�� ���۹���� �����Ѵ�.
/// ��밡���� �ɼ� �� ����Ʈ ���� ����� ������ �����Ѵ�.
/// @param a_pFront ����Ʈ���� ��ü�� ������
/// @param a_key �����ϰ��� �ϴ� �ɼ��� �̸�.
/// @param a_val �����ϰ��� �ϴ� �ɼ��� ��.
/// @return �����ϸ� 0��, �����ϸ� -1�� ��ȯ�Ѵ�. �ɼ��� �̸��� �� �� ���޵� ���, 
/// �Ǵ� �߸��� ���� ���޵� ��쿡 -1�� ��ȯ�Ѵ�.
///
APITYPE int 	setOptionLFrontEnd( LFrontEnd* a_pFront, char* a_key, char* a_val );

/// LFrontEnd�� ���� ������ �ɼ��� �����´�.
/// setOptionLFrontEnd() �Լ��� ����ϴ� Ű���� ����� �� �ִ�.
///
/// @param a_pFront ����Ʈ���� ��ü�� ������
/// @param a_key ���������� �ϴ� �ɼ�
/// @param a_val �ɼǰ��� ��ƿ� ���ڿ��� ������. �޸𸮴� ����� �Ҵ�Ǿ��ִٰ� �����ϰ�
/// ������ üũ�� ���� �ʴ´�. ȣ���ϴ� �ʿ��� ��ȯ�Ǵ� ���� �����Ͽ� ����� �޸𸮸� �⵵�� �Ѵ�.
/// @return �����ϸ� 0, �׷��� ������ -1�� ��ȯ�Ѵ�. 
///
APITYPE int 	getOptionLFrontEnd( LFrontEnd* a_pFront, char* a_key, char* a_val );

/// LFrontEnd�� ������ �Ķ���Ͱ��� ������ �� ����Ѵ�.
///
/// ���� FRONTEND_OUTDIM �� ����Ͽ� ���� �������� ��ȯ�Ǵ� Ư¡������ ������ �� �� �ִ�.
/// ���� ������ �� �ִ� �Ķ���ͷδ� FRONTEND_OUTDIM, EPD_PEAKVALUE ���� �ִ�.
///
/// @param a_pFront LFrontEnd ��ü�� ������
/// @param a_key �����������ϴ� �Ķ������ Ű��
/// @param a_val �Ķ���Ͱ��� ���� ������ ������ ������
/// @return �����ϸ� 0, �����ϸ� -1�� ��ȯ�Ѵ�. �����ϴ� ���� ������ Ű���� �־����� ���� ����̴�.
APITYPE int 	getIntValueLFrontEnd( LFrontEnd* a_pFront, char* a_key, int* a_val );

/// ���Ϸκ��� �ɼ��� �о�´�.
/// @param a_pFront ����Ʈ���� ��ü�� ������
/// @param a_fname ���ϸ�.
/// @return �����ϸ� 0, �����ϸ� 0�� �ƴѰ��� ��ȯ�Ѵ�.
APITYPE int 	readOptionLFrontEnd ( LFrontEnd* a_pFront, char* a_fname );

/// ������ �������� �����´�.
/// ������ ȣ��� stepFrameLFrontEnd �� �Ѱ��� ���� ��ȣ�� ��ũ���� �����Ѵ�.
/// ������ȣ�� ��ũ���� �ܼ� �����̸� �Ϲ������� ������ȣ�� envelope�� �������� �ִ밪����
/// �ణ ���� ������ �����ȴ�. ���� ������ ���� ������ 0 -- 32767 �̸� 
/// 32767���� ū ��쿡�� 32767 (2byte short�� ������ �ִ밪) ���� ���ѵȴ�.
/// @return ������ ��ũ��
APITYPE int 	getLFrameEnergy( LFrontEnd* a_pFront );

/// @return LFrontEnd�� ���� ���ڿ��� ��ȯ�Ѵ�. ���� ���ڿ��� �����Ͻÿ� ��������,
/// ������ �ɼǿ� ���� �����Ǿ����� ���� ���� �ִ�.
APITYPE char*	getLFrontEndVersion();

/// �������ĺ��� ������� ����� ���� ��ȣ ������ �����͸� ��ȯ�Ѵ�.
/// ���������� ���� �Է±����� ��� ��ȣ�� �����ϰ� �ִ� ������ �����͸� ��ȯ�Ѵ�.
/// �����ؼ� ȣ���� ��� ����ؼ� ó������ ��������� ��ȣ�� �������� �ȴ�.
/// ��, �� ������ ȣ���� ���, �� �����ӿ� �ش��ϴ� ����� ��ȣ�� �������� ���� �ƴ϶�,
/// �׻� ������ ���������� ��������� ��ȣ�� ��ȯ�Ѵ�.
/// @param a_pFront ����Ʈ���� ��ü�� ������
/// @param a_len ��ȯ�� �Է½�ȣ�� ����
/// @return ������ȣ�� ������
APITYPE short* getLDetectedSignal( LFrontEnd* a_pFront, unsigned int* a_len );

/// �������ĺ��� ������� ����� ���� ��ȣ�� �����Ͽ� ��ȯ�Ѵ�.
/// �����ؼ� ȣ���� ��� ����ؼ� ó������ ��������� ��ȣ�� �������� �ȴ�.
/// ��, �� ������ ȣ���� ���, �� �����ӿ� �ش��ϴ� ����� ��ȣ�� �������� ���� �ƴ϶�,
/// �׻� ������ ���������� ��������� ��ȣ�� ��ȯ�Ѵ�.
/// a_maxolen �� ��ȯ�� ��ȣ�� ���̺��� ���� ��� a_maxolen��ŭ�� ��ȯ�Ѵ�.
/// @param a_pFront ����Ʈ���� ��ü�� ������
/// @param a_osig ��ȣ�� �����Ͽ� ������ ������
/// @param a_maxolen a_osig�� �ִ� ����
/// @return ��ȯ�� ������ȣ�� ����
APITYPE unsigned int getLDetectedSignalExt( LFrontEnd* a_pFront, short* a_osig, unsigned int a_maxolen );

/// �������ĺ��� ������� ����� ���� ��ȣ�� ���̸� ��ȯ�Ѵ�.
APITYPE unsigned int getLDetectedSignalLength( LFrontEnd* a_pFront );

/// ���� ���ĺ��� ������� ����� ���� ������ ���̸� ��ȯ�Ѵ�.
/// �����ӱ��̰� �ƴ϶�, ���� �������� �����̴�. �� 39�� ���Ͱ� 10�������� ���
/// 390�� ��ȯ�Ѵ�.
APITYPE unsigned int getLFeatureLength( LFrontEnd* a_pFront );

/// ���� ���ĺ��� ������� ����� ���� ���͸� �����Ͽ� ��ȯ�Ѵ�.
/// �����ؼ� ȣ���� ��� ����ؼ� ó������ ��������� ���� ���͸� ��� 
/// �����Ͽ� ��ȯ�Ѵ�.
/// ��, �� ������ ȣ���� ���, �� �����ӿ� �ش��ϴ� ����� Ư¡���͸� �������� ���� �ƴ϶�,
/// �׻� ������ �������ĺ��� ������� ����� Ư¡���͸� ��ȯ�Ѵ�.
/// a_maxolen �� ��ȯ�� ��ȣ�� ���̺��� ���� ��� a_maxolen��ŭ�� ��ȯ�Ѵ�.
/// @param a_pFront ����Ʈ���� ��ü�� ������
/// @param a_ofeat ��ȣ�� �����Ͽ� ������ ������
/// @param a_maxolen a_osig�� �ִ� ����
/// @return ��ȯ�� ���ĺ����� ����
APITYPE unsigned int getLFeature( LFrontEnd* a_pFront, float* a_ofeat, unsigned int a_maxolen );

/// ���� ���ĺ��� ������� ����� ���� ���͸� �����ϴ� ������ �����͸� ��ȯ�Ѵ�.
/// MFCC_OPTION_STOREMFCCINMEMORY �ɼ��� Ȱ��ȭ�Ǿ� �־�� �Ѵ�. 
/// ��ȯ�� ���� ������ ���̴� getLFeatureLength �Լ��� �� �� �ִ�. 
/// @return ���ĺ��� ������ ������
APITYPE float* getLFeatureBuffer( LFrontEnd* a_pFront );

/// �������ĺ��� ������� ����� ���� ��ȣ ������ �����͸� ��ȯ�Ѵ�.
/// getLDetectedSignal() �Լ��� �����ϳ�, �̸��� ���������� �����Ͽ���.
APITYPE short* getLDetectedSignalBuffer( LFrontEnd* a_pFront, unsigned int* a_len );

/// �������ĺ��� ������� �Էµ� ���� ��ȣ ������ �����͸� ��ȯ�Ѵ�.
APITYPE short* getLInputSignalBuffer( LFrontEnd* a_pFront, unsigned int* a_len );

/// �������ĺ��� ������� �Էµ� ���� ��ȣ�� �����Ͽ� ��ȯ�Ѵ�.
/// @return ��ȯ�� ������ȣ�� ����
APITYPE unsigned int getLInputSignalExt( LFrontEnd* a_pFront, short* a_osig, unsigned int a_maxolen );

/// �������ĺ��� ������� �Էµ� ���� ��ȣ�� ���̸� ��ȯ�Ѵ�.
APITYPE unsigned int getLInputSignalLength( LFrontEnd* a_pFront );

/// ���� ���ۿ� �ִ� �Է½�ȣ �� �ֱ��� a_len ���ø�ŭ�� ��ȯ�Ѵ�.
/// @return ��ȯ�� ������ȣ�� ����
APITYPE unsigned int getLLastInputSignal( LFrontEnd* a_pFront, unsigned int a_len, short* a_osig );

/// ��������� ��ȣ�� �������� ������ ������ ������ ��ȯ�Ѵ�.
/// ������ ������� ���� ���¿��� ȣ���� ��� ������ �Էµ� ��ȣ�� �ð��� ��ȯ�Ѵ�.
/// ��µǴ� ���� ���� trailing silence�� ����Ͽ� ������ ������ ������ ��ȯ�ȴ�.
/// �ַ� ���� �Է��� ����� ��ġ �׽�Ʈ���� ���� ������� ������ ���ϱ� ���ؼ� ����Ѵ�.
/// @param a_pFront ����Ʈ���� ��ü�� ������
/// @param b ����� ��ȣ�� �������� ��� ���� ������
/// @param e ����� ��ȣ�� ������ ��� ���� ������
/// @return �׻� 0�� ��ȯ�Ѵ�. 
APITYPE int getLSpeechInterval( LFrontEnd* a_pFront, int* b, int* e );

/// ��������� ��ȣ�� �������� ������ ������ ������ ��ȯ�Ѵ�.
/// ������ ������� ���� ���¿��� ȣ���� ��� ������ �Էµ� ��ȣ�� �ð��� ��ȯ�Ѵ�.
/// ��µǴ� ���� ���� trailing silence�� ����Ͽ� ������ ������ ������ ��ȯ�ȴ�.
/// @param a_pFront ����Ʈ���� ��ü�� ������
/// @param b ����� ��ȣ�� �������� ��� ���� ������
/// @param e ����� ��ȣ�� ������ ��� ���� ������
/// @param makeup_end 1�� ��� ������ trailing silence�� ����Ͽ� �����Ѵ�.
/// @return �׻� 0�� ��ȯ�Ѵ�. 
APITYPE int getLSpeechIntervalExt( LFrontEnd* a_pFront, int* b, int* e, int makeup_end );

/// LFrontEnd�� ��� �ɼǸ���Ʈ �� ������ ���� stdout�� ����Ѵ�.
/// a_pFront �� NULL�� ��� �� �ɼ��� ����Ʈ ����
/// NULL�� �ƴ� ��� ���� ������ ���� ����Ѵ�.
/// ������ �ִ� ��� stderr�� ����Ѵ�.
APITYPE void getAllOptionsLFrontEnd( LFrontEnd* a_pFront );

/// ���� ���ۿ� ����� Ư¡���͸� �����Ͽ� ��ȯ�Ѵ�.
/// float array�� �����͸� ��ȯ�ϸ� ��ȯ�� array�� caller�� free�� �־�� �Ѵ�.
/// ��忡 ���� �Ʒ��� ���� �������� ��ȯ�Ѵ�.
///   - mode = FRONTEND_COPYFEATUREVECTOR_ASIS: ���ۿ� �ִ� ���� �״�� ��ȯ (eg. 39�� online cms)
///   - mode = FRONTEND_COPYFEATUREVECTOR_MEANSUBTRACTED: 
///      mode0���� static �κ��� mean�� �����Ͽ� ��ȯ (eg. 39�� gcms�� ����)
///   - mode = FRONTEND_COPYFEATUREVECTOR_STATICONLY:
///      cms�ϱ� ������ static feature�� ��ȯ (eg. 13�� static, no cms)
///   - mode = FRONTEND_COPYFEATUREVECTOR_MFCC39: 39�� global cms�� �����Ͽ� ��ȯ
///     (static mfcc -> global cms -> delta ����)
///   - mode = FRONTEND_COPYFEATUREVECTOR_MFCC52:
///      52�� global cms�� �����Ͽ� ��ȯ (static mfcc -> global cms -> dynamic ����)
///   - mode = FRONTEND_COPYFEATUREVECTOR_MFCC53:
///      53�� global cms�� �����Ͽ� ��ȯ (static mfcc -> global cms -> dynamic ����)
/// �ʿ��� ������ ���õ��� �ʾ� Ư¡ ���͸� ����� �� ���� ���,
///      *a_len�� 0���� �ϰ� NULL�� �����Ѵ�. 
/// mfcc�� online �� ���� ���� �̿��ϸ� cms, delta/dynamics�� �����ϰ� �ȴ�. 
/// �̶�  ������ �⺻ �ɼǿ� ���� ���ȴ�. ( POLEFILTERING ������� ���� ���� )
/// �Ϲ����� ��쿡 mode0�� �̿��Ͽ� ��ü Ư¡���͸� ���� ���Ŀ�
/// static �κ��� mean�� ����Ͽ� ���� ������ ����� �� �ִ�.
/// @param a_pFront LFrontEnd ��ü ������
/// @param mode ��������� ���
/// @param a_len ��¹����� ����( dimension * framelength )
/// @return �����Ҵ�� float array ������ (caller�� free�ؾ� ��)
APITYPE float* copyLFeatureVector( LFrontEnd* a_pFront, int mode, unsigned int* a_len );
#ifdef __cplusplus
}
#endif
#endif
