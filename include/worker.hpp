/**
 * @headerfile	worker.hpp "worker.hpp"
 * @file	worker
 * @brief	Worker API
 * @details	
 * @author	Kijeong Khil (kjkhil@itfact.co.kr)
 * @date	2016. 06. 07. 10:22:45
 * @see		
 */

#ifndef ITF_WORKER_H
#define ITF_WORKER_H

#include <exception>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <boost/noncopyable.hpp>
#include <log4cpp/Category.hh>
#include <libgearman/gearman.h>

#include "configuration.hpp"

namespace itfact {
	/// Worker APIs
	namespace worker {

		enum PROTOCOL {
			PROTOCOL_FILE,	///< ���� ����
			PROTOCOL_MOUNT,	///< ����Ʈ��(���������ν�) ������ VR������ �����ϱ� ���� ��������
			PROTOCOL_HTTP,	///< HTTP ��������
			PROTOCOL_HTTPS,	///< HTTPS ��������
			PROTOCOL_FTP,	///< FTP ��������
			PROTOCOL_FTPS,	///< FTPs ��������
			PROTOCOL_SFTP,	///< sFTP(SSH) ��������
			PROTOCOL_NONE	///< ��������(�˷�����) �ʴ� ���������̰ų� ���� ������
		};

		/**
		 * @brief	Gearman ��Ŀ ��� Ŭ����
		 * @author	Kijeong Khil (kjkhil@itfact.co.kr)
		 */
		class WorkerDaemon : private boost::noncopyable
		{
		private: // Member
			std::vector<std::thread> workers;
			bool is_running = false;
			common::Configuration config;
			log4cpp::Category *logger;

		public:
			WorkerDaemon();
			WorkerDaemon(const int argc, const char *argv[]);
			~WorkerDaemon();
			int initialize(const int argc, const char *argv[]);
			virtual int initialize() = 0;
			void stop();

			const common::Configuration *getConfig() {return &config;};
			const common::Configuration *getConfig() const {return &config;};
			log4cpp::Category *getLogger() {return logger;};
			log4cpp::Category *getLogger() const {return logger;};

			unsigned long getTotalWorkers(const std::string &name);
			bool isRunning() {return is_running;};
			bool isRunning() const {return is_running;};
			std::string getHost() {return config.getHost();};
			std::string getHost() const {return config.getHost();};
			in_port_t getPort() {return static_cast<in_port_t>(config.getPort());};
			in_port_t getPort() const {return static_cast<in_port_t>(config.getPort());};
			long getTimeout() {return config.getTimeout();};
			long getTimeout() const {return config.getTimeout();};

			static enum PROTOCOL
			downloadData(const common::Configuration *config,
						 const char *workload, const size_t workload_size,
						 std::vector<short> &buffer, const std::string *account = NULL,
						 log4cpp::Category *logger = &log4cpp::Category::getRoot());				
			static CURLcode uploadData( const common::Configuration *config,
										const std::string &uri, const std::string &pathname,
										const std::string *account = NULL,
										log4cpp::Category *logger = &log4cpp::Category::getRoot());
			static enum PROTOCOL checkProtocol(const common::Configuration *config,
				const char *workload, const size_t workload_size,
				log4cpp::Category *logger = &log4cpp::Category::getRoot());

		protected:
			void run(const std::string name, void *context, unsigned int count,
					 gearman_return_t (*fn)(gearman_job_st *, void *));
			void join();

		};
	}
}

#endif /* ITF_WORKER_H */
