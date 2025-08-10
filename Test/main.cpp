#include <iostream>
#include "GlobalBase.h"


using namespace std;
int main(int argc, char* argv[])
{
	const bool success = SetConsoleEncodingToUtf8();

	string text = GB_STR("Hello！世界！こんにちは");
	vector<string> parts = Utf8Split(text, GB_CHAR('！'));
	for (const string& part : parts)
	{
		cout << part << endl;
	}


	return 0;
}



