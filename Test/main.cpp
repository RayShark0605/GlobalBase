#include <iostream>
#include "GB_RunTests.h"
#include "GB_Utf8String.h"
#include "Desktop/GB_Mouse.h"
#include "Desktop/GB_Screen.h"
#include "CV/GB_Image.h"
#include <thread>

int main(int argc, char* argv[])
{
    const GB_Image templateImage("C:/Users/localuser/Desktop/template.png");
    GB_Image mainImage;
    GB_Screen::CaptureVirtualScreen(mainImage);
	mainImage.SaveToFile("C:/Users/localuser/Desktop/screenshot.png");
    const GB_ImageTemplateFindResult result = mainImage.FindTemplate(templateImage);
    GB_Mouse::MoveTo(result.centerPoint, GB_MouseMoveCoordinateType::VirtualScreenPhysicalPixel);
	GB_Mouse::ClickLeftButton();

    return 0;
}