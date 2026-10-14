// =====================================================================
// OLDS - Wi-Fi network list (used only to set the clock over NTP at boot)
//
// This file is DATA ONLY. The types (WifiNetwork, NET_PSK, NET_EAP) are
// defined in main.cpp. Do not share this file: it holds passwords.
//
// HOW TO ADD A NETWORK: add one line to the table below. Order = priority:
// the first line that is in range and connects is used.
//   { "SSID", NET_PSK, "password",        "",                  ""                 },
//   { "SSID", NET_EAP, "account password", "outer identity",   "inner username"   },
//
// A line whose text starts with "YOUR_" is skipped (placeholder not filled in).
// =====================================================================

const WifiNetwork WIFI_NETWORKS[] = {
    // ssid                 type     password                  identity                      username
    { "Xiaomi_Miwifi_5G",   NET_PSK, "DigiDagi666",            "",                           ""                          },
    { "Mob",                NET_PSK, "T1sv-UPKd-pnQP-AT6e",    "",                           ""                          },

    // eduroam (WPA2-Enterprise, PEAP/MSCHAPv2). Fill in the three YOUR_ fields:
    //   identity = outer (anonymous) identity, username = your institution login,
    //   password = your eduroam password. Your institution's eduroam page has the details.
    { "eduroam",            NET_EAP, "cojmuw-3gawvo-Jizxib",  "maczonandor@trefort-bp.edu.hu",    "maczonandor@trefort-bp.edu.hu" },
};