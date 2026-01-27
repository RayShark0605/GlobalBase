#include <iostream>
#include "GB_Process.h"

using namespace std;
int main(int argc, char* argv[])
{
	GB_EnsureRunningAsAdmin();
	cout << "Is running as admin: " << (GB_IsRunningAsAdmin() ? "Yes" : "No") << std::endl;
	
	system("pause");
	return 0;
}
