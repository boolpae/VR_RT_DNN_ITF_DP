/**
 * @file	vr.cc
 * @brief	STT(Speech to text)
 * @author	Kijeong Khil (kjkhil@itfact.co.kr)
 * @date	2016. 06. 30. 13:41:31
 * @see		vr_server.cc
 * @todo	ETRI API 중 스레드 세이프하지 않은 함수들이 존재하여 동기화할 필요가 있음
 */

#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fstream>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/lexical_cast.hpp>

#include "ETRIPP.h"

#include "vr.hpp"

using namespace itfact::vr::node;

#define THREAD_ID	std::this_thread::get_id()
#define LOG_INFO	__FILE__, __FUNCTION__, __LINE__
#define LOG_FMT		" [at %s (%s:%d)]"

static log4cpp::Category *job_log = NULL;

static struct {
	std::string laser_config;
	std::string sil_dnn;
	std::string chunking_filename;
	std::string tagging_filename;
	std::string user_dic;
} default_config = {
	.laser_config = "config/stt_laser.cfg",
	.sil_dnn = "config/sil_dnn.data",
	.chunking_filename = "chunking.release.bin",
	.tagging_filename = "tagging.release.bin",
	.user_dic = "user_dic.txt",
};

/**
 * @brief		Handle error
 * @author		Youngsoo Min (ysmin@itfact.co.kr)
 */
static int errorHandler(int a_errCode, char *a_errMsg) {
	job_log->error("[0x%X] %s(%d)", THREAD_ID, a_errMsg, a_errCode);
	return a_errCode;
}

/**
 * @brief		Load Laser module
 * @author		Youngsoo Min (ysmin@itfact.co.kr)
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 03. 18. 13:34
 * @retval		true	Success
 * @retval		false	Failure
 * @see			unloadLaserModule()
 */
bool VRServer::loadLaserModule() {
	logger = getLogger();
	job_log = logger;
	const itfact::common::Configuration *config = getConfig();

	unsigned long engine_core = static_cast<unsigned long>(DEFAULT_ENGINE_CORE);
	engine_core = config->getConfig<unsigned long>("stt.engine_core", engine_core);
	logger->info("load LASER(%s) module with %d cores", (useGPU ? "GPU" : "CPU"), engine_core);
	setLaserErrorHandleProc(NULL, (void *) errorHandler);
	setSLaserLBCores(engine_core);
	usleep(5 * 1024 * 1024);

	std::string image_path = "./";
	image_path = config->getConfig("stt.image_path", image_path.c_str());
	if (image_path.at(image_path.size() - 1) != '/')
		image_path.push_back('/');

	std::string chunking_file = std::string(image_path).
			append(config->getConfig("stt.chunking_filename", default_config.chunking_filename.c_str()));
	std::string tagging_file = std::string(image_path).
			append(config->getConfig("stt.tagging_filename", default_config.tagging_filename.c_str()));
	std::string user_dic_file = std::string(image_path).
			append(config->getConfig("stt.user_dic", default_config.user_dic.c_str()));

	std::string sil_dnn = config->getConfig("stt.sil_dnn", default_config.sil_dnn.c_str());
	std::FILE *file = fopen(sil_dnn.c_str(), "rb");
	if (!file) {
		logger->crit("cannot open %s", sil_dnn.c_str());
		return false;
	}
	std::shared_ptr<std::FILE> fp(file, fclose);

	if (config->isSet("stt.gpu_num")) {
		numGPU = std::stoi(config->getConfig("stt.gpu_num"));
	}
	else {
		numGPU = 1;
	}

	if (numGPU < 2) {
		if (config->isSet("stt.idGPU")) {
			idGPU = config->getConfig("stt.idGPU", idGPU);
		}
		else {
			idGPU = 0;
		}
	}

	if (numGPU < 2) {
		if ((master_laser = createMasterLaserDNN(
			const_cast<char *>(am_file.c_str()),
			const_cast<char *>(dnn_file.c_str()),
			prior_weight,
			const_cast<char *>(prior_file.c_str()),
			const_cast<char *>(norm_file.c_str()),
			mini_batch,
			(useGPU ? 1L : 0),
			idGPU,
			const_cast<char *>(fsm_file.c_str()),
			const_cast<char *>(sym_file.c_str()))) == NULL
			) {
			logger->crit("Fail to createMasterLaserDNN");
			return false;
		}

		std::string laser_config = config->getConfig("stt.laser_config", default_config.laser_config.c_str());
		int rc = readSLaserConfig(master_laser, const_cast<char *>(laser_config.c_str()));
		if (rc) {
			logger->crit("Fail to readSLaserConfig(%d): %s", rc, laser_config.c_str());
			unloadLaserModule();
			return false;
		}
	}
	else if (numGPU == 2) {
		if ((master_laser1 = createMasterLaserDNN(
			const_cast<char *>(am_file.c_str()),
			const_cast<char *>(dnn_file.c_str()),
			prior_weight,
			const_cast<char *>(prior_file.c_str()),
			const_cast<char *>(norm_file.c_str()),
			mini_batch,
			(useGPU ? 1L : 0),
			0,	// GPU ID
			const_cast<char *>(fsm_file.c_str()),
			const_cast<char *>(sym_file.c_str()))) == NULL
			) {
			logger->crit("Fail to createMasterLaserDNN");
			return false;
		}

		if ((master_laser2 = createMasterLaserDNN(
			const_cast<char *>(am_file.c_str()),
			const_cast<char *>(dnn_file.c_str()),
			prior_weight,
			const_cast<char *>(prior_file.c_str()),
			const_cast<char *>(norm_file.c_str()),
			mini_batch,
			(useGPU ? 1L : 0),
			1,	// GPU ID
			const_cast<char *>(fsm_file.c_str()),
			const_cast<char *>(sym_file.c_str()))) == NULL
			) {
			logger->crit("Fail to createMasterLaserDNN");
			return false;
		}

		std::string laser_config = config->getConfig("stt.laser_config", default_config.laser_config.c_str());
		int rc = readSLaserConfig(master_laser1, const_cast<char *>(laser_config.c_str()));
		if (rc) {
			logger->crit("Fail to readSLaserConfig(%d): %s", rc, laser_config.c_str());
			unloadLaserModule();
			return false;
		}
		
		rc = readSLaserConfig(master_laser2, const_cast<char *>(laser_config.c_str()));
		if (rc) {
			logger->crit("Fail to readSLaserConfig(%d): %s", rc, laser_config.c_str());
			unloadLaserModule();
			return false;
		}
	}

	/*
	if ((master_laser = createMasterLaserDLNet(
		const_cast<char *>(am_file.c_str()),
		const_cast<char *>(dnn_file.c_str()),
		prior_weight,
		const_cast<char *>(prior_file.c_str()),
		const_cast<char *>(norm_file.c_str()),
		mini_batch,
		(useGPU ? 1L : 0),
		idGPU,
		const_cast<char *>(fsm_file.c_str()),
		const_cast<char *>(sym_file.c_str()))) == NULL
		) {
		logger->crit("Fail to createMasterLaserDLNet");
		return false;
	}	

	std::string laser_config = config->getConfig("stt.laser_config", default_config.laser_config.c_str());
	int rc = readSLaserConfig(master_laser, const_cast<char *>(laser_config.c_str()));
	if (rc) {
		logger->crit("Fail to readSLaserConfig(%d): %s", rc, laser_config.c_str());
		unloadLaserModule();
		return false;
	}
	*/

	sil = (float *) malloc(sizeof(float) * (MAX_MINIBATCH + 128) * mfcc_size);
	if (sil == NULL) {
		logger->crit("%s" LOG_FMT, std::strerror(errno), LOG_INFO);
		unloadLaserModule();
		return false;
	}

	std::size_t rsize = 0;
	if ((rsize = fread(sil, sizeof(float), mfcc_size * 100, fp.get())) == 0) {
		logger->error("cannot read %s", sil_dnn.c_str());
		unloadLaserModule();
		return false;
	}

	for (int i = 1; i <= 10; ++i)
		memcpy(sil + i * (mfcc_size * 100), sil, sizeof(float) * mfcc_size * 100);

	// unsegment 초기화 
	if (getTotalWorkers("unsegment") > 0) {
		if (!Lat2cnWordNbestOutInit(
			const_cast<char *>(tagging_file.c_str()),
			const_cast<char *>(chunking_file.c_str()),
			const_cast<char *>(user_dic_file.c_str()),
			getTotalWorkers("unsegment"))
		) {
			logger->error("Fail to Lat2cnWordNbestOutInit");
			unloadLaserModule();
			return false;
		}
	}

	minimum_confidence = getConfig()->getConfig("stt.minimum_confidence", minimum_confidence);

	return true;
}

/**
 * @brief		Unload Laser module
 * @author		Youngsoo Min (ysmin@itfact.co.kr)
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 04. 19. 17:31
 * @see			loadLaserModule()
 */
void VRServer::unloadLaserModule() {
	if (numGPU < 2) {
		if (master_laser) {
			freeMasterLaserDNN(master_laser);
		}

		master_laser = NULL;
	}
	else if (numGPU == 2) {
		if (master_laser1) {
			freeMasterLaserDNN(master_laser1);
		}
		if (master_laser2) {
			freeMasterLaserDNN(master_laser2);
		}

		master_laser1 = NULL;
		master_laser2 = NULL;
	}

	if (sil)
		free(sil);
	closeSPLPostProc();

	//master_laser = NULL;
}

/**
 * @brief		특징 벡터로부터 최종 인식 결과를 가져옴
 * @author		Youngsoo Min (ysmin@itfact.co.kr)
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2017. 03. 03. 15:28:09
 * @param[in]	laser			Laser 엔진의 객체
 * @param[in]	index			입력 특징 벡터의 프레임 색인
 * @param[both]	last_position	마지막 종료 위치
 * @param[in]	feature_dim		
 * @param[in]	mfcc_size		
 * @param[in]	sil				
 * @param[out]	buffer			결과 저장
 * @return		Upon successful completion, a EXIT_SUCCESS is returned.\n
 				Otherwise,
 				a negative error code is returned indicating what went wrong.
 * @see			VRServer::stt()
 */
int itfact::vr::node::getFinalResult(
	Laser *laser,
	const std::size_t index,
	std::size_t &last_position,
	const std::size_t feature_dim,
	const std::size_t mfcc_size,
	float * const sil,
	std::string &buffer
) {
	int start = 0;
	int end = 0;
	char keyword[LENGTH_KEYWORD];
	float like = 0.0; // likelihood

	for (int i = 0; i < 40; ++i) {
		if (stepSARecFrameExt(laser, index + i, feature_dim, sil + i * mfcc_size) != EXIT_SUCCESS) {
			job_log->error("[0x%X] Fail to stepSARecFrameExt" LOG_FMT, THREAD_ID, LOG_INFO);
			buffer.push_back('.');
			return EXIT_FAILURE;
		}
	}

	// 단어 경계를 고려한 인식열에 대한 정렬이 완료된 최종 인식 결과를 가져옴
	char *resultP = getWBAdjustedResultSLaser(laser, index + 40, 1, true);
	if (resultP != NULL) {
		std::string tmp_resultP(resultP);
		tmp_resultP.push_back('\0');
		std::vector<std::string> v_result;
		boost::split(v_result, tmp_resultP, boost::is_any_of("\n"));
		for (auto &&line : v_result) {
			memset(keyword, 0x00, LENGTH_KEYWORD);
			if (sscanf(line.c_str(), "%d %d %s %f", &start, &end, keyword, &like) < 4)
				continue;
			char *tmp_keyword = keyword;

			// if (tmp_keyword[0] == '#')
			// 	++tmp_keyword;

			buffer.append(boost::lexical_cast<std::string>(start + last_position));
			buffer.push_back('\t');
			buffer.append(boost::lexical_cast<std::string>(end + last_position));
			buffer.push_back('\t');
			buffer.append(tmp_keyword, std::strlen(tmp_keyword));
			buffer.push_back('\t');
			buffer.append(boost::lexical_cast<std::string>(like));
			buffer.push_back('\n');
		}

		last_position += end;

		return EXIT_SUCCESS;
	} else {
		job_log->warn("[0x%X] Fail to getWBAdjustedResultSLaser", THREAD_ID);
		return EXIT_FAILURE;
	}
}

typedef struct {
	std::size_t start;
	std::size_t end;
	std::string keyword;
	double like;
} cell_t;
/**
 * @brief		특징 벡터로부터 중간 인식 결과를 가져옴
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2017. 03. 15. 23:20:57
 * @param[in]	laser			Laser 엔진의 객체
 * @param[in]	index			입력 특징 벡터의 프레임 색인
 * @param[both]	skip_position	무시할 위치 (0~)
 * @param[in]	last_position	마지막 종료 위치
 * @param[out]	buffer			결과 저장
 * @return		Upon successful completion, a EXIT_SUCCESS is returned.\n
 				Otherwise,
 				a negative error code is returned indicating what went wrong.
 */
int itfact::vr::node::getIntermediateResults(
	Laser *laser,
	const std::size_t index,
	std::size_t &skip_position,
	const std::size_t last_position,
	std::string &buffer,
	const float minimum_confidence,
	const std::size_t end_position
) {
	std::size_t start = 0;
	std::size_t end = 0;
	char keyword[LENGTH_KEYWORD];
	float like = 0.0; // likelihood

	char *result = getWBAdjustedResultSLaser(laser, index, 1, true);
	// char *result = getResultSLaser(laser, index, 1, true);
	if (result != NULL) {
		std::string str_result(result);
		str_result.push_back('\0');

		std::vector<std::string> v_result;
		std::vector<cell_t> temp_results;
		boost::split(v_result, str_result, boost::is_any_of("\n"));
		for (auto &&line : v_result) {
			memset(keyword, 0x00, LENGTH_KEYWORD);
			if (sscanf(line.c_str(), "%lu %lu %s %f", &start, &end, keyword, &like) < 4)
				continue;
			char *tmp_keyword = keyword;

			if (tmp_keyword[0] == '#')
				++tmp_keyword;

			if (std::strlen(tmp_keyword) == 0 || tmp_keyword[0] == '<')
				continue;

			if (start + last_position <= skip_position)
				continue;

			if (end_position < end + last_position)
				break;

			cell_t temp;
			temp.start = start + last_position;
			temp.end = end + last_position;
			temp.keyword = tmp_keyword;
			temp.like = like;
			temp_results.push_back(temp);
		}

		if (temp_results.size() > 0) {
			skip_position = temp_results[temp_results.size() - 1].start;
			for (size_t i = temp_results.size() - 1; i > 0; --i) {
				if (temp_results[i].like > minimum_confidence)
					break;
				skip_position = i > 0 ? temp_results[i - 1].start : 
					temp_results[i].start > 0 ? temp_results[i].start - 1 : temp_results[i].start;
			}
		}

		// while (temp_results.size() > 0) {
		// 	cell_t *temp = &temp_results[temp_results.size() - 1];
		// 	if (temp->like < 0)
		// 		temp_results.pop_back();
		// 	else
		// 		break;
		// }

		for (auto &&cell : temp_results) {
			buffer.append(boost::lexical_cast<std::string>(cell.start));
			buffer.push_back('\t');
			buffer.append(boost::lexical_cast<std::string>(cell.end));
			buffer.push_back('\t');
			buffer.append(cell.keyword);
			buffer.push_back('\t');
			buffer.append(boost::lexical_cast<std::string>(cell.like));
			buffer.push_back('\n');
		}

		return EXIT_SUCCESS;
	} else {
		job_log->warn("[0x%X] Fail to getResultSLaser", THREAD_ID);
		return EXIT_FAILURE;
	}
}

/**
* @brief		Speech to text
* @author		Youngsoo Min (ysmin@itfact.co.kr)
* @author		Kijeong Khil (kjkhil@itfact.co.kr)
* @date		2016. 04. 19. 17:32
* @param[in]	buffer		녹취 데이터
* @param[in]	bufferLen	녹취 데이터 길이
* @param[out]	result		STT 결과
* @return		Upon successful completion, a EXIT_SUCCESS is returned.\n
Otherwise,
a negative error code is returned indicating what went wrong.
* @see			VRServer::unsegment()
*/
int VRServer::stt(const short *buffer, const std::size_t bufferLen, long stt_thread_num, std::string &result) {
	std::size_t read_size = 80 * mini_batch;	// 8kHz Sampling Rate 포맷에서 80 Smaple이 1 frame에 해당함.
	std::size_t reset_period = getConfig()->getConfig("stt.reset_period", RESET_PERIOD);
	unsigned long i;
	int rc;

	LFrontEnd *_pFront = createLFrontEndExt(FRONTEND_OPTION_8KHZFRONTEND | FRONTEND_OPTION_DNNFBFRONTEND);
	if (_pFront == NULL) {
		logger->error("[0x%X] Fail to createLFrontEndExt" LOG_FMT, THREAD_ID, LOG_INFO);
		return EXIT_FAILURE;
	}
	std::shared_ptr<LFrontEnd> pFront(_pFront, closeLFrontEnd);

	if ((rc = readOptionLFrontEnd(pFront.get(), const_cast<char *>(frontend_config.c_str()))) != 0) {
		logger->error("[0x%X] Fail to readOptionFrontEnd: %s" LOG_FMT, THREAD_ID, frontend_config.c_str(), LOG_INFO);
		return EXIT_FAILURE;
	}

	if ((rc = setOptionLFrontEnd(pFront.get(), (char *)"FRONTEND_OPTION_DOEPD", (char *)"0")) != 0) {
		logger->error("[0x%X] Fail to setOptionLFrontEnd" LOG_FMT, THREAD_ID, LOG_INFO);
		return EXIT_FAILURE;
	}

	if ((rc = setOptionLFrontEnd(pFront.get(), (char *)"CMS_LEN_BLOCK", (char *)"0")) != 0) {
		logger->error("[0x%X] Fail to setOptionLFrontEnd" LOG_FMT, THREAD_ID, LOG_INFO);
		return EXIT_FAILURE;
	}

	/*
	Laser *_lP =
		createChildLaserDNN(master_laser,
			const_cast<char *>(am_file.c_str()),
			const_cast<char *>(dnn_file.c_str()),
			prior_weight,
			const_cast<char *>(prior_file.c_str()),
			const_cast<char *>(norm_file.c_str()),
			mini_batch, (useGPU ? 1L : 0), idGPU,
			const_cast<char *>(fsm_file.c_str()),
			const_cast<char *>(sym_file.c_str()));
	if (_lP == NULL) {
		logger->error("[0x%X] fail to createChildLaserDNN" LOG_FMT, THREAD_ID, LOG_INFO);
		return EXIT_FAILURE;
	}
	std::shared_ptr<Laser> lP(_lP, freeChildLaserDNN);
	*/

	Laser *_lP = NULL;

	if (numGPU < 2) {
		_lP =
			createChildLaserDNN(master_laser,
				const_cast<char *>(am_file.c_str()),
				const_cast<char *>(dnn_file.c_str()),
				prior_weight,
				const_cast<char *>(prior_file.c_str()),
				const_cast<char *>(norm_file.c_str()),
				mini_batch, (useGPU ? 1L : 0), idGPU,
				const_cast<char *>(fsm_file.c_str()),
				const_cast<char *>(sym_file.c_str()));
		if (_lP == NULL) {
			logger->error("[0x%X] fail to createChildLaserDNN" LOG_FMT, THREAD_ID, LOG_INFO);
			return EXIT_FAILURE;
		}
	}
	else if (numGPU == 2) {
		//idGPU = stt_thread_num % 2;
		idGPU = stt_job_count % 2;
		logger->debug("[0x%X] JOB_COUNT : %ld, GPU_ID : %ld" LOG_FMT, THREAD_ID, stt_job_count, idGPU, LOG_INFO);

		if (idGPU == 0) {
			_lP =
				createChildLaserDNN(master_laser1,
					const_cast<char *>(am_file.c_str()),
					const_cast<char *>(dnn_file.c_str()),
					prior_weight,
					const_cast<char *>(prior_file.c_str()),
					const_cast<char *>(norm_file.c_str()),
					mini_batch, (useGPU ? 1L : 0), idGPU,
					const_cast<char *>(fsm_file.c_str()),
					const_cast<char *>(sym_file.c_str()));
			if (_lP == NULL) {
				logger->error("[0x%X] fail to createChildLaserDNN" LOG_FMT, THREAD_ID, LOG_INFO);
				return EXIT_FAILURE;
			}
		}
		else {
			_lP =
				createChildLaserDNN(master_laser2,
					const_cast<char *>(am_file.c_str()),
					const_cast<char *>(dnn_file.c_str()),
					prior_weight,
					const_cast<char *>(prior_file.c_str()),
					const_cast<char *>(norm_file.c_str()),
					mini_batch, (useGPU ? 1L : 0), idGPU,
					const_cast<char *>(fsm_file.c_str()),
					const_cast<char *>(sym_file.c_str()));
			if (_lP == NULL) {
				logger->error("[0x%X] fail to createChildLaserDNN" LOG_FMT, THREAD_ID, LOG_INFO);
				return EXIT_FAILURE;
			}
		}

		stt_job_count++;
	}
	//std::shared_ptr<Laser> lP(_lP, freeChildLaserDLNet);
	//////////////////////////////////////////////////////////////////////////////////////////////////////


	// 특징 벡터
	std::size_t frame_size = mfcc_size * mini_batch;
	float *_feature_vector = (float *)malloc(sizeof(float) * (frame_size + mfcc_size * LDA_LEN_FRAMESTACK));
	if (_feature_vector == NULL) {
		logger->error("[0x%X] %s [at %s]", THREAD_ID, std::strerror(errno), "feature vector");
		return EXIT_FAILURE;
	}
	std::shared_ptr<float> feature_vector(_feature_vector, free);

	// 임시 버퍼
	float *_temp_buffer = (float *)malloc(sizeof(float) * frame_size);
	if (_temp_buffer == NULL) {
		logger->error("[0x%X] %s [at %s]", THREAD_ID, std::strerror(errno), "temp buffer");
		return EXIT_FAILURE;
	}
	std::shared_ptr<float> temp_buffer(_temp_buffer, free);

	//resetSLaser(lP.get());
	resetSLaser(_lP);
	resetLFrontEnd(pFront.get());

	// 녹취 파일을 읽어가며 처리
	std::size_t offset = 0;
	std::size_t index = 0;
	for (; offset < bufferLen; offset += read_size) {
		int fsize = 0;
		std::size_t rsize = read_size;
		std::size_t remain = bufferLen - offset;
		if (rsize > remain)
			rsize = remain;

		rc = stepFrameLFrontEnd(pFront.get(), rsize, const_cast<short *>(&buffer[offset]), &fsize, temp_buffer.get());
		logger->debug("[0x%X] stepFrameLFrontEnd(0x%x), read: %d, fsize: %d" LOG_FMT,
			THREAD_ID, rc, rsize, fsize, LOG_INFO);
		if (fsize <= 0)
			continue;

		for (i = 0; i < LDA_LEN_FRAMESTACK; ++i)
			memcpy(feature_vector.get() + i * mfcc_size, temp_buffer.get(), sizeof(float) * mfcc_size);
		memcpy(feature_vector.get() + i * mfcc_size, temp_buffer.get(), sizeof(float) * fsize);
		fsize = fsize + i * mfcc_size;

		// if (rc == noise)
		// 	continue;
		// else if (rc & timeout)
		// 	continue; // FIXME: Retry??

		std::size_t nf = fsize / mfcc_size;
		if (nf < mini_batch)
			memcpy(feature_vector.get() + nf * mfcc_size, sil, sizeof(float) * mfcc_size * (mini_batch - nf));

		for (i = 0; i < nf; ++i) {
			// 특징 벡터의 차원 값을 추가적으로 사용하여 프레임 기반의 탐색을 수행 (feature_dim = 128 * 600)
			//if (stepSARecFrameExt(lP.get(), index + i, feature_dim, feature_vector.get() + i * mfcc_size) != EXIT_SUCCESS)
			if (stepSARecFrameExt(_lP, index + i, feature_dim, feature_vector.get() + i * mfcc_size) != EXIT_SUCCESS)
				return EXIT_FAILURE;
		}

		index += nf;
		break;
	}

	offset += read_size;
	logger->debug("[0x%X] index: %d, offset: %d" LOG_FMT, THREAD_ID, index, offset, LOG_INFO);

	std::size_t last_position = 0;
	int fsize = 0;
	for (; offset < bufferLen; offset += read_size) {
		std::size_t rsize = read_size;
		std::size_t remain = bufferLen - offset;
		if (rsize > remain)
			rsize = remain;

		// 음성 신호로부터 특징 벡터 출력
		rc = stepFrameLFrontEnd(pFront.get(), rsize, const_cast<short *>(&buffer[offset]), &fsize, feature_vector.get());
		// if (rc)
		//logger->debug("[0x%X] stepFrameLFrontEnd(0x%x), read: %d, fsize: %d" LOG_FMT, THREAD_ID, rc, rsize, fsize, LOG_INFO);

		if (fsize <= 0)
			continue;

		// if (rc == noise)
		// 	continue;
		// else if (rc & timeout)
		// 	continue; // FIXME: Retry??

		// noise // 노이즈 구간
		// detecting // 음성 프레임 구간
		// detected // 음성 종료 구간
		// reset // LFrontEnd가 리셋됨. 검출된 음성 구간이 짧음
		// onset // 음성 신호 시작
		// offset // 음성 신호 종료
		// timeout
		// restart

		// if (rc & detecting || rc & detected) {
		// 	// 음성 프레임 구간
		// } else if (rc & onset || rc & offset) {
		// 	// 음성 신호 구간
		// } else if (rc & reset) {
		// 	logger->debug("[0x%X] LFrontEnd was reset" LOG_FMT, THREAD_ID, LOG_INFO);
		// 	// if (resetSLaser(lP.get())) {
		// 	// 	logger->error("[0x%X] Fail to resetSLaser" LOG_FMT, THREAD_ID, LOG_INFO);
		// 	// 	return EXIT_FAILURE;
		// 	// }

		// 	if (read_size > remain) {
		// 		// 녹취 파일의 끝이므로 분석 시도
		// 	} else {
		// 		// 검출된 음성이 짧으므로 모아서 처리
		// 	}
		// }

		std::size_t nf = fsize / mfcc_size;
		if (nf < mini_batch)
			memcpy(feature_vector.get() + nf * mfcc_size, sil, sizeof(float) * mfcc_size * (mini_batch - nf));

		for (i = 0; i < nf; ++i) {
			// 특징 벡터의 차원 값을 추가적으로 사용하여 프레임 기반의 탐색을 수행 (feature_dim = 128 * 600)
			//if (stepSARecFrameExt(lP.get(), index + i, feature_dim, feature_vector.get() + i * mfcc_size) != EXIT_SUCCESS)
			if (stepSARecFrameExt(_lP, index + i, feature_dim, feature_vector.get() + i * mfcc_size) != EXIT_SUCCESS)
				return EXIT_FAILURE;
		}
		index += nf;

		if (index > reset_period) {
			//if (getFinalResult(lP.get(), index, last_position, feature_dim, mfcc_size, sil, result) != EXIT_SUCCESS)
			if (getFinalResult(_lP, index, last_position, feature_dim, mfcc_size, sil, result) != EXIT_SUCCESS)
				continue;

			logger->debug("[0x%X] Reset LASER (last position: %lu)" LOG_FMT, THREAD_ID, last_position, LOG_INFO);
			// reallocSLaser(lP.get());

			//if (resetSLaser(lP.get())) {
			if (resetSLaser(_lP)) {
				logger->error("[0x%X] Fail to resetSLaser" LOG_FMT, THREAD_ID, LOG_INFO);
				return EXIT_FAILURE;
			}

			index = 0;
		}
	}

	// flush internal buffer (static + zero padding -> dynamic feat)
	rc = stepFrameLFrontEnd(pFront.get(), 0, NULL, &fsize, feature_vector.get());
	logger->debug("[0x%X] stepFrameLFrontEnd(0x%x), fsize: %d" LOG_FMT, THREAD_ID, rc, fsize, LOG_INFO);
	std::size_t nf = fsize / mfcc_size;
	for (i = 0; i < nf; ++i) {
		//if (stepSARecFrameExt(lP.get(), index + i, feature_dim, feature_vector.get() + i * mfcc_size) != EXIT_SUCCESS)
		if (stepSARecFrameExt(_lP, index + i, feature_dim, feature_vector.get() + i * mfcc_size) != EXIT_SUCCESS)
			return EXIT_FAILURE;
	}
	index += nf;

	if (index > 0) {
		logger->debug("[0x%X] partial backtracking size: %d" LOG_FMT, THREAD_ID, index * mfcc_size, LOG_INFO);
		//if (getFinalResult(lP.get(), index, last_position, feature_dim, mfcc_size, sil, result) != EXIT_SUCCESS)
		if (getFinalResult(_lP, index, last_position, feature_dim, mfcc_size, sil, result) != EXIT_SUCCESS)
			return EXIT_FAILURE;
	}
	

	if (_lP != NULL) {
		resetSLaser(_lP);
		freeChildLaserDNN(_lP);
	}
	
	return EXIT_SUCCESS;
}

/**
 * @brief		Unsegment 
 * @author		Youngsoo Min (ysmin@itfact.co.kr)
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 04. 20. 13:33
 * @param[in]	cell_data	셀 데이터 
 * @param[out]	result		Unsegment 결과
 * @return		Upon successful completion, a EXIT_SUCCESS is returned.\n
 				Otherwise,
 				a negative error code is returned indicating what went wrong.
 * @see			VRServer::stt()
 */
int VRServer::unsegment(const std::string &cell_data, std::string &result) {
	char tmp_buf1[LENGTH_SYNTAX];
	unsigned long start = 0;
	unsigned long end = 0;
	float like = 0.0; // likelihood
	char keyword[LENGTH_KEYWORD];

	std::vector<std::string> line;
	std::string tmp_buf;
	boost::split(line, cell_data, boost::is_any_of("\n"));
	for (size_t i = 0; i < line.size(); ++i) {
		memset(keyword, 0x00, LENGTH_KEYWORD);
		if (sscanf(line[i].c_str(), "%lu\t%lu\t%s\t%f", &start, &end, keyword, &like) < 3)
			continue;

		char *tmp_keyword = keyword;
		if (strncmp(tmp_keyword, "<s>", 3) == 0 || strncmp(tmp_keyword, "</s>", 4) == 0)
			continue;

		if (tmp_keyword[0] == '#')
			++tmp_keyword;

		tmp_buf.append(tmp_keyword);
		tmp_buf.push_back(' ');

		if (tmp_buf.size() > MAX_SYNTAX) {
			memset(tmp_buf1, 0x00, LENGTH_SYNTAX);
			SPLPostProc(const_cast<char *>(tmp_buf.c_str()), tmp_buf1);
			// SPLPostProcSentenceSegment_POS(tmp_buf1, tmp_buf2);
			result.append(tmp_buf1);
			tmp_buf = "";
		}
	}

	if (tmp_buf.size() > 0) {
		SPLPostProc(const_cast<char *>(tmp_buf.c_str()), tmp_buf1);
		// SPLPostProcSentenceSegment_POS(tmp_buf1, tmp_buf2);
		result.append(tmp_buf1);
	}

	// logger->debug("[0x%X] unsegment result : %s" LOG_FMT, THREAD_ID, result.c_str(), LOG_INFO);

	return EXIT_SUCCESS;
}

/**
 * @brief		Unsegment with time
 * @author		Youngsoo Min (ysmin@itfact.co.kr)
 * @date		2016. 08. 03. 18:50:34
 * @param[in]	cell_data	셀 데이터 
 * @param[out]	result		Unsegment 결과
 * @return		Upon successful completion, a EXIT_SUCCESS is returned.\n
 				Otherwise,
 				a negative error code is returned indicating what went wrong.
 * @see			VRServer::stt()
 */
int VRServer::unsegment_with_time(const std::string &mlf_file, const std::string &unseg_file, int pause) {
	int result = 0;

	if ((result = SPLPostProcMLF(const_cast<char *>(mlf_file.c_str()),
		const_cast<char *>(unseg_file.c_str()))) != 1) {
		logger->error("[0x%X] Fail to SPLPostProcMLF" LOG_FMT, THREAD_ID, LOG_INFO);
		return EXIT_FAILURE;
	}

	/*
	if ((result = SPLPostProcMLF(const_cast<char *>(mlf_file.c_str()),
								 const_cast<char *>(unseg_file.c_str()))) != 1) {
		logger->error("[0x%X] Fail to SPLPostProcMLF" LOG_FMT, THREAD_ID, LOG_INFO);
		return EXIT_FAILURE;
	}
	*/

	return EXIT_SUCCESS;
}

// v: 0, 1 = 0
// l: 0 = 0
// rate: 0, 0xF = 0
const short bitrate[4][4][16] = {
	{ // version: 2.5
		{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},	// layer: reserved
		{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},	// layer: III
		{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},	// layer: II
		{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}	// layer: I
	}, { // reserved
		{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},	// layer: reserved
		{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},	// layer: III
		{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},	// layer: II
		{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}	// layer: I
	}, { // version: 2
		{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},	// layer: reserved
		{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},	// layer: III
		{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},	// layer: II
		{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0}	// layer: I
	}, { // version: 1
		{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},	// layer: reserved
		{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},	// layer: III
		{0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},	// layer: II
		{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0}	// layer: I
	}
};

const long sampling_rate[4][4] = {
	{11025, 12000, 8000, 0}, // version: 2.5
	{0, 0, 0, 0}, // reserved
	{22050, 24000, 16000, 0}, // version: 2
	{44100, 48000, 32000, 0}, // version: 1
};

/**
 * @brief		ssp 
 * @author		Youngsoo Min (ysmin@itfact.co.kr)
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 08. 03. 18:00
 * @param[in]	cell_data	셀 데이터 
 * @param[out]	result		Unsegment 결과
 * @return		Upon successful completion, a EXIT_SUCCESS is returned.\n
 				Otherwise,
 				a negative error code is returned indicating what went wrong.
 * @see			VRServer::stt()
 */
int VRServer::ssp(const std::string &mlf_file, std::string &buf) {
	int result = 0;
	typedef struct s_t {
		char s[16];
		float value;
	} S_T;

	/* cls file format */
	typedef struct cls_t {
		int ts;
		int te;
		S_T first;
		S_T second;
		char str[4096];
	} CLS_T;

	std::string cls_file(mlf_file);
	cls_file.append(".cls");

	// convert mlf to cls using tool (MlfClassify_new.exe)
	std::string default_pathname = "./bin/MlfClassify_new.exe";
	if (getConfig()->isSet("ssp.util")) {
		char *part = NULL;
		size_t len = 0;
		ssize_t read;

		std::string cmd(getConfig()->getConfig("ssp.util"));
		cmd.push_back(' ');
		cmd.append(mlf_file);
		cmd.push_back('>');
		cmd.append(cls_file);

		job_log->debug("[0x%X] %s", THREAD_ID, cmd.c_str());
		int rc = std::system(cmd.c_str());
		if (rc) {
			job_log->error("[0x%X] Error(%d) occurred during execution '%s'", THREAD_ID, rc, cmd.c_str());
			return EXIT_FAILURE;
		}

		std::FILE *fp = fopen(cls_file.c_str(), "rt");
		if (!fp) {
			job_log->error("[0x%X] Cannot open file: %s", THREAD_ID, cls_file.c_str());
			return EXIT_FAILURE;
		}
		std::shared_ptr<FILE> fi_cls(fp, fclose);

		buf = "";
		while ((read = getline(&part, &len, fi_cls.get())) != -1) {
			buf.append(part);
		}
	} else {
		std::string cmd(getConfig()->getConfig("ssp.util", default_pathname.c_str()));
		cmd.append(" ./out ");
		cmd.append(mlf_file);
		cmd.push_back(' ');
		cmd.append(cls_file);
		cmd.append(" 5 500");
		if (std::system(cmd.c_str()))
			return EXIT_FAILURE;

		// read cls and extract data
		int first_cmp = 0;

		char *part = NULL;
		size_t len = 0;
		ssize_t read;

		char *str_p = NULL;
		char ss1[64];
		char ss2[64];

		CLS_T line;

		std::FILE *fp = fopen(cls_file.c_str(), "rt");
		if (!fp) {
			job_log->error("[0x%X] Cannot open file: %s", THREAD_ID, cls_file.c_str());
			return EXIT_FAILURE;
		}
		std::shared_ptr<FILE> fi_cls(fp, fclose);

		while ((read = getline(&part, &len, fi_cls.get())) != -1) {
			str_p = strstr(part, "str= ");
			strncpy(line.str, str_p + 5, strlen(str_p) - 5);
			line.str[strlen(str_p) - 5] = '\0';
			sscanf(part, "ts= %d, te= %d, [ %s %s ], str= %*s", &line.ts, &line.te, ss1, ss2);

			str_p = strchr(ss1, '=');
			strncpy(line.first.s, ss1, strlen(ss1) - strlen(str_p));
			line.first.s[strlen(ss1) - strlen(str_p)] = '\0';
			sscanf(str_p + 1, "%f", &line.first.value);

			str_p = strchr(ss2, '=');
			strncpy(line.second.s, ss2, strlen(ss2) - strlen(str_p));
			line.second.s[strlen(ss2) - strlen(str_p)] = '\0';
			sscanf(str_p + 1, "%f", &line.second.value);

			if ((first_cmp = strcmp(line.first.s, "s0")) == 0) {
				/* Nothing */
			} else {
				buf.append(boost::lexical_cast<std::string>(line.ts));
				buf.push_back('\t');
				buf.append(boost::lexical_cast<std::string>(line.te));
				buf.push_back('\t');
				buf.append(line.first.s);
				buf.push_back('\n');
			}
		}

		if (part != NULL) {
			free(part);
			part = NULL;
		}
	}

	if ((result = access(cls_file.c_str(), F_OK)) == 0) {
		unlink(cls_file.c_str());
	}

	return EXIT_SUCCESS;
}

/**
 * @brief		Check WAVE Format
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 07. 20. 10:49:33
 * @param[in]	data		WAVE data
 * @param[in]	data_size	WAVE data size
 * @return		Upon successful completion, a EXIT_SUCCESS is returned.\n
 				Otherwise,
 				a negative error code is returned indicating what went wrong.
 */
enum WAVE_FORMAT VRServer::check_wave_format(const short *data, const size_t data_size) {
	const char *cdata = (const char *) data;

	// STANDARD_WAVE "RIFF"
	// MPEG			 "ID3" METADATA_SIZE(4byte)

	if (data_size < 4)
		return UNKNOWN_FORMAT;

	if (std::strncmp(cdata, "RIFF", 4) == 0) {
		std::string metadata(cdata + 8 , 7);
		short channels = *(short *) (cdata + 22);
		int32_t sample_rate = *(int32_t *) (cdata + 24);
		short bits = *(short *) (cdata + 34);

		if (std::strncmp(cdata + 8, "WAVEfmt", 7) == 0) {
			job_log->debug("[0x%X] Standard WAVE Fromat", THREAD_ID);
			if (sample_rate == 8000 && bits == 16 && channels == 1)
				return STANDARD_WAVE;
			else if (channels == 2)
				return WAVE_2CH;
		}

		job_log->debug("[0x%X] Sample rate: %d, Bits: %d, Channels: %d", THREAD_ID, sample_rate, bits, channels);
		return WAVE;
	}

	const unsigned char *ucdata = (const unsigned char *) cdata;

	// http://mpgedit.org/mpgedit/mpeg_format/MP3Format.html
	// AAAAAAAA AAABBCCD EEEEFFGH IIJJKLMM
	// 11111111 11110011 01000000 11000100
	// F: Sampling Frequency index
	// H: Channel
	if (ucdata[0] == 0xFF && (ucdata[1] & 0xE0) == 0xE0) {
		// 00 - MPEG Version 2.5 (unofficial)
		// 10 - MPEG Version 2 (ISO/IEC 13818-3)
		// 11 - MPEG Version 1 (ISO/IEC 11172-3)
		short version = (ucdata[1] & 0x18) >> 3;

		// 01 - Layer III
		// 10 - Layer II
		// 11 - Layer I
		short layer = (ucdata[1] & 0x06) >> 1;
		if (version != 1 && layer != 0) {
			job_log->debug("[0x%X] MPEG Version %s Layer %s", THREAD_ID,
				version == 2 ? "MPEG Version 2(ISO/IEC 13818-3)" : (version == 3 ? "1(ISO/IEC 11172-3)" : "2.5(unofficial)"),
				layer == 1 ? "III" : (layer == 2 ? "II" : "I")
				);

			job_log->debug("[0x%X] Bitrate: %d kbps", THREAD_ID, bitrate[version][layer][(ucdata[2] & 0xF0) >> 4]);
			job_log->debug("[0x%X] Sampling rate frequency: %d Hz", THREAD_ID, sampling_rate[version][(ucdata[2] & 0x0C) >> 2]);
			switch ((ucdata[3] & 0xC0) >> 6) {
				case 0:
					job_log->debug("[0x%X] Channel Mode: Stereo", THREAD_ID);
					break;
				case 1:
					job_log->debug("[0x%X] Channel Mode: Joint stereo (Stereo)", THREAD_ID);
					break;
				case 2:
					job_log->debug("[0x%X] Channel Mode: Dual channel (2 mono channels)", THREAD_ID);
					break;
				case 3:
					job_log->debug("[0x%X] Channel Mode: Single channel (Mono)", THREAD_ID);
					return MPEG;
			}

			return MPEG_2CH;
		}
	}

	if (std::strncmp(cdata, "ID3", 3) == 0) {
		job_log->debug("[0x%X] MPEG ID3 version %d, %d", THREAD_ID, ucdata[3], ucdata[4]);
		return MPEG_ID3;
	}

	return UNKNOWN_FORMAT;
}

/**
 * @brief		채널 생성
 * @details		실시간 STT를 위한 채널 생성
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2017. 03. 06. 18:08:59
 * @param[in]	call_id	Call ID
 * @return		Upon successful completion, a EXIT_SUCCESS is returned.\n
 				Otherwise,
 				a negative error code is returned indicating what went wrong.
 * @see			VRServer::close_channel()
 */
int VRServer::create_channel(const std::string &call_id, const int stt_job_count) {
	int rc;
	std::size_t reset_period = getConfig()->getConfig("realtime.reset_period", RESET_PERIOD);

	job_log->debug("[0x%X] Create channel" LOG_FMT, THREAD_ID, LOG_INFO);

	LFrontEnd *_pFront = createLFrontEndExt(FRONTEND_OPTION_8KHZFRONTEND | FRONTEND_OPTION_DNNFBFRONTEND);
	if (_pFront == NULL) {
		job_log->error("[0x%X] Fail to createLFrontEndExt" LOG_FMT, THREAD_ID, LOG_INFO);
		return EXIT_FAILURE;
	}
	std::shared_ptr<LFrontEnd> frontend(_pFront, closeLFrontEnd);
	
	if ((rc = readOptionLFrontEnd(frontend.get(), const_cast<char *>(frontend_config.c_str()))) != 0) {
		job_log->error("[0x%X] Fail to readOptionFrontEnd: %s" LOG_FMT, THREAD_ID, frontend_config.c_str(), LOG_INFO);
		return EXIT_FAILURE;
	}

	if ((rc = setOptionLFrontEnd(frontend.get(), (char *)"FRONTEND_OPTION_DOEPD", (char *)"0")) != 0) {
		job_log->error("[0x%X] Fail to setOptionLFrontEnd" LOG_FMT, THREAD_ID, LOG_INFO);
		return EXIT_FAILURE;
	}

	if ((rc = setOptionLFrontEnd(frontend.get(), (char *)"CMS_LEN_BLOCK", (char *)"0")) != 0) {
		job_log->error("[0x%X] Fail to setOptionLFrontEnd" LOG_FMT, THREAD_ID, LOG_INFO);
		return EXIT_FAILURE;
	}
	/*
	Laser *_lP = createChildLaserDNN(
		masterLaserP,
		const_cast<char *>(am_file.c_str()),
		const_cast<char *>(dnn_file.c_str()),
		prior_weight,
		const_cast<char *>(prior_file.c_str()),
		const_cast<char *>(norm_file.c_str()),
		mini_batch, (useGPU ? 1L : 0), idGPU,
		const_cast<char *>(fsm_file.c_str()),
		const_cast<char *>(sym_file.c_str())
	);
	if (_lP == NULL) {
		job_log->error("[0x%X] fail to createChildLaserDNN" LOG_FMT, THREAD_ID, LOG_INFO);
		return EXIT_FAILURE;
	}
	std::shared_ptr<Laser> child_laser(_lP, freeChildLaserDNN);
	*/
	Laser *_lP = NULL;

	if (numGPU < 2) {
		_lP =
			createChildLaserDNN(master_laser,
				const_cast<char *>(am_file.c_str()),
				const_cast<char *>(dnn_file.c_str()),
				prior_weight,
				const_cast<char *>(prior_file.c_str()),
				const_cast<char *>(norm_file.c_str()),
				mini_batch, (useGPU ? 1L : 0), idGPU,
				const_cast<char *>(fsm_file.c_str()),
				const_cast<char *>(sym_file.c_str()));
		if (_lP == NULL) {
			logger->error("[0x%X] fail to createChildLaserDNN" LOG_FMT, THREAD_ID, LOG_INFO);
			return EXIT_FAILURE;
		}
		logger->debug("[0x%X] GPU_ID : %ld, _lP(0x%X)" LOG_FMT, THREAD_ID, idGPU, _lP, LOG_INFO);
	}
	else if (numGPU == 2) {
		//idGPU = stt_thread_num % 2;
		idGPU = stt_job_count % 2;
		logger->debug("[0x%X] JOB_COUNT : %ld, GPU_ID : %ld" LOG_FMT, THREAD_ID, stt_job_count, idGPU, LOG_INFO);

		if (idGPU == 0) {
			_lP =
				createChildLaserDNN(master_laser1,
					const_cast<char *>(am_file.c_str()),
					const_cast<char *>(dnn_file.c_str()),
					prior_weight,
					const_cast<char *>(prior_file.c_str()),
					const_cast<char *>(norm_file.c_str()),
					mini_batch, (useGPU ? 1L : 0), idGPU,
					const_cast<char *>(fsm_file.c_str()),
					const_cast<char *>(sym_file.c_str()));
			if (_lP == NULL) {
				logger->error("[0x%X] fail to createChildLaserDNN" LOG_FMT, THREAD_ID, LOG_INFO);
				return EXIT_FAILURE;
			}
		}
		else {
			_lP =
				createChildLaserDNN(master_laser2,
					const_cast<char *>(am_file.c_str()),
					const_cast<char *>(dnn_file.c_str()),
					prior_weight,
					const_cast<char *>(prior_file.c_str()),
					const_cast<char *>(norm_file.c_str()),
					mini_batch, (useGPU ? 1L : 0), idGPU,
					const_cast<char *>(fsm_file.c_str()),
					const_cast<char *>(sym_file.c_str()));
			if (_lP == NULL) {
				logger->error("[0x%X] fail to createChildLaserDNN" LOG_FMT, THREAD_ID, LOG_INFO);
				return EXIT_FAILURE;
			}
		}

	}
	std::shared_ptr<Laser> child_laser(_lP, freeChildLaserDNN);

	// 특징 벡터
	std::size_t frame_size = mfcc_size * mini_batch;
	float *_feature_vector = (float *) malloc(sizeof(float) * (frame_size + mfcc_size * LDA_LEN_FRAMESTACK)); 
	if (_feature_vector == NULL) {
		job_log->error("%s [at %s]", std::strerror(errno), "feature vector");
		return EXIT_FAILURE;
	}
	std::shared_ptr<float> feature_vector(_feature_vector, free);

	// resetSLaser(_lP);//child_laser.get()); 
	// resetLFrontEnd(frontend.get());

	std::shared_ptr<RealtimeSTT> realtime_stt =
		std::make_shared<RealtimeSTT>(
			call_id, feature_vector, frontend, child_laser/*_lP*/, mfcc_size, mini_batch, sil, job_log);
#if 0
	auto search = channel.find(call_id);
	if (search != channel.end())
		return EXIT_FAILURE;
#endif
	channel[call_id] = realtime_stt;
	realtime_stt->set_reset_period(reset_period);

	return EXIT_SUCCESS;
}

/**
 * @brief		Close channel
 * @details		채널 종료
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2017. 03. 09. 16:55:00
 * @param[in]	call_id	Call ID
 * @return		Upon successful completion, a EXIT_SUCCESS is returned.\n
 				Otherwise,
 				a negative error code is returned indicating what went wrong.
 * @see			VRServer::create_channel()
 */
int VRServer::close_channel(const std::string &call_id) {
	job_log->info("[0x%X] Close channel" LOG_FMT, THREAD_ID, LOG_INFO);

	auto search = channel.find(call_id);
	if (search == channel.end())
		return EXIT_FAILURE;

	channel.erase(search);
	return EXIT_SUCCESS;
}

/**
 * @brief		STT for realtime
 * @details		Realtime STT
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2017. 03. 09. 17:53:55
 * @param[in]	call_id		Call ID
 * @param[in]	buffer		녹취 데이터 
 * @param[in]	bufferLen	녹취 데이터 길이 
 * @param[in]	is_last		마지막 패킷 여부 
 * @param[out]	result		STT 결과
 * @return		Upon successful completion, a EXIT_SUCCESS is returned.\n
 				Otherwise,
 				a negative error code is returned indicating what went wrong.
 */
int VRServer::stt(
	const std::string &call_id,
	const short *buffer,
	const std::size_t bufferLen,
	const char state,
	std::string &result
) {
#ifdef USE_REALTIME_POOL
	std::map<std::string, std::shared_ptr<RealtimeSTT>>::iterator iter;
	std::shared_ptr<RealtimeSTT> node = nullptr;
	int rc = 0;

	{
		std::lock_guard<std::mutex> g(m_mxChannel);
		for(iter = channel.begin(); iter != channel.end(); iter++) {
			node = iter->second;
			if (!node->getCurrState()) {
				node->setCurrState(1);
				break;
			}
		}
	}

	if ( node ) {
		rc = node->stt(buffer, bufferLen, result);
		node->setCurrState(0);
	}
#else
	auto search = channel.find(call_id);
	if (search == channel.end()) {
		if ((state == 1) || (create_channel(call_id, channel.size()) != EXIT_SUCCESS)) {
			job_log->error("[0x%X] Cannot connect channel" LOG_FMT, THREAD_ID, LOG_INFO);
			return EXIT_FAILURE;
		}
	}

	auto node = channel[call_id];
	int rc = node->stt(buffer, bufferLen, result);

	if (state == 2) {
		// rc = node->free_buffer(result);
		close_channel(call_id);
	}
#endif
	return rc;
}
