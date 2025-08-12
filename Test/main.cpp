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
	cout << endl;

	cout << Utf8StartsWith(GB_STR("世界！My World！こんにちは"), GB_STR("世界！")) << endl;	// 1
	cout << Utf8StartsWith(GB_STR("こんにちは"), GB_STR("こん")) << endl;	// 1
	cout << Utf8StartsWith(GB_STR("こんにちは"), GB_STR("こんにちは")) << endl;	// 1
	cout << Utf8StartsWith(GB_STR("Hello"), GB_STR("hello")) << endl;	// 0
	cout << Utf8StartsWith(GB_STR("Hello"), GB_STR("hello"), false) << endl;	// 1
	cout << endl;

	cout << Utf8Trim(GB_STR(" こんにちは\n")) << endl;
	cout << Utf8TrimLeft(GB_STR(" 世界！\n")) << endl;
	cout << endl;

	cout << Utf8EndsWith(GB_STR("世界！My World！こんにちは"), GB_STR("ちは")) << endl;	// 1
	cout << Utf8EndsWith(GB_STR("こんにちは"), GB_STR("は")) << endl;	// 1
	cout << Utf8EndsWith(GB_STR("こんにちは"), GB_STR("こん")) << endl;	// 0
	cout << Utf8EndsWith(GB_STR("Hello"), GB_STR("LO")) << endl;	// 0
	cout << Utf8EndsWith(GB_STR("Hello"), GB_STR("LO"), false) << endl;	// 1
	cout << endl;

	cout << Utf8Replace(GB_STR("Hello"), GB_STR("ll"), GB_STR("00")) << endl;	// He00o
	cout << Utf8Replace(GB_STR("Hello"), GB_STR("LL"), GB_STR("00")) << endl;	// Hello
	cout << Utf8Replace(GB_STR("Hello"), GB_STR("LL"), GB_STR("00"), false) << endl;	// He00o
	cout << Utf8Replace(GB_STR("世界！My World！こんにちは"), GB_STR("こんに"), GB_STR("你好")) << endl;	// 世界！My World！你好ちは
	cout << Utf8Replace(GB_STR("世界！My World！こんにちは"), GB_STR("世"), GB_STR("你好")) << endl;	// 你好界！My World！こんにちは
	cout << endl;


	return 0;
}



