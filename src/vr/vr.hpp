/**
 * @headerfile	vr.hpp "vr.hpp"
 * @file	vr.hpp
 * @brief	VR Server
 * @author	Kijeong Khil (kjkhil@itfact.co.kr)
 * @date	2016. 06. 17. 17:35:46
 * @see		
 */

#ifndef __ITFACT_VR_SERVER_H__
#define __ITFACT_VR_SERVER_H__

#include <chrono>
#include "worker.hpp"
#include "frontend_api.h"
#include "Laser.h"

#ifdef USE_REALTIME_POOL
#include <mutex>
#endif

using namespace itfact::worker;

namespace itfact {
	/// ���� �ν�
	namespace vr {
		// ���� �ν� ���
		namespace node {
			enum WAVE_FORMAT {
				STANDARD_WAVE,  ///< ǥ�� PCM WAVE
				WAVE_2CH,		///< 2ä�� WAVE
				WAVE,			///< WAVE
				MPEG_ID3,		///< MPEG ID3 Tag
				MPEG,			///< MPEG
				MPEG_2CH,		///< MPEG Stereo
				UNKNOWN_FORMAT	///< �� �� ����
			};
			class RealtimeSTT;

			static const unsigned long MAX_MINIBATCH = 1024;
			//static const unsigned long DEFAULT_ENGINE_CORE = 10;
			static const unsigned long DEFAULT_ENGINE_CORE = 2;
			static const unsigned long LDA_LEN_FRAMESTACK = 15;
			static const unsigned long LENGTH_KEYWORD = 256;
			static const unsigned long LENGTH_SYNTAX = 1024;
			static const unsigned long MAX_SYNTAX = 256;
			//static const std::size_t RESET_PERIOD = 2000000; // 2 GiB
			static const std::size_t RESET_PERIOD = 500000; //  

			int getFinalResult(Laser *slaserP, const std::size_t index, std::size_t &last_position,
				const std::size_t feature_dim, const std::size_t mfcc_size, float * const sil,
				std::string &buffer);
			int getIntermediateResults(Laser *laser, const std::size_t index,
				std::size_t &skip_position, const std::size_t last_position, std::string &buffer,
				const float minimum_confidence = 0,
				const std::size_t end_position = SIZE_MAX);

			/**
			 * @brief	VR ���
			 * @author	Kijeong Khil (kjkhil@itfact.co.kr)
			 */
			class VRServer : public WorkerDaemon
			{
			private: // Member
				log4cpp::Category *logger = NULL;
				//std::shared_ptr<std::thread> monitoring_thread;
				Laser *master_laser = NULL;
				Laser *master_laser1 = NULL;
				Laser *master_laser2 = NULL;
				float *sil = NULL;
				float minimum_confidence = 0;
				std::map<std::string, std::shared_ptr<RealtimeSTT>> channel;

				// ----------
				std::size_t mfcc_size = 600;;
				std::size_t mini_batch = 128;	// DNN��忡�� ���ÿ� ó���� frame ������ ����(128 ����-ETRI)
				// std::size_t mini_batch = 600;	// LSTM��忡�� ���ÿ� ó���� frame ������ ����(600 ����-ETRI)
				double prior_weight = 0.8L;		// DNN �� �� prior ���� weight ����
				bool useGPU = true;				// GPU ��� ����
				long idGPU = 0;					// GPU ID
				int numGPU = 1;					// GPU ��
				long stt_job_count = 1;

				std::size_t feature_dim;

				std::string frontend_config;
				std::string am_file;
				std::string fsm_file;
				std::string sym_file;
				std::string dnn_file;
				std::string prior_file;
				std::string norm_file;				
#ifdef USE_REALTIME_POOL
				mutable std::mutex m_mxChannel;
#endif
			public:
				// Server Name
				std::string server_name;

			public:
				VRServer() : WorkerDaemon() {};
				VRServer(const int argc, const char *argv[]) : WorkerDaemon(argc, argv) {};
				~VRServer();
				virtual int initialize() override;
				int stt(const short *buffer, const std::size_t bufferLen, long stt_thread_num, std::string &result);
				int unsegment(const std::string &data, std::string &result);
				int unsegment_with_time(const std::string &mlf_file, const std::string &unseg_file, int pause);
				int ssp(const std::string &mlf_file, std::string &buf);				
				static enum WAVE_FORMAT check_wave_format(const short *data, const size_t data_size);

				bool init_sftp(std::string _host, std::string _port, std::string _id, std::string _passwd, bool bEncrypt);
				bool init_ftps(std::string _host, std::string _port, std::string _id, std::string _passwd, bool bEncrypt);

				// For Real-time
				int stt(const std::string &call_id, const short *buffer, const std::size_t bufferLen,
						const char state, std::string &result);

			private:
				//int monitoring(std::shared_ptr<std::string> path);
				bool loadLaserModule();
				void unloadLaserModule();

				// For Real-time
				int create_channel(const std::string &call_id, const int stt_job_count);
				int close_channel(const std::string &call_id);
			};

			class RealtimeSTT
			{
			private:
				log4cpp::Category *job_log = NULL;
				std::string m_callid;
				std::shared_ptr<float> feature_vector;
				std::shared_ptr<LFrontEnd> front;
				std::shared_ptr<Laser> laser;
				// Laser *laser;
				float *temp_buffer = NULL;//short *temp_buffer = NULL;
				std::size_t temp_buffer_len = 0;
				std::size_t running = 0;
				std::size_t reset_period;
				std::size_t index = 0;
				std::size_t skip_position = 0;
				//std::size_t last_position = 0;

				// ----------
				std::size_t mfcc_size = 600;
				std::size_t mini_batch = 256;
				std::size_t minimum_size;
				std::size_t feature_dim;
				float *sil = NULL;

				volatile uint8_t m_CurrState; // 0: stanby, 1: working

			public:
				RealtimeSTT(std::string callid,
							std::shared_ptr<float> buffer,
							std::shared_ptr<LFrontEnd> frontend,
							std::shared_ptr<Laser> child_laser,
							// Laser *child_laser,
							const std::size_t a_mfcc_size,
							const std::size_t a_mini_batch,
							float *a_sil,
							log4cpp::Category *logger = &log4cpp::Category::getRoot());
				~RealtimeSTT();

				void set_reset_period(const std::size_t period);
				int stt(const short *buffer, const std::size_t buffer_len, std::string &result);
				int free_buffer(std::string &result);

				uint8_t getCurrState() { return m_CurrState; }
				void setCurrState(uint8_t state) { m_CurrState=state; }

			private:
				RealtimeSTT();
			};
		}
	}
}

#endif /* __ITFACT_VR_SERVER_H__ */
