/**
 * @file	inotify.cc
 * @brief	디렉터리 감시 
 * @author	Kijeong Khil (kjkhil@itfact.co.kr)
 * @date	2016. 08. 08. 10:11:12
 * @todo	멀티스레딩 구조 변경(작업당 생성이 아닌 동시 작업 가능 수만큼 할당하도록 변경 필요)	
 */
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <ctime>
#include <regex>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sstream>
#include <dirent.h>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/identity.hpp>
#include <boost/multi_index/member.hpp>

#include "inotify.hpp"

#define BUF_LEN (10 * (sizeof(struct inotify_event) + FILENAME_MAX + 1))

using namespace itfact::vr::node;

static log4cpp::Category *logger = NULL;
static std::string token_header;
static std::mutex job_lock;
static std::mutex job_lock_spk;
static std::atomic<int> working_job(0);	///< 현재 작업 수
static std::atomic<int> working_job_spk(0);	///< 현재 작업 수
static std::atomic<int> available_workers(12);	///< 최대 작업 수
static std::atomic<int> available_workers_spk(1);	///< 최대 작업 수

static std::mutex curl_lock;

// 처리 파일의 총 사이즈
//static size_t total_file_size = 0;
static bool job_check_flag = false;

// 화자분리 쓰레드 jobs
static std::set<std::thread::id> jobs_spk;

static struct {
	std::string rec_ext;
	std::string index_type;
	std::string watch;
	std::string api_service;
	std::string apiserver_url;
	std::string apiserver_version;
	std::string passwd;
	bool delete_on_success;
	bool daily_output;
	bool unique_output;
} default_config = {
	.rec_ext = "wav",
	.index_type = "filename",
	.watch = "pcm",
	.api_service = "vr",
	.apiserver_url = "http://localhost:3000",
	.apiserver_version = "v1.0",
	.passwd = "vr_server",
	.delete_on_success = false,
	.daily_output = false,
	.unique_output = false,
};

typedef struct st_job_file {
	std::string filename_;
	size_t filesize_;	

	st_job_file(std::string& filename, size_t filesize)
		: filename_(filename)
		, filesize_(filesize)
	{}
} ST_JOB_FILE;

// multi_index 처리 (boost)
typedef boost::multi_index::multi_index_container<
	ST_JOB_FILE,
	boost::multi_index::indexed_by<
			// filename은 고유해야 한다.
			boost::multi_index::ordered_unique<
			boost::multi_index::identity<ST_JOB_FILE>
		>,
		boost::multi_index::ordered_unique<
			boost::multi_index::member<
				ST_JOB_FILE, std::string, &ST_JOB_FILE::filename_
			>
		>,
		// filesize은 고유하지 않으며, 정렬될 필요도 없다
		boost::multi_index::hashed_non_unique<
			boost::multi_index::member<
				ST_JOB_FILE, size_t, &ST_JOB_FILE::filesize_
			>
		>
	>
> indexes_job_file_t;

indexes_job_file_t job_file_index;

// operation 처리
static inline bool operator < (const ST_JOB_FILE& job_file1, const ST_JOB_FILE& job_file2) {
	return job_file1.filename_ < job_file2.filename_;
}

static const std::map<std::string, enum PROTOCOL>
protocol_list = { { "file", PROTOCOL_FILE },
				{ "http", PROTOCOL_HTTP },
				{ "https", PROTOCOL_HTTPS },
				{ "ftp", PROTOCOL_FTP },
				{ "ftps", PROTOCOL_FTPS },
				{ "sftp", PROTOCOL_SFTP },
				{ "scp", PROTOCOL_SFTP },
				{ "ssh", PROTOCOL_SFTP } };

/**
 */
int main(const int argc, char const *argv[]) {
	try {
		VRInotify server(argc, argv);
		server.monitoring();
	} catch (std::exception &e) {
		perror(e.what());
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

VRInotify::VRInotify(const int argc, const char *argv[]) : config(argc, argv) {
	CURLcode rc = curl_global_init(CURL_GLOBAL_ALL);
	if (rc)
		throw std::runtime_error(curl_easy_strerror(rc));

	handle_count = 0;
	multi_handle = curl_multi_init();
	if (multi_handle == NULL)
		throw std::runtime_error("Cannot allocation CURL-multi");

	logger = config.getLogger();
}

VRInotify::~VRInotify() {
	std::vector<std::shared_ptr<CURL>> curl_handles;
	while (curl_handles.size() > 0) {
		curl_multi_remove_handle(multi_handle, curl_handles.back().get());
		curl_handles.pop_back();
	}

	curl_multi_cleanup(multi_handle);
	curl_global_cleanup();
}

/**
 * @brief		작업 종료 큐에 등록 
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 11. 04. 13:34:38
 * @param[in]	threadId	ThreadId
 */
static inline void finishJob(std::thread::id threadId, std::set<std::thread::id> *list) {
	working_job.fetch_sub(1);
	if (list)
		list->erase(std::this_thread::get_id());
}

static inline void finishJob_spk(std::thread::id threadId, std::set<std::thread::id> *list) {
	working_job_spk.fetch_sub(1);
	if (list)
		list->erase(std::this_thread::get_id());
}

/**
* @brief		작업 종료 큐에 등록
* @author		Kijeong Khil (kjkhil@itfact.co.kr)
* @date		2016. 11. 04. 13:34:38
* @param[in]	threadId	ThreadId
*/
static inline void finishJob_fs_threshold(std::thread::id threadId, std::set<std::thread::id> *list, const itfact::common::Configuration *config, const std::string &download_uri) {
	// 파일사이즈 체크를 하여 inotify 처리라면
	if (config->getConfig("inotify.fs_threshold_yn").compare("y") == 0) {
		auto job_id = std::this_thread::get_id();
		std::stringstream ss;
		ss << job_id;
		std::string id = ss.str();

		// multi-index에 등록된 파일명으로 키인덱스를 찾아, 삭제한다.
		indexes_job_file_t::nth_index<1>::type& filenm_index = job_file_index.get<1>();
		filenm_index.erase(download_uri);
		logger->debug("[%s] " COLOR_GREEN "multi-index erase(count) - %d" COLOR_NC, id.c_str(), filenm_index.size());
	}

	working_job.fetch_sub(1);
	if (list)
		list->erase(std::this_thread::get_id());
}

/**
 * @brief		데이터 다운로드 
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 06. 28. 22:59:42
 * @param[in]	source		받은 데이터
 * @param[in]	size		받은 데이터 크기
 * @param[in]	nmemb		받은 데이터 개수
 * @param[out]	userData	저장 버퍼
 * @return		Upon successful completion, a data size is returned.\n
 				Otherwise, a negative zero is returned.
 */
static size_t getData(void *source , size_t size , size_t nmemb , void *userData) {
	const int total_size = size * nmemb;
	const int length = total_size / sizeof(char);
	char *data = (char *) source;
	std::string *buffer = (std::string *) userData;

	for (int i = 0; i < length; ++i)
		buffer->push_back(data[i]);

	return total_size;
}

static size_t throw_away(void *ptr, size_t size, size_t nmemb, void *data) {
	(void)ptr;
	(void)data;

	// we are not interested in the headers itself, so we only return the size would have saved...
	return (size_t)(size * nmemb);
}

static size_t get_file_size(const char *filename) {
	struct stat sb;
	if (stat(filename, &sb) != 0) {
		//logger->warn("[0x%X] Unsupported protocol: %s : %s", THREAD_ID, filename, strerror(errno));
		return -1;
	}
	return sb.st_size;
}

/**
 * @brief		사용 가능한 워커 총 수 확인 
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 08. 08. 19:08:14
 */
static void getTotalWorkers() {
	char buffer[128];
	bool isLoaded = false;
	while (true) {
		std::this_thread::sleep_for(std::chrono::seconds(isLoaded ? 300 : 15));
		std::memset(buffer, '\0', 128);
		std::string worker_info = "";
		FILE *_pipe = popen("/usr/local/bin/gearadmin --status | /bin/grep vr_stt", "r");
		if (!_pipe)
			continue;
		std::shared_ptr<FILE> pipe(_pipe, pclose);

		while (!std::feof(pipe.get())) {
			if (std::fgets(buffer, 128, pipe.get()) != NULL)
				worker_info += buffer;
		}

		std::vector<std::string> v;
		boost::split(v, worker_info, boost::is_any_of("\t "), boost::token_compress_on);
		if (v.size() < 2)
			continue;

		try {
			int total_workers = std::stoi(v[3]);
			//logger->debug("===========> %d", total_workers);
			if (total_workers > 0)
				available_workers = total_workers;
		} catch (std::exception &e) {
			logger->warn("Cannot read information");
		}

	}
}

static void getTotalWorkers_spk() {
	char buffer[256];
	bool isLoaded = false;
	while (true) {
		std::this_thread::sleep_for(std::chrono::seconds(isLoaded ? 300 : 15));
		std::memset(buffer, '\0', 256);
		std::string worker_info = "";
		FILE *_pipe = popen("/usr/local/bin/gearadmin --status | /bin/grep vr_spk", "r");
		if (!_pipe)
			continue;
		std::shared_ptr<FILE> pipe(_pipe, pclose);

		while (!std::feof(pipe.get())) {
			if (std::fgets(buffer, 256, pipe.get()) != NULL)
				worker_info += buffer;
		}

		//logger->debug("worker_info : %s", worker_info.c_str());

		std::vector<std::string> v_tmp;
		boost::split(v_tmp, worker_info, boost::is_any_of("\n"), boost::token_compress_on);

		if (v_tmp.size() < 1)
			continue;

		int total_workers = 0;

		for (int i = 0; i < (int)v_tmp.size(); i++) {
			std::vector<std::string> v;
			boost::split(v, v_tmp[i], boost::is_any_of("\t "), boost::token_compress_on);
			if (v.size() < 2)
				continue;

			try {
				total_workers += std::stoi(v[3]);
				if (total_workers > 0)
					available_workers_spk = total_workers;
			}
			catch (std::exception &e) {
				logger->warn("Cannot read information");
			}
		}

		//logger->debug("available_workers_spk : %d", total_workers);

		/*
		std::vector<std::string> v;
		boost::split(v, worker_info, boost::is_any_of("\t "), boost::token_compress_on);
		if (v.size() < 2)
			continue;

		try {
			int total_workers = std::stoi(v[3]);
			if (total_workers > 0)
				available_workers_spk = total_workers;
		}
		catch (std::exception &e) {
			logger->warn("Cannot read information");
		}
		*/

	}
}

/**
 * @brief		인증 토큰 요청 
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 08. 18. 10:13:19
 * @param[in]	config			서버 설정 
 * @param[in]	curl_headers	CURL 헤더 
 */
static std::string
__get_token(const char *id,
			const itfact::common::Configuration *config,
			std::shared_ptr<struct curl_slist> curl_headers) {
	// FIXME: 인증 방식 변경(API 인증 방식 추가)
	if (config->isSet("api.apikey")) {		
		std::string token("apikey: ");
		token.append(config->getConfig("api.apikey"));

		// apikey(token) 형식이 아닌 경우 에러 로그 출력
		if (token.size() < 20) {
			logger->error("[%s] APIKEY[token] does not match API server authentication processing.", id);
			logger->error("[%s] Check the configuration file.", id);
		}
		return token;
	}

	CURL *curl = curl_easy_init();
	if (!curl) {
		logger->error("[%s] Cannot allocation CURL", id);
		return "";
	}

	std::shared_ptr<CURL> ctx(curl, curl_easy_cleanup);
	std::string uri(config->getConfig("api.url", default_config.apiserver_url.c_str()));
	uri.append("/login");

	std::string auth_body("{\"username\": \"vr_server\", \"password\": \"");
	auth_body.append(config->getConfig("api.passwd", default_config.passwd.c_str()));
	auth_body.append("\"}");

	curl_easy_setopt(ctx.get(), CURLOPT_URL, uri.c_str()); // URL 설정 
	curl_easy_setopt(ctx.get(), CURLOPT_POST, true);
	curl_easy_setopt(ctx.get(), CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(ctx.get(), CURLOPT_HTTPHEADER, curl_headers.get());
	curl_easy_setopt(ctx.get(), CURLOPT_NOPROGRESS, true);
	curl_easy_setopt(ctx.get(), CURLOPT_TIMEOUT, 3L);
	curl_easy_setopt(ctx.get(), CURLOPT_POSTFIELDS, auth_body.c_str());
	curl_easy_setopt(ctx.get(), CURLOPT_WRITEFUNCTION, getData);
	std::string token("");
	curl_easy_setopt(ctx.get(), CURLOPT_WRITEDATA, (void *) &token); // 바디 출력 설정 

	// 인증 요청 
	std::string auth_token("authorization: ");
	try {
		CURLcode response_code = curl_easy_perform(ctx.get());
		if (response_code == CURLE_OK) {
			long http_code = 0;
			curl_easy_getinfo(ctx.get(), CURLINFO_RESPONSE_CODE, &http_code);
			if (http_code == 200) {
				auto pos = token.find("access_token");
				if (pos == std::string::npos) 
					throw std::invalid_argument("Login fail");

				auto t_pos = token.find(":", pos);
				if (t_pos == std::string::npos)
					throw std::invalid_argument("Login fail");

				auto s_pos = token.find("\"", t_pos);
				if (s_pos == std::string::npos)
					throw std::invalid_argument("Login fail");
				else
					++s_pos;

				auto e_pos = token.find("\"", s_pos);
				if (e_pos == std::string::npos) 
					throw std::invalid_argument("Login fail");

				auth_token.append(token.substr(s_pos, e_pos - s_pos));
				logger->debug("[%s] %s", id, auth_token.c_str());
			} else {
				logger->error("[%s] Access denied(%d)", id, http_code);
			}
		} else {
			logger->error("[%s] cURL error: %s(%d)", id, curl_easy_strerror(response_code), response_code);
		}
	} catch (std::exception &e) {
		// 인증 실패 
		logger->error("[%s] Error occurred: %s", id, e.what());
	}

	return auth_token;
}

static void copyFile(char *original, char *copiedFile) //파일복사
{
	int original_o, copiedFile_o;
	int read_o;

	char buf[1024];

	original_o = open(original, O_RDONLY);
	copiedFile_o = open(copiedFile, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);

	while ((read_o = read(original_o, buf, sizeof(buf))) > 0)
		write(copiedFile_o, buf, read_o);
}



/**
 * @brief		STT 요청 
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 10. 07. 14:56:47
 * @param[in]	metadata	Metadata
 * @return		Upon successful completion, a EXIT_SUCCESS is returned.\n
 				Otherwise,
 				a negative error code is returned indicating what went wrong.
 */
int VRInotify::sendRequest(
		const std::string &id,
		const itfact::common::Configuration *config,
		const char *apiserver_uri,
		const std::string &call_id,
		const std::string &body,
		const std::string &download_uri,
		const char *output_path, std::set<std::thread::id> *list) {
	std::lock_guard<std::mutex> guard(job_lock);

	// HTTP 헤더 생성 
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Accept: application/json");
	if (!headers) {
		logger->error("[%s] Cannot allocation Headers", id.c_str());

		if (config->getConfig("inotify.fs_threshold_yn").compare("y") == 0) {
			finishJob_fs_threshold(std::this_thread::get_id(), list, config, download_uri);
		}
		else {
			finishJob(std::this_thread::get_id(), list);
		}

		return EXIT_FAILURE;
	}
	std::shared_ptr<struct curl_slist> curl_headers(headers, curl_slist_free_all);
	curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");

	// 미인증 상태라면 인증 요청 	
	if (token_header.empty() || token_header.size() < 20) {
		logger->debug("[%s] Attempt to authenticate API server", id.c_str());
		token_header = __get_token(id.c_str(), config, curl_headers);
		if (token_header.empty() || token_header.size() < 20) {
			//finishJob(std::this_thread::get_id(), list, config, download_uri);
			if (config->getConfig("inotify.fs_threshold_yn").compare("y") == 0) {
				finishJob_fs_threshold(std::this_thread::get_id(), list, config, download_uri);
			}
			else {
				finishJob(std::this_thread::get_id(), list);
			}

			logger->error("[%s] API server authentication failed", id.c_str());

			return EXIT_FAILURE;
		}
	}
	curl_slist_append(headers, token_header.c_str());

	// STT 요청 
	CURL *curl = curl_easy_init();
	if (!curl) {
		logger->error("[%s] Cannot allocation CURL", id.c_str());
		//finishJob(std::this_thread::get_id(), list, config, download_uri);

		if (config->getConfig("inotify.fs_threshold_yn").compare("y") == 0) {
			finishJob_fs_threshold(std::this_thread::get_id(), list, config, download_uri);
		}
		else {
			finishJob(std::this_thread::get_id(), list);
		}
		
		return EXIT_FAILURE;
	}

	std::shared_ptr<CURL> ctx(curl, curl_easy_cleanup);
	curl_easy_setopt(ctx.get(), CURLOPT_URL, apiserver_uri); // URL 설정 
	curl_easy_setopt(ctx.get(), CURLOPT_POST, true);
	curl_easy_setopt(ctx.get(), CURLOPT_SSL_VERIFYPEER, 0L); // 인증서 무시 
	curl_easy_setopt(ctx.get(), CURLOPT_HTTPHEADER, curl_headers.get());
	curl_easy_setopt(ctx.get(), CURLOPT_NOPROGRESS, true);
	// curl_easy_setopt(ctx.get(), CURLOPT_VERBOSE, true);
	curl_easy_setopt(ctx.get(), CURLOPT_TIMEOUT, 0L);

	curl_easy_setopt(ctx.get(), CURLOPT_POSTFIELDS, body.c_str());
	// curl_easy_setopt(ctx.get(), CURLOPT_WRITEHEADER, stdout);
	curl_easy_setopt(ctx.get(), CURLOPT_WRITEFUNCTION, getData);
	std::string response_text("");
	curl_easy_setopt(ctx.get(), CURLOPT_WRITEDATA, (void *) &response_text); // 바디 출력 설정 

	// CURLMcode curl_multi_add_handle(CURLM *multi_handle, CURL *easy_handle); // FIXME:
	job_lock.unlock();
	logger->debug("[%s] POST %s HTTP/1.1\n%s", id.c_str(), apiserver_uri, body.c_str());
	try {
		CURLcode response_code = curl_easy_perform(ctx.get());
		if (response_code != CURLE_OK) {
			logger->error("[%s] Fail to request: (%d) %s", id.c_str(), response_code, curl_easy_strerror(response_code));
			//finishJob(std::this_thread::get_id(), list, config, download_uri);
			if (config->getConfig("inotify.fs_threshold_yn").compare("y") == 0) {
				finishJob_fs_threshold(std::this_thread::get_id(), list, config, download_uri);
			}
			else {
				finishJob(std::this_thread::get_id(), list);
			}

			return EXIT_FAILURE;
		}
	} catch (std::exception &e) {
		logger->error("[%s] Error %s", id.c_str(), e.what());
		//finishJob(std::this_thread::get_id(), list, config, download_uri);
		if (config->getConfig("inotify.fs_threshold_yn").compare("y") == 0) {
			finishJob_fs_threshold(std::this_thread::get_id(), list, config, download_uri);
		}
		else {
			finishJob(std::this_thread::get_id(), list);
		}

		return EXIT_FAILURE;
	}

	job_lock.lock();
	long http_code = 0;
	curl_easy_getinfo(ctx.get(), CURLINFO_RESPONSE_CODE, &http_code);
	if (http_code == 200 || http_code == 201) {
		logger->info("[%s] " COLOR_GREEN "STT Success - %s(%d-%s)" COLOR_NC, id.c_str(), download_uri.c_str(), http_code, response_text.c_str());

		bool delete_on_success =
			const_cast<itfact::common::Configuration *>(config)->getConfig<bool>(
				"inotify.delete_on_success", &default_config.delete_on_success);

		/*
		// 화자분리 처리
		if (config->isSet("spk.enable") && config->getConfig("spk.enable").compare("true") == 0) {
			try {
				//VRInotify::waitForFinish_spk(available_workers_spk.load(), 1, 1);

				std::string apiserver(config->getConfig("api.url", default_config.apiserver_url.c_str()));
				if (apiserver[apiserver.size() - 1] != '/')
					apiserver.push_back('/');
				apiserver.append("separating");
				if (apiserver[apiserver.size() - 1] != '/')
					apiserver.push_back('/');
				//apiserver.push_back('_');
				apiserver.append(config->getConfig("api.version", default_config.apiserver_version.c_str()));
				apiserver.append("/jobs");

				working_job_spk.fetch_add(1);
				std::thread job_spk(VRInotify::processRequest_spk,
					config,
					apiserver.c_str(),
					call_id,
					response_text,
					(char *)NULL,
					&jobs_spk
				);
				jobs_spk.insert(job_spk.get_id());
				job_spk.detach();
			}
			catch (std::exception &e) {
				logger->warn("SPK Job error: %s(%d) (#job: %d, running: %d)",
					e.what(), errno, jobs_spk.size(), working_job_spk.load());
			}
		}	
		*/

		// 성공이면 설정에 따라 삭제 
		if (delete_on_success) {
			std::string::size_type idx = download_uri.find("://");
			if (idx == std::string::npos || download_uri.find("file://") != std::string::npos) {
				std::string remove_pathname(idx == std::string::npos ? download_uri : download_uri.substr(idx + 3));
				try {
					std::remove(download_uri.c_str());
				} catch (std::exception &e) {
					logger->warn("[%s] Cannot remove: %s", id.c_str(), download_uri.c_str());
				}
			}
		}
	} else if (http_code == 401) {
		logger->notice("[%s] " COLOR_RED "Failure" COLOR_NC " (HTTP Response code: " COLOR_YELLOW "%d" COLOR_NC ")", id.c_str(), http_code);
		token_header = "";
		// 인증 실패의 경우 재시도 
		job_lock.unlock();
		logger->info("[%s] Retry job", id.c_str());
		return sendRequest(id, config, apiserver_uri, call_id, body, download_uri, output_path, list);
	} else {
		logger->warn("[%s] " COLOR_RED "Failure" COLOR_NC " (HTTP Response code: %s%d" COLOR_NC ")\n%s",
					  id.c_str(), (http_code >= 500 ? COLOR_RED : COLOR_YELLOW), http_code, response_text.c_str());
	}	

	//finishJob(std::this_thread::get_id(), list, config, download_uri);
	if (config->getConfig("inotify.fs_threshold_yn").compare("y") == 0) {
		finishJob_fs_threshold(std::this_thread::get_id(), list, config, download_uri);
	}
	else {
		finishJob(std::this_thread::get_id(), list);
	}

	return EXIT_SUCCESS;
}

int VRInotify::sendRequest_spk(
	const std::string &id,
	const itfact::common::Configuration *config,
	const char *apiserver_uri,
	const std::string &call_id,
	const std::string &body,
	const char *output_path, std::set<std::thread::id> *list) {
	std::lock_guard<std::mutex> guard(job_lock_spk);

	// HTTP 헤더 생성 
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Accept: application/json");
	if (!headers) {
		logger->error("[%s] Cannot allocation Headers", id.c_str());
		finishJob_spk(std::this_thread::get_id(), list);

		return EXIT_FAILURE;
	}
	std::shared_ptr<struct curl_slist> curl_headers(headers, curl_slist_free_all);
	curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");

	// 미인증 상태라면 인증 요청 	
	if (token_header.empty() || token_header.size() < 20) {
		logger->debug("[%s] Attempt to authenticate API server", id.c_str());
		token_header = __get_token(id.c_str(), config, curl_headers);
		if (token_header.empty() || token_header.size() < 20) {
			finishJob_spk(std::this_thread::get_id(), list);
			logger->error("[%s] API server authentication failed", id.c_str());

			return EXIT_FAILURE;
		}
	}
	curl_slist_append(headers, token_header.c_str());

	// STT 요청 
	CURL *curl = curl_easy_init();
	if (!curl) {
		logger->error("[%s] Cannot allocation CURL", id.c_str());
		finishJob_spk(std::this_thread::get_id(), list);
	
		return EXIT_FAILURE;
	}

	std::shared_ptr<CURL> ctx(curl, curl_easy_cleanup);
	curl_easy_setopt(ctx.get(), CURLOPT_URL, apiserver_uri); // URL 설정 
	curl_easy_setopt(ctx.get(), CURLOPT_POST, true);
	curl_easy_setopt(ctx.get(), CURLOPT_SSL_VERIFYPEER, 0L); // 인증서 무시 
	curl_easy_setopt(ctx.get(), CURLOPT_HTTPHEADER, curl_headers.get());
	curl_easy_setopt(ctx.get(), CURLOPT_NOPROGRESS, true);
	// curl_easy_setopt(ctx.get(), CURLOPT_VERBOSE, true);
	curl_easy_setopt(ctx.get(), CURLOPT_TIMEOUT, 0L);

	curl_easy_setopt(ctx.get(), CURLOPT_POSTFIELDS, body.c_str());
	// curl_easy_setopt(ctx.get(), CURLOPT_WRITEHEADER, stdout);
	curl_easy_setopt(ctx.get(), CURLOPT_WRITEFUNCTION, getData);
	std::string response_text("");
	curl_easy_setopt(ctx.get(), CURLOPT_WRITEDATA, (void *)&response_text); // 바디 출력 설정 

																			// CURLMcode curl_multi_add_handle(CURLM *multi_handle, CURL *easy_handle); // FIXME:
	job_lock_spk.unlock();
	logger->debug("[%s] SPK POST %s HTTP/1.1\n%s", id.c_str(), apiserver_uri, body.c_str());
	try {
		CURLcode response_code = curl_easy_perform(ctx.get());
		if (response_code != CURLE_OK) {
			logger->error("[%s] SPK Fail to request: (%d) %s", id.c_str(), response_code, curl_easy_strerror(response_code));
			finishJob_spk(std::this_thread::get_id(), list);

			return EXIT_FAILURE;
		}
	}
	catch (std::exception &e) {
		logger->error("[%s] Error %s", id.c_str(), e.what());
		finishJob_spk(std::this_thread::get_id(), list);

		return EXIT_FAILURE;
	}

	job_lock_spk.lock();
	long http_code = 0;
	curl_easy_getinfo(ctx.get(), CURLINFO_RESPONSE_CODE, &http_code);
	if (http_code == 200 || http_code == 201) {
		logger->info("[%s] " COLOR_GREEN "SPK Success - %s" COLOR_NC, id.c_str(), call_id.c_str());

		/*
		bool delete_on_success =
			const_cast<itfact::common::Configuration *>(config)->getConfig<bool>(
				"inotify.delete_on_success", &default_config.delete_on_success);
		*/
	}
	else if (http_code == 401) {
		logger->notice("[%s] " COLOR_RED "SPK Failure" COLOR_NC " (HTTP Response code: " COLOR_YELLOW "%d" COLOR_NC ")", id.c_str(), http_code);
		token_header = "";
		// 인증 실패의 경우 재시도 
		job_lock_spk.unlock();
		logger->info("[%s] SPK Retry job", id.c_str());
		return sendRequest_spk(id, config, apiserver_uri, call_id, body, output_path, list);
	}
	else {
		logger->warn("[%s] " COLOR_RED "SPK Failure" COLOR_NC " (HTTP Response code: %s%d" COLOR_NC ")\n%s",
			id.c_str(), (http_code >= 500 ? COLOR_RED : COLOR_YELLOW), http_code, response_text.c_str());
	}

	finishJob_spk(std::this_thread::get_id(), list);

	return EXIT_SUCCESS;
}

/**
 * @brief		다운로드 경로 설정 
 * @details
 * 	download_path 우선순위:
 * 	1. index file
 * 	2. configuration file
 * 	3. input directory
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 10. 10. 22:16:27
 * @param[in]	config	환경 설정 
 */
static inline std::string
getDownloadPath(const std::string &filename,
				const std::shared_ptr<std::map<std::string, std::string>> metadata,
				const char *download_path = NULL) {
	std::string download_uri("");
	auto down_path = metadata->find("download_path");
	if (down_path != metadata->end()) {
		logger->debug("Download path from metadata: %s", down_path->second.c_str());
		download_uri.append(down_path->second);
	} else 	if (download_path != NULL)
		download_uri.append(download_path);

	if (download_uri.size() > 0 && download_uri[download_uri.size() - 1] != '/')
		download_uri.push_back('/');

	download_uri.append(filename);
	return download_uri;
}

/**
 * @brief		Event 처리 
 * @details		iNotify에 의해 확인된 녹취 파일을 분석 처리 
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 07. 19. 09:39:12
 * @param[in]	path		파일 위치 
 * @param[in]	filename	파일명 
 * @param[in]	config		서버 설정 
 * @return		Upon successful completion, a EXIT_SUCCESS is returned.\n
 				Otherwise, a EXIT_FAILURE is returned.
 */
int VRInotify::processRequest(
	const itfact::common::Configuration *config,
	const char *apiserver_uri, const char *pathname,
	const char *download_path, const std::string &format_string,
	const std::string data, const char *output, std::set<std::thread::id> *list) {
	std::string id(COLOR_BLACK_BOLD);
	id.append("job:");
	id.append(boost::lexical_cast<std::string>(std::this_thread::get_id()));
	id.append(COLOR_NC);

	if (pathname)
		logger->info("[%s] Request STT: %s", id.c_str(), pathname);

	std::shared_ptr<std::map<std::string, std::string>> metadata;
	try {
		metadata = config->parsingConfig(format_string, data);
	} catch (std::exception &e) {
		logger->error("[%s] Cannot parsing. %s", id.c_str(), e.what());
		finishJob(std::this_thread::get_id(), list);
		return EXIT_FAILURE;
	}

	// 다운로드 경로가 없는 경우 입력 경로로 설정 
	std::string tmp_input_path;
	if (download_path == NULL) {
		tmp_input_path = config->getConfig("inotify.input_path");
		download_path = const_cast<char *>(tmp_input_path.c_str());
	}

	std::string download_uri("");
	if (pathname != NULL) {
		// filename이 따로 설정된 경우 
		auto filename = metadata->find("filename");
		if (filename != metadata->end())
			download_uri = getDownloadPath(filename->second, metadata, download_path);
		else
			download_uri.append(pathname);
	} else {
		auto filename = metadata->find("filename");
		if (filename != metadata->end())
			download_uri = getDownloadPath(filename->second, metadata, download_path);
		else {
			logger->error("[%s] Invalid index_format: need to filename", id.c_str());
			finishJob(std::this_thread::get_id(), list);
			return EXIT_FAILURE;
		}
	}
	logger->info("[%s] Process %s", id.c_str(), download_uri.c_str());	

	std::string post_data("{\"uri\": \"");
	post_data.append(download_uri);

	std::string call_id("");
	for (auto cur : *metadata.get()) {
		if (cur.first.compare("uri") == 0 ||
			cur.first.compare("filename") == 0 ||
			cur.first.compare("download_path") == 0 ||
			cur.first.compare("rec_time") == 0 ||
			cur.first.compare("output") == 0 ||
			cur.first.compare("silence") == 0) {
			continue;
		} else if (cur.first.compare("call_id") == 0) {
			call_id = cur.second;
		}

		post_data.append("\", \"");
		post_data.append(cur.first);
		post_data.append("\": \"");
		post_data.append(cur.second);

		if (cur.first.compare("rec_date") == 0) {
			auto search = metadata->find("rec_time");
			if (search != metadata->end())
				post_data.append(search->second);
		}
	}

	if (output) {
		post_data.append("\", \"output\": \"");
		post_data.append(output);
	}

	// 화자 분리 처리
	if (config->isSet("spk.enable") && config->getConfig("spk.enable").compare("true") == 0) {
		post_data.append("\", \"spk\": \"");
		post_data.append("true");
	}

	post_data.append("\", \"silence\": \"yes\"}");

	// debugging code add - start
	logger->debug("[%s] API Server call info - call id : %s", id.c_str(), call_id.c_str());
	logger->debug("[%s] API Server call info - post_data : %s", id.c_str(), post_data.c_str());
	logger->debug("[%s] API Server call info - download_uri : %s", id.c_str(), download_uri.c_str());
	logger->debug("[%s] API Server call info - output : %s", id.c_str(), output);
	// debugging code add - end

	return sendRequest(id, config, apiserver_uri, call_id, post_data, download_uri, output, list);
}

int VRInotify::processRequest_spk(
	const itfact::common::Configuration *config,
	const char *apiserver_uri,
	const std::string &call_id,
	const std::string data,
	const char *output,
	std::set<std::thread::id> *list) 
{
	std::string id(COLOR_BLACK_BOLD);
	id.append("job:");
	id.append(boost::lexical_cast<std::string>(std::this_thread::get_id()));
	id.append(COLOR_NC);
	
	logger->info("[%s] Request SPK - %s", id.c_str(), call_id.c_str());
	
	std::string post_data("");
	std::string tmp_data = data.substr(0, data.size() - 1);
	post_data = tmp_data;
	post_data.append(", \"silence\": \"yes\"}");
	
	// debugging code add - start
	logger->debug("[%s] SPK API Server call info - %s", id.c_str(), post_data.c_str());
	// debugging code add - end

	return sendRequest_spk(id, config, apiserver_uri, call_id, post_data, output, list);
}

/**
 * @brief		Wait for finish
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 09. 21. 22:32:19
 * @param[in]	max_worker	최대 제한 
 * @param[in]	seconds		슬립 시간 
 * @param[in]	increment	슬립 시간 증가값 
 */
void VRInotify::waitForFinish(const int max_worker, const int seconds, const int increment ) {
	int wait_time = seconds;
	while (true) {
		if (max_worker <= working_job.load()) {
			logger->debug("Sleep for %ds (running: %d, Max: %d)", wait_time, working_job.load(), max_worker);
			std::this_thread::sleep_for(std::chrono::seconds(wait_time));
			if (wait_time < 300)
				wait_time += increment;
		} else
			break;
	}
}

void VRInotify::waitForFinish_spk(const int max_worker, const int seconds, const int increment) {
	int wait_time = seconds;
	while (true) {
		if (max_worker <= working_job_spk.load()) {
			logger->debug("[SPK] Sleep for %ds (running: %d, Max: %d)", wait_time, working_job_spk.load(), max_worker);
			std::this_thread::sleep_for(std::chrono::seconds(wait_time));
			if (wait_time < 300)
				wait_time += increment;
		}
		else {
			break;
		}
	}
}

void VRInotify::waitForFinish_fs_threshold(const int max_worker, const size_t threshold_filesize, std::string filename, size_t filesize, const int seconds, const int increment) {
	int wait_time = seconds;
	while (true) {
		if (max_worker <= working_job.load()) {
			logger->debug("Sleep for %ds (running: %d, Max: %d, MulitIdx: %d)", wait_time, working_job.load(), max_worker);
			std::this_thread::sleep_for(std::chrono::seconds(wait_time));
			if (wait_time < 300)
				wait_time += increment;
		}
		else {
			// 파일사이즈 체크
			size_t sum_filesize = 0;
			typedef indexes_job_file_t::nth_index<0>::type::const_iterator const_iterator_t;
			for (const_iterator_t it = job_file_index.get<0>().begin(),
				iend = job_file_index.get<0>().end();
				it != iend;
				++it)
			{
				const ST_JOB_FILE& v_file = *it;
				(void)v_file;

				sum_filesize += v_file.filesize_;
			}

			if (threshold_filesize <= sum_filesize) {
				logger->debug("Sleep for %ds (running: %d, Max: %d, Multi_Idx: %d, (T)FileSize: %ld, (C)FileSize: %ld)", wait_time, working_job.load(), max_worker, job_file_index.size(), (unsigned long)threshold_filesize, (unsigned long)sum_filesize);
				std::this_thread::sleep_for(std::chrono::seconds(wait_time));
				if (wait_time < 180)
					wait_time += increment;
			}
			else {
				job_file_index.insert(ST_JOB_FILE(filename, filesize));
				logger->debug("[%s] " COLOR_GREEN "multi-index insert(count) - %d" COLOR_NC, filename.c_str(), job_file_index.size());
				break;
			}
		}
	}
}

std::string VRInotify::getDownloadURI(
	const common::Configuration *config,
	const char *pathname,
	const char *download_path,
	const std::string &format_string,
	const std::string data
) {
	std::shared_ptr<std::map<std::string, std::string>> metadata;
	try {
		metadata = config->parsingConfig(format_string, data);
	}
	catch (std::exception &e) {
		logger->error("[getFileInfo()] Cannot parsing. %s", e.what());
		return NULL;
	}

	// 다운로드 경로가 없는 경우 입력 경로로 설정 
	std::string tmp_input_path;
	if (download_path == NULL) {
		tmp_input_path = config->getConfig("inotify.input_path");
		download_path = const_cast<char *>(tmp_input_path.c_str());
	}

	std::string download_uri("");
	if (pathname != NULL) {
		// filename이 따로 설정된 경우 
		auto filename = metadata->find("filename");
		if (filename != metadata->end())
			download_uri = getDownloadPath(filename->second, metadata, download_path);
		else
			download_uri.append(pathname);
	}
	else {
		auto filename = metadata->find("filename");
		if (filename != metadata->end())
			download_uri = getDownloadPath(filename->second, metadata, download_path);
		else {
			logger->error("[getFileInfo()] Invalid index_format: need to filename");
			return NULL;
		}
	}
	logger->info("[getFileInfo()] Process %s", download_uri.c_str());

	return download_uri;
}

size_t VRInotify::getFileInfo(
	const common::Configuration *config,
	const std::string download_uri,
	const std::string *account) 
{
	size_t filesize = 0;

	//std::shared_ptr<std::map<std::string, std::string>> metadata;
	//try {
	//	metadata = config->parsingConfig(format_string, data);
	//}
	//catch (std::exception &e) {
	//	logger->error("[getFileInfo()] Cannot parsing. %s",  e.what());
	//	return -1;
	//}
	//
	//// 다운로드 경로가 없는 경우 입력 경로로 설정 
	//std::string tmp_input_path;
	//if (download_path == NULL) {
	//	tmp_input_path = config->getConfig("inotify.input_path");
	//	download_path = const_cast<char *>(tmp_input_path.c_str());
	//}
	//
	//std::string download_uri("");
	//if (pathname != NULL) {
	//	// filename이 따로 설정된 경우 
	//	auto filename = metadata->find("filename");
	//	if (filename != metadata->end())
	//		download_uri = getDownloadPath(filename->second, metadata, download_path);
	//	else
	//		download_uri.append(pathname);
	//}
	//else {
	//	auto filename = metadata->find("filename");
	//	if (filename != metadata->end())
	//		download_uri = getDownloadPath(filename->second, metadata, download_path);
	//	else {
	//		logger->error("[getFileInfo()] Invalid index_format: need to filename");
	//		return -1;
	//	}
	//}
	//logger->info("[getFileInfo()] Process %s", download_uri.c_str());
	///////////////////////////////////////////////////////////////////////////////////////////

	//std::string metadata1(std::string(download_uri, 10));
	std::string metadata1 = download_uri;
	logger->debug("[0x%X] metadata1 : %s", THREAD_ID, metadata1.c_str());

	// 로컬 파일
	std::string::size_type pos = metadata1.find("://");

	if (pos == std::string::npos) {
		filesize = get_file_size(download_uri.c_str());
		logger->debug("[0x%X] LOCAL FileName : %s, FileSize : %d", THREAD_ID, download_uri.c_str(), filesize);
		
		return filesize;
	}

	enum PROTOCOL protocol;
	try {
		logger->debug("[0x%X] PROTOCOL : %s", THREAD_ID, metadata1.substr(0, pos).c_str());
		protocol = protocol_list.at(metadata1.substr(0, pos));
	}
	catch (std::out_of_range &e) {
		// 스트리밍 데이터 | 미지원 프로토콜 
		logger->warn("[0x%X] Unsupported protocol: %s", THREAD_ID, metadata1.substr(0, pos).c_str());
		return -1;
	}

	std::string filename;
	if (protocol == PROTOCOL_FILE) { // 파일 리드 
		filename = std::string(download_uri).substr(7);
		logger->debug("[0x%X] PROTOCOL_FILE, Read file: %s", THREAD_ID, filename.c_str());

		std::FILE *fp = std::fopen(filename.c_str(), "rb");
		if (!fp) {
			int ret = errno;
			logger->error("%s: %s", std::strerror(ret), filename.c_str());
			throw std::runtime_error(std::strerror(ret));
		}
		std::shared_ptr<std::FILE> fd(fp, std::fclose);

		// read 
		std::fseek(fp, 0, SEEK_END);
		filesize = std::ftell(fp);

		logger->debug("[0x%X] PROTOCOL_FILE, FileName : %s, FileSize : %d", THREAD_ID, filename.c_str(), filesize);

		return filesize;
	}

	// 기타 프로토콜 정의(http, https, ftp, sftp)
	// 여기에서 curl 옵션 정의해서 header 정보 읽어서 파일 사이즈 추출하여 리턴
	if (protocol == PROTOCOL_FTP && config->getConfig<bool>("master.use_ftp_ssl", false))
		protocol = PROTOCOL_FTPS;

	std::lock_guard<std::mutex> guard(curl_lock);
	CURLcode response_code;
	CURL *_curl = curl_easy_init();
	if (!_curl)
		throw std::runtime_error("Cannot allocation CURL");
	std::shared_ptr<CURL> ctx(_curl, curl_easy_cleanup);
	double curl_fsize = 0.0;

	switch (protocol) {
	case PROTOCOL_FTPS:	// FTPS 프로토콜
		curl_easy_setopt(ctx.get(), CURLOPT_USE_SSL, CURLUSESSL_ALL);
		// CURLFTPAUTH_TLS // TLS first

	case PROTOCOL_SFTP:	// SFTP 프로토콜
	case PROTOCOL_FTP:	// FTP 프로토콜
		if (account && !account->empty()) {
			logger->debug("Account %s", account->c_str());
			curl_easy_setopt(ctx.get(), CURLOPT_USERPWD, account->c_str()); // User Password 설정 
		}

	case PROTOCOL_HTTPS:	// HTTPS 프로토콜
		if (config->getConfig<bool>("master.ssl_insecure", false))
			curl_easy_setopt(ctx.get(), CURLOPT_SSL_VERIFYPEER, 0);
	case PROTOCOL_HTTP:		// HTTP 프로토콜
		filename = std::string(download_uri);
		logger->debug("[0x%X] FileSize checking %s", THREAD_ID, filename.c_str());

		//// curl_easy_setopt(ctx.get(), CURLOPT_WRITEHEADER, stderr); // 헤더 출력 설정 
		//curl_easy_setopt(ctx.get(), CURLOPT_URL, filename.c_str());
		//curl_easy_setopt(ctx.get(), CURLOPT_NOBODY, 1L);
		//curl_easy_setopt(ctx.get(), CURLOPT_HEADERFUNCTION, throw_away);
		//curl_easy_setopt(ctx.get(), CURLOPT_HEADER, 0L);
		//curl_easy_setopt(ctx.get(), CURLOPT_NOPROGRESS, true);
		//// curl_easy_setopt(ctx.get(), CURLOPT_VERBOSE, true);

		curl_easy_setopt(ctx.get(), CURLOPT_URL, filename.c_str());
		curl_easy_setopt(ctx.get(), CURLOPT_WRITEFUNCTION, throw_away); // Write function 설정 
		curl_easy_setopt(ctx.get(), CURLOPT_HEADER, 0L);
		curl_easy_setopt(ctx.get(), CURLOPT_NOPROGRESS, true);
				
		response_code = curl_easy_perform(ctx.get());
		logger->debug("[0x%X] curl response code %d", THREAD_ID, response_code);
		if (response_code == CURLE_OK) {
			response_code = curl_easy_getinfo(ctx.get(), CURLINFO_CONTENT_LENGTH_DOWNLOAD, &curl_fsize);
			logger->debug("[0x%X] curl getinfo response code %d", THREAD_ID, response_code);
			if ((CURLE_OK == response_code) && (curl_fsize > 0.0)) {
				filesize = static_cast<size_t>(curl_fsize);
				logger->debug("[0x%X] FileName : %s, FileSize : %d", THREAD_ID, filename.c_str(), filesize);
			}
			else {
				filesize = -1;
			}
		}
		else {
			logger->alert("%s (%d)", curl_easy_strerror(response_code), response_code);
			throw std::runtime_error(curl_easy_strerror(response_code));
			filesize = -1;
		}

		break;

	default:	// 알 수 없는 프로토콜 
		logger->alert("[0x%X] Not implemented yet", THREAD_ID);
		throw std::domain_error("not implemented yet");
		filesize = -1;
		break;
	}

	return filesize;
}

//size_t VRInotify::getFileInfo(const itfact::common::Configuration *config,
//							const char *apiserver_uri, const char *pathname,
//							const char *download_path, const std::string &format_string,
//							const std::string data) {
//	size_t filesize = 0;
//	std::shared_ptr<std::map<std::string, std::string>> metadata;
//	try {
//		metadata = config->parsingConfig(format_string, data);
//	}
//	catch (std::exception &e) {
//		logger->error("[getFileInfo()] Cannot parsing. %s",  e.what());
//		return EXIT_FAILURE;
//	}
//
//	// 다운로드 경로가 없는 경우 입력 경로로 설정 
//	std::string tmp_input_path;
//	if (download_path == NULL) {
//		tmp_input_path = config->getConfig("inotify.input_path");
//		download_path = const_cast<char *>(tmp_input_path.c_str());
//	}
//
//	std::string download_uri("");
//	if (pathname != NULL) {
//		// filename이 따로 설정된 경우 
//		auto filename = metadata->find("filename");
//		if (filename != metadata->end())
//			download_uri = getDownloadPath(filename->second, metadata, download_path);
//		else
//			download_uri.append(pathname);
//	}
//	else {
//		auto filename = metadata->find("filename");
//		if (filename != metadata->end())
//			download_uri = getDownloadPath(filename->second, metadata, download_path);
//		else {
//			logger->error("[getFileInfo()] Invalid index_format: need to filename");
//			return EXIT_FAILURE;
//		}
//	}
//	logger->info("[getFileInfo()] Process %s", download_uri.c_str());
//
//	WorkerDaemon::getFileInfo()
//	
//
//	return 0;
//}

/**
 * @brief		Event 처리 
 * @details		iNotify에 의해 확인된 녹취 파일을 분석 처리 
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 07. 05. 10:55:57
 * @param[in]	path		파일 위치 
 * @param[in]	filename	파일명 
 * @param[in]	config		서버 설정 
 * @return		Upon successful completion, a EXIT_SUCCESS is returned.\n
 				Otherwise, a EXIT_FAILURE is returned.
 */
int VRInotify::runJob(const std::shared_ptr<std::string> path,
					  const std::shared_ptr<std::string> filename,
					  const itfact::common::Configuration *config) {
	int rc;
	std::string name(filename->c_str(), filename->rfind("."));
	char *output_path = NULL;
	std::string output_pathname;
	if (config->isSet("inotify.output_path")) {
		output_pathname = config->getConfig("inotify.output_path");
		const bool unique = config->getConfig<bool>("inotify.unique_output", default_config.unique_output);
		if (config->getConfig<bool>("inotify.daily_output", default_config.daily_output) || unique) {
			time_t now = time(0);
			tm *ltm = localtime(&now);
			if (output_pathname[output_pathname.size() - 1] != '/')
				output_pathname.push_back('/');
			output_pathname.append(boost::lexical_cast<std::string>(1900 + ltm->tm_year)); // Year
			output_pathname.push_back('/');
			output_pathname.append(boost::lexical_cast<std::string>(1 + ltm->tm_mon)); // Month
			output_pathname.push_back('/');
			output_pathname.append(boost::lexical_cast<std::string>(ltm->tm_mday)); // Day

			if (unique) {
				output_pathname.push_back('/');
				if (ltm->tm_hour < 10)
					output_pathname.push_back('0');
				output_pathname.append(boost::lexical_cast<std::string>(ltm->tm_hour)); // Hour
				if (ltm->tm_min < 10)
					output_pathname.push_back('0');
				output_pathname.append(boost::lexical_cast<std::string>(ltm->tm_min)); // Minute
				if (ltm->tm_sec < 10)
					output_pathname.push_back('0');
				output_pathname.append(boost::lexical_cast<std::string>(ltm->tm_sec)); // Second
				output_pathname.push_back('_');
				output_pathname.append(boost::lexical_cast<std::string>(std::clock() & (CLOCKS_PER_SEC - 1)));
				// output_pathname.append(boost::lexical_cast<std::string>(std::clock()));
			}
		}

		itfact::common::checkPath(output_pathname, true);
		output_path = const_cast<char *>(output_pathname.c_str());
	}

	std::string apiserver(config->getConfig("api.url", default_config.apiserver_url.c_str()));
	if (apiserver[apiserver.size() - 1] != '/')
		apiserver.push_back('/');
	apiserver.append(config->getConfig("api.service", default_config.api_service.c_str()));
	if (apiserver[apiserver.size() - 1] != '/')
		apiserver.push_back('/');
	apiserver.append(config->getConfig("api.version", default_config.apiserver_version.c_str()));
	apiserver.append("/jobs");

	// 분석할 파일 
	std::string pathname(path->c_str());
	if (pathname[pathname.size() - 1] != '/')
		pathname.push_back('/');
	pathname.append(filename->c_str());

	char *download_path = NULL;
	if (config->isSet("inotify.download_path"))
		download_path = const_cast<char *>(config->getConfig("inotify.download_path").c_str());

	std::string format_string;
	try {
		format_string = config->getConfig("inotify.index_format");
		logger->debug("index_format: %s", format_string.c_str());
	} catch (std::exception &e) {
		logger->error("Please check inotify.index_type and inotify.index_format");
		return EXIT_FAILURE;
	}

	// preprocess
	if (config->isSet("inotify.preprocess")) {
		std::string cmd(config->getConfig("inotify.preprocess"));
		const char *proc = config->getConfig("inotify.preprocess").c_str();
		cmd.push_back(' ');
		cmd.append(pathname.c_str());
		cmd.push_back(' ');

		// new pathname 
		pathname = "/tmp/";
		pathname.append(boost::lexical_cast<std::string>(std::this_thread::get_id()));
		pathname.append(".txt");

		cmd.append(pathname.c_str());
		logger->debug(cmd.c_str());
		rc = std::system(cmd.c_str());
		if (rc) {
			logger->error("Error(%d) occurred during execution '%s'", rc, proc);
			return EXIT_FAILURE;
		}
	}

	std::shared_ptr<std::map<std::string, std::string>> metadata;
	if (config->getConfig("inotify.index_type").compare("filename") == 0) {
		VRInotify::waitForFinish(available_workers.load(), 5, 5);
		std::string tmp_input_path = config->getConfig("inotify.input_path");
		download_path = const_cast<char *>(tmp_input_path.c_str());
		working_job.fetch_add(1);
		rc = VRInotify::processRequest( config, apiserver.c_str(), pathname.c_str(), download_path,
										format_string, *filename.get(), output_path);
	} else if (config->getConfig("inotify.index_type").compare("file") == 0) {
		VRInotify::waitForFinish(available_workers.load(), 5, 5);
		std::ifstream index_file(pathname);
		if (index_file.is_open()) {
			std::string::size_type idx = pathname.rfind(".");
			std::string wav_pathname(pathname.c_str(), (idx != std::string::npos ? idx : pathname.size()));
			wav_pathname.push_back('.');
			wav_pathname.append(config->getConfig("inotify.rec_ext", default_config.rec_ext.c_str()));
			std::string line;
			std::getline(index_file, line);
			working_job.fetch_add(1);
			rc = VRInotify::processRequest(config, apiserver.c_str(), wav_pathname.c_str(), download_path,
										   format_string, line, output_path);
		}
		index_file.close();
	} else if (config->getConfig("inotify.index_type").compare("pair") == 0) {
		// FIXME: 채널이 분리된 경우의 처리 
	} else if (config->getConfig("inotify.index_type").compare("list") == 0) {
		std::ifstream index_file(pathname);
		if (index_file.is_open()) {
			logger->info("Request STT with list '%s'", filename->c_str());
			std::set<std::thread::id> jobs;			

			size_t threshold_file_size = 0;
			// 파일사이즈 임계치 설정값 확인
			if (config->getConfig("inotify.fs_threshold_yn").compare("y") == 0) {
				std::string tmp_threshold_filesize = config->getConfig("inotify.fs_threshold");
				int f_threshold_size = std::stoi(tmp_threshold_filesize.substr(0, tmp_threshold_filesize.length() - 3), nullptr, 0);
				threshold_file_size = f_threshold_size * 1024 * 1024;
			}

			for (std::string line; std::getline(index_file, line); ) {
				if (line.empty() || line.size() < 5)
					continue;

				std::string download_uri = ""; 

				// 작업할 파일 사이즈 정보 추출 
				// 환경설정의 inotify 섹션에서 fs_threshold_yn 값이 y로 셋팅이 된 경우 처리
				if (config->getConfig("inotify.fs_threshold_yn").compare("y") == 0) {
					download_uri = getDownloadURI(config, (const char *)NULL, download_path, format_string, line);
					size_t filesize = getFileInfo(config, download_uri);
					if (filesize > 0) {
						VRInotify::waitForFinish_fs_threshold(available_workers.load(), threshold_file_size, download_uri, filesize, 1, 1);
					}
				}
				else {
					VRInotify::waitForFinish(available_workers.load(), 1, 1);
				}

				// FIXME: 동시 처리 문제를 확인하기 위한 코드 
				while (jobs.size() > 10000) {
					logger->warn("Waiting for end (# of running, remain: %lu)", working_job.load(), jobs.size());
					std::this_thread::sleep_for(std::chrono::seconds(5));
				}

				try {
					working_job.fetch_add(1);
					std::thread job(VRInotify::processRequest, config, apiserver.c_str(),
									(const char *) NULL, download_path,
									format_string, line, output_path, &jobs);
					jobs.insert(job.get_id());
					job.detach();
				} catch (std::exception &e) {
					logger->warn("Job error: %s(%d) (#job: %d, running: %d)",
								 e.what(), errno, jobs.size(), working_job.load());
					break;
				}
			}
			index_file.close();

			while (jobs.size() > 0)
				std::this_thread::sleep_for(std::chrono::seconds(1));

			rc = EXIT_SUCCESS;
			logger->info("Done: %s", filename->c_str());

			/*
			// job_check 디렉토리의 파일 삭제
			//if (config->isSet("inotify.job_check") && config->getConfig("inotify.job_check").compare("true") == 0) {
			if (config->isSet("inotify.job_check")) {
				if (config->getConfig("inotify.job_check").compare("true") == 0) {
					std::shared_ptr<std::string> inotify_recheck_path = std::make_shared<std::string>(config->getConfig("inotify.job_check_path"));
					std::string remove_file("");
					remove_file.append(inotify_recheck_path.get()->c_str());
					remove_file.push_back('/');
					remove_file.append(filename->c_str());
					std::remove(remove_file.c_str());
					logger->info("Remove Done: %s", remove_file.c_str());
				}
			}
			*/

		} else {
			rc = errno;
			logger->error("Cannot read '%s'", pathname.c_str());
			return EXIT_FAILURE;
		}
	}

	// postprocess
	if (rc == EXIT_SUCCESS && config->isSet("inotify.postprocess")) {
		std::string cmd(config->getConfig("inotify.postprocess"));
		const char *proc = config->getConfig("inotify.postprocess").c_str();
		cmd.push_back(' ');
		cmd.append(pathname);
		cmd.push_back(' ');
		cmd.append(output_pathname);

		logger->debug(cmd.c_str());
		rc = std::system(cmd.c_str());
		if (rc) {
			logger->error("Error(%d) occurred during execution '%s'", rc, proc);
			return EXIT_FAILURE;
		}
	}

	return rc;
}

int VRInotify::jobMonitor(const std::shared_ptr<std::string> path,
	const std::shared_ptr<std::string> monitor_path,
	const itfact::common::Configuration *config) 
{
	//std::this_thread::sleep_for(std::chrono::seconds(10));
	
	//if (job_check_flag == false) {
		//if (config->isSet("inotify.job_check") && config->getConfig("inotify.job_check").compare("true") == 0) {
	if (config->isSet("inotify.job_check")) {
		if (config->getConfig("inotify.job_check").compare("true") == 0) {
			DIR *dir;
			struct dirent *ent;
			char* check_path = (char *)monitor_path.get()->c_str();
			if ((dir = opendir(check_path)) != NULL) { /* 디렉토리를 열 수 있는 경우 */
														/* 디렉토리 안에 있는 모든 파일&디렉토리 출력 */

				while ((ent = readdir(dir)) != NULL) {
					if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
						continue;

					std::string org_file("");
					org_file.append(check_path);
					org_file.push_back('/');
					org_file.append(ent->d_name);
					logger->info("Recheck file copy : %s", org_file.c_str());

					std::string target_file("");
					target_file.append(path.get()->c_str());
					target_file.push_back('/');
					target_file.append(ent->d_name);

					copyFile((char *)org_file.c_str(), (char *)target_file.c_str());

					std::string cmd("");
					cmd.append("touch");
					cmd.push_back(' ');
					cmd.append(target_file.c_str());

					int rc = std::system(cmd.c_str());
					if (rc) {
						logger->error("Error(%d) occurred during execution '%s'", rc, cmd.c_str());
						return EXIT_FAILURE;
					}
				}
				closedir(dir);

				job_check_flag = true;
			}
			else { /* 디렉토리를 열 수 없는 경우 */
				logger->error("Cannot open dir : %s", check_path);
				return EXIT_FAILURE;
			}
		}
	}
	//}	

	return EXIT_SUCCESS;
}

/**
 * @brief		iNotify 초기화
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 06. 30. 17:33:12
 * @param[in]	path	모니터링할 경로
 * @return		Upon successful completion, a EXIT_SUCCESS is returned.\n
 				Otherwise,
 				a positive error code is returned indicating what went wrong.
 */
int VRInotify::monitoring() {	
	//bool job_check_flag = true;

	char buf[BUF_LEN] __attribute__ ((aligned(8)));
	memset(buf, 0x00, BUF_LEN);

	if (!config.isSet("inotify.input_path")) {
		logger->fatal("Not set inotify.input_path");
		return EXIT_FAILURE;
	}	

	std::shared_ptr<std::string> path = std::make_shared<std::string>(config.getConfig("inotify.input_path"));
	logger->info("Initialize monitoring module to watch %s", path->c_str());
	if (!itfact::common::checkPath(*path.get(), true)) {
		logger->error("Cannot create directory '%s' with error %s", path->c_str(), std::strerror(errno));
		return EXIT_FAILURE;
	}

	// 프로세스 재설정에 따른 처리
	//std::shared_ptr<std::string> inotify_recheck_path = std::make_shared<std::string>(config.getConfig("inotify.job_check_path"));
	/*
	std::shared_ptr<std::string> inotify_recheck_path;
	if (config.isSet("inotify.job_check")) {
		inotify_recheck_path = std::make_shared<std::string>(config.getConfig("inotify.job_check_path"));
		if (config.getConfig("inotify.job_check").compare("true") == 0) {
			logger->info("Initialize monitoring module to job recheck dir %s", inotify_recheck_path->c_str());
			if (!itfact::common::checkPath(*inotify_recheck_path.get(), true)) {
				logger->error("Cannot create directory '%s' with error %s", inotify_recheck_path->c_str(), std::strerror(errno));
				return EXIT_FAILURE;
			}
		}
	}
	*/

	int inotify = inotify_init();
	if (inotify < 0) {
		int rc = errno;
		logger->error("Cannot initialization iNotify '%s'", std::strerror(rc));
		return rc;
	}

	// IN_MOVED_TO
	int wd = inotify_add_watch(inotify, path->c_str(), IN_CLOSE_WRITE);
	if (wd < 0) {
		int rc = errno;
		logger->error("Cannot watch '%s' with error %s", path->c_str(), std::strerror(rc));
		return rc;
	}

	// 가용한 워커 수 확인 (설정된 값을 우선시)
	if (config.isSet("inotify.maximum_jobs")) {
		available_workers = config.getConfig<unsigned long>("inotify.maximum_jobs", available_workers.load());
	} else {
		std::thread nr_worker(getTotalWorkers);
		nr_worker.detach();

		if (config.isSet("spk.enable") && config.getConfig("spk.enable").compare("true") == 0) {
			std::thread nr_worker_spk(getTotalWorkers_spk);
			nr_worker_spk.detach();
		}
	}	

	std::string watch_ext = config.getConfig("inotify.watch", default_config.watch.c_str());
	while (true) {		

		/*
		// job 재처리 쓰레드 호출
		if (job_check_flag == true) {
			std::thread job_monitor(VRInotify::jobMonitor, path, inotify_recheck_path, &config);
			job_monitor.detach();
			job_check_flag = false;
		}
		*/

		ssize_t numRead = read(inotify, buf, BUF_LEN);
		if (numRead <= 0) {
			int rc = errno;
			if (rc == EINTR)
				continue;

			logger->warn("Error occurred: (%d), %s", rc, std::strerror(rc));
			return rc;
		}

		struct inotify_event *event = NULL;
		for (char *p = buf; p < buf + numRead; p += sizeof(struct inotify_event) + event->len) {
			event = (struct inotify_event *) p;		

			if (!(event->mask & IN_ISDIR)) {
				// Call Job
				std::shared_ptr<std::string> filename = std::make_shared<std::string>(event->name, event->len);
				std::string file_ext = filename->substr(filename->rfind(".") + 1);				

				if (filename->at(0) != '.' && file_ext.find(watch_ext) == 0 &&
					(file_ext.size() == watch_ext.size() || file_ext.at(watch_ext.size()) == '\0' )) {
					try {
						/*
						// 프로세스 재처리 설정이 있는 경우
						//if (config.isSet("inotify.job_check") && config.getConfig("inotify.job_check").compare("true") == 0) {
						if (config.isSet("inotify.job_check")) {
							if (config.getConfig("inotify.job_check").compare("true") == 0) {
								std::string target_file("");
								target_file.append(inotify_recheck_path.get()->c_str());
								target_file.push_back('/');
								target_file.append(filename->c_str());
								copyFile((char *)filename->c_str(), (char *)target_file.c_str());
							}
						}
						*/

						std::thread job(VRInotify::runJob, path, filename, &config);
						job.detach();
					} catch (std::exception &e) {
						logger->warn("%s: %s", e.what(), filename->c_str());
					}
				} else {
					logger->debug("Ignore %s (Watch: '%s', ext: '%s')", filename->c_str(),
									watch_ext.c_str(), file_ext.c_str());
				}
			}
		}
	}

	inotify_rm_watch(inotify, wd);
	return EXIT_SUCCESS;
}
