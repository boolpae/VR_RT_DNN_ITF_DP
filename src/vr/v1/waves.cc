/**
 * @file	waves.cc
 * @brief	Request Waves v1.0
 * @details	
 * @author	Kijeong Khil (kjkhil@itfact.co.kr)
 * @date	2016. 07. 12. 10:37:20
 * @see		restapi_v1.cc
 */
#include "restapi_v1.hpp"

using namespace itfact::vr::node::v1;

const std::string Waves::resource_name = "waves";

bool Waves::equals(const std::string *resource) {
	if (resource->compare(resource_name) == 0)
		return true;
	return false;
}

/**
 * @brief		Request 처리 
 * @details		
 * @author		Kijeong Khil (kjkhil@itfact.co.kr)
 * @date		2016. 07. 12. 10:39:41
 * @return		Upon successful completion, a EXIT_SUCCESS is returned.\n
 				Otherwise,
 				a negative error code is returned indicating what went wrong.
 * @retval		Value	Value description
 */
int Waves::request(
	const std::string *id,		///< Call ID
	const char *upload_data,	///< 전송할 데이터
	size_t *upload_data_size,	///< 전송할 데이터 크기
	void **con_cls				///< 사용자 정의 데이터
) {
	logger->debug("[%s] in Servers", job_name);
	switch (method) {
	case HTTP_GET:
	case HTTP_POST:
	case HTTP_PUT:
	case HTTP_PATCH:
	case HTTP_DELETE:
	default:
		RestApi::sendUnsupportedMethod(connection, "허용되지 않은 메소드입니다.");
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
