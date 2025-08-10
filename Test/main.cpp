#include <iostream>
#include "GlobalBase.h"


using namespace std;
int main(int argc, char* argv[])
{
	string encodingString;
	GetConsoleEncodingString(encodingString);
	cout << "Console encoding: " << encodingString << endl << endl;

	string text1_Utf8 = GB_STR("Hello！世界！My World！こんにちは");
	string text2_Utf8 = GB_STR("Hello");
	string text1_Ansi = Utf8ToAnsi(text1_Utf8);
	string text2_Ansi = Utf8ToAnsi(text2_Utf8);
	cout << text1_Ansi << endl;	// Hello�����磡My World������ˤ���
	cout << text2_Ansi << endl; // Hello
	cout << text1_Utf8 << endl; // Hello！世界！My World！こんにちは
	cout << text2_Utf8 << endl; // Hello

	const bool success = SetConsoleEncodingToUtf8();
	cout << endl;
	cout << text1_Ansi << endl; // Hello�����磡My World������ˤ���
	cout << text2_Ansi << endl; // Hello
	cout << text1_Utf8 << endl; // Hello！世界！My World！こんにちは
	cout << text2_Utf8 << endl; // Hello

	cout << IsUtf8(text1_Utf8) << endl;	// 1
	cout << IsUtf8(text1_Ansi) << endl;	// 0

	return 0;
}



