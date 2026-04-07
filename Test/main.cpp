#include <iostream>
#include "GB_Crypto.h"
#include "GB_Utf8String.h"
#include "GB_Timer.h"
#include "GB_DateTime.h"
#include "GB_Network.h"
#include "GB_IO.h"
#include "GB_DelayLoadRuntime.h"
#include "CV/GB_Image.h"

using namespace std;
int main(int argc, char* argv[])
{
    GB_InitializeRuntime();

    const std::string imageFilePath = GB_STR("p2.png");
    const GB_Image image(imageFilePath);
    const bool isEmpty = image.IsEmpty();
    const size_t width = image.GetWidth();
    const size_t height = image.GetHeight();
    const size_t rows = image.GetRows();
    const size_t cols = image.GetCols();
    const int channels = image.GetChannels();
    const GB_ImageDepth depth = image.GetDepth();

    const GB_Image rotatedImage = image.Rotate(45);
    rotatedImage.SaveToFile("test.png");
    return 0;
}