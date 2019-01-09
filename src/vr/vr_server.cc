/**
 * @file	vr_server.cc
 * @brief	VR Server
 * @details	
 * @author	Kijeong Khil (kjkhil@itfact.co.kr)
 * @date	2016. 06. 17. 17:32:24
 * @see		
 */
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/lexical_cast.hpp>

#include "ETRIPP.h"
#include "vr.hpp"
#include "restapi.hpp"

#include "chilkat/CkGlobal.h"
#include "chilkat/CkCrypt2.h"
#include "chilkat/CkSFtp.h"
#include "chilkat/CkFtp2.h"

#define THREAD_ID	std::this_thread::get_id()
#define LOG_INFO	__FILE__, __FUNCTION__, __LINE__
#define LOG_FMT		" [at %s (%s:%d)]"

using namespace itfact::vr::node;

static log4cpp::Category *job_log = NULL;
static std::string tmp_path;

static gearman_return_t job_stt(gearman_job_st *, void *);
static gearman_return_t job_unsegment(gearman_job_st *, void *);
static gearman_return_t job_unsegment_with_time(gearman_job_st *, void *);
static gearman_return_t job_ssp(gearman_job_st *job, void *context);
static gearman_return_t job_rt_stt(gearman_job_st *job, void *context);

static CkSFtp sftp;
static CkFtp2 ftp;

static const std::string unlock_license_key = "ITFACT.CBX012020_ducukXVn0eol";
static const std::string stt_secret_key = "itfact_stt_sec_key!@";

// 디코딩을 위한 함수 
//extern int ITFACT_ETC_Init_Convert_RECFile(void);
//extern int ITFACT_ETC_Convert_RECFile(char *ORGFile, char *TARFile, int WAVHEADER);


// 2018-06-15 LSTM 추가
static struct {
	std::string laser_config;
	std::string frontend_config;
	std::string sil_dnn;
	std::string am_filename;	// amFn
	std::string fsm_filename;	// fsmFn
	std::string sym_filename;	// symFn
	std::string dnn_filename;	// dnnFn
	std::string prior_filename;	// priFn
	std::string norm_filename;	// normFn
	std::string chunking_filename;
	std::string tagging_filename;
	std::string user_dic;
	std::string tmp_path;
	std::string fail_nofile;
	std::string fail_download;
	std::string fail_decoding;
} default_config = {
	.laser_config = "config/stt_laser.cfg",
	.frontend_config = "config/frontend_dnn.cfg",
	.sil_dnn = "config/sil_dnn.data",
	.am_filename = "stt_release.sam.bin",
	.fsm_filename = "stt_release.sfsm.bin",
	.sym_filename = "stt_release.sym.bin",
	.dnn_filename = "final.dnn.adapt",
	.prior_filename = "final.dnn.prior.bin",
	.norm_filename = "final.dnn.lda.bin",
	.chunking_filename = "chunking.release.bin",
	.tagging_filename = "tagging.release.bin",
	.user_dic = "user_dic.txt",
	.tmp_path = "/dev/shm/smart-vr",
	.fail_nofile = "E10100",
	.fail_download = "E10200",
	.fail_decoding = "E20400",
};

/**
 */
int main(const int argc, char const *argv[]) {
	try {
		VRServer server(argc, argv);
		server.initialize();
	} catch (std::exception &e) {
		perror(e.what());
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

VRServer::~VRServer() {
	if (master_laser)
		unloadLaserModule();
}

static inline std::string decrypt_passwd(std::string encrypt_passwd) {
	std::string passwd("");

	CkCrypt2 crypt;

	//  AES is also known as Rijndael.
	crypt.put_CryptAlgorithm("aes");

	//  CipherMode may be "ecb", "cbc", "ofb", "cfb", "gcm", etc.
	//  Note: Check the online reference documentation to see the Chilkat versions
	//  when certain cipher modes were introduced.
	crypt.put_CipherMode("cbc");

	//  KeyLength may be 128, 192, 256
	crypt.put_KeyLength(256);
	crypt.put_PaddingScheme(0);
	crypt.put_EncodingMode("hex");
	//crypt.SetEncodedIV("000102030405060708090A0B0C0D0E0F", "hex");
	//crypt.SetEncodedKey("000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", "hex");
	crypt.SetEncodedIV(stt_secret_key.c_str(), "hex");
	crypt.SetEncodedKey(stt_secret_key.c_str(), "hex");
	//  Now decrypt:
	passwd = crypt.decryptStringENC(encrypt_passwd.c_str());
	// chilkat Encryption 부분 이용 (AES256) - 끝

	return passwd;
}

bool VRServer::init_sftp(std::string _host, std::string _port, std::string _id, std::string _passwd, bool bEncrypt)
{
	/////////////////////////////////////////////////////////////////////////////////////////////
	// chilkat Encryption 부분 이용 (AES256) - 시작
	std::string passwd("");
	if (bEncrypt == true) {
		// 암호화된 패스워드 decryption 요청
		passwd = decrypt_passwd(_passwd);
		if (passwd.size() == 0)
			return false;
	}
	else {
		passwd = _passwd;
	}
	job_log->debug("sftp password : %s", passwd.c_str());
	/////////////////////////////////////////////////////////////////////////////////////////////

	// Set some timeouts, in milliseconds:
	sftp.put_ConnectTimeoutMs(1000 * 60 * 3);	// 3분 대기
	sftp.put_IdleTimeoutMs(0);					// 무제한 대기

												//  Connect to the SSH server.
												//  The standard SSH port = 22
												//  The hostname may be a hostname or IP address.
	int port;
	const char *hostname = 0;
	hostname = _host.c_str();
	port = std::stoi(_port);
	bool success = sftp.Connect(hostname, port);
	if (success != true) {
		job_log->error("sftp fail : %s", sftp.lastErrorText());
		return success;
	}

	//  Authenticate with the SSH server.  Chilkat SFTP supports
	//  both password-based authenication as well as public-key
	//  authentication.  This example uses password authenication.
	success = sftp.AuthenticatePw(_id.c_str(), passwd.c_str());
	if (success != true) {
		job_log->error("sftp fail : %s", sftp.lastErrorText());
		return success;
	}

	//  After authenticating, the SFTP subsystem must be initialized:
	success = sftp.InitializeSftp();
	if (success != true) {
		job_log->error("sftp fail : %s", sftp.lastErrorText());
		return success;
	}

	while (true) {
		success = sftp.get_IsConnected();
		if (success != true) {
			job_log->info("sftp connetion waiting ....");
			std::this_thread::sleep_for(std::chrono::seconds(3));
		}
		else
			break;
	}

	job_log->debug("sftp connection OK !!!");

	return success;
}

bool VRServer::init_ftps(std::string _host, std::string _port, std::string _id, std::string _passwd, bool bEncrypt)
{
	/////////////////////////////////////////////////////////////////////////////////////////////
	job_log->debug("#######################################");
	// chilkat Encryption 부분 이용 (AES256) - 시작
	std::string passwd("");
	if (bEncrypt == true) {
		// 암호화된 패스워드 decryption 요청
		passwd = decrypt_passwd(_passwd);
		if (passwd.size() == 0)
			return false;
	}
	else {
		passwd = _passwd;
	}
	job_log->debug("ftps password : %s", passwd.c_str());
	/////////////////////////////////////////////////////////////////////////////////////////////

	// Set some timeouts, in milliseconds:
	//ftp.put_ConnectTimeout(1000 * 60 * 5);		// 3분 대기
	ftp.put_ConnectTimeout(0);		// 3분 대기
	ftp.put_IdleTimeoutMs(0);					// 무제한 대기

												//  Connect to the SSH server.
												//  The standard SSH port = 22
												//  The hostname may be a hostname or IP address.
	int port;
	const char *hostname = 0;
	hostname = _host.c_str();
	port = std::stoi(_port);

	ftp.put_Passive(false);
	ftp.put_Hostname(hostname);
	ftp.put_Username(_id.c_str());
	ftp.put_Password(passwd.c_str());
	ftp.put_Port(port);

	//  We don't want AUTH SSL:
	ftp.put_AuthTls(true);

	//  We want Implicit SSL:
	ftp.put_Ssl(false);

	bool success = ftp.Connect();
	if (success != true) {
		job_log->error("ftps fail : %s", ftp.lastErrorText());
		return success;
	}

	job_log->debug("ftps connection OK !!!");

	return success;
}

/**
 * @brief		초기화 후 실행 
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 06. 27. 10:12:37
 * @param[in]	argc	인수의 개수 
 * @param[in]	argc	인수 배열 
 * @return		Upon successful completion, a EXIT_SUCCESS is returned.\n
 				Otherwise,
 				a negative error code is returned indicating what went wrong.
 */
int VRServer::initialize() {
	const itfact::common::Configuration *config = getConfig();
	std::string image_path = "./";
	
	server_name = getConfig()->getConfig("stt.server_name", "DEFAULT");
	image_path = config->getConfig("stt.image_path", image_path.c_str());
	if (image_path.at(image_path.size() - 1) != '/')
		image_path.push_back('/');

	job_log = getLogger();
	job_log->info("Smart VR Server v1.0 initialize");
	job_log->debug("=========================================");
	job_log->debug("========== Check configuration ==========");
	job_log->debug("=========================================");

	job_log->debug("stt.server_name: %s", server_name.c_str());
	job_log->debug("stt.worker: %d", getTotalWorkers("stt"));
	job_log->debug("unsegment.worker: %d", getTotalWorkers("unsegment"));
	job_log->debug("realtime.worker: %d, startnum(%d)", getTotalWorkers("realtime"), config->getConfig("realtime.startnum", 1));

	// 설정값 로드 
	mfcc_size = config->getConfig("stt.mfcc_size", mfcc_size);
	mini_batch = config->getConfig("stt.mini_batch", mini_batch);
	prior_weight = config->getConfig("stt.prior_weight", prior_weight);
	useGPU = config->getConfig("stt.useGPU", useGPU);
	idGPU = config->getConfig("stt.idGPU", idGPU);
	if (mini_batch > MAX_MINIBATCH)
		mini_batch = MAX_MINIBATCH;
	feature_dim = mfcc_size * mini_batch;

	job_log->debug("stt.mfcc_size: %d, stt.mini_batch: %d, stt.prior_weight: %f",
					mfcc_size, mini_batch, prior_weight);

	job_log->debug("stt.useGPU: %s, stt.idGPU: %d", (useGPU) ? "true" : "false" , idGPU);

	std::string chunking_file("");
	std::string tagging_file("");
	std::string user_dic_file("");

	std::string laser_config = config->getConfig("stt.laser_config", default_config.laser_config.c_str());
	frontend_config = config->getConfig("stt.frontend_config", default_config.frontend_config.c_str());
	std::string sil_dnn = config->getConfig("stt.sil_dnn", default_config.sil_dnn.c_str());

	job_log->debug("stt.laser_config: %s", laser_config.c_str());
	job_log->debug("stt.frontend_config: %s", frontend_config.c_str());
	job_log->debug("stt.sil_dnn: %s", sil_dnn.c_str());

	am_file = std::string(image_path).
		append(config->getConfig("stt.am_filename", default_config.am_filename.c_str()));
	fsm_file = std::string(image_path).
		append(config->getConfig("stt.fsm_filename", default_config.fsm_filename.c_str()));
	sym_file = std::string(image_path).
		append(config->getConfig("stt.sym_filename", default_config.sym_filename.c_str()));
	dnn_file = std::string(image_path).
		append(config->getConfig("stt.dnn_filename", default_config.dnn_filename.c_str()));
	prior_file = std::string(image_path).
		append(config->getConfig("stt.prior_filename", default_config.prior_filename.c_str()));
	norm_file = std::string(image_path).
		append(config->getConfig("stt.norm_filename", default_config.norm_filename.c_str()));

	chunking_file = std::string(image_path).
		append(config->getConfig("stt.chunking_filename", default_config.chunking_filename.c_str()));
	tagging_file = std::string(image_path).
		append(config->getConfig("stt.tagging_filename", default_config.tagging_filename.c_str()));
	user_dic_file = std::string(image_path).
		append(config->getConfig("stt.user_dic", default_config.user_dic.c_str()));

	tmp_path = config->getConfig("master.tmp_path", default_config.tmp_path.c_str());
	if (tmp_path.at(tmp_path.size() - 1) != '/')
		tmp_path.push_back('/');
	itfact::common::checkPath(tmp_path, true);

	job_log->debug("stt.am_filename: %s", am_file.c_str());
	job_log->debug("stt.fsm_filename: %s", fsm_file.c_str());
	job_log->debug("stt.sym_filename: %s", sym_file.c_str());
	job_log->debug("stt.dnn_filename: %s", dnn_file.c_str());
	job_log->debug("stt.prior_filename: %s", prior_file.c_str());
	job_log->debug("stt.norm_filename: %s", norm_file.c_str());
	job_log->debug("stt.chunking_filename: %s", chunking_file.c_str());
	job_log->debug("stt.tagging_filename: %s", tagging_file.c_str());
	job_log->debug("stt.user_dic: %s", user_dic_file.c_str());
	job_log->debug("=========================================");

	// Chilkat Library Unlock
	CkGlobal glob;
	if (config->isSet("protocol.use") && config->getConfig<bool>("protocol.use", true)) {
		job_log->debug("protocol env section use");
		bool success = glob.UnlockBundle(unlock_license_key.c_str());
		if (success != true) {
			job_log->error("chilkat unlock error : %s", glob.lastErrorText());
			return EXIT_FAILURE;
		}

		if (config->getConfig("protocol.type").compare("sftp") == 0) {
			std::string host = config->getConfig("protocol.host");
			std::string port = config->getConfig("protocol.port");
			std::string uname = config->getConfig("protocol.username");
			std::string passwd = config->getConfig("protocol.password");
			std::string encrypt_flag = config->getConfig("protocol.encrypt");
			bool bEncrypt = false;
			if (encrypt_flag.compare("false") == 0)
				bEncrypt = false;
			else
				bEncrypt = true;

			if (this->init_sftp(host, port, uname, passwd, bEncrypt) == false) {
				job_log->error("Connect fail SFTP server !!!!");
				return EXIT_FAILURE;
			}
		}

		if (config->getConfig("protocol.type").compare("ftps") == 0) {
			job_log->debug("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
			std::string host = config->getConfig("protocol.host");
			std::string port = config->getConfig("protocol.port");
			std::string uname = config->getConfig("protocol.username");
			std::string passwd = config->getConfig("protocol.password");
			std::string encrypt_flag = config->getConfig("protocol.encrypt");
			bool bEncrypt = false;
			if (encrypt_flag.compare("false") == 0)
				bEncrypt = false;
			else
				bEncrypt = true;

			if (this->init_ftps(host, port, uname, passwd, bEncrypt) == false) {
				job_log->error("Connect fail FTPS server !!!!");
				return EXIT_FAILURE;
			}
		}
	}

	//FIXME: License 체크 

	// module 초기화 
	if (!loadLaserModule()) {
		job_log->fatal("Cannot load LASER module");
		return EXIT_FAILURE;
	}

	// Controller 실행 
	//RestApi api(config, job_log);
	//api.start();

	unsigned long useg_worker = getTotalWorkers("unsegment");

	job_log->info("Connect to Master server(%s:%d)", config->getHost().c_str(), config->getPort());
	run("vr_stt", this, getTotalWorkers("stt"), job_stt);
	run("vr_text_only", this, useg_worker, job_unsegment);
	run("vr_text", this, useg_worker, job_unsegment_with_time);
	run("vr_ssp", this, getTotalWorkers("ssp"), job_ssp);
	run("vr_realtime", this, getTotalWorkers("realtime"), job_rt_stt);

#ifdef USE_REALTIME_POOL
	for(int i=0; i<getTotalWorkers("realtime"); i++) {
		create_channel(std::to_string(i), channel.size());
	}
#endif

	job_log->info("Done");
	join();

#ifdef USE_REALTIME_POOL
	for(int i=0; i<getTotalWorkers("realtime"); i++) {
		close_channel(std::to_string(i));
	}
#endif

	// module 종료 
	job_log->info("Release server");
	unloadLaserModule();

	return EXIT_SUCCESS;
}

static inline std::string __get_file_extname(std::string filename) {
	std::string ext_name = "";
	ext_name = filename.substr(filename.find_last_of(".") + 1);
	return ext_name;
}

/*
static inline int __stringicompare(const char *p_string1, const char *p_string2) {
	// p_string1과 p_string2 포인터가 마지막에 비교했던 문자가 다른 경우,
	// 이 문자의 변환 값을 기억할 변수들을 선언
	char last_char1, last_char2;

	// 이 반복문은 무한루프로 구성
	while (1) {
		// 같은 순서에 있는 문자가 서로 다른지를 체크
		if (*p_string1 != *p_string2) {
			// *p_string1과 *p_string2가 가리키는 문자가 대문자인지 체크하여 
			// 대문자이면 32를 더하고 대문자가 아니면 그냥 자신의 값을 대입한다.
			last_char1 = *p_string1 + 32 * (*p_string1 >= 'A' && *p_string1 <= 'Z');
			last_char2 = *p_string2 + 32 * (*p_string1 >= 'A' && *p_string1 <= 'Z');
			// 변경한 두 문자가 서로 다르다면 비교를 중단한다.
			if (last_char1 != last_char2) break;
		}

		// p_string1을 구성하는 문자가 null 문자인 경우에는 반복을 중단한다.
		if (*p_string1 == 0) break;
		p_string1++;
		p_string2++;

		// 두 포인터가 마지막에 비교했던 문자가 동일하다면 두 문자열은 동일하다는 뜻이다
		// 즉 반복문이 'if(*p_string1 == 0) break;'에 의해 종료되었다는 뜻이다
		// 두 문자열이 동일한 경우에는 0을 반환
		if (*p_string1 == *p_string2) return 0;

		return last_char1 - last_char2;	// 두 문자의 ASCII 값을 빼서 반환
	}
}
*/

/**
 * @brief		파일 저장 
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 10. 14. 17:55:25
 * @param[in]	job_name	Job name
 * @return		Upon successful completion, a TRUE is returned.\n
 				Otherwise, a FALSE is returned.
 * @see			job_stt()
 */
static inline bool
__store_file(gearman_job_st *job, const char *job_name, const bool is_wave,
			 const short *data, const size_t size,
			 const char *workload, const size_t workload_size,
			 const enum PROTOCOL protocol, std::string &input_file) {
	if (protocol == PROTOCOL_FILE) {
		std::string::size_type idx = std::string(workload, 10).find("://");
		if (idx == std::string::npos) {
			job_log->error("[%s] Cannot decoding: %s", job_name, workload);
			return false;
		}

		input_file = std::string(workload, workload_size).substr(idx + 3);
	} else {
		/*
		// 로컬 파일이 이니므로 파일 저장 
		input_file = tmp_path;
		input_file.push_back('/');
		input_file.append(gearman_job_handle(job));
		input_file.push_back('.');
		if (is_wave)
			input_file.append("wav");
		else
			input_file.append("mp3");


		job_log->debug("[%s] Write to %s", job_name, input_file.c_str());
		std::FILE *fp = std::fopen(input_file.c_str(), "wb");
		if (!fp) {
			job_log->error("[%s] Cannot write: %s", job_name, std::strerror(errno));
			return false;
		}
		std::shared_ptr<std::FILE> fd(fp, std::fclose);

		for (size_t i = 0; i < size; ++i) {
			if (std::fwrite((char *) &data[i], sizeof(short), 1, fd.get()) == 0)
				job_log->warn("[%s] Write size is zero: %s", job_name, std::strerror(errno));
		}
		*/

		// 기존 쓰레드값으로 다운로드 파일을 저장하던 부분을 다운로드 실제 파일명으로 저장하도록 변경
		job_log->debug("[%s] workload: %s", job_name, workload);
		std::string down_fn;
		std::string tmp_fn = std::string(workload, workload_size);
		std::vector<std::string> tmp_vec;

		// 20180719 파일 확장자 체크 부분 추가
		bool bSoundFile_Ext = false;	// wav, mp3 파일 확장자인지 체크
		std::string file_extname = __get_file_extname(tmp_fn);
		// 확장자 비교
		if (is_wave) {
			if (strcasecmp(file_extname.c_str(), "wav") == 0)
				bSoundFile_Ext = true;
			else
				bSoundFile_Ext = false;
		}
		else {
			if (strcasecmp(file_extname.c_str(), "mp3") == 0)
				bSoundFile_Ext = true;
			else
				bSoundFile_Ext = false;
		}

		boost::split(tmp_vec, tmp_fn, boost::is_any_of("/"));
		down_fn = tmp_vec[tmp_vec.size() - 1];
		job_log->debug("[%s] org filename %s", job_name, down_fn.c_str());

		if (bSoundFile_Ext == true) {
			int nFind = down_fn.rfind('.');
			down_fn = down_fn.substr(0, nFind);
		}

		input_file = tmp_path;
		//input_file.append(gearman_job_handle(job));
		input_file.append(down_fn);
		input_file.push_back('.');
		if (is_wave)
			input_file.append("wav");
		else
			input_file.append("mp3");


		job_log->debug("[%s] Write to %s", job_name, input_file.c_str());
		std::FILE *fp = std::fopen(input_file.c_str(), "wb");
		if (!fp) {
			job_log->error("[%s] Cannot write: %s", job_name, std::strerror(errno));
			return false;
		}
		std::shared_ptr<std::FILE> fd(fp, std::fclose);

		for (size_t i = 0; i < size; ++i) {
			if (std::fwrite((char *)&data[i], sizeof(short), 1, fd.get()) == 0)
				job_log->warn("[%s] Write size is zero: %s", job_name, std::strerror(errno));
		}
	}

	return true;
}

/**
 * @brief		파일 로드  
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 10. 14. 17:57:51
 * @param[in]	job_name	Job name
 * @return		Upon successful completion, a TRUE is returned.\n
 				Otherwise, a FALSE is returned.
 * @see			job_stt()
 */
static inline bool __load_file(const std::string &output_file, std::vector<short> &buffer) {
	std::FILE *fp = std::fopen(output_file.c_str(), "rb");
	if (!fp)
		return false;
	std::shared_ptr<std::FILE> fd(fp, std::fclose);

	short sData;
	while (std::fread(&sData, sizeof(short), 1, fd.get()) > 0)
		buffer.push_back(sData);

	return true;
}

/**
 * @brief		STT 수행
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 10. 14. 17:53:49
 * @param[in]	server	 VR 인스턴스 
 * @return		Upon successful completion, a TRUE is returned.\n
 				Otherwise, a FALSE is returned.
 * @see			job_stt()
 */
static inline bool __job_stt(VRServer *server, short *data, size_t size, long stt_thread_num, std::string &cell_data) {
	try {
		int rc = server->stt(data, size, stt_thread_num, cell_data);
		if (rc)
			return false;

		return true;
	} catch (std::exception &e) {
		return false;
	} catch (std::exception *e) {
		return false;
	}
}

/**
* @brief		chilkat 라이브러리를 이용한 SFTP 다운로드 처리
* @author		TaeBong Wang (tbwang@itfact.co.kr)
* @date			2018.01.19
* @param[in]	remote_file 원격지 다운로드파일, local_file 로컬 저장할 파일
* @return		Upon successful completion, a TRUE is returned.\n
Otherwise, a FALSE is returned.
* @see			job_stt()
*/
static inline bool __sftp_download(std::string remote_file, std::string local_file, gearman_job_st *job, void *context) {
	std::string __job_name(COLOR_BLACK_BOLD);
	__job_name.append("STT:");
	__job_name.append(gearman_job_handle(job));
	__job_name.append(COLOR_NC);
	const char *job_name = __job_name.c_str();
	VRServer *server = (VRServer *)context;

	bool success = false;
	if (sftp.get_IsConnected()) {
		success = sftp.DownloadFileByName(remote_file.c_str(), local_file.c_str());
		if (success != true) {
			job_log->error("[%s] sftp download fail(1st) - %s : %s", job_name, remote_file.c_str(), sftp.lastErrorText());
			for (int i = 0; i < 3; i++) {
				std::string sftp_host = server->getConfig()->getConfig("protocol.host");
				std::string sftp_port = server->getConfig()->getConfig("protocol.port");
				std::string sftp_id = server->getConfig()->getConfig("protocol.username");
				std::string sftp_pwd = server->getConfig()->getConfig("protocol.password");
				std::string encrypt_flag = server->getConfig()->getConfig("protocol.encrypt");
				bool bEncrypt = false;
				if (encrypt_flag.compare("false") == 0)
					bEncrypt = false;
				else
					bEncrypt = true;

				if (server->init_sftp(sftp_host, sftp_port, sftp_id, sftp_pwd, bEncrypt) == false) {
					job_log->error("Connect fail SFTP server !!!!");
					success = false;
				}

				std::this_thread::sleep_for(std::chrono::seconds(3));
				success = sftp.DownloadFileByName(remote_file.c_str(), local_file.c_str());
				if (success == true)
					break;
				else
					job_log->error("[%s] sftp download fail(%d st) - %s : %s", job_name, i, remote_file.c_str(), sftp.lastErrorText());
			}
			success = false;
		}
	}
	else {
		// 재접속 처리
		job_log->info("[%s] Reconnect SFTP Server !!!!", job_name);
		if (server->getConfig()->isSet("protocol.protocol")) {
			if (server->getConfig()->getConfig("protocol.protocol").compare("sftp") == 0) {
				std::string sftp_host = server->getConfig()->getConfig("protocol.host");
				std::string sftp_port = server->getConfig()->getConfig("protocol.port");
				std::string sftp_id = server->getConfig()->getConfig("protocol.username");
				std::string sftp_pwd = server->getConfig()->getConfig("protocol.password");
				std::string encrypt_flag = server->getConfig()->getConfig("protocol.encrypt");
				bool bEncrypt = false;
				if (encrypt_flag.compare("false") == 0)
					bEncrypt = false;
				else
					bEncrypt = true;

				if (server->init_sftp(sftp_host, sftp_port, sftp_id, sftp_pwd, bEncrypt) == false) {
					job_log->error("Connect fail SFTP server !!!!");
					success = false;
				}
				else {
					std::this_thread::sleep_for(std::chrono::seconds(3));

					success = sftp.DownloadFileByName(remote_file.c_str(), local_file.c_str());
					if (success != true) {
						job_log->debug("[%s] sftp download fail(2st) : %s", job_name, sftp.lastErrorText());

						for (int i = 0; i < 3; i++) {
							std::string sftp_host = server->getConfig()->getConfig("protocol.host");
							std::string sftp_port = server->getConfig()->getConfig("protocol.port");
							std::string sftp_id = server->getConfig()->getConfig("protocol.id");
							std::string sftp_pwd = server->getConfig()->getConfig("protocol.passwd");
							std::string encrypt_flag = server->getConfig()->getConfig("protocol.encrypt");
							bool bEncrypt = false;
							if (encrypt_flag.compare("false") == 0)
								bEncrypt = false;
							else
								bEncrypt = true;

							if (server->init_sftp(sftp_host, sftp_port, sftp_id, sftp_pwd, bEncrypt) == false) {
								job_log->error("Connect fail SFTP server !!!!");
								success = false;
							}

							std::this_thread::sleep_for(std::chrono::seconds(3));
							success = sftp.DownloadFileByName(remote_file.c_str(), local_file.c_str());
							if (success == true)
								break;
							else
								job_log->error("[%s] sftp download fail(%d st) - %s : %s", job_name, i, remote_file.c_str(), sftp.lastErrorText());
						}
						success = false;
					}
				}
			}
		}
	}

	return success;
}

/**
* @brief		chilkat 라이브러리를 이용한 FTPS 다운로드 처리
* @author		TaeBong Wang (tbwang@itfact.co.kr)
* @date			2018.01.19
* @param[in]	remote_file 원격지 다운로드파일, local_file 로컬 저장할 파일
* @return		Upon successful completion, a TRUE is returned.\n
Otherwise, a FALSE is returned.
* @see			job_stt()
*/
static inline bool __ftps_download(std::string remote_file, std::string local_file, gearman_job_st *job, void *context) {
	std::string __job_name(COLOR_BLACK_BOLD);
	__job_name.append("STT:");
	__job_name.append(gearman_job_handle(job));
	__job_name.append(COLOR_NC);
	const char *job_name = __job_name.c_str();
	VRServer *server = (VRServer *)context;

	bool success = false;
	if (ftp.get_IsConnected()) {
		success = ftp.GetFile(remote_file.c_str(), local_file.c_str());
		if (success != true) {
			job_log->error("[%s] ftps download fail(1st) - %s : %s", job_name, remote_file.c_str(), ftp.lastErrorText());
			for (int i = 0; i < 3; i++) {
				std::string ftp_host = server->getConfig()->getConfig("protocol.host");
				std::string ftp_port = server->getConfig()->getConfig("protocol.port");
				std::string ftp_id = server->getConfig()->getConfig("protocol.username");
				std::string ftp_pwd = server->getConfig()->getConfig("protocol.password");
				std::string encrypt_flag = server->getConfig()->getConfig("protocol.encrypt");
				bool bEncrypt = false;
				if (encrypt_flag.compare("false") == 0)
					bEncrypt = false;
				else
					bEncrypt = true;

				if (server->init_ftps(ftp_host, ftp_port, ftp_id, ftp_pwd, bEncrypt) == false) {
					job_log->error("Connect fail FTPS server !!!!");
					success = false;
				}

				std::this_thread::sleep_for(std::chrono::seconds(3));
				success = ftp.GetFile(remote_file.c_str(), local_file.c_str());
				if (success == true)
					break;
				else
					job_log->error("[%s] ftps download fail(%d st) - %s : %s", job_name, i, remote_file.c_str(), ftp.lastErrorText());
			}
			success = false;
		}
	}
	else {
		// 재접속 처리
		job_log->info("[%s] Reconnect FTPS Server !!!!", job_name);
		if (server->getConfig()->isSet("protocol.protocol")) {
			if (server->getConfig()->getConfig("protocol.protocol").compare("ftps") == 0) {
				std::string ftp_host = server->getConfig()->getConfig("protocol.host");
				std::string ftp_port = server->getConfig()->getConfig("protocol.port");
				std::string ftp_id = server->getConfig()->getConfig("protocol.username");
				std::string ftp_pwd = server->getConfig()->getConfig("protocol.password");
				std::string encrypt_flag = server->getConfig()->getConfig("protocol.encrypt");
				bool bEncrypt = false;
				if (encrypt_flag.compare("false") == 0)
					bEncrypt = false;
				else
					bEncrypt = true;

				if (server->init_ftps(ftp_host, ftp_port, ftp_id, ftp_pwd, bEncrypt) == false) {
					job_log->error("Connect fail FTPS server !!!!");
					success = false;
				}
				else {
					std::this_thread::sleep_for(std::chrono::seconds(3));

					success = ftp.GetFile(remote_file.c_str(), local_file.c_str());
					if (success != true) {
						job_log->debug("[%s] ftps download fail(2st) : %s", job_name, ftp.lastErrorText());

						for (int i = 0; i < 3; i++) {
							std::string ftp_host = server->getConfig()->getConfig("protocol.host");
							std::string ftp_port = server->getConfig()->getConfig("protocol.port");
							std::string ftp_id = server->getConfig()->getConfig("protocol.id");
							std::string ftp_pwd = server->getConfig()->getConfig("protocol.passwd");
							std::string encrypt_flag = server->getConfig()->getConfig("protocol.encrypt");
							bool bEncrypt = false;
							if (encrypt_flag.compare("false") == 0)
								bEncrypt = false;
							else
								bEncrypt = true;

							if (server->init_ftps(ftp_host, ftp_port, ftp_id, ftp_pwd, bEncrypt) == false) {
								job_log->error("Connect fail SFTP server !!!!");
								success = false;
							}

							std::this_thread::sleep_for(std::chrono::seconds(3));
							success = ftp.GetFile(remote_file.c_str(), local_file.c_str());
							if (success == true)
								break;
							else
								job_log->error("[%s] ftps download fail(%d st) - %s : %s", job_name, i, remote_file.c_str(), ftp.lastErrorText());
						}
						success = false;
					}
				}
			}
		}
	}

	return success;
}

/**
 * @brief		STT 요청 
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 06. 27. 13:39:27
 * @return		Upon successful completion, a GEARMAN_SUCCESS is returned.\n
 				Otherwise, a GEARMAN_ERROR is returned.
 * @see			job_unsegment()
 */
static gearman_return_t job_stt(gearman_job_st *job, void *context) {
	const char *workload = (const char *) gearman_job_workload(job);
	const size_t workload_size = gearman_job_workload_size(job);
	std::string __job_name(COLOR_BLACK_BOLD);
	__job_name.append("STT:");
	__job_name.append(gearman_job_handle(job));
	__job_name.append(COLOR_NC);
	const char *job_name = __job_name.c_str();
	VRServer *server = (VRServer *) context;

	// __job_name 파싱
	std::vector<std::string> vec_job_name;
	boost::split(vec_job_name, __job_name, boost::is_any_of(":"));
	std::string stt_thread_id = vec_job_name[vec_job_name.size() - 1];
	long stt_thread_num = std::stol(stt_thread_id);
	job_log->debug("====> stt_thread_id : %ld", stt_thread_num);
	
	job_log->debug("[%s, 0x%X] Recieved %d bytes", job_name, THREAD_ID, workload_size);
	if (workload_size < 10) {
		job_log->error("[%s] The file size is too small (< 10 bytes)", job_name);
		gearman_job_send_fail(job);
		return GEARMAN_ERROR;
	}

	// 프로토콜 확인 
	short *data;
	size_t size;
	std::vector<short> buffer;
	enum PROTOCOL protocol;

	std::string remote_file("");
	std::string local_file("");

	try {
		//job_log->debug("=====> %d", server->getConfig()->getConfig("protocol.use").compare("true"));
		if (server->getConfig()->isSet("protocol.use") && server->getConfig()->getConfig("protocol.use").compare("false") == 0) {
			job_log->info("[%s] ==== DOWNLOAD_START : %s ====", job_name, workload);
			protocol = WorkerDaemon::downloadData(server->getConfig(), workload, workload_size, buffer);
			job_log->info("[%s] ==== DOWNLOAD_END : %s ====", job_name, workload);
		}
		else if (server->getConfig()->isSet("protocol.use") && server->getConfig()->getConfig("protocol.use").compare("true") == 0) {
			// protocol check
			protocol = WorkerDaemon::checkProtocol(server->getConfig(), workload, workload_size);

			// 프로토콜로 오는 경우 파일패스에 쓰레기 값이 붙는 현상이 있어서 데이터 처리
			std::string get_file_nm("");
			get_file_nm = workload;
			get_file_nm = get_file_nm.substr(0, workload_size);
			job_log->debug("[%s] workload ==> %s", job_name, get_file_nm.c_str());

			// protocol type에 따른 다운로드
			if (server->getConfig()->getConfig("protocol.type").compare("sftp") == 0) {
				remote_file = get_file_nm.substr(7);
				std::string::size_type pos = remote_file.find("/");
				remote_file = remote_file.substr(pos);

				// AIG 에서는 FTP 홈경로가 E:/ 기본으로 셋팅되어 있어서
				// 다운로드 경로가 /SponsorRec/TT001/123.wav 이렇게 되어서 오면 다운로드를 정상적으로 처리하지 못함
				// 경로를 SponsorRec/TT001/123.wav 이렇게 되도록 처리함.(AIG 처리용으로만 사용)
				// 그 외 사이트는 위 소스의 remote_file 주석처리 부분 해제하고 사용
				//remote_file = remote_file.substr(pos + 1);


				std::string down_fn("");
				std::vector<std::string> tmp_vec;
				boost::split(tmp_vec, remote_file, boost::is_any_of("/"));
				down_fn = tmp_vec[tmp_vec.size() - 1];

				local_file = tmp_path;
				local_file.append(down_fn);

				job_log->debug("[%s] remote file : %s", job_name, remote_file.c_str());
				job_log->debug("[%s] local file : %s", job_name, local_file.c_str());

				job_log->info("[%s] ==== DOWNLOAD_START : %s ====", job_name, remote_file.c_str());

				if (__sftp_download(remote_file, local_file, job, context) == false) {
					// 처리 불가 
					std::string resp = default_config.fail_download;
					resp.push_back('\n');
					resp.append(server->server_name);
					job_log->error("[%s, 0x%X] Fail to download. %s", job_name, THREAD_ID, remote_file.c_str());
					gearman_job_send_complete(job, resp.c_str(), resp.size());
					// gearman_job_send_fail(job);
					return GEARMAN_ERROR;
				}

				job_log->info("[%s] ==== DOWNLOAD_END : %s ====", job_name, remote_file.c_str());

				// 다운로드 파일 버퍼로 로드 -- FIXME (다운로드 할 때부터 버퍼로 가져올 수 있는 방법 체크)
				if (!__load_file(local_file, buffer)) {
					job_log->error("[%s] Cannot download fail : %s", job_name, remote_file.c_str());
					gearman_job_send_fail(job);
					return GEARMAN_ERROR;
				}
			}

			if (server->getConfig()->getConfig("protocol.type").compare("ftps") == 0) {
				remote_file = get_file_nm.substr(7);
				std::string::size_type pos = remote_file.find("/");
				remote_file = remote_file.substr(pos);

				std::string down_fn("");
				std::vector<std::string> tmp_vec;
				boost::split(tmp_vec, remote_file, boost::is_any_of("/"));
				down_fn = tmp_vec[tmp_vec.size() - 1];

				local_file = tmp_path;
				local_file.append(down_fn);

				job_log->debug("[%s] remote file : %s", job_name, remote_file.c_str());
				job_log->debug("[%s] local file : %s", job_name, local_file.c_str());

				job_log->info("[%s] ==== DOWNLOAD_START : %s ====", job_name, remote_file.c_str());

				if (__ftps_download(remote_file, local_file, job, context) == false) {
					// 처리 불가 
					std::string resp = default_config.fail_download;
					resp.push_back('\n');
					resp.append(server->server_name);
					job_log->error("[%s, 0x%X] Fail to download. %s", job_name, THREAD_ID, remote_file.c_str());
					gearman_job_send_complete(job, resp.c_str(), resp.size());
					// gearman_job_send_fail(job);
					return GEARMAN_ERROR;
				}

				job_log->info("[%s] ==== DOWNLOAD_END : %s ====", job_name, remote_file.c_str());

				// 다운로드 파일 버퍼로 로드 -- FIXME (다운로드 할 때부터 버퍼로 가져올 수 있는 방법 체크)
				if (!__load_file(local_file, buffer)) {
					job_log->error("[%s] Cannot download fail : %s", job_name, remote_file.c_str());
					gearman_job_send_fail(job);
					return GEARMAN_ERROR;
				}
			}
		}
	} catch (std::exception &e) {
		// 처리 불가 
		std::string resp = default_config.fail_download;
		resp.push_back('\n');
		resp.append(server->server_name);
		job_log->error("[%s, 0x%X] Fail to download. %s", job_name, THREAD_ID, e.what());
		gearman_job_send_complete(job, resp.c_str(),
									  resp.size());
		// gearman_job_send_fail(job);
		return GEARMAN_ERROR;
	}

	if (protocol == PROTOCOL_NONE) {
		// Stream data
		data = (short *) workload;
		size = workload_size / sizeof(short);
	} else {
		data = (short *) &*buffer.begin();
		size = buffer.size();
	}

	// Check WAVE format
	int metadata_size;
	bool is_wave = false;
	std::string input_file, output_file;
	switch (VRServer::check_wave_format(data, size)) {
	default:
		// 분석 불가능한 포멧 
		job_log->error("[%s, 0x%X] Unsupported format", job_name, THREAD_ID);
		gearman_job_send_fail(job);

		// 파일삭제
		if (protocol != PROTOCOL_FILE)
			std::remove(input_file.c_str());

		return GEARMAN_ERROR;

	case UNKNOWN_FORMAT:
		job_log->warn("[%s, 0x%X] Input data like RAW PCM", job_name, THREAD_ID);
		// PCM_WAVE 타입일 수도 있으니 일단 분석 시도 
		if (server->getConfig()->isSet("protocol.use") && server->getConfig()->getConfig("protocol.use").compare("true") == 0) {
			input_file = local_file;
		}

		break;

	case STANDARD_WAVE:
		job_log->info("[%s, 0x%X] Input data is Standard WAVE format", job_name, THREAD_ID);
		//metadata_size = 44 / sizeof(short);
		//data += metadata_size;
		//size -= metadata_size;

		// 소프트웨어 화자분리를 위해 16k PCM 파일을 만들기 위한 작업
		if (server->getConfig()->isSet("spk.enable") && server->getConfig()->getConfig("spk.enable").compare("true") == 0) {
			// chilkat을 이용하지 않고 기존 방식으로 처리할 경우
			if (server->getConfig()->isSet("protocol.use") && server->getConfig()->getConfig("protocol.use").compare("false") == 0) {
				// 로컬 파일이 아닌 경우 저장 시도 
				if (!__store_file(job, job_name, true, data, size, workload, workload_size, protocol, input_file)) {
					gearman_job_send_fail(job);
					return GEARMAN_ERROR;
				}
			}
			else {
				input_file = local_file;
				// 녹취 포맷 체크 하기 위한 버퍼 clear
				buffer.clear();
			}
			
			// 바이너리 호출로 대체 
			if (server->getConfig()->isSet("stt.decoder")) {
				std::string cmd = "";
				cmd = server->getConfig()->getConfig("stt.decoder");
				cmd.push_back(' ');
				cmd.append(input_file.c_str());
				if (server->getConfig()->isSet("spk.enable") && server->getConfig()->getConfig("spk.enable").compare("true") == 0) {
					cmd.push_back(' ');
					cmd.append("spk_16k_only");
				}
				job_log->debug("[%s, 0x%X] %s", job_name, THREAD_ID, cmd.c_str());
				if (std::system(cmd.c_str())) {
					std::string resp = default_config.fail_decoding;
					resp.push_back('\n');
					resp.append(server->server_name);
					job_log->error("[%s, 0x%X] Fail to decoding: %s", job_name, THREAD_ID, input_file.c_str());
					gearman_job_send_complete(job, resp.c_str(), resp.size());
					// gearman_job_send_fail(job);

					// 파일삭제
					//if (protocol != PROTOCOL_FILE)
					//	std::remove(input_file.c_str());

					return GEARMAN_ERROR;
				}
			}
			else {
				job_log->error("[%s, 0x%X] Cannot decoding: %s", job_name, THREAD_ID, input_file.c_str());
				gearman_job_send_fail(job);
				// 파일삭제
				if (protocol != PROTOCOL_FILE)
					std::remove(input_file.c_str());

				return GEARMAN_ERROR;
			}
		}
		else {
			metadata_size = 44 / sizeof(short);
			data += metadata_size;
			size -= metadata_size;
		}

		break;

	case WAVE:
		job_log->info("[%s, 0x%X] Input data is WAVE format", job_name, THREAD_ID);
		is_wave = true;

	case MPEG:
	case MPEG_ID3:
		if (!is_wave)
			job_log->info("[%s, 0x%X] Input data is MPEG format", job_name, THREAD_ID);

		// chilkat을 이용하지 않고 기존 방식으로 처리할 경우
		if (server->getConfig()->isSet("protocol.use") && server->getConfig()->getConfig("protocol.use").compare("false") == 0) {
			// 로컬 파일이 아닌 경우 저장 시도 
			if (!__store_file(job, job_name, is_wave, data, size, workload, workload_size, protocol, input_file)) {
				// 기존 버퍼 삭제 
				buffer.clear();

				gearman_job_send_fail(job);
				return GEARMAN_ERROR;
			}
		}
		else {
			input_file = local_file;
		}

		// STT
		job_log->info("[%s] STT start - %s", job_name, input_file.c_str());

		// 기존 버퍼 삭제 
		buffer.clear();

		// 바이너리 호출로 대체 
		if (server->getConfig()->isSet("stt.decoder")) {
			std::string cmd = "";
			cmd = server->getConfig()->getConfig("stt.decoder");
			cmd.push_back(' ');
			cmd.append(input_file.c_str());
			if (server->getConfig()->isSet("spk.enable") && server->getConfig()->getConfig("spk.enable").compare("true") == 0) {
				cmd.push_back(' ');
				cmd.append("spk");
			}
			//job_log->debug("[%s, 0x%X] %s", job_name, THREAD_ID, cmd.c_str());
			if (std::system(cmd.c_str())) {
				std::string resp = default_config.fail_decoding;
				resp.push_back('\n');
				resp.append(server->server_name);
				job_log->error("[%s, 0x%X] Fail to decoding: %s", job_name, THREAD_ID, input_file.c_str());
				gearman_job_send_complete(job, resp.c_str(),
											  resp.size());
				// gearman_job_send_fail(job);

				// 파일삭제
				if (protocol != PROTOCOL_FILE)
					std::remove(input_file.c_str());

				return GEARMAN_ERROR;
			}
		} else {
			job_log->error("[%s, 0x%X] Cannot decoding: %s", job_name, THREAD_ID, input_file.c_str());
			gearman_job_send_fail(job);
			// 파일삭제
			if (protocol != PROTOCOL_FILE)
				std::remove(input_file.c_str());

			return GEARMAN_ERROR;
		}

		output_file = std::string(input_file.c_str(), input_file.size() - 3);
		output_file.append("pcm");

		// 파일 다시 로드 (PCM 파일 로드)
		if (!__load_file(output_file, buffer)) {
			job_log->error("[%s] Cannot decoding: %s", job_name, std::strerror(errno));
			gearman_job_send_fail(job);
			return GEARMAN_ERROR;
		}

		data = (short *) &*buffer.begin();
		size = buffer.size();

		// 임시 파일 삭제 
		job_log->debug("[%s] Delete temporary file: %s", job_name, output_file.c_str());
		
		std::remove(output_file.c_str());
		if (protocol != PROTOCOL_FILE)
			std::remove(input_file.c_str());

		// 버퍼 삭제
		buffer.clear();
		
		break;


	case WAVE_2CH:
		job_log->info("[%s] Input data is 2CH WAVE", job_name);

		// chilkat을 이용하지 않고 기존 방식으로 처리할 경우
		if (server->getConfig()->isSet("protocol.use") && server->getConfig()->getConfig("protocol.use").compare("false") == 0) {
			// 로컬 파일이 아닌 경우 저장 시도 
			if (!__store_file(job, job_name, true, data, size, workload, workload_size, protocol, input_file)) {
				gearman_job_send_fail(job);
				return GEARMAN_ERROR;
			}
		}
		else {
			input_file = local_file;
		}

		// STT
		job_log->info("[%s] STT start - %s", job_name, input_file.c_str());

		// 바이너리 호출로 대체 
		if (server->getConfig()->isSet("stt.separator")) {
			std::string cmd(server->getConfig()->getConfig("stt.separator"));
			cmd.push_back(' ');
			cmd.append(input_file.c_str());
			cmd.push_back(' ');
			cmd.append(input_file.substr(0, input_file.rfind("/")));
			job_log->debug("[%s] %s", job_name, cmd.c_str());
			if (std::system(cmd.c_str())) {
				job_log->error("[%s] Fail to separation: %s", job_name, input_file.c_str());
				gearman_job_send_fail(job);

				if (protocol != PROTOCOL_FILE)
					std::remove(input_file.c_str());

				return GEARMAN_ERROR;
			}
		} else {
			job_log->error("[%s] Cannot separation: %s", job_name, input_file.c_str());
			gearman_job_send_fail(job);

			if (protocol != PROTOCOL_FILE)
				std::remove(input_file.c_str());

			return GEARMAN_ERROR;
		}

		if (protocol != PROTOCOL_FILE)
			std::remove(input_file.c_str());

		std::this_thread::sleep_for(std::chrono::milliseconds(300));

		// 분리된 파일 처리 
		std::string part_data[2];
		for (int ch_idx = 0; ch_idx < 2; ++ch_idx) {
			job_log->info("[%s] part_data[%d] stt job prepare.", job_name, ch_idx);
			output_file = std::string(input_file.c_str(), input_file.size() - 4);
			output_file.push_back('_');
			output_file.append(ch_idx == 0 ? "left" : "right");
			output_file.append(".pcm");
			job_log->info("[%s] part_data[%d] %s - %s", job_name, ch_idx, input_file.c_str(), output_file.c_str());

			// 기존 버퍼 삭제 
			buffer.clear();

			// 파일 다시 로드 
			if (!__load_file(output_file, buffer)) {
				std::string resp = default_config.fail_decoding;
				resp.push_back('\n');
				resp.append(server->server_name);
				job_log->error("[%s] Cannot decoding: %s", job_name, std::strerror(errno));
				gearman_job_send_complete(job, resp.c_str(),
											  resp.size());
				// gearman_job_send_fail(job);
				std::remove(output_file.c_str());
				return GEARMAN_ERROR;
			}

			data = (short *) &*buffer.begin();
			size = buffer.size();

			if (!__job_stt(server, data, size, stt_thread_num, part_data[ch_idx])) {
				job_log->error("[%s] Fail to stt", job_name);
				gearman_job_send_fail(job);
				return GEARMAN_ERROR;
			}

			std::remove(output_file.c_str());

			// 버퍼 삭제
			buffer.clear();
		}

		// 결과 전송
		std::string resHdr("SUCCESS\n");
		resHdr.append(server->server_name);
		resHdr.push_back('\n');
		std::string fsize(boost::lexical_cast<std::string>(size * sizeof(short)));
		resHdr.append(fsize);
		std::string merge_data(resHdr);
		merge_data.push_back('\n');
 		merge_data.append(part_data[0]);
 		merge_data.append("||");
 		merge_data.append(part_data[1]);


		//job_log->debug("[%s] Done: %d bytes, %s", job_name, merge_data.size(), merge_data.c_str());
		job_log->debug("[%s] Done: %d bytes", job_name, merge_data.size());
		if (gearman_failed(gearman_job_send_complete(job, merge_data.c_str(), merge_data.size()))) {
			job_log->error("[%s] Fail to send result", job_name);
			return GEARMAN_ERROR;
		}
		
		return GEARMAN_SUCCESS;
	}

	// STT
	std::string resHdr("SUCCESS\n");
	resHdr.append(server->server_name);
	resHdr.push_back('\n');
	std::string fsize(boost::lexical_cast<std::string>(size * sizeof(short)));
	resHdr.append(fsize);
	std::string cell_data(resHdr);
	cell_data.push_back('\n');
	if (!__job_stt(server, data, size, stt_thread_num, cell_data)) {
		job_log->error("[%s] Fail to stt", job_name);
		gearman_job_send_fail(job);
		std::remove(input_file.c_str());
		return GEARMAN_ERROR;
	}

	// 파일 삭제
	if (protocol != PROTOCOL_FILE)
		std::remove(input_file.c_str());


	// 화자분리가 설정되어 있을 경우 JSON 형태로 데이터를 전송
	// JSON 포맷
	// {
	//		"spk_flag" : "true",
	//		"spk_node" : "vr_spk_1",
	//		"data" : "unsegment data"
	// }
	
	std::string final_text("");

	if (server->getConfig()->isSet("spk.enable") && server->getConfig()->getConfig("spk.enable").compare("true") == 0) {
		final_text.push_back('{');
		final_text.push_back('"');
		final_text.append("spk_flag");
		final_text.push_back('"');
		final_text.push_back(':');
		final_text.push_back('"');
		final_text.append("true");
		final_text.push_back('"');
		final_text.push_back(',');
		final_text.push_back('"');
		final_text.append("spk_node");
		final_text.push_back('"');
		final_text.push_back(':');
		final_text.push_back('"');
		if (server->getConfig()->isSet("spk.worker_name")) {
			final_text.append(server->getConfig()->getConfig("spk.worker_name"));
		}
		else {
			final_text.append("vr_spk");
		}
		final_text.push_back('"');
		final_text.push_back('}');
		final_text.push_back('\n');
		final_text.append(cell_data);

		// 결과 전송 
		//job_log->debug("[%s] Done: %s", job_name, cell_data.c_str());
		//job_log->debug("[%s] Done: %d bytes", job_name, final_text.size());
		//job_log->debug("[%s] Done: %s", job_name, final_text.c_str());
		job_log->debug("[%s] STT(SPK) Done: %s (%d bytes)", job_name, input_file.c_str(), final_text.size());
		gearman_return_t ret = gearman_job_send_complete(job, final_text.c_str(), final_text.size());
		if (gearman_failed(ret)) {
			job_log->error("[%s] Fail to send result", job_name);
			return GEARMAN_ERROR;
		}
	}
	else {
		// 결과 전송 
		job_log->debug("[%s] STT Done: %s (%d bytes)", job_name, input_file.c_str(), cell_data.size());
		gearman_return_t ret = gearman_job_send_complete(job, cell_data.c_str(), cell_data.size());
		if (gearman_failed(ret)) {
			job_log->error("[%s] Fail to send result", job_name);
			return GEARMAN_ERROR;
		}
	}

	//// 결과 전송 
	////job_log->debug("[%s] Done: %s", job_name, cell_data.c_str());
	//job_log->debug("[%s] Done: %d bytes", job_name, cell_data.size());
	//job_log->debug("[%s] STT Done: %s (%d bytes)", job_name, input_file.c_str(), cell_data.size());
	//gearman_return_t ret = gearman_job_send_complete(job, cell_data.c_str(), cell_data.size());
	//if (gearman_failed(ret)) {
	//	job_log->error("[%s] Fail to send result", job_name);
	//	return GEARMAN_ERROR;
	//}

	// buffer가 비어있지 않으면 비운다
	if (!buffer.empty())
		buffer.clear();

	return GEARMAN_SUCCESS;
}

/**
 * @brief		save_data
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 5. 12. 오전 11:27
 * @return		Upon successful completion, a EXIT_SUCCESS is returned.\n
 				Otherwise,
 				a positive error code is returned indicating what went wrong.
 */
static int save_data(const std::string &filename, const size_t data_size, const char *data) {
	std::ofstream ofs(filename);
	try {
		ofs.write(data, data_size);
	} catch (std::exception &e) {
		ofs.close();
		return errno;
	}
	ofs.flush();
	ofs.close();
	return EXIT_SUCCESS;
}

/**
 * @brief		Unsegment 요청
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 06. 30. 13:17:55
 * @return		Upon successful completion, a GEARMAN_SUCCESS is returned.\n
 				Otherwise, a GEARMAN_ERROR is returned.
 * @see			job_stt()
 */
static gearman_return_t job_unsegment(gearman_job_st *job, void *context) {
	const char *workload = (const char *) gearman_job_workload(job);
	const size_t workload_size = gearman_job_workload_size(job);
	std::string __job_name(COLOR_BLACK_BOLD);
	__job_name.append("Unsegment:");
	__job_name.append(gearman_job_handle(job));
	__job_name.append(COLOR_NC);
	const char *job_name = __job_name.c_str();
	// const char *job_name = (const char *) gearman_job_handle(job);
	VRServer *server = (VRServer *) context;

	job_log->debug("[%s, 0x%X] Recieved %d bytes", job_name, THREAD_ID, workload_size);
	if (workload_size < 10) {
		job_log->error("[%s] The file size is too small (< 10 bytes)", job_name);
		gearman_job_send_fail(job);
		return GEARMAN_ERROR;
	}

	// Unsegment 
	std::string cell_data(workload, workload_size);
	std::string text;
	try {
		if (server->unsegment(cell_data, text)) {
			job_log->error("[%s] Fail to unsegment", job_name);
			gearman_job_send_fail(job);
			return GEARMAN_ERROR;
		}
	} catch(std::exception &e) {
		job_log->error("[%s] Fail to unsegment, %s", job_name, e.what());
		gearman_job_send_fail(job);
		return GEARMAN_ERROR;
	} catch(std::exception *e) {
		job_log->error("[%s] Fail to unsegment, %s", job_name, e->what());
		gearman_job_send_fail(job);
		return GEARMAN_ERROR;
	}

	job_log->info("[%s] Unsegment Done: %s", job_name, text.c_str());

	// 결과 전송 
	job_log->debug("[%s] Done: %s", job_name, text.c_str());
	job_log->debug("[%s] Done: %d bytes", job_name, text.size());
	gearman_return_t ret = gearman_job_send_complete(job, text.c_str(), text.size());
	if (gearman_failed(ret)) {
		job_log->error("[%s] Fail to send result", job_name);
		return GEARMAN_ERROR;
	}

	return GEARMAN_SUCCESS;
}

/**
 * @brief		Unsegment(with Time) 요청
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 06. 30. 13:17:55
 * @return		Upon successful completion, a GEARMAN_SUCCESS is returned.\n
 				Otherwise, a GEARMAN_ERROR is returned.
 * @see			job_stt()
 */
static gearman_return_t job_unsegment_with_time(gearman_job_st *job, void *context) {
	const char *workload = (const char *) gearman_job_workload(job);
	const size_t workload_size = gearman_job_workload_size(job);
	std::string __job_name(COLOR_BLACK_BOLD);
	__job_name.append("Unsegment:");
	__job_name.append(gearman_job_handle(job));
	__job_name.append(COLOR_NC);
	const char *job_name = __job_name.c_str();
	// const char *job_name = (const char *) gearman_job_handle(job);
	VRServer *server = (VRServer *) context;

	job_log->debug("[%s] unsegment_with_time job start", job_name);

	job_log->debug("[%s, 0x%X] Recieved %d bytes", job_name, THREAD_ID, workload_size);
	if (workload_size < 10) {
		job_log->error("[%s] The file size is too small (< 10 bytes)", job_name);
		gearman_job_send_fail(job);
		return GEARMAN_ERROR;
	}

	// FIXME: 임시로 파일 저장 
	std::string mlf_pathname(tmp_path);
	mlf_pathname.append(gearman_job_handle(job));
	std::string text_pathname(mlf_pathname);
	text_pathname.append(".txt");
	mlf_pathname.append(".mlf");
	job_log->debug("[%s] mlf file : %s", job_name, mlf_pathname.c_str());
	job_log->debug("[%s] text file : %s", job_name, text_pathname.c_str());
	
	if (save_data(mlf_pathname, workload_size, workload)) {
		job_log->error("[%s] Fail to recieve data", job_name);
		return GEARMAN_ERROR;
	}
	

	// Unsegment 
	std::string text = "SUCCESS\n";
	text.append(server->server_name);
	text.push_back('\n');
	try {
		int unsegment_pause = 100;
		if (server->getConfig()->isSet("stt.unsegment_pause")) {
			unsegment_pause = server->getConfig()->getConfig("stt.unsegment_pause", unsegment_pause);
		}
		else {
			unsegment_pause = -1;	// env.conf 파일에서 설정이 없다면 -1로 설정하여 SPLPostProcMLF 함수를 타도록 함
		}

		if (server->unsegment_with_time(mlf_pathname, text_pathname, unsegment_pause)) {
			job_log->error("[%s] Fail to unsegment_with_time", job_name);
			gearman_job_send_fail(job);
			return GEARMAN_ERROR;
		}

		// FIXME: 임시로 파일 리드 
		std::ifstream text_file(text_pathname);
		if (text_file.is_open()) {
			for (std::string line; std::getline(text_file, line); ) {
				text.append(line);
				text.push_back('\n');
			}
			text_file.close();

			//job_log->debug("[%s] unsegment_with_time text : %s", job_name, text.c_str());
		} else
			throw std::runtime_error("Cannot load data");

		std::remove(mlf_pathname.c_str());
		std::remove(text_pathname.c_str());
	} catch(std::exception &e) {
		job_log->error("[%s] Fail to unsegment_with_time, %s", job_name, e.what());
		gearman_job_send_fail(job);
		return GEARMAN_ERROR;
	} catch(std::exception *e) {
		job_log->error("[%s] Fail to unsegment_with_time, %s", job_name, e->what());
		gearman_job_send_fail(job);
		return GEARMAN_ERROR;
	}

	// 결과 전송 
	job_log->debug("[%s] Done: %d bytes", job_name, text.size());
	gearman_return_t ret = gearman_job_send_complete(job, text.c_str(), text.size());
	if (gearman_failed(ret)) {
		job_log->error("[%s] Fail to send result", job_name);
		return GEARMAN_ERROR;
	}

	return GEARMAN_SUCCESS;
}

static int DecodeMimeBase64[256] = {
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* 00-0F */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* 10-1F */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,  /* 20-2F */
	52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,  /* 30-3F */
	-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,  /* 40-4F */
	15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,  /* 50-5F */
	-1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,  /* 60-6F */
	41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,  /* 70-7F */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* 80-8F */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* 90-9F */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* A0-AF */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* B0-BF */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* C0-CF */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* D0-DF */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* E0-EF */
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1   /* F0-FF */
};

typedef union{
    struct{
        unsigned char c1, c2, c3;
    };
    struct{
        unsigned int e1:6, e2:6, e3:6, e4:6;
    };
} BF;

/**
 * @brief		BASE64 디코더
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2017. 03. 30. 16:23:32
 * @param[in]	source	source buffer
 * @param[in]	length	source length
 * @param[out]	buffer	target buffer
 * @return		Upon successful completion, a EXIT_SUCCESS is returned.\n
 				Otherwise,
 				a negative error code is returned indicating what went wrong.
 */
unsigned long long inline
decode_base64(unsigned char * const source, std::size_t length, unsigned char *buffer) {
    std::size_t j = 0;
    std::size_t blank = 0;
    BF temp;
 
    for (std::size_t i = 0; i < length; i = i + 4, j = j + 3) {
        temp.e4 = DecodeMimeBase64[source[i]];
        temp.e3 = DecodeMimeBase64[source[i + 1]];
        if (source[i + 2] == '=') {
            temp.e2 = 0;
            ++blank;
        } else
        	temp.e2 = DecodeMimeBase64[source[i + 2]];

        if (source[i + 3] == '=') {
            temp.e1 = 0;
            ++blank;
        } else
        	temp.e1 = DecodeMimeBase64[source[i + 3]];
 
        buffer[j]   = temp.c3;
        buffer[j + 1] = temp.c2;
        buffer[j + 2] = temp.c1;
    }

    return j - blank;
}

/**
 * @brief		출금 동의 구간 검출 요청 
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 08. 03. 19:02:18
 * @return		Upon successful completion, a GEARMAN_SUCCESS is returned.\n
 				Otherwise, a GEARMAN_ERROR is returned.
 */
static gearman_return_t job_ssp(gearman_job_st *job, void *context) {
	const char *workload = (const char *) gearman_job_workload(job);
	const size_t workload_size = gearman_job_workload_size(job);
	std::string __job_name(COLOR_BLACK_BOLD);
	__job_name.append("SSP:");
	__job_name.append(gearman_job_handle(job));
	__job_name.append(COLOR_NC);
	const char *job_name = __job_name.c_str();
	VRServer *server = (VRServer *) context;

	job_log->info("[%s] Recieved %d bytes", job_name, workload_size);
	if (workload_size < 10) {
		job_log->error("[%s] The file size is too small (< 10 bytes)", job_name);
		gearman_job_send_fail(job);
		return GEARMAN_ERROR;
	}

	// <s> </s> 태그를 제외해야 하며 like 정보도 무시해야 함 
	std::string mlf_data;
	std::string recv_data(workload, workload_size);
	std::vector<std::string> lines;
	boost::split(lines, recv_data, boost::is_any_of("\n"));
	bool useUtil = server->getConfig()->isSet("ssp.util"); 
	for (size_t i = 0; i < lines.size(); ++i) {
		std::vector<std::string> line;
		boost::split(line, lines[i], boost::is_any_of("\t"));
		if (line.size() < 3)
			continue;
		if (!useUtil && (line[2].compare("<s>") == 0 || line[2].compare("</s>") == 0))
			continue;

		char *value = const_cast<char *>(line[2].c_str());
		if (value[0] == '#')
			++value;

		mlf_data.append(line[0]);
		mlf_data.push_back('\t');
		mlf_data.append(line[1]);
		mlf_data.push_back('\t');
		mlf_data.append(line[2]);
		if (useUtil) {
			mlf_data.push_back('\t');
			mlf_data.append(line[3]);
		}
		mlf_data.push_back('\n');
	}

	// FIXME: 임시로 파일 저장 
	std::string pathname(tmp_path);
	pathname.append(gearman_job_handle(job));
	pathname.append(".mlf");
	if (save_data(pathname, mlf_data.size(), mlf_data.c_str())) {
		job_log->error("[%s] Fail to recieve data", job_name);
		return GEARMAN_ERROR;
	}

	std::string result;
	try {
		if (server->ssp(pathname, result)) {
			job_log->error("[%s] Fail to ssp", job_name);
			gearman_job_send_fail(job);
			return GEARMAN_ERROR;
		}
		std::remove(pathname.c_str());
	} catch(std::exception &e) {
		job_log->error("[%s] Fail to ssp, %s", job_name, e.what());
		gearman_job_send_fail(job);
		return GEARMAN_ERROR;
	} catch(std::exception *e) {
		job_log->error("[%s] Fail to ssp, %s", job_name, e->what());
		gearman_job_send_fail(job);
		return GEARMAN_ERROR;
	}

	// 결과 전송 
	job_log->debug("[%s] Done: %d bytes", job_name, result.size());
	gearman_return_t ret = gearman_job_send_complete(job, result.c_str(), result.size());
	if (gearman_failed(ret)) {
		job_log->error("[%s] Fail to send result", job_name);
		return GEARMAN_ERROR;
	}

	return GEARMAN_SUCCESS;
}

/**
 * @brief		Realtime STT 요청 
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2017. 03. 14. 10:15:34
 * @return		Upon successful completion, a GEARMAN_SUCCESS is returned.\n
 				Otherwise, a GEARMAN_ERROR is returned.
 * @see			job_unsegment()
 */
static gearman_return_t job_rt_stt(gearman_job_st *job, void *context) {
	const char *workload = (const char *) gearman_job_workload(job);
	const size_t workload_size = gearman_job_workload_size(job);
	std::string __job_name(COLOR_BLACK_BOLD);
	__job_name.append("RT:");
	__job_name.append(gearman_job_handle(job));
	__job_name.append(COLOR_NC);
	const char *job_name = __job_name.c_str();
	VRServer *server = (VRServer *) context;

	job_log->info("[%s] Recieved %d bytes", job_name, workload_size);
	if (workload_size < 10) {
		job_log->error("[%s] The file size is too small (< 5 bytes)", job_name);
		gearman_job_send_fail(job);
		return GEARMAN_ERROR;
	}

	// 명령어 해석 
	// CALL_ID | CMD | DATA
	char *tmp = const_cast<char *>(workload);
	tmp = strchr(tmp, '|');
	if (tmp == NULL) {
		job_log->error("[%s] Cannot find Call ID", job_name);
		gearman_job_send_fail(job);
		return GEARMAN_ERROR;
	}

	int idx_call_id = tmp - workload;
	std::string call_id(workload, idx_call_id);

	tmp = strchr(tmp + 1, '|');
	if (tmp == NULL) {
		job_log->error("[%s] Invalid argument", job_name);
		gearman_job_send_fail(job);
		return GEARMAN_ERROR;
	}

	int idx_cmd = tmp - workload;
	std::string cmd(workload + idx_call_id + 1, idx_cmd - idx_call_id - 1);
	char state = 1;

	if (cmd.find("FIRS") == 0) state = 0;
	else if (cmd.find("LAST") == 0) state = 2;

	short *data = (short *) (tmp + 1);
	size_t size = (workload_size - idx_cmd - 1) / sizeof(short);

	// DEBUG, fvad를 이용하여 음성 데이터 처리 확인
	if (0) {
		struct timeval tv;
		char dist[32];

		gettimeofday(&tv, NULL);
        sprintf(dist, "%ld.%ld", tv.tv_sec, tv.tv_usec);
		std::string filename = call_id + std::string("_") + std::string(dist) + std::string(".pcm");
		std::ofstream pcmFile;

		pcmFile.open(filename, std::ofstream::out | std::ofstream::binary);
		if (pcmFile.is_open()) {
			pcmFile.write((const char*)data, size * sizeof(short));
			pcmFile.close();
		}
	}
#if 0
	// FIXME: 바이트 배열 변경 
	for (size_t i = 0; i < size; ++i)
		data[i] = ntohs(data[i]);
#endif
	job_log->debug("[%s] Call ID: %s[%s], length: %lu, state(%d)", job_name, call_id.c_str(), cmd.c_str(), size, state);
	// STT
	std::string cell_data = "";
	if (server->stt(call_id, data, size, (const char)state, cell_data) == EXIT_FAILURE) {
		job_log->error("[%s] Fail to stt", job_name);
		gearman_job_send_fail(job);
		return GEARMAN_ERROR;
	}

	// 결과 전송 
	job_log->debug("[%s] Done: %d bytes", job_name, cell_data.size());
#if 1
	std::string text="";

	if (state == 2) {
		text = server->server_name;
		text.push_back('\n');
	}
	try {
		if (server->unsegment(cell_data, text)) {
			job_log->error("[%s] Fail to unsegment", job_name);
			gearman_job_send_fail(job);
			return GEARMAN_ERROR;
		}
	} catch(std::exception &e) {
		job_log->error("[%s] Fail to unsegment, %s", job_name, e.what());
		gearman_job_send_fail(job);
		return GEARMAN_ERROR;
	} catch(std::exception *e) {
		job_log->error("[%s] Fail to unsegment, %s", job_name, e->what());
		gearman_job_send_fail(job);
		return GEARMAN_ERROR;
	}
	gearman_return_t ret = gearman_job_send_complete(job, text.c_str(), text.size());
#else
	gearman_return_t ret = gearman_job_send_complete(job, cell_data.c_str(), cell_data.size());
#endif

	if (gearman_failed(ret)) {
		job_log->error("[%s] Fail to send result", job_name);
		return GEARMAN_ERROR;
	}

	return GEARMAN_SUCCESS;
}
