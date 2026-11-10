#pragma once

#if defined(_WIN32)
#include <windows.h>
#include <guiddef.h>

// CLSID_EyeCamFilter: {5C9D017A-C081-4FE7-99E4-500D06474F29}
// Унікальний GUID DirectShow Source фільтра віртуальної камери EyeCam
DEFINE_GUID(CLSID_EyeCamFilter, 
    0x5c9d017a, 0xc081, 0x4fe7, 0x99, 0xe4, 0x50, 0xd, 0x6, 0x47, 0x4f, 0x29);

// Текстове представлення CLSID для реєстрації в реєстрі Windows
constexpr const wchar_t* CLSID_EYECAM_STRING = L"{5C9D017A-C081-4FE7-99E4-500D06474F29}";

// Назва пристрою, яка відображатиметься в списках вибору веб-камер Zoom, Teams, Discord, OBS
constexpr const wchar_t* VIRTUAL_CAMERA_FRIENDLY_NAME = L"EyeCam Virtual Camera";

#endif
