#include <iostream>
#include "GB_Crypto.h"
#include "GB_Utf8String.h"
#include "GB_Timer.h"
#include "GB_DateTime.h"

using namespace std;
int main(int argc, char* argv[])
{
    GB_DateTime utcDateTime = GB_DateTime::GetUtcTimeFromNetwork();
    GB_DateTime localDateTime = utcDateTime + GB_TimeDuration::CreateFromHours(8);
    std::string localDate = localDateTime.ToIsoString();
	const GB_TimeDuration duration = utcDateTime - localDateTime;

    return 0;
}