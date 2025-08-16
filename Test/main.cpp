#include <iostream>
#include "GlobalBase.h"
#include "GB_SysInfo.h"
#include "GB_Crypto.h"


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

	const CpuInfo info = GetCpuInfo();
	cout << endl << info.Serialize() << endl;
	cout << endl;

	const MotherboardInfo motherboardInfo = GetMotherboardInfo();
	cout << endl << motherboardInfo.Serialize() << endl;
	cout << endl;

	const OsInfo osInfo = GetOsInfo();
	cout << endl << osInfo.Serialize() << endl;
	cout << endl;

	const string hardwareId = GenerateHardwareId();
	cout << endl << hardwareId << endl;
	cout << endl;

	string rawInfo = GB_STR("Hello World! 你好，世界！我爱编程C++！！！！");
	string base64Str = GB_Base64Encode(rawInfo);
	string recInfo = GB_Base64Decode(base64Str);
	cout << "Raw Info: " << rawInfo << endl;
	cout << "Base64 Encoded: " << base64Str << endl; // SGVsbG8gV29ybGQhIOS9oOWlve+8jOS4lueVjO+8geaIkeeIsee8lueoi0MrK++8ge+8ge+8ge+8gQ==
	cout << "Base64 Decoded: " << recInfo << endl << endl;

	base64Str = GB_Base64Encode(rawInfo, true);
	recInfo = GB_Base64Decode(base64Str, true);
	cout << "Base64 Encoded: " << base64Str << endl; // SGVsbG8gV29ybGQhIOS9oOWlve-8jOS4lueVjO-8geaIkeeIsee8lueoi0MrK--8ge-8ge-8ge-8gQ==
	cout << "Base64 Decoded: " << recInfo << endl << endl;

	base64Str = GB_Base64Encode(rawInfo, true, true);
	recInfo = GB_Base64Decode(base64Str, true, true);
	cout << "Base64 Encoded: " << base64Str << endl; // SGVsbG8gV29ybGQhIOS9oOWlve-8jOS4lueVjO-8geaIkeeIsee8lueoi0MrK--8ge-8ge-8ge-8gQ
	cout << "Base64 Decoded: " << recInfo << endl << endl;

	base64Str = GB_Base64Encode(rawInfo, true, true); // SGVsbG8gV29ybGQhIOS9oOWlve-8jOS4lueVjO-8geaIkeeIsee8lueoi0MrK--8ge-8ge-8ge-8gQ
	recInfo = GB_Base64Decode(base64Str, true, true, true);
	cout << "Base64 Encoded: " << base64Str << endl;
	cout << "Base64 Decoded: " << recInfo << endl << endl;

	cout << "MD5 Hash: " << GB_GetMd5(rawInfo) << endl; // MD5 Hash: 04c68e5d27eb2b225698d90aeed9e7aa
	cout << "MD5 Hash: " << GB_GetMd5(GB_GetMd5(rawInfo)) << endl; // MD5 Hash: ba6f28ad0d84f4fdbae7c6e6a3b6893c

	cout << "SHA256 Hash: " << GB_GetSha256("") << endl; // SHA256 Hash: e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
	cout << "SHA256 Hash: " << GB_GetSha256("abc") << endl; // SHA256 Hash: ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
	cout << "SHA256 Hash: " << GB_GetSha256(rawInfo) << endl; // SHA256 Hash: 3a79267d8a07e9267749460442751729435129d08695187d9d47f780c190c4c9

	cout << "SHA512 Hash: " << GB_GetSha512("") << endl; // SHA512 Hash: cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e
	cout << "SHA512 Hash: " << GB_GetSha512("abc") << endl; // SHA512 Hash: ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f
	cout << "SHA512 Hash: " << GB_GetSha512(rawInfo) << endl; // SHA512 Hash: f607d355f04b2316b6adfdf19122242da587d583309271c73d95d829584581a207183d2e54cb3a95c4b99026bde19015ee8c217cc6bedf947464f31b502edf8d
	cout << endl;

	const string key = "我爱世界，世界爱我";
	const std::string b64 = GB_Aes256Encrypt(text1_Utf8, key);
	const std::string back = GB_Aes256Decrypt(b64, key);
	cout << "AES-256-CBC Encrypt: " << b64 << endl; // AES-256-CBC Encrypt: rrVFr02QJVaDulxo88y9RulkZEHWyHX4bosIj6i7pYswrlsEmpcD0nj9Z16bgDAFcXGc1+y6S22esrP1nR1G2g==
	cout << "AES-256-CBC Decrypt: " << back << endl;


	rawInfo = GB_STR("本功能为用户提供一个交互式工具，用于校准无地理参考或位置偏移的栅格影像。通过在影像和当前图形之间建立一系列匹配的控制点，系统将对影像进行非刚性变换，以修正其几何形变，使其与图形中的矢量数据精确对齐。此功能是整合扫描图纸、无坐标航拍像片等数据的关键工具，并支持对已有坐标的影像进行精度提升或纠正。");
	std::string publicKey, privateKey;
	GB_RsaGenerateKeyPair(publicKey, privateKey);
	const std::string encryptionText = GB_RsaEncrypt(rawInfo, publicKey);
	const std::string decryptedText = GB_RsaDecrypt(encryptionText, privateKey);
	cout << "encryptionText:" << encryptionText << endl;
	cout << "decryptedText:" << decryptedText << endl;


	return 0;
}



