#pragma once

// Icons Kompoz's starter set doesn't have, made as it makes its own.
// Path data from Google's Material Design icons, Apache License 2.0.

#include <kompoz/kompoz.hpp>

namespace airlink::AppIcons {

using kompoz::ImageVector;
using kompoz::Icons::detail::materialIcon;

// clang-format off
inline const ImageVector Smartphone = materialIcon("Filled.Smartphone", "M17,1.01L7,1c-1.1,0 -2,0.9 -2,2v18c0,1.1 0.9,2 2,2h10c1.1,0 2,-0.9 2,-2V3c0,-1.1 -0.9,-1.99 -2,-1.99zM17,19H7V5h10v14z");
inline const ImageVector Computer = materialIcon("Filled.Computer", "M20,18c1.1,0 1.99,-0.9 1.99,-2L22,6c0,-1.1 -0.9,-2 -2,-2H4c-1.1,0 -2,0.9 -2,2v10c0,1.1 0.9,2 2,2H0v2h24v-2h-4zM4,6h16v10H4V6z");
inline const ImageVector Link = materialIcon("Filled.Link", "M3.9,12c0,-1.71 1.39,-3.1 3.1,-3.1h4V7H7c-2.76,0 -5,2.24 -5,5s2.24,5 5,5h4v-1.9H7c-1.71,0 -3.1,-1.39 -3.1,-3.1zM8,13h8v-2H8v2zm9,-6h-4v1.9h4c1.71,0 3.1,1.39 3.1,3.1s-1.39,3.1 -3.1,3.1h-4V17h4c2.76,0 5,-2.24 5,-5s-2.24,-5 -5,-5z");
inline const ImageVector UploadFile = materialIcon("Filled.UploadFile", "M14,2H6C4.9,2 4.01,2.9 4.01,4L4,20c0,1.1 0.89,2 1.99,2H18c1.1,0 2,-0.9 2,-2V8L14,2zM18,20H6V4h7v5h5V20zM8,15.01l1.41,1.41L11,14.84V19h2v-4.16l1.59,1.59L16,15.01L12.01,11L8,15.01z");
inline const ImageVector ContentPaste = materialIcon("Filled.ContentPaste", "M19,2h-4.18C14.4,0.84 13.3,0 12,0c-1.3,0 -2.4,0.84 -2.82,2H5c-1.1,0 -2,0.9 -2,2v16c0,1.1 0.9,2 2,2h14c1.1,0 2,-0.9 2,-2V4c0,-1.1 -0.9,-2 -2,-2zM12,2c0.55,0 1,0.45 1,1s-0.45,1 -1,1 -1,-0.45 -1,-1 0.45,-1 1,-1zM19,20H5V4h2v3h10V4h2v16z");
inline const ImageVector Download = materialIcon("Filled.FileDownload", "M5,20h14v-2H5V20zM19,9h-4V3H9v6H5l7,7L19,9z");
inline const ImageVector Upload = materialIcon("Filled.FileUpload", "M5,20h14v-2H5V20zM5,10h4v6h6v-6h4l-7,-7L5,10z");
inline const ImageVector Error = materialIcon("Filled.Error", "M12,2C6.48,2 2,6.48 2,12s4.48,10 10,10 10,-4.48 10,-10S17.52,2 12,2zm1,15h-2v-2h2v2zm0,-4h-2V7h2v6z");
inline const ImageVector Devices = materialIcon("Filled.Devices", "M4,6h18V4H4c-1.1,0 -2,0.9 -2,2v11H0v3h14v-3H4V6zm19,2h-6c-0.55,0 -1,0.45 -1,1v10c0,0.55 0.45,1 1,1h6c0.55,0 1,-0.45 1,-1V9c0,-0.55 -0.45,-1 -1,-1zm-1,9h-4v-7h4v7z");
inline const ImageVector Sms = materialIcon("Filled.Sms", "M20,2H4c-1.1,0 -1.99,0.9 -1.99,2L2,22l4,-4h14c1.1,0 2,-0.9 2,-2V4c0,-1.1 -0.9,-2 -2,-2zM9,11H7V9h2v2zm4,0h-2V9h2v2zm4,0h-2V9h2v2z");
inline const ImageVector Phone = materialIcon("Filled.Phone", "M6.62,10.79c1.44,2.83 3.76,5.14 6.59,6.59l2.2,-2.2c0.27,-0.27 0.67,-0.36 1.02,-0.24 1.12,0.37 2.33,0.57 3.57,0.57 0.55,0 1,0.45 1,1V20c0,0.55 -0.45,1 -1,1 -9.39,0 -17,-7.61 -17,-17 0,-0.55 0.45,-1 1,-1h3.5c0.55,0 1,0.45 1,1 0,1.25 0.2,2.45 0.57,3.57 0.11,0.35 0.03,0.74 -0.25,1.02l-2.2,2.2z");
inline const ImageVector Apps = materialIcon("Filled.Apps", "M4,8h4V4H4v4zm6,12h4v-4h-4v4zm-6,0h4v-4H4v4zm0,-6h4v-4H4v4zm6,0h4v-4h-4v4zm6,-10v4h4V4h-4zm-6,4h4V4h-4v4zm6,6h4v-4h-4v4zm0,6h4v-4h-4v4z");
inline const ImageVector VerifiedUser = materialIcon("Filled.VerifiedUser", "M12,1L3,5v6c0,5.55 3.84,10.74 9,12 5.16,-1.26 9,-6.45 9,-12V5l-9,-4zM10,17l-4,-4 1.41,-1.41L10,14.17l6.59,-6.59L18,9l-8,8z");
inline const ImageVector Shield = materialIcon("Outlined.Shield", "M12,2L4,5v6.09c0,5.05 3.41,9.76 8,10.91 4.59,-1.15 8,-5.86 8,-10.91V5l-8,-3zM18,11.09c0,4 -2.55,7.7 -6,8.83 -3.45,-1.13 -6,-4.82 -6,-8.83v-4.7l6,-2.25 6,2.25v4.7z");
// clang-format on

inline const ImageVector& forPlatform(const std::string& platform) {
    return platform == "android" ? Smartphone : Computer;
}

}  // namespace airlink::AppIcons
