/**
 * @headerfile	inotify.hpp "inotify.hpp"
 * @file	inotify.hpp
 * @brief	디렉터리 감시
 * @author	Kijeong Khil (kjkhil@itfact.co.kr)
 * @date	2016. 06. 17. 17:35:46
 * @see		
 */

#ifndef __ITFACT_VR_INOTIFY_H__
#define __ITFACT_VR_INOTIFY_H__

#include <curl/curl.h>
#include "configuration.hpp"

namespace itfact {
	namespace vr {
		namespace node {
			enum PROTOCOL {
				PROTOCOL_FILE,	///< 로컬 파일
				PROTOCOL_HTTP,	///< HTTP 프로토콜
				PROTOCOL_HTTPS,	///< HTTPS 프로토콜
				PROTOCOL_FTP,	///< FTP 프로토콜
				PROTOCOL_FTPS,	///< FTPs 프로토콜
				PROTOCOL_SFTP,	///< sFTP(SSH) 프로토콜
				PROTOCOL_NONE	///< 지원하지(알려지지) 않는 프로토콜이거나 녹취 데이터
			};

			/**
			 * @brief	디렉터리를 감시하여 API 호출을 통해 작업을 수행하는 모듈 
			 * @author	Kijeong Khil (kjkhil@itfact.co.kr)
			 */
			class VRInotify
			{
			private:
				itfact::common::Configuration config;
				CURLM *multi_handle;
				int handle_count;

			public:
				VRInotify(const int argc, const char *argv[]);
				~VRInotify();
				int monitoring();
				static int runJob(const std::shared_ptr<std::string> path,
								  const std::shared_ptr<std::string> filename,
								  const itfact::common::Configuration *config);
				static int jobMonitor(const std::shared_ptr<std::string> path,
					const std::shared_ptr<std::string> monitor_path,
					const itfact::common::Configuration *config);
				static std::thread::id getFinishedJob();

			private:
				VRInotify();
				static void waitForFinish(const int max_worker, const int seconds, const int increment = 0);
				static void waitForFinish_spk(const int max_worker, const int seconds, const int increment = 0);
				static void waitForFinish_fs_threshold(const int max_worker, const size_t total_filesize, const std::string filename, const size_t filesize, const int seconds, const int increment = 0);
				static int processRequest(
					const itfact::common::Configuration *config,
					const char *apiserver_uri, const char *pathname,
					const char *download_path, const std::string &format_string,
					const std::string data, const char *output = NULL, std::set<std::thread::id> *list = NULL);
				static int processRequest_spk(
					const itfact::common::Configuration *config,
					const char *apiserver_uri,
					const std::string &call_id,
					const std::string data,
					const char *output = NULL,
					std::set<std::thread::id> *list = NULL);
				static int sendRequest(
					const std::string &id,
					const itfact::common::Configuration *config,
					const char *apiserver_uri,
					const std::string &call_id,
					const std::string &body,
					const std::string &download_uri,
					const char *output_path = NULL, std::set<std::thread::id> *list = NULL);
				static int sendRequest_spk(
					const std::string &id,
					const itfact::common::Configuration *config,
					const char *apiserver_uri,
					const std::string &call_id,
					const std::string &body,
					const char *output_path = NULL, std::set<std::thread::id> *list = NULL);
				static size_t getFileInfo(
					const itfact::common::Configuration *config,
					const std::string download_uri, 
					const std::string *account = NULL);
				static std::string getDownloadURI(
					const itfact::common::Configuration *config,
					const char *pathname,
					const char *download_path,
					const std::string &format_string,
					const std::string data);
				//static size_t getFileInfo(
				//	const itfact::common::Configuration *config,
				//	const char *apiserver_uri, const char *pathname,
				//	const char *download_path, const std::string &format_string,
				//	const char *download_uri, const std::string *account = NULL,
				//	log4cpp::Category *logger = &log4cpp::Category::getRoot());
			};
		}
	}
}

#endif /* __ITFACT_VR_INOTIFY_H__ */
