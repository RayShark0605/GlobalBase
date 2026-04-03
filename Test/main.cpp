#include <iostream>
#include "GB_Crypto.h"
#include "GB_Utf8String.h"
#include "GB_Timer.h"
#include "GB_DateTime.h"
#include "GB_Network.h"
#include "GB_DelayLoadRuntime.h"

using namespace std;
int main(int argc, char* argv[])
{
    GB_InitializeRuntime();
    GB_DateTime utcDateTime = GB_DateTime::GetUtcTimeFromNetwork();
    GB_DateTime localDateTime = utcDateTime + GB_TimeDuration::CreateFromHours(8);
    std::string localDate = localDateTime.ToIsoString();
	const GB_TimeDuration duration = utcDateTime - localDateTime;

    const GB_NetworkResponse response = GB_RequestUrlData(GB_STR("http://localhost:8080/geoserver/gwc/service/wmts?service=WMTS&version=1.1.1&request=GetCapabilities"));

    return 0;
}